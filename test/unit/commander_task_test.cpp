// SPDX-License-Identifier: GPL-3.0-or-later
// The commander work item on a fake clock: requests on vehicle_command
// answered on vehicle_command_ack (ADR-0005), and the reboot (ADR-0023): a
// reboot is refused while armed, happens reboot_delay_us after it is
// accepted so the reply leaves first, and arming is refused meanwhile. And
// the reset cause it publishes on vehicle_status (ADR-0005). And both
// setpoint levels, direct-actuator and body velocity, keeping the setpoint
// timeout and the idle disarm (ADR-0005).
#include <vk/commander/commander_task.hpp>
#include <vk/core/bus.hpp>
#include <vk/core/fault.hpp>
#include <vk/core/vehicle_control.hpp>
#include <vk/core/work_queue.hpp>
#include <vk/hal/reset.hpp>
#include <vk/msg/actuator_setpoint.hpp>
#include <vk/msg/body_velocity_setpoint.hpp>
#include <vk/msg/estimator_attitude.hpp>
#include <vk/msg/estimator_restart.hpp>
#include <vk/msg/imu_sample.hpp>
#include <vk/msg/manual_control_input.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/vehicle_command.hpp>
#include <vk/msg/vehicle_command_ack.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using vk::commander::CommanderTask;
using vk::core::ArmingResult;
using vk::core::RebootAction;
using vk::core::RebootResult;
using vk::core::RestartEstimatorsResult;
using vk::hal::BootTarget;

constexpr std::uint32_t interval_us = 5'000;
constexpr vk::core::ControlSource link0 = 0;

std::uint64_t fake_now = 0;

std::uint64_t fake_clock() noexcept
{
    return fake_now;
}

std::vector<BootTarget> resets;

// Sends one request as `source` and runs the queue one control cycle: the
// commander's acknowledgement of it.
vk::msg::VehicleCommandAck request(vk::core::WorkQueue& queue, vk::core::Command command, std::uint8_t argument,
                                   vk::core::ControlSource source = link0)
{
    static std::uint16_t sequence = 0;
    static vk::core::Publication<vk::msg::VehicleCommand> commands;
    vk::core::Subscription<vk::msg::VehicleCommandAck> acks{0};
    vk::msg::VehicleCommandAck ack{};
    while (acks.copy(ack)) {
    }
    ++sequence;
    commands.publish({.timestamp = fake_now,
                      .source = source,
                      .sequence = sequence,
                      .command = static_cast<std::uint8_t>(command),
                      .argument = argument});
    fake_now += interval_us;
    queue.process();
    vk::msg::VehicleCommandAck answer{};
    while (acks.copy(ack)) {
        if (ack.sequence == sequence && ack.source == source) {
            answer = ack;
        }
    }
    EXPECT_EQ(answer.sequence, sequence) << "no acknowledgement";
    return answer;
}

std::uint8_t to_u8(auto value)
{
    return static_cast<std::uint8_t>(value);
}

vk::msg::VehicleCommandAck reboot(vk::core::WorkQueue& queue, RebootAction action)
{
    return request(queue, vk::core::Command::Reboot, to_u8(action));
}

void fake_reset(BootTarget target)
{
    resets.push_back(target);  // A real reset does not return.
}

class CommanderTaskTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fake_now = 1'000'000;
        resets.clear();
        task.start();
        // The bus outlives each test: replace what earlier tests left on the setpoints.
        body.publish({.timestamp = 0, .source = vk::core::no_source, .vx = NAN, .vy = NAN, .yaw_rate = NAN});
        direct.publish({.timestamp = 0, .source = vk::core::no_source, .wheel_speed = {NAN, NAN, NAN, NAN}});
        imu.publish(vk::msg::ImuSample{.timestamp = fake_now});  // the IMU answered: the board verifies
        step(interval_us);
    }

    // Runs the queue every control cycle for `duration_us`.
    void step(std::uint64_t duration_us)
    {
        for (const std::uint64_t end = fake_now + duration_us; fake_now < end;) {
            fake_now += interval_us;
            queue.process();
        }
    }

    void arm()
    {
        ASSERT_EQ(request(queue, vk::core::Command::Mode, to_u8(vk::core::Mode::Test)).result,
                  to_u8(vk::core::ModeResult::Ok));
        const auto armed = request(queue, vk::core::Command::Arm, 1);
        ASSERT_EQ(armed.result, to_u8(ArmingResult::Ok));
        ASSERT_TRUE(armed.armed);
    }

    vk::core::WorkQueue queue{"wq:test", 1, &fake_clock};
    // Sends a body velocity setpoint every control cycle for `duration_us`, as
    // setpoint arbitration publishes them, stamped when accepted.
    void send_body(float vx, float yaw_rate, std::uint64_t duration_us)
    {
        for (const std::uint64_t end = fake_now + duration_us; fake_now < end;) {
            body.publish({.timestamp = fake_now, .source = link0, .vx = vx, .vy = 0.0F, .yaw_rate = yaw_rate});
            step(interval_us);
        }
    }

    vk::core::Publication<vk::msg::ImuSample> imu{0};
    vk::core::Publication<vk::msg::BodyVelocitySetpoint> body{0};
    vk::core::Publication<vk::msg::ActuatorSetpoint> direct{0};
    CommanderTask task{{.interval_us = interval_us, .actuators_started = true, .reset = &fake_reset}, queue};
};

TEST_F(CommanderTaskTest, ARebootIsRefusedWhileArmed)
{
    arm();
    EXPECT_EQ(reboot(queue, RebootAction::Bootloader).result, to_u8(RebootResult::Armed));
    step(500'000);
    EXPECT_TRUE(resets.empty());
}

TEST_F(CommanderTaskTest, AnAcceptedRebootHappensAfterTheDelayWithItsTarget)
{
    ASSERT_EQ(reboot(queue, RebootAction::Bootloader).result, to_u8(RebootResult::Ok));
    step(CommanderTask::reboot_delay_us - interval_us);
    EXPECT_TRUE(resets.empty()) << "the reply must leave before the reset";
    step(interval_us);
    ASSERT_FALSE(resets.empty());
    EXPECT_EQ(resets.front(), BootTarget::Bootloader);
}

TEST_F(CommanderTaskTest, APlainRebootStartsTheFirmware)
{
    ASSERT_EQ(reboot(queue, RebootAction::Reboot).result, to_u8(RebootResult::Ok));
    step(CommanderTask::reboot_delay_us);
    ASSERT_FALSE(resets.empty());
    EXPECT_EQ(resets.front(), BootTarget::Firmware);
}

TEST_F(CommanderTaskTest, ArmingIsRefusedWhileARebootIsPending)
{
    ASSERT_EQ(request(queue, vk::core::Command::Mode, to_u8(vk::core::Mode::Test)).result, 0);
    ASSERT_EQ(reboot(queue, RebootAction::Reboot).result, to_u8(RebootResult::Ok));
    EXPECT_EQ(request(queue, vk::core::Command::Arm, 1).result, to_u8(ArmingResult::Rebooting));
    EXPECT_FALSE(task.armed());
}

TEST_F(CommanderTaskTest, ARepeatedRequestKeepsTheFirstDeadlineAndTakesTheNewTarget)
{
    ASSERT_EQ(reboot(queue, RebootAction::Reboot).result, to_u8(RebootResult::Ok));
    step(CommanderTask::reboot_delay_us / 2 - interval_us);
    ASSERT_EQ(reboot(queue, RebootAction::Bootloader).result, to_u8(RebootResult::Ok));
    step(CommanderTask::reboot_delay_us / 2);
    ASSERT_FALSE(resets.empty());
    EXPECT_EQ(resets.front(), BootTarget::Bootloader);
}

TEST(CommanderTaskWithoutReset, ARebootIsNotAvailable)
{
    fake_now = 1'000'000;
    vk::core::WorkQueue queue{"wq:test", 1, &fake_clock};
    CommanderTask task{{.interval_us = interval_us}, queue};
    task.start();
    EXPECT_EQ(reboot(queue, RebootAction::Reboot).result, to_u8(RebootResult::NotAvailable));
}

TEST(CommanderTaskAfterAFault, PublishesTheResetCauseAndRefusesArming)
{
    fake_now = 1'000'000;
    vk::core::WorkQueue queue{"wq:test", 1, &fake_clock};
    vk::core::Publication<vk::msg::ImuSample> imu{0};
    vk::core::Subscription<vk::msg::VehicleStatus> status{0};
    CommanderTask task{{.interval_us = interval_us,
                        .actuators_started = true,
                        .reset_report = {.cause = vk::core::ResetCause::Fault,
                                         .fault = vk::core::Fault::Assert,
                                         .fault_address = 0x0801ABCD}},
                       queue};
    task.start();
    imu.publish(vk::msg::ImuSample{.timestamp = fake_now});
    fake_now += interval_us;
    queue.process();
    vk::msg::VehicleStatus published{};
    ASSERT_TRUE(status.copy(published));
    EXPECT_EQ(published.reset_cause, static_cast<std::uint8_t>(vk::core::ResetCause::Fault));
    EXPECT_EQ(published.fault, static_cast<std::uint8_t>(vk::core::Fault::Assert));
    EXPECT_EQ(published.fault_address, 0x0801ABCDU);
    ASSERT_EQ(request(queue, vk::core::Command::Mode, to_u8(vk::core::Mode::Test)).result, 0);
    EXPECT_EQ(request(queue, vk::core::Command::Arm, 1).result, to_u8(ArmingResult::UnexpectedReset));
}

}  // namespace

TEST_F(CommanderTaskTest, BodyVelocitySetpointsKeepTheVehicleArmedAndTimeOut)
{
    arm();
    send_body(0.5F, 0.0F, 1'000'000);
    EXPECT_TRUE(task.armed());
    // They stop: after COM_SP_TIMEOUT (0.5 s) the vehicle disarms for it.
    step(400'000);
    EXPECT_TRUE(task.armed());
    step(200'000);
    EXPECT_FALSE(task.armed());
    vk::msg::VehicleStatus status{};
    vk::core::Subscription<vk::msg::VehicleStatus> statuses{0};
    ASSERT_TRUE(statuses.copy(status));
    EXPECT_EQ(status.disarm_reason, static_cast<std::uint8_t>(vk::core::DisarmReason::SetpointTimeout));
}

TEST_F(CommanderTaskTest, TheLevelAcceptedLastDecidesWhetherTheTargetsAreIdle)
{
    arm();
    // A zero body velocity after a moving direct-actuator setpoint is idle:
    // the vehicle disarms after COM_DISARM_IDLE (10 s) of it.
    direct.publish({.timestamp = fake_now, .source = link0, .wheel_speed = {5.0F, 5.0F, NAN, NAN}});
    step(interval_us);
    send_body(0.0F, 0.0F, 9'000'000);
    EXPECT_TRUE(task.armed());
    send_body(0.0F, 0.0F, 1'100'000);
    EXPECT_FALSE(task.armed());
}

TEST_F(CommanderTaskTest, ATurnInPlaceIsNotIdle)
{
    arm();
    send_body(0.0F, 1.0F, 10'500'000);
    EXPECT_TRUE(task.armed());
}

namespace {

TEST_F(CommanderTaskTest, RequestsAreAnsweredWithTheStateAfterThem)
{
    const auto mode = request(queue, vk::core::Command::Mode, to_u8(vk::core::Mode::Test));
    EXPECT_EQ(mode.command, to_u8(vk::core::Command::Mode));
    EXPECT_EQ(mode.mode, to_u8(vk::core::Mode::Test));
    // Arming from another source: refused, the vehicle stays disarmed.
    const auto other = request(queue, vk::core::Command::Arm, 1, 1);
    EXPECT_EQ(other.source, 1);
    EXPECT_EQ(other.result, to_u8(ArmingResult::NotInControl));
    EXPECT_FALSE(other.armed);
    const auto estop = request(queue, vk::core::Command::EmergencyStop, 1, 1);
    EXPECT_TRUE(estop.emergency_stop) << "the emergency stop is accepted from any source";
    EXPECT_FALSE(request(queue, vk::core::Command::EmergencyStop, 0).emergency_stop);
    // An unknown mode is refused with the mode now.
    const auto unknown = request(queue, vk::core::Command::Mode, 200);
    EXPECT_EQ(unknown.result, to_u8(vk::core::ModeResult::UnknownMode));
    EXPECT_EQ(unknown.mode, to_u8(vk::core::Mode::Test));
}

TEST_F(CommanderTaskTest, SetpointsFromASourceNotInControlAreDroppedAndCounted)
{
    arm();
    const std::uint32_t before = task.setpoints_rejected();
    // From Link 1, not in control: they do not count as driving, so the
    // vehicle disarms as idle after COM_DISARM_IDLE (10 s), and each is counted.
    for (int i = 0; i < 2020; ++i) {
        body.publish({.timestamp = fake_now, .source = 1, .vx = 0.5F, .vy = 0.0F, .yaw_rate = 0.0F});
        step(interval_us);
    }
    EXPECT_FALSE(task.armed());
    EXPECT_EQ(task.state().disarm_reason, vk::core::DisarmReason::Idle);
    EXPECT_GE(task.setpoints_rejected() - before, 2000U);
    vk::core::Subscription<vk::msg::VehicleStatus> statuses{0};
    vk::msg::VehicleStatus status{};
    ASSERT_TRUE(statuses.copy(status));
    // vehicle_status comes five times a second: at most one interval behind.
    EXPECT_LE(status.setpoints_rejected, task.setpoints_rejected());
    EXPECT_GE(status.setpoints_rejected + CommanderTask::status_interval_us / interval_us, task.setpoints_rejected());
}

TEST_F(CommanderTaskTest, DisarmedSetpointsFromTheSourceInControlAreNotCountedRejected)
{
    using vk::core::Mode;
    using vk::core::remote_control;
    ASSERT_EQ(request(queue, vk::core::Command::Mode, to_u8(Mode::Manual), remote_control).result,
              to_u8(vk::core::ModeResult::Ok));
    // Engage the emergency stop as well, matching a vehicle left disarmed in MANUAL (#200).
    ASSERT_TRUE(request(queue, vk::core::Command::EmergencyStop, 1, remote_control).emergency_stop);
    ASSERT_FALSE(task.armed());

    const std::uint32_t before = task.setpoints_rejected();
    for (int i = 0; i < 60; ++i) {
        const float vx = i < 30 ? 0.0F : 0.3F;
        body.publish({.timestamp = fake_now, .source = remote_control, .vx = vx, .vy = 0.0F, .yaw_rate = 0.0F});
        step(interval_us);
    }
    EXPECT_EQ(task.setpoints_rejected(), before);

    // Released, still disarmed: own body velocity setpoints still do not count as rejected.
    ASSERT_FALSE(request(queue, vk::core::Command::EmergencyStop, 0, remote_control).emergency_stop);
    for (int i = 0; i < 40; ++i) {
        body.publish({.timestamp = fake_now, .source = remote_control, .vx = 0.25F, .vy = 0.0F, .yaw_rate = 0.0F});
        step(interval_us);
    }
    EXPECT_EQ(task.setpoints_rejected(), before);

    // Non-zero disarmed setpoints were ignored, so they do not trip SetpointsActive on arming.
    vk::core::Publication<vk::msg::ManualControlInput> manual_pub{0};
    manual_pub.publish({.timestamp = fake_now, .connected = true, .sticks = {0.0F, 0.0F, 0.0F, 0.0F}});
    step(interval_us);
    const auto armed = request(queue, vk::core::Command::Arm, 1, remote_control);
    EXPECT_EQ(armed.result, to_u8(ArmingResult::Ok));
    EXPECT_TRUE(armed.armed);
    ASSERT_EQ(request(queue, vk::core::Command::Arm, 0, remote_control).result, to_u8(ArmingResult::Ok));

    // A Link (not in control) or a disallowed level still counts as rejected while disarmed.
    for (int i = 0; i < 5; ++i) {
        body.publish({.timestamp = fake_now, .source = link0, .vx = 0.2F, .vy = 0.0F, .yaw_rate = 0.0F});
        step(interval_us);
    }
    EXPECT_EQ(task.setpoints_rejected(), before + 5U);

    direct.publish({.timestamp = fake_now, .source = remote_control, .wheel_speed = {1.0F, 1.0F, NAN, NAN}});
    step(interval_us);
    EXPECT_EQ(task.setpoints_rejected(), before + 6U);

    // vehicle_status publishes the steady count.
    step(CommanderTask::status_interval_us);
    vk::core::Subscription<vk::msg::VehicleStatus> statuses{0};
    vk::msg::VehicleStatus status{};
    ASSERT_TRUE(statuses.copy(status));
    EXPECT_EQ(status.setpoints_rejected, before + 6U);
}

TEST_F(CommanderTaskTest, TakeoverCountsLinksDroppedSetpointsOnly)
{
    using vk::core::Mode;
    using vk::core::remote_control;
    arm();
    vk::core::Publication<vk::msg::ManualControlInput> manual_pub{0};
    manual_pub.publish({.timestamp = fake_now, .connected = true, .sticks = {0.0F, 0.0F, 0.0F, 0.0F}});
    step(interval_us);
    ASSERT_EQ(request(queue, vk::core::Command::Mode, to_u8(Mode::Manual), remote_control).result,
              to_u8(vk::core::ModeResult::Ok));
    ASSERT_TRUE(task.armed());

    const std::uint32_t before = task.setpoints_rejected();
    for (int i = 0; i < 20; ++i) {
        manual_pub.publish({.timestamp = fake_now, .connected = true, .sticks = {0.0F, 0.0F, 0.0F, 0.0F}});
        body.publish({.timestamp = fake_now, .source = remote_control, .vx = 0.2F, .vy = 0.0F, .yaw_rate = 0.0F});
        step(interval_us);
        body.publish({.timestamp = fake_now, .source = link0, .vx = 0.2F, .vy = 0.0F, .yaw_rate = 0.0F});
        step(interval_us);
    }
    EXPECT_TRUE(task.armed());
    EXPECT_EQ(task.setpoints_rejected() - before, 20U);

    // Disarmed after takeover: RC setpoints stop counting as active and do not count as rejected.
    ASSERT_EQ(request(queue, vk::core::Command::Arm, 0, remote_control).result, to_u8(ArmingResult::Ok));
    const std::uint32_t after_disarm = task.setpoints_rejected();
    for (int i = 0; i < 20; ++i) {
        body.publish({.timestamp = fake_now, .source = remote_control, .vx = 0.0F, .vy = 0.0F, .yaw_rate = 0.0F});
        step(interval_us);
    }
    EXPECT_EQ(task.setpoints_rejected(), after_disarm);
}

TEST_F(CommanderTaskTest, ABringupRequestIsAnsweredWithItsRefusal)
{
    arm();
    const auto started = request(queue, vk::core::Command::Bringup, to_u8(vk::core::BringupCase::Motors));
    EXPECT_EQ(started.result, to_u8(vk::core::BringupStart::NotAvailable)) << "not built into this task";
}

TEST_F(CommanderTaskTest, RestartEstimatorsIsRefusedWhileArmed)
{
    vk::core::Subscription<vk::msg::EstimatorRestart> restart{0};
    vk::msg::EstimatorRestart msg{};
    while (restart.copy(msg)) {
    }
    arm();
    const auto ack = request(queue, vk::core::Command::RestartEstimators, 0);
    EXPECT_EQ(ack.command, to_u8(vk::core::Command::RestartEstimators));
    EXPECT_EQ(ack.result, to_u8(RestartEstimatorsResult::Armed));
    EXPECT_TRUE(ack.armed);
    EXPECT_FALSE(restart.copy(msg));
}

TEST_F(CommanderTaskTest, RestartEstimatorsIsAcceptedWhileDisarmed)
{
    vk::core::Subscription<vk::msg::EstimatorRestart> restart{0};
    vk::msg::EstimatorRestart msg{};
    while (restart.copy(msg)) {
    }
    const auto ack = request(queue, vk::core::Command::RestartEstimators, 0);
    EXPECT_EQ(ack.command, to_u8(vk::core::Command::RestartEstimators));
    EXPECT_EQ(ack.result, to_u8(RestartEstimatorsResult::Ok));
    EXPECT_FALSE(ack.armed);
    ASSERT_TRUE(restart.copy(msg));
    EXPECT_EQ(msg.timestamp, fake_now);
}

TEST_F(CommanderTaskTest, SetsMagMissingFromPrimaryEkfAttitude)
{
    vk::core::Publication<vk::msg::VehicleAttitude> attitude_pub{0};
    vk::core::Subscription<vk::msg::VehicleStatus> status_sub{0};
    vk::msg::VehicleStatus status{};

    attitude_pub.publish(vk::msg::VehicleAttitude{.estimator_id = 2, .mag_missing = true});
    step(interval_us);
    while (status_sub.copy(status)) {
    }
    EXPECT_TRUE(status.mag_missing);

    attitude_pub.publish(vk::msg::VehicleAttitude{.estimator_id = 2, .mag_missing = false});
    step(interval_us);
    while (status_sub.copy(status)) {
    }
    EXPECT_FALSE(status.mag_missing);
}

TEST_F(CommanderTaskTest, SetsMagMissingFromSecondaryEkfAttitude)
{
    vk::core::Publication<vk::msg::VehicleAttitude> primary_pub{0};
    vk::core::Publication<vk::msg::EstimatorAttitude> secondary_pub{1};
    vk::core::Subscription<vk::msg::VehicleStatus> status_sub{0};
    vk::msg::VehicleStatus status{};

    // Mahony is primary (estimator_id 1) and publishes mag_missing false.
    primary_pub.publish(vk::msg::VehicleAttitude{.estimator_id = 1, .mag_missing = false});
    // EKF is secondary on instance 1 (estimator_id 2) and publishes mag_missing true.
    secondary_pub.publish(vk::msg::EstimatorAttitude{.estimator_id = 2, .mag_missing = true});
    step(interval_us);
    while (status_sub.copy(status)) {
    }
    EXPECT_TRUE(status.mag_missing);

    // EKF secondary now clears mag_missing.
    secondary_pub.publish(vk::msg::EstimatorAttitude{.estimator_id = 2, .mag_missing = false});
    step(interval_us);
    while (status_sub.copy(status)) {
    }
    EXPECT_FALSE(status.mag_missing);
}

}  // namespace
