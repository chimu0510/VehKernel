// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge's outputs as ROS2 messages (ADR-0003, ADR-0014, ADR-0027, spec 0015):
// pure functions from a generated message and a stamp to a ROS2 message, in ROS2's
// frames (frames.hpp). They need ROS2's message packages but no node, so a gtest
// covers them. The node only stamps, picks the QoS and publishes.
#pragma once

#include <vehkernel_msgs/msg/firmware_info.hpp>
#include <vehkernel_msgs/msg/vehicle_status.hpp>

#include <geometry_msgs/msg/transform_stamped.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/imu.hpp>

#include <vk/msg/firmware_info.hpp>
#include <vk/msg/imu_sample.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace vk::bridge {

// The variance of a quantity the MCU does not estimate: a standard deviation of
// 1000 (m, rad or m/s) says the entry carries no information, and stays finite
// and positive definite, which -1 or infinity would not for a filter that
// consumes the matrix (Nav2, robot_localization).
inline constexpr double unknown_variance = 1.0e6;

// Nanoseconds since the epoch of ROS2's clock for a time on the Bridge's
// monotonic clock, given both clocks read at the same moment.
[[nodiscard]] constexpr std::int64_t ros_stamp_ns(std::int64_t host_us, std::uint64_t steady_now_us,
                                                  std::int64_t ros_now_ns)
{
    return ros_now_ns - (static_cast<std::int64_t>(steady_now_us) - host_us) * 1000;
}

[[nodiscard]] builtin_interfaces::msg::Time to_time(std::int64_t ns);

// The time a message's samples were taken: `timestamp_sample` where the topic has
// one, else `timestamp`. For the vehicle status that is its publication time, and
// the status reaches the Bridge as the latest value at the topic map's 5 Hz, so its
// stamp may be up to one Link period older than its arrival; the streamed topics
// (odometry, IMU) arrive within a few milliseconds of their stamps.
[[nodiscard]] constexpr std::uint64_t sample_time_us(const vk::msg::VehicleLocalOdometry& m)
{
    return m.timestamp_sample;
}
[[nodiscard]] constexpr std::uint64_t sample_time_us(const vk::msg::VehicleAttitude& m) { return m.timestamp_sample; }
[[nodiscard]] constexpr std::uint64_t sample_time_us(const vk::msg::ImuSample& m) { return m.timestamp; }
[[nodiscard]] constexpr std::uint64_t sample_time_us(const vk::msg::VehicleStatus& m) { return m.timestamp; }
[[nodiscard]] constexpr std::uint64_t sample_time_us(const vk::msg::FirmwareInfo& m) { return m.timestamp; }

struct FrameNames {
    std::string odom = "odom";
    std::string base = "base_link";
};

[[nodiscard]] nav_msgs::msg::Odometry make_odometry(const vk::msg::VehicleLocalOdometry& odometry, std::int64_t stamp_ns,
                                                   const FrameNames& names);
[[nodiscard]] geometry_msgs::msg::TransformStamped make_transform(const vk::msg::VehicleLocalOdometry& odometry,
                                                                  std::int64_t stamp_ns, const FrameNames& names);

// IMU noise from the MCU's parameters, as standard deviations; empty until read.
struct ImuNoise {
    std::optional<float> gyro;  // EKF_GYR_NOISE [rad/s/sqrt(Hz)]
    std::optional<float> accel;  // EKF_ACC_NOISE [m/s^2]
};

// `attitude` is the latest vehicle_attitude, if any: without it the orientation is
// unknown, which sensor_msgs/Imu says with a -1 at the first element of its
// covariance and an all-zero quaternion. A noise not yet read leaves its covariance
// all zero, which the message defines as unknown.
[[nodiscard]] sensor_msgs::msg::Imu make_imu(const vk::msg::ImuSample& sample,
                                            const std::optional<vk::msg::VehicleAttitude>& attitude,
                                            const ImuNoise& noise, std::int64_t stamp_ns, const std::string& frame);

[[nodiscard]] vehkernel_msgs::msg::VehicleStatus make_vehicle_status(const vk::msg::VehicleStatus& status,
                                                                    std::int64_t stamp_ns);

// The firmware's identity (ADR-0023); `same_firmware` compares two without their stamps.
[[nodiscard]] vehkernel_msgs::msg::FirmwareInfo make_firmware_info(const vk::msg::FirmwareInfo& info,
                                                                  std::int64_t stamp_ns);
[[nodiscard]] bool same_firmware(const vk::msg::FirmwareInfo& a, const vk::msg::FirmwareInfo& b);

}  // namespace vk::bridge
