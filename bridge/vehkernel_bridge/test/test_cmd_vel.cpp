// SPDX-License-Identifier: GPL-3.0-or-later
// L1 tests of the /cmd_vel mapping (ADR-0003, ADR-0014, ADR-0018, spec 0015 seam D):
// REP 103's forward-left-up twist to the body velocity setpoint's forward-right-down.
#include <vehkernel_bridge/cmd_vel.hpp>

#include <gtest/gtest.h>

#include <limits>

namespace vk::bridge {
namespace {

TEST(CmdVel, MapsTwistToTheBodyFrame)
{
    geometry_msgs::msg::Twist twist;
    twist.linear.x = 0.4;
    twist.linear.y = 0.2;  // to the left
    twist.angular.z = 0.7;  // counter-clockwise from above
    const auto mapped = make_body_velocity_setpoint(twist);
    if (!mapped.has_value()) {
        FAIL() << "no setpoint";
    }
    const vk::msg::BodyVelocitySetpoint setpoint = *mapped;
    EXPECT_FLOAT_EQ(setpoint.vx, 0.4F);
    EXPECT_FLOAT_EQ(setpoint.vy, -0.2F);  // right is negative left
    EXPECT_FLOAT_EQ(setpoint.yaw_rate, -0.7F);  // about z down, positive turns right
    EXPECT_EQ(setpoint.timestamp, 0U);
    EXPECT_EQ(setpoint.source, 0U);
}

TEST(CmdVel, MapsTwistStampedTheSameAndIgnoresTheStampAndTheUnusedAxes)
{
    geometry_msgs::msg::TwistStamped twist;
    twist.header.stamp.sec = 12;
    twist.header.frame_id = "base_link";
    twist.twist.linear.x = -0.3;
    twist.twist.linear.y = -0.1;
    twist.twist.linear.z = 5.0;
    twist.twist.angular.x = 5.0;
    twist.twist.angular.y = 5.0;
    twist.twist.angular.z = -1.2;
    const auto mapped = make_body_velocity_setpoint(twist);
    if (!mapped.has_value()) {
        FAIL() << "no setpoint";
    }
    const vk::msg::BodyVelocitySetpoint setpoint = *mapped;
    EXPECT_FLOAT_EQ(setpoint.vx, -0.3F);
    EXPECT_FLOAT_EQ(setpoint.vy, 0.1F);
    EXPECT_FLOAT_EQ(setpoint.yaw_rate, 1.2F);
}

TEST(CmdVel, ATwistOfZerosIsAZeroSetpoint)
{
    const auto mapped = make_body_velocity_setpoint(geometry_msgs::msg::Twist{});
    if (!mapped.has_value()) {
        FAIL() << "no setpoint";
    }
    const vk::msg::BodyVelocitySetpoint setpoint = *mapped;
    EXPECT_EQ(setpoint.vx, 0.0F);
    EXPECT_EQ(setpoint.yaw_rate, 0.0F);  // -0 is zero too
    EXPECT_EQ(setpoint.vy, 0.0F);
}

TEST(CmdVel, ANonFiniteVelocityIsNotSent)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(make_body_velocity_setpoint(nan, 0.0, 0.0).has_value());
    EXPECT_FALSE(make_body_velocity_setpoint(0.0, inf, 0.0).has_value());
    EXPECT_FALSE(make_body_velocity_setpoint(0.0, 0.0, -inf).has_value());
    geometry_msgs::msg::TwistStamped twist;
    twist.twist.linear.x = nan;
    EXPECT_FALSE(make_body_velocity_setpoint(twist).has_value());
}

TEST(CmdVel, TheZeroSetpointIsAllZero)
{
    const auto setpoint = make_zero_setpoint();
    EXPECT_EQ(setpoint.vx, 0.0F);
    EXPECT_EQ(setpoint.vy, 0.0F);
    EXPECT_EQ(setpoint.yaw_rate, 0.0F);
}

}  // namespace
}  // namespace vk::bridge
