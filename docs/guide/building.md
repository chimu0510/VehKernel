# Building

## Development environment

Vehkernel builds in a container. Every tool that affects build output, from the Arm toolchain, CMake and Ninja to kconfiglib, clang-tidy and the Python tools, is pinned in the image that [`.devcontainer/Dockerfile`](../../.devcontainer/Dockerfile) builds ([ADR-0016](../adr/0016-dependency-policy.md)). CI publishes pre-built `linux/amd64` and `linux/arm64` images to `ghcr.io/chimu0510/vehkernel-dev:latest` and runs in the same image. **Run `cmake` and `ctest` inside the container, not on the host.** On the host, configuring fails, for example with `No module named 'kconfiglib'`, or builds with a different compiler.

Third-party C code (FreeRTOS, CMSIS, ST LL, TinyUSB) is pinned as git submodules under `external/`. Clone with them, or run `git submodule update --init --recursive` in an existing clone:

```bash
git clone --recurse-submodules https://github.com/chimu0510/VehKernel.git
cd VehKernel
```

There are two ways into the container.

**VS Code.** Open the repository with Dev Containers ("Reopen in Container"). [`.devcontainer/devcontainer.json`](../../.devcontainer/devcontainer.json) pulls `ghcr.io/chimu0510/vehkernel-dev:latest`, and its terminal is inside the container, so the commands in these guides run as written.

**Docker on the command line**, as CI does. On Ubuntu 24.04 (`amd64` or `arm64`), run [`tools/setup/vk_setup.py`](../../tools/setup/vk_setup.py) to install Docker (`docker.io` and `docker-buildx`, preserving an existing `docker-ce` install), add your user to the `docker` group, pull `ghcr.io/chimu0510/vehkernel-dev:latest`, and install the `vk` wrapper to `~/.local/bin/vk` ([Environment setup](setup.md)):

```bash
tools/setup/vk_setup.py --role dev
```

Log out and log back in if you were just added to the `docker` group (and ensure `~/.local/bin` is on `PATH`), then verify:

```bash
tools/setup/vk_setup.py --role dev --check
```

To pull manually when Docker is already configured, or to build locally as a fallback (for example offline or after editing `.devcontainer/Dockerfile`):

```bash
# Pre-built image from GHCR (default)
docker pull ghcr.io/chimu0510/vehkernel-dev:latest

# Local fallback build (or: tools/setup/vk_setup.py --role dev --build-local)
docker build -t vehkernel-dev:latest -f .devcontainer/Dockerfile .
```

The image builds natively on `linux/amd64` and `linux/arm64` hosts (Raspberry Pi 5, Jetson, Apple Silicon through Docker Desktop's Linux VM), selecting the matching Arm toolchain archive ([ADR-0016](../adr/0016-dependency-policy.md)). On any other architecture, the build stops at its first step.

Run container commands through [`tools/setup/vk`](../../tools/setup/vk) (installed as `~/.local/bin/vk` by `--role dev`, or run as `./tools/setup/vk`):

```bash
vk cmake --preset test
vk cmake --build --preset test
vk ctest --preset test
```

`ctest` runs what its own preset built: configure and build `test` before `ctest --preset test`.

What `tools/setup/vk` handles automatically:

- Selects `VK_IMAGE` if set, otherwise `ghcr.io/chimu0510/vehkernel-dev:latest`, falling back to local `vehkernel-dev:latest` when only the local image is present.
- Runs as your host user (`-u "$(id -u):$(id -g)"`) with `HOME=/tmp` and `safe.directory='*'` via `GIT_CONFIG_*`, so the build tree belongs to your user and `git` works regardless of host UID.
- Mounts the repository at the same path (`-v "$PWD:$PWD" -w "$PWD"`), so host tools and editors can read `compile_commands.json`, `--msg-dir`, and flashing outputs.
- Detects git worktrees (`git rev-parse --git-common-dir`) and mounts the common `.git` directory read-only at its host path, so `git describe`, `finalize_image.py`, and `reuse_lint` work in worktrees.
- Uses `--network host` so a Link monitor on the host can reach SITL's UDP port.
- Adds `-t` only when both stdin and stdout are TTYs, so interactive tools (`menuconfig`, `Ctrl-C` on SITL) get a terminal while pipes and scripts work cleanly.

Chain commands with `vk bash -c "cmake --preset sitl && cmake --build --preset sitl"`.

Pull a newer image (`docker pull ghcr.io/chimu0510/vehkernel-dev:latest`) or rebuild locally (`tools/setup/vk_setup.py --role dev --build-local`) after `.devcontainer/Dockerfile`, `pyproject.toml`, `uv.lock` or `.python-version` change; an outdated image lacks newer tools. To update the OS packages in the image, the host compiler among them, move `UBUNTU_SNAPSHOT` in the Dockerfile: they come from the Ubuntu snapshot service at that timestamp, so two builds of the same Dockerfile install the same versions. A build directory configured anywhere else, on the host or at another mount path such as CI's `/work`, fails to configure again: delete it, for example `rm -rf build/sitl`, and configure again in the container.

### Container or host

| In the container | On the host |
|---|---|
| `cmake`, `ctest` and every preset's build | The Link monitor on a serial port (`--serial /dev/ttyACM0`), including recording a log from a board |
| SITL, and the SITL and unit tests; estimator replay and comparison (`tools/log/`) and pyulog's tools | Flashing: `tools/flash/flash_serial.py`, `tools/flash/deploy.py` (ssh to a companion computer), `stm32flash`, ArduPilot's `uploader.py` and OpenOCD |
| clang-tidy, the generators and the license check (`reuse lint`) | `tools/bringup/run_bringup.py`, which flashes over SWD |

The host tools need the board's USB devices. They read the build tree that the container wrote, and need only Python 3.10+ with `pyserial` besides the tools below. On Ubuntu 24.04 or Debian 12 / Raspberry Pi OS, install and verify them with `tools/setup/vk_setup.py --role flash` and `tools/setup/vk_setup.py --role flash --check` ([Environment setup](setup.md)):

| Tool | Install / version | Used for |
|---|---|---|
| OpenOCD | `sudo apt install openocd` (0.12.0) | Flashing and bring-up over SWD ([Flashing](flashing.md#over-swd), [Bring-up image](testing.md#bring-up-image)) |
| [`stm32flash`](https://sourceforge.net/projects/stm32flash/) | `sudo apt install stm32flash` (0.7) | Flashing through the STM32 system bootloader ([Flashing](flashing.md#a-new-board-new-board)) |
| [pyserial](https://pypi.org/project/pyserial/) | `sudo apt install python3-serial` (3.5, as `pyproject.toml` pins it) | The Link monitor on a serial port ([Link monitor](link-monitor.md)), and ArduPilot's `uploader.py`, which updates the firmware through the bootloader ([Flashing](flashing.md#updates-update)) |

On the host, serial and SWD access also require `dialout`/`plugdev` group membership, the udev rules in [`tools/setup/udev/99-vehkernel.rules`](../../tools/setup/udev/99-vehkernel.rules), and `ModemManager`/`brltty` mitigation (so neither probes `/dev/ttyACM*` or toggles DTR/RTS on a download circuit). `tools/setup/vk_setup.py --role flash` automates all of these steps and `--role flash --check` verifies them; see [Environment setup](setup.md#udev-rules-99-vehkernelrules) for details and the manual commands.

## Presets

There is one CMake preset per build target:

| Preset | Output |
|---|---|
| `c50c-v1_1` | C50C V1.1 firmware and bootloader. In `app/`: `vehkernel.elf`, `vehkernel.hex` and `vehkernel.bin` (the firmware alone, from S4), `vehkernel.apj` (ArduPilot's firmware file, ADR-0023) and `vehkernel_with_bl.hex` (the bootloader and the firmware, for a new board). In `bootloader/`: `vehkernel-bootloader.elf` and `.bin` (S0). Every firmware image carries ArduPilot's app descriptor, filled after linking with its CRCs, size, git hash, version and board ID |
| `c50c-v1_1-bringup` | The C50C V1.1 bring-up image ([Testing](testing.md#bring-up-image)) |
| `c50c-v1_1-examples` | The C50C V1.1 board examples ([Examples](examples.md)). In `app/examples/`: `example-NN-name.elf`, `.bin`, `.hex`, `.apj` |
| `sitl` | SITL executable for the development host ([SITL](sitl.md)) |
| `sitl-examples` | The SITL-compatible board examples ([Examples](examples.md)). In `app/examples/`: `example-NN-name` |
| `test` | Host tests, run with `ctest --preset test` ([Testing](testing.md)), and `vehkernel-replay` in `test/`, the replay executable that `tools/log/replay.py` runs ([Logging](logging.md#estimator-replay-and-comparison)) |
| `fuzz` | The Link fuzz targets, built with Clang, libFuzzer and the sanitizers, run with `ctest --preset fuzz` ([Testing](testing.md#fuzzing-the-link-decoders)) |

Configure and build a preset in the container (omit `vk` inside VS Code Dev Containers):

```bash
vk cmake --preset c50c-v1_1
vk cmake --build --preset c50c-v1_1
```

Every firmware and bootloader build prints the usage of each memory region; outgrowing a region fails the link (ADR-0017, ADR-0020). The bootloader's budget is the BOOT region, 16 KB on the C50C. The link also prints two warnings from newlib's `nosys` stubs, `_getpid is not implemented and will always fail` and `_kill is not implemented and will always fail`. They are expected: the C library links in its signal handling, and the firmware has no processes or signals for these stubs to serve.

The `sitl`, `test` and `fuzz` presets also run clang-tidy and treat every warning as an error. CI builds every preset and runs the tests in the same image.

## Build configuration (Kconfig)

Kconfig decides what each build contains (ADR-0017). The layers are the SoC defaults (`soc/<vendor>/<family>/<soc>/defconfig`, or `platform/sitl/defconfig` for SITL), the board defaults (`boards/<board>/defconfig`), and the debug overlays: the local `build/<preset>/debug.config`, which is never committed, then any fragments listed in the `VK_DEBUG_OVERLAY` cache variable. The `c50c-v1_1-bringup` preset uses the latter for the committed `test/hardware/bringup.config`. Edit the local overlay by hand or interactively in the container (`vk` allocates a TTY automatically when run from a terminal); the next build reconfigures:

```bash
vk cmake --build --preset c50c-v1_1 --target menuconfig
```
