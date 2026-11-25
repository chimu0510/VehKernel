// SPDX-License-Identifier: GPL-3.0-or-later
// The host side of a Link session in C++ (ADR-0002, ADR-0027, spec 0015): the
// handshake, the per-topic schema hash comparison with `@required` (ADR-0006),
// repeated time synchronization, the peer timeout and reconnection with backoff.
// It is the C++ counterpart of HostSession and LinkMonitor in tools/link/vklink.
//
// It knows neither rclcpp nor the operating system: it reads and writes a
// Transport and reads a Clock, so a test drives it with a fake of each, and the
// node, the serial device and the UDP socket stay thin layers around it.
#pragma once

#include <vehkernel_bridge/schema_table.hpp>
#include <vehkernel_bridge/transport.hpp>

#include <vk/link/frame.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vk::bridge {

struct SessionConfig {
    // Every version from the oldest the host speaks: a version only adds to the one
    // before (ADR-0002). The Python HostSession offers the same.
    std::vector<std::uint8_t> offered_versions{2, 3, 4, 5};
    std::uint64_t hello_interval_us = 250'000;
    // From opening the transport to the last topic_info.
    std::uint64_t handshake_timeout_us = 5'000'000;
    std::uint64_t time_sync_interval_us = 500'000;
    // No frame from the MCU for this long ends the connection; the MCU's own
    // peer timeout is 3 s.
    std::uint64_t peer_timeout_us = 3'000'000;
    // The wait before opening the transport again after a failure, doubled after
    // each failure up to the maximum, and reset by a connection.
    std::uint64_t backoff_initial_us = 500'000;
    std::uint64_t backoff_max_us = 5'000'000;
};

enum class SessionState : std::uint8_t {
    Disconnected,  // waiting to open the transport, or to try again
    Handshaking,   // hello sent, topic_info still arriving
    Connected,
};

// Why a connection attempt or a connection ended.
enum class DisconnectReason : std::uint8_t {
    OpenFailed,              // the device or the socket could not be opened
    HandshakeTimeout,        // the MCU did not describe every topic in time
    NoCommonVersion,         // the MCU speaks none of the offered protocol versions
    RequiredTopicMismatch,   // a `@required` topic's schema hash differs: the connection is refused
    PeerTimeout,             // nothing heard from the MCU, which may have rebooted
    TransportLost,           // the device went away, for example the cable was unplugged
};

[[nodiscard]] const char* to_string(DisconnectReason reason);

struct Disconnect {
    DisconnectReason reason;
    // What happened, in a sentence, naming the topics of a refusal.
    std::string detail;
    // The `@required` topics whose hash differs, for RequiredTopicMismatch.
    std::vector<std::string> required_topics;
};

struct DisabledTopic {
    std::string name;
    std::uint64_t mcu_hash;
    std::uint64_t host_hash;
};

// What a completed handshake found.
struct ConnectionInfo {
    std::uint8_t protocol_version = 0;
    std::uint16_t topic_count = 0;
    std::uint64_t param_set_hash = 0;
    std::uint16_t param_count = 0;
    // Topics whose schema hash differs: not delivered to subscribers.
    std::vector<DisabledTopic> disabled_topics;
    // Topics the MCU announced and this build of the Bridge does not know.
    std::vector<std::string> unknown_topics;
    // `@required` topics the MCU did not announce at all.
    std::vector<std::string> missing_required_topics;
};

// One time synchronization exchange (ADR-0002). mcu time = host time + clock_offset.
struct TimeSync {
    std::uint64_t host_send_us;
    std::uint64_t mcu_recv_us;
    std::uint64_t mcu_send_us;
    std::uint64_t host_recv_us;
    std::int64_t rtt_us;
    std::int64_t clock_offset_us;
};

struct SessionStats {
    std::uint32_t frames = 0;           // valid frames received, over every connection
    std::uint32_t crc_errors = 0;
    std::uint32_t malformed = 0;
    std::uint32_t lost = 0;             // frames missing from sequence gaps
    std::uint32_t topic_messages = 0;   // topic data delivered to a subscriber
    std::uint32_t decode_failures = 0;  // topic data whose size did not match its message
    std::uint32_t connections = 0;      // handshakes completed
};

// What the session tells its owner. Every callback may be empty.
struct SessionEvents {
    std::function<void(const ConnectionInfo&)> on_connected;
    std::function<void(const Disconnect&)> on_disconnected;
    std::function<void(const TimeSync&)> on_time_sync;
    // A frame the session does not consume itself (replies to requests), while connected.
    std::function<void(const vk::link::Frame&)> on_frame;
};

class Session {
public:
    Session(Transport& transport, const Clock& clock, SessionConfig config = {});

    SessionEvents events;

    // Calls `handler(instance, message)` for each message of `Message`'s topic
    // that decodes, while connected and when the MCU's schema hash agrees. Subscribe
    // before the first poll(); a subscription made later takes effect at the next
    // connection.
    template <class Message>
    void subscribe(std::function<void(std::uint8_t, const Message&)> handler)
    {
        decoders_[std::string(Message::topic_name)] =
            [this, handler = std::move(handler)](std::uint8_t instance, std::span<const std::uint8_t> bytes) {
                Message message;
                if (!Message::decode(bytes, message)) {
                    ++stats_.decode_failures;
                    return;
                }
                ++stats_.topic_messages;
                handler(instance, message);
            };
    }

    // Does what is due: opens the transport, reads and handles what arrived,
    // sends hello and time synchronization, and ends a connection that timed out.
    // Call it every few milliseconds.
    void poll();

    // Sends a frame the negotiated protocol version has; false when it is not
    // connected, the version lacks the frame, or the transport refused it.
    bool send_frame(vk::link::FrameType type, std::span<const std::uint8_t> body);

    // Sends a message as inbound topic data, for a topic the MCU announced and whose
    // schema hash agrees; false when not connected, the MCU did not announce the
    // topic, its hash differs (the topic is disabled), or the transport refused it.
    template <class Message>
    bool send_topic(std::uint8_t instance, const Message& message)
    {
        std::array<std::uint8_t, Message::encoded_size> bytes{};
        if (message.encode(bytes) != bytes.size()) {
            return false;
        }
        return send_topic_data(Message::topic_name, instance, bytes);
    }

    [[nodiscard]] SessionState state() const { return state_; }
    [[nodiscard]] bool connected() const { return state_ == SessionState::Connected; }
    [[nodiscard]] const ConnectionInfo& info() const { return info_; }
    [[nodiscard]] const std::optional<TimeSync>& time_sync() const { return time_sync_; }
    [[nodiscard]] SessionStats stats() const;

    // An MCU time on the host's clock; empty before the first time synchronization.
    [[nodiscard]] std::optional<std::int64_t> mcu_to_host_us(std::uint64_t mcu_us) const;

private:
    using Decoder = std::function<void(std::uint8_t, std::span<const std::uint8_t>)>;

    struct PeerTopic {
        std::uint8_t instances;
        std::uint64_t schema_hash;
        std::string name;
    };

    void attempt(std::uint64_t now_us);
    void read_input();
    void handle(const vk::link::Frame& frame);
    void on_hello_reply(std::span<const std::uint8_t> body);
    void on_topic_info(std::span<const std::uint8_t> body);
    void on_topic_data(std::span<const std::uint8_t> body);
    void on_time_sync_reply(std::span<const std::uint8_t> body);
    void complete_handshake();
    void send_time_sync(std::uint64_t now_us);
    bool write_frame(vk::link::FrameType type, std::span<const std::uint8_t> body);
    bool send_topic_data(std::string_view name, std::uint8_t instance, std::span<const std::uint8_t> message);
    void fail(DisconnectReason reason, std::string detail, std::vector<std::string> required_topics = {});

    Transport& transport_;
    const Clock& clock_;
    SessionConfig config_;
    SessionState state_ = SessionState::Disconnected;

    vk::link::FrameReader reader_;
    vk::link::FrameWriter writer_;
    std::array<std::uint8_t, vk::link::max_wire_size> wire_{};
    SessionStats stats_;
    // The reader's counts are per connection; those of earlier connections are kept here.
    vk::link::ReaderStats earlier_reader_{};

    std::uint64_t next_attempt_us_ = 0;
    std::uint64_t backoff_us_;
    std::uint64_t attempt_started_us_ = 0;
    std::uint64_t last_heard_us_ = 0;
    std::uint64_t next_hello_us_ = 0;
    std::uint64_t next_time_sync_us_ = 0;

    // The negotiated protocol version, 0 until the MCU answered.
    std::uint8_t version_ = 0;
    std::size_t expected_topics_ = 0;
    std::unordered_map<std::uint16_t, PeerTopic> peer_topics_;
    ConnectionInfo info_;
    std::optional<TimeSync> time_sync_;

    std::unordered_map<std::string, Decoder> decoders_;
    // Topic ID to the decoder of an enabled, subscribed topic.
    std::unordered_map<std::uint16_t, const Decoder*> active_;
};

}  // namespace vk::bridge
