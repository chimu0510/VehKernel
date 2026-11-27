#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure this computer's clock offset against an NTP server (ADR-0028, spec 0017).

Uses only the Python 3.10+ standard library and works both directly and over
stdin so two machines can be compared against the same server:

    tools/setup/ntp_offset.py pool.ntp.org
    ssh user@companion python3 - pool.ntp.org < tools/setup/ntp_offset.py
"""

import argparse
import socket
import struct
import sys
import time

NTP_EPOCH_DELTA = 2208988800
NTP_PACKET_SIZE = 48
NTP_DEFAULT_PORT = 123
NTP_DEFAULT_TIMEOUT = 2.0


def build_request() -> bytes:
    """Build a 48-byte NTP v3 client request packet (LI=0, VN=3, Mode=3)."""
    return b"\x1b" + 47 * b"\0"


def parse_reply(reply: bytes) -> float:
    """Extract the server transmit timestamp from an NTP reply as Unix seconds."""
    if len(reply) < NTP_PACKET_SIZE:
        raise ValueError(f"short NTP reply: expected at least {NTP_PACKET_SIZE} bytes, got {len(reply)}")
    seconds, fraction = struct.unpack("!II", reply[40:48])
    if seconds == 0 and fraction == 0:
        raise ValueError("NTP reply contains an empty transmit timestamp")
    return seconds - NTP_EPOCH_DELTA + fraction / (2**32)


def query_offset(server: str, timeout: float = NTP_DEFAULT_TIMEOUT, port: int = NTP_DEFAULT_PORT) -> float:
    """Query an NTP server over UDP and return (local_clock - server_clock) in seconds."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        t0 = time.time()
        sock.sendto(build_request(), (server, port))
        reply = sock.recv(NTP_PACKET_SIZE)
        t1 = time.time()
    server_time = parse_reply(reply)
    return (t0 + t1) / 2.0 - server_time


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ntp_offset.py",
        description="Print this computer's clock offset relative to an NTP server.",
    )
    parser.add_argument("server", help="NTP server hostname or IPv4 address (for example, pool.ntp.org)")
    parser.add_argument("--timeout", type=float, default=NTP_DEFAULT_TIMEOUT,
                        help=f"UDP response timeout in seconds (default: {NTP_DEFAULT_TIMEOUT:g})")
    parser.add_argument("--port", type=int, default=NTP_DEFAULT_PORT,
                        help=f"UDP port to query (default: {NTP_DEFAULT_PORT})")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.timeout <= 0:
        print("error: --timeout must be positive", file=sys.stderr)
        return 2
    if not (1 <= args.port <= 65535):
        print("error: --port must be between 1 and 65535", file=sys.stderr)
        return 2
    try:
        offset = query_offset(args.server, timeout=args.timeout, port=args.port)
    except TimeoutError:
        print(f"{args.server}: no answer within {args.timeout:g} s", file=sys.stderr)
        return 1
    except (OSError, ValueError) as exc:
        print(f"{args.server}: {exc}", file=sys.stderr)
        return 1
    print(f"{args.server}: this computer minus the server: {offset:+.3f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
