# Testing

Tests are split into six layers, L0 to L5 (ADR-0018). The directories under `test/` follow them: `unit/` and `fuzz/` (L1), `replay/` (L2), `sitl/` (L3), `hardware/` (L4); L0 static checks run from the root config files and CI. The L5 test lives in the Bridge's colcon package, `bridge/vehkernel_bridge/test/`, since colcon runs it ([ROS2 Bridge (L5)](#ros2-bridge-l5)); `ros2/` is empty until the Nav2 test of a later spec.

## Host tests and SITL tests

Unit tests and SITL tests run through `ctest` in the development container ([Environment setup](setup.md), [Building](building.md#development-environment); omit `vk` inside VS Code Dev Containers):

```bash
vk cmake --preset test
vk cmake --build --preset test
vk ctest --preset test
```

The SITL tests (`sitl_*`) start the SITL executable and check it from the development host with the scripts in `test/sitl/`; each guide names the tests that cover it ([SITL](sitl.md), [Link monitor](link-monitor.md), [Driving the wheels](driving.md), [Local display](local-display.md), [Logging](logging.md)). SITL tests run serially under `ctest -j`, since they judge timing on the shared host. CI builds every preset and runs the tests in the same image.

CI gives a failed test a second attempt (`--repeat until-pass:2`), since a stall of the shared runner can fail a check that judges timing. A local run does not, so a failure there counts at once. A test that passed only on its second attempt leaves CI green, so an intermittent bug can pass too: its first attempt stays in the job's log as `***Failed`, followed by the same test's `Passed`. Search the log for `***Failed` when a test is suspect.

The license check, `reuse_lint`, covers every file in the working tree that git does not ignore, untracked ones included. A stray file such as a log written to the repository root fails it with `The following files have no licensing information`: move it out of the tree, or under `build/` or `vendor/`, which git ignores.

The Python lint, `python_lint`, runs ruff (rules `E`, `F`, `W` and `BLE`, line length 120, configured in [`pyproject.toml`](../../pyproject.toml)) over the repository's own Python. The bundled host tools in `tools/link/`, `tools/log/`, `tools/flash/`, and `tools/setup/` are checked against Python 3.10 (a `ruff.toml` in each), so syntax newer than 3.10 fails there and not in the generators ([ADR-0026](../adr/0026-host-tools-bundle.md), [ADR-0028](../adr/0028-host-environment-setup.md)). Fix findings by hand or with `ruff check --fix`; a deliberate exception takes a `# noqa: <code>` comment. `ruff format` is not used.

### Faster local runs

A full run takes a few minutes, for two reasons. The `test` preset runs clang-tidy on every file, which takes about 95 % of the build time (66 s against 3 s on 24 cores). The SITL tests, about 6 minutes, always run one at a time: they judge timing, and running them together would make them fail. While iterating, turn clang-tidy off in the build tree and leave the SITL tests out:

```bash
vk cmake --preset test -DVK_CLANG_TIDY=OFF
vk cmake --build --preset test
vk ctest --preset test -E '^sitl_' -j "$(nproc)"
```

Run one SITL test by name with `vk ctest --preset test -R sitl_body_motion`. Before a pull request, turn clang-tidy back on (`vk cmake --preset test -DVK_CLANG_TIDY=ON`) and run the whole suite, as CI does.

## Estimator replay (L2)

Replay re-runs the firmware's estimator work items on recorded ULog files on the development host, fed from a log in recorded order with the recorded parameters, and compares what they publish with what the log holds (spec 0011, [Logging](logging.md#estimator-replay-and-comparison)). The test `sitl_replay` records a SITL ground vehicle driving a square over UDP, replays it and checks both:
- Faithfulness: the replayed estimate matches the recorded estimate exactly from the estimators' restart to the end of the log.
- Odometry metrics: the replayed odometry ends within `check_odometry.py`'s bounds against the logged ground truth.
- Dropouts: the recorded log has no dropouts.

The test `replay_board_log` replays a log recorded on the C50C V1.1 (`test/replay/data/c50c_v1_1_drive.ulg`, 30 s from the estimators' restart, kept under 1 MB) and checks the replayed estimate within 1e-4 of the recorded one, per field. The board's float maths is not the host's, so the match is not exact; the largest difference seen was 2.6e-6 on the cut and 1.4e-5 on the whole drive. When the estimators change on purpose, the log is recorded again on the board, or the test is switched to metrics against the previous estimate, and the commit says which.

## Fuzzing the Link decoders

The Link decodes bytes from any connected host, so its decoders are fuzzed with libFuzzer (L1, ADR-0018). The `fuzz` preset builds three fuzz targets with Clang, AddressSanitizer, UndefinedBehaviorSanitizer and libstdc++'s bounds checks; the last catch an overrun from one array member into the next, which AddressSanitizer cannot see. The targets live in `test/fuzz/`:

| Target | Input | Checks besides the sanitizers |
|---|---|---|
| `vk_fuzz_cobs` | The bytes between two delimiters, through the COBS decoder | Decoding never overruns its output; anything decoded, encoded again, decodes the same; any input round-trips through the encoder |
| `vk_fuzz_frame_reader` | A byte stream through the frame reader: COBS, length, CRC, frame type and sequence | Frames passed on have a known type and fit; they are read back the same when written again; each delimiter ends at most one frame |
| `vk_fuzz_session` | Frames of any type and body, in any order and at any time step, through `Session::handle()` with the build's parameters and a fake vehicle control; topic data through the schema's decoder for its topic | Every frame the session sends is one valid frame with no sequence gap; the session counts exactly the frames the sink took |

The session target skips COBS and the CRC, which the frame reader target covers, so its inputs reach the frame bodies. Its input format is in `test/fuzz/session_input.hpp`.

The build writes the seed corpus from the schema and the protocol, so it never falls out of step: a valid frame of each request the MCU answers, topic data for every schema topic, and a whole session. `vk ctest --preset fuzz` runs each target for 60 s, three at a time, and fails on any sanitizer report, failed check or input that runs over 10 s; CI runs it on every push:

```bash
vk cmake --preset fuzz
vk cmake --build --preset fuzz
vk ctest --preset fuzz
```

For a shorter or longer bounded run, set `VK_FUZZ_SECONDS` (`vk cmake --preset fuzz -DVK_FUZZ_SECONDS=600`). Longer runs are started by hand inside the container from the build directory; what the fuzzer finds accumulates in `corpus/` and carries over to the next run:

```bash
cd build/fuzz/test/fuzz
vk ./vk_fuzz_session -max_total_time=3600 -jobs=4 -max_len=4096 -artifact_prefix=artifacts/ corpus/session seeds/session
```

A finding leaves its input in `build/fuzz/test/fuzz/artifacts/`. Run the target on that file alone to reproduce it, fix the bug, and add the case as a unit test in `test/unit/link_test.cpp`, so the `test` preset keeps checking it. A memory safety bug in a Link decoder is a security issue (`SECURITY.md`): report it privately rather than in a public issue.

## ROS2 Bridge (L5)

`bridge/vehkernel_bridge/test/test_sitl_bridge.py` is a `launch_testing` test that starts SITL and the Bridge on UDP and checks the outputs, arming, `/cmd_vel`, the silence and the disarm on a killed Bridge (spec 0015, [ROS2 Bridge](ros2-bridge.md#testing)). It runs in the ROS2 image, not in the development container, with `colcon test` from the workspace of the guide. The environment variable `VK_SITL_EXECUTABLE` names the SITL executable of a `sitl` build; without it the test is skipped. CI builds SITL in the development container's job and passes it to the Bridge job of the same architecture, which runs the test with `--repeat until-pass:2` as above. The test takes about 25 s, since it waits out `COM_DISARM_IDLE` (10 s).

## Bring-up image

The bring-up test build (L4) runs the hardware bring-up cases at boot instead of the application: it reads the ICM20948's WHO_AM_I over software I2C and measures the IMU read rate. It never configures actuator outputs, so it is safe with motors connected. Build it in the development container, then run the bring-up runner on the **host** (requires `openocd` and ST-Link udev rules via `tools/setup/vk_setup.py --role flash`), which flashes it over SWD, reads the result block back and exits non-zero if a case fails or no results arrive:

```bash
vk cmake --preset c50c-v1_1-bringup
vk cmake --build --preset c50c-v1_1-bringup
```

On the host:

```bash
tools/bringup/run_bringup.py build/c50c-v1_1-bringup/app/vehkernel.elf
```

Bring-up cases that move actuators run in the main firmware's test mode instead ([Driving the wheels](driving.md#motor-bring-up)).

## Hardware checks

Each milestone ends with a hardware check on the board, recorded on its spec's last ticket (`docs/specs/`). The checks a person can repeat:

| Check | Guide |
|---|---|
| Serial Link at 200 Hz for a minute | [Link monitor](link-monitor.md#checks) |
| Attitude follows the board | [Link monitor](link-monitor.md#attitude) |
| Parameters survive a power cycle | [Link monitor](link-monitor.md#parameters) |
| Motor and encoder mapping | [Driving the wheels](driving.md#motor-bring-up) |
| Fault stop and hardware watchdog | [Driving the wheels](driving.md#fault-stop-check) |
