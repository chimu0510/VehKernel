# Spec 0015: the ROS2 Bridge, enough for Nav2 to drive

**Status**: done. Issue: [#191](https://github.com/chimu0510/VehKernel/issues/191); tickets #192 to #198. The hardware check in [#198](https://github.com/chimu0510/VehKernel/issues/198) confirmed it on 2026-11-26 on the maintainer's Raspberry Pi 5 with the C50C V1.1, with ROS2 Jazzy from apt; a power cycle of the board and `teleop_twist_keyboard` itself were not covered.

This spec is done and no longer edited; later decisions live in the ADRs and the code, which win where they differ.

## Problem Statement

Vehkernel estimates the attitude and the local odometry, closes the body velocity loop and guards the outputs, and the companion computer reaches all of it over a Link (specs 0003 to 0014). But nothing on the companion computer speaks ROS2:

- **ROS2 nodes cannot see the vehicle.** The odometry, the IMU, the attitude and the vehicle status reach only the Link monitor and the logs. Nav2, a SLAM node or RViz has nothing to subscribe to.
- **ROS2 nodes cannot drive the vehicle.** Body velocity setpoints come only from a Link tool in `TEST`, at low output power and with an idle disarm after 10 s, or from the remote control in `MANUAL`. No mode accepts setpoints from the companion computer as the one driving.
- **A schema change would take the Bridge offline.** The host refuses the whole connection on any differing schema hash. ADR-0006 already decides per-topic handling and `@required` "with the Bridge" (#83), but neither exists.

ADR-0003 decides the vehicle-level ROS2 API and ADR-0027 how the Bridge is built. This spec builds the part Nav2 needs: `/cmd_vel` in, `/odom` and TF out.

## Solution

Deliver the fifteenth milestone: **a ROS2 Jazzy Bridge on the companion computer that publishes the odometry, the TF, the IMU and the vehicle status, takes `/cmd_vel` in a new `COMPANION` mode, and offers arming, mode and emergency stop services**. Afterwards:

- `ros2 run vehkernel_bridge bridge` on the companion computer, with the board on a serial Link or SITL on UDP, publishes `/odom`, the TF `odom` → `base_link`, `/imu/data` and `/vehkernel/vehicle_status`.
- A ROS2 node arms the vehicle and requests `COMPANION` through services, then drives it with `/cmd_vel`; `teleop_twist_keyboard` and Nav2 work without adapters.
- When `/cmd_vel` falls silent the vehicle stops and stays armed; when the Bridge or its Link dies the vehicle stops and disarms.
- A topic whose schema hash differs is disabled and reported, and only a differing `@required` topic refuses the connection.
- CI builds the Bridge in the ROS2 image on amd64 and arm64 and runs it against SITL.

## User Stories

1. As a ROS2 developer, I want the vehicle's odometry on `/odom` and the TF `odom` → `base_link`, so that Nav2, SLAM and RViz work with no node of my own in between.
2. As a ROS2 developer, I want the IMU on `/imu/data` with its orientation and covariances, so that a SLAM node or `robot_localization` can fuse it.
3. As a ROS2 developer, I want every message in ROS2's frames and conventions (REP 103, REP 105), so that I do not convert north-east-down by hand.
4. As a ROS2 developer, I want message stamps to be the MCU's sample times on the companion computer's clock, so that TF lookups and sensor fusion line up with my other sensors.
5. As a ROS2 developer, I want to drive the vehicle with `/cmd_vel`, as `TwistStamped` by default and as `Twist` when configured, so that Nav2 on Jazzy and `teleop_twist_keyboard` both work.
6. As a ROS2 developer, I want services to arm and disarm, to change the mode and to engage and release the emergency stop, each answering with the reason when the MCU refuses, so that a launch file or an operator panel can bring the vehicle up.
7. As a ROS2 developer, I want the vehicle status (mode, armed, source in control, emergency stop, arming refusal, disarm reason) as a ROS2 message, so that my application can react to it.
8. As a vehicle operator, I want the vehicle to stop and stay armed when `/cmd_vel` falls silent, as when Nav2 reaches its goal, so that every new goal does not need a new arming.
9. As a vehicle operator, I want the vehicle to stop and disarm when the Bridge or its Link dies, so that a crashed companion computer cannot leave the wheels turning.
10. As a vehicle operator, I want the remote control to take over from the Bridge at any time, so that I keep the last word.
11. As a vehicle operator, I want the Bridge to keep working when the firmware and the Bridge differ in a diagnostic topic, so that updating one does not take the vehicle off ROS2.
12. As a vehicle operator, I want the Bridge to refuse to drive when a topic the control path needs differs, so that a mismatch can never send wrong setpoints.
13. As a vehicle operator, I want the Bridge to reconnect on its own after the board reboots or the cable is replugged, so that I do not restart ROS2.
14. As a vehicle operator, I want to build and run the Bridge on my Raspberry Pi 5 with ROS2 installed from apt, following one guide, so that it runs beside my other nodes.
15. As a developer, I want CI to build the Bridge and drive SITL through it, so that a change to the firmware, the schema or the Bridge cannot break ROS2 unnoticed.
16. As a developer, I want the Link monitor to be able to request `COMPANION` and send setpoints, so that I can test the mode without ROS2.

## Implementation Decisions

- **`COMPANION` mode** (ADR-0005), value 3 in `schema/enum/mode.enum`:
  - Armable; not entered while armed; requested by a Link, which becomes the source in control; accepts body velocity setpoints only. Direct-actuator setpoints in `COMPANION` and the attitude level wait for a vehicle that needs them (ADR-0003).
  - The output cap of `TEST` applies (ADR-0005, "Actuator output is capped by a parameter, low by default"), and so does the setpoint timeout `COM_SP_TIMEOUT`: a gap after the first setpoint stops and disarms.
  - Zero setpoints are not idle: `COM_DISARM_IDLE` does not disarm on zero setpoints in `COMPANION`, because the Bridge sends them while ROS2 is idle (below). No setpoint at all since arming still disarms after `COM_DISARM_IDLE`, so a vehicle armed and left without a source does not stay armed. A `ModeRules` field says so, as the other per-mode rules do.
  - The remote control's takeover and the emergency stop work as in every mode.
  - The Link monitor gains `COMPANION` in its mode request and the local display and the Link tools name it, all from the enum.
- **`@required`** (ADR-0006): a schema annotation on a message; the generator emits it into the C++ headers (`static constexpr bool required`) and the Python module. This spec marks `body_velocity_setpoint`, `vehicle_status` and `vehicle_local_odometry`. The Link monitor keeps its behaviour (stays connected, names the mismatches, fails `--check`), and adds whether each mismatch is required.
- **The Bridge** (ADR-0027) in `bridge/`, two colcon packages:
  - `vehkernel_msgs`: `VehicleStatus.msg` (the fields of `vehicle_status` that an application needs, with the enum values as constants), `SetMode.srv` (mode in, accepted and reason out), and the replies of arming and the emergency stop (`std_srvs/SetBool` where enough, own services where a reason is returned).
  - `vehkernel_bridge`: the node `bridge`. Its CMake runs `generate_schema.py` into its build tree and compiles `modules/link`'s COBS, CRC16 and frame sources with it.
  - **Transport**: `serial` (device, baud rate, default 921600) or `udp` (address and port, default `127.0.0.1:15600` for SITL), as ROS2 parameters of the node.
  - **Session**: the host side of the custom protocol in C++, offering the protocol versions the Python `HostSession` offers; the handshake; per-topic hash comparison with `@required`; time synchronization repeated as `link_monitor.py` does; the MCU's peer timeout; reconnection with backoff after a timeout, a reboot or a lost device.
  - **Outputs**, each stamped with the MCU sample time plus the time synchronization offset:
    - `/odom` (`nav_msgs/Odometry`) from `vehicle_local_odometry`: frame `odom`, child `base_link` (names are parameters), position and orientation from north-east-down to east-north-up, body velocity and yaw rate from forward-right-down to forward-left-up, covariances from its variances, unknown entries large, and the north-east correlation trimmed to within the square root of the two variances' product, so that rounding in the MCU's float32 matrix cannot make it lose positive semidefiniteness (#198).
    - TF `odom` → `base_link` from the same message, unless the parameter `publish_tf` is false (for a VIO node that publishes it, ADR-0014).
    - `/imu/data` (`sensor_msgs/Imu`) from `imu_sample` instance 0, with the orientation of the latest `vehicle_attitude` and its variances; angular velocity and linear acceleration covariances from `EKF_GYR_NOISE` and `EKF_ACC_NOISE` squared, read with the parameter service at each connection.
    - `/vehkernel/vehicle_status` (`vehkernel_msgs/VehicleStatus`) on every `vehicle_status` received.
    - The frame conversion is one header of pure functions with unit tests (a quaternion, a vector and a covariance each way).
  - **Inputs**:
    - `/cmd_vel`: `geometry_msgs/TwistStamped` by default, `geometry_msgs/Twist` when `cmd_vel_stamped` is false. `linear.x` → `vx`, `-linear.y` → `vy`, `-angular.z` → `yaw_rate`, sent as `body_velocity_setpoint`.
    - **Zero setpoints while idle**: when no `/cmd_vel` arrives for `cmd_vel_timeout` (default 0.25 s), the Bridge sends zero setpoints at 10 Hz, as long as the vehicle is armed in `COMPANION` with this Link in control. Silence from ROS2 then stops the vehicle and keeps it armed; silence from the Bridge or the Link trips `COM_SP_TIMEOUT` on the MCU. `cmd_vel_timeout` must stay well below `COM_SP_TIMEOUT` (0.5 s by default), or the first zero setpoint arrives after the MCU has already timed out; the Bridge reads `COM_SP_TIMEOUT` at every connection and warns when it does not.
    - **This Link in control**: the MCU does not tell a host which Link instance it is on. The Bridge learns it as the source in control of the first `vehicle_status` after the MCU accepted its own request for `COMPANION` on this connection, or takes it from the `link_instance` parameter. Until it knows, it sends no zero setpoints, so a Bridge started against a vehicle another host armed does not keep that vehicle armed.
    - Services under `/vehkernel/`: `arm` (`std_srvs/SetBool`), `set_mode` (`vehkernel_msgs/SetMode`, by name or value), `emergency_stop` (`std_srvs/SetBool`). Each waits for the acknowledgement and returns the MCU's reason text when refused, or a timeout.
  - **Not forwarded**: setpoints while a required topic differs; the Bridge then refuses the connection and says which topic.
- **Build and run on the companion computer**: ROS2 Jazzy from its apt repository on Ubuntu 24.04, a colcon workspace with the repository checked out under `src/`, `colcon build --base-paths src/VehKernel/bridge --packages-up-to vehkernel_bridge` (the repository's top level is a CMake project, which colcon takes for one package and does not descend into). A guide, `docs/guide/ros2-bridge.md`, covers it, with the serial device's permissions and a launch file example.
- **CI**: a job per architecture in `ros:jazzy-ros-base` by digest, building `bridge/` with colcon and running its unit tests and the SITL test below, with SITL built in the development container's job and passed as an artifact.

## Testing Decisions

- **Seam A: the mode (L1).** Unit tests of `mode_rules` and the commander: `COMPANION` arms from a Link, refuses entering while armed, accepts body velocity only, drops direct-actuator setpoints, does not idle-disarm on zero setpoints but does with no setpoint since arming, keeps the setpoint timeout, and yields to the remote control.
- **Seam B: `COMPANION` in SITL (L3).** A SITL check through `vklink`: request `COMPANION`, arm, send body velocity setpoints and see the wheels move; send zeros for longer than `COM_DISARM_IDLE` and stay armed; stop sending and see stop and disarm after `COM_SP_TIMEOUT`.
- **Seam C: `@required` (L0, L1).** Generator tests with expected output for a required and an ordinary message; a host tool test that a mismatch of a required topic is flagged as required.
- **Seam D: the Bridge's pieces (L1, gtest in colcon).** The frame conversion functions; the session against frames captured from SITL or built with the generated encoders (handshake, a disabled topic, a refused required topic, time synchronization, reconnection); `/cmd_vel` mapping and the zero-setpoint timer with a fake clock.
- **Seam E: SITL and the Bridge (L5, launch_testing).** SITL with the built-in simulator, the Bridge on UDP: `/odom`, TF, `/imu/data` and `/vehkernel/vehicle_status` arrive at their rates; `set_mode` `COMPANION` and `arm` succeed; `/cmd_vel` forward moves `/odom` forward in `odom`'s x or y as the heading says; silence keeps it armed and stopped; killing the Bridge disarms SITL.
- **Seam F: hardware (L4, manual, `ready-for-human`).** On the maintainer's Raspberry Pi 5 with the C50C V1.1, ROS2 Jazzy from apt: build with the guide; topics at their rates with the board on the bench, `/odom` and `/imu/data` in RViz turning the right way as the board is turned; wheels lifted, arm and drive with `teleop_twist_keyboard`, release, check the stop, unplug the Link and check the disarm, take over with the remote control; on the ground, a short drive with `teleop_twist_keyboard` and the odometry in RViz. Results are recorded on the ticket.
- **Prior art**: `test/sitl/check_link.py` and the other SITL checks for driving SITL over `vklink`; the commander's unit tests for mode rules; the generator tests' expected files.

## Out of Scope

- Nav2 itself, maps and localization, and Nav2 in CI: the next spec, once this API exists.
- MCU parameters as ROS2 parameters (ADR-0012), beyond reading the IMU noise at connection.
- External pose and body velocity measurement inputs (ADR-0014) and the external estimator.
- Direct-actuator and attitude setpoint levels in ROS2, and ros2_control (ADR-0003).
- Log streaming through the Bridge and alignment with rosbag2 (ADR-0019).
- Battery and health topics: the MCU publishes no battery measurement yet.
- `SERIALn_FUNCTION`, and restricting `COMPANION` to the Bridge's port (ADR-0027).
- A container for the Bridge on the companion computer, and ROS2 releases other than Jazzy.
- XRCE-DDS and MAVLink protocols (ADR-0002).

## Further Notes

- North-east-down to east-north-up keeps the MCU's heading zero at boot: without the magnetometer the vehicle starts facing `odom`'s +y (north in east-north-up). REP 105 does not fix the start heading, and Nav2 does not need one; with the magnetometer fused `odom` is aligned to east and north.
- `COMPANION` is the first mode where the companion computer drives at full trust; the output cap stays until a vehicle's hardware check says otherwise.
- #83 closes with this spec.
