// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge node (ADR-0027, spec 0015).
#include <vehkernel_bridge/bridge_node.hpp>

#include <vehkernel_bridge/serial_transport.hpp>
#include <vehkernel_bridge/udp_transport.hpp>

#include <vk/msg/vehicle_status.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace vk::bridge {

BridgeNode::BridgeNode(const rclcpp::NodeOptions& options) : rclcpp::Node("bridge", options)
{
    const auto transport = declare_parameter<std::string>("transport", "udp");
    const auto device = declare_parameter<std::string>("serial_device", "/dev/ttyACM0");
    const auto baud = static_cast<int>(declare_parameter<std::int64_t>("serial_baud", SerialTransport::default_baud));
    const auto address = declare_parameter<std::string>("udp_address", "127.0.0.1");
    const auto port = static_cast<std::uint16_t>(declare_parameter<std::int64_t>("udp_port", UdpTransport::default_port));
    const auto poll_period_ms = declare_parameter<std::int64_t>("poll_period_ms", 2);

    if (transport == "serial") {
        if (!SerialTransport::baud_supported(baud)) {
            throw std::invalid_argument("serial_baud " + std::to_string(baud) + " is not a supported baud rate");
        }
        transport_ = std::make_unique<SerialTransport>(device, baud);
    } else if (transport == "udp") {
        transport_ = std::make_unique<UdpTransport>(address, port);
    } else {
        throw std::invalid_argument("transport must be serial or udp, not " + transport);
    }
    if (poll_period_ms < 1) {
        throw std::invalid_argument("poll_period_ms must be at least 1");
    }

    session_ = std::make_unique<Session>(*transport_, clock_);
    session_->events.on_connected = [this](const ConnectionInfo& info) { on_connected(info); };
    session_->events.on_disconnected = [this](const Disconnect& disconnect) { on_disconnected(disconnect); };
    session_->events.on_time_sync = [this](const TimeSync& sync) { on_time_sync(sync); };
    // The vehicle status shows that topics are decoded; later tickets publish it to ROS2.
    session_->subscribe<vk::msg::VehicleStatus>([this](std::uint8_t /*instance*/, const vk::msg::VehicleStatus& status) {
        if (last_mode_ != status.mode || last_armed_ != status.armed) {
            RCLCPP_INFO(get_logger(), "vehicle status: mode %u, %s, emergency stop %s", static_cast<unsigned>(status.mode),
                        status.armed ? "armed" : "disarmed", status.emergency_stop ? "engaged" : "released");
            last_mode_ = status.mode;
            last_armed_ = status.armed;
        }
    });

    RCLCPP_INFO(get_logger(), "Link over %s", transport_->description());
    poll_timer_ = create_wall_timer(std::chrono::milliseconds(poll_period_ms), [this] { session_->poll(); });
    statistics_timer_ = create_wall_timer(std::chrono::seconds(10), [this] { log_link_statistics(); });
}

void BridgeNode::on_connected(const ConnectionInfo& info)
{
    time_sync_logged_ = false;
    last_mode_.reset();
    last_armed_.reset();
    RCLCPP_INFO(get_logger(), "handshake done: protocol version %u, %u topics, %u parameters", info.protocol_version,
                info.topic_count, info.param_count);
    if (info.disabled_topics.empty()) {
        RCLCPP_INFO(get_logger(), "no topic's schema hash differs");
    }
    for (const DisabledTopic& topic : info.disabled_topics) {
        RCLCPP_WARN(get_logger(), "topic %s disabled: its schema hash differs (MCU %016llX, Bridge %016llX)",
                    topic.name.c_str(), static_cast<unsigned long long>(topic.mcu_hash),
                    static_cast<unsigned long long>(topic.host_hash));
    }
    for (const std::string& name : info.missing_required_topics) {
        RCLCPP_WARN(get_logger(), "required topic %s is not offered by the MCU", name.c_str());
    }
    for (const std::string& name : info.unknown_topics) {
        RCLCPP_DEBUG(get_logger(), "topic %s is not in the Bridge's schema, ignored", name.c_str());
    }
}

void BridgeNode::on_disconnected(const Disconnect& disconnect)
{
    if (disconnect.reason == DisconnectReason::RequiredTopicMismatch) {
        RCLCPP_ERROR(get_logger(), "connection refused: %s", disconnect.detail.c_str());
    } else {
        RCLCPP_WARN(get_logger(), "link down (%s): %s; trying again", to_string(disconnect.reason),
                    disconnect.detail.c_str());
    }
}

void BridgeNode::on_time_sync(const TimeSync& sync)
{
    const auto offset = static_cast<long long>(sync.clock_offset_us);
    const auto rtt = static_cast<long long>(sync.rtt_us);
    if (!time_sync_logged_) {
        time_sync_logged_ = true;
        RCLCPP_INFO(get_logger(), "time synchronization: MCU clock = Bridge clock + %lld us, round trip %lld us", offset,
                    rtt);
    } else {
        RCLCPP_DEBUG(get_logger(), "time synchronization: offset %lld us, round trip %lld us", offset, rtt);
    }
}

void BridgeNode::log_link_statistics()
{
    if (!session_->connected()) {
        return;
    }
    const SessionStats stats = session_->stats();
    RCLCPP_DEBUG(get_logger(), "link: %u frames, %u lost, %u CRC errors, %u malformed", stats.frames, stats.lost,
                 stats.crc_errors, stats.malformed);
}

}  // namespace vk::bridge
