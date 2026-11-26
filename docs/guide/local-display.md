# Local display and local menu

A board with a display and a user key shows the vehicle's status on the display, and its local menu changes a few parameters without a development host (ADR-0022). The board's display and key are on its board page, for example [C50C V1.1](../boards/c50c-v1_1.md).

## Pages

The local display shows three pages of 21 columns by 8 lines, whose values change at most five times a second, while what the user key changes is on the panel within 50 ms:

| Page | Shows |
|---|---|
| Status | Mode, arming, the emergency stop, board verification, actuator power and the last reset cause (`RST`), skipped arming checks, the last arming refusal and disarm reason, output watchdog trips |
| Attitude | Roll, pitch and yaw in degrees, and how many samples the accelerometer correction skipped |
| Link | Each Link's handshake, rate scale, drops and lost frames |

## User key and local menu

The key has three gestures: a **click** (released, with no second press within 250 ms) moves on, a **double click** (two presses within 250 ms) confirms, and a **long press** (held 1 s, acting while it is still held) goes back. A press shows on the panel at once, as a dot at the bottom right corner. A key pressed at boot does nothing until it is released, and a key held down or stuck acts at most once.

Armed, a click switches pages, a double click only shows that the menu needs the vehicle disarmed, and a long press returns to the first page. Disarmed, a click switches pages, and a double click opens the menu, and **arming is refused with "local menu open" while it is open**. The menu lists groups of menu parameters, then the parameters of a group:

- At the groups, a click moves to the next group, wrapping around; a double click enters the group; a long press closes the menu, back to the page it was opened from.
- In a group, a click moves to the next parameter, wrapping around, and the value of each is on its line; a double click edits it; a long press goes back to the groups.
- In an edit, the screen shows the unit, the range, the current value (NOW) and a staged value (NEW), and a row of actions: `+`, `-`, `X10` (the step becomes ten menu steps), `DEF` and `SAVE` for a number, `NEXT`, `PREV`, `DEF` and `SAVE` for an enum. A click moves to the next action and a double click runs it; a long press discards the staged value and goes back. `+` and `-` step by the parameter's menu step within its range, or through an enum's values. `DEF` stages the default. Nothing is written until `SAVE`, which goes through the same parameter checks as a Link, shows `SAVED` or why it was refused for a second and returns to the group; every connected Link sees the change. A changed parameter that needs a reboot shows `REBOOT NEEDED`.
- The menu closes by itself after 30 s without a key press, discarding a staged edit, and at once if the vehicle is armed.

## Display protection

While disarmed, the display dims after `DSP_DIM_S` seconds without a key press and turns off after `DSP_OFF_S` seconds (the image is kept; `0` never does); armed, it stays on and bright, and the menu does not count as idle. `DSP_BRIGHT` sets the normal contrast. A key press on a dimmed or dark display only wakes it. Arming or disarming, a mode change, the emergency stop engaging, a fault, an arming refusal and a takeover by the remote control wake it too. Every two minutes the whole image moves by one pixel along a four-step cycle, armed or not, unless `DSP_SHIFT` is off.

## Menu parameters

The menu parameters are `SENS_BOARD_ROT`, `SERIAL1_BAUD` and `SERIAL2_BAUD` (on the board), `ATT_KP`, `ATT_KI` and `ATT_ACC_REJ`; a parameter enters the menu only with `menu = true` in its definition, plus a `menu_step` for a number; an enum steps through its values, and a bitmask never enters. Nothing in the `MOT` or `COM` groups ever does: the menu never arms, changes the mode, touches the emergency stop or moves an actuator. Changing `SERIAL1_BAUD` or `SERIAL2_BAUD` from the menu recovers a board set to a rate the host cannot use.

## In SITL

SITL simulates an SSD1306; `--display <path>` writes what it shows to a PBM image after every change (black while the display is off), which any image viewer opens, and the contrast to `<path>.contrast`:

```bash
build/sitl/app/vehkernel-sitl --duration 0 --display /tmp/vk-display.pbm
```

The [Link monitor](link-monitor.md) presses the simulated user key, then prints the menu's state, which the live view also shows:

```bash
tools/link/link_monitor.py --udp 127.0.0.1:15600 key click
tools/link/link_monitor.py --udp 127.0.0.1:15600 key double
tools/link/link_monitor.py --udp 127.0.0.1:15600 key long
tools/link/link_monitor.py --udp 127.0.0.1:15600 key hold 5
```

SITL times each press itself and releases the key when it ends, so a slow host or Link does not change how long the menu sees it held (#79).

CI reads the status page back from the image (`sitl_display`), and opens the menu, saves parameters, sees arming refused while it is open and the menu time out (`sitl_local_menu`).
