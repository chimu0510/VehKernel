// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the commander's state machine (ADR-0005, spec 0006): modes,
// arming checks, actuator power sense, the emergency stop, the timers and
// the reset cause.
#include <vk/commander/commander.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using vk::commander::ActuatorPower;
using vk::commander::Commander;
using vk::commander::Inputs;
using vk::commander::Settings;
using vk::core::ArmingResult;
using vk::core::DisarmReason;
using vk::core::Mode;
using vk::core::ModeResult;

constexpr std::uint8_t link0 = 0;
constexpr std::uint8_t link1 = 1;
constexpr std::uint64_t cycle_us = 5'000;

// A commander driven cycle by cycle, with inputs where every check passes.
class CommanderTest : public ::testing::Test {
protected:
    void step(std::uint64_t duration_us = cycle_us)
    {
        const std::uint64_t end = now + duration_us;
        while (now < end) {
            now += cycle_us;
            commander.update(now, inputs, settings);
        }
    }

    // Powered long enough, in TEST with link0 in control.
    void ready()
    {
        step(Commander::power_settle_us + cycle_us);
        ASSERT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Ok);
    }

    void arm()
    {
        ready();
        ASSERT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
        ASSERT_TRUE(commander.state().armed);
    }

    Commander commander;
    Inputs inputs{.board_verified = true, .actuator_power = true, .actuators_started = true};
    Settings settings;
    std::uint64_t now = 0;
};

TEST_F(CommanderTest, BootsInStandbyWithNoSourceAndRefusesToArm)
{
    step(Commander::power_settle_us + cycle_us);
    EXPECT_EQ(commander.state().mode, Mode::Standby);
    EXPECT_EQ(commander.state().source, vk::core::no_source);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::WrongMode);
    EXPECT_FALSE(commander.state().armed);
}

TEST_F(CommanderTest, ArmsInTestFromTheSourceInControl)
{
    arm();
    EXPECT_EQ(commander.state().source, link0);
    EXPECT_EQ(commander.state().arming_refusal, ArmingResult::Ok);
}

TEST_F(CommanderTest, RefusesEachArmingCheckWithItsReasonInOrder)
{
    ready();
    inputs.board_verified = false;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::BoardNotVerified);
    EXPECT_EQ(commander.state().arming_refusal, ArmingResult::BoardNotVerified);
    inputs.board_verified = true;
    step();

    commander.request_emergency_stop(now, true);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::EmergencyStop);
    commander.request_emergency_stop(now, false);

    EXPECT_EQ(commander.request_arming(now, link1, true), ArmingResult::NotInControl);

    inputs.setpoints_idle = false;
    inputs.last_setpoint_us = now;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::SetpointsActive);
    // A setpoint older than the setpoint timeout no longer counts.
    step(static_cast<std::uint64_t>(settings.setpoint_timeout_s * 1e6F) + cycle_us);
    EXPECT_NE(commander.request_arming(now, link0, true), ArmingResult::SetpointsActive);
    commander.request_arming(now, link0, false);
    inputs.setpoints_idle = true;

    inputs.bringup_running = true;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::BringupRunning);
    inputs.bringup_running = false;

    inputs.actuator_power = false;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::ActuatorPowerOff);
    inputs.actuator_power = true;
    step(Commander::power_settle_us + cycle_us);

    inputs.actuators_started = false;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::ActuatorsNotStarted);
    inputs.actuators_started = true;

    inputs.local_menu_open = true;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::LocalMenuOpen);
    EXPECT_FALSE(commander.state().armed);
}

// ADR-0022: arming is refused while the local menu is open, a check
// COM_ARM_CHECK cannot skip, and accepted once it closes.
TEST_F(CommanderTest, TheLocalMenuOpenRefusesArmingEvenWithEveryCheckSkipped)
{
    settings.arm_check = 0;
    ready();
    inputs.local_menu_open = true;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::LocalMenuOpen);
    EXPECT_EQ(commander.state().arming_refusal, ArmingResult::LocalMenuOpen);
    inputs.local_menu_open = false;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
}

// ADR-0005: after a watchdog or fault reset arming is refused, a check
// COM_ARM_CHECK cannot skip; any other reset cause arms.
TEST_F(CommanderTest, AWatchdogOrFaultResetRefusesArmingEvenWithEveryCheckSkipped)
{
    using vk::core::ResetCause;
    settings.arm_check = 0;
    for (const ResetCause cause : {ResetCause::Watchdog, ResetCause::Fault}) {
        Commander fresh;
        commander = fresh;
        inputs.reset_report = {.cause = cause, .fault = vk::core::Fault::HardFault, .fault_address = 0x08012345};
        ready();
        EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::UnexpectedReset);
        EXPECT_FALSE(commander.state().armed);
        EXPECT_EQ(commander.state().reset_report, inputs.reset_report);
    }
    for (const ResetCause cause :
         {ResetCause::Unknown, ResetCause::PowerOn, ResetCause::Pin, ResetCause::Software, ResetCause::LowPower}) {
        Commander fresh;
        commander = fresh;
        inputs.reset_report = {.cause = cause};
        ready();
        EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok) << static_cast<int>(cause);
    }
}

TEST_F(CommanderTest, ModeChangesOnlyWhileDisarmed)
{
    arm();
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Standby), ModeResult::Armed);
    EXPECT_EQ(commander.request_mode(now, link1, Mode::Test), ModeResult::Armed);
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Ok);  // a repeat changes nothing
    EXPECT_EQ(commander.state().mode, Mode::Test);
    EXPECT_EQ(commander.state().source, link0);
}

TEST_F(CommanderTest, StandbyReleasesControl)
{
    ready();
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Standby), ModeResult::Ok);
    EXPECT_EQ(commander.state().source, vk::core::no_source);
}

TEST_F(CommanderTest, DisarmIsAcceptedFromAnySource)
{
    arm();
    EXPECT_EQ(commander.request_arming(now, link1, false), ArmingResult::Ok);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::Request);
}

TEST_F(CommanderTest, ActuatorPowerSettlesBeforeItCounts)
{
    inputs.actuator_power = false;
    step();
    EXPECT_EQ(commander.state().actuator_power, ActuatorPower::Unpowered);
    ASSERT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Ok);
    // Settling counts from the first powered sample, one cycle in.
    inputs.actuator_power = true;
    step(Commander::power_settle_us);
    EXPECT_EQ(commander.state().actuator_power, ActuatorPower::Settling);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::ActuatorPowerOff);
    step(cycle_us);
    EXPECT_EQ(commander.state().actuator_power, ActuatorPower::Powered);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
}

TEST_F(CommanderTest, AOneCycleGlitchRestartsTheSettleTime)
{
    inputs.actuator_power = false;
    step();
    inputs.actuator_power = true;
    step(Commander::power_settle_us - 2 * cycle_us);
    inputs.actuator_power = false;
    step();
    inputs.actuator_power = true;
    step(Commander::power_settle_us);
    EXPECT_EQ(commander.state().actuator_power, ActuatorPower::Settling);
    step(cycle_us);
    EXPECT_EQ(commander.state().actuator_power, ActuatorPower::Powered);
}

TEST_F(CommanderTest, LosingActuatorPowerDisarmsInTheCycleItIsSampledAndNeverReArms)
{
    arm();
    inputs.actuator_power = false;
    step();
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::ActuatorPowerLost);
    inputs.actuator_power = true;
    step(Commander::power_settle_us * 2);
    EXPECT_FALSE(commander.state().armed);
}

TEST_F(CommanderTest, ASkippedPowerCheckNeitherRefusesNorDisarmsButIsReported)
{
    settings.arm_check = 0;
    inputs.actuator_power = false;
    step();
    EXPECT_EQ(commander.state().skipped_checks, vk::core::arm_checks::actuator_power);
    EXPECT_EQ(commander.state().actuator_power, ActuatorPower::Unpowered);
    ASSERT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Ok);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
    step(cycle_us * 10);
    EXPECT_TRUE(commander.state().armed);
}

TEST_F(CommanderTest, ABoardWithoutPowerSensePassesAsNotAvailable)
{
    inputs.actuator_power.reset();
    step();
    EXPECT_EQ(commander.state().actuator_power, ActuatorPower::NotAvailable);
    ASSERT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Ok);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
}

TEST_F(CommanderTest, TheOtherChecksStayInForceWithEveryCheckSkipped)
{
    settings.arm_check = 0;
    inputs.board_verified = false;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::BoardNotVerified);
    inputs.board_verified = true;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::WrongMode);
}

TEST_F(CommanderTest, EmergencyStopKeepsTheVehicleArmedForFiveSecondsThenDisarms)
{
    arm();
    inputs.setpoints_idle = false;  // so the idle disarm stays out of the way
    commander.request_emergency_stop(now, true);
    step(Commander::emergency_stop_disarm_us - cycle_us);
    EXPECT_TRUE(commander.state().armed);
    EXPECT_TRUE(commander.state().emergency_stop);
    step(cycle_us);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::EmergencyStop);
    // It stays engaged across disarming until released.
    EXPECT_TRUE(commander.state().emergency_stop);
    inputs.setpoints_idle = true;
    step();
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::EmergencyStop);
    EXPECT_FALSE(commander.request_emergency_stop(now, false));
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
}

TEST_F(CommanderTest, EmergencyStopReleasedWithinFiveSecondsKeepsTheVehicleArmed)
{
    arm();
    inputs.setpoints_idle = false;
    commander.request_emergency_stop(now, true);
    step(Commander::emergency_stop_disarm_us / 2);
    commander.request_emergency_stop(now, false);
    step(Commander::emergency_stop_disarm_us);
    EXPECT_TRUE(commander.state().armed);
}

TEST_F(CommanderTest, IdleDisarmsAfterDisarmIdleWithNoSetpoint)
{
    arm();
    const auto idle_us = static_cast<std::uint64_t>(settings.disarm_idle_s * 1e6F);
    step(idle_us - cycle_us);
    EXPECT_TRUE(commander.state().armed);
    step(cycle_us);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::Idle);
}

TEST_F(CommanderTest, SetpointTimeoutCountsFromTheLastSetpoint)
{
    arm();
    inputs.setpoints_idle = false;
    inputs.last_setpoint_us = now;
    const auto timeout_us = static_cast<std::uint64_t>(settings.setpoint_timeout_s * 1e6F);
    step(timeout_us);
    EXPECT_TRUE(commander.state().armed);
    step(cycle_us);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::SetpointTimeout);
}

TEST_F(CommanderTest, TheEmergencyStopHoldsTheSetpointTimeout)
{
    arm();
    inputs.setpoints_idle = false;
    inputs.last_setpoint_us = now;
    commander.request_emergency_stop(now, true);
    step(Commander::emergency_stop_disarm_us / 2);
    commander.request_emergency_stop(now, false);
    step(cycle_us);
    EXPECT_TRUE(commander.state().armed);
}

using vk::core::BringupStart;

constexpr std::uint8_t motors_case = 1;

TEST_F(CommanderTest, ABringupCaseStartsOnlyFromTheArmedSourceInControl)
{
    inputs.bringup_available = true;
    step();
    EXPECT_EQ(commander.request_bringup(link0, motors_case), BringupStart::WrongMode);
    arm();
    EXPECT_EQ(commander.request_bringup(link0, 9), BringupStart::UnknownCase);
    EXPECT_EQ(commander.request_bringup(link1, motors_case), BringupStart::NotInControl);
    EXPECT_EQ(commander.request_bringup(link0, motors_case), BringupStart::Ok);
    commander.request_arming(now, link0, false);
    EXPECT_EQ(commander.request_bringup(link0, motors_case), BringupStart::NotArmed);
}

TEST_F(CommanderTest, ABringupCaseNeedsToBeBuiltInAndNotRunning)
{
    arm();
    EXPECT_EQ(commander.request_bringup(link0, motors_case), BringupStart::NotAvailable);
    inputs.bringup_available = true;
    inputs.bringup_running = true;
    step();
    EXPECT_EQ(commander.request_bringup(link0, motors_case), BringupStart::AlreadyRunning);
    // While it runs: no setpoints, no idle or setpoint timeout, no arming.
    EXPECT_FALSE(commander.accepts_setpoint(link0, vk::core::SetpointLevel::DirectActuator));
    step(static_cast<std::uint64_t>(settings.disarm_idle_s * 1e6F) + cycle_us);
    EXPECT_TRUE(commander.state().armed);
}

TEST_F(CommanderTest, TheEmergencyStopDuringABringupCaseDisarmsAtOnce)
{
    arm();
    inputs.bringup_running = true;
    step();
    commander.request_emergency_stop(now, true);
    step();
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::BringupAbort);
}

TEST_F(CommanderTest, ItsSourceFallingSilentAbortsABringupCase)
{
    arm();
    commander.source_lost(link0);
    EXPECT_TRUE(commander.state().armed);  // no case running: nothing to abort
    inputs.bringup_running = true;
    step();
    commander.source_lost(link1);
    EXPECT_TRUE(commander.state().armed);
    commander.source_lost(link0);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::BringupAbort);
}

TEST_F(CommanderTest, ReportsTheOutputWatchdogTrips)
{
    inputs.output_watchdog_trips = 2;
    step();
    EXPECT_EQ(commander.state().output_watchdog_trips, 2U);
}

}  // namespace

// --- Per-mode rules and MANUAL (spec 0010, #82) ---

namespace {

using vk::core::remote_control;

TEST_F(CommanderTest, EachModeTakesRequestsOnlyFromItsSources)
{
    step(Commander::power_settle_us + cycle_us);
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Manual), ModeResult::WrongSource);
    EXPECT_EQ(commander.request_mode(now, remote_control, Mode::Test), ModeResult::WrongSource);
    EXPECT_EQ(commander.state().mode, Mode::Standby);
    EXPECT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    EXPECT_EQ(commander.state().source, remote_control);
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Standby), ModeResult::Ok);
    EXPECT_EQ(commander.state().source, vk::core::no_source);
}

TEST_F(CommanderTest, TestIsEnteredOnlyWhileDisarmed)
{
    arm();
    EXPECT_EQ(commander.request_mode(now, link1, Mode::Test), ModeResult::Armed);
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Standby), ModeResult::Armed);
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Ok) << "unchanged: not refused";
    EXPECT_EQ(commander.state().source, link0);
}

TEST_F(CommanderTest, TheRemoteControlTakesOverWhileArmed)
{
    arm();
    EXPECT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    EXPECT_TRUE(commander.state().armed) << "taking over does not disarm";
    EXPECT_EQ(commander.state().mode, Mode::Manual);
    EXPECT_EQ(commander.state().source, remote_control);
    // The Link's setpoints no longer count; the remote control's body velocity does.
    EXPECT_FALSE(commander.accepts_setpoint(link0, vk::core::SetpointLevel::DirectActuator));
    EXPECT_FALSE(commander.accepts_setpoint(link0, vk::core::SetpointLevel::BodyVelocity));
    EXPECT_FALSE(commander.accepts_setpoint(remote_control, vk::core::SetpointLevel::DirectActuator));
    EXPECT_TRUE(commander.accepts_setpoint(remote_control, vk::core::SetpointLevel::BodyVelocity));
    // Back to TEST only once disarmed.
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Armed);
}

TEST_F(CommanderTest, TakingOverRestartsTheSetpointTimeoutForTheNewSource)
{
    arm();
    inputs.remote_control_connected = true;
    inputs.last_setpoint_us = now;
    inputs.setpoints_idle = false;
    step(400'000);
    ASSERT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    // 0.4 s of the old source's silence do not count against the new one.
    step(300'000);
    EXPECT_TRUE(commander.state().armed);
    step(300'000);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::SetpointTimeout);
}

TEST_F(CommanderTest, TakingOverEndsABringupCaseWithADisarm)
{
    arm();
    inputs.bringup_running = true;
    step();
    EXPECT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::BringupAbort);
    EXPECT_EQ(commander.state().mode, Mode::Manual);
}

TEST_F(CommanderTest, ArmingInManualNeedsTheRemoteControlReadyAndInControl)
{
    step(Commander::power_settle_us + cycle_us);
    ASSERT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::NotInControl);
    EXPECT_EQ(commander.request_arming(now, remote_control, true), ArmingResult::RcNotConnected);
    inputs.remote_control_connected = true;
    step();
    EXPECT_EQ(commander.request_arming(now, remote_control, true), ArmingResult::SticksNotCentred);
    inputs.sticks_centred = true;
    step();
    EXPECT_EQ(commander.request_arming(now, remote_control, true), ArmingResult::Ok);
    // Disarming is accepted from any source.
    EXPECT_EQ(commander.request_arming(now, link0, false), ArmingResult::Ok);
    EXPECT_FALSE(commander.state().armed);
}

TEST_F(CommanderTest, TestNeedsNoRemoteControl)
{
    arm();  // inputs.remote_control_connected is false
    EXPECT_TRUE(commander.state().armed);
}

}  // namespace

// --- COMPANION (spec 0015, ADR-0005) ---

namespace {

class CompanionCommanderTest : public CommanderTest {
protected:
    void ready_companion()
    {
        step(Commander::power_settle_us + cycle_us);
        ASSERT_EQ(commander.request_mode(now, link0, Mode::Companion), ModeResult::Ok);
    }

    void arm_companion()
    {
        ready_companion();
        ASSERT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
        ASSERT_TRUE(commander.state().armed);
    }
};

TEST(ModeRulesTest, CompanionIsArmableFromALinkWithBodyVelocityAndZeroSetpointsNotIdle)
{
    using vk::core::mode_rules;
    using vk::core::SetpointLevel;
    const vk::core::ModeRules rules = mode_rules(Mode::Companion);
    EXPECT_TRUE(rules.armable);
    EXPECT_FALSE(rules.enter_while_armed);
    EXPECT_TRUE(rules.from_link);
    EXPECT_FALSE(rules.from_remote_control);
    EXPECT_FALSE(rules.direct_actuator);
    EXPECT_TRUE(rules.body_velocity);
    EXPECT_FALSE(rules.needs_remote_control);
    EXPECT_FALSE(rules.zero_setpoints_idle);
    EXPECT_TRUE(vk::core::known_mode(static_cast<std::uint8_t>(Mode::Companion)));
    EXPECT_TRUE(vk::core::may_request(Mode::Companion, link1));
    EXPECT_FALSE(vk::core::may_request(Mode::Companion, vk::core::remote_control));
    EXPECT_TRUE(vk::core::mode_accepts(Mode::Companion, SetpointLevel::BodyVelocity));
    EXPECT_FALSE(vk::core::mode_accepts(Mode::Companion, SetpointLevel::DirectActuator));
    // Zero setpoints count as idle in every other mode.
    for (const Mode mode : {Mode::Standby, Mode::Test, Mode::Manual}) {
        EXPECT_TRUE(mode_rules(mode).zero_setpoints_idle);
    }
}

TEST_F(CompanionCommanderTest, ArmsWhenARequestingLinkIsInControl)
{
    ready_companion();
    EXPECT_EQ(commander.state().source, link0);
    EXPECT_EQ(commander.request_arming(now, link1, true), ArmingResult::NotInControl);
    EXPECT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
    EXPECT_TRUE(commander.state().armed);
    EXPECT_EQ(commander.state().mode, Mode::Companion);
}

TEST_F(CompanionCommanderTest, IsEnteredOnlyWhileDisarmed)
{
    arm();  // in TEST
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Companion), ModeResult::Armed);
    EXPECT_EQ(commander.state().mode, Mode::Test);
    // And once armed in COMPANION, it is not left for TEST or STANDBY either.
    EXPECT_EQ(commander.request_arming(now, link0, false), ArmingResult::Ok);
    ASSERT_EQ(commander.request_mode(now, link0, Mode::Companion), ModeResult::Ok);
    ASSERT_EQ(commander.request_arming(now, link0, true), ArmingResult::Ok);
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Test), ModeResult::Armed);
    EXPECT_EQ(commander.request_mode(now, link0, Mode::Companion), ModeResult::Ok) << "unchanged: not refused";
}

TEST_F(CompanionCommanderTest, TheRemoteControlCannotRequestIt)
{
    step(Commander::power_settle_us + cycle_us);
    EXPECT_EQ(commander.request_mode(now, vk::core::remote_control, Mode::Companion), ModeResult::WrongSource);
    EXPECT_EQ(commander.state().mode, Mode::Standby);
}

TEST_F(CompanionCommanderTest, AcceptsBodyVelocityFromItsLinkAndDropsTheRest)
{
    using vk::core::SetpointLevel;
    arm_companion();
    EXPECT_TRUE(commander.accepts_setpoint(link0, SetpointLevel::BodyVelocity));
    EXPECT_FALSE(commander.accepts_setpoint(link0, SetpointLevel::DirectActuator));
    EXPECT_FALSE(commander.accepts_setpoint(link1, SetpointLevel::BodyVelocity));
    EXPECT_FALSE(commander.accepts_setpoint(vk::core::remote_control, SetpointLevel::BodyVelocity));
}

TEST_F(CompanionCommanderTest, ZeroSetpointsAreNotIdle)
{
    arm_companion();
    inputs.setpoints_idle = true;
    const auto idle_us = static_cast<std::uint64_t>(settings.disarm_idle_s * 1e6F);
    // Zero setpoints, kept current, for longer than the idle time.
    const std::uint64_t end = now + idle_us + 1'000'000;
    while (now < end) {
        inputs.last_setpoint_us = now;
        step(100'000);
    }
    EXPECT_TRUE(commander.state().armed);
}

TEST_F(CompanionCommanderTest, NoSetpointSinceArmingStillDisarmsAfterDisarmIdle)
{
    arm_companion();
    const auto idle_us = static_cast<std::uint64_t>(settings.disarm_idle_s * 1e6F);
    step(idle_us - cycle_us);
    EXPECT_TRUE(commander.state().armed);
    step(cycle_us);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::Idle);
}

TEST_F(CompanionCommanderTest, StillStopsAndDisarmsOnTheSetpointTimeout)
{
    arm_companion();
    inputs.setpoints_idle = true;
    inputs.last_setpoint_us = now;
    const auto timeout_us = static_cast<std::uint64_t>(settings.setpoint_timeout_s * 1e6F);
    step(timeout_us);
    EXPECT_TRUE(commander.state().armed);
    step(cycle_us);
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::SetpointTimeout);
}

TEST_F(CompanionCommanderTest, YieldsToTheRemoteControlsManualTakeover)
{
    using vk::core::SetpointLevel;
    arm_companion();
    ASSERT_EQ(commander.request_mode(now, vk::core::remote_control, Mode::Manual), ModeResult::Ok);
    EXPECT_TRUE(commander.state().armed) << "taking over does not disarm";
    EXPECT_EQ(commander.state().mode, Mode::Manual);
    EXPECT_EQ(commander.state().source, vk::core::remote_control);
    EXPECT_FALSE(commander.accepts_setpoint(link0, SetpointLevel::BodyVelocity));
    EXPECT_TRUE(commander.accepts_setpoint(vk::core::remote_control, SetpointLevel::BodyVelocity));
}

}  // namespace

// --- The RC failsafe (spec 0010) ---

namespace {

class ManualCommanderTest : public CommanderTest {
protected:
    void arm_manual()
    {
        inputs.remote_control_connected = true;
        inputs.sticks_centred = true;
        step(Commander::power_settle_us + cycle_us);
        ASSERT_EQ(commander.request_mode(now, vk::core::remote_control, Mode::Manual), ModeResult::Ok);
        ASSERT_EQ(commander.request_arming(now, vk::core::remote_control, true), ArmingResult::Ok);
        inputs.last_setpoint_us = now;
        inputs.setpoints_idle = false;
    }
};

TEST_F(ManualCommanderTest, LosingTheRemoteControlStopsAndDisarms)
{
    arm_manual();
    step(100'000);
    EXPECT_TRUE(commander.state().armed);
    inputs.remote_control_connected = false;
    step();
    EXPECT_FALSE(commander.state().armed);
    EXPECT_EQ(commander.state().disarm_reason, DisarmReason::RcLost);
    // It does not re-arm when the remote control comes back.
    inputs.remote_control_connected = true;
    step(100'000);
    EXPECT_FALSE(commander.state().armed);
}

TEST_F(ManualCommanderTest, SticksOffCentreWhileArmedDoNotDisarm)
{
    arm_manual();
    inputs.sticks_centred = false;
    step(100'000);
    EXPECT_TRUE(commander.state().armed) << "the sticks matter only for arming";
}

TEST_F(CommanderTest, TheRemoteControlDoesNotMatterInTest)
{
    arm();
    inputs.last_setpoint_us = now;
    inputs.setpoints_idle = false;
    step(100'000);
    EXPECT_TRUE(commander.state().armed);
}

}  // namespace

// --- The arming_checks bitmasks (spec 0016, #206) ---

namespace {

using vk::commander::ArmingChecks;
using vk::commander::bit;

class ArmingChecksTest : public CommanderTest {
protected:
    // The bits must say what an arm request returns in the same state.
    void expect_matches_request(ArmingResult expected_first)
    {
        const ArmingChecks checks = commander.arming_checks(now);
        EXPECT_EQ(checks.first_failing(), expected_first);
        EXPECT_EQ(commander.request_arming(now, link0, true), expected_first);
        commander.request_arming(now, link0, false);
    }
};

TEST_F(ArmingChecksTest, NothingFailsWhenArmingWouldSucceed)
{
    ready();
    const ArmingChecks checks = commander.arming_checks(now);
    EXPECT_EQ(checks.failing, 0U);
    // TEST needs no remote control; the power check runs.
    EXPECT_EQ(checks.skipped, bit(ArmingResult::RcNotConnected) | bit(ArmingResult::SticksNotCentred));
    expect_matches_request(ArmingResult::Ok);
}

TEST_F(ArmingChecksTest, EachCheckSetsItsOwnBit)
{
    ready();
    inputs.board_verified = false;
    step();
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::BoardNotVerified));
    expect_matches_request(ArmingResult::BoardNotVerified);
    inputs.board_verified = true;
    step();

    commander.request_emergency_stop(now, true);
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::EmergencyStop));
    expect_matches_request(ArmingResult::EmergencyStop);
    commander.request_emergency_stop(now, false);

    inputs.setpoints_idle = false;
    inputs.last_setpoint_us = now;
    step();
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::SetpointsActive));
    expect_matches_request(ArmingResult::SetpointsActive);
    inputs.setpoints_idle = true;

    inputs.bringup_running = true;
    step();
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::BringupRunning));
    expect_matches_request(ArmingResult::BringupRunning);
    inputs.bringup_running = false;

    inputs.actuator_power = false;
    step();
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::ActuatorPowerOff));
    expect_matches_request(ArmingResult::ActuatorPowerOff);
    inputs.actuator_power = true;
    step(Commander::power_settle_us + cycle_us);

    inputs.actuators_started = false;
    step();
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::ActuatorsNotStarted));
    expect_matches_request(ArmingResult::ActuatorsNotStarted);
    inputs.actuators_started = true;

    inputs.local_menu_open = true;
    step();
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::LocalMenuOpen));
    expect_matches_request(ArmingResult::LocalMenuOpen);
    inputs.local_menu_open = false;

    inputs.reset_report = {.cause = vk::core::ResetCause::Watchdog};
    step();
    EXPECT_EQ(commander.arming_checks(now).failing, bit(ArmingResult::UnexpectedReset));
    expect_matches_request(ArmingResult::UnexpectedReset);
}

TEST_F(ArmingChecksTest, ASourceNotInControlFailsNotInControl)
{
    ready();
    EXPECT_EQ(commander.evaluate_arming(now, link1).failing, bit(ArmingResult::NotInControl));
    EXPECT_EQ(commander.evaluate_arming(now, link1).first_failing(), commander.request_arming(now, link1, true));
}

TEST_F(ArmingChecksTest, EveryFailingCheckIsReportedAndTheFirstIsTheRequestsResult)
{
    step(Commander::power_settle_us + cycle_us);  // STANDBY, in no one's control
    inputs.board_verified = false;
    inputs.actuators_started = false;
    inputs.local_menu_open = true;
    step();
    const ArmingChecks checks = commander.arming_checks(now);
    EXPECT_EQ(checks.failing, bit(ArmingResult::BoardNotVerified) | bit(ArmingResult::WrongMode) |
                                  bit(ArmingResult::ActuatorsNotStarted) | bit(ArmingResult::LocalMenuOpen));
    EXPECT_EQ(checks.failing & bit(ArmingResult::Ok), 0U);
    EXPECT_EQ(commander.request_arming(now, vk::core::no_source, true), checks.first_failing());
    EXPECT_EQ(checks.first_failing(), ArmingResult::BoardNotVerified);
}

TEST_F(ArmingChecksTest, ThePowerCheckIsSkippedWhenComArmCheckClearsIt)
{
    settings.arm_check = 0;
    ready();
    inputs.actuator_power = false;
    step();
    const ArmingChecks checks = commander.arming_checks(now);
    EXPECT_EQ(checks.failing, 0U);
    EXPECT_NE(checks.skipped & bit(ArmingResult::ActuatorPowerOff), 0U);
    expect_matches_request(ArmingResult::Ok);
}

TEST_F(ArmingChecksTest, TheRemoteControlChecksApplyInManualOnly)
{
    step(Commander::power_settle_us + cycle_us);
    ASSERT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    ArmingChecks checks = commander.arming_checks(now);
    EXPECT_EQ(checks.failing, bit(ArmingResult::RcNotConnected) | bit(ArmingResult::SticksNotCentred));
    EXPECT_EQ(checks.skipped, 0U);
    EXPECT_EQ(commander.request_arming(now, remote_control, true), checks.first_failing());
    inputs.remote_control_connected = true;
    step();
    checks = commander.arming_checks(now);
    EXPECT_EQ(checks.failing, bit(ArmingResult::SticksNotCentred));
    EXPECT_EQ(commander.request_arming(now, remote_control, true), checks.first_failing());
}

// The checks run in a fixed order that is not the order of the values.
TEST_F(ArmingChecksTest, TheRemoteControlChecksComeBeforeTheSetpointCheck)
{
    step(Commander::power_settle_us + cycle_us);
    ASSERT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    inputs.setpoints_idle = false;
    inputs.last_setpoint_us = now;
    step();
    const ArmingChecks checks = commander.arming_checks(now);
    EXPECT_NE(checks.failing & bit(ArmingResult::SetpointsActive), 0U);
    EXPECT_EQ(checks.first_failing(), ArmingResult::RcNotConnected);
    EXPECT_EQ(commander.request_arming(now, remote_control, true), ArmingResult::RcNotConnected);
}

TEST_F(ArmingChecksTest, SticksOffCentreComeBeforeTheLocalMenu)
{
    step(Commander::power_settle_us + cycle_us);
    ASSERT_EQ(commander.request_mode(now, remote_control, Mode::Manual), ModeResult::Ok);
    inputs.remote_control_connected = true;
    inputs.local_menu_open = true;
    step();
    const ArmingChecks checks = commander.arming_checks(now);
    EXPECT_NE(checks.failing & bit(ArmingResult::LocalMenuOpen), 0U);
    EXPECT_EQ(checks.first_failing(), ArmingResult::SticksNotCentred);
    EXPECT_EQ(commander.request_arming(now, remote_control, true), ArmingResult::SticksNotCentred);
}

}  // namespace
