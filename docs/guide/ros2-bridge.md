# ROS2 Bridge

The Bridge is a ROS2 node on the companion computer that connects one Link to ROS2 ([ADR-0027](../adr/0027-ros2-bridge.md), [spec 0015](../specs/0015-ros2-bridge.md)). It publishes the odometry, the TF, the IMU and the vehicle status, takes `/cmd_vel` in `COMPANION` mode, and offers services to arm, change the mode and stop in an emergency. It supports ROS2 Jazzy on Ubuntu 24.04, on amd64 and arm64, a Raspberry Pi 5 included. The board, or SITL, keeps running without it: the Bridge adds ROS2, it is not needed for the vehicle to be safe.

## Installing ROS2

On a fresh Ubuntu 24.04 companion computer (`amd64` or `arm64`, including Raspberry Pi 5), the automated path configures the ROS2 Jazzy apt repository, installs `build-essential`, ROS2 Jazzy, `colcon`, `rosdep`, serial permissions, and builds the Bridge in one command ([Environment setup](setup.md)):

```bash
tools/setup/vk_setup.py --role companion --yes --workspace ~/vk_ws --build-bridge
tools/setup/vk_setup.py --role companion --workspace ~/vk_ws --build-bridge --check
```

To install manually instead, configure the ROS2 Jazzy apt repository following [Installing ROS 2 Jazzy](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html) up to and including `ros-jazzy-ros-base` (the desktop packages are not needed), then install the C++ toolchain and build tools:

```bash
sudo apt install build-essential ros-jazzy-ros-base python3-colcon-common-extensions python3-rosdep git
sudo rosdep init
rosdep update
```

`build-essential` provides `g++` and `make`, which are absent from a fresh Ubuntu 24.04 Server or Raspberry Pi image and required by `colcon build`. `rosdep init` fails with a message that the file exists when the machine has run it before; that is fine. If you plan to build the Bridge with clang-tidy enabled (`--cmake-args -DVK_CLANG_TIDY=ON`), also install `clang-tidy` (`sudo apt install clang-tidy`).

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

The build runs `tools/generators/generate_schema.py` with the system's Python, so the firmware, the host tools and the Bridge always use the same message definitions. To run clang-tidy on the Bridge's C++ sources (requires `sudo apt install clang-tidy`), pass `--cmake-args -DVK_CLANG_TIDY=ON` to `colcon build`. Source `install/setup.bash` in every new terminal before the commands below. The Bridge's tests run with `colcon test --base-paths src/VehKernel/bridge --packages-select vehkernel_bridge`; the L5 test needs a SITL build ([Testing](#testing)).

## Running against SITL

Start SITL in the development container ([SITL](sitl.md); prefix with `vk` when using Docker), where its UDP Link is on `127.0.0.1:15600`:

```bash
vk cmake --preset sitl
vk cmake --build --preset sitl
vk build/sitl/app/vehkernel-sitl --duration 0
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

Connect the board's USB port to the companion computer. Ensure your user is in the `dialout` group (log out and back in afterwards), install [`tools/setup/udev/99-vehkernel.rules`](../../tools/setup/udev/99-vehkernel.rules), and keep `ModemManager` disabled and `brltty` removed so neither probes the board's USB serial adapters or toggles DTR/RTS on a download port (`tools/setup/vk_setup.py --role companion` configures and checks all of these):

```bash
sudo usermod -aG dialout "$USER"
sudo cp tools/setup/udev/99-vehkernel.rules /etc/udev/rules.d/99-vehkernel.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo systemctl disable --now ModemManager.service
sudo apt purge brltty
```

Each serial adapter appears as `/dev/ttyACM0`, `/dev/ttyACM1`, etc. in plug-in order. On the C50C V1.1, both CH9102F adapters (`SERIAL1` and `SERIAL2`) share the same `/dev/serial/by-id/` entry (`usb-WCH.CN_USB_Single_Serial_0002-if00`), so `/dev/serial/by-id/` cannot distinguish them ([C50C V1.1 serial ports](../boards/c50c-v1_1.md#serial-ports)). For a stable device path across reboots and replugs, use `/dev/serial/by-path/` or pin `/dev/vehkernel-link` by physical USB port using the commented `ENV{ID_PATH}` or `KERNELS` template in [`tools/setup/udev/99-vehkernel.rules`](../../tools/setup/udev/99-vehkernel.rules):

```bash
ls -l /dev/serial/by-path/
```

Run the Bridge on the serial Link, at the Link's default 921600 baud:

```bash
ros2 run vehkernel_bridge bridge --ros-args -p transport:=serial -p serial_device:=/dev/ttyACM0
```

The Bridge reconnects on its own, with a growing wait between attempts, when the board reboots or the cable is replugged; the log says why the Link went down.

## Running the Bridge as a user service

To start the Bridge automatically at boot and manage it without `sudo`, install the optional user `systemd` unit [`tools/setup/systemd/vehkernel-bridge.service`](../../tools/setup/systemd/vehkernel-bridge.service) ([ADR-0027](../adr/0027-ros2-bridge.md), [ADR-0028](../adr/0028-host-environment-setup.md)):

```bash
tools/setup/vk_setup.py --role companion --install-bridge-service \
    --workspace ~/vk_ws --device /dev/vehkernel-link --baud 921600 --ros-domain-id 0
```

This installs `~/.config/systemd/user/vehkernel-bridge.service`, creates `~/.config/vehkernel/bridge.env` if absent (preserving existing edits; see [`tools/setup/systemd/bridge.env.example`](../../tools/setup/systemd/bridge.env.example)), runs `systemctl --user enable vehkernel-bridge.service`, and enables lingering (`sudo loginctl enable-linger "$USER"`) so the user service starts at boot without an interactive login. Edit `~/.config/vehkernel/bridge.env` to change `VK_BRIDGE_WORKSPACE` (must be an absolute path; `systemd` `EnvironmentFile` does not expand `~`), `VK_BRIDGE_DEVICE`, `VK_BRIDGE_BAUD`, or `ROS_DOMAIN_ID`.

Start, inspect, restart, and view logs for the service:

```bash
systemctl --user start vehkernel-bridge.service
systemctl --user status vehkernel-bridge.service
systemctl --user restart vehkernel-bridge.service
journalctl --user -u vehkernel-bridge.service -f
```

Because `vehkernel-bridge.service` is a user unit, [`tools/flash/deploy.py`](../../tools/flash/deploy.py) automatically detects an active service over non-interactive SSH, stops it before serial flashing, and restarts it afterwards (even if flashing fails) without requiring `sudo` ([Flashing](flashing.md#from-the-development-host-in-one-command)).

## Viewing in RViz

RViz runs on an Ubuntu 24.04 desktop machine on the same network, such as the development host, rather than on the companion computer. Install RViz2, the IMU display plugin, and `teleop_twist_keyboard` with `vk_setup.py --role desktop` (which also checks `ROS_DOMAIN_ID`, `ufw`, and NTP synchronization; [Environment setup](setup.md)):

```bash
tools/setup/vk_setup.py --role desktop --yes
tools/setup/vk_setup.py --role desktop --check
```

Or install the packages manually after configuring the ROS2 Jazzy apt repository:

```bash
sudo apt install ros-jazzy-rviz2 ros-jazzy-rviz-imu-plugin ros-jazzy-teleop-twist-keyboard
```

Check that the Bridge's topics arrive on the desktop machine:

```bash
echo ${ROS_DOMAIN_ID:-0}
ros2 topic list
```

The topics should include `/odom`, `/imu/data` and `/vehkernel/vehicle_status`. If topics do not appear or RViz drops TF/messages, check:

- **`ROS_DOMAIN_ID`**: must match on both machines (unset defaults to `0`). A shell startup file such as `~/.bashrc` applies to interactive shells, whereas `vehkernel-bridge.service` reads `ROS_DOMAIN_ID` from `~/.config/vehkernel/bridge.env` and a Bridge launched over non-interactive SSH does not source `~/.bashrc`.
- **Firewall (`ufw`)**: if `sudo ufw status` is active on either machine, allow ROS2 DDS UDP traffic (`sudo ufw allow 7400:7500/udp`).
- **Multicast and network**: both computers must be on the same subnet with UDP multicast allowed (check that Wi-Fi AP/client isolation is disabled).
- **Clock synchronization**: synchronize both clocks first ([Clock synchronization](companion-computer.md#clock-synchronization)); verify with `tools/setup/ntp_offset.py pool.ntp.org`. RViz drops TF and messages stamped too far from its own clock.

The package installs an RViz config, which shows the TF with `base_link`'s axes, `/odom` as a trail of axes, and a grid, with the view following `base_link`:

```bash
rviz2 -d $(ros2 pkg prefix vehkernel_bridge)/share/vehkernel_bridge/rviz/bridge.rviz
```

On a desktop machine where the Bridge is not built, open the same file from a checkout: `rviz2 -d bridge/vehkernel_bridge/rviz/bridge.rviz`. Scrolling zooms and dragging orbits around `base_link`. A config of your own needs RViz's default tools (`Interact` and `MoveCamera`) for the mouse to move the view, and a view whose target frame is `base_link` to keep the vehicle in sight: wheel odometry counts wheels turned while lifted as travel, so after a test on the bench the vehicle starts far from `odom`'s origin.

In the config, turning the board turns `base_link`'s axes: red is forward, green left and blue up. Yaw left is positive about up, nose up is negative pitch about left, and left side up is positive roll about forward. RViz on Jazzy has no display for `sensor_msgs/Imu` of its own. To see `/imu/data` itself, install `ros-jazzy-rviz-imu-plugin` (included in `--role desktop`), add its `Imu` display, and set the topic's reliability to best effort. Its orientation is the one `/odom` carries.

## Launch file

`launch/bridge.launch.py` starts the Bridge with its Link given as launch arguments:

```bash
ros2 launch vehkernel_bridge bridge.launch.py transport:=serial serial_device:=/dev/vehkernel-link
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

The wheels turn at the 0.2 m/s the message says, within the output cap `MOT_OUT_MAX` (30 % duty by default). `ros2 topic echo /odom` shows the vehicle moving forward. Each service answers with the MCU's reason when it refuses, for example that the emergency stop is engaged, or with a timeout when the board does not answer.

To drive with a keyboard from a desktop machine, install `ros-jazzy-teleop-twist-keyboard` (`sudo apt install ros-jazzy-teleop-twist-keyboard`, also installed by `vk_setup.py --role desktop`) and start the Bridge with `cmd_vel_stamped:=false` (`teleop_twist_keyboard` publishes `Twist`, whereas Nav2 on Jazzy publishes `TwistStamped`, the default):

```bash
ros2 run teleop_twist_keyboard teleop_twist_keyboard
```

Disarm with `{data: false}` on `/vehkernel/arm`. The remote control takes over from the Bridge at any time ([Remote control](driving.md#remote-control)).

## Topics, services and parameters

Every output is stamped with the MCU's sample time on the companion computer's clock, from the time synchronization the Bridge repeats while connected. When other computers, such as a development host running RViz, take part in the same ROS2 system, synchronize their clocks with the companion computer's first ([Clock synchronization](companion-computer.md#clock-synchronization)).

| Topic | Type | Direction | Contents |
|---|---|---|---|
| `/odom` | `nav_msgs/Odometry` | out | Pose and velocity of `base_link` in `odom`, with covariances from the estimator's variances; unknown entries are large, and the horizontal position's is kept positive semidefinite |
| `/tf` | `tf2_msgs/TFMessage` | out | `odom` → `base_link` from the same data, unless `publish_tf` is false |
| `/imu/data` | `sensor_msgs/Imu` | out | The IMU's angular velocity and acceleration in `base_link`, the orientation of the latest attitude estimate, covariances from `EKF_GYR_NOISE` and `EKF_ACC_NOISE`; sensor data QoS (best effort) |
| `/vehkernel/vehicle_status` | `vehkernel_msgs/VehicleStatus` | out | Mode, armed, source in control, emergency stop, actuator power, arming refusal, disarm reason, reset cause |
| `/vehkernel/firmware_info` | `vehkernel_msgs/FirmwareInfo` | out | The firmware's version, commit, dirty flag and board ID (ADR-0023), published at each connection and when it changes, transient local; the Bridge also logs it |
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
