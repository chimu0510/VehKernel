# Documentation

| Section | Contents |
|---|---|
| [`guide/`](guide/) | How to build, flash, run SITL, drive and test, for users and developers |
| [`boards/`](boards/) | One page per supported board: connectors, ports and wiring |
| [`adr/`](adr/README.md) | Architecture decision records: why the firmware is built the way it is |
| [`specs/`](specs/README.md) | Milestone specs: what each milestone set out to build |
| [`agents/`](agents/) | Workflow rules for the issue tracker, triage labels and handoff between sessions, written for AI coding agents and followed by human contributors too |
| [`../GLOSSARY.md`](../GLOSSARY.md) | The project vocabulary; docs, code and issues use its terms |

## Guides

### Getting Started

| Guide | Contents |
|---|---|
| [Environment setup](guide/setup.md) | Role-based host, companion and desktop setup (`vk_setup.py`), `vk` wrapper, udev rules, `vehkernel-bridge.service` |
| [First deployment](guide/first-deployment.md) | End-to-end walkthrough from a fresh C50C V1.1 and companion computer to ROS2 and a first bench drive |
| [Building](guide/building.md) | Devcontainer, host tools, presets, Kconfig |
| [SITL](guide/sitl.md) | Options, report, motion profiles, transports |
| [Examples](guide/examples.md) | Building, flashing, SITL and lessons for the board examples |

### Deployment & Operations

| Guide | Contents |
|---|---|
| [Flashing](guide/flashing.md) | SWD, the serial flasher, remote `deploy.py`, rebooting into the bootloader |
| [Companion computer](guide/companion-computer.md) | Host tools bundle, unpacking, parameters, flashing, clock synchronization, the Bridge |
| [ROS2 Bridge](guide/ros2-bridge.md) | Installing ROS2 Jazzy, building and running the Bridge, topics, services, parameters, frames, silence |
| [Driving the wheels](guide/driving.md) | Safety conditions, arming, motor bring-up, driving, odometry, remote control |
| [Link monitor](guide/link-monitor.md) | Live view, checks, attitude, parameters, recording a log |
| [Local display](guide/local-display.md) | Status bar, pages, user key, local menu, display protection |
| [Estimation](guide/estimation.md) | Attitude estimators, heading fusion, magnetometer calibration, board orientation check |
| [Logging](guide/logging.md) | Recording, profiles, dropouts, opening logs, replay |
| [Troubleshooting](guide/troubleshooting.md) | Common failure modes across container builds, serial ports, flashing, ROS2 discovery, estimation and arming |

### Developer & Architecture

| Guide | Contents |
|---|---|
| [Architecture](guide/architecture.md) | Software layers, bus backbone, work queues, control chain, Links and safety boundaries |
| [Configuration and codegen](guide/configuration-and-codegen.md) | Kconfig layers, board definitions, topic schemas, parameter definitions and code generators |
| [Testing](guide/testing.md) | Test layers, `ctest`, estimator replay, fuzzing, CI jobs, bring-up image, hardware checks |

### Board & Design Reference

| Document | Contents |
|---|---|
| [C50C V1.1](boards/c50c-v1_1.md) | C50C V1.1 connectors, ports and wiring ([ADR-0024](adr/0024-c50c-v1_1-board.md)) |
| [Architecture Decision Records](adr/README.md) | Every architectural decision and why it was made |
| [Milestone specs](specs/README.md) | What each milestone set out to build and its hardware verification |
| [Glossary](../GLOSSARY.md) | The project vocabulary; docs, code and issues use its terms |

## Agent documentation

Maintainer and AI coding agent workflow documents live under [`agents/`](agents/):

| Document | Contents |
|---|---|
| [Issue tracker](agents/issue-tracker.md) | Managing GitHub issues, sub-issues and specs with `gh` |
| [Triage labels](agents/triage-labels.md) | Five-role issue triage vocabulary |
| [Domain docs](agents/domain.md) | Maintaining `GLOSSARY.md` and `docs/adr/` |
| [Handoff](agents/handoff.md) | Starting a session, finishing a ticket and ending a session |
| [Delegation](agents/delegation.md) | Splitting implementation work through agy or a Claude Code session |
| [Delegating to agy](agents/delegating-to-agy.md) | Sandboxing the Antigravity CLI for ticket work: install, login, running, checking the sandbox |

## Repository layout

| Directory | Contents |
|---|---|
| `app/` | Entry points: firmware image and SITL executable |
| `boards/` | Board definitions and board defconfigs |
| `bootloader/` | The Vehkernel bootloader |
| `bridge/` | ROS2 Bridge for the companion computer (ADR-0027, spec 0015): the colcon packages `vehkernel_msgs` and `vehkernel_bridge` |
| `cmake/` | Toolchain, Kconfig integration, generator invocation and the layering rules |
| `core/` | Bus, work queues, parameter runtime and the types shared by modules and Links |
| `docs/` | This documentation |
| `drivers/` | Driver layer: device drivers |
| `examples/` | Board examples, one component at a time |
| `external/` | External code, pinned as git submodules |
| `hal/` | Interface layer: HAL interfaces |
| `lib/` | Estimation math components |
| `modules/` | Module layer: one directory per module |
| `platform/` | Platform layer: one directory per platform, SITL among them |
| `schema/` | Topic schemas, enums whose values cross the wire, and log profiles |
| `sim/` | SITL's simulated devices and the ground vehicle they move |
| `soc/` | One directory per SoC, grouped by vendor and family |
| `test/` | Tests, by layer ([Testing](guide/testing.md)) |
| `tools/` | Host tools, one directory per purpose (`agy/`, `bootloader/`, `bringup/`, `bundle/`, `examples/`, `firmware/`, `flash/`, `generators/`, `kconfig/`, `link/`, `log/`, `setup/`, `socdb/`) |
| `tools/setup/` | Role-based setup CLI (`vk_setup.py`), worktree-safe `vk` wrapper, `ntp_offset.py`, udev rules, and user `systemd` templates ([ADR-0028](adr/0028-host-environment-setup.md)) |
| `.devcontainer/` | The pinned development image used locally and by CI |
| `.github/` | CI workflow, issue forms and the pull request template |

Parameter definitions (`params.toml`) live in the directory of their module or driver, so they move or disappear together with their module (ADR-0012). Log profiles span several modules' topics and live beside the schema, in `schema/log_profiles.toml` (ADR-0019).
