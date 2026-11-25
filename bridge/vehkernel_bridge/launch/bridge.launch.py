# SPDX-License-Identifier: GPL-3.0-or-later
# Starts the Bridge with its Link given as launch arguments (ADR-0027, spec 0015).
#
#     ros2 launch vehkernel_bridge bridge.launch.py transport:=serial serial_device:=/dev/ttyACM0
#     ros2 launch vehkernel_bridge bridge.launch.py transport:=udp udp_port:=15600
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue

# name, default, type, what it is; each is the Bridge parameter of the same name.
ARGUMENTS = (
    ("transport", "udp", str, "The Link: serial for a board, udp for SITL"),
    ("serial_device", "/dev/ttyACM0", str, "The board's serial device"),
    ("serial_baud", "921600", int, "The serial baud rate"),
    ("udp_address", "127.0.0.1", str, "SITL's address"),
    ("udp_port", "15600", int, "SITL's UDP port"),
    ("publish_tf", "true", bool, "Publish the TF odom to base_link"),
    ("cmd_vel_stamped", "true", bool, "/cmd_vel is a TwistStamped; false for a Twist"),
)


def generate_launch_description() -> LaunchDescription:
    declarations = [DeclareLaunchArgument(name, default_value=default, description=text)
                    for name, default, _, text in ARGUMENTS]
    parameters = {name: ParameterValue(LaunchConfiguration(name), value_type=kind)
                  for name, _, kind, _ in ARGUMENTS}
    bridge = Node(package="vehkernel_bridge", executable="bridge", name="bridge", output="screen",
                  parameters=[parameters])
    return LaunchDescription([*declarations, bridge])
