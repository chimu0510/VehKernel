# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for tools/setup/ntp_offset.py (ADR-0028, spec 0017)."""

import importlib.util
import os
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parent.parent / "ntp_offset.py"


def load_module():
    spec = importlib.util.spec_from_file_location("ntp_offset", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_ntp_reply(unix_time: float) -> bytes:
    ntp_time = unix_time + 2208988800
    seconds = int(ntp_time)
    fraction = int((ntp_time - seconds) * (2**32))
    return b"\x1c" + 39 * b"\0" + struct.pack("!II", seconds, fraction)


def test_tool_is_executable():
    assert os.access(TOOL, os.X_OK)


def test_build_request_packet_format():
    mod = load_module()
    packet = mod.build_request()
    assert len(packet) == 48
    assert packet[0] == 0x1B
    assert packet[1:] == b"\0" * 47


def test_parse_reply_decodes_transmit_timestamp():
    mod = load_module()
    expected = 1700000000.5
    reply = make_ntp_reply(expected)
    assert mod.parse_reply(reply) == pytest.approx(expected, abs=1e-6)


@pytest.mark.parametrize("bad_reply", [b"", b"\x1c" * 47, b"\x1c" + 47 * b"\0"])
def test_parse_reply_rejects_short_or_empty_timestamp(bad_reply):
    mod = load_module()
    with pytest.raises(ValueError):
        mod.parse_reply(bad_reply)


def test_query_offset_with_mocked_socket(monkeypatch):
    mod = load_module()
    sent = {}
    server_unix = 1700000100.0
    reply = make_ntp_reply(server_unix)

    class FakeSocket:
        def __init__(self, family, kind):
            sent["family"] = family
            sent["kind"] = kind

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

        def settimeout(self, timeout):
            sent["timeout"] = timeout

        def sendto(self, data, addr):
            sent["data"] = data
            sent["addr"] = addr
            return len(data)

        def recv(self, bufsize):
            sent["bufsize"] = bufsize
            return reply

    times = iter([1700000100.240, 1700000100.260])
    monkeypatch.setattr(mod.socket, "socket", FakeSocket)
    monkeypatch.setattr(mod.time, "time", lambda: next(times))

    offset = mod.query_offset("ntp.example.invalid", timeout=3.5, port=1234)
    assert offset == pytest.approx(0.250, abs=1e-6)
    assert sent["family"] == socket.AF_INET
    assert sent["kind"] == socket.SOCK_DGRAM
    assert sent["timeout"] == 3.5
    assert sent["addr"] == ("ntp.example.invalid", 1234)
    assert sent["data"] == mod.build_request()
    assert sent["bufsize"] == 48


def test_main_formats_positive_and_negative_offsets(monkeypatch, capsys):
    mod = load_module()
    monkeypatch.setattr(mod, "query_offset", lambda server, timeout, port: -0.0124)
    rc = mod.main(["pool.ntp.org"])
    assert rc == 0
    out = capsys.readouterr()
    assert out.out == "pool.ntp.org: this computer minus the server: -0.012 s\n"
    assert out.err == ""


def test_main_reports_timeout_cleanly(monkeypatch, capsys):
    mod = load_module()

    def raise_timeout(server, timeout, port):
        raise TimeoutError("timed out")

    monkeypatch.setattr(mod, "query_offset", raise_timeout)
    rc = mod.main(["pool.ntp.org", "--timeout", "1.5"])
    assert rc == 1
    out = capsys.readouterr()
    assert out.out == ""
    assert "pool.ntp.org: no answer within 1.5 s" in out.err


def test_main_reports_dns_and_malformed_errors(monkeypatch, capsys):
    mod = load_module()

    def raise_oserror(server, timeout, port):
        raise socket.gaierror("Name or service not known")

    monkeypatch.setattr(mod, "query_offset", raise_oserror)
    rc = mod.main(["bad.invalid"])
    assert rc == 1
    assert "bad.invalid:" in capsys.readouterr().err


def test_cli_help():
    result = subprocess.run(
        [sys.executable, str(TOOL), "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0
    assert "ntp_offset.py" in result.stdout
    assert "--timeout" in result.stdout


def test_stdin_invocation_works_against_loopback_udp():
    """Verifies `python3 - <server> < tools/setup/ntp_offset.py` works end-to-end."""
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_sock.bind(("127.0.0.1", 0))
    port = server_sock.getsockname()[1]
    server_sock.settimeout(5.0)
    received = {}

    def serve_once():
        with server_sock:
            data, addr = server_sock.recvfrom(64)
            received["data"] = data
            server_sock.sendto(make_ntp_reply(1700000000.0), addr)

    thread = threading.Thread(target=serve_once, daemon=True)
    thread.start()

    script_text = TOOL.read_text(encoding="utf-8")
    result = subprocess.run(
        [sys.executable, "-", "127.0.0.1", "--port", str(port), "--timeout", "2"],
        input=script_text,
        capture_output=True,
        text=True,
        check=False,
    )
    thread.join(timeout=5.0)

    assert result.returncode == 0, result.stderr
    assert received["data"] == b"\x1b" + 47 * b"\0"
    assert result.stdout.startswith("127.0.0.1: this computer minus the server: ")
    assert result.stdout.strip().endswith(" s")
