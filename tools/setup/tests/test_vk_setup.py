# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/setup/vk_setup.py (ADR-0028, spec 0017)."""

from __future__ import annotations

import importlib.util
import io
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

SETUP_DIR = Path(__file__).resolve().parent.parent
TOOL = SETUP_DIR / "vk_setup.py"

UBUNTU_2404_OS_RELEASE = """\
PRETTY_NAME="Ubuntu 24.04.1 LTS"
NAME="Ubuntu"
VERSION_ID="24.04"
VERSION="24.04.1 LTS (Noble Numbat)"
VERSION_CODENAME=noble
ID=ubuntu
ID_LIKE=debian
"""

DEBIAN_12_OS_RELEASE = """\
PRETTY_NAME="Debian GNU/Linux 12 (bookworm)"
NAME="Debian GNU/Linux"
VERSION_ID="12"
VERSION_CODENAME=bookworm
ID=debian
"""

RPI_OS_12_OS_RELEASE = """\
PRETTY_NAME="Raspbian GNU/Linux 12 (bookworm)"
NAME="Raspbian GNU/Linux"
VERSION_ID="12"
VERSION_CODENAME=bookworm
ID=raspbian
ID_LIKE=debian
"""

UBUNTU_2204_OS_RELEASE = """\
PRETTY_NAME="Ubuntu 22.04.4 LTS"
VERSION_ID="22.04"
VERSION_CODENAME=jammy
ID=ubuntu
"""


def load_module():
    spec = importlib.util.spec_from_file_location("vk_setup", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["vk_setup"] = module
    spec.loader.exec_module(module)
    return module


vk_setup = load_module()


class FakeSystemEnv(vk_setup.SystemEnv):
    """In-memory fake SystemEnv that records commands and file mutations without touching the host."""

    def __init__(
        self,
        *,
        os_release: str | None = UBUNTU_2404_OS_RELEASE,
        arch: str = "x86_64",
        euid: int = 1000,
        user: str = "pilot",
        home: Path = Path("/home/pilot"),
        cwd: Path = Path("/home/pilot"),
        script_dir: Path | None = None,
        environ: dict[str, str] | None = None,
        bins: set[str] | None = None,
        packages: set[str] | None = None,
        existing_groups: set[str] | None = None,
        user_groups: set[str] | None = None,
        active_groups: set[str] | None = None,
        files: dict[Path, bytes] | None = None,
        executable_files: set[Path] | None = None,
        dirs: set[Path] | None = None,
        systemd_active: bool = True,
        active_services: set[tuple[str, bool]] | None = None,
        enabled_services: set[tuple[str, bool]] | None = None,
        cmd_handlers: dict[tuple[str, ...], vk_setup.CmdResult] | None = None,
    ) -> None:
        self._stdout = io.StringIO()
        self._stderr = io.StringIO()
        super().__init__(
            script_dir=script_dir if script_dir is not None else SETUP_DIR,
            stdout=self._stdout,
            stderr=self._stderr,
        )
        self._os_release = os_release
        self._arch = arch
        self._euid = euid
        self._user = user
        self._home = home
        self._cwd = cwd
        self._environ = (
            dict(environ)
            if environ is not None
            else {"PATH": f"{home / '.local' / 'bin'}:/usr/local/bin:/usr/bin:/bin"}
        )
        self.bins: set[str] = (
            set(bins)
            if bins is not None
            else {"sudo", "apt-get", "dpkg-query", "udevadm", "systemctl", "loginctl", "git", "curl"}
        )
        self.packages: set[str] = set(packages) if packages is not None else set()
        self.existing_groups: set[str] = (
            set(existing_groups)
            if existing_groups is not None
            else {"dialout", "plugdev", "docker"}
        )
        self.user_groups_set: set[str] = set(user_groups) if user_groups is not None else set()
        self._active_groups: set[str] | None = set(active_groups) if active_groups is not None else None
        self.files: dict[Path, bytes] = dict(files) if files is not None else {}
        self.executable_files: set[Path] = set(executable_files) if executable_files is not None else set()
        self.dirs: set[Path] = set(dirs) if dirs is not None else {Path("/run/udev"), Path("/usr/share/keyrings")}
        if systemd_active:
            self.dirs.add(Path("/run/systemd/system"))
        self.active_services: set[tuple[str, bool]] = set(active_services) if active_services is not None else set()
        self.enabled_services: set[tuple[str, bool]] = set(enabled_services) if enabled_services is not None else set()
        self.cmd_handlers: dict[tuple[str, ...], vk_setup.CmdResult] = dict(cmd_handlers) if cmd_handlers else {}

        self.commands: list[dict] = []
        self.written_files: list[tuple[Path, bytes, int | None]] = []
        self.created_dirs: list[Path] = []

        # Populate bundled templates from real SETUP_DIR unless overridden in self.files.
        for rel in (Path("vk"), Path("udev/99-vehkernel.rules"), Path("systemd/vehkernel-bridge.service")):
            real_path = SETUP_DIR / rel
            target_path = self.script_dir / rel
            if target_path not in self.files and real_path.exists():
                self.files[target_path] = real_path.read_bytes()

    @property
    def output(self) -> str:
        return self._stdout.getvalue()

    @property
    def error_output(self) -> str:
        return self._stderr.getvalue()

    @property
    def euid(self) -> int:
        return self._euid

    @property
    def user(self) -> str:
        return self._user

    @property
    def home(self) -> Path:
        return self._home

    @property
    def cwd(self) -> Path:
        return self._cwd

    @property
    def environ(self) -> dict[str, str]:
        return dict(self._environ)

    def read_os_release(self) -> str | None:
        return self._os_release

    def machine(self) -> str:
        return self._arch

    def which(self, cmd: str) -> str | None:
        return f"/usr/bin/{cmd}" if cmd in self.bins else None

    def path_exists(self, path: Path) -> bool:
        return path in self.files or path in self.dirs

    def is_dir(self, path: Path) -> bool:
        return path in self.dirs

    def is_executable(self, path: Path) -> bool:
        return path in self.executable_files

    def read_text(self, path: Path) -> str | None:
        data = self.files.get(path)
        return data.decode("utf-8") if data is not None else None

    def read_bytes(self, path: Path) -> bytes | None:
        return self.files.get(path)

    def mkdir(self, path: Path) -> None:
        self.dirs.add(path)
        self.created_dirs.append(path)

    def write_bytes(self, path: Path, content: bytes, mode: int | None = None) -> None:
        self.files[path] = content
        self.dirs.add(path.parent)
        if mode is not None and (mode & stat.S_IXUSR):
            self.executable_files.add(path)
        else:
            self.executable_files.discard(path)
        self.written_files.append((path, content, mode))

    def group_exists(self, group: str) -> bool:
        return group in self.existing_groups

    def configured_groups(self, user: str) -> set[str]:
        return set(self.user_groups_set)

    def active_groups(self) -> set[str]:
        if self._active_groups is not None:
            return set(self._active_groups)
        return set(self.user_groups_set)

    def service_is_active(self, unit: str, *, user: bool = False) -> bool:
        if not self.systemd_running():
            return False
        return (unit, user) in self.active_services

    def service_is_enabled(self, unit: str, *, user: bool = False) -> bool:
        if not self.systemd_running():
            return False
        return (unit, user) in self.enabled_services

    def dpkg_installed(self, package: str) -> bool:
        return package in self.packages

    def run_cmd(
        self,
        argv: list[str],
        *,
        cwd: Path | None = None,
        env_overrides: dict[str, str] | None = None,
        input_data: bytes | None = None,
        capture: bool = False,
    ) -> vk_setup.CmdResult:
        self.commands.append(
            {
                "argv": list(argv),
                "cwd": cwd,
                "env": env_overrides,
                "input": input_data,
                "capture": capture,
            }
        )
        key = tuple(argv)
        if key in self.cmd_handlers:
            return self.cmd_handlers[key]
        # Simulate tee writes into self.files so idempotency/state checks see them.
        stripped = argv[1:] if argv and argv[0] == "sudo" else argv
        if len(stripped) >= 2 and stripped[0] == "tee" and input_data is not None:
            self.files[Path(stripped[1])] = input_data
        return vk_setup.CmdResult(returncode=0)

    def command_argvs(self) -> list[list[str]]:
        return [entry["argv"] for entry in self.commands]


# ---------------------------------------------------------------------------
# L0 / CLI basics
# ---------------------------------------------------------------------------


SPDX_TAG = "SPDX-License-Identifier"
EXPECTED_SPDX = f"# {SPDX_TAG}: GPL-3.0-or-later"


def test_tool_executable_and_spdx_header():
    assert os.access(TOOL, os.X_OK), "tools/setup/vk_setup.py must be executable"
    lines = TOOL.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "#!/usr/bin/env python3"
    assert lines[1] == EXPECTED_SPDX


def test_cli_help_lists_roles_and_flags():
    result = subprocess.run(
        [sys.executable, str(TOOL), "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0
    for token in (
        "--role",
        "dev",
        "flash",
        "companion",
        "desktop",
        "--check",
        "--dry-run",
        "--yes",
        "--no-sudo",
        "--build-local",
        "--image",
        "--workspace",
        "--build-bridge",
        "--repo-dir",
        "--install-bridge-service",
        "--device",
        "--baud",
        "--ros-domain-id",
    ):
        assert token in result.stdout


def test_check_and_dry_run_are_mutually_exclusive():
    with pytest.raises(SystemExit) as excinfo:
        vk_setup.main(["--role", "flash", "--check", "--dry-run"])
    assert excinfo.value.code == 2


@pytest.mark.parametrize(
    "bad_args",
    [
        ["--role", "companion", "--baud", "0"],
        ["--role", "companion", "--baud", "-921600"],
        ["--role", "companion", "--ros-domain-id", "-1"],
        ["--role", "companion", "--ros-domain-id", "233"],
    ],
)
def test_invalid_numeric_flags_return_exit_2(bad_args):
    env = FakeSystemEnv()
    rc = vk_setup.main(bad_args, env=env)
    assert rc == 2
    assert "error:" in env.error_output


def test_deduplicate_roles_preserves_order():
    assert vk_setup.deduplicate_roles(["flash", "dev", "flash", "companion", "dev"]) == [
        "flash",
        "dev",
        "companion",
    ]


# ---------------------------------------------------------------------------
# OS & architecture matrix
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("arch,dpkg_arch", [("x86_64", "amd64"), ("aarch64", "arm64")])
@pytest.mark.parametrize("role", ["dev", "flash", "companion", "desktop"])
def test_ubuntu_2404_supports_all_roles(arch, dpkg_arch, role):
    env = FakeSystemEnv(os_release=UBUNTU_2404_OS_RELEASE, arch=arch)
    rc = vk_setup.main(["--role", role, "--dry-run"], env=env)
    assert rc == 0
    assert f"noble/{dpkg_arch}" in env.output


@pytest.mark.parametrize("os_release", [DEBIAN_12_OS_RELEASE, RPI_OS_12_OS_RELEASE])
@pytest.mark.parametrize("arch", ["x86_64", "aarch64"])
def test_debian_12_and_rpi_os_support_flash(os_release, arch):
    env = FakeSystemEnv(os_release=os_release, arch=arch)
    rc = vk_setup.main(["--role", "flash", "--dry-run"], env=env)
    assert rc == 0
    assert "bookworm/" in env.output


@pytest.mark.parametrize("os_release", [DEBIAN_12_OS_RELEASE, RPI_OS_12_OS_RELEASE])
@pytest.mark.parametrize("role", ["dev", "companion", "desktop"])
def test_debian_12_and_rpi_os_reject_non_flash_roles(os_release, role):
    env = FakeSystemEnv(os_release=os_release, arch="aarch64")
    rc = vk_setup.main(["--role", role, "--dry-run"], env=env)
    assert rc == 2
    assert "Ubuntu 24.04" in env.error_output
    assert "--role flash" in env.error_output
    assert env.commands == []


@pytest.mark.parametrize(
    "os_release,arch",
    [
        (UBUNTU_2204_OS_RELEASE, "x86_64"),
        ('ID=fedora\nVERSION_ID="40"\nPRETTY_NAME="Fedora Linux 40"\n', "x86_64"),
        (None, "x86_64"),
        (UBUNTU_2404_OS_RELEASE, "armv7l"),
        (UBUNTU_2404_OS_RELEASE, "riscv64"),
    ],
)
def test_unsupported_os_or_arch_fails_fast(os_release, arch):
    env = FakeSystemEnv(os_release=os_release, arch=arch)
    rc = vk_setup.main(["--role", "flash", "--check"], env=env)
    assert rc == 2
    assert "error:" in env.error_output


# ---------------------------------------------------------------------------
# Role: dev
# ---------------------------------------------------------------------------


def test_dev_apply_installs_docker_pulls_image_and_installs_vk():
    env = FakeSystemEnv(bins={"sudo", "apt-get", "dpkg-query", "git"})
    rc = vk_setup.main(["--role", "dev", "--yes"], env=env)
    assert rc == 0

    argvs = env.command_argvs()
    assert ["sudo", "env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "update"] in argvs
    assert [
        "sudo",
        "env",
        "DEBIAN_FRONTEND=noninteractive",
        "apt-get",
        "install",
        "-y",
        "docker.io",
        "docker-buildx",
    ] in argvs
    assert ["sudo", "usermod", "-aG", "docker", "pilot"] in argvs
    # Fresh non-root session is not yet in active 'docker' group -> uses sudo docker pull.
    assert ["sudo", "docker", "pull", vk_setup.DEFAULT_IMAGE] in argvs

    vk_dest = env.home / ".local" / "bin" / "vk"
    assert vk_dest in env.files
    assert env.files[vk_dest] == (SETUP_DIR / "vk").read_bytes()
    assert env.is_executable(vk_dest)
    assert "Log out and log back in" in env.output


def test_dev_apply_uses_sudo_docker_when_group_not_active_and_plain_docker_when_active():
    repo = Path("/work/VehKernel")
    dockerfile = repo / ".devcontainer" / "Dockerfile"

    # Configured in /etc/group, but current login session lacks active 'docker' group -> sudo docker build.
    fresh_env = FakeSystemEnv(
        bins={"sudo", "docker"},
        user_groups={"docker"},
        active_groups=set(),
        files={dockerfile: b"FROM ubuntu:24.04\n"},
    )
    assert vk_setup.main(["--role", "dev", "--build-local", "--repo-dir", str(repo)], env=fresh_env) == 0
    assert [
        "sudo",
        "docker",
        "build",
        "-t",
        "vehkernel-dev:latest",
        "-f",
        str(dockerfile),
        str(repo),
    ] in fresh_env.command_argvs()

    # Active 'docker' group -> plain docker pull without sudo.
    active_env = FakeSystemEnv(
        bins={"sudo", "docker"},
        user_groups={"docker"},
        active_groups={"docker"},
    )
    assert vk_setup.main(["--role", "dev"], env=active_env) == 0
    assert ["docker", "pull", vk_setup.DEFAULT_IMAGE] in active_env.command_argvs()
    assert ["sudo", "docker", "pull", vk_setup.DEFAULT_IMAGE] not in active_env.command_argvs()


def test_dev_apply_preserves_existing_docker_ce():
    env = FakeSystemEnv(
        bins={"sudo", "apt-get", "dpkg-query", "docker", "git"},
        user_groups={"docker"},
    )
    rc = vk_setup.main(["--role", "dev"], env=env)
    assert rc == 0
    argvs = env.command_argvs()
    assert not any("docker.io" in cmd for cmd in argvs)
    assert not any("usermod" in cmd for cmd in argvs)
    assert ["docker", "pull", vk_setup.DEFAULT_IMAGE] in argvs


def test_dev_apply_pull_failure_prints_build_local_hint():
    env = FakeSystemEnv(
        bins={"sudo", "docker"},
        user_groups={"docker"},
        cmd_handlers={
            ("docker", "pull", vk_setup.DEFAULT_IMAGE): vk_setup.CmdResult(returncode=1, stderr="network error"),
        },
    )
    rc = vk_setup.main(["--role", "dev"], env=env)
    assert rc == 1
    assert "--build-local" in env.error_output


def test_dev_apply_build_local_uses_devcontainer_dockerfile():
    repo = Path("/work/VehKernel")
    dockerfile = repo / ".devcontainer" / "Dockerfile"
    env = FakeSystemEnv(
        bins={"sudo", "docker"},
        user_groups={"docker"},
        files={dockerfile: b"FROM ubuntu:24.04\n"},
    )
    rc = vk_setup.main(["--role", "dev", "--build-local", "--repo-dir", str(repo)], env=env)
    assert rc == 0
    assert [
        "docker",
        "build",
        "-t",
        "vehkernel-dev:latest",
        "-f",
        str(dockerfile),
        str(repo),
    ] in env.command_argvs()


def test_dev_check_pass_and_fallback_local_image():
    repo = Path("/work/VehKernel")
    vk_dest = Path("/home/pilot/.local/bin/vk")
    env = FakeSystemEnv(
        bins={"docker", "git"},
        user_groups={"docker"},
        files={
            vk_dest: (SETUP_DIR / "vk").read_bytes(),
            repo / ".gitmodules": b"[submodule]\n",
        },
        executable_files={vk_dest},
        cmd_handlers={
            ("docker", "image", "inspect", vk_setup.DEFAULT_IMAGE): vk_setup.CmdResult(returncode=1),
            ("docker", "image", "inspect", vk_setup.LOCAL_IMAGE): vk_setup.CmdResult(returncode=0),
            ("git", "-C", str(repo), "submodule", "status"): vk_setup.CmdResult(
                returncode=0,
                stdout=" abc123 external/cmsis\n",
            ),
        },
    )
    rc = vk_setup.main(["--role", "dev", "--repo-dir", str(repo), "--check"], env=env)
    assert rc == 0
    assert "[FAIL]" not in env.output
    assert "local fallback container image" in env.output
    assert env.written_files == []


def test_dev_check_reports_failures_and_relogin_warning():
    repo = Path("/work/VehKernel")
    env = FakeSystemEnv(
        bins={"docker", "git"},
        environ={"PATH": "/usr/bin:/bin"},
        user_groups={"docker"},
        active_groups=set(),  # in /etc/group but not current session
        files={repo / ".gitmodules": b"[submodule]\n"},
        cmd_handlers={
            ("docker", "image", "inspect", vk_setup.DEFAULT_IMAGE): vk_setup.CmdResult(
                returncode=1,
                stderr="permission denied while trying to connect to the Docker daemon socket",
            ),
            ("docker", "image", "inspect", vk_setup.LOCAL_IMAGE): vk_setup.CmdResult(
                returncode=1,
                stderr="permission denied while trying to connect to the Docker daemon socket",
            ),
            ("git", "-C", str(repo), "submodule", "status"): vk_setup.CmdResult(
                returncode=0,
                stdout="-abc123 external/cmsis\n",
            ),
        },
    )
    rc = vk_setup.main(["--role", "dev", "--repo-dir", str(repo), "--check"], env=env)
    assert rc == 1
    assert "[WARN] group:docker:" in env.output
    assert "[WARN] dev:local-bin-path:" in env.output
    assert "[FAIL] dev:image:" in env.output
    assert "permission denied" in env.output.lower()
    assert "newgrp docker" in env.output
    assert "docker pull" not in env.output
    assert "[FAIL] dev:vk-wrapper:" in env.output
    assert "[FAIL] dev:git-submodules:" in env.output
    assert "git submodule update --init --recursive" in env.output


def test_dev_check_reports_docker_pull_when_active_group_lacks_image():
    vk_dest = Path("/home/pilot/.local/bin/vk")
    env = FakeSystemEnv(
        bins={"docker"},
        user_groups={"docker"},
        active_groups={"docker"},
        files={vk_dest: (SETUP_DIR / "vk").read_bytes()},
        executable_files={vk_dest},
        cmd_handlers={
            ("docker", "image", "inspect", vk_setup.DEFAULT_IMAGE): vk_setup.CmdResult(
                returncode=1, stderr="Error: No such image"
            ),
            ("docker", "image", "inspect", vk_setup.LOCAL_IMAGE): vk_setup.CmdResult(
                returncode=1, stderr="Error: No such image"
            ),
        },
    )
    rc = vk_setup.main(["--role", "dev", "--check"], env=env)
    assert rc == 1
    assert "[FAIL] dev:image:" in env.output
    assert f"docker pull {vk_setup.DEFAULT_IMAGE}" in env.output


# ---------------------------------------------------------------------------
# Role: flash
# ---------------------------------------------------------------------------


def test_flash_apply_full_host_setup_and_mitigations():
    env = FakeSystemEnv(
        packages={"brltty"},
        active_services={("ModemManager.service", False)},
    )
    rc = vk_setup.main(["--role", "flash", "--yes"], env=env)
    assert rc == 0

    argvs = env.command_argvs()
    assert ["sudo", "env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "update"] in argvs
    assert [
        "sudo",
        "env",
        "DEBIAN_FRONTEND=noninteractive",
        "apt-get",
        "install",
        "-y",
        "python3-serial",
        "stm32flash",
        "openocd",
    ] in argvs
    assert ["sudo", "usermod", "-aG", "dialout", "pilot"] in argvs
    assert ["sudo", "usermod", "-aG", "plugdev", "pilot"] in argvs
    assert ["sudo", "tee", str(vk_setup.UDEV_DEST)] in argvs
    assert ["sudo", "chmod", "0644", str(vk_setup.UDEV_DEST)] in argvs
    assert ["sudo", "udevadm", "control", "--reload-rules"] in argvs
    assert ["sudo", "udevadm", "trigger"] in argvs
    assert ["sudo", "systemctl", "disable", "--now", "ModemManager.service"] in argvs
    assert ["sudo", "env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "purge", "-y", "brltty"] in argvs
    # Under sudo, DEBIAN_FRONTEND is passed via `sudo env ...` so sudo env_reset cannot strip it.
    apt_cmds = [c for c in env.commands if "apt-get" in c["argv"]]
    assert all(c["argv"][:3] == ["sudo", "env", "DEBIAN_FRONTEND=noninteractive"] for c in apt_cmds)
    assert all(c["env"] is None for c in apt_cmds)


def test_flash_apply_skips_plugdev_when_absent_and_skips_udev_systemd_in_container():
    env = FakeSystemEnv(
        os_release=DEBIAN_12_OS_RELEASE,
        euid=0,
        user="root",
        bins={"apt-get", "dpkg-query"},  # no sudo, no udevadm, no systemctl
        existing_groups={"dialout"},  # no plugdev
        dirs=set(),  # no /run/udev, no /run/systemd/system
        systemd_active=False,
    )
    rc = vk_setup.main(["--role", "flash", "--yes"], env=env)
    assert rc == 0
    argvs = env.command_argvs()
    assert not any(cmd[0] == "sudo" for cmd in argvs)
    assert not any("usermod" in cmd for cmd in argvs)
    assert not any("udevadm" in cmd for cmd in argvs)
    assert not any("systemctl" in cmd for cmd in argvs)
    # As root (no sudo), DEBIAN_FRONTEND=noninteractive is passed directly via env_overrides.
    apt_cmds = [c for c in env.commands if "apt-get" in c["argv"]]
    assert apt_cmds
    assert all(c["env"] == {"DEBIAN_FRONTEND": "noninteractive"} for c in apt_cmds)
    assert env.files[vk_setup.UDEV_DEST] == (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes()


def test_flash_apply_is_idempotent_when_already_configured():
    udev_bytes = (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes()
    env = FakeSystemEnv(
        packages={"python3-serial", "stm32flash", "openocd"},
        user_groups={"dialout", "plugdev"},
        files={vk_setup.UDEV_DEST: udev_bytes},
    )
    rc = vk_setup.main(["--role", "flash"], env=env)
    assert rc == 0
    assert env.commands == []
    assert env.written_files == []


def test_flash_check_pass_and_fail_diagnostics():
    udev_bytes = (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes()
    good_env = FakeSystemEnv(
        packages={"python3-serial", "stm32flash", "openocd"},
        user_groups={"dialout", "plugdev"},
        files={vk_setup.UDEV_DEST: udev_bytes},
    )
    assert vk_setup.main(["--role", "flash", "--check"], env=good_env) == 0
    assert "[FAIL]" not in good_env.output

    bad_env = FakeSystemEnv(
        packages={"python3-serial", "brltty"},
        user_groups=set(),
        files={vk_setup.UDEV_DEST: b"# stale rules\n"},
        active_services={("ModemManager.service", False)},
    )
    assert vk_setup.main(["--role", "flash", "--check"], env=bad_env) == 1
    assert "[FAIL] pkg:stm32flash:" in bad_env.output
    assert "[FAIL] pkg:openocd:" in bad_env.output
    assert "[FAIL] group:dialout:" in bad_env.output
    assert "[FAIL] group:plugdev:" in bad_env.output
    assert "[FAIL] udev:99-vehkernel.rules:" in bad_env.output
    assert "[FAIL] service:ModemManager:" in bad_env.output
    assert "[FAIL] pkg:brltty-absent:" in bad_env.output


# ---------------------------------------------------------------------------
# Role: companion
# ---------------------------------------------------------------------------


def test_companion_apply_sets_up_ros2_repo_build_essential_and_rosdep():
    env = FakeSystemEnv(arch="aarch64")
    rc = vk_setup.main(["--role", "companion", "--yes"], env=env)
    assert rc == 0

    argvs = env.command_argvs()
    # Serial base packages WITHOUT openocd
    assert [
        "sudo",
        "env",
        "DEBIAN_FRONTEND=noninteractive",
        "apt-get",
        "install",
        "-y",
        "python3-serial",
        "stm32flash",
    ] in argvs
    assert not any("openocd" in cmd for cmd in argvs)
    # ROS2 Jazzy apt repo setup for arm64 / noble
    assert [
        "sudo",
        "env",
        "DEBIAN_FRONTEND=noninteractive",
        "add-apt-repository",
        "-y",
        "universe",
    ] in argvs
    assert [
        "sudo",
        "curl",
        "-sSL",
        vk_setup.ROS_KEY_URL,
        "-o",
        str(vk_setup.ROS_KEYRING_PATH),
    ] in argvs
    assert vk_setup.ROS_LIST_PATH in env.files
    ros_list = env.files[vk_setup.ROS_LIST_PATH].decode("utf-8")
    assert "arch=arm64" in ros_list
    assert "noble main" in ros_list
    # Companion ROS packages including build-essential
    assert [
        "sudo",
        "env",
        "DEBIAN_FRONTEND=noninteractive",
        "apt-get",
        "install",
        "-y",
        "build-essential",
        "ros-jazzy-ros-base",
        "python3-colcon-common-extensions",
        "python3-rosdep",
        "git",
    ] in argvs
    # rosdep init uses sudo; rosdep update does NOT use sudo
    assert ["sudo", "rosdep", "init"] in argvs
    assert ["rosdep", "update"] in argvs
    assert ["sudo", "rosdep", "update"] not in argvs


def test_companion_apply_skips_ros2_repo_and_rosdep_init_when_present():
    udev_bytes = (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes()
    env = FakeSystemEnv(
        packages={
            "python3-serial",
            "stm32flash",
            "build-essential",
            "ros-jazzy-ros-base",
            "python3-colcon-common-extensions",
            "python3-rosdep",
            "git",
        },
        user_groups={"dialout"},
        files={
            vk_setup.UDEV_DEST: udev_bytes,
            vk_setup.ROS_SOURCES_PATH: b"Types: deb\n",
            vk_setup.ROSDEP_DEFAULT_LIST: b"yaml ...\n",
        },
    )
    rc = vk_setup.main(["--role", "companion"], env=env)
    assert rc == 0
    argvs = env.command_argvs()
    assert not any("curl" in cmd for cmd in argvs)
    assert not any("apt-get" in cmd for cmd in argvs)
    assert ["sudo", "rosdep", "init"] not in argvs
    assert argvs == [["rosdep", "update"]]


def test_companion_build_bridge_runs_rosdep_install_and_colcon():
    repo = Path("/work/VehKernel")
    ws = Path("/tmp/vk_ws")
    env = FakeSystemEnv(
        euid=0,
        user="root",
        packages={
            "python3-serial",
            "stm32flash",
            "build-essential",
            "ros-jazzy-ros-base",
            "python3-colcon-common-extensions",
            "python3-rosdep",
            "git",
        },
        files={
            vk_setup.UDEV_DEST: (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes(),
            vk_setup.ROS_LIST_PATH: b"deb ...\n",
            vk_setup.ROSDEP_DEFAULT_LIST: b"yaml ...\n",
            repo / "bridge" / "vehkernel_bridge" / "package.xml": b"<package/>",
        },
        systemd_active=False,
    )
    rc = vk_setup.main(
        ["--role", "companion", "--yes", "--workspace", str(ws), "--repo-dir", str(repo), "--build-bridge"],
        env=env,
    )
    assert rc == 0
    assert ws in env.created_dirs

    rosdep_cmd = [
        "rosdep",
        "install",
        "--from-paths",
        str(repo / "bridge"),
        "--ignore-src",
        "--rosdistro",
        "jazzy",
        "-y",
    ]
    assert any(c["argv"] == rosdep_cmd and c["cwd"] == ws for c in env.commands)

    colcon_cmds = [c for c in env.commands if c["argv"][:2] == ["bash", "-c"] and "colcon build" in c["argv"][2]]
    assert len(colcon_cmds) == 1
    assert colcon_cmds[0]["cwd"] == ws
    assert f"--base-paths {repo / 'bridge'}" in colcon_cmds[0]["argv"][2]
    assert "--packages-up-to vehkernel_bridge" in colcon_cmds[0]["argv"][2]


def test_companion_install_bridge_service_and_preserves_existing_bridge_env():
    udev_bytes = (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes()
    base_packages = {
        "python3-serial",
        "stm32flash",
        "build-essential",
        "ros-jazzy-ros-base",
        "python3-colcon-common-extensions",
        "python3-rosdep",
        "git",
    }
    env = FakeSystemEnv(
        packages=base_packages,
        user_groups={"dialout"},
        files={
            vk_setup.UDEV_DEST: udev_bytes,
            vk_setup.ROS_LIST_PATH: b"deb ...\n",
            vk_setup.ROSDEP_DEFAULT_LIST: b"yaml ...\n",
        },
    )
    rc = vk_setup.main(
        [
            "--role",
            "companion",
            "--install-bridge-service",
            "--workspace",
            "~/custom_ws",
            "--device",
            "/dev/vehkernel-link",
            "--baud",
            "460800",
            "--ros-domain-id",
            "42",
        ],
        env=env,
    )
    assert rc == 0

    unit_dest = env.home / ".config" / "systemd" / "user" / "vehkernel-bridge.service"
    env_dest = env.home / ".config" / "vehkernel" / "bridge.env"
    assert env.files[unit_dest] == (SETUP_DIR / "systemd" / "vehkernel-bridge.service").read_bytes()
    bridge_env_text = env.files[env_dest].decode("utf-8")
    assert "VK_BRIDGE_WORKSPACE=/home/pilot/custom_ws\n" in bridge_env_text
    assert "VK_BRIDGE_DEVICE=/dev/vehkernel-link\n" in bridge_env_text
    assert "VK_BRIDGE_BAUD=460800\n" in bridge_env_text
    assert "ROS_DOMAIN_ID=42\n" in bridge_env_text

    argvs = env.command_argvs()
    assert ["systemctl", "--user", "daemon-reload"] in argvs
    assert ["systemctl", "--user", "enable", "vehkernel-bridge.service"] in argvs
    assert ["sudo", "loginctl", "enable-linger", "pilot"] in argvs

    # User customizes bridge.env; re-running MUST preserve their edits.
    custom_env = b"# user edit\nVK_BRIDGE_DEVICE=/dev/my-port\n"
    env.files[env_dest] = custom_env
    env.written_files.clear()
    rc2 = vk_setup.main(
        ["--role", "companion", "--install-bridge-service", "--device", "/dev/ttyACM9"],
        env=env,
    )
    assert rc2 == 0
    assert env.files[env_dest] == custom_env
    assert not any(path == env_dest for path, _, _ in env.written_files)
    assert "preserving existing" in env.output


def test_companion_check_verifies_packages_tools_rosdep_and_service():
    udev_bytes = (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes()
    unit_bytes = (SETUP_DIR / "systemd" / "vehkernel-bridge.service").read_bytes()
    unit_dest = Path("/home/pilot/.config/systemd/user/vehkernel-bridge.service")
    env_dest = Path("/home/pilot/.config/vehkernel/bridge.env")

    good_env = FakeSystemEnv(
        bins={"sudo", "dpkg-query", "systemctl", "g++", "colcon", "rosdep"},
        packages={
            "python3-serial",
            "stm32flash",
            "build-essential",
            "ros-jazzy-ros-base",
            "python3-colcon-common-extensions",
            "python3-rosdep",
            "git",
        },
        user_groups={"dialout"},
        files={
            vk_setup.UDEV_DEST: udev_bytes,
            vk_setup.ROS_LIST_PATH: b"deb ...\n",
            vk_setup.ROS_SETUP_BASH: b"# ros setup\n",
            vk_setup.ROSDEP_DEFAULT_LIST: b"yaml ...\n",
            unit_dest: unit_bytes,
            env_dest: b"VK_BRIDGE_DEVICE=/dev/ttyACM0\n",
            vk_setup.LINGER_DIR / "pilot": b"",
        },
        enabled_services={("vehkernel-bridge.service", True)},
    )
    assert vk_setup.main(["--role", "companion", "--check"], env=good_env) == 0
    assert "[FAIL]" not in good_env.output

    # Missing build-essential / g++ and bridge.env fails cleanly.
    bad_env = FakeSystemEnv(
        bins={"sudo", "dpkg-query", "systemctl", "colcon", "rosdep"},
        packages={"python3-serial", "stm32flash", "ros-jazzy-ros-base"},
        user_groups={"dialout"},
        files={
            vk_setup.UDEV_DEST: udev_bytes,
            unit_dest: unit_bytes,
        },
    )
    assert vk_setup.main(["--role", "companion", "--check"], env=bad_env) == 1
    assert "[FAIL] pkg:build-essential:" in bad_env.output
    assert "[FAIL] cmd:g++:" in bad_env.output
    assert "[FAIL] ros2:setup.bash:" in bad_env.output
    assert "[FAIL] ros2:rosdep-init:" in bad_env.output
    assert "[FAIL] companion:bridge-env:" in bad_env.output
    assert "[FAIL] companion:bridge-service-enabled:" in bad_env.output


# ---------------------------------------------------------------------------
# Role: desktop
# ---------------------------------------------------------------------------


def test_desktop_apply_installs_rviz_and_teleop():
    env = FakeSystemEnv(files={vk_setup.ROS_LIST_PATH: b"deb ...\n"})
    rc = vk_setup.main(["--role", "desktop", "--yes"], env=env)
    assert rc == 0
    assert [
        "sudo",
        "env",
        "DEBIAN_FRONTEND=noninteractive",
        "apt-get",
        "install",
        "-y",
        "ros-jazzy-rviz2",
        "ros-jazzy-rviz-imu-plugin",
        "ros-jazzy-teleop-twist-keyboard",
    ] in env.command_argvs()


def test_desktop_check_domain_ufw_and_clock():
    base_pkgs = {"ros-jazzy-rviz2", "ros-jazzy-rviz-imu-plugin", "ros-jazzy-teleop-twist-keyboard"}
    base_files = {
        vk_setup.ROS_LIST_PATH: b"deb ...\n",
        vk_setup.ROS_SETUP_BASH: b"# ros\n",
    }

    # Active ufw without DDS rule + unsynced clock -> warnings only (exit 0).
    warn_env = FakeSystemEnv(
        bins={"dpkg-query", "systemctl", "ufw", "timedatectl"},
        packages=base_pkgs,
        files=base_files,
        environ={"PATH": "/usr/bin", "ROS_DOMAIN_ID": "7"},
        cmd_handlers={
            ("ufw", "status"): vk_setup.CmdResult(returncode=0, stdout="Status: active\n22/tcp ALLOW\n"),
            ("timedatectl", "show", "-p", "NTPSynchronized", "--value"): vk_setup.CmdResult(
                returncode=0, stdout="no\n"
            ),
        },
    )
    assert vk_setup.main(["--role", "desktop", "--check"], env=warn_env) == 0
    assert "[PASS] desktop:ros-domain-id: ROS_DOMAIN_ID=7" in warn_env.output
    assert "[WARN] desktop:ufw:" in warn_env.output
    assert "[WARN] desktop:clock-sync:" in warn_env.output

    # Invalid ROS_DOMAIN_ID -> FAIL (exit 1).
    bad_env = FakeSystemEnv(
        bins={"dpkg-query", "systemctl"},
        packages=base_pkgs,
        files=base_files,
        environ={"PATH": "/usr/bin", "ROS_DOMAIN_ID": "not-an-int"},
    )
    assert vk_setup.main(["--role", "desktop", "--check"], env=bad_env) == 1
    assert "[FAIL] desktop:ros-domain-id:" in bad_env.output


# ---------------------------------------------------------------------------
# Cross-cutting: --dry-run, --no-sudo / root, combined roles, bundle layout
# ---------------------------------------------------------------------------


def test_dry_run_performs_zero_mutations_across_all_roles():
    repo = Path("/work/VehKernel")
    env = FakeSystemEnv(
        files={
            repo / ".devcontainer" / "Dockerfile": b"FROM ubuntu:24.04\n",
            repo / "bridge" / "vehkernel_bridge" / "package.xml": b"<package/>\n",
        },
        packages={"brltty"},
        active_services={("ModemManager.service", False)},
    )
    rc = vk_setup.main(
        [
            "--role",
            "dev",
            "--role",
            "flash",
            "--role",
            "companion",
            "--role",
            "desktop",
            "--yes",
            "--build-bridge",
            "--install-bridge-service",
            "--repo-dir",
            str(repo),
            "--dry-run",
        ],
        env=env,
    )
    assert rc == 0
    assert env.commands == []
    assert env.written_files == []
    assert env.created_dirs == []
    assert "[dry-run]" in env.output
    # Shared serial packages and dialout group should not be planned twice across flash + companion.
    assert env.output.count("usermod -aG dialout pilot") == 1
    assert env.output.count("add-apt-repository -y universe") == 1


def test_no_sudo_omits_sudo_prefix_and_writes_directly():
    env = FakeSystemEnv(euid=1000, user="pilot")
    rc = vk_setup.main(["--role", "flash", "--no-sudo", "--yes"], env=env)
    assert rc == 0
    assert not any(cmd[0] == "sudo" for cmd in env.command_argvs())
    assert env.files[vk_setup.UDEV_DEST] == (SETUP_DIR / "udev" / "99-vehkernel.rules").read_bytes()


def test_templates_resolve_relative_to_unpacked_bundle_script_dir():
    bundle_setup_dir = Path("/opt/vehkernel-bundle/tools/setup")
    custom_rules = f'{EXPECTED_SPDX}\nSUBSYSTEM=="tty"\n'.encode("utf-8")
    env = FakeSystemEnv(
        euid=0,
        user="root",
        cwd=Path("/tmp"),
        script_dir=bundle_setup_dir,
        files={bundle_setup_dir / "udev" / "99-vehkernel.rules": custom_rules},
        systemd_active=False,
    )
    rc = vk_setup.main(["--role", "flash", "--yes"], env=env)
    assert rc == 0
    assert env.files[vk_setup.UDEV_DEST] == custom_rules
