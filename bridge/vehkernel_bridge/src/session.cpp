// SPDX-License-Identifier: GPL-3.0-or-later
// Host side of a Link session (ADR-0002, ADR-0027, spec 0015).
#include <vehkernel_bridge/session.hpp>

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace vk::bridge {
namespace {

using vk::link::BodyReader;
using vk::link::BodyWriter;
using vk::link::Frame;
using vk::link::FrameType;

constexpr std::size_t hello_reply_size = 13;
constexpr std::size_t topic_info_fixed_size = 12;
constexpr std::size_t time_sync_reply_size = 24;
constexpr std::size_t read_chunk_size = 1024;
// Bound on the reading of one poll, so a flood cannot hold the node's thread.
constexpr int max_reads_per_poll = 64;

// The protocol version each frame the host sends first appeared in; the host
// refuses to send one the negotiated version lacks (ADR-0002), as HOST_FRAME_VERSIONS does.
std::uint8_t first_version(FrameType type)
{
    switch (type) {
    case FrameType::TopicData:
    case FrameType::Hello:
    case FrameType::TimeSyncRequest:
        return 1;
    case FrameType::ParamListRequest:
    case FrameType::ParamGet:
    case FrameType::ParamSet:
        return 2;
    case FrameType::ModeRequest:
    case FrameType::ArmingRequest:
    case FrameType::EstopRequest:
    case FrameType::BringupRun:
    case FrameType::SimActuatorPower:
    case FrameType::SimUserKey:
        return 3;
    case FrameType::RebootRequest:
    case FrameType::SimGamepad:
        return 4;
    case FrameType::LogStart:
    case FrameType::LogStop:
        return 5;
    default:
        return vk::link::protocol_version;
    }
}

std::string hex(std::uint64_t value)
{
    std::array<char, 19> text{};
    std::snprintf(text.data(), text.size(), "0x%016llX", static_cast<unsigned long long>(value));
    return text.data();
}

}  // namespace

const char* to_string(DisconnectReason reason)
{
    switch (reason) {
    case DisconnectReason::OpenFailed:
        return "open failed";
    case DisconnectReason::HandshakeTimeout:
        return "handshake timeout";
    case DisconnectReason::NoCommonVersion:
        return "no common protocol version";
    case DisconnectReason::RequiredTopicMismatch:
        return "required topic mismatch";
    case DisconnectReason::PeerTimeout:
        return "peer timeout";
    case DisconnectReason::TransportLost:
        return "transport lost";
    }
    return "unknown";
}

Session::Session(Transport& transport, const Clock& clock, SessionConfig config)
    : transport_(transport), clock_(clock), config_(std::move(config)), backoff_us_(config_.backoff_initial_us)
{
}

SessionStats Session::stats() const
{
    SessionStats total = stats_;
    const vk::link::ReaderStats& now = reader_.stats();
    total.frames = earlier_reader_.frames + now.frames;
    total.crc_errors = earlier_reader_.crc_errors + now.crc_errors;
    total.malformed = earlier_reader_.malformed + now.malformed;
    total.lost = earlier_reader_.lost + now.lost;
    return total;
}

std::optional<std::int64_t> Session::mcu_to_host_us(std::uint64_t mcu_us) const
{
    if (!time_sync_.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(mcu_us) - time_sync_->clock_offset_us;
}

void Session::poll()
{
    const std::uint64_t now = clock_.now_us();
    if (state_ == SessionState::Disconnected) {
        if (now >= next_attempt_us_) {
            attempt(now);
        }
        return;
    }

    read_input();
    if (state_ == SessionState::Disconnected) {
        return;
    }

    const std::uint64_t later = clock_.now_us();
    // While handshaking the handshake timeout counts instead: the MCU may be slow to describe its topics.
    if (state_ == SessionState::Connected && later - last_heard_us_ > config_.peer_timeout_us) {
        fail(DisconnectReason::PeerTimeout, "nothing heard from the MCU for " +
                                                std::to_string((later - last_heard_us_) / 1000) + " ms");
        return;
    }
    if (state_ == SessionState::Handshaking) {
        if (later - attempt_started_us_ >= config_.handshake_timeout_us) {
            fail(DisconnectReason::HandshakeTimeout,
                 "the MCU described " + std::to_string(peer_topics_.size()) + " of " +
                     std::to_string(expected_topics_) + " topics in " +
                     std::to_string(config_.handshake_timeout_us / 1000) + " ms");
            return;
        }
        // Until the MCU answers; a hello after that would restart its list of topics.
        if (version_ == 0 && later >= next_hello_us_) {
            std::array<std::uint8_t, 1 + 255> hello{};
            BodyWriter writer(hello);
            writer.u8(static_cast<std::uint8_t>(config_.offered_versions.size()));
            for (const std::uint8_t offered : config_.offered_versions) {
                writer.u8(offered);
            }
            next_hello_us_ = later + config_.hello_interval_us;
            write_frame(FrameType::Hello, writer.written());
        }
    } else if (later >= next_time_sync_us_) {
        send_time_sync(later);
    }
}

void Session::attempt(std::uint64_t now_us)
{
    if (!transport_.open()) {
        fail(DisconnectReason::OpenFailed, std::string("cannot open ") + transport_.description());
        return;
    }
    // A new connection starts with fresh framing: sequence numbers and a partly
    // received frame of the last one mean nothing to this peer.
    earlier_reader_.frames += reader_.stats().frames;
    earlier_reader_.crc_errors += reader_.stats().crc_errors;
    earlier_reader_.malformed += reader_.stats().malformed;
    earlier_reader_.lost += reader_.stats().lost;
    reader_ = vk::link::FrameReader{};
    writer_ = vk::link::FrameWriter{};
    version_ = 0;
    expected_topics_ = 0;
    peer_topics_.clear();
    active_.clear();
    info_ = ConnectionInfo{};
    time_sync_.reset();
    state_ = SessionState::Handshaking;
    attempt_started_us_ = now_us;
    last_heard_us_ = now_us;
    next_hello_us_ = now_us;  // poll() sends the first hello at once
    // The hello goes out at once, not at the next poll.
    poll();
}

void Session::read_input()
{
    std::array<std::uint8_t, read_chunk_size> chunk{};
    for (int i = 0; i < max_reads_per_poll && state_ != SessionState::Disconnected; ++i) {
        const std::ptrdiff_t count = transport_.read(chunk);
        if (count == transport_lost) {
            fail(DisconnectReason::TransportLost, std::string(transport_.description()) + " was lost");
            return;
        }
        if (count == 0) {
            return;
        }
        reader_.feed(std::span(chunk).first(static_cast<std::size_t>(count)), [this](const Frame& frame) {
            // A frame handled may end the connection; the rest of the bytes are then another peer's.
            if (state_ != SessionState::Disconnected) {
                last_heard_us_ = clock_.now_us();
                handle(frame);
            }
        });
    }
}

void Session::handle(const Frame& frame)
{
    switch (frame.type) {
    case FrameType::HelloReply:
        if (state_ == SessionState::Handshaking) {
            on_hello_reply(frame.body);
        }
        break;
    case FrameType::TopicInfo:
        if (state_ == SessionState::Handshaking && version_ != 0) {
            on_topic_info(frame.body);
        }
        break;
    case FrameType::TopicData:
        if (state_ == SessionState::Connected) {
            on_topic_data(frame.body);
        }
        break;
    case FrameType::TimeSyncReply:
        if (state_ == SessionState::Connected) {
            on_time_sync_reply(frame.body);
        }
        break;
    default:
        if (state_ == SessionState::Connected && events.on_frame) {
            events.on_frame(frame);
        }
        break;
    }
}

void Session::on_hello_reply(std::span<const std::uint8_t> body)
{
    // A short reply is a malformed frame, not a refusal: the hello repeats.
    if (body.size() < hello_reply_size) {
        return;
    }
    BodyReader reader(body);
    const std::uint8_t chosen = reader.u8();
    const std::uint16_t topic_count = reader.u16();
    const bool offered = std::find(config_.offered_versions.begin(), config_.offered_versions.end(), chosen) !=
                         config_.offered_versions.end();
    if (chosen == 0 || !offered) {
        fail(DisconnectReason::NoCommonVersion,
             chosen == 0 ? "the MCU speaks none of the offered protocol versions"
                         : "the MCU chose protocol version " + std::to_string(chosen) + ", which was not offered");
        return;
    }
    version_ = chosen;
    expected_topics_ = topic_count;
    info_ = ConnectionInfo{};
    info_.protocol_version = chosen;
    info_.topic_count = topic_count;
    info_.param_set_hash = reader.u64();
    info_.param_count = reader.u16();
    peer_topics_.clear();
    if (topic_count == 0) {
        complete_handshake();
    }
}

void Session::on_topic_info(std::span<const std::uint8_t> body)
{
    if (body.size() < topic_info_fixed_size) {
        return;
    }
    BodyReader reader(body);
    const std::uint16_t id = reader.u16();
    const std::uint8_t instances = reader.u8();
    const std::uint64_t hash = reader.u64();
    const std::uint8_t name_size = reader.u8();
    const auto name = reader.bytes(name_size);
    if (!reader.ok()) {
        return;
    }
    peer_topics_[id] = PeerTopic{instances, hash, std::string(name.begin(), name.end())};
    if (peer_topics_.size() == expected_topics_) {
        complete_handshake();
    }
}

void Session::complete_handshake()
{
    std::vector<std::string> required_mismatches;
    // In topic ID order, so the report reads the same on every run.
    std::vector<std::uint16_t> ids;
    ids.reserve(peer_topics_.size());
    for (const auto& entry : peer_topics_) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    for (const std::uint16_t id : ids) {
        const PeerTopic& peer = peer_topics_.at(id);
        const auto local = std::find_if(local_topics.begin(), local_topics.end(),
                                        [&peer](const LocalTopic& topic) { return topic.name == peer.name; });
        if (local == local_topics.end()) {
            info_.unknown_topics.push_back(peer.name);
        } else if (local->schema_hash != peer.schema_hash) {
            info_.disabled_topics.push_back(DisabledTopic{peer.name, peer.schema_hash, local->schema_hash});
            if (local->required) {
                required_mismatches.push_back(peer.name);
            }
        } else if (const auto decoder = decoders_.find(peer.name); decoder != decoders_.end()) {
            active_[id] = &decoder->second;
        }
    }
    for (const LocalTopic& topic : local_topics) {
        if (topic.required && std::none_of(peer_topics_.begin(), peer_topics_.end(), [&topic](const auto& entry) {
                return entry.second.name == topic.name;
            })) {
            info_.missing_required_topics.emplace_back(topic.name);
        }
    }

    if (!required_mismatches.empty()) {
        std::string detail = "the schema hash of ";
        for (std::size_t i = 0; i < required_mismatches.size(); ++i) {
            const auto mismatch = std::find_if(info_.disabled_topics.begin(), info_.disabled_topics.end(),
                                               [&](const DisabledTopic& topic) { return topic.name == required_mismatches[i]; });
            detail += (i == 0 ? "" : ", ") + required_mismatches[i] + " (MCU " + hex(mismatch->mcu_hash) + ", host " +
                      hex(mismatch->host_hash) + ")";
        }
        detail += required_mismatches.size() == 1 ? ", a required topic, differs"
                                                  : ", required topics, differ";
        fail(DisconnectReason::RequiredTopicMismatch, std::move(detail), std::move(required_mismatches));
        return;
    }

    state_ = SessionState::Connected;
    backoff_us_ = config_.backoff_initial_us;
    ++stats_.connections;
    // The first time synchronization goes out at once, as the Link monitor's does.
    send_time_sync(clock_.now_us());
    if (state_ == SessionState::Connected && events.on_connected) {
        events.on_connected(info_);
    }
}

void Session::on_topic_data(std::span<const std::uint8_t> body)
{
    BodyReader reader(body);
    const std::uint16_t id = reader.u16();
    const std::uint8_t instance = reader.u8();
    if (!reader.ok()) {
        return;
    }
    const auto decoder = active_.find(id);
    if (decoder != active_.end()) {
        (*decoder->second)(instance, reader.rest());
    }
}

void Session::on_time_sync_reply(std::span<const std::uint8_t> body)
{
    if (body.size() < time_sync_reply_size) {
        return;
    }
    BodyReader reader(body);
    TimeSync sync{};
    sync.host_send_us = reader.u64();
    sync.mcu_recv_us = reader.u64();
    sync.mcu_send_us = reader.u64();
    sync.host_recv_us = clock_.now_us();
    // The round trip less the MCU's own time in it, then the NTP clock offset:
    // ((T1 - T0) + (T2 - T3)) / 2, where mcu time = host time + offset.
    sync.rtt_us = static_cast<std::int64_t>(sync.host_recv_us - sync.host_send_us) -
                  static_cast<std::int64_t>(sync.mcu_send_us - sync.mcu_recv_us);
    sync.clock_offset_us = (static_cast<std::int64_t>(sync.mcu_recv_us - sync.host_send_us) +
                            static_cast<std::int64_t>(sync.mcu_send_us - sync.host_recv_us)) /
                           2;
    time_sync_ = sync;
    if (events.on_time_sync) {
        events.on_time_sync(sync);
    }
}

void Session::send_time_sync(std::uint64_t now_us)
{
    std::array<std::uint8_t, 8> body{};
    BodyWriter writer(body);
    writer.u64(now_us);
    next_time_sync_us_ = now_us + config_.time_sync_interval_us;
    write_frame(FrameType::TimeSyncRequest, writer.written());
}

bool Session::send_frame(FrameType type, std::span<const std::uint8_t> body)
{
    if (state_ != SessionState::Connected || first_version(type) > version_) {
        return false;
    }
    return write_frame(type, body);
}

bool Session::send_topic_data(std::string_view name, std::uint8_t instance, std::span<const std::uint8_t> message)
{
    if (state_ != SessionState::Connected) {
        return false;
    }
    const auto disabled = std::find_if(info_.disabled_topics.begin(), info_.disabled_topics.end(),
                                       [name](const DisabledTopic& topic) { return topic.name == name; });
    if (disabled != info_.disabled_topics.end()) {
        return false;
    }
    const auto peer = std::find_if(peer_topics_.begin(), peer_topics_.end(),
                                   [name](const auto& entry) { return entry.second.name == name; });
    if (peer == peer_topics_.end()) {
        return false;
    }
    std::array<std::uint8_t, vk::link::max_body_size> body{};
    if (message.size() + 3 > body.size()) {
        return false;
    }
    BodyWriter writer(body);
    writer.u16(peer->first);
    writer.u8(instance);
    writer.bytes(message);
    return write_frame(FrameType::TopicData, writer.written());
}

bool Session::write_frame(FrameType type, std::span<const std::uint8_t> body)
{
    const std::size_t size = writer_.write(type, body, wire_);
    if (size == 0) {
        return false;
    }
    if (!transport_.write(std::span<const std::uint8_t>(wire_).first(size))) {
        // The frame never left, so the peer does not count its number as lost.
        writer_.retract();
        if (state_ != SessionState::Disconnected) {
            fail(DisconnectReason::TransportLost, std::string("cannot write to ") + transport_.description());
        }
        return false;
    }
    return true;
}

void Session::fail(DisconnectReason reason, std::string detail, std::vector<std::string> required_topics)
{
    transport_.close();
    state_ = SessionState::Disconnected;
    active_.clear();
    next_attempt_us_ = clock_.now_us() + backoff_us_;
    backoff_us_ = std::min(backoff_us_ * 2, config_.backoff_max_us);
    if (events.on_disconnected) {
        events.on_disconnected(Disconnect{reason, std::move(detail), std::move(required_topics)});
    }
}

}  // namespace vk::bridge
