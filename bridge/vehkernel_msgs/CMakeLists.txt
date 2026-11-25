# SPDX-License-Identifier: GPL-3.0-or-later
# ROS2 messages and services of the Bridge (ADR-0027, spec 0015).
cmake_minimum_required(VERSION 3.22)
project(vehkernel_msgs)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_msgs REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/VehicleStatus.msg"
  "srv/SetMode.srv"
  DEPENDENCIES std_msgs)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
