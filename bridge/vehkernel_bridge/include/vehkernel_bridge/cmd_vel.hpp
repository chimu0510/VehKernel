// SPDX-License-Identifier: GPL-3.0-or-later
// /cmd_vel as a body velocity setpoint (ADR-0003, ADR-0014, ADR-0027, spec 0015):
// a pure function from a twist to the message the MCU takes, ROS2's forward-left-up
// body frame (REP 103) to Vehkernel's forward-right-down one. Needs ROS2's message
// packages but no node, so a gtest covers it.
#pragma once

#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>

#include <vk/msg/body_velocity_setpoint.hpp>

#include <optional>

namespace vk::bridge {

// `linear.x` becomes `vx`, `-linear.y` becomes `vy` and `-angular.z` becomes
// `yaw_rate`. The timestamp and the source stay 0: the Link tags the source and
// the MCU stamps a setpoint when it arrives (the schema's comment, and what the
// host tools send). Empty when a field the vehicle uses is not finite: NaN would
// leave the vehicle uncontrolled and infinity is no velocity, so nothing is sent.
[[nodiscard]] std::optional<vk::msg::BodyVelocitySetpoint> make_body_velocity_setpoint(double linear_x, double linear_y,
                                                                                         double angular_z);
[[nodiscard]] std::optional<vk::msg::BodyVelocitySetpoint> make_body_velocity_setpoint(
    const geometry_msgs::msg::Twist& twist);
[[nodiscard]] std::optional<vk::msg::BodyVelocitySetpoint> make_body_velocity_setpoint(
    const geometry_msgs::msg::TwistStamped& twist);

// The zero setpoint of the idle rule: the vehicle stops and stays armed.
[[nodiscard]] vk::msg::BodyVelocitySetpoint make_zero_setpoint();

}  // namespace vk::bridge
