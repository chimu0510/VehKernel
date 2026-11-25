// SPDX-License-Identifier: GPL-3.0-or-later
// L1 tests of the Bridge's session (ADR-0018, ADR-0027, spec 0015 seam D): the
// session against a fake transport and a fake clock, with the MCU's frames built
// by the generated encoders and the Link's frame code.
#include <vehkernel_bridge/session.hpp>

#include <vk/link/frame.hpp>
#include <vk/msg/body_velocity_setpoint.hpp>
#include <vk/msg/imu_sample.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <vector>

namespace vk::bridge {
namespace {

using vk::link::BodyReader;
using vk::link::BodyWriter;
using vk::link::FrameType;

constexpr std::uint64_t ms = 1000;
constexpr std::uint64_t second = 1'000'000;

class FakeClock final : public Clock {
public:
    [[nodiscard]] std::uint64_t now_us() const override { return now_; }
    void advance(std::uint64_t us) { now_ += us; }

private:
    std::uint64_t now_ = 5 * second;
};

class FakeTransport final : public Transport {
public:
    bool open() override
    {
        ++open_calls;
        if (!present) {
            return false;
        }
        is_open = true;
        lost = false;
        return true;
    }
    void close() override
    {
        is_open = false;
        ++close_calls;
    }
    std::ptrdiff_t read(std::span<std::uint8_t> out) override
    {
        if (lost) {
            return transport_lost;
        }
        const std::size_t count = std::min(out.size(), incoming.size());
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = incoming.front();
            incoming.pop_front();
        }
        return static_cast<std::ptrdiff_t>(count);
    }
    bool write(std::span<const std::uint8_t> data) override
    {
        if (lost) {
            return false;
        }
        outgoing.insert(outgoing.end(), data.begin(), data.end());
        return true;
    }
    [[nodiscard]] const char* description() const override { return "fake transport"; }

    bool present = true;  // a device that can be opened
    bool lost = false;    // a device that went away while open
    bool is_open = false;
    int open_calls = 0;
    int close_calls = 0;
    std::deque<std::uint8_t> incoming;
    std::vector<std::uint8_t> outgoing;
};

struct HostFrame {
    std::uint8_t version;
    FrameType type;
    std::vector<std::uint8_t> body;
};

struct Announced {
    std::uint16_t id;
    std::uint8_t instances;
    std::uint64_t hash;
    std::string name;
};

template <class Message>
Announced announce(std::uint64_t hash = Message::schema_hash)
{
    return Announced{Message::topic_id, static_cast<std::uint8_t>(Message::instances), hash,
                     std::string(Message::topic_name)};
}

// The topics a vehicle offers a Bridge: the three it requires and the IMU.
std::vector<Announced> vehicle_topics()
{
    return {announce<vk::msg::BodyVelocitySetpoint>(), announce<vk::msg::VehicleStatus>(),
            announce<vk::msg::VehicleLocalOdometry>(), announce<vk::msg::ImuSample>()};
}

class SessionTest : public ::testing::Test {
protected:
    SessionTest() : session(transport, clock)
    {
        session.events.on_connected = [this](const ConnectionInfo& info) { connections.push_back(info); };
        session.events.on_disconnected = [this](const Disconnect& disconnect) { disconnects.push_back(disconnect); };
        session.events.on_time_sync = [this](const TimeSync& sync) { syncs.push_back(sync); };
        session.events.on_frame = [this](const vk::link::Frame& frame) {
            other_frames.push_back(frame.type);
        };
    }

    // What the host wrote since the last call.
    std::vector<HostFrame> sent()
    {
        std::vector<HostFrame> frames;
        host_reader.feed(transport.outgoing, [&frames](const vk::link::Frame& frame) {
            frames.push_back({frame.version, frame.type, {frame.body.begin(), frame.body.end()}});
        });
        transport.outgoing.clear();
        return frames;
    }

    std::vector<HostFrame> sent_of(FrameType type)
    {
        std::vector<HostFrame> frames = sent();
        std::erase_if(frames, [type](const HostFrame& frame) { return frame.type != type; });
        return frames;
    }

    // The MCU sends one frame; the session reads it at its next poll.
    void mcu_sends(FrameType type, std::span<const std::uint8_t> body)
    {
        std::array<std::uint8_t, vk::link::max_wire_size> wire{};
        const std::size_t size = mcu_writer.write(type, body, wire);
        ASSERT_GT(size, 0U);
        transport.incoming.insert(transport.incoming.end(), wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(size));
    }

    void mcu_hello_reply(std::uint8_t chosen, std::uint16_t topic_count)
    {
        std::array<std::uint8_t, 13> body{};
        BodyWriter writer(body);
        writer.u8(chosen);
        writer.u16(topic_count);
        writer.u64(0x1234);
        writer.u16(7);
        mcu_sends(FrameType::HelloReply, writer.written());
    }

    void mcu_topic_info(const Announced& topic)
    {
        std::array<std::uint8_t, vk::link::max_body_size> body{};
        BodyWriter writer(body);
        writer.u16(topic.id);
        writer.u8(topic.instances);
        writer.u64(topic.hash);
        writer.u8(static_cast<std::uint8_t>(topic.name.size()));
        writer.bytes({reinterpret_cast<const std::uint8_t*>(topic.name.data()), topic.name.size()});
        mcu_sends(FrameType::TopicInfo, writer.written());
    }

    template <class Message>
    void mcu_topic_data(const Message& message, std::uint8_t instance = 0)
    {
        std::array<std::uint8_t, vk::link::max_body_size> body{};
        BodyWriter writer(body);
        writer.u16(Message::topic_id);
        writer.u8(instance);
        std::array<std::uint8_t, Message::encoded_size> encoded{};
        ASSERT_EQ(message.encode(encoded), Message::encoded_size);
        writer.bytes(encoded);
        mcu_sends(FrameType::TopicData, writer.written());
    }

    // The MCU answers the host's hello and describes `topics`.
    void mcu_handshake(const std::vector<Announced>& topics, std::uint8_t chosen = vk::link::protocol_version)
    {
        mcu_hello_reply(chosen, static_cast<std::uint16_t>(topics.size()));
        for (const Announced& topic : topics) {
            mcu_topic_info(topic);
        }
    }

    // A time_sync_reply to the newest request, as an MCU whose clock runs `offset_us` ahead
    // of the host's would send it, `one_way_us` after the request left.
    void mcu_time_sync_reply(const HostFrame& request, std::int64_t offset_us, std::uint64_t one_way_us,
                             std::uint64_t processing_us = 0)
    {
        BodyReader reader(request.body);
        const std::uint64_t host_send = reader.u64();
        const std::uint64_t mcu_recv = host_send + one_way_us + static_cast<std::uint64_t>(offset_us);
        std::array<std::uint8_t, 24> body{};
        BodyWriter writer(body);
        writer.u64(host_send);
        writer.u64(mcu_recv);
        writer.u64(mcu_recv + processing_us);
        mcu_sends(FrameType::TimeSyncReply, writer.written());
    }

    // Polls every `step_us` for `duration_us`, as the node's timer does.
    void run(std::uint64_t duration_us, std::uint64_t step_us = 2 * ms)
    {
        for (std::uint64_t elapsed = 0; elapsed < duration_us; elapsed += step_us) {
            clock.advance(step_us);
            session.poll();
        }
    }

    // A connected session with the vehicle's topics, and the host's frames so far dropped.
    void connect(const std::vector<Announced>& topics = vehicle_topics(), std::uint8_t chosen = vk::link::protocol_version)
    {
        session.poll();
        mcu_handshake(topics, chosen);
        session.poll();
        ASSERT_TRUE(session.connected());
        sent();
    }

    FakeClock clock;
    FakeTransport transport;
    Session session;
    vk::link::FrameReader host_reader;  // reads what the host wrote
    vk::link::FrameWriter mcu_writer;   // numbers the frames the MCU wrote
    std::vector<ConnectionInfo> connections;
    std::vector<Disconnect> disconnects;
    std::vector<TimeSync> syncs;
    std::vector<FrameType> other_frames;
};

TEST_F(SessionTest, OpensTheTransportAndOffersEveryVersionFromTwo)
{
    EXPECT_EQ(session.state(), SessionState::Disconnected);
    session.poll();
    EXPECT_TRUE(transport.is_open);
    EXPECT_EQ(session.state(), SessionState::Handshaking);

    const auto hellos = sent_of(FrameType::Hello);
    ASSERT_EQ(hellos.size(), 1U);
    EXPECT_EQ(hellos[0].version, vk::link::protocol_version);
    EXPECT_EQ(hellos[0].body, (std::vector<std::uint8_t>{4, 2, 3, 4, 5}));
}

TEST_F(SessionTest, RepeatsHelloUntilTheMcuAnswersThenNoMore)
{
    session.poll();
    run(1000 * ms);
    const std::size_t unanswered = sent_of(FrameType::Hello).size();
    EXPECT_GE(unanswered, 3U);  // every 250 ms; the first went out at the first poll

    mcu_hello_reply(vk::link::protocol_version, 2);
    session.poll();
    sent();
    run(1000 * ms);
    // A hello after the answer would restart the MCU's list of topics.
    EXPECT_EQ(sent_of(FrameType::Hello).size(), 0U);
}

TEST_F(SessionTest, HandshakeConnectsAndReportsWhatItFound)
{
    session.poll();
    sent();
    mcu_handshake(vehicle_topics());
    session.poll();

    ASSERT_TRUE(session.connected());
    ASSERT_EQ(connections.size(), 1U);
    const ConnectionInfo& info = connections[0];
    EXPECT_EQ(info.protocol_version, vk::link::protocol_version);
    EXPECT_EQ(info.topic_count, 4U);
    EXPECT_EQ(info.param_set_hash, 0x1234U);
    EXPECT_EQ(info.param_count, 7U);
    EXPECT_TRUE(info.disabled_topics.empty());
    EXPECT_TRUE(info.unknown_topics.empty());
    EXPECT_TRUE(info.missing_required_topics.empty());
    EXPECT_TRUE(disconnects.empty());
}

TEST_F(SessionTest, TheFirstTimeSyncRequestGoesOutAtOnceAfterTheHandshake)
{
    session.poll();
    sent();
    mcu_handshake(vehicle_topics());
    session.poll();
    const auto requests = sent_of(FrameType::TimeSyncRequest);
    ASSERT_EQ(requests.size(), 1U);
    BodyReader reader(requests[0].body);
    EXPECT_EQ(reader.u64(), clock.now_us());
}

TEST_F(SessionTest, NegotiatesTheVersionTheMcuChoosesAndGatesFramesByIt)
{
    connect(vehicle_topics(), 3);
    EXPECT_EQ(session.info().protocol_version, 3U);

    const std::array<std::uint8_t, 1> mode{3};
    EXPECT_TRUE(session.send_frame(FrameType::ModeRequest, mode));  // version 3 has it
    EXPECT_EQ(sent_of(FrameType::ModeRequest).size(), 1U);
    const std::array<std::uint8_t, 1> reboot{0};
    EXPECT_FALSE(session.send_frame(FrameType::RebootRequest, reboot));  // version 4 has it
    const std::array<std::uint8_t, 4> profile{};
    EXPECT_FALSE(session.send_frame(FrameType::LogStart, profile));  // version 5 has it
    EXPECT_TRUE(sent().empty());
}

TEST_F(SessionTest, AFrameNeedsAConnection)
{
    const std::array<std::uint8_t, 1> mode{3};
    EXPECT_FALSE(session.send_frame(FrameType::ModeRequest, mode));
    session.poll();
    EXPECT_FALSE(session.send_frame(FrameType::ModeRequest, mode));  // handshaking
}

TEST_F(SessionTest, RefusesAVersionItDidNotOffer)
{
    session.poll();
    mcu_hello_reply(6, 4);
    session.poll();
    EXPECT_FALSE(session.connected());
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::NoCommonVersion);
}

TEST_F(SessionTest, AShortHelloReplyIsIgnoredAndTheHelloRepeats)
{
    session.poll();
    sent();
    // A valid chosen version and topic count, but without the parameter fields.
    const std::array<std::uint8_t, 3> shortened{vk::link::protocol_version, 4, 0};
    mcu_sends(FrameType::HelloReply, shortened);
    session.poll();
    EXPECT_TRUE(disconnects.empty());
    EXPECT_EQ(session.state(), SessionState::Handshaking);

    run(300 * ms);
    EXPECT_FALSE(sent_of(FrameType::Hello).empty());  // still asking
    mcu_handshake(vehicle_topics());
    session.poll();
    EXPECT_TRUE(session.connected());
}

TEST_F(SessionTest, AnMcuWithNoCommonVersionAnswersZeroAndIsGivenUpOn)
{
    session.poll();
    mcu_hello_reply(0, 0);
    session.poll();
    EXPECT_FALSE(session.connected());
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::NoCommonVersion);
    EXPECT_FALSE(transport.is_open);
}

TEST_F(SessionTest, ADifferingTopicIsDisabledAndTheConnectionStays)
{
    std::vector<Announced> topics = vehicle_topics();
    topics.back() = announce<vk::msg::ImuSample>(vk::msg::ImuSample::schema_hash + 1);
    topics.push_back(Announced{200, 1, 0xABCD, "some_future_topic"});

    int imu = 0;
    int status = 0;
    session.subscribe<vk::msg::ImuSample>([&imu](std::uint8_t, const vk::msg::ImuSample&) { ++imu; });
    session.subscribe<vk::msg::VehicleStatus>([&status](std::uint8_t, const vk::msg::VehicleStatus&) { ++status; });
    connect(topics);

    ASSERT_EQ(connections.size(), 1U);
    ASSERT_EQ(connections[0].disabled_topics.size(), 1U);
    EXPECT_EQ(connections[0].disabled_topics[0].name, "imu_sample");
    EXPECT_EQ(connections[0].disabled_topics[0].mcu_hash, vk::msg::ImuSample::schema_hash + 1);
    EXPECT_EQ(connections[0].disabled_topics[0].host_hash, vk::msg::ImuSample::schema_hash);
    EXPECT_EQ(connections[0].unknown_topics, (std::vector<std::string>{"some_future_topic"}));

    // The disabled topic's data is dropped; the topic that agrees is delivered.
    mcu_topic_data(vk::msg::ImuSample{});
    mcu_topic_data(vk::msg::VehicleStatus{});
    session.poll();
    EXPECT_EQ(imu, 0);
    EXPECT_EQ(status, 1);
}

TEST_F(SessionTest, ADifferingRequiredTopicRefusesTheConnectionNamingIt)
{
    std::vector<Announced> topics = vehicle_topics();
    topics[0] = announce<vk::msg::BodyVelocitySetpoint>(vk::msg::BodyVelocitySetpoint::schema_hash ^ 0xFF);
    session.poll();
    mcu_handshake(topics);
    session.poll();

    EXPECT_FALSE(session.connected());
    EXPECT_TRUE(connections.empty());
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::RequiredTopicMismatch);
    EXPECT_EQ(disconnects[0].required_topics, (std::vector<std::string>{"body_velocity_setpoint"}));
    EXPECT_NE(disconnects[0].detail.find("body_velocity_setpoint"), std::string::npos);
    EXPECT_FALSE(transport.is_open);
}

TEST_F(SessionTest, EveryDifferingRequiredTopicIsNamed)
{
    std::vector<Announced> topics = vehicle_topics();
    topics[1] = announce<vk::msg::VehicleStatus>(1);
    topics[2] = announce<vk::msg::VehicleLocalOdometry>(2);
    topics[3] = announce<vk::msg::ImuSample>(3);  // not required: not named
    session.poll();
    mcu_handshake(topics);
    session.poll();

    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects[0].required_topics, (std::vector<std::string>{"vehicle_local_odometry", "vehicle_status"}));
    EXPECT_EQ(disconnects[0].detail.find("imu_sample"), std::string::npos);
}

TEST_F(SessionTest, ARequiredTopicTheMcuDoesNotOfferIsReportedNotRefused)
{
    connect({announce<vk::msg::VehicleStatus>(), announce<vk::msg::VehicleLocalOdometry>()});
    EXPECT_EQ(connections[0].missing_required_topics, (std::vector<std::string>{"body_velocity_setpoint"}));
}

TEST_F(SessionTest, DecodesTopicDataWithTheGeneratedHeaders)
{
    std::vector<std::pair<std::uint8_t, vk::msg::VehicleStatus>> received;
    session.subscribe<vk::msg::VehicleStatus>(
        [&received](std::uint8_t instance, const vk::msg::VehicleStatus& status) { received.emplace_back(instance, status); });
    connect();

    vk::msg::VehicleStatus status;
    status.mode = 3;
    status.armed = true;
    status.source = 1;
    status.timestamp = 123456;
    mcu_topic_data(status, 0);
    session.poll();

    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received[0].first, 0U);
    EXPECT_EQ(received[0].second.mode, 3U);
    EXPECT_TRUE(received[0].second.armed);
    EXPECT_EQ(received[0].second.source, 1U);
    EXPECT_EQ(received[0].second.timestamp, 123456U);
    EXPECT_EQ(session.stats().topic_messages, 1U);
}

TEST_F(SessionTest, TopicDataOfTheWrongSizeIsCountedAndDropped)
{
    int received = 0;
    session.subscribe<vk::msg::VehicleStatus>([&received](std::uint8_t, const vk::msg::VehicleStatus&) { ++received; });
    connect();

    std::array<std::uint8_t, 8> body{};
    BodyWriter writer(body);
    writer.u16(vk::msg::VehicleStatus::topic_id);
    writer.u8(0);
    writer.u16(1);  // a message too short
    mcu_sends(FrameType::TopicData, writer.written());
    session.poll();
    EXPECT_EQ(received, 0);
    EXPECT_EQ(session.stats().decode_failures, 1U);
}

TEST_F(SessionTest, TimeSynchronizationComputesTheOffsetAndRoundTrip)
{
    connect();
    clock.advance(500 * ms);
    session.poll();
    const auto requests = sent_of(FrameType::TimeSyncRequest);
    ASSERT_EQ(requests.size(), 1U);  // connect() consumed the first request; this is the one 500 ms later

    // The MCU's clock is 7.5 s ahead; the request takes 350 us to get there and the
    // reply 350 us back, and the MCU needs 100 us in between.
    clock.advance(800);
    mcu_time_sync_reply(requests[0], 7'500'000, 350, 100);
    session.poll();

    ASSERT_EQ(syncs.size(), 1U);
    EXPECT_EQ(syncs[0].clock_offset_us, 7'500'000);
    EXPECT_EQ(syncs[0].rtt_us, 700);
    ASSERT_TRUE(session.time_sync().has_value());
    EXPECT_EQ(session.mcu_to_host_us(10'000'000 + 400), static_cast<std::int64_t>(2'500'400));
}

TEST_F(SessionTest, TimeSynchronizationHandlesAnMcuClockBehindTheHost)
{
    connect();
    clock.advance(500 * ms);
    session.poll();
    const auto requests = sent_of(FrameType::TimeSyncRequest);
    ASSERT_EQ(requests.size(), 1U);
    clock.advance(1000);
    mcu_time_sync_reply(requests[0], -4'000'000, 500, 0);
    session.poll();
    ASSERT_EQ(syncs.size(), 1U);
    EXPECT_EQ(syncs[0].clock_offset_us, -4'000'000);
    EXPECT_EQ(syncs[0].rtt_us, 1000);
}

TEST_F(SessionTest, NoHostTimeBeforeTheFirstTimeSynchronization)
{
    connect();
    EXPECT_FALSE(session.mcu_to_host_us(1000).has_value());
}

TEST_F(SessionTest, TimeSynchronizationRepeatsEveryHalfSecond)
{
    connect();
    run(2900 * ms);  // the MCU is silent, so stay inside the peer timeout
    EXPECT_EQ(sent_of(FrameType::TimeSyncRequest).size(), 5U);  // at 0.5, 1.0, 1.5, 2.0 and 2.5 s
}

TEST_F(SessionTest, KeepsTheConnectionWhileTheMcuKeepsAnswering)
{
    connect();
    for (int i = 0; i < 40; ++i) {  // 20 s
        clock.advance(500 * ms);
        session.poll();
        const auto requests = sent_of(FrameType::TimeSyncRequest);
        ASSERT_EQ(requests.size(), 1U);
        mcu_time_sync_reply(requests[0], 1000, 200);
        session.poll();
    }
    EXPECT_TRUE(session.connected());
    EXPECT_EQ(syncs.size(), 40U);
    EXPECT_TRUE(disconnects.empty());
}

TEST_F(SessionTest, TopicDataAlsoKeepsTheConnectionAlive)
{
    connect();
    for (int i = 0; i < 20; ++i) {
        clock.advance(500 * ms);
        mcu_topic_data(vk::msg::VehicleStatus{});
        session.poll();
    }
    EXPECT_TRUE(session.connected());
}

TEST_F(SessionTest, PeerTimeoutEndsTheConnectionAndClosesTheTransport)
{
    connect();
    run(2900 * ms);
    EXPECT_TRUE(session.connected());
    run(200 * ms);
    EXPECT_FALSE(session.connected());
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::PeerTimeout);
    EXPECT_FALSE(transport.is_open);
}

TEST_F(SessionTest, ReconnectsAfterAnMcuRebootWithBackoff)
{
    connect();
    const int opened = transport.open_calls;
    run(3100 * ms);  // the MCU rebooted: it no longer answers
    ASSERT_EQ(disconnects.size(), 1U);

    // The first retry comes after the initial backoff, not at once.
    sent();
    run(400 * ms);
    EXPECT_EQ(transport.open_calls, opened);
    run(200 * ms);
    EXPECT_EQ(transport.open_calls, opened + 1);
    EXPECT_EQ(session.state(), SessionState::Handshaking);
    EXPECT_EQ(sent_of(FrameType::Hello).empty(), false);

    // The rebooted MCU numbers its frames from zero again and describes the topics afresh.
    mcu_writer = vk::link::FrameWriter{};
    mcu_handshake(vehicle_topics());
    session.poll();
    EXPECT_TRUE(session.connected());
    EXPECT_EQ(connections.size(), 2U);
    EXPECT_EQ(session.stats().connections, 2U);
    // The restart of the numbering is not counted as lost frames.
    EXPECT_EQ(session.stats().lost, 0U);
}

TEST_F(SessionTest, BackoffDoublesToItsMaximumAndAConnectionResetsIt)
{
    transport.present = false;  // the device is not there
    std::vector<std::uint64_t> attempts;
    int seen = 0;
    const std::uint64_t start = clock.now_us();
    for (std::uint64_t elapsed = 0; elapsed < 20 * second; elapsed += 10 * ms) {
        clock.advance(10 * ms);
        session.poll();
        if (transport.open_calls != seen) {
            seen = transport.open_calls;
            attempts.push_back(clock.now_us() - start);
        }
    }
    // At once, then after 0.5 s, 1 s, 2 s, 4 s, then every 5 s.
    ASSERT_GE(attempts.size(), 7U);
    EXPECT_LE(attempts[0], 10 * ms);
    EXPECT_EQ(attempts[1] - attempts[0], 500 * ms);
    EXPECT_EQ(attempts[2] - attempts[1], 1000 * ms);
    EXPECT_EQ(attempts[3] - attempts[2], 2000 * ms);
    EXPECT_EQ(attempts[4] - attempts[3], 4000 * ms);
    EXPECT_EQ(attempts[5] - attempts[4], 5000 * ms);
    EXPECT_EQ(attempts[6] - attempts[5], 5000 * ms);
    for (const Disconnect& disconnect : disconnects) {
        EXPECT_EQ(disconnect.reason, DisconnectReason::OpenFailed);
    }

    // Plugged in at last: it connects, and the next failure starts from the initial backoff.
    transport.present = true;
    run(6 * second);
    mcu_handshake(vehicle_topics());
    session.poll();
    ASSERT_TRUE(session.connected());
    disconnects.clear();
    transport.lost = true;
    session.poll();
    ASSERT_EQ(disconnects.size(), 1U);
    transport.lost = false;
    const int opened = transport.open_calls;
    run(600 * ms);
    EXPECT_EQ(transport.open_calls, opened + 1);
}

TEST_F(SessionTest, ALostDeviceEndsTheConnectionAndIsOpenedAgainWhenItIsBack)
{
    connect();
    transport.lost = true;
    transport.present = false;  // unplugged
    session.poll();
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::TransportLost);
    EXPECT_FALSE(transport.is_open);

    run(1200 * ms);
    EXPECT_FALSE(disconnects.empty());
    EXPECT_EQ(disconnects.back().reason, DisconnectReason::OpenFailed);

    transport.present = true;  // plugged back in
    run(5 * second);
    EXPECT_EQ(session.state(), SessionState::Handshaking);
    mcu_handshake(vehicle_topics());
    session.poll();
    EXPECT_TRUE(session.connected());
}

TEST_F(SessionTest, AFailedWriteIsALostTransport)
{
    session.poll();
    mcu_handshake(vehicle_topics());
    session.poll();
    ASSERT_TRUE(session.connected());
    transport.lost = true;
    clock.advance(600 * ms);
    session.poll();  // the time synchronization request cannot be written, nor anything read
    EXPECT_FALSE(session.connected());
    ASSERT_FALSE(disconnects.empty());
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::TransportLost);
}

TEST_F(SessionTest, AHandshakeThatNeverCompletesTimesOut)
{
    session.poll();
    mcu_hello_reply(vk::link::protocol_version, 4);
    mcu_topic_info(announce<vk::msg::VehicleStatus>());  // 1 of 4
    run(4900 * ms);
    EXPECT_EQ(session.state(), SessionState::Handshaking);
    run(200 * ms);
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::HandshakeTimeout);
    EXPECT_NE(disconnects[0].detail.find("1 of 4"), std::string::npos);
}

TEST_F(SessionTest, AHandshakeWithNoTopicsCompletesAtOnce)
{
    session.poll();
    mcu_hello_reply(vk::link::protocol_version, 0);
    session.poll();
    EXPECT_TRUE(session.connected());
    EXPECT_EQ(connections[0].missing_required_topics.size(), 3U);
}

TEST_F(SessionTest, FramesItDoesNotConsumeGoToTheOwnerWhileConnected)
{
    connect();
    const std::array<std::uint8_t, 1> mode{3};
    mcu_sends(FrameType::ModeReply, mode);
    session.poll();
    EXPECT_EQ(other_frames, (std::vector<FrameType>{FrameType::ModeReply}));
}

TEST_F(SessionTest, CorruptedFramesAreCountedAndTheSessionCarriesOn)
{
    connect();
    // Noise, then a frame whose CRC is wrong, then a good one.
    for (const std::uint8_t byte : {0x55, 0xAA, 0x01, 0x00}) {
        transport.incoming.push_back(byte);
    }
    mcu_topic_data(vk::msg::VehicleStatus{});
    session.poll();
    EXPECT_TRUE(session.connected());
    EXPECT_GE(session.stats().crc_errors + session.stats().malformed, 1U);
}

TEST_F(SessionTest, HostFramesAreNumberedInOrder)
{
    session.poll();
    run(600 * ms);
    const auto frames = sent();
    ASSERT_GE(frames.size(), 2U);
    // The host numbers its frames in order, so the MCU counts nothing as lost.
    EXPECT_EQ(host_reader.stats().lost, 0U);
}

}  // namespace
}  // namespace vk::bridge
