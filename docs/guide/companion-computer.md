# Companion computer

A companion computer, such as a Raspberry Pi 5 or an NVIDIA Jetson, runs vehicle applications (like ROS2) and talks to Vehkernel over a Link (ADR-0001, ADR-0002). Companion computers are usually arm64 Linux systems, and they need not build anything to use the tools: no CMake, no development container.

To monitor, configure, log, and flash the board from the companion computer, each firmware build produces an architecture-independent **host tools bundle** (ADR-0026, spec 0014).

## The host tools bundle

The host tools (the Link monitor `tools/link/link_monitor.py`, log utilities in `tools/log/`, the serial flasher `tools/flash/flash_serial.py`, and the setup utilities in `tools/setup/` including `vk_setup.py`, `ntp_offset.py`, udev rules, and user `systemd` templates) are pure Python scripts requiring only the standard library and `pyserial` ([ADR-0026](../adr/0026-host-tools-bundle.md), [ADR-0028](../adr/0028-host-environment-setup.md)). To function against a build they also need build outputs:
- Generated Python message decoders (`generated/schema/python/vkmsg.py`).
- Parameter metadata (`generated/params/params.json`), providing names, types, units, ranges, and enum values.
- The board Flash layout (`generated/board/board.json`) and firmware images (`app/vehkernel.bin`, `vehkernel.apj`, `vehkernel.elf`, `vehkernel.hex`, and `vehkernel_with_bl.hex`).

The host tools bundle gathers these files at their standard repository paths, alongside `manifest.json` recording the preset, board, git commit, protocol version, parameter set hash, and minimum Python version.

### Prerequisites and serial permissions

Bundled tools support Python 3.10 or newer (the default on Ubuntu 22.04/24.04 LTS, Debian 12 / Raspberry Pi OS, and JetPack 6). They do not use features newer than 3.10 (such as `tomllib`, `typing.Self`, or `ExceptionGroup`).

On Ubuntu 24.04 or Debian 12 / Raspberry Pi OS, install and verify the serial packages, `dialout` group membership, [`tools/setup/udev/99-vehkernel.rules`](../../tools/setup/udev/99-vehkernel.rules), and `ModemManager`/`brltty` mitigation directly from an unpacked Host Tools Bundle (no git checkout required; [Environment setup](setup.md)):

```bash
# Ubuntu 24.04 companion running the ROS2 Bridge:
tools/setup/vk_setup.py --role companion --dry-run
tools/setup/vk_setup.py --role companion
tools/setup/vk_setup.py --role companion --check

# Ubuntu 24.04 or Debian 12 / Raspberry Pi OS (host tools and flashing only):
tools/setup/vk_setup.py --role flash --dry-run
tools/setup/vk_setup.py --role flash
tools/setup/vk_setup.py --role flash --check
```

To configure manually instead:

```bash
sudo apt install python3-serial stm32flash
sudo usermod -aG dialout "$USER"
sudo cp tools/setup/udev/99-vehkernel.rules /etc/udev/rules.d/99-vehkernel.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo systemctl disable --now ModemManager.service
sudo apt purge brltty
```

Log out and log back in after adding your user to `dialout`. Disabling `ModemManager` and removing `brltty` prevents them from probing the board's CH9102F USB serial adapters and toggling DTR/RTS on C50C V1.1 `SERIAL2` (which resets the board or enters the STM32 system bootloader). (Raspberry Pi OS and recent Ubuntu refuse a system-wide `pip install`; use `python3-serial` from apt, or `pip install pyserial` inside a virtual environment.)

For one-command remote deployment from a development host with [`tools/flash/deploy.py`](../../tools/flash/deploy.py), ensure `openssh-server` is installed and running on the companion computer (`sudo apt install openssh-server`) and configure SSH key authentication (and any custom port/host alias in `~/.ssh/config` on the development host) so non-interactive SSH succeeds without password prompts.

## Getting a bundle

### From CI

CI builds the bundle for every board preset on each push and keeps it as the `bundles` artifact (ticket #173). Download the archive `vehkernel-<preset>-<commit>.tar.gz` that matches the firmware commit.

### From a development build

On a development host, building any board preset or `sitl` in the development container (`vk`) automatically creates the bundle directory and archive:

```bash
vk cmake --build --preset c50c-v1_1
```

This writes:
- The unpacked directory: `build/c50c-v1_1/bundle/`
- The compressed archive: `build/c50c-v1_1/vehkernel-c50c-v1_1-<commit>.tar.gz`

Copy the archive to the companion computer:

```bash
scp build/c50c-v1_1/vehkernel-c50c-v1_1-*.tar.gz pi@companion:/home/pi/
```

To copy the bundle and flash the board wired to the companion computer in one step, run `tools/flash/deploy.py pi@companion` on the development host instead ([Flashing](flashing.md#from-the-development-host-in-one-command)). If the companion computer runs the ROS2 Bridge via the optional [`vehkernel-bridge.service`](ros2-bridge.md#running-the-bridge-as-a-user-service) user unit, `deploy.py` automatically pauses the service before flashing and restarts it afterwards without requiring remote `sudo`.

## Unpacking and running

On the companion computer, unpack the archive:

```bash
tar -xzf vehkernel-c50c-v1_1-*.tar.gz
cd vehkernel-c50c-v1_1-*
```

When run from inside the unpacked bundle, the tools automatically locate `manifest.json` two levels above their own directories. Their defaults for `--msg-dir`, `--param-meta`, and `--build` point to the bundle itself, so no extra path options are needed.

### Monitoring and checks

Start the live monitor over the serial Link (defaulting to 921600 baud):

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0
```

Run a 5-second health check:

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 --check --duration 5 --expect-rate imu_sample:0=200
```

### Parameters

List all parameters with their descriptions, units, and values:

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 param list
```

Read or set parameters by name (enum values can be given by name):

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 param get ATT_KP
tools/link/link_monitor.py --serial /dev/ttyACM0 param set ATT_KP 2.5
tools/link/link_monitor.py --serial /dev/ttyACM0 param set SENS_BOARD_ROT YAW_90
```

### Recording logs

Record a ULog file over the Link with a selected profile ([Logging](logging.md)):

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 record drive.ulg --profile estimator_replay --duration 30
```

The recorded log can be inspected or calibrated on the companion computer using the bundled log tools (e.g. `tools/log/mag_cal.py`). Note that `replay.py` requires native binary `vehkernel-replay` and remains on the development host.

### Flashing firmware

A board bundle carries the firmware images in `app/`. To update the firmware through the Vehkernel bootloader ([Flashing](flashing.md)):

```bash
tools/flash/flash_serial.py update
```

The tool asks the board over the Link to reboot into its bootloader, then flashes `app/vehkernel.apj` via `uploader.py`. Stored parameters are preserved.

For a new or corrupted board, write the bootloader and firmware through the download port with:

```bash
tools/flash/flash_serial.py new-board
```

## Clock synchronization

The companion computer's clock and the development host's must agree before ROS2 runs across both, or logs from both are compared. The Bridge stamps every output with the MCU's sample time on the companion computer's clock ([ROS2 Bridge](ros2-bridge.md#topics-services-and-parameters)), so the MCU needs nothing; but when RViz or Nav2 runs on the development host, an offset between the two computers makes their TF and messages look stale or from the future, and RViz drops them. Keep both within a few milliseconds of the same NTP server.

A Raspberry Pi 5 has a real-time clock, but without its backup battery it starts each boot from the last time it saved, so it falls behind by as long as it was off until NTP corrects it. Nothing corrects it when the NTP service is off, or when its NTP server, often the local router, keeps the wrong time itself.

### Checking

On each computer:

```bash
timedatectl
```

`System clock synchronized: yes` and `NTP service: active` mean it is synchronized. `timedatectl timesync-status` names the server and the last offset.

### Choosing a server

Before configuring a server, check that it answers with the right time from this network using [`tools/setup/ntp_offset.py`](../../tools/setup/ntp_offset.py) (standard-library Python 3.10+, shipped in both the repository and every Host Tools Bundle, so you never need to create an untracked script in a git checkout):

```bash
tools/setup/ntp_offset.py pool.ntp.org
```

A timeout means the network blocks NTP to that server; an offset of days or weeks on a computer whose clock is right means the server keeps the wrong time. Pick a server that answers with an offset of milliseconds on a synchronized computer: your country's national time service, or `pool.ntp.org`.

### Fixing

On Ubuntu, `systemd-timesyncd` keeps the clock. Name the server in a drop-in file, which leaves `/etc/systemd/timesyncd.conf` as it is; the empty `NTP=` clears any server set there, such as the router. Then turn NTP on and restart the service; the clock jumps to the right time at once, and is corrected again at every boot. Run this on each computer, with your server in place of the examples:

```bash
sudo mkdir -p /etc/systemd/timesyncd.conf.d && printf "[Time]\nNTP=\nNTP=0.pool.ntp.org\nFallbackNTP=1.pool.ntp.org 2.pool.ntp.org\n" | sudo tee /etc/systemd/timesyncd.conf.d/50-ntp.conf && sudo timedatectl set-ntp true && sudo systemctl restart systemd-timesyncd
```

On the companion computer, over ssh, add `-t` so that sudo can ask for the password:

```bash
ssh -t pi@companion 'sudo mkdir -p /etc/systemd/timesyncd.conf.d && printf "[Time]\nNTP=\nNTP=0.pool.ntp.org\nFallbackNTP=1.pool.ntp.org 2.pool.ntp.org\n" | sudo tee /etc/systemd/timesyncd.conf.d/50-ntp.conf && sudo timedatectl set-ntp true && sudo systemctl restart systemd-timesyncd'
```

A computer that runs chrony instead of `systemd-timesyncd` names its servers in `/etc/chrony/chrony.conf`, and `chronyc tracking` shows its offset.

### Comparing the two clocks

Run `tools/setup/ntp_offset.py` with the same server on the development host and, over ssh, on the companion computer (streaming the script over stdin so the companion computer does not even need a local copy):

```bash
tools/setup/ntp_offset.py pool.ntp.org
ssh pi@companion python3 - pool.ntp.org < tools/setup/ntp_offset.py
```

The difference between the two offsets is the offset between the two computers. Each measurement is good to about half its network round trip, a few milliseconds over Wi-Fi, so two synchronized computers differ by a few milliseconds. Comparing `date` on both over ssh is not a measurement: the ssh round trip, often hundreds of milliseconds, and the remote command's start-up land in the result.

## ROS2

The ROS2 Bridge connects the Link to ROS2 Jazzy on the companion computer: `/odom`, the TF, `/imu/data`, the vehicle status, `/cmd_vel` and services for arming, the mode and the emergency stop. It is built from the repository with colcon, not from a bundle, and needs ROS2 Jazzy from apt on Ubuntu 24.04; its guide covers installing, building, serial device permissions, the optional [`vehkernel-bridge.service`](ros2-bridge.md#running-the-bridge-as-a-user-service) user unit, and running it against SITL and a board ([ROS2 Bridge](ros2-bridge.md)). Both the Bridge and the Link monitor open the board's serial device: `tools/flash/deploy.py` automatically pauses and restarts `vehkernel-bridge.service` around flashing, while manual `link_monitor.py` sessions require stopping the Bridge first (`systemctl --user stop vehkernel-bridge.service`).

## Workaround before bundles

Before host tools bundles were introduced, the workaround was copying `build/<preset>/generated/` and `app/` by hand from a development host to identical relative paths on the companion computer, and passing `--msg-dir` and `--param-meta` explicitly. The bundle replaces this workaround with self-contained, reproducible, commit-tagged archives.
