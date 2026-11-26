// SPDX-License-Identifier: GPL-3.0-or-later
// L1 tests of the Bridge's outputs (ADR-0014, ADR-0018, spec 0015 seam D): the
// ROS2 messages built from generated messages, in ROS2's frames, with their
// covariances and stamps, and the VehicleStatus constants against the schema's enums.
#include <vehkernel_bridge/outputs.hpp>

#include <vk/msg/enums.hpp>

#include <gtest/gtest.h>

#include <cmath>

namespace vk::bridge {
namespace {

using vehkernel_msgs::msg::VehicleStatus;

constexpr double tolerance = 1e-6;
constexpr double h = 0.70710678118654752440;

// The vehicle at 3 m north and 2 m east, facing east, moving forward and turning right at 0.1 rad/s.
vk::msg::VehicleLocalOdometry make_sample()
{
    vk::msg::VehicleLocalOdometry m;
    m.timestamp = 2'000'000;
    m.timestamp_sample = 1'999'000;
    m.position = {3.0F, 2.0F, 0.0F};
    m.q = {static_cast<float>(h), 0.0F, 0.0F, static_cast<float>(h)};  // yaw +90 degrees about down: east
    m.velocity = {0.5F, 0.1F, 0.0F};
    m.yaw_rate = 0.1F;  // about down: clockwise seen from above
    m.position_variance = {0.04F, 0.01F, 0.09F};  // north-north, north-east, east-east
    m.heading_variance = 0.02F;
    m.velocity_variance = {0.001F, 0.002F, 0.003F};
    return m;
}

TEST(Outputs, OdometryIsInRosFrames)
{
    const auto odom = make_odometry(make_sample(), 1'700'000'000'123'456'789, FrameNames{});
    EXPECT_EQ(odom.header.frame_id, "odom");
    EXPECT_EQ(odom.child_frame_id, "base_link");
    EXPECT_EQ(odom.header.stamp.sec, 1'700'000'000);
    EXPECT_EQ(odom.header.stamp.nanosec, 123'456'789U);
    // North 3, east 2 is x = 2 (east), y = 3 (north).
    EXPECT_NEAR(odom.pose.pose.position.x, 2.0, tolerance);
    EXPECT_NEAR(odom.pose.pose.position.y, 3.0, tolerance);
    EXPECT_NEAR(odom.pose.pose.position.z, 0.0, tolerance);
    // Facing east is ENU's x: no rotation.
    EXPECT_NEAR(odom.pose.pose.orientation.w, 1.0, tolerance);
    EXPECT_NEAR(odom.pose.pose.orientation.x, 0.0, tolerance);
    EXPECT_NEAR(odom.pose.pose.orientation.y, 0.0, tolerance);
    EXPECT_NEAR(odom.pose.pose.orientation.z, 0.0, tolerance);
    // Forward-left-up: right 0.1 is left -0.1; a turn to the right is a negative rate about up.
    EXPECT_NEAR(odom.twist.twist.linear.x, 0.5, tolerance);
    EXPECT_NEAR(odom.twist.twist.linear.y, -0.1, tolerance);
    EXPECT_NEAR(odom.twist.twist.angular.z, -0.1, tolerance);
}

TEST(Outputs, OdometryCovariancesAreRotatedAndUnknownEntriesAreLarge)
{
    const auto odom = make_odometry(make_sample(), 0, FrameNames{});
    const auto& pose = odom.pose.covariance;
    EXPECT_NEAR(pose[0], 0.09, tolerance);   // x is east: east-east
    EXPECT_NEAR(pose[1], 0.01, tolerance);   // x-y: north-east
    EXPECT_NEAR(pose[6], 0.01, tolerance);
    EXPECT_NEAR(pose[7], 0.04, tolerance);   // y is north: north-north
    EXPECT_DOUBLE_EQ(pose[14], unknown_variance);  // z: down is not estimated
    EXPECT_DOUBLE_EQ(pose[21], unknown_variance);  // roll
    EXPECT_DOUBLE_EQ(pose[28], unknown_variance);  // pitch
    EXPECT_NEAR(pose[35], 0.02, tolerance);  // yaw
    const auto& twist = odom.twist.covariance;
    EXPECT_NEAR(twist[0], 0.001, tolerance);
    EXPECT_NEAR(twist[7], 0.002, tolerance);
    EXPECT_DOUBLE_EQ(twist[14], unknown_variance);
    EXPECT_DOUBLE_EQ(twist[21], unknown_variance);
    EXPECT_DOUBLE_EQ(twist[28], unknown_variance);
    EXPECT_NEAR(twist[35], 0.003, tolerance);
    // Symmetric, with the cross terms between position and orientation at zero.
    for (std::size_t row = 0; row < 6; ++row) {
        for (std::size_t column = 0; column < 6; ++column) {
            EXPECT_DOUBLE_EQ(pose[row * 6 + column], pose[column * 6 + row]);
        }
    }
    EXPECT_DOUBLE_EQ(pose[3], 0.0);
}

TEST(Outputs, NotANumberVarianceIsUnknown)
{
    auto sample = make_sample();
    sample.heading_variance = std::nanf("");
    sample.position_variance = {std::nanf(""), std::nanf(""), 0.09F};
    const auto odom = make_odometry(sample, 0, FrameNames{});
    EXPECT_DOUBLE_EQ(odom.pose.covariance[35], unknown_variance);
    EXPECT_DOUBLE_EQ(odom.pose.covariance[7], unknown_variance);
    EXPECT_DOUBLE_EQ(odom.pose.covariance[1], 0.0);
}

// The horizontal position block's determinant, east-east times north-north
// less the correlation squared; below zero, the matrix has a negative eigenvalue.
double horizontal_determinant(const nav_msgs::msg::Odometry& odom)
{
    const auto& pose = odom.pose.covariance;
    return pose[0] * pose[7] - pose[1] * pose[6];
}

TEST(Outputs, ANearlySingularPositionCovarianceStaysPositiveSemidefinite)
{
    // Read from a C50C at rest (#198): in float32, the north-east correlation
    // squared exceeds north-north times east-east by rounding.
    auto sample = make_sample();
    sample.position_variance = {3.6578e-6F, 1.2597e-6F, 4.3353e-7F};
    const auto odom = make_odometry(sample, 0, FrameNames{});
    EXPECT_GE(horizontal_determinant(odom), 0.0);
    EXPECT_DOUBLE_EQ(odom.pose.covariance[1], odom.pose.covariance[6]);
    EXPECT_NEAR(odom.pose.covariance[1], 1.2593e-6, 1e-9);  // the correlation, only trimmed

    sample.position_variance = {0.04F, -0.5F, 0.09F};  // far outside: trimmed to -sqrt(0.04 * 0.09)
    const auto negative = make_odometry(sample, 0, FrameNames{});
    EXPECT_GE(horizontal_determinant(negative), 0.0);
    EXPECT_NEAR(negative.pose.covariance[1], -0.06, 1e-9);
}

TEST(Outputs, FrameNamesAreParameters)
{
    const auto odom = make_odometry(make_sample(), 0, FrameNames{"map", "chassis"});
    EXPECT_EQ(odom.header.frame_id, "map");
    EXPECT_EQ(odom.child_frame_id, "chassis");
    const auto tf = make_transform(make_sample(), 0, FrameNames{"map", "chassis"});
    EXPECT_EQ(tf.header.frame_id, "map");
    EXPECT_EQ(tf.child_frame_id, "chassis");
}

TEST(Outputs, TransformMatchesTheOdometryPose)
{
    auto sample = make_sample();
    sample.q = {1.0F, 0.0F, 0.0F, 0.0F};  // facing north: +y of ENU
    const auto odom = make_odometry(sample, 42, FrameNames{});
    const auto tf = make_transform(sample, 42, FrameNames{});
    EXPECT_EQ(tf.header.stamp, odom.header.stamp);
    EXPECT_DOUBLE_EQ(tf.transform.translation.x, odom.pose.pose.position.x);
    EXPECT_DOUBLE_EQ(tf.transform.translation.y, odom.pose.pose.position.y);
    EXPECT_NEAR(tf.transform.rotation.w, h, tolerance);
    EXPECT_NEAR(tf.transform.rotation.z, h, tolerance);
}

TEST(Outputs, ImuInFluWithTheAttitudeAndTheNoiseFromTheParameters)
{
    vk::msg::ImuSample sample;
    sample.timestamp = 1'000'000;
    sample.accel = {0.1F, 0.2F, -9.8F};  // at rest: the specific force points up, which is -z of FRD
    sample.gyro = {0.01F, 0.02F, 0.03F};
    vk::msg::VehicleAttitude attitude;
    attitude.q = {1.0F, 0.0F, 0.0F, 0.0F};
    attitude.variance = {0.001F, 0.002F, 0.004F};  // roll, pitch, yaw

    const auto imu = make_imu(sample, attitude, ImuNoise{0.0011F, 2.0F}, 5, "base_link");
    EXPECT_EQ(imu.header.frame_id, "base_link");
    EXPECT_NEAR(imu.linear_acceleration.x, 0.1, tolerance);
    EXPECT_NEAR(imu.linear_acceleration.y, -0.2, tolerance);
    EXPECT_NEAR(imu.linear_acceleration.z, 9.8, tolerance);
    EXPECT_NEAR(imu.angular_velocity.x, 0.01, tolerance);
    EXPECT_NEAR(imu.angular_velocity.y, -0.02, tolerance);
    EXPECT_NEAR(imu.angular_velocity.z, -0.03, tolerance);
    EXPECT_NEAR(imu.orientation.w, h, tolerance);  // facing north is +y of ENU
    EXPECT_NEAR(imu.orientation.z, h, tolerance);
    // Rotations about east (x), north (y) and up (z): pitch, roll, yaw.
    EXPECT_NEAR(imu.orientation_covariance[0], 0.002, tolerance);
    EXPECT_NEAR(imu.orientation_covariance[4], 0.001, tolerance);
    EXPECT_NEAR(imu.orientation_covariance[8], 0.004, tolerance);
    EXPECT_NEAR(imu.angular_velocity_covariance[0], 0.0011 * 0.0011, 1e-12);
    EXPECT_NEAR(imu.angular_velocity_covariance[8], 0.0011 * 0.0011, 1e-12);
    EXPECT_NEAR(imu.linear_acceleration_covariance[4], 4.0, 1e-9);
    EXPECT_DOUBLE_EQ(imu.angular_velocity_covariance[1], 0.0);
}

TEST(Outputs, ImuWithoutTheNoiseOrTheAttitudeSaysUnknown)
{
    const vk::msg::ImuSample sample;
    const auto imu = make_imu(sample, std::nullopt, ImuNoise{}, 0, "base_link");
    // All zero is "unknown" for the rates and the acceleration, -1 at the first entry for the orientation.
    for (const double value : imu.angular_velocity_covariance) {
        EXPECT_DOUBLE_EQ(value, 0.0);
    }
    for (const double value : imu.linear_acceleration_covariance) {
        EXPECT_DOUBLE_EQ(value, 0.0);
    }
    EXPECT_DOUBLE_EQ(imu.orientation_covariance[0], -1.0);
}

TEST(Outputs, VehicleStatusCarriesItsFields)
{
    vk::msg::VehicleStatus status;
    status.timestamp = 7;
    status.mode = static_cast<std::uint8_t>(vk::msg::Mode::Companion);
    status.armed = true;
    status.source = 2;
    status.emergency_stop = true;
    status.actuator_power = static_cast<std::uint8_t>(vk::msg::ActuatorPower::Powered);
    status.arming_refusal = static_cast<std::uint8_t>(vk::msg::ArmingResult::WrongMode);
    status.disarm_reason = static_cast<std::uint8_t>(vk::msg::DisarmReason::SetpointTimeout);
    status.reset_cause = static_cast<std::uint8_t>(vk::msg::ResetCause::Watchdog);
    status.setpoints_rejected = 9;
    const auto out = make_vehicle_status(status, 1'000'000'500);
    EXPECT_EQ(out.header.stamp.sec, 1);
    EXPECT_EQ(out.header.stamp.nanosec, 500U);
    EXPECT_EQ(out.mode, VehicleStatus::MODE_COMPANION);
    EXPECT_TRUE(out.armed);
    EXPECT_EQ(out.source, 2);
    EXPECT_TRUE(out.emergency_stop);
    EXPECT_EQ(out.actuator_power, VehicleStatus::ACTUATOR_POWER_POWERED);
    EXPECT_EQ(out.arming_refusal, VehicleStatus::ARMING_WRONG_MODE);
    EXPECT_EQ(out.disarm_reason, VehicleStatus::DISARM_SETPOINT_TIMEOUT);
    EXPECT_EQ(out.reset_cause, VehicleStatus::RESET_WATCHDOG);
    EXPECT_EQ(out.setpoints_rejected, 9U);
}

// The constants of VehicleStatus.msg are hand-written; these compare them with the
// generated enums, so a change of the schema's enums fails here until the message follows.
template <class E>
constexpr std::uint8_t value(E e)
{
    return static_cast<std::uint8_t>(e);
}

using vk::msg::ActuatorPower;
using vk::msg::ArmingResult;
using vk::msg::DisarmReason;
using vk::msg::Mode;
using vk::msg::ResetCause;

// NOLINTBEGIN(readability-magic-numbers)
static_assert(VehicleStatus::MODE_STANDBY == value(Mode::Standby));
static_assert(VehicleStatus::MODE_TEST == value(Mode::Test));
static_assert(VehicleStatus::MODE_MANUAL == value(Mode::Manual));
static_assert(VehicleStatus::MODE_COMPANION == value(Mode::Companion));
static_assert(VehicleStatus::ACTUATOR_POWER_NOT_AVAILABLE == value(ActuatorPower::NotAvailable));
static_assert(VehicleStatus::ACTUATOR_POWER_UNPOWERED == value(ActuatorPower::Unpowered));
static_assert(VehicleStatus::ACTUATOR_POWER_SETTLING == value(ActuatorPower::Settling));
static_assert(VehicleStatus::ACTUATOR_POWER_POWERED == value(ActuatorPower::Powered));
static_assert(VehicleStatus::ARMING_OK == value(ArmingResult::Ok));
static_assert(VehicleStatus::ARMING_BOARD_NOT_VERIFIED == value(ArmingResult::BoardNotVerified));
static_assert(VehicleStatus::ARMING_EMERGENCY_STOP == value(ArmingResult::EmergencyStop));
static_assert(VehicleStatus::ARMING_WRONG_MODE == value(ArmingResult::WrongMode));
static_assert(VehicleStatus::ARMING_NOT_IN_CONTROL == value(ArmingResult::NotInControl));
static_assert(VehicleStatus::ARMING_SETPOINTS_ACTIVE == value(ArmingResult::SetpointsActive));
static_assert(VehicleStatus::ARMING_BRINGUP_RUNNING == value(ArmingResult::BringupRunning));
static_assert(VehicleStatus::ARMING_ACTUATOR_POWER_OFF == value(ArmingResult::ActuatorPowerOff));
static_assert(VehicleStatus::ARMING_ACTUATORS_NOT_STARTED == value(ArmingResult::ActuatorsNotStarted));
static_assert(VehicleStatus::ARMING_LOCAL_MENU_OPEN == value(ArmingResult::LocalMenuOpen));
static_assert(VehicleStatus::ARMING_REBOOTING == value(ArmingResult::Rebooting));
static_assert(VehicleStatus::ARMING_UNEXPECTED_RESET == value(ArmingResult::UnexpectedReset));
static_assert(VehicleStatus::ARMING_RC_NOT_CONNECTED == value(ArmingResult::RcNotConnected));
static_assert(VehicleStatus::ARMING_STICKS_NOT_CENTRED == value(ArmingResult::SticksNotCentred));
static_assert(VehicleStatus::DISARM_NONE == value(DisarmReason::None));
static_assert(VehicleStatus::DISARM_REQUEST == value(DisarmReason::Request));
static_assert(VehicleStatus::DISARM_EMERGENCY_STOP == value(DisarmReason::EmergencyStop));
static_assert(VehicleStatus::DISARM_ACTUATOR_POWER_LOST == value(DisarmReason::ActuatorPowerLost));
static_assert(VehicleStatus::DISARM_SETPOINT_TIMEOUT == value(DisarmReason::SetpointTimeout));
static_assert(VehicleStatus::DISARM_IDLE == value(DisarmReason::Idle));
static_assert(VehicleStatus::DISARM_BRINGUP_ABORT == value(DisarmReason::BringupAbort));
static_assert(VehicleStatus::DISARM_RC_LOST == value(DisarmReason::RcLost));
static_assert(VehicleStatus::RESET_UNKNOWN == value(ResetCause::Unknown));
static_assert(VehicleStatus::RESET_POWER_ON == value(ResetCause::PowerOn));
static_assert(VehicleStatus::RESET_PIN == value(ResetCause::Pin));
static_assert(VehicleStatus::RESET_SOFTWARE == value(ResetCause::Software));
static_assert(VehicleStatus::RESET_WATCHDOG == value(ResetCause::Watchdog));
static_assert(VehicleStatus::RESET_FAULT == value(ResetCause::Fault));
static_assert(VehicleStatus::RESET_LOW_POWER == value(ResetCause::LowPower));
// NOLINTEND(readability-magic-numbers)

TEST(Outputs, RosStampIsTheSampleTimeOnRosClock)
{
    // A sample taken 3 ms of steady time before the node's clock read.
    constexpr std::uint64_t steady_now = 10'000'000;
    constexpr std::int64_t ros_now = 1'700'000'000'000'000'000;
    EXPECT_EQ(ros_stamp_ns(9'997'000, steady_now, ros_now), ros_now - 3'000'000);
    EXPECT_EQ(ros_stamp_ns(10'000'000, steady_now, ros_now), ros_now);
}

TEST(Outputs, TheSampleTimeIsTimestampSampleWhereTheTopicHasOne)
{
    EXPECT_EQ(sample_time_us(make_sample()), 1'999'000U);  // not `timestamp`
    vk::msg::ImuSample imu;
    imu.timestamp = 11;
    EXPECT_EQ(sample_time_us(imu), 11U);
    vk::msg::VehicleAttitude attitude;
    attitude.timestamp = 1;
    attitude.timestamp_sample = 2;
    EXPECT_EQ(sample_time_us(attitude), 2U);
}

}  // namespace
}  // namespace vk::bridge
