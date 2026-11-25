// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge node (ADR-0027, spec 0015).
#include <vehkernel_bridge/bridge_node.hpp>

#include <vehkernel_bridge/serial_transport.hpp>
#include <vehkernel_bridge/udp_transport.hpp>

#include <vk/msg/vehicle_attitude.hpp>
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
    names_.odom = declare_parameter<std::string>("odom_frame", "odom");
    names_.base = declare_parameter<std::string>("base_frame", "base_link");
    publish_tf_ = declare_parameter<bool>("publish_tf", true);
    // The IMU is mounted on the body; the MCU already rotated its samples into it (ADR-0014).
    imu_frame_ = names_.base;

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
    imu_noise_ = std::make_unique<ImuNoiseReader>(
        [this](vk::link::FrameType type, std::span<const std::uint8_t> body) { return session_->send_frame(type, body); },
        [this](const std::string& text) { RCLCPP_INFO(get_logger(), "%s", text.c_str()); });
    session_->events.on_frame = [this](const vk::link::Frame& frame) { on_frame(frame); };

    // /imu/data is sensor data; the rest keep the default: reliable, keeping the last 10 (spec 0015).
    odometry_pub_ = create_publisher<nav_msgs::msg::Odometry>("odom", rclcpp::QoS(10));
    imu_pub_ = create_publisher<sensor_msgs::msg::Imu>("imu/data", rclcpp::SensorDataQoS());
    status_pub_ = create_publisher<vehkernel_msgs::msg::VehicleStatus>("vehkernel/vehicle_status", rclcpp::QoS(10));
    if (publish_tf_) {
        tf_broadcaster_ = std::make_unique<tf2_ros::TransformBroadcaster>(*this);
    }

    session_->subscribe<vk::msg::VehicleLocalOdometry>(
        [this](std::uint8_t /*instance*/, const vk::msg::VehicleLocalOdometry& odometry) { on_odometry(odometry); });
    session_->subscribe<vk::msg::VehicleAttitude>(
        [this](std::uint8_t /*instance*/, const vk::msg::VehicleAttitude& attitude) { attitude_ = attitude; });
    session_->subscribe<vk::msg::ImuSample>([this](std::uint8_t instance, const vk::msg::ImuSample& sample) {
        if (instance == 0) {
            on_imu(sample);
        }
    });
    session_->subscribe<vk::msg::VehicleStatus>(
        [this](std::uint8_t /*instance*/, const vk::msg::VehicleStatus& status) { on_status(status); });

    RCLCPP_INFO(get_logger(), "Link over %s", transport_->description());
    poll_timer_ = create_wall_timer(std::chrono::milliseconds(poll_period_ms), [this] {
        session_->poll();
        imu_noise_->poll(clock_.now_us());
    });
    statistics_timer_ = create_wall_timer(std::chrono::seconds(10), [this] { log_link_statistics(); });
}

void BridgeNode::on_connected(const ConnectionInfo& info)
{
    time_sync_logged_ = false;
    last_mode_.reset();
    last_armed_.reset();
    attitude_.reset();
    imu_noise_->restart(clock_.now_us());
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

void BridgeNode::on_frame(const vk::link::Frame& frame)
{
    imu_noise_->on_frame(frame, clock_.now_us());
}

std::optional<std::int64_t> BridgeNode::stamp_ns(std::uint64_t mcu_us)
{
    const std::optional<std::int64_t> host_us = session_->mcu_to_host_us(mcu_us);
    if (!host_us.has_value()) {
        return std::nullopt;
    }
    // Both clocks read together: the offset between them is the one of this moment.
    return ros_stamp_ns(*host_us, clock_.now_us(), get_clock()->now().nanoseconds());
}

void BridgeNode::on_odometry(const vk::msg::VehicleLocalOdometry& odometry)
{
    const auto stamp = stamp_ns(sample_time_us(odometry));
    if (!stamp.has_value()) {
        return;
    }
    odometry_pub_->publish(make_odometry(odometry, *stamp, names_));
    if (tf_broadcaster_) {
        tf_broadcaster_->sendTransform(make_transform(odometry, *stamp, names_));
    }
}

void BridgeNode::on_imu(const vk::msg::ImuSample& sample)
{
    const auto stamp = stamp_ns(sample_time_us(sample));
    if (!stamp.has_value()) {
        return;
    }
    const ImuNoise noise{imu_noise_->gyro(), imu_noise_->accel()};
    imu_pub_->publish(make_imu(sample, attitude_, noise, *stamp, imu_frame_));
}

void BridgeNode::on_status(const vk::msg::VehicleStatus& status)
{
    if (last_mode_ != status.mode || last_armed_ != status.armed) {
        RCLCPP_INFO(get_logger(), "vehicle status: mode %u, %s, emergency stop %s", static_cast<unsigned>(status.mode),
                    status.armed ? "armed" : "disarmed", status.emergency_stop ? "engaged" : "released");
        last_mode_ = status.mode;
        last_armed_ = status.armed;
    }
    const auto stamp = stamp_ns(sample_time_us(status));
    if (stamp.has_value()) {
        status_pub_->publish(make_vehicle_status(status, *stamp));
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
