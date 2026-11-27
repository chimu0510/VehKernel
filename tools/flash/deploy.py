#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deploy a board build to a companion computer and flash the board wired to
it, in one command, from the development host (#199, ADR-0026).

The board's host tools bundle archive is copied over ssh, unpacked into a
directory under the remote home, replacing an older copy of the same bundle,
and the bundle's own flash_serial.py runs there: update by default, which
keeps the stored parameters. Arguments after the host go to flash_serial.py.
The remote computer needs Python 3.10, pyserial and, for new-board,
stm32flash; ssh options such as the port and the key belong in ~/.ssh/config.

    tools/flash/deploy.py pi@companion                          # update over /dev/ttyACM0
    tools/flash/deploy.py pi@companion --port /dev/ttyACM1 update
    tools/flash/deploy.py pi@companion new-board                # back up, erase, write all
    tools/flash/deploy.py --archive vehkernel-c50c-v1_1-abc1234.tar.gz pi@companion

Run it on the host, not in the development container: it needs only ssh,
and the build's archive is in the checkout. By default it stops an active
user systemd unit (vehkernel-bridge.service) before flashing and restarts it
afterwards, even on flash failure, without requiring sudo; pass
--bridge-service UNIT to name another unit or --no-bridge-service to skip
systemd coordination. Stop any other program that holds the board's serial
device on the remote computer first.

Exits with flash_serial.py's exit code (0 on success, 1 when a transfer
failed), 1 when the copy failed or ssh could not connect, and 2 on a usage
or archive error.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_PRESET = "c50c-v1_1"
DEFAULT_REMOTE_DIR = "vehkernel"
DEFAULT_BRIDGE_SERVICE = "vehkernel-bridge.service"
# make_bundle.py names the archive's single top directory vehkernel-<preset>-<commit>.
# The remote command removes that directory before it unpacks, so it must be a plain name.
TOP_DIR = re.compile(r"vehkernel-[A-Za-z0-9._-]+")
# flash_serial.py's commands; without one, the flasher's options are followed by update.
FLASH_COMMANDS = {"update", "new-board", "backup", "reset"}
# What ssh exits with when it could not connect or the connection broke.
SSH_FAILED = 255


class DeployError(Exception):
    """A problem with the inputs, not a failed copy or flash."""


class Bundle:
    """A host tools bundle archive: its top directory and its manifest."""

    def __init__(self, archive: Path) -> None:
        if not archive.is_file():
            raise DeployError(f"{archive} is missing")
        try:
            with tarfile.open(archive, "r:gz") as tar:
                names = tar.getnames()
                tops = {name.split("/", 1)[0] for name in names}
                if len(tops) != 1:
                    raise DeployError(f"{archive} is not a bundle: it has {len(tops)} top directories")
                self.top_dir = tops.pop()
                member = tar.extractfile(f"{self.top_dir}/manifest.json")
                manifest = json.load(member) if member is not None else None
        except (tarfile.TarError, OSError, KeyError, ValueError) as error:
            raise DeployError(f"{archive} is not a bundle: {error}") from error
        if TOP_DIR.fullmatch(self.top_dir) is None:
            raise DeployError(f"{archive} is not a bundle: its top directory is {self.top_dir!r}")
        if not isinstance(manifest, dict):
            raise DeployError(f"{archive} is not a bundle: manifest.json is not an object")
        self.archive = archive
        self.board = manifest.get("board")
        self.commit = str(manifest.get("commit", "unknown"))
        self.preset = str(manifest.get("preset", "unknown"))
        if not self.board:
            raise DeployError(f"{archive} is the {self.preset} bundle, which carries no firmware")
        if f"{self.top_dir}/app/vehkernel.apj" not in names:
            raise DeployError(f"{archive} carries no app/vehkernel.apj")


def default_archive(preset: str) -> Path:
    """The preset's bundle archive in its build directory: the newest, if a build left more than one."""
    build = REPO / "build" / preset
    archives = sorted(build.glob(f"vehkernel-{preset}-*.tar.gz"), key=lambda path: path.stat().st_mtime)
    if not archives:
        raise DeployError(f"no bundle archive in {build}; build it first: cmake --build --preset {preset}")
    return archives[-1]


def checkout_commit() -> str | None:
    """The checkout's commit as make_bundle.py writes it, or None without git."""
    try:
        result = subprocess.run(["git", "-C", str(REPO), "describe", "--always", "--dirty"],
                                capture_output=True, text=True, check=False)
    except OSError:
        return None
    return result.stdout.strip() if result.returncode == 0 and result.stdout.strip() else None


def upload_command(ssh: str, host: str, remote_dir: str, archive_name: str) -> list[str]:
    """Copies the archive from stdin into the remote directory; a broken copy leaves no archive behind."""
    directory = shlex.quote(remote_dir)
    part = shlex.quote(f"{remote_dir}/{archive_name}.part")
    final = shlex.quote(f"{remote_dir}/{archive_name}")
    script = f"mkdir -p {directory} && cat > {part} && mv {part} {final}"
    return [ssh, host, script]


def flash_command(ssh: str, host: str, remote_dir: str, bundle: Bundle, flash_args: list[str],
                  tty: bool, bridge_service: str | None = DEFAULT_BRIDGE_SERVICE) -> list[str]:
    """Unpacks the archive in place of an older copy of the bundle and runs its flasher.

    When bridge_service is set, stops an active user systemd unit before flashing and restarts
    it afterwards (including on flash failure or signal interruption), preserving the flasher's
    exit code. A missing systemctl or an inactive/uninstalled unit is a silent no-op.
    """
    archive = shlex.quote(bundle.archive.name)
    top = shlex.quote(bundle.top_dir)
    flasher = " ".join(shlex.quote(arg) for arg in ["python3", "tools/flash/flash_serial.py", *flash_args])
    unpack = f"cd {shlex.quote(remote_dir)} && rm -rf {top} && tar -xzf {archive} && rm -f {archive} && cd {top}"
    if not bridge_service:
        script = f"{unpack} && exec {flasher}"
    else:
        unit = shlex.quote(bridge_service)
        stop_msg = shlex.quote(f"Stopping {bridge_service}...")
        restart_msg = shlex.quote(f"Restarting {bridge_service}...")
        restart_warn = shlex.quote(f"warning: failed to restart {bridge_service}")
        script = (
            f"{unpack} || exit $?; "
            '[ -n "${XDG_RUNTIME_DIR:-}" ] || XDG_RUNTIME_DIR="/run/user/$(id -u)"; '
            "export XDG_RUNTIME_DIR; "
            '[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] || '
            'DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"; '
            "export DBUS_SESSION_BUS_ADDRESS; "
            "vk_stopped_bridge=0; "
            "vk_restart_bridge() { "
            'if [ "$vk_stopped_bridge" -eq 1 ]; then '
            "vk_stopped_bridge=0; "
            f"echo {restart_msg} >&2; "
            f"systemctl --user start {unit} || echo {restart_warn} >&2; "
            "fi; "
            "}; "
            "trap vk_restart_bridge EXIT; "
            "trap 'vk_restart_bridge; exit 130' INT; "
            "trap 'vk_restart_bridge; exit 143' TERM; "
            "trap 'vk_restart_bridge; exit 129' HUP; "
            "if command -v systemctl >/dev/null 2>&1 "
            f"&& systemctl --user is-active --quiet {unit} 2>/dev/null; then "
            f"echo {stop_msg} >&2; "
            f"systemctl --user stop {unit} || exit $?; "
            "vk_stopped_bridge=1; "
            "fi; "
            f"{flasher}; "
            "vk_rc=$?; "
            "vk_restart_bridge; "
            'exit "$vk_rc"'
        )
    # A terminal makes ssh forward Ctrl-C to the flasher, and stream its output as it comes.
    return [ssh, "-t", host, script] if tty else [ssh, host, script]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Copy a board build's host tools bundle to a companion computer over ssh and flash the "
                    "board wired to it with the bundle's flash_serial.py.",
        epilog="Arguments after the host go to flash_serial.py; without a command, update is added.")
    parser.add_argument("--preset", default=DEFAULT_PRESET,
                        help=f"the board preset whose build to deploy (default {DEFAULT_PRESET})")
    parser.add_argument("--archive", type=Path,
                        help="the bundle archive to deploy (default: the preset's in build/<preset>)")
    parser.add_argument("--remote-dir", default=DEFAULT_REMOTE_DIR,
                        help=f"the directory on the remote computer, relative to its home "
                             f"(default {DEFAULT_REMOTE_DIR})")
    parser.add_argument("--ssh", default="ssh", help="ssh executable")
    parser.add_argument("--bridge-service", default=DEFAULT_BRIDGE_SERVICE, metavar="UNIT",
                        help=f"user systemd unit to stop before flashing and restart afterwards "
                             f"(default {DEFAULT_BRIDGE_SERVICE})")
    parser.add_argument("--no-bridge-service", action="store_true",
                        help="do not check, stop, or restart a user systemd bridge service")
    parser.add_argument("--dry-run", action="store_true", help="print the commands without running them")
    parser.add_argument("host", help="the companion computer, as ssh takes it (user@host or a ~/.ssh/config name)")
    parser.add_argument("flash_args", nargs=argparse.REMAINDER,
                        help="flash_serial.py's arguments (update when they name no command)")
    args = parser.parse_args(argv)
    if args.no_bridge_service:
        bridge_service: str | None = None
    else:
        bridge_service = args.bridge_service.strip()
        if not bridge_service:
            parser.error("--bridge-service must not be empty")
    flash_args = args.flash_args[1:] if args.flash_args[:1] == ["--"] else args.flash_args
    if not FLASH_COMMANDS.intersection(flash_args):
        flash_args = [*flash_args, "update"]

    try:
        bundle = Bundle(args.archive if args.archive is not None else default_archive(args.preset))
    except DeployError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"Deploying {bundle.archive.name} ({bundle.board}, commit {bundle.commit}) to "
          f"{args.host}:{args.remote_dir}/{bundle.top_dir}", flush=True)
    if args.archive is None:
        current = checkout_commit()
        if current is not None and current != bundle.commit:
            print(f"warning: the bundle is of commit {bundle.commit}, the checkout is at {current}; "
                  f"rebuild with cmake --build --preset {args.preset} to deploy the checkout", file=sys.stderr)

    upload = upload_command(args.ssh, args.host, args.remote_dir, bundle.archive.name)
    flash = flash_command(args.ssh, args.host, args.remote_dir, bundle, flash_args, sys.stdin.isatty(),
                          bridge_service=bridge_service)
    if args.dry_run:
        print(f"{shlex.join(upload)} < {shlex.quote(str(bundle.archive))}")
        print(shlex.join(flash))
        return 0

    with open(bundle.archive, "rb") as archive:
        copied = subprocess.run(upload, stdin=archive, check=False)
    if copied.returncode != 0:
        print(f"FAIL: copying {bundle.archive.name} to {args.host} failed with exit code {copied.returncode}",
              file=sys.stderr)
        return 1
    print(f"Running flash_serial.py {shlex.join(flash_args)} on {args.host}", flush=True)
    flashed = subprocess.run(flash, check=False)
    if flashed.returncode == SSH_FAILED:
        print(f"FAIL: ssh to {args.host} failed", file=sys.stderr)
        return 1
    return flashed.returncode


if __name__ == "__main__":
    sys.exit(main())
