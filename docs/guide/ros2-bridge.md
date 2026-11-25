# ROS2 Bridge

The Bridge is a ROS2 node on the companion computer that connects one Link to ROS2 ([ADR-0027](../adr/0027-ros2-bridge.md), [spec 0015](../specs/0015-ros2-bridge.md)). It publishes the odometry, the TF, the IMU and the vehicle status, takes `/cmd_vel` in `COMPANION` mode, and offers services to arm, change the mode and stop in an emergency. It supports ROS2 Jazzy on Ubuntu 24.04, on amd64 and arm64, a Raspberry Pi 5 included. The board, or SITL, keeps running without it: the Bridge adds ROS2, it is not needed for the vehicle to be safe.

## Installing ROS2

Install ROS2 Jazzy from its apt repository, following [Installing ROS 2 Jazzy](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html) up to and including the package `ros-jazzy-ros-base` (the desktop packages are not needed), then the build tools:

```bash
sudo apt install ros-jazzy-ros-base python3-colcon-common-extensions python3-rosdep git
sudo rosdep init
rosdep update
```

`rosdep init` fails with a message that the file exists when the machine has run it before; that is fine.

## Building the Bridge

The Bridge's two packages, `vehkernel_msgs` and `vehkernel_bridge`, live in `bridge/` of this repository, and `vehkernel_bridge` builds against the repository's schema and Link code, so build them from a checkout of the whole repository, placed under `src/` of a colcon workspace. The submodules (`external/`) are for the firmware and are not needed. The repository's top level is a CMake project, which colcon would take for one package and stop at, so the commands point colcon at `bridge/` with `--base-paths`:

```bash
mkdir -p ~/vk_ws/src
cd ~/vk_ws/src
git clone https://github.com/chimu0510/VehKernel.git
cd ~/vk_ws
source /opt/ros/jazzy/setup.bash
rosdep install --from-paths src/VehKernel/bridge --ignore-src --rosdistro jazzy -y
colcon build --base-paths src/VehKernel/bridge --packages-up-to vehkernel_bridge
source install/setup.bash
```

The build runs `tools/generators/generate_schema.py` with the system's Python, so the firmware, the host tools and the Bridge always use the same message definitions. Source `install/setup.bash` in every new terminal before the commands below. The Bridge's tests run with `colcon test --base-paths src/VehKernel/bridge --packages-select vehkernel_bridge`; the L5 test needs a SITL build ([Testing](#testing)).

## Running against SITL

Start SITL in the development container ([SITL](sitl.md)), where its UDP Link is on `127.0.0.1:15600`:

```bash
cmake --preset sitl
cmake --build --preset sitl
build/sitl/app/vehkernel-sitl --duration 0
```

Run the Bridge on the same host, or in a ROS2 container started with `--network host`, since SITL's Link answers on the loopback address. The Bridge's default transport is UDP on `127.0.0.1:15600`:

```bash
ros2 run vehkernel_bridge bridge
```

```text
[INFO] [bridge]: Link over udp 127.0.0.1:15600
[INFO] [bridge]: handshake done: protocol version 5, 29 topics, 61 parameters
[INFO] [bridge]: no topic's schema hash differs
[INFO] [bridge]: time synchronization: MCU clock = Bridge clock + ... us, round trip 5960 us
```

In another terminal, check the outputs:

```bash
ros2 topic hz /odom
ros2 topic echo --once /vehkernel/vehicle_status
```

## Running against a board

Connect the board's USB port to the companion computer. It appears as a serial device, usually `/dev/ttyACM0`; `/dev/serial/by-id/` names it by its USB identity, which stays the same when other devices are plugged in before it. A user must be in the `dialout` group to open it; log out and in again afterwards:

```bash
sudo usermod -aG dialout $USER
ls -l /dev/serial/by-id/
```

Run the Bridge on the serial Link, at the Link's default 921600 baud:

```bash
ros2 run vehkernel_bridge bridge --ros-args -p transport:=serial -p serial_device:=/dev/ttyACM0
```

The Bridge reconnects on its own, with a growing wait between attempts, when the board reboots or the cable is replugged; the log says why the Link went down.

## Launch file

`launch/bridge.launch.py` starts the Bridge with its Link given as launch arguments:

```bash
ros2 launch vehkernel_bridge bridge.launch.py transport:=serial serial_device:=/dev/serial/by-id/<the board's name>
ros2 launch vehkernel_bridge bridge.launch.py transport:=udp udp_port:=15600
```

Its arguments are `transport`, `serial_device`, `serial_baud`, `udp_address`, `udp_port`, `publish_tf` and `cmd_vel_stamped`, the parameters of the same name below. For your own application, include it from your own launch file and start the rest beside it, or start the node directly with more parameters:

```python
from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        Node(package="vehkernel_bridge", executable="bridge", name="bridge", output="screen",
             parameters=[{"transport": "serial", "serial_device": "/dev/ttyACM0",
                          "publish_tf": False, "cmd_vel_stamped": False}]),
    ])
```

## Driving

The vehicle boots in `STANDBY`. A ROS2 node asks for `COMPANION`, which makes the Bridge's Link the source in control, then arms ([Driving the wheels](driving.md)):

```bash
ros2 service call /vehkernel/set_mode vehkernel_msgs/srv/SetMode "{mode_name: COMPANION}"
ros2 service call /vehkernel/arm std_srvs/srv/SetBool "{data: true}"
ros2 topic pub -r 10 /cmd_vel geometry_msgs/msg/TwistStamped "{twist: {linear: {x: 0.2}}}"
```

The wheels turn at the 0.2 m/s the message says, within the output cap `MOT_OUT_MAX` (30 % duty by default). `ros2 topic echo /odom` shows the vehicle moving forward. Each service answers with the MCU's reason when it refuses, for example that the emergency stop is engaged, or with a timeout when the board does not answer. `teleop_twist_keyboard` publishes `Twist`; run the Bridge with `cmd_vel_stamped:=false` for it. Nav2 on Jazzy publishes `TwistStamped`, the default.

Disarm with `{data: false}` on `/vehkernel/arm`. The remote control takes over from the Bridge at any time ([Remote control](driving.md#remote-control)).

## Topics, services and parameters

Every output is stamped with the MCU's sample time on the companion computer's clock, from the time synchronization the Bridge repeats while connected.

| Topic | Type | Direction | Contents |
|---|---|---|---|
| `/odom` | `nav_msgs/Odometry` | out | Pose and velocity of `base_link` in `odom`, with covariances from the estimator's variances; unknown entries are large |
| `/tf` | `tf2_msgs/TFMessage` | out | `odom` → `base_link` from the same data, unless `publish_tf` is false |
| `/imu/data` | `sensor_msgs/Imu` | out | The IMU's angular velocity and acceleration in `base_link`, the orientation of the latest attitude estimate, covariances from `EKF_GYR_NOISE` and `EKF_ACC_NOISE`; sensor data QoS (best effort) |
| `/vehkernel/vehicle_status` | `vehkernel_msgs/VehicleStatus` | out | Mode, armed, source in control, emergency stop, actuator power, arming refusal, disarm reason, reset cause |
| `/cmd_vel` | `geometry_msgs/TwistStamped`, or `Twist` | in | `linear.x` forward, `linear.y` left and `angular.z` counterclockwise, in `base_link` |

| Service | Type | Effect |
|---|---|---|
| `/vehkernel/arm` | `std_srvs/SetBool` | `data: true` arms, `false` disarms |
| `/vehkernel/set_mode` | `vehkernel_msgs/SetMode` | Requests a mode by `mode_name` (`STANDBY`, `TEST`, `MANUAL`, `COMPANION`) or by `mode_value`; the MCU accepts `COMPANION` only while disarmed |
| `/vehkernel/emergency_stop` | `std_srvs/SetBool` | `data: true` engages the emergency stop, `false` releases it ([Safety conditions](driving.md#safety-conditions)) |

| Parameter | Default | Meaning |
|---|---|---|
| `transport` | `udp` | `serial` for a board, `udp` for SITL |
| `serial_device`, `serial_baud` | `/dev/ttyACM0`, `921600` | The serial Link |
| `udp_address`, `udp_port` | `127.0.0.1`, `15600` | SITL's UDP Link |
| `poll_period_ms` | `2` | How often the Bridge reads the Link, in milliseconds |
| `odom_frame`, `base_frame` | `odom`, `base_link` | The frames of `/odom` and the TF; `/imu/data` is in `base_frame` |
| `publish_tf` | `true` | Set to `false` when another node publishes `odom` → `base_link`, a visual odometry node for example |
| `cmd_vel_stamped` | `true` | `/cmd_vel` is a `TwistStamped`; `false` for a `Twist` |
| `cmd_vel_timeout` | `0.25` | Seconds without `/cmd_vel` before the Bridge sends zero setpoints; it must stay well below `COM_SP_TIMEOUT`, and the Bridge warns when it does not |
| `service_timeout` | `2.0` | Seconds a service waits for the MCU's answer |
| `link_instance` | `-1` | This Link's number as the MCU names the source in control; `-1` learns it ([Silence and a lost Link](#silence-and-a-lost-link)) |

## Frames

The messages follow REP 103 and REP 105: east-north-up for the world, x forward, y left and z up for the body. The Bridge converts from the firmware's north-east-down and forward-right-down frames, so no node needs to. The MCU's heading is zero at boot: without the magnetometer the vehicle starts facing `odom`'s +y; with the magnetometer fused, `odom` is aligned to east and north ([Estimation](estimation.md)). `odom` drifts, as REP 105 says, and the Bridge does not publish `map`.

`/cmd_vel` maps `linear.x` to forward speed, `-linear.y` to the right speed and `-angular.z` to the yaw rate, which the MCU counts positive turning right. A vehicle that cannot move sideways, such as the C50C, ignores `linear.y`.

## Silence and a lost Link

- **`/cmd_vel` falls silent**, as when Nav2 reaches its goal: after `cmd_vel_timeout` the Bridge sends zero setpoints at 10 Hz, the vehicle stops and stays armed. Zero setpoints do not count as idle in `COMPANION`, so `COM_DISARM_IDLE` does not disarm it. An armed vehicle that has received no setpoint at all since arming disarms after `COM_DISARM_IDLE` (10 s).
- **The Bridge or its Link dies**: the setpoints stop, and after `COM_SP_TIMEOUT` (0.5 s) the vehicle stops and disarms, with the disarm reason `SETPOINT_TIMEOUT` in `/vehkernel/vehicle_status`. The Bridge does not re-arm after it reconnects; call the arm service again.
- **The Bridge does not know its own Link yet**: the MCU does not tell a host which Link it is on. The Bridge learns it from the first status after the MCU accepted its own request for `COMPANION`, or takes it from `link_instance`. Until then it sends no zero setpoints, so a Bridge started against a vehicle that another host armed cannot keep that vehicle armed.
- **A topic's schema hash differs** between the firmware and the Bridge: the topic is disabled and the log names it. Only a differing topic that the control path needs (`body_velocity_setpoint`, `vehicle_status`, `vehicle_local_odometry`) makes the Bridge refuse the connection, and the log then says which topic. Rebuild the Bridge from the firmware's commit to fix it.

## Testing

`colcon test --base-paths src/VehKernel/bridge --packages-select vehkernel_bridge` runs the unit tests and the L5 test `test_sitl_bridge.py`, a `launch_testing` test that starts SITL and the Bridge on UDP, and checks what this guide describes: the four outputs arrive, `COMPANION` and arming succeed, `/cmd_vel` moves `/odom` forward, a silence longer than `COM_DISARM_IDLE` keeps the vehicle armed and stopped, and killing the Bridge disarms SITL, which a second Bridge reads. SITL's UDP Link takes one peer, so the test drives everything through the first Bridge. It needs a SITL executable from the `sitl` preset, named by `VK_SITL_EXECUTABLE`; without it the test is skipped and the rest passes:

```bash
VK_SITL_EXECUTABLE=/path/to/build/sitl/app/vehkernel-sitl colcon test --base-paths src/VehKernel/bridge --packages-select vehkernel_bridge
colcon test-result --verbose
```

The test takes about 25 s, most of it the silence. CI runs it in `ros:jazzy-ros-base` on amd64 and arm64 with the SITL build of the development container's job, and gives a failed test a second attempt, as it does the SITL checks ([Testing](testing.md)). Hardware checks with the board are manual, in the ticket of spec 0015.
