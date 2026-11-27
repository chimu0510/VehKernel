#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Role-based host and companion environment installer and doctor (ADR-0028, spec 0017).

Pure Python 3.10+ standard library CLI that installs, previews (--dry-run), or
verifies (--check) host prerequisites for four composable Vehkernel machine roles:
  - dev:       Docker, pre-built devcontainer image (or local build), and ~/.local/bin/vk
  - flash:     openocd, stm32flash, pyserial, dialout/plugdev groups, udev rules, ModemManager/brltty
  - companion: ROS2 Jazzy, build-essential, colcon, rosdep, serial perms, optional Bridge build/service
  - desktop:   ROS2 Jazzy RViz2, IMU plugin, teleop keyboard, ROS_DOMAIN_ID and firewall checks
"""

from __future__ import annotations

import argparse
import dataclasses
import getpass
import grp
import os
import platform
import pwd
import shlex
import shutil
import stat
import subprocess
import sys
from pathlib import Path
from typing import TextIO

ALL_ROLES: tuple[str, ...] = ("dev", "flash", "companion", "desktop")
DEBIAN_BOOKWORM_ROLES: tuple[str, ...] = ("flash",)

DEFAULT_IMAGE = "ghcr.io/chimu0510/vehkernel-dev:latest"
LOCAL_IMAGE = "vehkernel-dev:latest"

ROS_DISTRO = "jazzy"
ROS_SETUP_BASH = Path(f"/opt/ros/{ROS_DISTRO}/setup.bash")
ROS_KEYRING_PATH = Path("/usr/share/keyrings/ros-archive-keyring.gpg")
ROS_KEY_URL = "https://raw.githubusercontent.com/ros/rosdistro/master/ros.key"
ROS_LIST_PATH = Path("/etc/apt/sources.list.d/ros2.list")
ROS_SOURCES_PATH = Path("/etc/apt/sources.list.d/ros2.sources")
ROSDEP_DEFAULT_LIST = Path("/etc/ros/rosdep/sources.list.d/20-default.list")

UDEV_DEST = Path("/etc/udev/rules.d/99-vehkernel.rules")
RUN_UDEV_DIR = Path("/run/udev")
RUN_SYSTEMD_DIR = Path("/run/systemd/system")
LINGER_DIR = Path("/var/lib/systemd/linger")

DOCKER_FALLBACK_PACKAGES: tuple[str, ...] = ("docker.io", "docker-buildx")
SERIAL_BASE_PACKAGES: tuple[str, ...] = ("python3-serial", "stm32flash")
FLASH_PACKAGES: tuple[str, ...] = ("python3-serial", "stm32flash", "openocd")
ROS_REPO_PREREQ_PACKAGES: tuple[str, ...] = (
    "locales",
    "software-properties-common",
    "curl",
    "ca-certificates",
)
COMPANION_ROS_PACKAGES: tuple[str, ...] = (
    "build-essential",
    "ros-jazzy-ros-base",
    "python3-colcon-common-extensions",
    "python3-rosdep",
    "git",
)
DESKTOP_ROS_PACKAGES: tuple[str, ...] = (
    "ros-jazzy-rviz2",
    "ros-jazzy-rviz-imu-plugin",
    "ros-jazzy-teleop-twist-keyboard",
)

SUPPORTED_ARCH_MAP: dict[str, str] = {
    "x86_64": "amd64",
    "aarch64": "arm64",
    "arm64": "arm64",
}


class UsageError(Exception):
    """Raised on unsupported OS/arch/role or invalid CLI options (exit code 2)."""


class SetupError(Exception):
    """Raised when an apply step fails at runtime (exit code 1)."""


@dataclasses.dataclass(frozen=True)
class OsInfo:
    os_id: str
    version_id: str
    codename: str
    pretty_name: str
    arch: str
    dpkg_arch: str

    @property
    def is_ubuntu_noble(self) -> bool:
        return self.os_id == "ubuntu" and (self.codename == "noble" or self.version_id == "24.04")

    @property
    def is_debian_bookworm(self) -> bool:
        return self.os_id in ("debian", "raspbian") and (self.codename == "bookworm" or self.version_id == "12")


@dataclasses.dataclass(frozen=True)
class CmdResult:
    returncode: int
    stdout: str = ""
    stderr: str = ""


@dataclasses.dataclass(frozen=True)
class CheckResult:
    name: str
    status: str  # "PASS", "FAIL", "WARN"
    detail: str
    remediation: str = ""


class SystemEnv:
    """Host environment interface for production and test injection."""

    def __init__(
        self,
        *,
        script_dir: Path | None = None,
        stdout: TextIO | None = None,
        stderr: TextIO | None = None,
    ) -> None:
        self.script_dir = script_dir if script_dir is not None else Path(__file__).resolve().parent
        self.stdout = stdout if stdout is not None else sys.stdout
        self.stderr = stderr if stderr is not None else sys.stderr

    @property
    def euid(self) -> int:
        return os.geteuid()

    @property
    def user(self) -> str:
        return getpass.getuser()

    @property
    def home(self) -> Path:
        return Path.home()

    @property
    def cwd(self) -> Path:
        return Path.cwd()

    @property
    def environ(self) -> dict[str, str]:
        return dict(os.environ)

    def read_os_release(self) -> str | None:
        return self.read_text(Path("/etc/os-release"))

    def machine(self) -> str:
        return platform.machine()

    def which(self, cmd: str) -> str | None:
        return shutil.which(cmd, path=self.environ.get("PATH"))

    def path_exists(self, path: Path) -> bool:
        return path.exists()

    def is_dir(self, path: Path) -> bool:
        return path.is_dir()

    def is_executable(self, path: Path) -> bool:
        return path.is_file() and os.access(path, os.X_OK)

    def read_text(self, path: Path) -> str | None:
        try:
            return path.read_text(encoding="utf-8")
        except OSError:
            return None

    def read_bytes(self, path: Path) -> bytes | None:
        try:
            return path.read_bytes()
        except OSError:
            return None

    def mkdir(self, path: Path) -> None:
        path.mkdir(parents=True, exist_ok=True)

    def write_bytes(self, path: Path, content: bytes, mode: int | None = None) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        if mode is not None:
            path.chmod(mode)

    def write_text(self, path: Path, content: str, mode: int | None = None) -> None:
        self.write_bytes(path, content.encode("utf-8"), mode=mode)

    def group_exists(self, group: str) -> bool:
        try:
            grp.getgrnam(group)
            return True
        except KeyError:
            return False

    def configured_groups(self, user: str) -> set[str]:
        groups: set[str] = set()
        try:
            pw = pwd.getpwnam(user)
            try:
                groups.add(grp.getgrgid(pw.pw_gid).gr_name)
            except KeyError:
                pass
        except KeyError:
            pass
        for entry in grp.getgrall():
            if user in entry.gr_mem:
                groups.add(entry.gr_name)
        return groups

    def active_groups(self) -> set[str]:
        groups: set[str] = set()
        for gid in os.getgroups():
            try:
                groups.add(grp.getgrgid(gid).gr_name)
            except KeyError:
                pass
        return groups

    def systemd_running(self) -> bool:
        return self.which("systemctl") is not None and self.is_dir(RUN_SYSTEMD_DIR)

    def service_is_active(self, unit: str, *, user: bool = False) -> bool:
        if not self.systemd_running():
            return False
        cmd = ["systemctl"]
        if user:
            cmd.append("--user")
        cmd.extend(["is-active", "--quiet", unit])
        return self.run_cmd(cmd, capture=True).returncode == 0

    def service_is_enabled(self, unit: str, *, user: bool = False) -> bool:
        if not self.systemd_running():
            return False
        cmd = ["systemctl"]
        if user:
            cmd.append("--user")
        cmd.extend(["is-enabled", "--quiet", unit])
        return self.run_cmd(cmd, capture=True).returncode == 0

    def dpkg_installed(self, package: str) -> bool:
        if self.which("dpkg-query") is None:
            return False
        res = self.run_cmd(
            ["dpkg-query", "-W", "-f=${Status}", package],
            capture=True,
        )
        return res.returncode == 0 and "install ok installed" in res.stdout

    def run_cmd(
        self,
        argv: list[str],
        *,
        cwd: Path | None = None,
        env_overrides: dict[str, str] | None = None,
        input_data: bytes | None = None,
        capture: bool = False,
    ) -> CmdResult:
        run_env = self.environ
        if env_overrides:
            run_env = {**run_env, **env_overrides}
        try:
            proc = subprocess.run(
                argv,
                cwd=str(cwd) if cwd is not None else None,
                env=run_env,
                input=input_data,
                capture_output=capture,
                check=False,
            )
        except OSError as exc:
            return CmdResult(returncode=127, stdout="", stderr=str(exc))
        stdout = proc.stdout.decode("utf-8", errors="replace") if capture and proc.stdout else ""
        stderr = proc.stderr.decode("utf-8", errors="replace") if capture and proc.stderr else ""
        return CmdResult(returncode=proc.returncode, stdout=stdout, stderr=stderr)


def parse_os_release(text: str) -> dict[str, str]:
    """Parse /etc/os-release key=value pairs."""
    data: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        val = val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
            val = val[1:-1]
        data[key.strip()] = val
    return data


def detect_os(env: SystemEnv) -> OsInfo:
    """Detect OS and architecture from /etc/os-release and platform.machine()."""
    arch = env.machine()
    dpkg_arch = SUPPORTED_ARCH_MAP.get(arch)
    if dpkg_arch is None:
        raise UsageError(
            f"Unsupported architecture '{arch}'. "
            "Vehkernel supports x86_64 (amd64) and aarch64 (arm64)."
        )

    os_release_text = env.read_os_release()
    if os_release_text is None:
        raise UsageError(
            "Cannot read /etc/os-release. Supported hosts: Ubuntu 24.04 (noble) for all roles "
            "(dev, flash, companion, desktop), and Debian 12 / Raspberry Pi OS (bookworm) for --role flash. "
            "Non-Debian Linux, macOS, and native Windows are out of scope (use WSL2 Ubuntu 24.04 on Windows)."
        )

    parsed = parse_os_release(os_release_text)
    os_id = parsed.get("ID", "").lower()
    version_id = parsed.get("VERSION_ID", "")
    codename = parsed.get("VERSION_CODENAME", "").lower()
    pretty_name = parsed.get("PRETTY_NAME") or f"{os_id} {version_id}".strip() or "unknown"

    if os_id == "ubuntu" and not codename and version_id == "24.04":
        codename = "noble"
    elif os_id in ("debian", "raspbian") and not codename and version_id == "12":
        codename = "bookworm"

    return OsInfo(
        os_id=os_id,
        version_id=version_id,
        codename=codename,
        pretty_name=pretty_name,
        arch=arch,
        dpkg_arch=dpkg_arch,
    )


def validate_os_and_roles(os_info: OsInfo, roles: list[str]) -> None:
    """Validate that the detected OS supports every requested role."""
    if os_info.is_ubuntu_noble:
        return

    if os_info.is_debian_bookworm:
        unsupported = [role for role in roles if role not in DEBIAN_BOOKWORM_ROLES]
        if unsupported:
            roles_str = ", ".join(unsupported)
            raise UsageError(
                f"Role(s) not supported on {os_info.pretty_name} (Debian 12 / Raspberry Pi OS bookworm): "
                f"{roles_str}. ROS2 Jazzy and full roles (dev, companion, desktop) require Ubuntu 24.04 (noble) "
                "(ADR-0027, ADR-0028). On Debian 12 / Raspberry Pi OS, only --role flash is supported."
            )
        return

    raise UsageError(
        f"Unsupported OS '{os_info.pretty_name}' (ID={os_info.os_id or 'unknown'}, "
        f"VERSION_ID={os_info.version_id or 'unknown'}, "
        f"VERSION_CODENAME={os_info.codename or 'unknown'}). "
        "Supported hosts: Ubuntu 24.04 (noble) for all roles (dev, flash, companion, desktop), "
        "and Debian 12 / Raspberry Pi OS (bookworm) for --role flash. "
        "Non-Debian Linux, macOS, and native Windows are out of scope (use WSL2 Ubuntu 24.04 on Windows)."
    )


def deduplicate_roles(roles: list[str]) -> list[str]:
    """Deduplicate role names while preserving first-seen order."""
    seen: set[str] = set()
    ordered: list[str] = []
    for role in roles:
        if role not in seen:
            seen.add(role)
            ordered.append(role)
    return ordered


def expand_path(raw: str, home: Path) -> Path:
    """Expand a leading '~' against the injected home directory."""
    if raw == "~":
        return home
    if raw.startswith("~/"):
        return home / raw[2:]
    return Path(raw)


def find_repo_dir(env: SystemEnv, explicit: str | None, marker: Path) -> Path | None:
    """Locate a Vehkernel repository checkout containing `marker`."""
    if explicit is not None:
        candidate = expand_path(explicit, env.home)
        if not candidate.is_absolute():
            candidate = env.cwd / candidate
        return candidate if env.path_exists(candidate / marker) else None

    candidates: list[Path] = [env.script_dir.parent.parent, env.cwd, *env.cwd.parents]
    seen: set[Path] = set()
    for candidate in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)
        if env.path_exists(candidate / marker):
            return candidate
    return None


def format_cmd(argv: list[str]) -> str:
    return " ".join(shlex.quote(arg) for arg in argv)


def render_bridge_env(workspace: Path, device: str, baud: int, ros_domain_id: int) -> str:
    spdx = "SPDX-License-Identifier"
    return (
        f"# {spdx}: GPL-3.0-or-later\n"
        "# Environment configuration for vehkernel-bridge.service.\n"
        "# Generated by tools/setup/vk_setup.py; existing files are preserved on re-run.\n"
        f"VK_BRIDGE_WORKSPACE={workspace}\n"
        f"VK_BRIDGE_DEVICE={device}\n"
        f"VK_BRIDGE_BAUD={baud}\n"
        f"ROS_DOMAIN_ID={ros_domain_id}\n"
    )


def format_ros2_list_line(os_info: OsInfo) -> str:
    return (
        f"deb [arch={os_info.dpkg_arch} signed-by={ROS_KEYRING_PATH}] "
        f"http://packages.ros.org/ros2/ubuntu {os_info.codename} main\n"
    )


def is_ros2_repo_configured(env: SystemEnv) -> bool:
    return (
        env.path_exists(ROS_SOURCES_PATH)
        or env.path_exists(ROS_LIST_PATH)
        or env.dpkg_installed("ros2-apt-source")
        or env.dpkg_installed("ros-apt-source")
    )


class SetupRunner:
    """Executes apply, --dry-run, or --check across selected roles."""

    def __init__(self, env: SystemEnv, args: argparse.Namespace, os_info: OsInfo, roles: list[str]) -> None:
        self.env = env
        self.args = args
        self.os_info = os_info
        self.roles = roles
        self.dry_run: bool = bool(args.dry_run)
        self.yes: bool = bool(args.yes)
        self.use_sudo: bool = (not bool(args.no_sudo)) and (env.euid != 0)
        self.apt_updated: bool = False
        self.ros2_repo_done: bool = False
        self.serial_prereqs_done: bool = False
        self.installed_packages: set[str] = set()
        self.added_groups: list[str] = []
        self.checks: list[CheckResult] = []
        self._recorded_check_names: set[str] = set()

    def out(self, line: str) -> None:
        self.env.stdout.write(line + "\n")

    def err(self, line: str) -> None:
        self.env.stderr.write(line + "\n")

    def sudo_prefix(self) -> list[str]:
        return ["sudo"] if self.use_sudo else []

    def _sudo_cmd_hint(self, cmd: str) -> str:
        return f"sudo {cmd}" if self.use_sudo else cmd

    def _ensure_sudo_available(self) -> None:
        if self.use_sudo and not self.dry_run and self.env.which("sudo") is None:
            raise SetupError(
                "sudo is required for privileged operations but was not found in PATH. "
                "Install sudo, run as root, or pass --no-sudo."
            )

    def run_action(
        self,
        argv: list[str],
        *,
        privileged: bool = False,
        cwd: Path | None = None,
        env_overrides: dict[str, str] | None = None,
        input_data: bytes | None = None,
        capture: bool = False,
        failure_hint: str = "",
    ) -> CmdResult:
        if privileged and self.use_sudo:
            if env_overrides:
                env_args = [f"{k}={v}" for k, v in sorted(env_overrides.items())]
                full_argv = ["sudo", "env", *env_args, *argv]
            else:
                full_argv = ["sudo", *argv]
            cmd_env: dict[str, str] | None = None
        else:
            full_argv = list(argv)
            cmd_env = env_overrides
        if self.dry_run:
            prefix = f"[dry-run] (cwd={cwd}) run:" if cwd is not None else "[dry-run] run:"
            self.out(f"{prefix} {format_cmd(full_argv)}")
            return CmdResult(returncode=0)

        if privileged:
            self._ensure_sudo_available()

        self.out(f"[run] {format_cmd(full_argv)}")
        res = self.env.run_cmd(
            full_argv,
            cwd=cwd,
            env_overrides=cmd_env,
            input_data=input_data,
            capture=capture,
        )
        if res.returncode != 0:
            msg = f"Command failed (exit {res.returncode}): {format_cmd(full_argv)}"
            if failure_hint:
                msg = f"{msg}\nHint: {failure_hint}"
            raise SetupError(msg)
        return res

    def _apt_env(self) -> dict[str, str] | None:
        if self.yes:
            return {"DEBIAN_FRONTEND": "noninteractive"}
        return None

    def apt_update(self, *, force: bool = False) -> None:
        if self.apt_updated and not force:
            return
        self.run_action(["apt-get", "update"], privileged=True, env_overrides=self._apt_env())
        self.apt_updated = True

    def ensure_apt_packages(self, packages: tuple[str, ...] | list[str]) -> None:
        missing = [
            pkg
            for pkg in packages
            if pkg not in self.installed_packages and not self.env.dpkg_installed(pkg)
        ]
        if not missing:
            self.out(f"[ok] apt packages already installed: {', '.join(packages)}")
            return
        self.apt_update()
        cmd = ["apt-get", "install"]
        if self.yes:
            cmd.append("-y")
        cmd.extend(missing)
        self.run_action(cmd, privileged=True, env_overrides=self._apt_env())
        self.installed_packages.update(missing)

    def ensure_group(self, group: str, *, require_exists: bool = False) -> None:
        if self.env.euid == 0:
            self.out(f"[ok] running as root; skipping '{group}' group membership")
            return
        if group in self.added_groups:
            return
        exists = self.env.group_exists(group)
        if require_exists and not exists:
            self.out(f"[skip] group '{group}' does not exist on this system")
            return
        if group in self.env.configured_groups(self.env.user):
            self.out(f"[ok] user '{self.env.user}' is already in group '{group}'")
            return
        if not exists and not self.dry_run:
            self.out(f"[skip] group '{group}' does not exist on this system")
            return
        self.run_action(["usermod", "-aG", group, self.env.user], privileged=True)
        self.added_groups.append(group)

    def write_user_file(self, dest: Path, content: bytes, *, mode: int = 0o644) -> bool:
        existing = self.env.read_bytes(dest)
        needs_exec = bool(mode & stat.S_IXUSR)
        if existing == content and (not needs_exec or self.env.is_executable(dest)):
            self.out(f"[ok] {dest} is already up to date")
            return False
        if self.dry_run:
            self.out(f"[dry-run] write {dest} (mode {mode:04o})")
            return True
        self.env.write_bytes(dest, content, mode=mode)
        self.out(f"[write] {dest} (mode {mode:04o})")
        return True

    def write_privileged_file(self, dest: Path, content: bytes, *, mode: int = 0o644) -> bool:
        existing = self.env.read_bytes(dest)
        if existing == content:
            self.out(f"[ok] {dest} is already up to date")
            return False
        if self.dry_run:
            action = "sudo write" if self.use_sudo else "write"
            self.out(f"[dry-run] {action} {dest} (mode {mode:04o})")
            return True
        if not self.use_sudo:
            self.env.write_bytes(dest, content, mode=mode)
            self.out(f"[write] {dest} (mode {mode:04o})")
            return True
        if not self.env.is_dir(dest.parent):
            self.run_action(["mkdir", "-p", str(dest.parent)], privileged=True)
        self.run_action(["tee", str(dest)], privileged=True, input_data=content, capture=True)
        self.run_action(["chmod", f"{mode:04o}", str(dest)], privileged=True)
        return True

    def add_check(self, name: str, status: str, detail: str, remediation: str = "") -> None:
        if name in self._recorded_check_names:
            return
        self._recorded_check_names.add(name)
        self.checks.append(CheckResult(name=name, status=status, detail=detail, remediation=remediation))

    def _check_apt_package(self, package: str, role_hint: str) -> None:
        if self.env.dpkg_installed(package):
            self.add_check(f"pkg:{package}", "PASS", f"package '{package}' is installed")
        else:
            install_hint = self._sudo_cmd_hint(f"apt-get install -y {package}")
            self.add_check(
                f"pkg:{package}",
                "FAIL",
                f"package '{package}' is not installed",
                remediation=f"{install_hint} (or vk_setup.py --role {role_hint})",
            )

    def _check_group(self, group: str, *, optional_if_missing: bool = False) -> None:
        if self.env.euid == 0:
            self.add_check(f"group:{group}", "PASS", f"running as root; '{group}' group not required")
            return
        if optional_if_missing and not self.env.group_exists(group):
            self.add_check(f"group:{group}", "PASS", f"group '{group}' is not present on this system")
            return
        configured = self.env.configured_groups(self.env.user)
        if group not in configured:
            self.add_check(
                f"group:{group}",
                "FAIL",
                f"user '{self.env.user}' is not in group '{group}'",
                remediation=self._sudo_cmd_hint(f"usermod -aG {group} {self.env.user}"),
            )
            return
        active = self.env.active_groups()
        if group not in active:
            self.add_check(
                f"group:{group}",
                "WARN",
                f"user '{self.env.user}' is in group '{group}', but this login session has not refreshed it",
                remediation=f"Log out and log back in (or run 'newgrp {group}')",
            )
            return
        self.add_check(f"group:{group}", "PASS", f"user '{self.env.user}' is in group '{group}'")

    def _template_bytes(self, relpath: Path) -> bytes:
        src = self.env.script_dir / relpath
        data = self.env.read_bytes(src)
        if data is None:
            raise SetupError(f"Bundled template not found: {src}")
        return data

    # ------------------------------------------------------------------
    # Shared helpers: serial/udev/ModemManager/brltty and ROS2 Jazzy repo
    # ------------------------------------------------------------------

    def apply_serial_prereqs(self, *, include_openocd: bool, include_plugdev: bool) -> None:
        pkgs = FLASH_PACKAGES if include_openocd else SERIAL_BASE_PACKAGES
        self.ensure_apt_packages(pkgs)
        self.ensure_group("dialout")
        if include_plugdev:
            self.ensure_group("plugdev", require_exists=True)

        if self.serial_prereqs_done:
            return
        self.serial_prereqs_done = True

        udev_bytes = self._template_bytes(Path("udev/99-vehkernel.rules"))
        changed = self.write_privileged_file(UDEV_DEST, udev_bytes, mode=0o644)
        if changed:
            if self.env.which("udevadm") is not None and self.env.is_dir(RUN_UDEV_DIR):
                self.run_action(["udevadm", "control", "--reload-rules"], privileged=True)
                self.run_action(["udevadm", "trigger"], privileged=True)
            else:
                self.out("[skip] udevadm or /run/udev unavailable; skipping udev reload")

        if self.env.systemd_running():
            if self.env.service_is_active("ModemManager.service"):
                self.run_action(["systemctl", "disable", "--now", "ModemManager.service"], privileged=True)
            else:
                self.out("[ok] ModemManager.service is not active")
        else:
            self.out("[skip] systemd is not running; skipping ModemManager check")

        if self.env.dpkg_installed("brltty"):
            cmd = ["apt-get", "purge"]
            if self.yes:
                cmd.append("-y")
            cmd.append("brltty")
            self.run_action(cmd, privileged=True, env_overrides=self._apt_env())
        else:
            self.out("[ok] brltty is not installed")

    def check_serial_prereqs(self, *, include_openocd: bool, include_plugdev: bool, role_hint: str) -> None:
        pkgs = FLASH_PACKAGES if include_openocd else SERIAL_BASE_PACKAGES
        for pkg in pkgs:
            self._check_apt_package(pkg, role_hint)

        self._check_group("dialout")
        if include_plugdev:
            self._check_group("plugdev", optional_if_missing=True)

        udev_src = self.env.script_dir / "udev" / "99-vehkernel.rules"
        expected_udev = self.env.read_bytes(udev_src)
        installed_udev = self.env.read_bytes(UDEV_DEST)
        if installed_udev is None:
            self.add_check(
                "udev:99-vehkernel.rules",
                "FAIL",
                f"{UDEV_DEST} is not installed",
                remediation=f"tools/setup/vk_setup.py --role {role_hint}",
            )
        elif expected_udev is not None and installed_udev != expected_udev:
            self.add_check(
                "udev:99-vehkernel.rules",
                "FAIL",
                f"{UDEV_DEST} differs from bundled {udev_src}",
                remediation=f"tools/setup/vk_setup.py --role {role_hint}",
            )
        else:
            self.add_check("udev:99-vehkernel.rules", "PASS", f"{UDEV_DEST} is installed and up to date")

        if self.env.systemd_running():
            if self.env.service_is_active("ModemManager.service"):
                self.add_check(
                    "service:ModemManager",
                    "FAIL",
                    "ModemManager.service is active and can probe C50C V1.1 SERIAL2 (toggling DTR/RTS)",
                    remediation=self._sudo_cmd_hint("systemctl disable --now ModemManager.service"),
                )
            else:
                self.add_check("service:ModemManager", "PASS", "ModemManager.service is not active")
        else:
            self.add_check("service:ModemManager", "PASS", "systemd is not running; ModemManager not active")

        if self.env.dpkg_installed("brltty"):
            self.add_check(
                "pkg:brltty-absent",
                "FAIL",
                "brltty is installed and can claim CH9102F USB serial adapters",
                remediation=self._sudo_cmd_hint("apt-get purge -y brltty"),
            )
        else:
            self.add_check("pkg:brltty-absent", "PASS", "brltty is not installed")

    def apply_ros2_repo(self) -> None:
        if self.ros2_repo_done:
            return
        self.ros2_repo_done = True

        if is_ros2_repo_configured(self.env):
            self.out("[ok] ROS2 Jazzy apt repository is already configured")
            return

        self.ensure_apt_packages(ROS_REPO_PREREQ_PACKAGES)
        self.run_action(["add-apt-repository", "-y", "universe"], privileged=True, env_overrides=self._apt_env())
        if not self.env.is_dir(ROS_KEYRING_PATH.parent):
            self.run_action(["mkdir", "-p", str(ROS_KEYRING_PATH.parent)], privileged=True)
        self.run_action(
            ["curl", "-sSL", ROS_KEY_URL, "-o", str(ROS_KEYRING_PATH)],
            privileged=True,
        )
        list_content = format_ros2_list_line(self.os_info).encode("utf-8")
        self.write_privileged_file(ROS_LIST_PATH, list_content, mode=0o644)
        self.apt_update(force=True)

    def check_ros2_repo(self, role_hint: str) -> None:
        if is_ros2_repo_configured(self.env):
            self.add_check("ros2:apt-repo", "PASS", "ROS2 Jazzy apt repository is configured")
        else:
            self.add_check(
                "ros2:apt-repo",
                "FAIL",
                "ROS2 Jazzy apt repository is not configured",
                remediation=f"tools/setup/vk_setup.py --role {role_hint}",
            )

    def check_ros_setup_bash(self, role_hint: str) -> None:
        if self.env.path_exists(ROS_SETUP_BASH):
            self.add_check("ros2:setup.bash", "PASS", f"{ROS_SETUP_BASH} exists")
        else:
            self.add_check(
                "ros2:setup.bash",
                "FAIL",
                f"{ROS_SETUP_BASH} is missing",
                remediation=f"tools/setup/vk_setup.py --role {role_hint}",
            )

    # ------------------------------------------------------------------
    # Role: dev
    # ------------------------------------------------------------------

    def apply_dev(self) -> None:
        if self.env.which("docker") is None:
            self.ensure_apt_packages(DOCKER_FALLBACK_PACKAGES)
        else:
            self.out("[ok] docker is already installed (preserving existing installation)")

        self.ensure_group("docker")

        docker_needs_sudo = self.env.euid != 0 and "docker" not in self.env.active_groups()

        if self.args.build_local:
            repo_dir = find_repo_dir(self.env, self.args.repo_dir, Path(".devcontainer/Dockerfile"))
            if repo_dir is None:
                raise SetupError(
                    "Cannot find .devcontainer/Dockerfile for --build-local. "
                    "Run from a Vehkernel checkout or pass --repo-dir <path>."
                )
            dockerfile = repo_dir / ".devcontainer" / "Dockerfile"
            self.run_action(
                ["docker", "build", "-t", LOCAL_IMAGE, "-f", str(dockerfile), str(repo_dir)],
                privileged=docker_needs_sudo,
            )
        else:
            image = self.args.image
            self.run_action(
                ["docker", "pull", image],
                privileged=docker_needs_sudo,
                failure_hint=(
                    f"If '{image}' cannot be pulled (offline or registry unreachable), "
                    "build locally with: tools/setup/vk_setup.py --role dev --build-local"
                ),
            )

        vk_bytes = self._template_bytes(Path("vk"))
        vk_dest = self.env.home / ".local" / "bin" / "vk"
        self.write_user_file(vk_dest, vk_bytes, mode=0o755)

        if not self._is_local_bin_on_path():
            self.out(
                f"[note] {vk_dest.parent} is not on PATH; add "
                '\'export PATH="$HOME/.local/bin:$PATH"\' to your shell profile or log out and back in.'
            )

        repo_dir = find_repo_dir(self.env, self.args.repo_dir, Path(".gitmodules"))
        if repo_dir is not None and self.env.which("git") is not None:
            uninit = self._uninitialized_submodules(repo_dir)
            if uninit:
                self.out(
                    f"[note] uninitialized git submodule(s) in {repo_dir}: {', '.join(uninit)}; "
                    "run: git submodule update --init --recursive"
                )

    def _is_local_bin_on_path(self) -> bool:
        local_bin = self.env.home / ".local" / "bin"
        raw_path = self.env.environ.get("PATH", "")
        for entry in raw_path.split(os.pathsep):
            if not entry:
                continue
            if expand_path(entry, self.env.home) == local_bin:
                return True
        return False

    def _uninitialized_submodules(self, repo_dir: Path) -> list[str]:
        res = self.env.run_cmd(["git", "-C", str(repo_dir), "submodule", "status"], capture=True)
        if res.returncode != 0:
            return []
        missing: list[str] = []
        for line in res.stdout.splitlines():
            if line.startswith("-"):
                parts = line[1:].strip().split()
                if len(parts) >= 2:
                    missing.append(parts[1])
        return missing

    def check_dev(self) -> None:
        docker_path = self.env.which("docker")
        if docker_path is not None:
            self.add_check("dev:docker", "PASS", f"docker found at {docker_path}")
        else:
            self.add_check(
                "dev:docker",
                "FAIL",
                "docker is not installed or not in PATH",
                remediation="tools/setup/vk_setup.py --role dev",
            )

        self._check_group("docker")

        target_image = LOCAL_IMAGE if self.args.build_local else self.args.image
        if docker_path is None:
            self.add_check(
                "dev:image",
                "FAIL",
                f"cannot inspect image '{target_image}' because docker is not installed",
                remediation="tools/setup/vk_setup.py --role dev",
            )
        else:
            inspect_res = self.env.run_cmd(["docker", "image", "inspect", target_image], capture=True)
            if inspect_res.returncode == 0:
                self.add_check("dev:image", "PASS", f"container image '{target_image}' is available locally")
            elif (
                not self.args.build_local
                and target_image == DEFAULT_IMAGE
                and self.env.run_cmd(["docker", "image", "inspect", LOCAL_IMAGE], capture=True).returncode == 0
            ):
                self.add_check(
                    "dev:image",
                    "PASS",
                    f"local fallback container image '{LOCAL_IMAGE}' is available",
                )
            elif "permission denied" in inspect_res.stderr.lower() or (
                self.env.euid != 0 and "docker" not in self.env.active_groups()
            ):
                if "docker" in self.env.configured_groups(self.env.user):
                    detail = (
                        f"cannot inspect image '{target_image}' "
                        "(Docker socket permission denied; current session lacks 'docker' group)"
                    )
                    remediation = "Log out and log back in (or run 'newgrp docker'), then re-run --check"
                else:
                    detail = (
                        f"cannot inspect image '{target_image}' "
                        f"(Docker socket permission denied; user '{self.env.user}' is not in 'docker' group)"
                    )
                    remediation = (
                        f"{self._sudo_cmd_hint(f'usermod -aG docker {self.env.user}')} "
                        "&& newgrp docker (or log out and back in)"
                    )
                self.add_check("dev:image", "FAIL", detail, remediation=remediation)
            else:
                remediation = (
                    "tools/setup/vk_setup.py --role dev --build-local"
                    if self.args.build_local
                    else f"docker pull {target_image} (or tools/setup/vk_setup.py --role dev --build-local)"
                )
                self.add_check(
                    "dev:image",
                    "FAIL",
                    f"container image '{target_image}' is not available locally",
                    remediation=remediation,
                )

        vk_dest = self.env.home / ".local" / "bin" / "vk"
        vk_src = self.env.script_dir / "vk"
        expected_vk = self.env.read_bytes(vk_src)
        installed_vk = self.env.read_bytes(vk_dest)
        if installed_vk is None:
            self.add_check(
                "dev:vk-wrapper",
                "FAIL",
                f"{vk_dest} is not installed",
                remediation="tools/setup/vk_setup.py --role dev",
            )
        elif not self.env.is_executable(vk_dest):
            self.add_check(
                "dev:vk-wrapper",
                "FAIL",
                f"{vk_dest} is not executable",
                remediation=f"chmod 0755 {vk_dest}",
            )
        elif expected_vk is not None and installed_vk != expected_vk:
            self.add_check(
                "dev:vk-wrapper",
                "FAIL",
                f"{vk_dest} differs from bundled {vk_src}",
                remediation="tools/setup/vk_setup.py --role dev",
            )
        else:
            self.add_check("dev:vk-wrapper", "PASS", f"{vk_dest} is installed and executable")

        if self._is_local_bin_on_path():
            self.add_check("dev:local-bin-path", "PASS", f"{vk_dest.parent} is on PATH")
        else:
            self.add_check(
                "dev:local-bin-path",
                "WARN",
                f"{vk_dest.parent} is not on PATH",
                remediation='Add \'export PATH="$HOME/.local/bin:$PATH"\' to ~/.bashrc or log out and back in',
            )

        repo_dir = find_repo_dir(self.env, self.args.repo_dir, Path(".gitmodules"))
        if repo_dir is not None and self.env.which("git") is not None:
            uninit = self._uninitialized_submodules(repo_dir)
            if uninit:
                self.add_check(
                    "dev:git-submodules",
                    "FAIL",
                    f"uninitialized git submodule(s): {', '.join(uninit)}",
                    remediation="git submodule update --init --recursive",
                )
            else:
                self.add_check("dev:git-submodules", "PASS", "git submodules are initialized")

    # ------------------------------------------------------------------
    # Role: flash
    # ------------------------------------------------------------------

    def apply_flash(self) -> None:
        self.apply_serial_prereqs(include_openocd=True, include_plugdev=True)

    def check_flash(self) -> None:
        self.check_serial_prereqs(include_openocd=True, include_plugdev=True, role_hint="flash")

    # ------------------------------------------------------------------
    # Role: companion
    # ------------------------------------------------------------------

    def _resolved_workspace(self) -> Path:
        ws = expand_path(self.args.workspace, self.env.home)
        if not ws.is_absolute():
            ws = self.env.cwd / ws
        return ws

    def apply_companion(self) -> None:
        self.apply_serial_prereqs(include_openocd=False, include_plugdev=False)
        self.apply_ros2_repo()
        self.ensure_apt_packages(COMPANION_ROS_PACKAGES)

        if not self.env.path_exists(ROSDEP_DEFAULT_LIST):
            self.run_action(["rosdep", "init"], privileged=True)
        else:
            self.out(f"[ok] rosdep is already initialized ({ROSDEP_DEFAULT_LIST})")

        self.run_action(["rosdep", "update"])

        workspace = self._resolved_workspace()
        if self.args.build_bridge:
            repo_dir = find_repo_dir(self.env, self.args.repo_dir, Path("bridge/vehkernel_bridge/package.xml"))
            if repo_dir is None:
                raise SetupError(
                    "Cannot find bridge/vehkernel_bridge/package.xml for --build-bridge. "
                    "Run from a Vehkernel checkout or pass --repo-dir <path>."
                )
            bridge_dir = repo_dir / "bridge"
            if self.dry_run:
                self.out(f"[dry-run] mkdir -p {workspace}")
            else:
                self.env.mkdir(workspace)

            self.run_action(
                [
                    "rosdep",
                    "install",
                    "--from-paths",
                    str(bridge_dir),
                    "--ignore-src",
                    "--rosdistro",
                    ROS_DISTRO,
                    "-y",
                ],
                cwd=workspace,
                env_overrides=self._apt_env(),
            )
            colcon_script = (
                f"set -e; source {shlex.quote(str(ROS_SETUP_BASH))} && "
                f"colcon build --base-paths {shlex.quote(str(bridge_dir))} --packages-up-to vehkernel_bridge"
            )
            self.run_action(["bash", "-c", colcon_script], cwd=workspace)

        if self.args.install_bridge_service:
            unit_bytes = self._template_bytes(Path("systemd/vehkernel-bridge.service"))
            unit_dest = self.env.home / ".config" / "systemd" / "user" / "vehkernel-bridge.service"
            self.write_user_file(unit_dest, unit_bytes, mode=0o644)

            env_dest = self.env.home / ".config" / "vehkernel" / "bridge.env"
            if self.env.path_exists(env_dest):
                self.out(f"[ok] preserving existing {env_dest}")
            else:
                env_content = render_bridge_env(
                    workspace=workspace,
                    device=self.args.device,
                    baud=self.args.baud,
                    ros_domain_id=self.args.ros_domain_id,
                ).encode("utf-8")
                self.write_user_file(env_dest, env_content, mode=0o644)

            if self.env.systemd_running():
                self.run_action(["systemctl", "--user", "daemon-reload"])
                self.run_action(["systemctl", "--user", "enable", "vehkernel-bridge.service"])
                if self.env.which("loginctl") is not None:
                    self.run_action(["loginctl", "enable-linger", self.env.user], privileged=True)
                else:
                    self.out("[skip] loginctl not found; skipping enable-linger")
            else:
                self.out("[skip] systemd is not running; skipping systemctl enable and loginctl enable-linger")

    def check_companion(self) -> None:
        self.check_serial_prereqs(include_openocd=False, include_plugdev=False, role_hint="companion")
        self.check_ros2_repo("companion")

        for pkg in COMPANION_ROS_PACKAGES:
            self._check_apt_package(pkg, "companion")

        required_tools = (
            ("g++", "build-essential"),
            ("colcon", "python3-colcon-common-extensions"),
            ("rosdep", "python3-rosdep"),
        )
        for tool, pkg_hint in required_tools:
            found = self.env.which(tool)
            if found is not None:
                self.add_check(f"cmd:{tool}", "PASS", f"{tool} found at {found}")
            else:
                self.add_check(
                    f"cmd:{tool}",
                    "FAIL",
                    f"{tool} is not in PATH",
                    remediation=self._sudo_cmd_hint(f"apt-get install -y {pkg_hint}"),
                )

        self.check_ros_setup_bash("companion")

        if self.env.path_exists(ROSDEP_DEFAULT_LIST):
            self.add_check("ros2:rosdep-init", "PASS", f"rosdep is initialized ({ROSDEP_DEFAULT_LIST})")
        else:
            self.add_check(
                "ros2:rosdep-init",
                "FAIL",
                f"rosdep is not initialized ({ROSDEP_DEFAULT_LIST} missing)",
                remediation=f"{self._sudo_cmd_hint('rosdep init')} && rosdep update",
            )

        workspace = self._resolved_workspace()
        if self.args.build_bridge:
            ws_setup = workspace / "install" / "setup.bash"
            if self.env.path_exists(ws_setup):
                self.add_check("companion:bridge-build", "PASS", f"Bridge workspace built ({ws_setup})")
            else:
                self.add_check(
                    "companion:bridge-build",
                    "FAIL",
                    f"Bridge workspace not built ({ws_setup} missing)",
                    remediation=f"tools/setup/vk_setup.py --role companion --workspace {workspace} --build-bridge",
                )

        unit_dest = self.env.home / ".config" / "systemd" / "user" / "vehkernel-bridge.service"
        env_dest = self.env.home / ".config" / "vehkernel" / "bridge.env"
        if self.args.install_bridge_service or self.env.path_exists(unit_dest):
            unit_src = self.env.script_dir / "systemd" / "vehkernel-bridge.service"
            expected_unit = self.env.read_bytes(unit_src)
            installed_unit = self.env.read_bytes(unit_dest)
            if installed_unit is None:
                self.add_check(
                    "companion:bridge-service",
                    "FAIL",
                    f"{unit_dest} is not installed",
                    remediation="tools/setup/vk_setup.py --role companion --install-bridge-service",
                )
            elif expected_unit is not None and installed_unit != expected_unit:
                self.add_check(
                    "companion:bridge-service",
                    "FAIL",
                    f"{unit_dest} differs from bundled {unit_src}",
                    remediation="tools/setup/vk_setup.py --role companion --install-bridge-service",
                )
            else:
                self.add_check("companion:bridge-service", "PASS", f"{unit_dest} is installed")

            if self.env.path_exists(env_dest):
                self.add_check("companion:bridge-env", "PASS", f"{env_dest} exists")
            else:
                self.add_check(
                    "companion:bridge-env",
                    "FAIL",
                    f"{env_dest} is missing",
                    remediation="tools/setup/vk_setup.py --role companion --install-bridge-service",
                )

            if self.env.systemd_running():
                if self.env.service_is_enabled("vehkernel-bridge.service", user=True):
                    self.add_check("companion:bridge-service-enabled", "PASS", "vehkernel-bridge.service is enabled")
                else:
                    self.add_check(
                        "companion:bridge-service-enabled",
                        "FAIL",
                        "vehkernel-bridge.service is not enabled",
                        remediation="systemctl --user enable vehkernel-bridge.service",
                    )

                linger_file = LINGER_DIR / self.env.user
                if self.env.path_exists(linger_file):
                    self.add_check("companion:linger", "PASS", f"systemd lingering enabled for '{self.env.user}'")
                else:
                    self.add_check(
                        "companion:linger",
                        "WARN",
                        f"systemd lingering is not enabled for '{self.env.user}' (Bridge will not start before login)",
                        remediation=self._sudo_cmd_hint(f"loginctl enable-linger {self.env.user}"),
                    )

    # ------------------------------------------------------------------
    # Role: desktop
    # ------------------------------------------------------------------

    def apply_desktop(self) -> None:
        self.apply_ros2_repo()
        self.ensure_apt_packages(DESKTOP_ROS_PACKAGES)

    def check_desktop(self) -> None:
        self.check_ros2_repo("desktop")
        for pkg in DESKTOP_ROS_PACKAGES:
            self._check_apt_package(pkg, "desktop")
        self.check_ros_setup_bash("desktop")

        raw_domain = self.env.environ.get("ROS_DOMAIN_ID")
        if raw_domain is None or raw_domain == "":
            self.add_check(
                "desktop:ros-domain-id",
                "PASS",
                "ROS_DOMAIN_ID is unset (defaults to 0; ensure it matches the companion computer)",
            )
        else:
            try:
                domain_val = int(raw_domain)
                if 0 <= domain_val <= 232:
                    self.add_check(
                        "desktop:ros-domain-id",
                        "PASS",
                        f"ROS_DOMAIN_ID={domain_val} (ensure it matches the companion computer)",
                    )
                else:
                    self.add_check(
                        "desktop:ros-domain-id",
                        "FAIL",
                        f"ROS_DOMAIN_ID={raw_domain!r} is outside valid DDS range 0..232",
                        remediation="export ROS_DOMAIN_ID=0",
                    )
            except ValueError:
                self.add_check(
                    "desktop:ros-domain-id",
                    "FAIL",
                    f"ROS_DOMAIN_ID={raw_domain!r} is not an integer",
                    remediation="export ROS_DOMAIN_ID=0",
                )

        if self.env.which("ufw") is None:
            self.add_check("desktop:ufw", "PASS", "ufw firewall is not installed")
        else:
            ufw_res = self.env.run_cmd(["ufw", "status"], capture=True)
            if ufw_res.returncode != 0:
                self.add_check(
                    "desktop:ufw",
                    "WARN",
                    "ufw is installed but could not be queried without root privileges",
                    remediation=(
                        "Run 'sudo ufw status' and ensure ROS2 DDS UDP ports "
                        "(7400-7500/udp) are allowed if active"
                    ),
                )
            elif "Status: inactive" in ufw_res.stdout:
                self.add_check("desktop:ufw", "PASS", "ufw firewall is inactive")
            elif "Status: active" in ufw_res.stdout:
                if "7400" in ufw_res.stdout:
                    self.add_check("desktop:ufw", "PASS", "ufw firewall is active with DDS UDP port rule present")
                else:
                    self.add_check(
                        "desktop:ufw",
                        "WARN",
                        "ufw firewall is active without an explicit ROS2 DDS UDP rule (ports 7400-7500)",
                        remediation="sudo ufw allow 7400:7500/udp",
                    )
            else:
                self.add_check("desktop:ufw", "PASS", "ufw status checked")

        if self.env.systemd_running() and self.env.which("timedatectl") is not None:
            ntp_res = self.env.run_cmd(
                ["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                capture=True,
            )
            if ntp_res.returncode == 0 and ntp_res.stdout.strip() == "yes":
                self.add_check("desktop:clock-sync", "PASS", "system clock is NTP-synchronized")
            else:
                self.add_check(
                    "desktop:clock-sync",
                    "WARN",
                    "system clock is not NTP-synchronized; RViz drops TF/messages if clocks drift",
                    remediation="sudo timedatectl set-ntp true && tools/setup/ntp_offset.py pool.ntp.org",
                )
        else:
            self.add_check(
                "desktop:clock-sync",
                "PASS",
                "timedatectl unavailable; check clock offset with tools/setup/ntp_offset.py pool.ntp.org",
            )

    # ------------------------------------------------------------------
    # Top-level dispatch
    # ------------------------------------------------------------------

    def run(self) -> int:
        if self.args.check:
            return self._run_check()
        return self._run_apply()

    def _run_apply(self) -> int:
        mode_label = "dry-run" if self.dry_run else "apply"
        self.out(
            f"vk_setup ({mode_label}): OS={self.os_info.pretty_name} "
            f"({self.os_info.codename}/{self.os_info.dpkg_arch}), roles={', '.join(self.roles)}"
        )
        dispatch = {
            "dev": self.apply_dev,
            "flash": self.apply_flash,
            "companion": self.apply_companion,
            "desktop": self.apply_desktop,
        }
        for role in self.roles:
            self.out(f"\n==> Role: {role}")
            dispatch[role]()

        if self.added_groups:
            groups_str = ", ".join(self.added_groups)
            if self.dry_run:
                self.out(
                    f"\n[dry-run] Note: adding user '{self.env.user}' to group(s) ({groups_str}) "
                    "will require logging out and back in to take effect."
                )
            else:
                self.out(
                    f"\nNote: added user '{self.env.user}' to group(s): {groups_str}. "
                    "Log out and log back in for the new group membership to take effect."
                )
        return 0

    def _run_check(self) -> int:
        self.out(
            f"vk_setup (check): OS={self.os_info.pretty_name} "
            f"({self.os_info.codename}/{self.os_info.dpkg_arch}), roles={', '.join(self.roles)}"
        )
        dispatch = {
            "dev": self.check_dev,
            "flash": self.check_flash,
            "companion": self.check_companion,
            "desktop": self.check_desktop,
        }
        for role in self.roles:
            dispatch[role]()

        passed = 0
        warned = 0
        failed = 0
        for check in self.checks:
            self.out(f"[{check.status}] {check.name}: {check.detail}")
            if check.remediation and check.status in ("FAIL", "WARN"):
                self.out(f"       remediation: {check.remediation}")
            if check.status == "PASS":
                passed += 1
            elif check.status == "WARN":
                warned += 1
            else:
                failed += 1

        self.out(f"\nSummary: {passed} passed, {warned} warnings, {failed} failed")
        return 1 if failed > 0 else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="vk_setup.py",
        description="Install, preview, or verify host and companion prerequisites for Vehkernel.",
    )
    parser.add_argument(
        "--role",
        action="append",
        choices=ALL_ROLES,
        required=True,
        help="Machine role to configure or check (repeatable, e.g. --role dev --role flash).",
    )

    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument(
        "--check",
        action="store_true",
        help="Read-only doctor: check prerequisites and print pass/fail/warn with remediation.",
    )
    mode_group.add_argument(
        "--dry-run",
        action="store_true",
        help="Print every planned command and file write without modifying the system.",
    )

    parser.add_argument(
        "--yes",
        "-y",
        action="store_true",
        help="Pass -y and DEBIAN_FRONTEND=noninteractive to apt-get.",
    )
    parser.add_argument(
        "--no-sudo",
        action="store_true",
        help="Do not prefix privileged commands with sudo (also omitted automatically when root).",
    )

    dev_group = parser.add_argument_group("dev role options")
    dev_group.add_argument(
        "--build-local",
        action="store_true",
        help="Build .devcontainer/Dockerfile locally as vehkernel-dev:latest instead of pulling.",
    )
    dev_group.add_argument(
        "--image",
        default=DEFAULT_IMAGE,
        help=f"Development container image reference to pull/check (default: {DEFAULT_IMAGE}).",
    )

    comp_group = parser.add_argument_group("companion role options")
    comp_group.add_argument(
        "--workspace",
        default="~/vk_ws",
        help="Colcon workspace path for building/running vehkernel_bridge (default: ~/vk_ws).",
    )
    comp_group.add_argument(
        "--build-bridge",
        action="store_true",
        help="Run rosdep install and colcon build for vehkernel_bridge in --workspace.",
    )
    comp_group.add_argument(
        "--repo-dir",
        default=None,
        help="Path to Vehkernel git checkout (auto-detected when run inside a checkout).",
    )
    comp_group.add_argument(
        "--install-bridge-service",
        action="store_true",
        help="Install and enable ~/.config/systemd/user/vehkernel-bridge.service and bridge.env.",
    )
    comp_group.add_argument(
        "--device",
        default="/dev/ttyACM0",
        help="Serial device written to bridge.env when creating it (default: /dev/ttyACM0).",
    )
    comp_group.add_argument(
        "--baud",
        type=int,
        default=921600,
        help="Serial baud rate written to bridge.env when creating it (default: 921600).",
    )
    comp_group.add_argument(
        "--ros-domain-id",
        type=int,
        default=0,
        help="ROS_DOMAIN_ID written to bridge.env when creating it (default: 0).",
    )

    return parser


def main(argv: list[str] | None = None, *, env: SystemEnv | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    sys_env = env if env is not None else SystemEnv()

    if args.baud <= 0:
        sys_env.stderr.write("vk_setup.py: error: --baud must be positive\n")
        return 2
    if not (0 <= args.ros_domain_id <= 232):
        sys_env.stderr.write("vk_setup.py: error: --ros-domain-id must be between 0 and 232\n")
        return 2

    roles = deduplicate_roles(args.role)
    try:
        os_info = detect_os(sys_env)
        validate_os_and_roles(os_info, roles)
        runner = SetupRunner(sys_env, args, os_info, roles)
        return runner.run()
    except UsageError as exc:
        sys_env.stderr.write(f"vk_setup.py: error: {exc}\n")
        return 2
    except SetupError as exc:
        sys_env.stderr.write(f"vk_setup.py: error: {exc}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
