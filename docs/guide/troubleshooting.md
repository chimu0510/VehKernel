# Troubleshooting

Start every diagnosis by running the built-in environment doctor on the affected machine for the role(s) it serves ([Environment setup](setup.md)):

```bash
tools/setup/vk_setup.py --role <dev|flash|companion|desktop> --check
```

`--check` is read-only and prints `[PASS]`, `[WARN]`, and `[FAIL]` items with direct remediation commands. The sections below map specific symptoms encountered on the bench and host to their causes and fixes.

---

## Build, container, and CI

### `permission denied while trying to connect to the Docker daemon socket`

- **Cause:** Your user was recently added to the `docker` group (for example by `vk_setup.py --role dev`), and the current login session has not picked up the new group yet.
- **Fix:** Log out and log back in (or reboot), then verify `id -nG | grep -w docker` and `tools/setup/vk_setup.py --role dev --check`. Avoid relying on `newgrp docker` in long-lived terminals.

### CMake configure fails with source/binary path mismatch or `No module named 'kconfiglib'`

- **Cause:** Either `cmake` was invoked directly on the host instead of inside the development container, or an existing `build/<preset>/` directory was configured at a different mount path (for example CI's `/work` or a moved checkout).
- **Fix:** Delete the stale build directory and reconfigure inside `vk` ([Building](building.md#development-environment)):
  ```bash
  rm -rf build/<preset>
  vk cmake --preset <preset>
  ```

### `reuse_lint` fails with `The following files have no licensing information`

- **Cause:** `reuse_lint` checks every non-gitignored file in the working tree, including untracked files. Saving a flash backup (`new-board`), a `.ulg` recording, or a scratch script in the repository root triggers this failure ([Testing](testing.md#host-tests-and-sitl-tests)).
- **Fix:** Move dumps, logs, and scratch files out of the repository root into `/tmp/`, `~/` (for flash backups), or the git-ignored `vendor/` or `build/` directories. For new first-party source files that belong in the repository, add an `SPDX-License-Identifier` header (`GPL-3.0-or-later`) or add an appropriate annotation to `REUSE.toml`.

### File saved under host `/tmp/` is missing inside `vk`

- **Cause:** `tools/setup/vk` mounts only the current workspace (`-v "$PWD:$PWD"`) and, for worktrees, the common `.git` directory. Inside the container, `/tmp` is the container's own ephemeral filesystem (`HOME=/tmp`)—it is not shared with host `/tmp/`.
- **Fix:** Store files that both host tools (serial recording, flashing) and container tools (`vehkernel-replay`, `vk` SITL `--param-file`) must access under git-ignored directories inside the checkout, such as `vendor/` or `build/` (for example `mkdir -p vendor` and `vendor/drive.ulg` or `build/vk-params.bin`).

### `ghcr.io/chimu0510/vehkernel-dev:latest` cannot be pulled, or local `docker build` fails against `snapshot.ubuntu.com`

- **Cause:** Network proxy/registry restrictions block GHCR, or `snapshot.ubuntu.com` is temporarily throttled/unreachable during a local image build.
- **Fix:**
  - If GHCR is blocked or you edited `.devcontainer/Dockerfile`, build the image locally as `vehkernel-dev:latest` (`vk` automatically falls back to it when the GHCR image is absent):
    ```bash
    tools/setup/vk_setup.py --role dev --build-local
    ```
  - If `snapshot.ubuntu.com` returns transient HTTP errors during `--build-local`, retry or pull the pre-built multi-arch image from GHCR when network access permits (`docker pull ghcr.io/chimu0510/vehkernel-dev:latest`).

### Build fails with missing files under `external/` (FreeRTOS, CMSIS, ST LL, TinyUSB)

- **Cause:** The repository was cloned without `--recurse-submodules`, or submodules were not updated after pulling a commit that bumped them.
- **Fix:** Initialize and update submodules from the checkout root:
  ```bash
  git submodule update --init --recursive
  ```

---

## Serial, flashing, and hardware

### `PermissionError: [Errno 13] Permission denied: '/dev/ttyACM0'`

- **Cause:** Your user is not in the `dialout` group (or has not re-logged in after being added), or `/etc/udev/rules.d/99-vehkernel.rules` is not installed/reloaded.
- **Fix:** Run `tools/setup/vk_setup.py --role flash` (or `--role companion`), log out and log back in, and verify with `--check`.

### Board resets unexpectedly when plugged in, or enters the STM32 system bootloader instead of booting firmware

- **Cause:** `ModemManager` or `brltty` probes new `/dev/ttyACM*` devices and toggles DTR/RTS. On C50C V1.1 `SERIAL2` (the download port), RTS drives `BOOT0` and DTR+RTS drives `NRST`, resetting the MCU or holding it in the system bootloader ([C50C V1.1 serial ports](../boards/c50c-v1_1.md#serial-ports)).
- **Fix:** Install `99-vehkernel.rules` (which sets `ENV{ID_MM_DEVICE_IGNORE}="1"` for WCH CH9102F `1a86:55d4`), disable `ModemManager`, and purge `brltty`:
  ```bash
  tools/setup/vk_setup.py --role flash
  tools/setup/vk_setup.py --role flash --check
  ```
  Note also that physically plugging a USB cable into `SERIAL2` pulses the download circuit once (reporting `reset_cause=PIN`); this is expected hardware behavior.

### `/dev/ttyACM0` and `/dev/ttyACM1` swap across reboots, and `/dev/serial/by-id/` shows only one device

- **Cause:** Both CH9102F USB-serial adapters on C50C V1.1 (`SERIAL1` and `SERIAL2`) report identical USB descriptors and collide on one `/dev/serial/by-id/usb-WCH.CN_USB_Single_Serial_0002-if00` symlink.
- **Fix:** Identify each port once with `link_monitor.py --check` (`mcu_link instance=0` is `SERIAL1`, `instance=1` is `SERIAL2`), then reference them via `/dev/serial/by-path/` or pin `/dev/vehkernel-link` and `/dev/vehkernel-download` by physical USB port using the commented `ENV{ID_PATH}` / `KERNELS` template in [`tools/setup/udev/99-vehkernel.rules`](../../tools/setup/udev/99-vehkernel.rules), followed by `sudo udevadm control --reload-rules && sudo udevadm trigger`.

### OpenOCD fails with `LIBUSB_ERROR_ACCESS` on ST-Link

- **Cause:** ST-Link USB permissions (`plugdev` group and `uaccess` udev rules for VID `0483`) are missing or require a new login session.
- **Fix:** Run `tools/setup/vk_setup.py --role flash`, log out and log back in, replug the ST-Link probe, and verify with `tools/setup/vk_setup.py --role flash --check`. Remember to run `openocd` and `tools/bringup/run_bringup.py` on the **host**, never inside `vk`.

### `param_status` shows high `bytes_used`, `unsaved > 0`, or skipped entries

- **Cause:** Non-default parameters are appended to an active 16 KB Flash sector (`S1` or `S2`). While armed, the firmware compacts into the pre-erased spare sector if needed, but **never erases a sector while armed** ([ADR-0020](../adr/0020-internal-flash-layout-and-write-policy.md)). If both sectors fill while armed, further writes remain in RAM (`unsaved > 0`) until disarm. Skipped entries occur if a stored parameter was removed, changed type, fell out of range, or was interrupted by power loss mid-write.
- **Fix:** Disarm the vehicle so `param_storage` on `wq:slow` can erase the old sector and flush any `unsaved` entries, then confirm `unsaved=0` in `link_monitor.py --check`. Changes to `reboot_required` parameters show `pending_reboot=1` until you run `link_monitor.py reboot`.

### `deploy.py ... new-board` fails or risks losing the vendor flash backup

- **Cause:** `flash_serial.py new-board` writes `<board>_flash_<time>.bin` into its current working directory when `--backup` is omitted. Under `deploy.py`, that working directory is the unpacked bundle (`~/vehkernel/vehkernel-<preset>-<commit>/`), which `deploy.py` deletes and replaces on the next deploy of the same commit; also, `new-board` refuses to overwrite an existing backup file (exit code 2). Passing an unquoted `~/...` to `deploy.py` expands to the development host's home directory before SSH runs (which fails if local and remote usernames differ), and `flash_serial.py` does not expand a quoted literal `~`.
- **Fix:** Pass a remote-safe `--backup` path outside the unpacked bundle directory, such as `../c50c_v1_1_flash.bin` (writes to `~/vehkernel/c50c_v1_1_flash.bin`, surviving bundle replacement) or an explicit remote path `/home/<user>/c50c_v1_1_flash.bin`:
  ```bash
  tools/flash/deploy.py user@companion --port /dev/vehkernel-download new-board --backup ../c50c_v1_1_flash.bin
  ```

---

## Companion computer, `systemd`, and ROS2 Bridge

### `colcon build` fails on a fresh Ubuntu 24.04 companion computer (`CMAKE_CXX_COMPILER not set` / missing `g++`)

- **Cause:** Minimal Ubuntu 24.04 Server and Raspberry Pi images do not ship with `build-essential` (`g++`, `make`), which `colcon build` requires for C++ packages (`vehkernel_bridge`).
- **Fix:** Run `tools/setup/vk_setup.py --role companion --yes` (which installs `build-essential`, ROS2 Jazzy, `colcon`, and `rosdep`), or install manually with `sudo apt install build-essential`.

### `systemctl --user` fails with `Failed to connect to bus` over SSH, or Bridge does not start at boot

- **Cause:** User `systemd` sessions require lingering (`loginctl enable-linger`) and `dbus-user-session`, and non-interactive SSH shells may not export `XDG_RUNTIME_DIR` or `DBUS_SESSION_BUS_ADDRESS`.
- **Fix:**
  - Enable lingering and ensure `dbus-user-session` is installed:
    ```bash
    sudo apt install dbus-user-session
    sudo loginctl enable-linger "$USER"
    ```
  - When invoking `systemctl --user` over a non-interactive SSH command, export the runtime environment first:
    ```bash
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
    export DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"
    systemctl --user status vehkernel-bridge.service
    ```
  (`tools/flash/deploy.py` sets these variables automatically when pausing and restarting `vehkernel-bridge.service`.)

### Desktop `ros2 topic list` is empty or does not see `/odom` and `/vehkernel/vehicle_status`

- **Cause:** One of three network/environment mismatches between the companion computer and desktop machine ([Viewing in RViz](ros2-bridge.md#viewing-in-rviz)):
  1. **`ROS_DOMAIN_ID` mismatch:** Interactive shells read `~/.bashrc`, whereas `vehkernel-bridge.service` reads `ROS_DOMAIN_ID` from `~/.config/vehkernel/bridge.env` (unset defaults to `0`).
  2. **Firewall (`ufw`) blocking DDS UDP discovery:** Active `ufw` on either machine drops ROS2 DDS UDP traffic (`7400:7500/udp`).
  3. **Subnet / Wi-Fi multicast isolation:** Both machines are on different subnets or the Wi-Fi AP blocks UDP multicast.
- **Fix:**
  - Compare `echo ${ROS_DOMAIN_ID:-0}` on the desktop with `ROS_DOMAIN_ID` in `~/.config/vehkernel/bridge.env` on the companion computer; restart the service after edits (`systemctl --user restart vehkernel-bridge.service`).
  - If `sudo ufw status` is active on either host, allow DDS UDP ports:
    ```bash
    sudo ufw allow 7400:7500/udp
    ```
  - Verify `tools/setup/vk_setup.py --role desktop --check` reports no `[FAIL]` items.

### RViz2 shows `TF` errors or drops `/odom` and `/imu/data` messages

- **Cause:** Clock skew between the companion computer and the desktop machine. The Bridge stamps ROS2 messages using the companion computer's wall clock synchronized to MCU timestamps; Raspberry Pi 5 boards without an RTC battery boot with stale clocks until NTP syncs ([Clock synchronization](companion-computer.md#clock-synchronization)).
- **Fix:** Measure both machines against the same NTP server using [`tools/setup/ntp_offset.py`](../../tools/setup/ntp_offset.py):
  ```bash
  tools/setup/ntp_offset.py pool.ntp.org
  ssh user@companion python3 - pool.ntp.org < tools/setup/ntp_offset.py
  ```
  If the two offsets differ by more than a few tens of milliseconds, configure `systemd-timesyncd` on both machines against the same reachable NTP server and verify `timedatectl` reports `System clock synchronized: yes`.

### Bridge logs schema hash mismatch—or refuses to connect

- **Cause:** The MCU firmware and the ROS2 Bridge were built from different schema commits ([ADR-0006](../adr/0006-schema-and-wire-encoding.md)).
  - If a non-required diagnostic topic's hash differs, the Bridge disables only that topic, logs its name, and stays connected.
  - If a topic marked `@required` (`vehicle_status`, `body_velocity_setpoint`, or `vehicle_local_odometry`) differs, the Bridge refuses the connection to protect the control path.
- **Fix:** Align both ends to the same git commit: rebuild the Bridge on the companion computer (`colcon build --base-paths src/VehKernel/bridge --packages-up-to vehkernel_bridge`) or deploy the matching firmware bundle with `tools/flash/deploy.py`.

### Serial contention: `link_monitor.py` gets garbage/CRC errors or Bridge keeps reconnecting

- **Cause:** Two processes (for example `vehkernel-bridge.service` and a manual `link_monitor.py` or `flash_serial.py` session) opened the same serial device simultaneously.
- **Fix:**
  - For firmware updates, use `tools/flash/deploy.py`, which automatically stops `vehkernel-bridge.service` before flashing and restarts it afterwards.
  - Before running `link_monitor.py` or `flash_serial.py` manually on the companion computer, stop the Bridge first and restart it when finished:
    ```bash
    systemctl --user stop vehkernel-bridge.service
    # run link_monitor.py or flash_serial.py ...
    systemctl --user start vehkernel-bridge.service
    ```
  - On C50C V1.1, you can also keep the Bridge on `SERIAL1` (`/dev/vehkernel-link`) while connecting `link_monitor.py` to `SERIAL2` (`/dev/vehkernel-download`), as both ports run independent Links.
