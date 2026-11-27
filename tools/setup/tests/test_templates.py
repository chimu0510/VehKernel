# SPDX-License-Identifier: GPL-3.0-or-later
"""L0 syntax and content checks for tools/setup/ templates and wrapper (ADR-0028, spec 0017)."""

import configparser
import re
import subprocess
from pathlib import Path

SETUP_DIR = Path(__file__).resolve().parent.parent
VK = SETUP_DIR / "vk"
UDEV_RULES = SETUP_DIR / "udev" / "99-vehkernel.rules"
SERVICE_UNIT = SETUP_DIR / "systemd" / "vehkernel-bridge.service"
BRIDGE_ENV = SETUP_DIR / "systemd" / "bridge.env.example"
RUFF_TOML = SETUP_DIR / "ruff.toml"

STLINK_PIDS = ("3748", "374b", "3752", "374e", "374f", "3753", "3754")
STLINK_VCP_PIDS = ("374b", "3752", "374e", "374f", "3753", "3754")
UDEV_TOKEN_RE = re.compile(r'^[A-Za-z0-9_]+(\{[^}]+\})?\s*(==|!=|\+=|:=|=)\s*"[^"]*"$')
SPDX_TAG = "SPDX-License-Identifier"
EXPECTED_SPDX = f"# {SPDX_TAG}: GPL-3.0-or-later"


def test_vk_posix_sh_syntax():
    result = subprocess.run(["sh", "-n", str(VK)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr


def test_spdx_headers_present():
    for path in (UDEV_RULES, SERVICE_UNIT, BRIDGE_ENV, RUFF_TOML):
        first_line = path.read_text(encoding="utf-8").splitlines()[0]
        assert first_line == EXPECTED_SPDX, f"{path} missing SPDX header"

    vk_lines = VK.read_text(encoding="utf-8").splitlines()
    assert vk_lines[0] == "#!/usr/bin/env sh"
    assert vk_lines[1] == EXPECTED_SPDX


def test_udev_rules_syntax_and_devices():
    text = UDEV_RULES.read_text(encoding="utf-8")
    active_lines = [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")]
    assert active_lines, "expected active udev rules"

    for line in active_lines:
        tokens = [tok.strip() for tok in line.split(",")]
        for tok in tokens:
            assert UDEV_TOKEN_RE.match(tok), f"invalid udev rule token: {tok!r} in {line!r}"

    for pid in STLINK_PIDS:
        matches = [
            line for line in active_lines
            if 'idVendor}=="0483"' in line and f'idProduct}}=="{pid}"' in line and 'SUBSYSTEM=="usb"' in line
        ]
        assert len(matches) == 1, f"missing USB udev rule for ST-Link 0483:{pid}"
        rule = matches[0]
        assert 'MODE="0660"' in rule
        assert 'GROUP="plugdev"' in rule
        assert 'TAG+="uaccess"' in rule
        assert 'ENV{ID_MM_DEVICE_IGNORE}="1"' in rule

    for pid in STLINK_VCP_PIDS:
        matches = [
            line for line in active_lines
            if 'idVendor}=="0483"' in line and f'idProduct}}=="{pid}"' in line and 'SUBSYSTEM=="tty"' in line
        ]
        assert len(matches) == 1, f"missing VCP tty udev rule for ST-Link 0483:{pid}"
        assert 'ENV{ID_MM_DEVICE_IGNORE}="1"' in matches[0]

    ch9102_tty = [
        line for line in active_lines
        if 'idVendor}=="1a86"' in line and 'idProduct}=="55d4"' in line and 'SUBSYSTEM=="tty"' in line
    ]
    assert len(ch9102_tty) == 1
    rule = ch9102_tty[0]
    assert 'ENV{ID_MM_DEVICE_IGNORE}="1"' in rule
    assert 'MODE="0660"' in rule
    assert 'GROUP="dialout"' in rule
    assert 'TAG+="uaccess"' in rule

    for line in active_lines:
        if '1a86' in line or '55d4' in line:
            assert "SYMLINK" not in line, "active CH9102F rule must not create a colliding static symlink"

    comment_lines = [line for line in text.splitlines() if line.lstrip().startswith("#")]
    comments = "\n".join(comment_lines)
    assert "ENV{ID_PATH}" in comments
    assert "KERNELS" in comments
    assert 'SYMLINK+="vehkernel-link"' in comments
    assert 'SYMLINK+="vehkernel-download"' in comments


def test_systemd_unit_structure_and_exec():
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.read(SERVICE_UNIT, encoding="utf-8")

    assert parser.has_section("Unit")
    assert parser.has_section("Service")
    assert parser.has_section("Install")

    env_file = parser.get("Service", "EnvironmentFile")
    assert "%h/.config/vehkernel/bridge.env" in env_file

    exec_start = parser.get("Service", "ExecStart")
    assert "/opt/ros/jazzy/setup.bash" in exec_start
    assert "install/setup.bash" in exec_start
    assert "exec ros2 run vehkernel_bridge bridge" in exec_start
    assert "transport:=serial" in exec_start
    assert "serial_device:=" in exec_start
    assert "serial_baud:=" in exec_start

    assert parser.get("Service", "Restart") == "on-failure"
    assert parser.get("Service", "RestartSec") == "3"
    assert parser.get("Install", "WantedBy") == "default.target"


def test_bridge_env_example_documents_required_variables():
    text = BRIDGE_ENV.read_text(encoding="utf-8")
    for var in ("VK_BRIDGE_WORKSPACE", "VK_BRIDGE_DEVICE", "VK_BRIDGE_BAUD", "ROS_DOMAIN_ID"):
        assert var in text, f"{var} missing from bridge.env.example"


def test_ruff_toml_targets_py310():
    text = RUFF_TOML.read_text(encoding="utf-8")
    assert 'extend = "../../pyproject.toml"' in text
    assert 'target-version = "py310"' in text
