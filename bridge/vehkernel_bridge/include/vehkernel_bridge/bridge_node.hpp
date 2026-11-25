// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge node: one Link as ROS2 topics and services (ADR-0027, spec 0015).
// It publishes the odometry, the TF, the IMU and the vehicle status in ROS2's
// frames and on the companion computer's clock (ticket #195).
#pragma once

#include <vehkernel_bridge/imu_noise.hpp>
#include <vehkernel_bridge/outputs.hpp>
#include <vehkernel_bridge/session.hpp>
#include <vehkernel_bridge/steady_clock.hpp>
#include <vehkernel_bridge/transport.hpp>

#include <vehkernel_msgs/msg/vehicle_status.hpp>

#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <tf2_ros/transform_broadcaster.h>

#include <vk/msg/imu_sample.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <memory>
#include <optional>

namespace vk::bridge {

class BridgeNode final : public rclcpp::Node {
public:
    explicit BridgeNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());

private:
    void on_connected(const ConnectionInfo& info);
    void on_disconnected(const Disconnect& disconnect);
    void on_time_sync(const TimeSync& sync);
    void log_link_statistics();
    void on_frame(const vk::link::Frame& frame);

    // The MCU time of a sample as nanoseconds of ROS2's clock; empty before the first
    // time synchronization of the connection, when nothing can be stamped.
    [[nodiscard]] std::optional<std::int64_t> stamp_ns(std::uint64_t mcu_us);
    void on_odometry(const vk::msg::VehicleLocalOdometry& odometry);
    void on_imu(const vk::msg::ImuSample& sample);
    void on_status(const vk::msg::VehicleStatus& status);

    SteadyClock clock_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<ImuNoiseReader> imu_noise_;
    FrameNames names_;
    std::string imu_frame_;
    bool publish_tf_ = true;
    rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odometry_pub_;
    rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr imu_pub_;
    rclcpp::Publisher<vehkernel_msgs::msg::VehicleStatus>::SharedPtr status_pub_;
    std::unique_ptr<tf2_ros::TransformBroadcaster> tf_broadcaster_;
    // The orientation and variances an IMU message carries; the latest attitude of the connection.
    std::optional<vk::msg::VehicleAttitude> attitude_;
    rclcpp::TimerBase::SharedPtr poll_timer_;
    rclcpp::TimerBase::SharedPtr statistics_timer_;
    // The first time synchronization of a connection is logged at info level, later ones at debug.
    bool time_sync_logged_ = false;
    std::optional<std::uint8_t> last_mode_;
    std::optional<bool> last_armed_;
};

}  // namespace vk::bridge
