// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge's outputs as ROS2 messages (ADR-0003, ADR-0014, ADR-0027, spec 0015).
#include <vehkernel_bridge/outputs.hpp>

#include <vehkernel_bridge/frames.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace vk::bridge {

namespace {

using frames::Mat3;
using frames::Quat;
using frames::Vec3;

constexpr Vec3 to_vec3(const std::array<float, 3>& v)
{
    return {static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])};
}

constexpr Quat to_quat(const std::array<float, 4>& q)
{
    return {static_cast<double>(q[0]), static_cast<double>(q[1]), static_cast<double>(q[2]), static_cast<double>(q[3])};
}

constexpr Mat3 diagonal(double a, double b, double c) { return {a, 0, 0, 0, b, 0, 0, 0, c}; }

// A finite variance of at least zero; the MCU's NaN or negative one is unknown.
double variance_or_unknown(float variance)
{
    const auto value = static_cast<double>(variance);
    return value >= 0.0 && value < unknown_variance ? value : unknown_variance;
}

// The 6x6 covariance of nav_msgs/Odometry: x, y, z, then rotation about x, y, z.
// `first` is the upper left 3x3 block and `second` the lower right; the blocks
// of correlation between the two are zero.
void set_block_diagonal(std::array<double, 36>& out, const Mat3& first, const Mat3& second)
{
    out.fill(0.0);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            out[row * 6 + column] = first[row * 3 + column];
            out[(row + 3) * 6 + column + 3] = second[row * 3 + column];
        }
    }
}

}  // namespace

builtin_interfaces::msg::Time to_time(std::int64_t ns)
{
    builtin_interfaces::msg::Time time;
    // Negative nanoseconds (a stamp before the epoch) cannot be a ROS time.
    const std::int64_t clamped = std::max<std::int64_t>(ns, 0);
    time.sec = static_cast<std::int32_t>(clamped / 1'000'000'000);
    time.nanosec = static_cast<std::uint32_t>(clamped % 1'000'000'000);
    return time;
}

nav_msgs::msg::Odometry make_odometry(const vk::msg::VehicleLocalOdometry& odometry, std::int64_t stamp_ns,
                                      const FrameNames& names)
{
    nav_msgs::msg::Odometry out;
    out.header.stamp = to_time(stamp_ns);
    out.header.frame_id = names.odom;
    out.child_frame_id = names.base;

    const Vec3 position = frames::ned_to_enu(to_vec3(odometry.position));
    out.pose.pose.position.x = position[0];
    out.pose.pose.position.y = position[1];
    out.pose.pose.position.z = position[2];
    const Quat q = frames::attitude_ned_frd_to_enu_flu(to_quat(odometry.q));
    out.pose.pose.orientation.w = q.w;
    out.pose.pose.orientation.x = q.x;
    out.pose.pose.orientation.y = q.y;
    out.pose.pose.orientation.z = q.z;

    // The twist is in the child frame: the body, forward-left-up.
    const Vec3 velocity = frames::frd_to_flu(to_vec3(odometry.velocity));
    out.twist.twist.linear.x = velocity[0];
    out.twist.twist.linear.y = velocity[1];
    out.twist.twist.linear.z = velocity[2];
    const Vec3 rate = frames::frd_to_flu({0.0, 0.0, static_cast<double>(odometry.yaw_rate)});
    out.twist.twist.angular.x = rate[0];
    out.twist.twist.angular.y = rate[1];
    out.twist.twist.angular.z = rate[2];

    // Pose covariance. The MCU estimates the horizontal position (north-north,
    // north-east, east-east) and the heading; down, roll and pitch it does not.
    // A correlation beyond sqrt(nn * ee) is not a covariance: the MCU's float32
    // matrix, nearly singular while one direction is far better known than the
    // other, gets there by rounding (#198). Trimmed, the block stays positive
    // semidefinite, which robot_localization and RViz expect.
    const double nn = variance_or_unknown(odometry.position_variance[0]);
    const double ee = variance_or_unknown(odometry.position_variance[2]);
    const auto ne_reported = static_cast<double>(odometry.position_variance[1]);
    const double ne_bound = std::sqrt(nn * ee);
    const double ne = std::isfinite(ne_reported) ? std::clamp(ne_reported, -ne_bound, ne_bound) : 0.0;
    const Mat3 position_ned{nn, ne, 0, ne, ee, 0, 0, 0, unknown_variance};
    const Mat3 attitude_ned = diagonal(unknown_variance, unknown_variance, variance_or_unknown(odometry.heading_variance));
    set_block_diagonal(out.pose.covariance, frames::covariance_ned_to_enu(position_ned),
                       frames::covariance_ned_to_enu(attitude_ned));

    // Twist covariance: vx, vy and the yaw rate are estimated, the rest is not.
    const Mat3 linear_frd = diagonal(variance_or_unknown(odometry.velocity_variance[0]),
                                     variance_or_unknown(odometry.velocity_variance[1]), unknown_variance);
    const Mat3 angular_frd =
        diagonal(unknown_variance, unknown_variance, variance_or_unknown(odometry.velocity_variance[2]));
    set_block_diagonal(out.twist.covariance, frames::covariance_frd_to_flu(linear_frd),
                       frames::covariance_frd_to_flu(angular_frd));
    return out;
}

geometry_msgs::msg::TransformStamped make_transform(const vk::msg::VehicleLocalOdometry& odometry,
                                                    std::int64_t stamp_ns, const FrameNames& names)
{
    const nav_msgs::msg::Odometry odom = make_odometry(odometry, stamp_ns, names);
    geometry_msgs::msg::TransformStamped out;
    out.header = odom.header;
    out.child_frame_id = odom.child_frame_id;
    out.transform.translation.x = odom.pose.pose.position.x;
    out.transform.translation.y = odom.pose.pose.position.y;
    out.transform.translation.z = odom.pose.pose.position.z;
    out.transform.rotation = odom.pose.pose.orientation;
    return out;
}

sensor_msgs::msg::Imu make_imu(const vk::msg::ImuSample& sample, const std::optional<vk::msg::VehicleAttitude>& attitude,
                               const ImuNoise& noise, std::int64_t stamp_ns, const std::string& frame)
{
    sensor_msgs::msg::Imu out;
    out.header.stamp = to_time(stamp_ns);
    out.header.frame_id = frame;

    const Vec3 rate = frames::frd_to_flu(to_vec3(sample.gyro));
    out.angular_velocity.x = rate[0];
    out.angular_velocity.y = rate[1];
    out.angular_velocity.z = rate[2];
    // The accelerometer's specific force: at rest it points up, +9.8 on z in forward-left-up.
    const Vec3 specific_force = frames::frd_to_flu(to_vec3(sample.accel));
    out.linear_acceleration.x = specific_force[0];
    out.linear_acceleration.y = specific_force[1];
    out.linear_acceleration.z = specific_force[2];

    if (attitude.has_value()) {
        const Quat q = frames::attitude_ned_frd_to_enu_flu(to_quat(attitude->q));
        out.orientation.w = q.w;
        out.orientation.x = q.x;
        out.orientation.y = q.y;
        out.orientation.z = q.z;
        // The variances of roll, pitch and yaw taken as those of rotations about
        // north, east and down, and turned into rotations about east, north and up.
        const Mat3 ned = diagonal(variance_or_unknown(attitude->variance[0]), variance_or_unknown(attitude->variance[1]),
                                  variance_or_unknown(attitude->variance[2]));
        const Mat3 enu = frames::covariance_ned_to_enu(ned);
        std::copy(enu.begin(), enu.end(), out.orientation_covariance.begin());
    } else {
        out.orientation.w = 0.0;  // no orientation, as sensor_msgs/Imu says it
        out.orientation_covariance[0] = -1.0;
    }

    if (noise.gyro.has_value()) {
        const double variance = static_cast<double>(*noise.gyro) * static_cast<double>(*noise.gyro);
        out.angular_velocity_covariance = {variance, 0, 0, 0, variance, 0, 0, 0, variance};
    }
    if (noise.accel.has_value()) {
        const double variance = static_cast<double>(*noise.accel) * static_cast<double>(*noise.accel);
        out.linear_acceleration_covariance = {variance, 0, 0, 0, variance, 0, 0, 0, variance};
    }
    return out;
}

vehkernel_msgs::msg::VehicleStatus make_vehicle_status(const vk::msg::VehicleStatus& status, std::int64_t stamp_ns)
{
    vehkernel_msgs::msg::VehicleStatus out;
    out.header.stamp = to_time(stamp_ns);
    out.mode = static_cast<std::uint8_t>(status.mode);
    out.armed = status.armed;
    out.source = status.source;
    out.emergency_stop = status.emergency_stop;
    out.actuator_power = static_cast<std::uint8_t>(status.actuator_power);
    out.arming_refusal = static_cast<std::uint8_t>(status.arming_refusal);
    out.disarm_reason = static_cast<std::uint8_t>(status.disarm_reason);
    out.reset_cause = static_cast<std::uint8_t>(status.reset_cause);
    out.setpoints_rejected = status.setpoints_rejected;
    return out;
}

vehkernel_msgs::msg::FirmwareInfo make_firmware_info(const vk::msg::FirmwareInfo& info, std::int64_t stamp_ns)
{
    vehkernel_msgs::msg::FirmwareInfo out;
    out.header.stamp = to_time(stamp_ns);
    out.version_major = info.version_major;
    out.version_minor = info.version_minor;
    out.version_patch = info.version_patch;
    out.git_hash = info.git_hash;
    out.git_dirty = info.git_dirty;
    out.board_id = info.board_id;
    return out;
}

bool same_firmware(const vk::msg::FirmwareInfo& a, const vk::msg::FirmwareInfo& b)
{
    return a.version_major == b.version_major && a.version_minor == b.version_minor &&
           a.version_patch == b.version_patch && a.git_hash == b.git_hash && a.git_dirty == b.git_dirty &&
           a.board_id == b.board_id;
}

}  // namespace vk::bridge
