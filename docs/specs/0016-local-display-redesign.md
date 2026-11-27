# Spec 0016: the local display redesigned, with graphics, gestures and more settings

**Status**: done. Issue: [#203](https://github.com/chimu0510/VehKernel/issues/203); tickets [#204](https://github.com/chimu0510/VehKernel/issues/204) to [#211](https://github.com/chimu0510/VehKernel/issues/211), and [#212](https://github.com/chimu0510/VehKernel/issues/212) for the run time. The hardware check in [#211](https://github.com/chimu0510/VehKernel/issues/211) confirmed it on 2026-11-27 on the C50C V1.1 through the maintainer's Raspberry Pi 5; a save refused by the parameter service could not be reached from the menu on the board, and the pixel shift was too small to see by eye. Its findings are #213, #214 and #215.

This spec is done and no longer edited; later decisions live in the ADRs and the code, which win where they differ.

## Problem Statement

The local display and the local menu (spec 0007, ADR-0022) were built when the vehicle had an attitude estimate and two Links. Since then the board has gained wheel control and local odometry, a remote control, the magnetometer, the `COMPANION` mode and the ROS2 Bridge, and it reports its firmware identity (#202). The display shows none of these:

- **Three text pages miss most of the vehicle.** Status, attitude as numbers and the Links: no speed, wheels, odometry, gamepad, magnetometer, firmware or timing, and no way to see which arming check fails before trying to arm.
- **Text only.** An attitude, a stick position or a wheel speed reads faster as a picture than as numbers.
- **The key is slow and has no "back".** A short press moves and a long press confirms, so every choice waits a second, and leaving a screen means walking to `CANCEL` or `EXIT`.
- **Eight parameters, in one flat list.** The settings a user changes on the bench, such as the estimator, the remote control's limits or the wheel geometry, are not there.
- **Nothing protects the panel.** An OLED left showing the same image marks.

## Solution

Deliver the sixteenth milestone: **a redesigned local display with a status bar, eight graphical pages, a grouped settings menu driven by click, double click and long press, eighteen menu parameters and display protection**. Afterwards:

- Every page has a status bar; the pages are overview, attitude (an artificial horizon), motion, arming checks, sensors, Links, remote control and system.
- A click moves on, a double click confirms, a long press goes back.
- While disarmed, a double click opens the settings: seven groups of parameters, each edited with staged values and saved through the parameter service.
- The commander publishes which arming checks would fail now, for the display and the Link tools.
- The display dims and turns off when left alone while disarmed, and moves its image by a pixel every two minutes.
- The safety rules of ADR-0022 stay: no arming, mode changes or emergency stop from the display, no `MOT` or `COM` parameter, settings only while disarmed.

## User Stories

1. As a vehicle operator, I want the mode, arming state, emergency stop, Links and remote control on every page, so that I never have to switch pages to know whether the vehicle can move.
2. As a vehicle operator, I want an artificial horizon, so that I see at a glance whether the attitude estimate follows the board.
3. As a vehicle operator, I want the speed, the yaw rate, the odometry and a bar per wheel for speed, target and output, so that I can tell on the bench whether each wheel follows its target.
4. As a vehicle operator, I want each arming check shown as passing, failing or skipped, so that I know why arming would be refused before I try.
5. As a vehicle operator, I want the IMU, the magnetometer and whether a calibration is applied, so that I can check the sensors without a host.
6. As a vehicle operator, I want each Link's state and which one is in control, so that I know whether the companion computer drives.
7. As a vehicle operator, I want the gamepad's sticks, triggers and buttons drawn, so that I can check the remote control before arming.
8. As a vehicle operator, I want the firmware version and commit, the uptime, the reset cause and the timing on the board, so that I know what runs and whether it is healthy.
9. As a vehicle operator, I want click to move on, double click to confirm and long press to go back, so that the one key is quick and I can always leave a screen.
10. As a vehicle operator, I want the settings grouped (attitude, compass, manual, vehicle, board, Links, display), so that I find a parameter without scrolling through all of them.
11. As a vehicle operator, I want to change the estimator, the remote control's limits and the wheel geometry on the board, so that setting up a vehicle in the field needs no laptop.
12. As a vehicle operator, I want every change staged and saved only on `SAVE`, with the reason shown when it is refused, so that a stray press never writes a parameter.
13. As a vehicle operator, I want the display to dim and turn off when left alone on the bench, and to wake on a key press or an important event, so that the panel lasts and I still see what matters.
14. As a vehicle operator, I want a key press on a dark display only to wake it, so that I never act on a screen I cannot see.
15. As a safety-minded maintainer, I want the display never to arm, change the mode, touch the emergency stop or change a `MOT` or `COM` parameter, so that one key next to the wheels cannot move the vehicle.
16. As a developer, I want every page and menu screen checked against a reference image in the unit tests and the menu driven in SITL, so that a layout change is a reviewed diff.
17. As a developer, I want the arming checks that would fail now on the Link, so that `link_monitor.py` and the Bridge can show them as well.

## Implementation Decisions

- **ADR-0022** is edited in place: graphics, the status bar, the pages, refresh, display protection, the key gestures, the menu's groups and edit screen, the whitelist and `menu_group`.
- **Graphics** in `modules/local_display`, on the SSD1306 driver's 128 × 64 image:
  - Drawing: lines, rectangles (outlined and filled), circles and bitmaps, clipped to the image.
  - Fonts: the existing 5 × 7 font and a large digit font about 10 × 16, drawn for Vehkernel.
  - Icons, 8 × 8: armed, emergency stop, Link up and down, gamepad, `COMPANION`, fault, warning, check and cross. Constant arrays, GPL-3.0-or-later, no third-party font or icon.
  - No animation, grey levels or scrolling text.
- **Status bar**, 8 pixels, on every page and in the menu: the mode as `STB`, `TST`, `MAN` or `CMP`; armed; the emergency stop; each Link up or down, the one in control in inverse video; the gamepad; a warning while an arming refusal is pending and a fault icon while a fault is recorded; page dots. Pages use the 56 pixels below it.
- **Pages**, in order:
  1. **Overview**: the mode and the arming state in double-width text (the large font has digits only); the source in control; emergency stop; actuator power; reset cause; the last arming refusal or disarm reason; heading and forward speed.
  2. **Attitude**: on the left a 56 × 56 artificial horizon (the horizon tilted by roll and shifted by pitch, a ±30° scale, a fixed vehicle symbol); on the right roll, pitch and yaw, the estimator (`EKF` or `MAH`) and its counts on the attitude topics: accelerometer rejected, magnetometer rejected and fused (no topic carries an accelerometer fused count, and adding one would change `vehicle_attitude`'s schema hash). Refreshed at 10 Hz.
  3. **Motion**: forward speed in the large font, yaw rate, north and east position; one bar pair per wheel of the frame (`FRAME_TYPE`): a filled bar for the speed with a tick at the target, and a thin bar for the output duty.
  4. **Arming checks**: one line per check with a check, a cross or `SKP`, from `arming_checks`; a click scrolls when they do not fit.
  5. **Sensors**: the IMU (identity read, sample rate measured from `imu_sample`; no topic carries IMU failures); the magnetometer (state, samples, failures, field strength); whether a magnetometer calibration is applied (`CAL_MAG_*` differ from their defaults).
  6. **Links**: per Link its port name, handshake icon and rate scale, then drops, lost and frames received; `▶` before the Link in control and `CMP` in `COMPANION`.
  7. **Remote control**: two 24 × 24 circles with the stick positions, trigger bars, the names of the buttons pressed; `NOT CONNECTED` and the time since it was lost when it is gone.
  8. **System**: firmware version and commit, `*` for a dirty build (`firmware_info`); board ID and name; uptime; reset cause and fault; the work item with the longest run or any overrun; parameter count and storage use.
- **Refresh**: at most 5 Hz, the attitude page 10 Hz; a key's effect within 50 ms; only the SSD1306 pages that changed are sent. The display starts on the overview and never changes page by itself.
- **`arming_checks` topic** from the commander, at 5 Hz: a bitmask of the checks that would fail now and one of those skipped, with bits in the order of `arming_result`'s values. It is evaluated by the same code as an arming request, without arming. The Link's topic map sends it at its rate, and `link_monitor.py` shows it. It is a topic of its own, so that `vehicle_status`, required by the Bridge, keeps its schema hash.
- **Key gestures**: click (released, no second press within 250 ms) moves on; double click (two presses within 250 ms) confirms; long press (held 1 s, acting while held) goes back. A press shows at once. While armed, a click changes page, a double click shows that the menu needs the vehicle disarmed, and a long press returns to the overview.
- **Menu**: a double click on a page, while disarmed, opens the group list. Click moves on and wraps; double click enters a group, then a parameter's edit screen; long press goes back one level and, at the groups, closes the menu back to the page it came from. `LOCAL_MENU_OPEN`, closing at arming and the 30 s timeout stay.
- **Edit screen**: name, unit, range, `NOW`, `NEW`, and actions `+`, `-`, `x10`, `DEF`, `SAVE` for a number, or `NEXT`, `PREV`, `DEF`, `SAVE` for an enumeration. Click moves the caret, double click runs the action, long press discards and goes back. `SAVE` goes through the parameter service, shows `SAVED` or the refusal for about a second and returns to the list; a `reboot_required` parameter shows `REBOOT NEEDED`, and its row in the list carries a warning icon.
- **Menu parameters**: `menu_group` in the parameter definition, required by the parameter generator with `menu = true`. Groups and order: ATTITUDE (`EST_PRIMARY`, `ATT_KP`, `ATT_KI`, `ATT_ACC_REJ`), COMPASS (`EKF_MAG_TYPE`, `EKF_MAG_DECL`), MANUAL (`MAN_VX_MAX`, `MAN_YAW_MAX`, `MAN_DEADZONE`), VEHICLE (`WHL_RADIUS`, `WHL_TRACK`), BOARD (`SENS_BOARD_ROT`), LINKS (`SERIAL1_BAUD`, `SERIAL2_BAUD`), DISPLAY (`DSP_BRIGHT`, `DSP_DIM_S`, `DSP_OFF_S`, `DSP_SHIFT`). The LINKS group exists only on boards with the STM32F4 platform; SITL's menu has six groups and sixteen parameters. New parameters: `DSP_BRIGHT` (10 to 100 %, default 100), `DSP_DIM_S` (default 60, 0 never), `DSP_OFF_S` (default 600, 0 never), `DSP_SHIFT` (default on).
- **Display protection**: while disarmed, dimmed to about 10 % contrast after `DSP_DIM_S` without a key press and turned off (SSD1306 display off, image kept) after `DSP_OFF_S`; while armed never dimmed or off. The menu does not count as idle. Every two minutes the image moves one pixel along a four-step cycle in x and y when `DSP_SHIFT` is on, armed or not; layouts keep a one-pixel margin. A key press on a dimmed or dark display only wakes it; arming or disarming, a mode change, the emergency stop engaging, a fault, an arming refusal and a takeover by the remote control wake it too, and restart the idle time.
- **SITL**: the simulated SSD1306 models contrast, display on and off and the image offset; `--display` writes black while off and records the contrast. `link_monitor.py key` takes `click`, `double`, `long` and `hold <seconds>`; `short` is removed.
- **Budget**: `local_display`'s run time on the C50C stays below 10 ms per run with no overrun, measured with the timing monitor.

## Testing Decisions

- **Seam A: drawing and images (L1).** Unit tests of the drawing functions, fonts and icons; every page and menu screen rendered from fixed inputs and compared with a reference image bit for bit. The old reference images are replaced.
- **Seam B: gestures and the menu (L1).** The gesture recognizer over simulated time: click, double click, long press, the 250 ms boundary, a held key, a key pressed at boot. The menu state machine: entering, moving, wrapping, going back, editing, `x10`, `DEF`, `SAVE` accepted and refused, armed behaviour, the timeout.
- **Seam C: display protection (L1).** Dimming, turning off, waking by key (consumed) and by each event, never while armed, the pixel cycle.
- **Seam D: `arming_checks` (L1, L3).** The commander's bits against each check's state, matching what an arming request returns; `link_monitor.py` shows it; the SITL Link check sees it at its rate.
- **Seam E: the parameter generator (L0).** `menu_group` required with `menu = true`, rejected without it, and expected output for the groups.
- **Seam F: SITL (L3).** `sitl_display` checks the new overview; `sitl_local_menu` opens the menu with the new gestures, edits and saves a parameter, sees arming refused while open and the menu time out, and sees the display dim, turn off, wake on a key press without acting, and stay on while armed.
- **Seam G: hardware (L4, manual, `ready-for-human`).** On the C50C V1.1: each page against the vehicle (turn the board, drive the wheels lifted, move the sticks), the gestures, editing and saving a parameter, dimming, turning off, waking and the pixel shift, and `local_display`'s run time from the timing monitor. Results are recorded on the ticket.
- **Prior art**: the current local display's golden images and `sitl_display` and `sitl_local_menu`; the commander's arming check tests; the parameter generator's menu cases.

## Out of Scope

- Actions from the display: calibration (#188, the magnetometer), restarting the estimators, rebooting. A later spec, with its own safety review.
- Arming, mode changes and the emergency stop from the display, resetting every parameter, any `MOT` or `COM` parameter (ADR-0022).
- Displays other than the C50C V1.1's SSD1306 on SOFT_SPI: the C50C V1.2's pins, I2C and hardware SPI displays, detecting a display.
- A battery page: the MCU measures no battery yet.
- A graphical heading tape, until the magnetometer check (#155) is done.
- Inverting the image against marking.

## Further Notes

- Tickets, in dependency order: #204 drawing, fonts, icons and the reference image tests; #205 `menu_group` and the eighteen menu parameters; #206 `arming_checks` in the commander, the topic map and `link_monitor.py`; #207 gestures, the menu and edit screen, display protection, and SITL's `key` command, after #205; #208 the status bar, overview, attitude and motion pages, after #204, #206 and #207; #209 the arming checks, sensors, Links, remote control and system pages, after the same; #210 the SITL checks and the rewritten `docs/guide/local-display.md`, after #208 and #209; #211 the hardware check. #204, #205 and #206 can run at the same time.
- Each ticket is delegated to a Claude Code session on Sonnet in its own worktree, as in spec 0015; the main agent reviews, reruns the checks and lands it.
- When the spec is done the version becomes 0.3.0 (ADR-0023).
