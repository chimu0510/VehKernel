#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""L3 SITL display test (ADR-0018, spec 0016): the local display's overview.

Runs SITL with --display for a few seconds and reads the text on the image the
simulated SSD1306 last showed: the overview and its status bar, in STANDBY and
disarmed, with actuator power on.

    check_display.py build/test/app/vehkernel-sitl
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from display_text import has_text, load_glyphs  # noqa: E402

DURATION_S = 3.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sitl", help="the vehkernel-sitl executable")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / "display.pbm"
        result = subprocess.run([args.sitl, "--duration", str(DURATION_S), "--udp", "127.0.0.1:0",
                                 "--display", str(image)],
                                capture_output=True, text=True, timeout=30, check=False)
        if result.returncode != 0:
            print(result.stdout, result.stderr, sep="\n")
            print(f"FAIL: SITL exited with {result.returncode}")
            return 1
        if not image.exists():
            print("FAIL: SITL wrote no display image")
            return 1
        glyphs = load_glyphs()
        # The overview's small text and the status bar, drawn at pixel positions; the mode and the
        # arming state in large letters are not matched here (#210 rewrites this check).
        expected = ["STB", "SRC NONE", "ESTOP OFF", "PWR ON", "RST POWER"]
        failures = [f"{text!r} not found on the display" for text in expected if not has_text(image, text, glyphs)]
    for failure in failures:
        print(f"FAIL: {failure}")
    if not failures:
        print("PASS: the overview shows STB, no source in control, the emergency stop off, actuator power on")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
