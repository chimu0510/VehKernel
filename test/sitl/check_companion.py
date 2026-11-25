#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""L3 SITL companion mode test (ADR-0005, spec 0015): COMPANION through the Link.

Starts SITL with its default ground vehicle and its UDP Link on a free port,
and drives it through the Link monitor library. In order:

- A Link requests COMPANION and arms.
- A forward body velocity setpoint at 20 Hz turns the wheels.
- Zero setpoints for longer than COM_DISARM_IDLE (set to its minimum, 1 s)
  leave the vehicle armed with the wheels stopped, where TEST would disarm; zero
  setpoints are not idle in COMPANION.
- Setpoints stopped, the vehicle stops and disarms with the setpoint timeout
  after COM_SP_TIMEOUT.
- Armed again with no setpoint at all, it disarms as idle after COM_DISARM_IDLE.

    check_companion.py build/test/app/vehkernel-sitl --msg-dir build/test/generated/schema/python
"""

from __future__ import annotations

import argparse
import math
import statistics
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_actuators import (  # noqa: E402
    DISARM_IDLE_S, REACT_US, REASON, REPLY_S, SETTLED_S, SP_TIMEOUT_S, STOPPED, Vehicle, arm, set_param,
    stopped_after)
from check_attitude import free_udp_port  # noqa: E402
from check_body_velocity import BodyVehicle, send_for  # noqa: E402
from vklink.session import MODES, ParamType, ParamValue  # noqa: E402

COMPANION = next(number for number, name in MODES.items() if name == "COMPANION")
FORWARD = 0.2  # m/s
MOVING = 1.0   # rad/s: the wheels' mean speed that counts as turning


def mean_speed(vehicle: Vehicle, from_us: int) -> float:
    reached = [e for e in vehicle.encoders if e.timestamp >= from_us]
    return sum(e.speed[0] for e in reached) / len(reached) if reached else 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sitl")
    parser.add_argument("--msg-dir", required=True)
    args = parser.parse_args()

    failures: list[str] = []
    port = free_udp_port()
    sitl = subprocess.Popen([args.sitl, "--duration", "0", "--udp", f"127.0.0.1:{port}"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print(sitl.stdout.readline(), end="")  # the link line
    vehicle = BodyVehicle(args, port)
    try:
        if not vehicle.connect():
            failures.append("no handshake with SITL")
        else:
            run(vehicle, failures)
    finally:
        vehicle.close()
        sitl.terminate()
        sitl.communicate(timeout=30)
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    if not failures:
        print("PASS: COMPANION mode")
    return 1 if failures else 0


def run(vehicle: BodyVehicle, failures: list[str]) -> None:
    if not isinstance(set_param(vehicle, "COM_DISARM_IDLE", ParamType.FLOAT, DISARM_IDLE_S), ParamValue):
        failures.append("COM_DISARM_IDLE could not be set")
    mode = vehicle.monitor.request_mode(COMPANION, REPLY_S)
    if mode is None or mode.refused:
        failures.append("mode COMPANION was not taken")
        return
    vehicle.monitor.run(SETTLED_S)
    if not arm(vehicle, failures, "in COMPANION"):
        return

    first, _ = send_for(vehicle, lambda: vehicle.send_body(FORWARD, 0.0, 0.0), 2.0)
    speed = mean_speed(vehicle, vehicle.mcu_us(first) + 1_000_000)
    if speed < MOVING:
        failures.append(f"forward body velocity: the wheels turned at {speed:.2f} rad/s on average")

    # Zeros for longer than the idle disarm: armed and stopped.
    zeros, last = send_for(vehicle, lambda: vehicle.send_body(0.0, 0.0, 0.0), DISARM_IDLE_S + 1.5)
    status = vehicle.status()
    if status is None or not status.armed:
        failures.append("zero setpoints for longer than COM_DISARM_IDLE disarmed the vehicle")
    # The speed loop holds zero speed, so judge the wheels, not exactly zero duty.
    held = [e for e in vehicle.encoders if e.timestamp >= vehicle.mcu_us(zeros) + 1_000_000]
    medians = [statistics.median(wheel) for wheel in zip(*(e.speed for e in held))] if held else [math.inf]
    if any(abs(speed) > STOPPED for speed in medians):
        failures.append("zero setpoints did not stop the wheels: median speeds "
                        + ", ".join(f"{s:.2f}" for s in medians) + " rad/s")

    # Setpoints stopped: stop and disarm after COM_SP_TIMEOUT.
    vehicle.monitor.run(SP_TIMEOUT_S + 0.5)
    problem = stopped_after(vehicle.outputs, vehicle.mcu_us(last) + int(SP_TIMEOUT_S * 1e6) + REACT_US)
    if problem:
        failures.append(f"setpoints stopped, the outputs were not zero after COM_SP_TIMEOUT: {problem}")
    status = vehicle.status()
    if status is None or status.armed or status.disarm_reason != REASON["setpoint timeout"]:
        failures.append("setpoints stopped, vehicle_status does not report a setpoint timeout disarm")

    # Armed again with no setpoint at all: the idle disarm applies as in every mode.
    if arm(vehicle, failures, "in COMPANION again"):
        vehicle.monitor.run(DISARM_IDLE_S + 0.5)
        status = vehicle.status()
        if status is None or status.armed or status.disarm_reason != REASON["idle"]:
            failures.append("armed with no setpoint, the vehicle did not disarm as idle")


if __name__ == "__main__":
    sys.exit(main())
