# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the POSIX sh wrapper tools/setup/vk (ADR-0028, spec 0017)."""

import json
import os
import pty
import select
import stat
import subprocess
import sys
from pathlib import Path

import pytest

VK = Path(__file__).resolve().parent.parent / "vk"
DEFAULT_IMAGE = "ghcr.io/chimu0510/vehkernel-dev:latest"
FALLBACK_IMAGE = "vehkernel-dev:latest"

FAKE_DOCKER = f"""\
#!{sys.executable}
import json, os, sys

args = sys.argv[1:]
if len(args) >= 3 and args[0] == "image" and args[1] == "inspect":
    images = [img for img in os.environ.get("FAKE_DOCKER_IMAGES", "").split(",") if img]
    sys.exit(0 if args[2] in images else 1)

if args and args[0] == "run":
    record = {{
        "args": args,
        "cwd": os.getcwd(),
    }}
    with open(os.environ["DOCKER_RECORD"], "w", encoding="utf-8") as fh:
        json.dump(record, fh)
    sys.exit(int(os.environ.get("FAKE_DOCKER_EXIT", "0")))

sys.stderr.write(f"unexpected docker args: {{args!r}}\\n")
sys.exit(99)
"""


@pytest.fixture()
def fake_docker(tmp_path):
    bin_dir = tmp_path / "fake-bin"
    bin_dir.mkdir()
    docker = bin_dir / "docker"
    docker.write_text(FAKE_DOCKER, encoding="utf-8")
    docker.chmod(docker.stat().st_mode | stat.S_IXUSR)
    record = tmp_path / "docker-record.json"
    return {"bin_dir": bin_dir, "record": record}


def make_env(fake_docker, images=DEFAULT_IMAGE, vk_image=None):
    env = os.environ.copy()
    env["PATH"] = f"{fake_docker['bin_dir']}{os.pathsep}{env.get('PATH', '')}"
    env["DOCKER_RECORD"] = str(fake_docker["record"])
    env["FAKE_DOCKER_IMAGES"] = images
    env.pop("VK_IMAGE", None)
    if vk_image is not None:
        env["VK_IMAGE"] = vk_image
    return env


def run_vk(args, cwd, env):
    return subprocess.run(
        [str(VK), *args],
        cwd=str(cwd),
        env=env,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        check=False,
    )


def git(cwd, *args):
    env = os.environ.copy()
    env.update({
        "GIT_AUTHOR_NAME": "Test",
        "GIT_AUTHOR_EMAIL": "test@example.invalid",
        "GIT_COMMITTER_NAME": "Test",
        "GIT_COMMITTER_EMAIL": "test@example.invalid",
    })
    res = subprocess.run(["git", *args], cwd=str(cwd), env=env, capture_output=True, text=True, check=False)
    assert res.returncode == 0, f"git {args} failed: {res.stderr}"
    return res


def test_vk_is_executable():
    assert os.access(VK, os.X_OK)


def test_usage_and_help(tmp_path, fake_docker):
    env = make_env(fake_docker)
    no_args = run_vk([], tmp_path, env)
    assert no_args.returncode == 2
    assert "Usage: vk" in no_args.stderr

    help_res = run_vk(["--help"], tmp_path, env)
    assert help_res.returncode == 0
    assert "Usage: vk" in help_res.stdout
    assert not fake_docker["record"].exists()


def test_default_ghcr_image_and_non_tty_flags(tmp_path, fake_docker):
    env = make_env(fake_docker, images=DEFAULT_IMAGE)
    res = run_vk(["cmake", "--preset", "test"], tmp_path, env)
    assert res.returncode == 0, res.stderr

    args = json.loads(fake_docker["record"].read_text(encoding="utf-8"))["args"]
    assert args[0] == "run"
    assert "--rm" in args
    assert "-i" in args
    assert "-t" not in args
    assert "-it" not in args
    assert "-u" in args
    assert args[args.index("-u") + 1] == f"{os.getuid()}:{os.getgid()}"
    assert "HOME=/tmp" in args
    assert "GIT_CONFIG_COUNT=1" in args
    assert "GIT_CONFIG_KEY_0=safe.directory" in args
    assert "GIT_CONFIG_VALUE_0=*" in args
    assert "--network" in args and args[args.index("--network") + 1] == "host"
    assert f"{tmp_path}:{tmp_path}" in args
    assert "-w" in args and args[args.index("-w") + 1] == str(tmp_path)
    img_idx = args.index(DEFAULT_IMAGE)
    assert args[img_idx + 1:] == ["cmake", "--preset", "test"]


def test_falls_back_to_local_image_when_ghcr_missing(tmp_path, fake_docker):
    env = make_env(fake_docker, images=FALLBACK_IMAGE)
    res = run_vk(["ctest", "--preset", "test"], tmp_path, env)
    assert res.returncode == 0, res.stderr

    args = json.loads(fake_docker["record"].read_text(encoding="utf-8"))["args"]
    assert FALLBACK_IMAGE in args
    assert DEFAULT_IMAGE not in args


def test_errors_clearly_when_neither_image_exists(tmp_path, fake_docker):
    env = make_env(fake_docker, images="")
    res = run_vk(["ctest", "--preset", "test"], tmp_path, env)
    assert res.returncode == 1
    assert DEFAULT_IMAGE in res.stderr
    assert FALLBACK_IMAGE in res.stderr
    assert "docker pull" in res.stderr
    assert "docker build" in res.stderr
    assert not fake_docker["record"].exists()


def test_vk_image_env_overrides_defaults(tmp_path, fake_docker):
    env = make_env(fake_docker, images="", vk_image="example.invalid/custom-dev:v1")
    res = run_vk(["bash", "-c", "echo hello world"], tmp_path, env)
    assert res.returncode == 0, res.stderr

    args = json.loads(fake_docker["record"].read_text(encoding="utf-8"))["args"]
    img_idx = args.index("example.invalid/custom-dev:v1")
    assert args[img_idx + 1:] == ["bash", "-c", "echo hello world"]


def test_tty_adds_dash_t_flag(tmp_path, fake_docker):
    env = make_env(fake_docker, images=DEFAULT_IMAGE)
    master_fd, slave_fd = pty.openpty()
    try:
        proc = subprocess.Popen(
            [str(VK), "cmake", "--version"],
            cwd=str(tmp_path),
            env=env,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=subprocess.PIPE,
            close_fds=True,
        )
        os.close(slave_fd)
        slave_fd = -1
        # Drain master_fd so the child never blocks on TTY output.
        while proc.poll() is None:
            readable, _, _ = select.select([master_fd], [], [], 0.1)
            if readable:
                try:
                    os.read(master_fd, 4096)
                except OSError:
                    break
        _, stderr = proc.communicate(timeout=5)
        assert proc.returncode == 0, stderr.decode("utf-8", errors="replace")
    finally:
        if slave_fd >= 0:
            os.close(slave_fd)
        os.close(master_fd)

    args = json.loads(fake_docker["record"].read_text(encoding="utf-8"))["args"]
    assert "-t" in args
    assert "-i" in args


def test_primary_git_checkout_does_not_add_extra_ro_git_mount(tmp_path, fake_docker):
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init")
    env = make_env(fake_docker, images=DEFAULT_IMAGE)
    res = run_vk(["git", "status"], repo, env)
    assert res.returncode == 0, res.stderr

    args = json.loads(fake_docker["record"].read_text(encoding="utf-8"))["args"]
    mounts = [args[i + 1] for i, tok in enumerate(args) if tok == "-v"]
    assert mounts == [f"{repo}:{repo}"]


def test_git_worktree_mounts_common_git_dir_read_only(tmp_path, fake_docker):
    repo = tmp_path / "main repo"
    repo.mkdir()
    git(repo, "init")
    git(repo, "commit", "--allow-empty", "-m", "initial")

    wt = tmp_path / "wt branch"
    git(repo, "worktree", "add", "-b", "wt-test", str(wt))

    env = make_env(fake_docker, images=DEFAULT_IMAGE)
    res = run_vk(["ctest", "--preset", "test"], wt, env)
    assert res.returncode == 0, res.stderr

    args = json.loads(fake_docker["record"].read_text(encoding="utf-8"))["args"]
    mounts = [args[i + 1] for i, tok in enumerate(args) if tok == "-v"]
    common_git = str((repo / ".git").resolve())
    assert f"{wt}:{wt}" in mounts
    assert f"{common_git}:{common_git}:ro" in mounts
