// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge node (ADR-0027, spec 0015).
#include <vehkernel_bridge/bridge_node.hpp>

#include <vehkernel_bridge/cmd_vel.hpp>
#include <vehkernel_bridge/serial_transport.hpp>
#include <vehkernel_bridge/udp_transport.hpp>

#include <vk/msg/enums.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/firmware_info.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
    const auto send = [this](vk::link::FrameType type, std::span<const std::uint8_t> body) {
        return session_->send_frame(type, body);
    };
    imu_noise_ = std::make_unique<ImuNoiseReader>(
        send,
        [this](const std::string& text) { RCLCPP_INFO(get_logger(), "%s", text.c_str()); });
    session_->events.on_frame = [this](const vk::link::Frame& frame) { on_frame(frame); };

    const auto cmd_vel_timeout_s = declare_parameter<double>("cmd_vel_timeout", 0.25);
    const auto cmd_vel_stamped = declare_parameter<bool>("cmd_vel_stamped", true);
    const auto service_timeout_s = declare_parameter<double>("service_timeout", 2.0);
    // This Link's instance as the MCU numbers the source in control; -1 to learn it from the
    // first status after the MCU accepted this Bridge's request for COMPANION.
    const auto link_instance = declare_parameter<std::int64_t>("link_instance", -1);
    if (cmd_vel_timeout_s <= 0.0 || service_timeout_s <= 0.0) {
        throw std::invalid_argument("cmd_vel_timeout and service_timeout must be positive");
    }
    if (link_instance < -1 || link_instance > 254) {
        throw std::invalid_argument("link_instance must be -1 or a Link instance from 0 to 254");
    }
    SchedulerConfig scheduler_config;
    scheduler_config.cmd_vel_timeout_us = static_cast<std::uint64_t>(cmd_vel_timeout_s * 1e6);
    cmd_vel_timeout_us_ = scheduler_config.cmd_vel_timeout_us;
    sp_timeout_reader_ = std::make_unique<FloatParamReader>(
        std::vector<std::string>{"COM_SP_TIMEOUT"}, "cmd_vel_timeout is not checked against it", send,
        [this](const std::string& text) { RCLCPP_INFO(get_logger(), "%s", text.c_str()); });
    scheduler_ = std::make_unique<SetpointScheduler>(
        scheduler_config, link_instance < 0 ? std::nullopt : std::optional<std::uint8_t>(static_cast<std::uint8_t>(link_instance)));
    TrackerConfig tracker_config;
    tracker_config.timeout_us = static_cast<std::uint64_t>(service_timeout_s * 1e6);
    tracker_ = std::make_unique<RequestTracker>(send, tracker_config);

    // /imu/data is sensor data; the rest keep the default: reliable, keeping the last 10 (spec 0015).
    odometry_pub_ = create_publisher<nav_msgs::msg::Odometry>("odom", rclcpp::QoS(10));
    imu_pub_ = create_publisher<sensor_msgs::msg::Imu>("imu/data", rclcpp::SensorDataQoS());
    status_pub_ = create_publisher<vehkernel_msgs::msg::VehicleStatus>("vehkernel/vehicle_status", rclcpp::QoS(10));
    // Transient local, so that a node started after the connection still gets it.
    firmware_pub_ = create_publisher<vehkernel_msgs::msg::FirmwareInfo>("vehkernel/firmware_info",
                                                                        rclcpp::QoS(1).transient_local());
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
    session_->subscribe<vk::msg::FirmwareInfo>(
        [this](std::uint8_t /*instance*/, const vk::msg::FirmwareInfo& info) { on_firmware_info(info); });

    // /cmd_vel is sent on as it arrives. Nav2 on Jazzy publishes TwistStamped; teleop_twist_keyboard
    // and older nodes publish Twist.
    if (cmd_vel_stamped) {
        cmd_vel_stamped_sub_ = create_subscription<geometry_msgs::msg::TwistStamped>(
            "cmd_vel", rclcpp::QoS(10),
            [this](const geometry_msgs::msg::TwistStamped& twist) { on_cmd_vel(make_body_velocity_setpoint(twist)); });
    } else {
        cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
            "cmd_vel", rclcpp::QoS(10),
            [this](const geometry_msgs::msg::Twist& twist) { on_cmd_vel(make_body_velocity_setpoint(twist)); });
    }
    arm_srv_ = create_service<std_srvs::srv::SetBool>(
        "vehkernel/arm", [this](const std::shared_ptr<rclcpp::Service<std_srvs::srv::SetBool>>& service,
                                const std::shared_ptr<rmw_request_id_t>& header,
                                const std::shared_ptr<std_srvs::srv::SetBool::Request>& request) {
            handle_set_bool(Command::Arming, service, header, request);
        });
    emergency_stop_srv_ = create_service<std_srvs::srv::SetBool>(
        "vehkernel/emergency_stop", [this](const std::shared_ptr<rclcpp::Service<std_srvs::srv::SetBool>>& service,
                                           const std::shared_ptr<rmw_request_id_t>& header,
                                           const std::shared_ptr<std_srvs::srv::SetBool::Request>& request) {
            handle_set_bool(Command::EmergencyStop, service, header, request);
        });
    set_mode_srv_ = create_service<vehkernel_msgs::srv::SetMode>(
        "vehkernel/set_mode", [this](const std::shared_ptr<rclcpp::Service<vehkernel_msgs::srv::SetMode>>& service,
                                     const std::shared_ptr<rmw_request_id_t>& header,
                                     const std::shared_ptr<vehkernel_msgs::srv::SetMode::Request>& request) {
            handle_set_mode(service, header, request);
        });

    RCLCPP_INFO(get_logger(), "Link over %s", transport_->description());
    poll_timer_ = create_wall_timer(std::chrono::milliseconds(poll_period_ms), [this] {
        session_->poll();
        const std::uint64_t now_us = clock_.now_us();
        imu_noise_->poll(now_us);
        sp_timeout_reader_->poll(now_us);
        check_cmd_vel_timeout();
        tracker_->poll(now_us);
        send_zero_setpoint_if_due(now_us);
    });
    statistics_timer_ = create_wall_timer(std::chrono::seconds(10), [this] { log_link_statistics(); });
}

void BridgeNode::on_connected(const ConnectionInfo& info)
{
    time_sync_logged_ = false;
    last_mode_.reset();
    last_armed_.reset();
    attitude_.reset();
    scheduler_->reset();
    imu_noise_->restart(clock_.now_us());
    sp_timeout_reader_->restart(clock_.now_us());
    sp_timeout_checked_ = false;
    logged_source_.reset();
    logged_firmware_.reset();
    published_firmware_.reset();
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
    scheduler_->reset();
    tracker_->fail_all();
    if (disconnect.reason == DisconnectReason::RequiredTopicMismatch) {
        RCLCPP_ERROR(get_logger(), "connection refused: %s", disconnect.detail.c_str());
    } else {
        RCLCPP_WARN(get_logger(), "link down (%s): %s; trying again", to_string(disconnect.reason),
                    disconnect.detail.c_str());
    }
}

void BridgeNode::on_frame(const vk::link::Frame& frame)
{
    if (!tracker_->on_frame(frame)) {
        if (!imu_noise_->on_frame(frame, clock_.now_us())) {
            sp_timeout_reader_->on_frame(frame, clock_.now_us());
        }
    }
}

void BridgeNode::check_cmd_vel_timeout()
{
    if (sp_timeout_checked_ || !sp_timeout_reader_->complete()) {
        return;
    }
    sp_timeout_checked_ = true;
    const std::optional<float> sp_timeout = sp_timeout_reader_->value(0);
    if (sp_timeout.has_value() && !cmd_vel_timeout_is_safe(cmd_vel_timeout_us_, *sp_timeout)) {
        RCLCPP_WARN(get_logger(),
                    "cmd_vel_timeout %.3f s is not well below COM_SP_TIMEOUT %.3f s (it needs %.3f s of margin): "
                    "silence from /cmd_vel may disarm the vehicle instead of stopping it",
                    static_cast<double>(cmd_vel_timeout_us_) * 1e-6, static_cast<double>(*sp_timeout),
                    static_cast<double>(timeout_margin_us) * 1e-6);
    }
}

void BridgeNode::on_cmd_vel(const std::optional<vk::msg::BodyVelocitySetpoint>& setpoint)
{
    if (!setpoint.has_value()) {
        RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "/cmd_vel dropped: a velocity is not finite");
        return;
    }
    // Nothing is sent before the handshake or while the connection is refused or down.
    if (session_->send_topic(0, *setpoint)) {
        scheduler_->on_cmd_vel(clock_.now_us());
    }
}

void BridgeNode::send_zero_setpoint_if_due(std::uint64_t now_us)
{
    if (session_->connected() && scheduler_->zero_due(now_us)) {
        session_->send_topic(0, make_zero_setpoint());
    }
}

void BridgeNode::handle_set_bool(Command command, const std::shared_ptr<rclcpp::Service<std_srvs::srv::SetBool>>& service,
                                 const std::shared_ptr<rmw_request_id_t>& header,
                                 const std::shared_ptr<std_srvs::srv::SetBool::Request>& request)
{
    const char* what = command == Command::Arming ? (request->data ? "arm" : "disarm")
                                                    : (request->data ? "engage the emergency stop" : "release the emergency stop");
    tracker_->submit(
        command, request->data ? 1 : 0,
        [this, service, header, what](const Reply& reply) {
            std_srvs::srv::SetBool::Response response;
            response.success = reply.accepted();
            response.message = reply.reason;
            RCLCPP_INFO(get_logger(), "request to %s: %s%s%s", what, reply.accepted() ? "accepted" : "not accepted",
                        reply.accepted() ? "" : ", ", reply.reason.c_str());
            service->send_response(*header, response);
        },
        clock_.now_us());
}

void BridgeNode::handle_set_mode(const std::shared_ptr<rclcpp::Service<vehkernel_msgs::srv::SetMode>>& service,
                                 const std::shared_ptr<rmw_request_id_t>& header,
                                 const std::shared_ptr<vehkernel_msgs::srv::SetMode::Request>& request)
{
    const ModeChoice choice = resolve_mode(request->mode_name, request->mode_value);
    if (!choice.mode.has_value()) {
        vehkernel_msgs::srv::SetMode::Response response;
        response.accepted = false;
        response.reason = choice.error;
        service->send_response(*header, response);
        return;
    }
    const std::string name = mode_name(*choice.mode);
    tracker_->submit(
        Command::Mode, *choice.mode,
        [this, service, header, name, mode = *choice.mode](const Reply& reply) {
            if (reply.accepted() && mode == static_cast<std::uint8_t>(vk::msg::Mode::Companion)) {
                scheduler_->on_companion_accepted();
            }
            vehkernel_msgs::srv::SetMode::Response response;
            response.accepted = reply.accepted();
            response.reason = reply.reason;
            RCLCPP_INFO(get_logger(), "request for mode %s: %s%s%s", name.c_str(), reply.accepted() ? "accepted" : "not accepted",
                        reply.accepted() ? "" : ", ", reply.reason.c_str());
            service->send_response(*header, response);
        },
        clock_.now_us());
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
        const std::string_view disarm_reason = vk::msg::enum_name(static_cast<vk::msg::DisarmReason>(status.disarm_reason));
        RCLCPP_INFO(get_logger(), "vehicle status: mode %u, %s, emergency stop %s%s%.*s", static_cast<unsigned>(status.mode),
                    status.armed ? "armed" : "disarmed", status.emergency_stop ? "engaged" : "released",
                    status.armed ? "" : ", last disarm: ", status.armed ? 0 : static_cast<int>(disarm_reason.size()),
                    disarm_reason.data());
        last_mode_ = status.mode;
        last_armed_ = status.armed;
    }
    scheduler_->on_status(status.mode, status.armed, status.source, clock_.now_us());
    if (const auto own = scheduler_->own_source(); own != logged_source_) {
        logged_source_ = own;
        if (own.has_value()) {
            RCLCPP_INFO(get_logger(), "this Link is source %u in the vehicle status", static_cast<unsigned>(*own));
        }
    }
    const auto stamp = stamp_ns(sample_time_us(status));
    if (stamp.has_value()) {
        status_pub_->publish(make_vehicle_status(status, *stamp));
    }
}

void BridgeNode::on_firmware_info(const vk::msg::FirmwareInfo& info)
{
    if (!logged_firmware_.has_value() || !same_firmware(*logged_firmware_, info)) {
        RCLCPP_INFO(get_logger(), "firmware %u.%u.%u, git %08x%s, board %u", static_cast<unsigned>(info.version_major),
                    static_cast<unsigned>(info.version_minor), static_cast<unsigned>(info.version_patch),
                    static_cast<unsigned>(info.git_hash), info.git_dirty ? " with changes not committed" : "",
                    static_cast<unsigned>(info.board_id));
        logged_firmware_ = info;
    }
    if (published_firmware_.has_value() && same_firmware(*published_firmware_, info)) {
        return;
    }
    const auto stamp = stamp_ns(sample_time_us(info));
    if (stamp.has_value()) {
        firmware_pub_->publish(make_firmware_info(info, *stamp));
        published_firmware_ = info;
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
