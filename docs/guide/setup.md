# Environment setup

Vehkernel separates machine prerequisites into four composable roles managed by [`tools/setup/vk_setup.py`](../../tools/setup/vk_setup.py) ([ADR-0028](../adr/0028-host-environment-setup.md), [spec 0017](../specs/0017-user-environment-setup.md)). The CLI uses only the Python 3.10+ standard library, runs idempotently, previews every planned change with `--dry-run`, and audits an existing machine with `--check`.

## Supported operating systems

| OS | Architectures | Supported roles | Notes |
|---|---|---|---|
| Ubuntu 24.04 (`noble`) | `x86_64` (`amd64`), `aarch64` (`arm64`, including Raspberry Pi 5) | `dev`, `flash`, `companion`, `desktop` | Full support, including ROS2 Jazzy ([ADR-0027](../adr/0027-ros2-bridge.md)) |
| Debian 12 / Raspberry Pi OS (`bookworm`) | `amd64`, `arm64` | `flash` | Serial flashing, SWD, and bundled host tools only; requesting `dev`, `companion` or `desktop` exits with code 2 |

Non-Debian Linux distributions, macOS, and native Windows hosts are out of scope ([ADR-0028](../adr/0028-host-environment-setup.md)). On Windows, run Ubuntu 24.04 under WSL2.

## Roles

Pass `--role` once per role to configure or check; roles are composable (for example, `--role dev --role flash` on a development host wired to a board).

| Role | Target machine | What it installs and checks |
|---|---|---|
| `dev` | Development host | Docker (`docker.io` and `docker-buildx` if `docker` is absent; preserves an existing `docker-ce` install), `docker` group membership, pre-built `ghcr.io/chimu0510/vehkernel-dev:latest` image (or local `vehkernel-dev:latest` with `--build-local`), `~/.local/bin/vk` wrapper, `~/.local/bin` on `PATH`, and git submodule initialization |
| `flash` | Development host or companion computer flashing hardware | `python3-serial`, `stm32flash`, `openocd`, `dialout` and `plugdev` group membership, `/etc/udev/rules.d/99-vehkernel.rules`, `ModemManager.service` disabled, and `brltty` removed |
| `companion` | Ubuntu 24.04 companion computer running the ROS2 Bridge | Serial prerequisites (`python3-serial`, `stm32flash`, `dialout`, `99-vehkernel.rules`, `ModemManager`/`brltty` mitigation), ROS2 Jazzy apt repository, `build-essential`, `ros-jazzy-ros-base`, `python3-colcon-common-extensions`, `python3-rosdep`, `git`, `rosdep init`/`update`, optional Bridge build (`--build-bridge`), and optional user `systemd` service (`--install-bridge-service`) |
| `desktop` | Ubuntu 24.04 machine running RViz2 and teleoperation | ROS2 Jazzy apt repository, `ros-jazzy-rviz2`, `ros-jazzy-rviz-imu-plugin`, `ros-jazzy-teleop-twist-keyboard`, plus `--check` verification of `ROS_DOMAIN_ID`, `ufw` DDS UDP ports, and NTP synchronization |

## Quick start by machine

### Development host (builds, SITL, tests and flashing)

Clone the repository with submodules, preview the setup, apply it, and verify:

```bash
git clone --recurse-submodules https://github.com/chimu0510/VehKernel.git
cd VehKernel
tools/setup/vk_setup.py --role dev --role flash --dry-run
tools/setup/vk_setup.py --role dev --role flash
```

Adding your user to `docker`, `dialout`, or `plugdev` takes effect in a new login session. Log out and log back in, then verify:

```bash
tools/setup/vk_setup.py --role dev --role flash --check
```

If `ghcr.io/chimu0510/vehkernel-dev:latest` is unreachable or you edited `.devcontainer/Dockerfile`, build the image locally as `vehkernel-dev:latest`:

```bash
tools/setup/vk_setup.py --role dev --build-local
```

### Companion computer (Ubuntu 24.04)

On a fresh Ubuntu 24.04 companion computer (such as a Raspberry Pi 5), configure ROS2 Jazzy, build the Bridge into `~/vk_ws`, and install the optional `vehkernel-bridge.service` user unit:

```bash
tools/setup/vk_setup.py --role companion --yes --workspace ~/vk_ws --build-bridge --install-bridge-service
```

Log out and log back in for the `dialout` group to take effect, then verify:

```bash
tools/setup/vk_setup.py --role companion --workspace ~/vk_ws --build-bridge --install-bridge-service --check
```

To run host tools or flash a board from a companion computer running Debian 12 / Raspberry Pi OS (without a git checkout), run `vk_setup.py --role flash` directly from an unpacked [Host Tools Bundle](companion-computer.md#the-host-tools-bundle).

### Desktop visualization and teleoperation (Ubuntu 24.04)

On an Ubuntu 24.04 machine viewing the vehicle in RViz2 or driving with `teleop_twist_keyboard`:

```bash
tools/setup/vk_setup.py --role desktop --yes
tools/setup/vk_setup.py --role desktop --check
```

## CLI reference

### Common options

| Option | Effect |
|---|---|
| `--role <dev\|flash\|companion\|desktop>` | Role to apply or check; repeatable and required |
| `--dry-run` | Print every planned command and file write without modifying the system |
| `--check` | Read-only doctor: print `[PASS]`, `[WARN]`, and `[FAIL]` checks with remediation steps; exits `0` when no check fails, `1` on failures, and `2` on invalid usage or an unsupported OS |
| `--yes`, `-y` | Pass `-y` and `DEBIAN_FRONTEND=noninteractive` to `apt-get` |
| `--no-sudo` | Do not prefix privileged commands with `sudo` (also omitted automatically when running as root) |

### `dev` role options

| Option | Default | Effect |
|---|---|---|
| `--build-local` | off | Build `.devcontainer/Dockerfile` locally as `vehkernel-dev:latest` instead of pulling |
| `--image <ref>` | `ghcr.io/chimu0510/vehkernel-dev:latest` | Container image reference to pull or check |
| `--repo-dir <path>` | auto-detected | Path to the Vehkernel checkout (used for `--build-local` and git submodule checks) |

### `companion` role options

| Option | Default | Effect |
|---|---|---|
| `--workspace <path>` | `~/vk_ws` | Colcon workspace path for building and running `vehkernel_bridge` |
| `--build-bridge` | off | Run `rosdep install` and `colcon build --base-paths <repo>/bridge --packages-up-to vehkernel_bridge` in `--workspace` |
| `--repo-dir <path>` | auto-detected | Path to the Vehkernel checkout containing `bridge/vehkernel_bridge/package.xml` |
| `--install-bridge-service` | off | Install `~/.config/systemd/user/vehkernel-bridge.service`, create `~/.config/vehkernel/bridge.env` if absent, enable the user service, and run `loginctl enable-linger "$USER"` |
| `--device <path>` | `/dev/ttyACM0` | `VK_BRIDGE_DEVICE` written when creating `~/.config/vehkernel/bridge.env` |
| `--baud <rate>` | `921600` | `VK_BRIDGE_BAUD` written when creating `~/.config/vehkernel/bridge.env` |
| `--ros-domain-id <0..232>` | `0` | `ROS_DOMAIN_ID` written when creating `~/.config/vehkernel/bridge.env` |

Existing `~/.config/vehkernel/bridge.env` files are preserved on re-run so local edits are not overwritten.

## Installed helper files

### The `vk` container wrapper

`--role dev` installs [`tools/setup/vk`](../../tools/setup/vk) to `~/.local/bin/vk` (`0755`). You can also run `./tools/setup/vk` directly from a checkout.

- Selects `VK_IMAGE` if set, otherwise `ghcr.io/chimu0510/vehkernel-dev:latest`, automatically falling back to local `vehkernel-dev:latest` if only the local build is present.
- Allocates a TTY (`-t`) only when both stdin and stdout are terminals, so pipes, redirection, and CI/editor scripts work cleanly.
- Detects git worktrees via `git rev-parse --git-common-dir` and mounts the common `.git` directory read-only at its host path, so `git describe`, `finalize_image.py`, and `reuse_lint` work identically inside worktrees and primary checkouts.
- Runs as your host UID and GID (`-u "$(id -u):$(id -g)"`) with `HOME=/tmp`, `safe.directory='*'` via `GIT_CONFIG_*`, `--network host`, and `-v "$PWD:$PWD" -w "$PWD"`.

See [Building](building.md#development-environment) for build presets and VS Code Dev Containers.

### Udev rules (`99-vehkernel.rules`)

`--role flash` and `--role companion` install [`tools/setup/udev/99-vehkernel.rules`](../../tools/setup/udev/99-vehkernel.rules) to `/etc/udev/rules.d/99-vehkernel.rules` and reload udev:

- Grants non-root access (`MODE="0660"`, `GROUP="plugdev"`, `TAG+="uaccess"`) and sets `ENV{ID_MM_DEVICE_IGNORE}="1"` for STMicroelectronics ST-Link V2, V2-1, and V3 probes (`0483`: `3748`, `374b`, `3752`, `374e`, `374f`, `3753`, `3754`) and their virtual COM ports.
- Grants non-root access (`MODE="0660"`, `GROUP="dialout"`, `TAG+="uaccess"`) and sets `ENV{ID_MM_DEVICE_IGNORE}="1"` for WCH CH9102F USB serial adapters (`1a86:55d4`) so `ModemManager` does not toggle DTR/RTS on the C50C V1.1 download circuit.
- Provides a commented `ENV{ID_PATH}` and `KERNELS` template to pin C50C V1.1's two CH9102F ports (`/dev/vehkernel-link` and `/dev/vehkernel-download`) by physical USB port, avoiding the `/dev/serial/by-id/` collision ([C50C V1.1 serial ports](../boards/c50c-v1_1.md#serial-ports)).

### Optional `vehkernel-bridge.service` user unit

`--role companion --install-bridge-service` installs [`tools/setup/systemd/vehkernel-bridge.service`](../../tools/setup/systemd/vehkernel-bridge.service) to `~/.config/systemd/user/vehkernel-bridge.service` and writes `~/.config/vehkernel/bridge.env` (see [`tools/setup/systemd/bridge.env.example`](../../tools/setup/systemd/bridge.env.example)). Running the Bridge as a user unit with lingering enabled (`loginctl enable-linger "$USER"`) starts it at boot and lets [`tools/flash/deploy.py`](../../tools/flash/deploy.py) stop and restart it over non-interactive SSH without `sudo` ([Running the Bridge as a user service](ros2-bridge.md#running-the-bridge-as-a-user-service)).

### Running from a Host Tools Bundle

Every Host Tools Bundle includes `tools/setup/` (`vk_setup.py`, `ntp_offset.py`, `vk`, udev rules, and `systemd` templates), so a companion computer can run `--role flash`, `--role companion --check`, or clock offset checks directly from the unpacked archive without a git checkout ([Companion computer](companion-computer.md)).
