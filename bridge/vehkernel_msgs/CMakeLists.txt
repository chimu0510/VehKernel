# SPDX-License-Identifier: GPL-3.0-or-later
# ROS2 messages and services of the Bridge (ADR-0027, spec 0015). The package
# starts empty: the skeleton ticket (#194) needs none, and the tickets that
# publish the vehicle status and offer the services add theirs here.
cmake_minimum_required(VERSION 3.22)
project(vehkernel_msgs)

find_package(ament_cmake REQUIRED)

ament_package()
