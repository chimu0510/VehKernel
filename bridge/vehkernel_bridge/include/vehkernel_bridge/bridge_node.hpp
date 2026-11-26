// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge node: one Link as ROS2 topics and services (ADR-0027, spec 0015).
// It publishes the odometry, the TF, the IMU and the vehicle status in ROS2's
// frames and on the companion computer's clock (ticket #195), and takes /cmd_vel and
// the arming, mode and emergency stop services (ticket #196).
#pragma once

#include <vehkernel_bridge/float_param_reader.hpp>
#include <vehkernel_bridge/imu_noise.hpp>
#include <vehkernel_bridge/outputs.hpp>
#include <vehkernel_bridge/request_tracker.hpp>
#include <vehkernel_bridge/session.hpp>
#include <vehkernel_bridge/setpoint_scheduler.hpp>
#include <vehkernel_bridge/steady_clock.hpp>
#include <vehkernel_bridge/transport.hpp>

#include <vehkernel_msgs/msg/firmware_info.hpp>
#include <vehkernel_msgs/msg/vehicle_status.hpp>
#include <vehkernel_msgs/srv/set_mode.hpp>

#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <tf2_ros/transform_broadcaster.h>

#include <vk/msg/body_velocity_setpoint.hpp>
#include <vk/msg/firmware_info.hpp>
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
    void check_cmd_vel_timeout();
    void on_frame(const vk::link::Frame& frame);

    // The MCU time of a sample as nanoseconds of ROS2's clock; empty before the first
    // time synchronization of the connection, when nothing can be stamped.
    [[nodiscard]] std::optional<std::int64_t> stamp_ns(std::uint64_t mcu_us);
    void on_odometry(const vk::msg::VehicleLocalOdometry& odometry);
    void on_imu(const vk::msg::ImuSample& sample);
    void on_status(const vk::msg::VehicleStatus& status);
    void on_firmware_info(const vk::msg::FirmwareInfo& info);

    // The inputs. A /cmd_vel is sent on as it arrives; the zero setpoints of the idle
    // rule come from the poll timer.
    void on_cmd_vel(const std::optional<vk::msg::BodyVelocitySetpoint>& setpoint);
    void send_zero_setpoint_if_due(std::uint64_t now_us);
    // A service call is answered from the request tracker's callback when the MCU's
    // acknowledgement or the timeout arrives, not from the service's callback, so
    // waiting never blocks the node's executor.
    void handle_set_bool(Command command, const std::shared_ptr<rclcpp::Service<std_srvs::srv::SetBool>>& service,
                         const std::shared_ptr<rmw_request_id_t>& header,
                         const std::shared_ptr<std_srvs::srv::SetBool::Request>& request);
    void handle_set_mode(const std::shared_ptr<rclcpp::Service<vehkernel_msgs::srv::SetMode>>& service,
                         const std::shared_ptr<rmw_request_id_t>& header,
                         const std::shared_ptr<vehkernel_msgs::srv::SetMode::Request>& request);

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
    rclcpp::Publisher<vehkernel_msgs::msg::FirmwareInfo>::SharedPtr firmware_pub_;
    // The identity last logged and published in this connection, so each is done once per change.
    std::optional<vk::msg::FirmwareInfo> logged_firmware_;
    std::optional<vk::msg::FirmwareInfo> published_firmware_;
    std::unique_ptr<tf2_ros::TransformBroadcaster> tf_broadcaster_;
    // The orientation and variances an IMU message carries; the latest attitude of the connection.
    std::optional<vk::msg::VehicleAttitude> attitude_;
    // COM_SP_TIMEOUT, read at each connection to check cmd_vel_timeout against it.
    std::unique_ptr<FloatParamReader> sp_timeout_reader_;
    bool sp_timeout_checked_ = false;
    std::uint64_t cmd_vel_timeout_us_ = 0;
    std::optional<std::uint8_t> logged_source_;
    std::unique_ptr<RequestTracker> tracker_;
    std::unique_ptr<SetpointScheduler> scheduler_;
    rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_sub_;
    rclcpp::Subscription<geometry_msgs::msg::TwistStamped>::SharedPtr cmd_vel_stamped_sub_;
    rclcpp::Service<std_srvs::srv::SetBool>::SharedPtr arm_srv_;
    rclcpp::Service<vehkernel_msgs::srv::SetMode>::SharedPtr set_mode_srv_;
    rclcpp::Service<std_srvs::srv::SetBool>::SharedPtr emergency_stop_srv_;
    rclcpp::TimerBase::SharedPtr poll_timer_;
    rclcpp::TimerBase::SharedPtr statistics_timer_;
    // The first time synchronization of a connection is logged at info level, later ones at debug.
    bool time_sync_logged_ = false;
    std::optional<std::uint8_t> last_mode_;
    std::optional<bool> last_armed_;
};

}  // namespace vk::bridge
