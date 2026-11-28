# Spec 0017: user environment setup and deployment

**Status**: done. Issue: [#216](https://github.com/chimu0510/VehKernel/issues/216); tickets #217 to #223, confirmed on Ubuntu 24.04 development host, Raspberry Pi 5 companion with C50C V1.1, and Ubuntu 24.04 desktop RViz2 in #223.
This spec is done and no longer edited; later decisions live in the ADRs and the code, which win where they differ.

## Problem Statement

A developer or user cloning Vehkernel on a fresh machine currently cannot follow `docs/guide/` end-to-end without hitting missing packages, missing permissions, or fragile host assumptions:

- **No environment setup or verification tooling exists.** Every host dependency (`docker`, `openocd`, `stm32flash`, `python3-serial`, ROS2 Jazzy packages, `dialout`/`plugdev` group membership, udev rules, `ModemManager`/`brltty` handling) is either scattered across prose or omitted altogether.
- **Fresh companion deployments fail at `colcon build` and serial access.** `docs/guide/ros2-bridge.md` installs `ros-jazzy-ros-base`, `python3-colcon-common-extensions`, `python3-rosdep`, and `git`, omitting `build-essential` (`g++`/`make`), so compiling `bridge/` fails on a clean Ubuntu 24.04 Server or Raspberry Pi 5 installation. `docs/guide/companion-computer.md`, `docs/guide/ros2-bridge.md`, and `tools/bundle/README.md` also omit `dialout` group membership and `ModemManager`/`brltty` mitigation (which toggles DTR/RTS on C50C `SERIAL2` and resets the board or enters the ROM bootloader), and `ros2-bridge.md` recommends `/dev/serial/by-id/`, where C50C V1.1's two CH9102F adapters collide on the same name.
- **No pre-built devcontainer image is published.** Every new contributor must build `.devcontainer/Dockerfile` locally from scratch, downloading the Arm toolchain, building GoogleTest, and pulling OS packages from `snapshot.ubuntu.com`, which frequently returns HTTP 5xx outages.
- **The documented `vk()` shell snippet is ephemeral and fragile.** It is never installed, hardcodes `-it` (breaking pipes and scripts), mounts only `$PWD` (breaking `git describe` and `reuse_lint` in git worktrees), and relies on host UID/git ownership matching the container.
- **Guide commands carry implicit assumptions and `reuse_lint` traps.** Guides omit `openocd` installation and ST-Link udev rules, omit `--msg-dir`/`--param-meta` in `estimation.md`, mix container and host commands in `logging.md`, omit `ros-jazzy-teleop-twist-keyboard` and Desktop RViz setup, ask users to hand-copy `ntp_offset.py` or save logs/flash dumps inside the repository root (breaking `reuse_lint`), and leave `vehkernel_bridge` without a standard service lifecycle when `tools/flash/deploy.py` needs the serial port.

## Solution

Deliver the seventeenth milestone: **reproducible, role-based host and companion setup tooling, pre-built multi-arch devcontainer images, and clean-machine-verified deployment guides**. Afterwards:

- A user on Ubuntu 24.04 (`x86_64` or `arm64`, including Raspberry Pi 5) or Debian 12 / Raspberry Pi OS (for flashing and host tools) can run `tools/setup/vk_setup.py --role <role>` to install prerequisites, udev rules, and wrapper scripts idempotently, preview changes with `--dry-run`, and diagnose any machine with `--check`.
- Pre-built `linux/amd64` and `linux/arm64` devcontainer images are published to `ghcr.io/chimu0510/vehkernel-dev` by CI, with local `docker build` retained as a first-class fallback, and an installed `vk` wrapper works across terminals, pipes, and git worktrees.
- A Companion Computer can optionally run `vehkernel_bridge` as a user `systemd` service (`vehkernel-bridge.service`), and `tools/flash/deploy.py` automatically stops and restarts it around serial flashing without requiring remote `sudo`.
- `ntp_offset.py`, `vk_setup.py`, udev rules, and `systemd` unit templates travel inside the Host Tools Bundle so companions need no git checkout to check or maintain their environment.
- Every guide under `docs/guide/` is audited and verified against clean OS containers so fresh-machine onboarding succeeds end-to-end.

## User Stories

1. As a firmware or SITL contributor on a fresh Ubuntu 24.04 workstation, I want one command to check Docker, pull the pre-built `vehkernel-dev` image, and install a worktree-safe `vk` command, so that I can build and run `ctest --preset test` in minutes without hitting `snapshot.ubuntu.com` build failures.
2. As a developer flashing or bringing up a C50C V1.1 board on Ubuntu 24.04 or Debian 12 / Raspberry Pi OS, I want one command to install `openocd`, `stm32flash`, `python3-serial`, `dialout`/`plugdev` permissions, ST-Link and CH9102F udev rules, and `ModemManager`/`brltty` protection, so that SWD bring-up and serial flashing work without `sudo` or board resets.
3. As a user deploying the ROS2 Bridge on a fresh Ubuntu 24.04 Companion Computer (such as a Raspberry Pi 5), I want one command to configure the ROS2 Jazzy apt repository, `build-essential`, `colcon`, `rosdep`, serial permissions, and an optional `vehkernel-bridge.service` user unit, so that `colcon build` and runtime operation succeed on the first try.
4. As a developer deploying firmware over SSH with `tools/flash/deploy.py`, I want it to pause an active `vehkernel-bridge.service` before flashing and restart it afterwards (even if flashing fails), so that I do not have to SSH in manually to kill and relaunch the Bridge.
5. As an operator on a ROS2 Desktop machine, I want a `desktop` setup role that installs RViz2, `ros-jazzy-rviz-imu-plugin`, and `ros-jazzy-teleop-twist-keyboard` and checks `ROS_DOMAIN_ID` and firewall settings, so that monitoring and teleoperation work out of the box.
6. As a user troubleshooting a broken setup on any device, I want `vk_setup.py --role <role> --check` to report every missing package, group, udev rule, service conflict, or environment mismatch with the exact corrective action, and exit non-zero on failure.

## Implementation Decisions

- **ADR updates and new ADR-0028.**
  - Add `docs/adr/0028-host-environment-setup.md` (indexed in `docs/adr/README.md`) recording the trade-offs for role-based setup in pure Python 3.10+ stdlib, udev/user-systemd templates, and OS support boundaries (Ubuntu 24.04 for full roles; Debian 12 / Raspberry Pi OS for `flash`/host tools; rejected Ansible/Nix/ROS2-in-Docker).
  - Update `docs/adr/0016-dependency-policy.md` in place to cover publishing multi-arch `ghcr.io/chimu0510/vehkernel-dev` images from CI (keeping `.devcontainer/Dockerfile` as the single source of truth with local build fallback) and referencing `tools/setup/vk_setup.py` for outside-container host dependencies.
  - Update `docs/adr/0026-host-tools-bundle.md` in place to include `tools/setup/` (`vk_setup.py`, `ntp_offset.py`, udev and systemd templates) in the Host Tools Bundle.
  - Update `docs/adr/0027-ros2-bridge.md` in place to record the required C++ build toolchain (`build-essential`), the optional `vehkernel-bridge.service` user unit, and coordination with `deploy.py`.
- **Role-based CLI (`tools/setup/vk_setup.py`).**
  - Written in pure Python 3.10+ standard library (zero third-party runtime dependencies) with `tools/setup/ruff.toml` (`target-version = "py310"`), matching `tools/link/`, `tools/log/`, and `tools/flash/` and covered by `python_lint`, `pytest`, and CI's `host-tools-py310` job.
  - Supports composable `--role` values (`dev`, `flash`, `companion`, `desktop`), `--check` (read-only doctor, exits `0` on pass, `1` on failures), `--dry-run` (prints planned commands and file changes without mutating), `--yes` (non-interactive apt), and role-specific options (`--build-local` for `dev`; `--workspace`, `--build-bridge`, `--install-bridge-service`, `--device`, `--baud`, `--ros-domain-id` for `companion`).
  - OS detection via `/etc/os-release`: supports Ubuntu 24.04 (`noble`, `amd64`/`arm64`) for all four roles, and Debian 12 (`bookworm`, including Raspberry Pi OS) for `--role flash`. Fails fast with a clear message when `dev`, `companion`, or `desktop` is requested on an unsupported OS (e.g. ROS2 Jazzy on Debian 12 per ADR-0027).
  - Idempotent execution: checks existing packages, group memberships, installed rule/unit files, and active services before modifying; invokes `sudo` only for privileged system operations (never when already root, e.g. inside containers); prints a clear notice when a new group (`docker`, `dialout`, `plugdev`) requires re-login.
- **Docker installation & `vk` container wrapper (`tools/setup/vk`).**
  - `--role dev` checks for an existing `docker` binary; if missing on Ubuntu 24.04, installs `docker.io` and `docker-buildx` (avoiding third-party apt repos while respecting an already-installed `docker-ce`), adds the user to `docker`, pulls `ghcr.io/chimu0510/vehkernel-dev:latest` (or runs `docker build` with `--build-local`), and installs `tools/setup/vk` to `~/.local/bin/vk`.
  - `tools/setup/vk` is a POSIX `sh` wrapper (syntax-checked via `sh -n` in L0 tests) that:
    - Adds `-t` only when both stdin and stdout are TTYs (`[ -t 0 ] && [ -t 1 ]`), so pipes and scripts work cleanly.
    - Detects git worktrees via `git rev-parse --git-common-dir` and mounts the common `.git` directory read-only at its host path, so `git describe`, `finalize_image.py`, and `reuse_lint` work identically in worktrees and primary checkouts.
    - Runs with `-u "$(id -u):$(id -g)"`, sets `HOME=/tmp` if the host UID is absent from `/etc/passwd`, and passes `safe.directory='*'` via `GIT_CONFIG_*` env vars (in addition to setting `git config --global --add safe.directory '*'` in `.devcontainer/Dockerfile`).
    - Honors `VK_IMAGE` (defaulting to `ghcr.io/chimu0510/vehkernel-dev:latest`, with automatic fallback to local `vehkernel-dev:latest` if only the local tag exists).
- **GHCR multi-arch devcontainer publishing.**
  - Add a GitHub Actions workflow (`.github/workflows/devcontainer.yml`, or jobs in `ci.yml`) with `permissions: { contents: read, packages: write }` that builds `linux/amd64` on `ubuntu-24.04` and `linux/arm64` on `ubuntu-24.04-arm`, computes a content hash over `.devcontainer/Dockerfile`, `pyproject.toml`, `uv.lock`, and `.python-version`, pushes per-arch digests, and creates a multi-arch manifest tagged both `sha-<hash>` and `latest` on pushes to `main` (and `workflow_dispatch`).
  - `ci.yml`'s `build` job attempts `docker pull ghcr.io/chimu0510/vehkernel-dev:sha-<hash>` first and falls back to local `docker build` when the tag does not yet exist (e.g. a PR editing the Dockerfile or a fork without a published match).
  - `.devcontainer/devcontainer.json` points to `ghcr.io/chimu0510/vehkernel-dev:latest` (or documents the local build option cleanly for Dockerfile changes).
- **Udev rules (`tools/setup/udev/99-vehkernel.rules`).**
  <!-- REUSE-IgnoreStart -->
  - Ships with `# SPDX-License-Identifier: GPL-3.0-or-later`.
  <!-- REUSE-IgnoreEnd -->
  - Grants non-root access (`MODE="0660"`, `GROUP="plugdev"`, `TAG+="uaccess"`) and `ENV{ID_MM_DEVICE_IGNORE}="1"` (for VCP interfaces) to STMicroelectronics ST-Link probes (`0483`: `3748`, `374b`, `3752`, `374e`, `374f`, `3753`, `3754`).
  - Sets `ENV{ID_MM_DEVICE_IGNORE}="1"`, `MODE="0660"`, `GROUP="dialout"`, `TAG+="uaccess"` for WCH CH9102F (`1a86:55d4`) without creating a colliding static symlink.
  - Includes a commented `ENV{ID_PATH}` / `KERNELS` template showing how to pin C50C V1.1's `SERIAL1` (`/dev/vehkernel-link`) and `SERIAL2` (`/dev/vehkernel-download`) by physical USB port, resolving the `/dev/serial/by-id/` collision documented in `docs/boards/c50c-v1_1.md`.
- **Optional Companion `systemd` user service (`tools/setup/systemd/vehkernel-bridge.service`).**
  - Installed under `~/.config/systemd/user/vehkernel-bridge.service` with environment file `~/.config/vehkernel/bridge.env` (`VK_BRIDGE_WORKSPACE`, `VK_BRIDGE_DEVICE`, `VK_BRIDGE_BAUD`, `ROS_DOMAIN_ID`), enabled with `loginctl enable-linger $USER` so it starts at boot without requiring `sudo` at runtime.
  - Choosing a user service rather than a system service avoids `sudo` prompts and `/etc/sudoers.d` edits when `tools/flash/deploy.py` stops and restarts the Bridge over non-interactive SSH.
- **`tools/flash/deploy.py` Bridge coordination.**
  - Adds `--bridge-service <unit>` (default `vehkernel-bridge.service`) and `--no-bridge-service` before the positional `host` argument.
  - Replaces `exec` in `flash_command()` with a POSIX `sh` wrapper that exports `XDG_RUNTIME_DIR="/run/user/$(id -u)"` (and `DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"`) when unset, checks `command -v systemctl` and `systemctl --user is-active --quiet <unit>`, stops the unit only if active, runs `flash_serial.py`, captures its exit code, restarts the unit only if `deploy.py` stopped it (even on flash failure), and exits with the exact flash exit code.
  - Clean no-op when `systemctl` is absent or the service is not installed/running, preserving backwards compatibility with non-systemd companions and existing `deploy.py` usage.
- **`ntp_offset.py` and Host Tools Bundle.**
  - Move the inline script from `docs/guide/companion-computer.md` into `tools/setup/ntp_offset.py` (stdlib Python 3.10+, SPDX header, `0o755`, preserves `python3 - <server> < tools/setup/ntp_offset.py` stdin compatibility for remote comparison).
  - Update `tools/bundle/make_bundle.py`, `test/bundle/test_bundle.py`, `tools/bundle/tests/test_make_bundle.py`, and `tools/bundle/README.md` to ship `tools/setup/` (`vk_setup.py`, `ntp_offset.py`, udev rules, systemd unit/env templates) inside the Host Tools Bundle.
- **Guide overhaul (`docs/guide/*`, `README.md`, `CONTRIBUTING.md`, `docs/boards/c50c-v1_1.md`).**
  - Add a dedicated quick-reference setup section (or `docs/guide/setup.md` linked from `README.md` and `docs/README.md`) and fix every cataloged gap across `building.md`, `companion-computer.md`, `ros2-bridge.md`, `flashing.md`, `logging.md`, `estimation.md`, `link-monitor.md`, `driving.md`, `local-display.md`, `sitl.md`, `testing.md`, `examples.md`, and `tools/bundle/README.md` (including `build-essential`, `ModemManager`/`brltty`, `by-path` vs `by-id`, `openocd` + ST-Link udev, `--msg-dir`/`--param-meta`, `pyulog`/PlotJuggler/`test` preset callouts, `teleop_twist_keyboard`, and storing output artifacts outside the repo root so `reuse_lint` passes).

## Testing Decisions

- **Seam A: L0 linting, REUSE, and syntax checks.** `reuse_lint` covers every new `.py`, `.sh`, `.rules`, `.service`, `.example`, and `.md` file; `python_lint` (`ruff`) checks `tools/setup/` with `target-version = "py310"`; a CMake/pytest check runs `sh -n tools/setup/vk` and validates udev/systemd template syntax.
- **Seam B: L1 unit tests for `vk_setup.py`, `ntp_offset.py`, `vk`, and `deploy.py`.**
  - `tools/setup/tests/test_vk_setup.py` tests OS/arch matrix (Ubuntu 24.04 amd64/arm64, Debian 12/RPi OS, unsupported OS rejection), role dependency resolution, `--dry-run`, idempotency, `--check` pass/fail diagnostics and exit codes, and `sudo` suppression when root — all via injected command/filesystem fakes (runs in both `ctest --preset test` and CI's `host-tools-py310` job).
  - `tools/setup/tests/test_ntp_offset.py` tests NTP packet packing/parsing and CLI formatting with a mocked UDP socket.
  - `tools/setup/tests/test_vk_wrapper.py` verifies TTY auto-detection, worktree `.git` common-dir mounting, `VK_IMAGE` selection/fallback, and `safe.directory` env vars via a fake `docker` binary in `PATH`.
  - `tools/flash/tests/test_deploy.py` adds cases for active `vehkernel-bridge.service` stop + restart (on flash exit 0 and exit 1), inactive service no-op, missing `systemctl` no-op, and `--no-bridge-service`.
- **Seam C: L1/L3 Host Tools Bundle reproducibility tests.** Update `tools/bundle/tests/test_make_bundle.py` and `test/bundle/test_bundle.py` to assert `tools/setup/` files are included with executable bits and byte-identical bundle reproducibility.
- **Seam D: Clean-OS container verification (CI).**
  - A CI job runs inside a clean `ubuntu:24.04` container (NO pre-installed `build-essential`, NO pre-installed ROS2, NO pre-initialized `rosdep`): runs `vk_setup.py --role companion --yes --workspace /tmp/vk_ws --build-bridge`, then `vk_setup.py --role companion --check`, proving that `bridge/` (`vehkernel_msgs` + `vehkernel_bridge`) builds cleanly from zero on a fresh Ubuntu 24.04 system.
  - A companion CI step runs inside a clean `debian:bookworm-slim` container: runs `vk_setup.py --role flash --yes`, then `vk_setup.py --role flash --check`, and verifies `--role companion` exits cleanly with the unsupported-OS message.
- **Seam E: Fresh-machine and bench check (`ready-for-human`).**
  - On a real development host (or clean VM): pull `ghcr.io/chimu0510/vehkernel-dev:latest`, run `vk_setup.py --role dev --role flash`, verify `vk cmake --preset test` in both main checkout and a worktree, and verify rootless OpenOCD / `run_bringup.py` against ST-Link + C50C V1.1.
  - On the Raspberry Pi 5 companion: run `vk_setup.py --role companion --check` and install `vehkernel-bridge.service`; run `tools/flash/deploy.py` from the dev host and confirm the Bridge stops, firmware flashes, and the Bridge resumes publishing `/odom` and `/vehkernel/vehicle_status`.
  - On a Desktop machine: run `vk_setup.py --role desktop --check` and confirm RViz2 (`bridge.rviz`) + `teleop_twist_keyboard` connect to the companion Bridge.

**Prior art**: `tools/flash/deploy.py` and `tools/flash/tests/test_deploy.py` (fake CLI testing pattern); `tools/bundle/make_bundle.py` (allow-listed bundle packaging); `.github/workflows/ci.yml` (`ubuntu-24.04` / `ubuntu-24.04-arm` multi-arch matrix and `host-tools-py310` job).

## Out of Scope

- **Non-Debian Linux distributions, macOS, and Windows native hosts** (Windows users can use WSL2 Ubuntu 24.04 with the Ubuntu workflow; macOS/Fedora/Arch are out of scope).
- **ROS2 releases other than Jazzy** (per ADR-0027) and **running the ROS2 Bridge inside a Docker container** (per ADR-0027).
- **Publishing `.deb` packages, PyPI wheels, or Snap/Flatpak packages** (per ADR-0026).
- **Automated SSH key provisioning, Wi-Fi / static IP configuration, or custom site NTP server settings** (site-specific secrets and network topology stay outside the repository).
- **Nav2 installation and navigation stack bring-up** (next milestone after this spec).

## Further Notes

Tickets in dependency order:
1. **#217**: ADRs (`ADR-0028`, updates to `ADR-0016`/`0026`/`0027`), `tools/setup/` templates (`99-vehkernel.rules`, `vehkernel-bridge.service`, `bridge.env.example`), `ntp_offset.py`, and worktree-safe `vk` wrapper + unit tests.
2. **#218** (blocked by #217): `tools/setup/vk_setup.py` role-based setup (`dev`, `flash`, `companion`, `desktop`), `--dry-run`, `--check` doctor, unit tests, and Python 3.10 CI wiring.
3. **#219** (blocked by #217, #218): Host Tools Bundle integration for `tools/setup/` + `tools/flash/deploy.py` `vehkernel-bridge.service` stop/restart + tests.
4. **#220** (blocked by #217): GHCR multi-arch `vehkernel-dev` publishing workflow, `.devcontainer/` updates (`safe.directory`, image reference), and CI pull-with-fallback integration.
5. **#221** (blocked by #218, #219, #220): Complete `docs/guide/*`, `README.md`, `CONTRIBUTING.md`, `docs/boards/c50c-v1_1.md`, and `tools/bundle/README.md` onboarding audit fixes.
6. **#222** (blocked by #218, #221): Clean-OS container CI verification (`ubuntu:24.04` companion + `bridge/` build, `debian:bookworm-slim` flash role).
7. **#223** (`ready-for-human`, blocked by #219, #220, #221, #222): Fresh-machine and bench check (dev host + ST-Link, RPi 5 companion + `vehkernel-bridge.service` + `deploy.py`, Desktop RViz + teleop).
