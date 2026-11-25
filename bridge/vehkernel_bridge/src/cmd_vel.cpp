// SPDX-License-Identifier: GPL-3.0-or-later
// /cmd_vel as a body velocity setpoint (ADR-0003, ADR-0014, spec 0015).
#include <vehkernel_bridge/cmd_vel.hpp>

#include <cmath>

namespace vk::bridge {

std::optional<vk::msg::BodyVelocitySetpoint> make_body_velocity_setpoint(double linear_x, double linear_y,
                                                                          double angular_z)
{
    if (!std::isfinite(linear_x) || !std::isfinite(linear_y) || !std::isfinite(angular_z)) {
        return std::nullopt;
    }
    vk::msg::BodyVelocitySetpoint setpoint{};
    setpoint.vx = static_cast<float>(linear_x);
    setpoint.vy = static_cast<float>(-linear_y);
    setpoint.yaw_rate = static_cast<float>(-angular_z);
    return setpoint;
}

std::optional<vk::msg::BodyVelocitySetpoint> make_body_velocity_setpoint(const geometry_msgs::msg::Twist& twist)
{
    return make_body_velocity_setpoint(twist.linear.x, twist.linear.y, twist.angular.z);
}

std::optional<vk::msg::BodyVelocitySetpoint> make_body_velocity_setpoint(const geometry_msgs::msg::TwistStamped& twist)
{
    return make_body_velocity_setpoint(twist.twist);
}

vk::msg::BodyVelocitySetpoint make_zero_setpoint()
{
    return vk::msg::BodyVelocitySetpoint{};
}

}  // namespace vk::bridge
