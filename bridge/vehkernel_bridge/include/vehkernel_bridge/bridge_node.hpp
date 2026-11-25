// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge node: one Link as ROS2 topics and services (ADR-0027, spec 0015).
// This is its skeleton: it connects, stays connected and reports, and publishes
// nothing to ROS2 yet.
#pragma once

#include <vehkernel_bridge/session.hpp>
#include <vehkernel_bridge/steady_clock.hpp>
#include <vehkernel_bridge/transport.hpp>

#include <rclcpp/rclcpp.hpp>

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

    SteadyClock clock_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Session> session_;
    rclcpp::TimerBase::SharedPtr poll_timer_;
    rclcpp::TimerBase::SharedPtr statistics_timer_;
    // The first time synchronization of a connection is logged at info level, later ones at debug.
    bool time_sync_logged_ = false;
    std::optional<std::uint8_t> last_mode_;
    std::optional<bool> last_armed_;
};

}  // namespace vk::bridge
