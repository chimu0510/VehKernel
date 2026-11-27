# Vehkernel Host Tools Bundle

This bundle contains the host tools, setup utilities (`tools/setup/`), generated schema decoders, parameter metadata, and firmware images for a single build of Vehkernel (ADR-0026, ADR-0028). It runs directly from the unpacked directory on any architecture with no git checkout and no firmware build.

## Requirements

- Python 3.10 or newer
- pyserial: `sudo apt install python3-serial` (or `pip install pyserial` in a virtual environment)
- stm32flash (for initial flashing of a new board over a download port): `sudo apt install stm32flash`
- Serial port permissions and interference protection on Linux:
  - Add your user to the `dialout` group (`sudo usermod -aG dialout "$USER"` and log out and back in), plus `plugdev` if using an ST-Link probe.
  - Install `tools/setup/udev/99-vehkernel.rules` to `/etc/udev/rules.d/99-vehkernel.rules` and keep `ModemManager` and `brltty` from probing the board's USB-UART adapters (probing toggles DTR/RTS on C50C V1.1 `SERIAL2` and resets the MCU or enters the ROM bootloader).

On Ubuntu 24.04 or Debian 12 / Raspberry Pi OS, run the bundled `vk_setup.py` directly from the unpacked bundle directory to install or verify these prerequisites without a git checkout:

```bash
tools/setup/vk_setup.py --role flash --dry-run
tools/setup/vk_setup.py --role flash
tools/setup/vk_setup.py --role flash --check
```

On an Ubuntu 24.04 Companion Computer running the ROS2 Bridge, verify the companion setup with:

```bash
tools/setup/vk_setup.py --role companion --check
```

## Running from the bundle

Unpack the tarball and run commands from the unpacked bundle directory (`tar -xzf vehkernel-<preset>-<commit>.tar.gz && cd vehkernel-<preset>-<commit>`). The archive preserves executable permissions (`0755`) on the tool scripts (`tools/link/link_monitor.py`, `tools/flash/flash_serial.py`, `tools/setup/vk_setup.py`, `tools/setup/ntp_offset.py`, `tools/setup/vk`, and the Python log tools), so you can invoke them directly or with `python3`.

The tools detect the bundle and automatically use its generated message definitions, parameter metadata, and firmware images as defaults without extra `--msg-dir`, `--param-meta`, or `--build` flags.

### Environment setup and clock checks

Preview or audit the machine environment straight from the bundle (no git checkout required):

```bash
python3 tools/setup/vk_setup.py --role flash --dry-run
python3 tools/setup/vk_setup.py --role companion --check
```

Measure this machine's clock offset against an NTP server (or compare a Companion Computer against a development host):

```bash
tools/setup/ntp_offset.py pool.ntp.org
ssh user@companion python3 - pool.ntp.org < tools/setup/ntp_offset.py
```

The bundle also includes the udev rules (`tools/setup/udev/99-vehkernel.rules`) and optional user `systemd` templates (`tools/setup/systemd/vehkernel-bridge.service`, `tools/setup/systemd/bridge.env.example`).

### Monitoring and checks

Connect to SITL over UDP or to a board over serial:

```bash
tools/link/link_monitor.py --udp 127.0.0.1:15600
tools/link/link_monitor.py --serial /dev/ttyACM0
```

Run a check for 5 seconds expecting IMU samples at 200 Hz:

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 --check --duration 5 --expect-rate imu_sample:0=200
```

### Parameters

List all parameters with descriptions and units:

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 param list
```

Read or set a parameter:

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 param get ATT_KP
tools/link/link_monitor.py --serial /dev/ttyACM0 param set ATT_KP 2.5
tools/link/link_monitor.py --serial /dev/ttyACM0 param set SENS_BOARD_ROT YAW_90
```

### Recording logs

Record a ULog file over the Link with a log profile:

```bash
tools/link/link_monitor.py --serial /dev/ttyACM0 record drive.ulg --profile estimator_replay --duration 30
```

### Flashing

On a board bundle carrying firmware images in `app/`, update the firmware through the Vehkernel bootloader (parameters are preserved):

```bash
tools/flash/flash_serial.py update
```

To back up and flash a new board through its download circuit:

```bash
tools/flash/flash_serial.py new-board
```
