// SPDX-License-Identifier: GPL-3.0-or-later
// L1 tests of the zero setpoint scheduler (ADR-0005, ADR-0018, spec 0015 seam D):
// with a fake clock, which is the time the tests pass in.
#include <vehkernel_bridge/setpoint_scheduler.hpp>

#include <vk/msg/enums.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace vk::bridge {
namespace {

constexpr std::uint64_t ms = 1000;
constexpr auto companion = static_cast<std::uint8_t>(vk::msg::Mode::Companion);
constexpr auto test_mode = static_cast<std::uint8_t>(vk::msg::Mode::Test);
constexpr std::uint8_t none = 255;

class SchedulerTest : public ::testing::Test {
protected:
    explicit SchedulerTest(std::optional<std::uint8_t> own = std::uint8_t{0}) : scheduler(SchedulerConfig{500 * ms, 100 * ms}, own) {}

    // Polls every 2 ms until `until_us` and returns the times a zero setpoint was due.
    std::vector<std::uint64_t> run_until(std::uint64_t until_us)
    {
        std::vector<std::uint64_t> due;
        for (; now < until_us; now += 2 * ms) {
            if (scheduler.zero_due(now)) {
                due.push_back(now);
            }
        }
        return due;
    }

    std::uint64_t now = 1'000'000;
    SetpointScheduler scheduler;
};

TEST_F(SchedulerTest, SendsNothingBeforeAnyStatus)
{
    EXPECT_TRUE(run_until(now + 3'000 * ms).empty());
}

TEST_F(SchedulerTest, StartsOneTimeoutAfterTheLastCmdVelThenSendsAtTenHertz)
{
    scheduler.on_status(companion, true, 0, now);
    scheduler.on_cmd_vel(now);
    const std::uint64_t last_cmd_vel = now;
    EXPECT_TRUE(run_until(last_cmd_vel + 500 * ms).empty());

    const auto due = run_until(last_cmd_vel + 1'500 * ms);
    ASSERT_GE(due.size(), 9U);
    EXPECT_EQ(due.front(), last_cmd_vel + 500 * ms);
    for (std::size_t i = 1; i < due.size(); ++i) {
        EXPECT_EQ(due[i] - due[i - 1], 100 * ms);
    }
    EXPECT_EQ(due.size(), 10U);
}

TEST_F(SchedulerTest, StopsWhenCmdVelReturnsAndStartsAgainAfterTheNextSilence)
{
    scheduler.on_status(companion, true, 0, now);
    ASSERT_FALSE(run_until(now + 600 * ms).empty());

    scheduler.on_cmd_vel(now);
    const std::uint64_t returned = now;
    EXPECT_TRUE(run_until(returned + 499 * ms).empty());
    scheduler.on_cmd_vel(now);
    const std::uint64_t again = now;
    EXPECT_TRUE(run_until(again + 400 * ms).empty());
    const auto due = run_until(again + 700 * ms);
    ASSERT_FALSE(due.empty());
    EXPECT_GE(due.front(), again + 500 * ms);
}

TEST_F(SchedulerTest, ArmedWithNoCmdVelAtAllStartsOneTimeoutAfterTheVehicleIsDriving)
{
    scheduler.on_status(companion, false, none, now);
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
    const std::uint64_t armed_at = now;
    scheduler.on_status(companion, true, 0, now);
    EXPECT_TRUE(run_until(armed_at + 500 * ms).empty());
    const auto due = run_until(armed_at + 700 * ms);
    ASSERT_FALSE(due.empty());
    EXPECT_EQ(due.front(), armed_at + 500 * ms);
}

TEST_F(SchedulerTest, SendsNothingDisarmedOrInAnotherMode)
{
    scheduler.on_status(companion, false, 0, now);
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
    scheduler.on_status(test_mode, true, 0, now);
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
    scheduler.on_status(static_cast<std::uint8_t>(vk::msg::Mode::Manual), true, 0x10, now);
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
    EXPECT_FALSE(scheduler.driving());
}

TEST_F(SchedulerTest, StopsAtOnceWhenTheStatusSaysDisarmedAndStartsAfterArmingAgain)
{
    scheduler.on_status(companion, true, 0, now);
    ASSERT_FALSE(run_until(now + 700 * ms).empty());
    scheduler.on_status(companion, false, 0, now);
    EXPECT_FALSE(scheduler.zero_due(now));
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
    scheduler.on_status(companion, true, 0, now);
    EXPECT_TRUE(scheduler.driving());
    EXPECT_FALSE(scheduler.zero_due(now));
}

TEST_F(SchedulerTest, ResetForgetsTheStatus)
{
    scheduler.on_status(companion, true, 0, now);
    ASSERT_FALSE(run_until(now + 700 * ms).empty());
    scheduler.reset();
    EXPECT_FALSE(scheduler.driving());
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
}

class OwnSourceTest : public SchedulerTest {
protected:
    OwnSourceTest() : SchedulerTest(std::uint8_t{1}) {}
};

TEST_F(OwnSourceTest, SendsOnlyWhileThisLinkIsInControl)
{
    scheduler.on_status(companion, true, 0, now);  // another Link
    EXPECT_FALSE(scheduler.driving());
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
    scheduler.on_status(companion, true, 1, now);  // this Link
    EXPECT_TRUE(scheduler.driving());
    EXPECT_FALSE(run_until(now + 700 * ms).empty());
    scheduler.on_status(companion, true, none, now);  // nobody
    EXPECT_FALSE(scheduler.driving());
}

// Without a configured instance the scheduler learns its own.
class LearnTest : public SchedulerTest {
protected:
    LearnTest() : SchedulerTest(std::nullopt) {}
};

TEST_F(LearnTest, SendsNoZeroSetpointUntilItsInstanceIsKnown)
{
    scheduler.on_status(companion, true, 0, now);  // armed by someone else, perhaps another host
    EXPECT_FALSE(scheduler.own_source().has_value());
    EXPECT_FALSE(scheduler.driving());
    EXPECT_TRUE(run_until(now + 3'000 * ms).empty());
}

TEST_F(LearnTest, LearnsItFromTheFirstCompanionStatusAfterTheAcceptedRequest)
{
    scheduler.on_status(companion, false, 3, now);  // before the acknowledgement: teaches nothing
    scheduler.on_companion_accepted();
    scheduler.on_status(test_mode, false, 7, now);  // a stale status that still says another mode
    EXPECT_FALSE(scheduler.own_source().has_value());
    scheduler.on_status(companion, false, none, now);  // no source in control: not an instance
    EXPECT_FALSE(scheduler.own_source().has_value());
    scheduler.on_status(companion, false, 1, now);
    EXPECT_EQ(scheduler.own_source(), 1);
    // Later statuses teach nothing more: the instance stays.
    scheduler.on_status(companion, true, 2, now);
    EXPECT_EQ(scheduler.own_source(), 1);
    EXPECT_FALSE(scheduler.driving());
    scheduler.on_status(companion, true, 1, now);
    EXPECT_TRUE(scheduler.driving());
    EXPECT_FALSE(run_until(now + 700 * ms).empty());
}

TEST_F(LearnTest, ARefusedRequestTeachesNothing)
{
    // The node calls on_companion_accepted() only for an accepted request.
    scheduler.on_status(companion, true, 1, now);
    EXPECT_FALSE(scheduler.own_source().has_value());
    EXPECT_TRUE(run_until(now + 2'000 * ms).empty());
}

TEST_F(LearnTest, TheInstanceIsForgottenOnANewConnection)
{
    scheduler.on_companion_accepted();
    scheduler.on_status(companion, true, 1, now);
    ASSERT_EQ(scheduler.own_source(), 1);
    scheduler.reset();
    EXPECT_FALSE(scheduler.own_source().has_value());
    scheduler.on_status(companion, true, 1, now);
    EXPECT_FALSE(scheduler.driving());
    // An acknowledgement awaited when the connection dropped is not carried over either.
    scheduler.on_companion_accepted();
    scheduler.reset();
    scheduler.on_status(companion, true, 2, now);
    EXPECT_FALSE(scheduler.own_source().has_value());
}

TEST(SchedulerSourceTest, TheConfiguredInstanceOverridesWhatItLearns)
{
    SetpointScheduler scheduler(SchedulerConfig{}, std::uint8_t{1});
    EXPECT_EQ(scheduler.own_source(), 1);
    scheduler.on_companion_accepted();
    scheduler.on_status(companion, true, 0, 0);
    EXPECT_EQ(scheduler.own_source(), 1);
    EXPECT_FALSE(scheduler.driving());
    scheduler.on_status(companion, true, 1, 0);
    EXPECT_TRUE(scheduler.driving());
}

TEST(TimeoutCheck, WarnsUnlessCmdVelTimeoutIsAMarginBelowComSpTimeout)
{
    EXPECT_FALSE(cmd_vel_timeout_is_safe(500 * ms, 0.5F));   // the race seen in SITL
    EXPECT_FALSE(cmd_vel_timeout_is_safe(400 * ms, 0.5F));   // 0.4 + 0.1 is not below 0.5
    EXPECT_TRUE(cmd_vel_timeout_is_safe(250 * ms, 0.5F));    // the default
    EXPECT_FALSE(cmd_vel_timeout_is_safe(250 * ms, 0.3F));
    EXPECT_TRUE(cmd_vel_timeout_is_safe(250 * ms, 1.0F));
    EXPECT_EQ(SchedulerConfig{}.cmd_vel_timeout_us, 250 * ms);
}

TEST(SchedulerConfigTest, HonoursItsTimeoutAndPeriod)
{
    SetpointScheduler scheduler(SchedulerConfig{200 * ms, 50 * ms}, std::uint8_t{0});
    std::uint64_t now = 0;
    scheduler.on_status(companion, true, 0, now);
    std::vector<std::uint64_t> due;
    for (; now < 400 * ms; now += ms) {
        if (scheduler.zero_due(now)) {
            due.push_back(now);
        }
    }
    ASSERT_EQ(due.size(), 4U);
    EXPECT_EQ(due[0], 200 * ms);
    EXPECT_EQ(due[1], 250 * ms);
}

}  // namespace
}  // namespace vk::bridge
