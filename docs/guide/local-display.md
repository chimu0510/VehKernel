# Local display and local menu

A board with a display and a user key shows the vehicle's status on the display, and its local menu changes a few parameters without a development host (ADR-0022, spec 0016). The board's display and key are on its board page, for example [C50C V1.1](../boards/c50c-v1_1.md). The display is a 128 × 64 monochrome OLED (SSD1306), and the user key is the board's only input.

The display only reads: it never arms the vehicle, changes the mode, touches the emergency stop or moves an actuator ([What the display never does](#what-the-display-never-does)).

## Status bar

The top 8 pixels of every page and of every menu screen show what you need before the vehicle moves, whichever page is open:

| Item | Meaning |
|---|---|
| `STB`, `TST`, `MAN`, `CMP` | The mode: `STANDBY`, `TEST`, `MANUAL` or `COMPANION`; `---` before the first status arrives |
| Armed icon | The vehicle is armed |
| Emergency stop icon | The emergency stop is engaged |
| Link icons | One per Link: an icon for a Link whose handshake is up, a broken one for a Link that is down. The Link that is in control is drawn in inverse video |
| Gamepad icon | A remote control is connected (inverse video while it is in control) |
| Warning icon | The last arming request was refused; the reason is on the overview and the arming checks page |
| Fault icon | A fault is raised; its name is on the system page |
| Page dots | Eight dots, one per page; the current page's dot is a filled square |

The status bar is the only warning. A fault or a refusal never changes the page by itself, so the image does not jump while someone drives.

## Pages

The display starts on the overview. A **click** moves to the next page, wrapping from the system page back to the overview, and a **long press** returns to the overview. Values change at most five times a second (the attitude page ten), while what the key does is on the panel within 50 ms.

| # | Page | Shows |
|---|---|---|
| 1 | Overview | The mode and the arming state in large letters; `SRC` the source in control (`NONE`, `RC` or `LINKn`); `ESTOP` `ON` or `OFF`; `PWR` the actuator power state; `RST` the last reset cause; `REFUSED <reason>` for the last arming refusal, or `DISARM <reason>` for the last disarm reason; `HDG` the heading in degrees from north and `V` the forward speed in m/s (`---` without an estimate or odometry) |
| 2 | Attitude | A 56 × 56 artificial horizon, tilted by the roll and shifted by the pitch, with a ±30° scale on the right and a fixed vehicle symbol; the roll, pitch and yaw in degrees; the estimator (`EKF` or `MAH`) and its counts: `ACC` samples rejected by the accelerometer correction, `MAG` rejected and fused magnetometer samples (`---` where no topic carries a count) |
| 3 | Motion | The forward speed in large digits (m/s), the yaw rate (degrees per second) and the north and east position (m) from the local odometry; one bar pair per wheel of the frame: a filled bar for the measured speed with a tick at the target, and a thin bar for the output duty |
| 4 | Arming checks | One line per arming check: a check mark if it passes now, a cross if it would refuse arming, `SKP` if it is skipped. It shows the result an arming request would get at this moment, without arming. Thirteen checks do not fit: a click scrolls the list, and once the last lines show, the next click moves to the next page |
| 5 | Sensors | `IMU` whether the board was verified (`ID OK` or `NO ID`) and the rate of IMU samples; `MAG` the magnetometer's state, `SMP` samples, `FAIL` failures and `LAST` the last failure; `FIELD` the field strength in µT; `CAL APPLIED` or `CAL NOT APPLIED` according to whether the `CAL_MAG_*` parameters differ from their defaults |
| 6 | Links | Per Link its port name, whether its handshake is up, its rate scale in percent (`NONE` until the Link has reported), `DROP` the frames dropped, `LOST` the frames lost and `RX` the frames received; a triangle marks the Link in control and `CMP` appears beside it in `COMPANION` |
| 7 | Remote control | Two circles with the stick positions (forward is up), two bars for the triggers `LT` and `RT`, and the names of the buttons pressed; `NOT CONNECTED` and the time since it was lost when the remote control is gone or has gone silent |
| 8 | System | `FW` the firmware version and the commit, with `*` for a dirty build; `BRD` the board ID and name; `UP` the uptime; `RST` the last reset cause, or `FAULT` and its name while a fault is raised; the work item with the most overruns or the longest run time (`OVR` and the count, or the longest run in ms); `PARAM` the parameter count and `STORE` the share of the parameter sector in use |

A page that has not received its topic yet shows `NO DATA` or `---` where the value would be.

## User key

The key has three gestures:

| Gesture | Meaning |
|---|---|
| Click | Released, with no second press within 250 ms: moves to the next page, group, parameter or action |
| Double click | Two presses within 250 ms: confirms, such as opening the menu or running an action |
| Long press | Held for 1 s, acting while the key is still held: goes back one level |

A press shows on the panel at once, as a dot at the bottom right corner, so the wait for a possible second press does not feel like lag. A key pressed at boot does nothing until it is released, and a key held down or stuck acts at most once.

While the display is dimmed or off, the first press only wakes it ([Display protection](#display-protection)).

While the vehicle is **armed**, a click still switches pages and a long press returns to the overview, but a double click only shows `MENU NEEDS DISARMED` for a moment: the menu does not open.

## Local menu

While the vehicle is disarmed, a double click on any page opens the menu, and **arming is refused with "local menu open" while it is open**, which the Link reports as the arming refusal `LOCAL_MENU_OPEN`. The menu holds settings only; the pages are the information. It has three levels: groups, the parameters of a group, and a parameter's edit screen.

- **Groups.** The title `SETTINGS` shows the group under the cursor, such as `1/7`, and each row the group's name and the number of its parameters. A click moves to the next group, wrapping around; a double click enters the group; a long press closes the menu, back to the page it was opened from.
- **Parameters.** The title is the group's name. Each row shows a parameter and its value; a warning icon marks a parameter changed since boot that needs a reboot. A click moves to the next parameter, wrapping around; a double click edits it; a long press goes back to the groups.
- **Edit screen.** See below.
- **Closing.** The menu closes by itself after 30 s without a key press, discarding a staged edit, and at once if the vehicle is armed, for example by a Link. While the menu is open the display does not count as idle, and it does not dim or turn off.

### Edit screen

The edit screen shows the name and unit, the range (`RANGE` for a number, `CHOOSE ONE` for an enumeration), the value now (`NOW`), the staged value (`NEW`, and `STEP X1` or `STEP X10` for a number) and a row of actions with the selected one filled:

| Parameter | Actions |
|---|---|
| A number | `+`, `-`, `X10`, `DEF`, `SAVE` |
| An enumeration | `NEXT`, `PREV`, `DEF`, `SAVE` |

A click moves to the next action and a double click runs it; a long press discards the staged value and goes back to the parameters. `+` and `-` step by the parameter's menu step within its range, or through an enumeration's values, and stop at the ends. `X10` switches the step between one and ten menu steps. `DEF` stages the default value. **Nothing is written until `SAVE`**, which goes through the same parameter service and the same checks as a Link, so a value out of range or a locked parameter is refused: the screen shows `SAVED` or `REFUSED:` with the reason for about a second and returns to the parameters; any key press ends it sooner. Every connected Link sees the change as a parameter update. A saved parameter that needs a reboot marks the screen with `REBOOT NEEDED`; the menu never reboots the board.

When a Link changes a parameter the menu is showing, the menu shows the new value, and the last confirmed write wins.

### Groups and parameters

The menu has seven groups and eighteen parameters. A parameter is in the menu only when its definition sets `menu = true` and names its group with `menu_group`, so a new parameter stays out until someone decides otherwise.

| Group | Parameters |
|---|---|
| ATTITUDE | `EST_PRIMARY` (the attitude estimator, `MAHONY` or `EKF`), `ATT_KP`, `ATT_KI` (the Mahony gains), `ATT_ACC_REJ` (the accelerometer correction is skipped when the specific force differs from gravity by more than this, in g) |
| COMPASS | `EKF_MAG_TYPE` (`AUTO`, `OFF` or `HEADING`), `EKF_MAG_DECL` (the magnetic declination, in degrees) |
| MANUAL | `MAN_VX_MAX` (forward speed at full stick), `MAN_YAW_MAX` (yaw rate at full stick), `MAN_DEADZONE` |
| VEHICLE | `WHL_RADIUS`, `WHL_TRACK` (the wheel geometry that odometry and kinematics use) |
| BOARD | `SENS_BOARD_ROT` (how the IMU is mounted) |
| LINKS | `SERIAL1_BAUD`, `SERIAL2_BAUD` (the serial Links' rates; changing one from the menu recovers a board set to a rate the host cannot use) |
| DISPLAY | `DSP_BRIGHT`, `DSP_DIM_S`, `DSP_OFF_S`, `DSP_SHIFT` |

`EST_PRIMARY`, `EKF_MAG_TYPE`, `WHL_RADIUS`, `WHL_TRACK`, `SENS_BOARD_ROT` and the serial rates need a reboot to take effect. SITL has no serial ports, so its menu has six groups and sixteen parameters, without LINKS.

Enumerations step through their value names; a bitmask never enters the menu.

## Display protection

An OLED that keeps showing the same image marks. Four parameters, all in the DISPLAY group, protect it:

| Parameter | Default | Meaning |
|---|---|---|
| `DSP_BRIGHT` | 100 % | The contrast while the display is awake (10 to 100 %) |
| `DSP_DIM_S` | 60 s | Seconds without a key press, while disarmed, before the display dims to about 10 % contrast; 0 never dims |
| `DSP_OFF_S` | 600 s | Seconds without a key press, while disarmed, before the display turns off; the image is kept; 0 never turns it off |
| `DSP_SHIFT` | `ON` | Moves the whole image by one pixel every two minutes along a four-step cycle |

- **Armed, the display never dims or turns off**, so the status stays readable while the vehicle moves; the pixel shift still protects it.
- **A key press on a dimmed or dark display only wakes it.** It never acts, so you never act on a screen you cannot see; the next press acts.
- **Events wake the display too**, and restart the idle time: arming or disarming, a mode change, the emergency stop engaging, a fault, an arming refusal and a takeover by the remote control.
- The menu is use, not idle time, so it does not dim.

## What the display never does

The display and the menu are one more client of the parameter service, and nothing they do can make an actuator move:

- No arming and no disarming.
- No mode change.
- No engaging or releasing the emergency stop.
- No `MOT` or `COM` parameter: the groups of the actuator outputs, the arming checks, the failsafe actions and the setpoint timeouts stay out of the menu, and the parameter generator rejects `menu = true` in them. Control gains and limits (`RATE_`, `VEL_`), `RC_TIMEOUT`, the frame and wheel outputs, the magnetometer calibration (written by calibration) and the EKF's noise and gates stay out too.
- No settings while armed: the menu opens only while disarmed, and closes at once at arming.
- No reboot, no calibration and no reset of every parameter at once.

A display that is missing or does not answer affects nothing else: it is not part of verifying the board and not an arming check. On the C50C the firmware cannot tell, because the bus is write-only.

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

`click`, `double` and `long` press the key with the gesture's timing, and `hold <seconds>` holds it down for that long. SITL times each press itself and releases the key when it ends, so a slow host or Link does not change how long the menu sees it held (#79).

CI checks both in SITL. `sitl_display` reads the overview and the status bar back from the image: the mode and the arming state in large letters, the lines of small text, the mode tag and the page dots. `sitl_local_menu` steps through the pages, opens the menu with a double click, moves through the groups, edits and saves parameters and reads them back over the Link, sees arming refused while the menu is open and the menu time out, and sees the display dim, turn off, wake on a key press without acting and stay on while armed, with `DSP_DIM_S` and `DSP_OFF_S` set short. The layout of every page and menu screen is checked against reference images in the unit tests.
