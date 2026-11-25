# SPDX-License-Identifier: GPL-3.0-or-later
"""Deploy tool tests (#199), through its command line. A fake ssh runs the
remote command with sh in a directory standing for the remote home, and the
bundle archive holds a fake flash_serial.py that records its arguments and
exits with a chosen code, so the copy, the unpacking and the flasher's run
are all checked without a network or a board."""

import io
import json
import os
import stat
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parent.parent / "deploy.py"
TOP = "vehkernel-c50c-v1_1-abc1234"

FAKE_FLASHER = """\
import json, os, sys
with open(os.environ["FLASH_RECORD"], "w") as f:
    json.dump({"args": sys.argv[1:], "cwd": os.getcwd()}, f)
sys.exit(int(os.environ.get("FLASH_EXIT", "0")))
"""

# Drops ssh's -t, the host and the remote command's words like ssh, and runs it from the remote home.
FAKE_SSH = """\
#!/bin/sh
echo "$@" >> "$SSH_LOG"
[ "$1" = "-t" ] && shift
shift
[ -n "$SSH_EXIT" ] && exit "$SSH_EXIT"
cd "$REMOTE_HOME" && exec sh -c "$*"
"""


def make_archive(path: Path, top: str = TOP, board: str | None = "c50c-v1_1", apj: bool = True,
                 extra_top: bool = False) -> Path:
    files = {
        "manifest.json": json.dumps({"board": board, "commit": "abc1234", "preset": "c50c-v1_1"}).encode(),
        "tools/flash/flash_serial.py": FAKE_FLASHER.encode(),
    }
    if apj:
        files["app/vehkernel.apj"] = b"{}\n"
    with tarfile.open(path, "w:gz") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        if extra_top:
            info = tarfile.TarInfo("stray.txt")
            tar.addfile(info, io.BytesIO(b""))
    return path


@pytest.fixture()
def remote(tmp_path, monkeypatch):
    home = tmp_path / "remote-home"
    home.mkdir()
    ssh = tmp_path / "ssh"
    ssh.write_text(FAKE_SSH)
    ssh.chmod(ssh.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("REMOTE_HOME", str(home))
    monkeypatch.setenv("SSH_LOG", str(tmp_path / "ssh.log"))
    monkeypatch.setenv("FLASH_RECORD", str(tmp_path / "flash.json"))
    monkeypatch.delenv("SSH_EXIT", raising=False)
    monkeypatch.delenv("FLASH_EXIT", raising=False)
    return {"home": home, "ssh": ssh, "record": tmp_path / "flash.json", "log": tmp_path / "ssh.log"}


def run(args, ssh: Path):
    return subprocess.run([sys.executable, str(TOOL), "--ssh", str(ssh), *args],
                          capture_output=True, text=True, stdin=subprocess.DEVNULL, check=False)


def test_update_copies_unpacks_and_runs_the_bundles_flasher(tmp_path, remote):
    archive = make_archive(tmp_path / f"{TOP}.tar.gz")
    result = run(["--archive", str(archive), "pi@companion"], remote["ssh"])
    assert result.returncode == 0, result.stderr
    record = json.loads(remote["record"].read_text())
    assert record["args"] == ["update"]
    assert Path(record["cwd"]) == remote["home"] / "vehkernel" / TOP
    remote_dir = remote["home"] / "vehkernel"
    assert sorted(p.name for p in remote_dir.iterdir()) == [TOP], "the archive is removed once unpacked"
    hosts = [line.split()[0] for line in remote["log"].read_text().splitlines()]
    assert hosts == ["pi@companion", "pi@companion"], "no -t without a terminal"


def test_flasher_arguments_pass_through_quoted(tmp_path, remote):
    archive = make_archive(tmp_path / f"{TOP}.tar.gz")
    result = run(["--archive", str(archive), "pi@companion", "--port", "/dev/serial/by-id/a b", "new-board",
                  "--no-backup"], remote["ssh"])
    assert result.returncode == 0, result.stderr
    record = json.loads(remote["record"].read_text())
    assert record["args"] == ["--port", "/dev/serial/by-id/a b", "new-board", "--no-backup"]


def test_update_follows_flasher_options_without_a_command(tmp_path, remote):
    archive = make_archive(tmp_path / f"{TOP}.tar.gz")
    result = run(["--archive", str(archive), "pi@companion", "--", "--port", "/dev/ttyACM1"], remote["ssh"])
    assert result.returncode == 0, result.stderr
    assert json.loads(remote["record"].read_text())["args"] == ["--port", "/dev/ttyACM1", "update"]


def test_an_older_copy_of_the_bundle_is_replaced(tmp_path, remote):
    stale = remote["home"] / "custom" / TOP
    stale.mkdir(parents=True)
    (stale / "left-over").write_text("old")
    archive = make_archive(tmp_path / f"{TOP}.tar.gz")
    result = run(["--archive", str(archive), "--remote-dir", "custom", "pi@companion"], remote["ssh"])
    assert result.returncode == 0, result.stderr
    assert not (stale / "left-over").exists()
    assert (stale / "manifest.json").is_file()


def test_the_flashers_exit_code_is_returned(tmp_path, remote, monkeypatch):
    monkeypatch.setenv("FLASH_EXIT", "1")
    archive = make_archive(tmp_path / f"{TOP}.tar.gz")
    assert run(["--archive", str(archive), "pi@companion"], remote["ssh"]).returncode == 1


def test_a_failed_copy_stops_before_flashing(tmp_path, remote, monkeypatch):
    monkeypatch.setenv("SSH_EXIT", "255")
    archive = make_archive(tmp_path / f"{TOP}.tar.gz")
    result = run(["--archive", str(archive), "pi@companion"], remote["ssh"])
    assert result.returncode == 1
    assert "copying" in result.stderr
    assert len(remote["log"].read_text().splitlines()) == 1
    assert not remote["record"].exists()


def test_dry_run_prints_the_commands_and_runs_nothing(tmp_path, remote):
    archive = make_archive(tmp_path / f"{TOP}.tar.gz")
    result = run(["--dry-run", "--archive", str(archive), "pi@companion", "update"], remote["ssh"])
    assert result.returncode == 0, result.stderr
    assert "tools/flash/flash_serial.py update" in result.stdout
    assert not remote["log"].exists()


@pytest.mark.parametrize("kwargs, message", [
    ({"board": None}, "carries no firmware"),
    ({"apj": False}, "no app/vehkernel.apj"),
    ({"extra_top": True}, "top directories"),
    ({"top": "other-name"}, "top directory is"),
])
def test_archives_that_are_not_board_bundles_are_refused(tmp_path, remote, kwargs, message):
    archive = make_archive(tmp_path / "bundle.tar.gz", **kwargs)
    result = run(["--archive", str(archive), "pi@companion"], remote["ssh"])
    assert result.returncode == 2
    assert message in result.stderr
    assert not remote["log"].exists()


def test_a_missing_archive_is_a_usage_error(tmp_path, remote):
    result = run(["--archive", str(tmp_path / "none.tar.gz"), "pi@companion"], remote["ssh"])
    assert result.returncode == 2
    assert "missing" in result.stderr


def test_the_tool_is_executable():
    assert os.access(TOOL, os.X_OK)
