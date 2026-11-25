// SPDX-License-Identifier: GPL-3.0-or-later
// L1 tests of the request tracker behind the arming, mode and emergency stop
// services (ADR-0002, ADR-0005, ADR-0018, spec 0015 seam D): with a fake send
// function and the time it is given, for an accepted, a refused and an unanswered
// request.
#include <vehkernel_bridge/request_tracker.hpp>

#include <vk/msg/enums.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace vk::bridge {
namespace {

using vk::link::BodyWriter;
using vk::link::Frame;
using vk::link::FrameType;

constexpr std::uint64_t ms = 1000;
constexpr auto companion = static_cast<std::uint8_t>(vk::msg::Mode::Companion);
constexpr auto test_mode = static_cast<std::uint8_t>(vk::msg::Mode::Test);

struct Sent {
    FrameType type;
    std::vector<std::uint8_t> body;
};

class TrackerTest : public ::testing::Test {
protected:
    TrackerTest()
        : tracker(
              [this](FrameType type, std::span<const std::uint8_t> body) {
                  if (!connected) {
                      return false;
                  }
                  sent.push_back({type, {body.begin(), body.end()}});
                  return true;
              },
              TrackerConfig{250 * ms, 2'000 * ms})
    {
    }

    RequestTracker::Done record()
    {
        return [this](const Reply& reply) { replies.push_back(reply); };
    }

    bool deliver(FrameType type, std::initializer_list<std::uint8_t> bytes)
    {
        const std::vector<std::uint8_t> body(bytes);
        return tracker.on_frame(Frame{5, type, 0, body});
    }

    static std::vector<std::uint8_t> arming_reply_body(bool armed)
    {
        std::array<std::uint8_t, 5> body{};
        BodyWriter writer(body);
        writer.u8(armed ? 1 : 0);
        writer.u32(0);
        return {body.begin(), body.end()};
    }

    bool deliver_arming_reply(bool armed)
    {
        const auto body = arming_reply_body(armed);
        return tracker.on_frame(Frame{5, FrameType::ArmingReply, 0, body});
    }

    // Polls every 2 ms until `until_us`.
    void run_until(std::uint64_t until_us)
    {
        for (; now < until_us; now += 2 * ms) {
            tracker.poll(now);
        }
    }

    bool connected = true;
    std::uint64_t now = 1'000'000;
    std::vector<Sent> sent;
    std::vector<Reply> replies;
    RequestTracker tracker;
};

TEST_F(TrackerTest, ASentRequestCarriesItsArgumentAndWaitsForTheAnswer)
{
    tracker.submit(Command::Mode, companion, record(), now);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].type, FrameType::ModeRequest);
    EXPECT_EQ(sent[0].body, (std::vector<std::uint8_t>{companion}));
    EXPECT_TRUE(replies.empty());
    EXPECT_TRUE(tracker.pending(Command::Mode));
}

TEST_F(TrackerTest, AcceptedModeArmingAndEmergencyStop)
{
    tracker.submit(Command::Mode, companion, record(), now);
    EXPECT_TRUE(deliver(FrameType::ModeReply, {companion}));
    tracker.submit(Command::Arming, 1, record(), now);
    EXPECT_EQ(sent[1].type, FrameType::ArmingRequest);
    EXPECT_TRUE(deliver_arming_reply(true));
    tracker.submit(Command::EmergencyStop, 1, record(), now);
    EXPECT_EQ(sent[2].type, FrameType::EstopRequest);
    EXPECT_TRUE(deliver(FrameType::EstopReply, {1}));

    ASSERT_EQ(replies.size(), 3U);
    for (const Reply& reply : replies) {
        EXPECT_TRUE(reply.accepted());
        EXPECT_TRUE(reply.reason.empty());
    }
    EXPECT_EQ(replies[0].state, companion);
    EXPECT_FALSE(tracker.pending(Command::Mode));
}

TEST_F(TrackerTest, ADisarmAndAReleaseAreAcceptedWhenTheStateIsOff)
{
    tracker.submit(Command::Arming, 0, record(), now);
    EXPECT_TRUE(deliver_arming_reply(false));
    tracker.submit(Command::EmergencyStop, 0, record(), now);
    EXPECT_TRUE(deliver(FrameType::EstopReply, {0}));
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_TRUE(replies[0].accepted());
    EXPECT_TRUE(replies[1].accepted());
}

TEST_F(TrackerTest, ARefusedModeReturnsTheMcusReason)
{
    tracker.submit(Command::Mode, companion, record(), now);
    // mode_reject: the reason, then the mode still in effect.
    EXPECT_TRUE(deliver(FrameType::ModeReject, {static_cast<std::uint8_t>(vk::msg::ModeResult::Armed), test_mode}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].outcome, Outcome::Refused);
    EXPECT_NE(replies[0].reason.find("only while disarmed"), std::string::npos) << replies[0].reason;
    EXPECT_EQ(replies[0].state, test_mode);
}

TEST_F(TrackerTest, ARefusedArmingNamesTheArmingCheckThatFailed)
{
    tracker.submit(Command::Arming, 1, record(), now);
    EXPECT_TRUE(deliver(FrameType::ArmingReject, {static_cast<std::uint8_t>(vk::msg::ArmingResult::WrongMode)}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].outcome, Outcome::Refused);
    EXPECT_EQ(replies[0].reason, "arming refused: wrong mode");

    tracker.submit(Command::Arming, 1, record(), now);
    EXPECT_TRUE(deliver(FrameType::ArmingReject, {static_cast<std::uint8_t>(vk::msg::ArmingResult::EmergencyStop)}));
    EXPECT_EQ(replies[1].reason, "arming refused: emergency stop");

    tracker.submit(Command::Arming, 1, record(), now);
    EXPECT_TRUE(deliver(FrameType::ArmingReject, {200}));
    EXPECT_EQ(replies[2].reason, "arming refused: reason 200");
}

TEST_F(TrackerTest, AnAnswerThatShowsAnotherStateIsRefused)
{
    tracker.submit(Command::Mode, companion, record(), now);
    EXPECT_TRUE(deliver(FrameType::ModeReply, {test_mode}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].outcome, Outcome::Refused);
    EXPECT_EQ(replies[0].reason, "the vehicle is in mode TEST, not COMPANION");
}

TEST_F(TrackerTest, AnUnansweredRequestGoesAgainThenTimesOut)
{
    tracker.submit(Command::Arming, 1, record(), now);
    const std::uint64_t submitted = now;
    run_until(submitted + 1'000 * ms);
    // The request at 0 and again every 250 ms: 250, 500, 750.
    EXPECT_EQ(sent.size(), 4U);
    EXPECT_TRUE(replies.empty());
    for (const Sent& frame : sent) {
        EXPECT_EQ(frame.type, FrameType::ArmingRequest);
    }

    run_until(submitted + 2'100 * ms);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].outcome, Outcome::TimedOut);
    EXPECT_FALSE(replies[0].reason.empty());
    EXPECT_FALSE(replies[0].accepted());
    EXPECT_FALSE(tracker.pending(Command::Arming));
    // And no more repeats once it has ended.
    const std::size_t sent_by_then = sent.size();
    run_until(now + 1'000 * ms);
    EXPECT_EQ(sent.size(), sent_by_then);
}

TEST_F(TrackerTest, AnAnswerAfterARepeatEndsItAndALateDuplicateIsIgnored)
{
    tracker.submit(Command::Mode, companion, record(), now);
    run_until(now + 600 * ms);
    EXPECT_GE(sent.size(), 3U);
    EXPECT_TRUE(deliver(FrameType::ModeReply, {companion}));
    EXPECT_TRUE(deliver(FrameType::ModeReply, {companion}));  // the answer to the repeat
    EXPECT_EQ(replies.size(), 1U);
}

TEST_F(TrackerTest, AnAnswerOfAnotherKindDoesNotEndTheRequest)
{
    tracker.submit(Command::Arming, 1, record(), now);
    EXPECT_TRUE(deliver(FrameType::ModeReply, {companion}));
    EXPECT_TRUE(replies.empty());
    EXPECT_TRUE(tracker.pending(Command::Arming));
}

TEST_F(TrackerTest, FramesThatAreNotAnswersAreNotConsumed)
{
    EXPECT_FALSE(deliver(FrameType::ParamValue, {0, 0, 0, 0, 0}));
    EXPECT_FALSE(deliver(FrameType::BringupResult, {1, 2, 0}));
}

TEST_F(TrackerTest, ARequestWhileNotConnectedEndsAtOnceAndSendsNothing)
{
    connected = false;
    tracker.submit(Command::Arming, 1, record(), now);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].outcome, Outcome::NotSent);
    EXPECT_FALSE(tracker.pending(Command::Arming));
    EXPECT_TRUE(sent.empty());
}

TEST_F(TrackerTest, ASecondRequestOfTheSameKindWhilePendingIsBusyAndTheFirstStillAnswers)
{
    tracker.submit(Command::Arming, 1, record(), now);
    tracker.submit(Command::Arming, 0, record(), now);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].outcome, Outcome::Busy);
    EXPECT_EQ(sent.size(), 1U);
    EXPECT_TRUE(deliver_arming_reply(true));
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_TRUE(replies[1].accepted());
}

TEST_F(TrackerTest, TheLinkGoingDownEndsEveryPendingRequest)
{
    tracker.submit(Command::Mode, companion, record(), now);
    tracker.submit(Command::Arming, 1, record(), now);
    tracker.fail_all();
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_EQ(replies[0].outcome, Outcome::LinkLost);
    EXPECT_EQ(replies[1].outcome, Outcome::LinkLost);
    EXPECT_FALSE(tracker.pending(Command::Mode));
}

TEST_F(TrackerTest, ADoneCallbackMayRequestAgain)
{
    int answered = 0;
    tracker.submit(
        Command::Mode, test_mode,
        [&](const Reply&) {
            ++answered;
            tracker.submit(Command::Mode, companion, record(), now);
        },
        now);
    EXPECT_TRUE(deliver(FrameType::ModeReply, {test_mode}));
    EXPECT_EQ(answered, 1);
    EXPECT_TRUE(tracker.pending(Command::Mode));
    EXPECT_TRUE(deliver(FrameType::ModeReply, {companion}));
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_TRUE(replies[0].accepted());
}

TEST(ResolveMode, ByNameInAnyLetterCaseAndByValue)
{
    EXPECT_EQ(resolve_mode("COMPANION", -1).mode, companion);
    EXPECT_EQ(resolve_mode("companion", -1).mode, companion);
    EXPECT_EQ(resolve_mode("Test", -1).mode, test_mode);
    EXPECT_EQ(resolve_mode("", 3).mode, 3);
    EXPECT_EQ(resolve_mode("", 0).mode, 0);  // STANDBY by value
    EXPECT_EQ(resolve_mode("manual", 2).mode, 2);  // both, and they agree
}

TEST(ResolveMode, RefusesWhatNamesNoModeOrTwoModesOrNone)
{
    EXPECT_FALSE(resolve_mode("hover", -1).mode.has_value());
    EXPECT_NE(resolve_mode("hover", -1).error.find("hover"), std::string::npos);
    EXPECT_FALSE(resolve_mode("", 9).mode.has_value());
    EXPECT_FALSE(resolve_mode("", 256).mode.has_value());
    EXPECT_FALSE(resolve_mode("", -2).mode.has_value());
    EXPECT_FALSE(resolve_mode("test", 3).mode.has_value());
    const ModeChoice neither = resolve_mode("", -1);
    EXPECT_FALSE(neither.mode.has_value());
    EXPECT_FALSE(neither.error.empty());
}

// The names a request may use are the schema's own, so the table cannot drift: every
// value the generated enum has resolves by its name to itself, and back.
TEST(ResolveMode, TheNamesAreTheGeneratedEnumsNames)
{
    for (const auto& entry : vk::msg::EnumValues<vk::msg::Mode>::values) {
        EXPECT_EQ(resolve_mode(entry.name, -1).mode, static_cast<std::uint8_t>(entry.value)) << entry.name;
        EXPECT_EQ(mode_name(static_cast<std::uint8_t>(entry.value)), std::string(entry.name));
    }
    EXPECT_EQ(mode_name(200), "mode 200");
    static_assert(vk::msg::EnumValues<vk::msg::Mode>::values.size() == 4, "a new mode needs the services' docs and SetMode.srv looked at");
}

}  // namespace
}  // namespace vk::bridge
