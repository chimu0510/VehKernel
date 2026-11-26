#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""L3 SITL local menu test (ADR-0018, ADR-0022, spec 0016): the menu on SITL's user key.

Clicks, double clicks and long presses SITL's simulated user key over the Link
and checks, against local_menu, the parameters read over the Link and the
display image:

1. A double click opens the group list, and arming is refused with "local menu
   open"; a long press closes it back to the page it was opened from.
2. SENS_BOARD_ROT edited to YAW_90 and saved reads YAW_90 over the Link,
   local_menu reports the save and a reboot required, and the value is still
   there after SITL restarts with the same parameter file.
3. ATT_KP: x10 and - stop at the minimum; a long press discards the edit;
   Default and Save restore the default.
4. 30 s without a key press close the menu and discard a staged edit; arming
   is then accepted in TEST.
5. Armed, a click switches the page, a double click only shows that the menu
   needs the vehicle disarmed, and a long press returns to the first page.
6. A value a Link sets while the menu shows it appears on the display.
7. Display protection: dimmed and turned off while disarmed (DSP_DIM_S and
   DSP_OFF_S set short), a click on a dark display only wakes it, and never
   dimmed while armed.

    check_local_menu.py build/test/app/vehkernel-sitl --msg-dir build/test/generated/schema/python
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "link"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_attitude import free_udp_port  # noqa: E402
from display_text import has_text, is_dark, read_contrast, screen_text  # noqa: E402
from vklink.monitor import LinkMonitor, UdpConnection, load_messages  # noqa: E402
from vklink.session import ParamReject, ParamType  # noqa: E402

HANDSHAKE_S = 4.0
REPLY_S = 3.0
SETTLE_S = 0.6      # past the 250 ms of a click, for local_menu and the display (5 Hz) to follow
IDLE_CLOSE_S = 30.0
BOOT_S = 3.0        # board verified and actuator power settled
# The menu's groups and their parameters in the menu's order, as SITL compiles them in: the board's
# serial rates (LINKS) are the STM32F4 platform's.
GROUPS = {
    "ATTITUDE": ["EST_PRIMARY", "ATT_KP", "ATT_KI", "ATT_ACC_REJ"],
    "COMPASS": ["EKF_MAG_TYPE", "EKF_MAG_DECL"],
    "MANUAL": ["MAN_VX_MAX", "MAN_YAW_MAX", "MAN_DEADZONE"],
    "VEHICLE": ["WHL_RADIUS", "WHL_TRACK"],
    "BOARD": ["SENS_BOARD_ROT"],
    "DISPLAY": ["DSP_BRIGHT", "DSP_DIM_S", "DSP_OFF_S", "DSP_SHIFT"],
}
GROUP_NAMES = list(GROUPS)
# Actions of an edit, in the order a click moves through them.
NUMBER_ACTIONS = ["+", "-", "x10", "default", "save"]
ENUM_ACTIONS = ["next", "prev", "default", "save"]
MODE_TEST = 1
# local_menu.state
CLOSED, LIST, EDIT, GROUPS_STATE = 0, 1, 2, 3
FULL_CONTRAST = 0xEF
DIM_S, OFF_S = 3, 6  # DSP_DIM_S and DSP_OFF_S in the protection check


class Check:
    def __init__(self):
        self.failures: list[str] = []

    def expect(self, condition: bool, what: str) -> None:
        print(f"{'ok  ' if condition else 'FAIL'} {what}")
        if not condition:
            self.failures.append(what)


class Menu:
    """SITL with its Link, display image and simulated user key."""

    def __init__(self, args, param_file: Path, image: Path):
        self.port = free_udp_port()
        self.image = image
        self.group_cursor = 0  # the group under the cursor in the group list
        self.process = subprocess.Popen(
            [args.sitl, "--duration", "0", "--udp", f"127.0.0.1:{self.port}", "--param-file", str(param_file),
             "--display", str(image)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        self.connection = UdpConnection("127.0.0.1", self.port)
        self.monitor = LinkMonitor(self.connection, load_messages(args.msg_dir))
        if not self.monitor.handshake(HANDSHAKE_S):
            self.close()
            raise RuntimeError("no handshake with SITL")
        self.monitor.run(BOOT_S)

    def close(self) -> None:
        self.connection.close()
        self.process.terminate()
        self.process.wait(timeout=10)

    def click(self, times: int = 1) -> None:
        for _ in range(times):
            self.monitor.click_key()
            self.monitor.run(SETTLE_S)

    def double(self) -> None:
        self.monitor.double_click_key()
        self.monitor.run(SETTLE_S)

    def long(self) -> None:
        self.monitor.long_press_key()
        self.monitor.run(SETTLE_S)

    def state(self):
        self.monitor.run(0.25)
        menus = self.monitor.latest("local_menu")
        return menus[0][1] if menus else None

    def lines(self, settle: float = 0.2) -> list[str]:
        """The text of the page the display shows, in its eight lines."""
        self.monitor.run(settle)
        return screen_text(self.image)

    def shows(self, text: str, settle: float = 0.2) -> bool:
        """Whether the menu screen the display shows has `text` drawn on it."""
        self.monitor.run(settle)
        return has_text(self.image, text)

    def param(self, name: str):
        reply = self.monitor.get_param(name, REPLY_S)
        return None if reply is None or isinstance(reply, ParamReject) else reply.value

    def open_menu(self) -> None:
        """A double click on a page opens the group list, with the first group under the cursor."""
        self.double()
        self.group_cursor = 0

    def enter(self, name: str) -> None:
        """From the group list, edits `name`; the first action is selected."""
        group = next(group for group, names in GROUPS.items() if name in names)
        target = GROUP_NAMES.index(group)
        self.click((target - self.group_cursor) % len(GROUP_NAMES))
        self.group_cursor = target
        self.double()
        self.click(GROUPS[group].index(name))
        self.double()

    def action(self, current: str, wanted: str, actions: list[str]) -> None:
        """Moves from the action `current` to `wanted` and runs it."""
        self.click((actions.index(wanted) - actions.index(current)) % len(actions))
        self.double()

    def arm(self, arm: bool):
        return self.monitor.request_arming(arm, REPLY_S)


def run(args, check: Check, workdir: Path) -> None:
    param_file, image = workdir / "params.bin", workdir / "display.pbm"
    menu = Menu(args, param_file, image)
    try:
        default_kp = menu.param("ATT_KP")

        print("1. a double click opens the groups, and arming is refused")
        menu.click()                     # the attitude page
        menu.open_menu()
        state = menu.state()
        check.expect(state is not None and state.state == GROUPS_STATE, "local_menu reports the group list open")
        check.expect(menu.shows("SETTINGS"), "the display shows the group list")
        check.expect(menu.shows("ATTITUDE") and menu.shows("COMPASS"), "the display lists the groups")
        reply = menu.monitor.request_mode(MODE_TEST, REPLY_S)
        check.expect(reply is not None and not reply.refused, "TEST accepted while the menu is open")
        reply = menu.arm(True)
        check.expect(reply is not None and reply.refused and reply.reason_text == "local menu open",
                     "arming refused with 'local menu open'")
        menu.long()
        state = menu.state()
        check.expect(state is not None and state.state == CLOSED, "a long press at the groups closes the menu")
        check.expect(menu.lines()[0].startswith("ATTITUDE"), "back on the page the menu was opened from")
        menu.open_menu()

        print("2. SENS_BOARD_ROT to YAW_90 from the menu")
        menu.enter("SENS_BOARD_ROT")
        state = menu.state()
        check.expect(state is not None and state.state == EDIT, "local_menu reports the edit")
        check.expect(menu.shows("NEXT") and menu.shows("PREV"), "an enum edit offers NEXT and PREV")
        menu.double()
        menu.double()                    # NONE, YAW_45, YAW_90
        check.expect(menu.shows("NEW  YAW_90"), "the edit stages YAW_90")
        check.expect(menu.param("SENS_BOARD_ROT") == 0, "nothing written before SAVE")
        menu.action("next", "save", ENUM_ACTIONS)
        check.expect(menu.shows("SAVED", settle=0.0), "the edit shows SAVED")
        check.expect(menu.param("SENS_BOARD_ROT") == 2, "the Link reads YAW_90 after SAVE")
        state = menu.state()
        check.expect(state is not None and state.last_result == 0, "local_menu reports the save")
        check.expect(state is not None and state.reboot_required, "local_menu reports a reboot required")
        menu.monitor.run(1.2)
        state = menu.state()
        check.expect(state is not None and state.state == LIST, "the edit returns to the list after about a second")
        menu.long()                      # the groups

        print("3. ATT_KP: x10 and - stop at the minimum, a long press discards, Default and Save")
        menu.enter("ATT_KP")
        menu.action("+", "x10", NUMBER_ACTIONS)
        menu.action("x10", "-", NUMBER_ACTIONS)
        menu.double()
        check.expect(menu.shows("NEW  0.0") and menu.shows("STEP X10"), "x10 and - stop at the minimum, 0.0")
        menu.long()
        state = menu.state()
        check.expect(state is not None and state.state == LIST, "a long press goes back to the list")
        check.expect(menu.param("ATT_KP") == default_kp, "a long press writes nothing")
        menu.double()
        menu.action("+", "+", NUMBER_ACTIONS)
        menu.action("+", "save", NUMBER_ACTIONS)
        bumped = menu.param("ATT_KP")
        check.expect(bumped is not None and abs(bumped - (default_kp + 0.1)) < 1e-4, "+ and SAVE write one step up")
        menu.monitor.run(1.2)
        menu.double()
        menu.action("+", "default", NUMBER_ACTIONS)
        menu.action("default", "save", NUMBER_ACTIONS)
        check.expect(menu.param("ATT_KP") == default_kp, "DEF and SAVE restore the default")
        menu.monitor.run(1.2)

        print("6. a value set over the Link appears on the display")
        menu.click()                     # ATT_KI
        reply = menu.monitor.set_param("ATT_KI", ParamType.FLOAT, 0.2, REPLY_S)
        check.expect(reply is not None and not isinstance(reply, ParamReject), "ATT_KI set over the Link")
        check.expect(menu.shows("0.20"), "the menu shows ATT_KI = 0.20")

        print("4. 30 s without a key press close the menu and discard the edit")
        menu.double()
        menu.action("+", "+", NUMBER_ACTIONS)
        menu.monitor.run(IDLE_CLOSE_S + 1.0)
        state = menu.state()
        check.expect(state is not None and state.state == CLOSED, "local_menu reports the menu closed")
        check.expect(abs(menu.param("ATT_KI") - 0.2) < 1e-4, "the staged edit was discarded")
        reply = menu.arm(True)
        check.expect(reply is not None and not reply.refused, "arming accepted once the menu is closed")

        print("5. armed, the key only switches pages")
        menu.double()
        state = menu.state()
        check.expect(state is not None and state.state == CLOSED, "a double click does not open the menu while armed")
        check.expect(menu.lines(settle=0.0)[7].startswith("MENU NEEDS DISARMED"),
                     "the display says the menu needs the vehicle disarmed")
        menu.click()
        check.expect(menu.lines()[0].startswith("LINK"), "a click switches to the next page")
        menu.long()
        check.expect(menu.lines()[0].startswith("STATUS"), "a long press returns to the first page")
        menu.arm(False)
    finally:
        menu.close()

    print("2. SENS_BOARD_ROT survives a restart")
    menu = Menu(args, param_file, image)
    try:
        check.expect(menu.param("SENS_BOARD_ROT") == 2, "SENS_BOARD_ROT is YAW_90 after the restart")
    finally:
        menu.close()

    print("7. display protection")
    menu = Menu(args, workdir / "params7.bin", image)
    try:
        for name, value in (("DSP_DIM_S", DIM_S), ("DSP_OFF_S", OFF_S)):
            reply = menu.monitor.set_param(name, ParamType.INT32, value, REPLY_S)
            check.expect(reply is not None and not isinstance(reply, ParamReject), f"{name} set to {value}")
        # The display has been idle since it started, so it dimmed at once; a mode change wakes it.
        reply = menu.monitor.request_mode(MODE_TEST, REPLY_S)
        check.expect(reply is not None and not reply.refused, "TEST accepted, which wakes the display")
        menu.monitor.run(0.6)
        check.expect(read_contrast(image) == FULL_CONTRAST and not is_dark(image),
                     "the display is at its full contrast")
        menu.monitor.run(DIM_S + 1.0)
        check.expect(0 < read_contrast(image) < FULL_CONTRAST and not is_dark(image),
                     f"dimmed after {DIM_S} s without a key press")
        menu.monitor.run(OFF_S - DIM_S + 1.0)
        check.expect(is_dark(image), f"turned off after {OFF_S} s")
        menu.click()                     # only wakes the display
        check.expect(not is_dark(image) and read_contrast(image) == FULL_CONTRAST, "a click wakes the display")
        check.expect(menu.lines()[0].startswith("STATUS"), "the click that woke it did not change the page")
        menu.click()
        check.expect(menu.lines()[0].startswith("ATTITUDE"), "the next click acts")
        reply = menu.arm(True)
        check.expect(reply is not None and not reply.refused, "armed")
        menu.monitor.run(OFF_S + 2.0)
        check.expect(read_contrast(image) == FULL_CONTRAST and not is_dark(image), "never dimmed or off while armed")
        menu.arm(False)
    finally:
        menu.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sitl", help="the vehkernel-sitl executable")
    parser.add_argument("--msg-dir", required=True, help="directory of the generated vkmsg.py")
    args = parser.parse_args()
    check = Check()
    with tempfile.TemporaryDirectory() as tmp:
        try:
            run(args, check, Path(tmp))
        except RuntimeError as error:
            check.expect(False, str(error))
    if check.failures:
        print(f"FAIL: {len(check.failures)} check(s) failed")
        return 1
    print("PASS: the local menu and display protection follow ADR-0022")
    return 0


if __name__ == "__main__":
    sys.exit(main())
