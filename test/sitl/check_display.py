#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""L3 SITL display test (ADR-0018, spec 0016): the local display's overview.

Runs SITL with --display for a few seconds and reads the image the simulated
SSD1306 last showed, with display_text.py: the overview in STANDBY and disarmed
with actuator power on (the mode and the arming state in double-width letters,
the source in control, the emergency stop, actuator power, the reset cause, the
last disarm reason, heading and speed) and its status bar (the mode tag and the
page dots marking the first page).

    check_display.py build/test/app/vehkernel-sitl
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from display_text import has_text, load_glyphs, page_index  # noqa: E402

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
        # The overview of spec 0016: the mode and the arming state in double-width letters, the lines
        # of small text below them, and the status bar's mode tag and page dots.
        wide = ["STANDBY", "DISARMED"]
        small = ["SRC NONE", "ESTOP OFF", "PWR ON", "RST POWER", "DISARM ", "HDG ", "M/S"]
        failures = [f"{text!r} not drawn double-width on the display" for text in wide
                    if not has_text(image, text, glyphs, scale=2)]
        failures += [f"{text!r} not found on the display" for text in small if not has_text(image, text, glyphs)]
        # The status bar draws the mode as STB too, in the small font.
        if not has_text(image, "STB", glyphs):
            failures.append("the status bar shows no mode tag STB")
        if has_text(image, "NO DATA", glyphs, scale=2):
            failures.append("the overview shows NO DATA: no vehicle_status reached the display")
        if page_index(image) != 0:
            failures.append(f"the status bar's page dots mark page {page_index(image)}, not the overview")
    for failure in failures:
        print(f"FAIL: {failure}")
    if not failures:
        print("PASS: the overview shows STANDBY and DISARMED, no source in control, the emergency stop off, "
              "actuator power on, and the status bar marks page 1")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
