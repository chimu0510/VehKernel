// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's arming checks, sensors, Links, remote control and system
// pages (spec 0016, ADR-0022): the helpers that are not drawing, and every page
// rendered from fixed inputs and compared bit for bit with a reference image.
// Run with VK_UPDATE_GOLDEN=1 to rewrite the images after an intended change,
// then review them.
#include "reference_image.hpp"

#include <vk/drivers/ssd1306.hpp>
#include <vk/local_display/graphics.hpp>
#include <vk/local_display/page_arming_checks.hpp>
#include <vk/local_display/page_links.hpp>
#include <vk/local_display/page_remote_control.hpp>
#include <vk/local_display/page_sensors.hpp>
#include <vk/local_display/page_system.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace {

using namespace vk::local_display;
using vk::drivers::MonoFrame;

// A page is drawn below the status bar and inside a one-pixel margin on every side.
::testing::AssertionResult matches(const MonoFrame& frame, std::string_view name)
{
    for (std::size_t y = 0; y < MonoFrame::height; ++y) {
        for (std::size_t x = 0; x < MonoFrame::width; ++x) {
            if (frame.get(x, y) && (x < 1 || x > 126 || y < 9 || y > 62)) {
                return ::testing::AssertionFailure() << name << " draws outside the page area at " << x << ", " << y;
            }
        }
    }
    return vk::test::matches_reference(frame, VK_PAGES_GOLDEN_DIR, name);
}

template <class Inputs, class Draw>
::testing::AssertionResult renders(const Inputs& inputs, Draw draw, std::string_view name)
{
    MonoFrame frame;
    Canvas canvas(frame);
    draw(canvas, inputs);
    return matches(frame, name);
}

constexpr std::uint32_t bit(vk::msg::ArmingResult result)
{
    return 1U << static_cast<unsigned>(result);
}

void set_name(vk::msg::WorkItemStatus& item, std::string_view name)
{
    std::memcpy(item.name.data(), name.data(), name.size());
}

// ---- Arming checks ----

ArmingChecksPageInputs arming_inputs(std::size_t scroll)
{
    using R = vk::msg::ArmingResult;
    ArmingChecksPageInputs inputs;
    vk::msg::ArmingChecks checks{};
    checks.failing = bit(R::BoardNotVerified) | bit(R::NotInControl) | bit(R::RcNotConnected);
    checks.skipped = bit(R::ActuatorPowerOff) | bit(R::SticksNotCentred);
    inputs.checks = checks;
    inputs.scroll = scroll;
    return inputs;
}

TEST(ArmingChecksPage, EveryCheckButOkHasALine)
{
    EXPECT_EQ(arming_checks_line_count, 13U);
    EXPECT_EQ(arming_checks_lines_fit, 6U);
    EXPECT_EQ(arming_checks_max_scroll(), 7U);
}

TEST(ArmingChecksPage, AClickScrollsAPageAtATimeThenMovesOn)
{
    std::size_t scroll = 0;
    std::size_t clicks = 0;
    while (arming_checks_can_scroll(scroll)) {
        scroll = arming_checks_scrolled(scroll);
        ++clicks;
    }
    EXPECT_EQ(scroll, arming_checks_max_scroll());
    EXPECT_EQ(clicks, 2U);
    EXPECT_EQ(arming_checks_scrolled(scroll), scroll);
}

TEST(ArmingChecksPage, FirstLines)
{
    EXPECT_TRUE(renders(arming_inputs(0), draw_arming_checks_page, "arming_checks_first"));
}

TEST(ArmingChecksPage, ScrolledOnePage)
{
    EXPECT_TRUE(renders(arming_inputs(6), draw_arming_checks_page, "arming_checks_scrolled"));
}

TEST(ArmingChecksPage, ScrolledToTheEnd)
{
    EXPECT_TRUE(renders(arming_inputs(7), draw_arming_checks_page, "arming_checks_end"));
}

TEST(ArmingChecksPage, ScrollPastTheEndIsTheEnd)
{
    MonoFrame past;
    MonoFrame end;
    Canvas past_canvas(past);
    Canvas end_canvas(end);
    draw_arming_checks_page(past_canvas, arming_inputs(99));
    draw_arming_checks_page(end_canvas, arming_inputs(7));
    EXPECT_EQ(vk::test::pbm_of(past), vk::test::pbm_of(end));
}

TEST(ArmingChecksPage, AllPassing)
{
    ArmingChecksPageInputs inputs;
    inputs.checks = vk::msg::ArmingChecks{};
    EXPECT_TRUE(renders(inputs, draw_arming_checks_page, "arming_checks_all_pass"));
}

TEST(ArmingChecksPage, NoData)
{
    EXPECT_TRUE(renders(ArmingChecksPageInputs{}, draw_arming_checks_page, "arming_checks_no_data"));
}

// ---- Sensors ----

TEST(SampleRateMeter, MeasuresTheRateFromTheTimestampsAndTheCountPublished)
{
    SampleRateMeter meter;
    EXPECT_FALSE(meter.rate_hz());
    for (std::uint64_t i = 0; i <= 1000; ++i) {
        meter.add(5'000 + (i * 1000), 20 + i);  // 1 kHz
    }
    EXPECT_FLOAT_EQ(meter.rate_hz().value_or(0.0F), 1000.0F);
}

TEST(SampleRateMeter, SeesAllTheSamplesWhenOnlyTheNewestOfEachBatchIsLooked)
{
    SampleRateMeter meter;
    // A look every 200 ms at a 500 Hz stream, 100 samples apart.
    for (std::uint64_t look = 0; look <= 10; ++look) {
        meter.add(look * 200'000, look * 100);
    }
    EXPECT_FLOAT_EQ(meter.rate_hz().value_or(0.0F), 500.0F);
}

TEST(SampleRateMeter, ShowsNothingBeforeAWindowIsFull)
{
    SampleRateMeter meter;
    for (std::uint64_t i = 0; i < 900; ++i) {
        meter.add(i * 1000, i);
    }
    EXPECT_FALSE(meter.rate_hz());
}

TEST(SampleRateMeter, FollowsAChangeOfRate)
{
    SampleRateMeter meter;
    std::uint64_t now = 0;
    std::uint64_t count = 0;
    for (int i = 0; i < 2100; ++i) {
        meter.add(now, count++);
        now += 1000;
    }
    EXPECT_FLOAT_EQ(meter.rate_hz().value_or(0.0F), 1000.0F);
    for (int i = 0; i < 700; ++i) {
        meter.add(now, count++);
        now += 4000;  // 250 Hz; the window that spans the change reads between the two
    }
    EXPECT_NEAR(meter.rate_hz().value_or(0.0F), 250.0F, 1.0F);
}

TEST(SampleRateMeter, ATimestampThatGoesBackStartsAgain)
{
    SampleRateMeter meter;
    for (std::uint64_t i = 0; i <= 1000; ++i) {
        meter.add(10'000'000 + (i * 1000), i);
    }
    meter.add(0, 0);
    for (std::uint64_t i = 1; i <= 100; ++i) {
        meter.add(i * 10'000, i);  // 100 Hz
    }
    EXPECT_NEAR(meter.rate_hz().value_or(0.0F), 100.0F, 0.5F);
}

TEST(MagnetometerCalibration, DefaultsAreNotACalibration)
{
    EXPECT_FALSE(magnetometer_calibration_applied(vk::params::Cal{}));
}

TEST(MagnetometerCalibration, AnyOffsetOrScaleOffItsDefaultIsOne)
{
    using Cal = vk::params::Cal;
    for (float Cal::* field : {&Cal::mag_xoff, &Cal::mag_yoff, &Cal::mag_zoff, &Cal::mag_xscale, &Cal::mag_yscale, &Cal::mag_zscale}) {
        Cal cal;
        cal.*field += 0.5F;
        EXPECT_TRUE(magnetometer_calibration_applied(cal));
    }
}

TEST(MagnetometerField, StrengthIsTheLengthOfTheVector)
{
    vk::msg::MagnetometerSample sample{};
    sample.field = {3.0F, 4.0F, 12.0F};
    EXPECT_FLOAT_EQ(magnetic_field_strength(sample), 13.0F);
}

SensorsPageInputs healthy_sensors()
{
    SensorsPageInputs inputs;
    vk::msg::VehicleStatus status{};
    status.board_verified = true;
    inputs.status = status;
    inputs.imu_rate_hz = 1000.0F;
    vk::msg::MagnetometerStatus mag{};
    mag.state = static_cast<std::uint8_t>(vk::msg::MagnetometerState::Running);
    mag.samples = 123456;
    mag.failures = 2;
    mag.last_failure = static_cast<std::uint8_t>(vk::msg::MagnetometerFailure::Stale);
    inputs.magnetometer_status = mag;
    vk::msg::MagnetometerSample sample{};
    sample.field = {20.0F, -10.0F, 40.5F};
    inputs.magnetometer_sample = sample;
    return inputs;
}

TEST(SensorsPage, Healthy)
{
    EXPECT_TRUE(renders(healthy_sensors(), draw_sensors_page, "sensors_healthy"));
}

TEST(SensorsPage, CalibrationApplied)
{
    SensorsPageInputs inputs = healthy_sensors();
    inputs.magnetometer_calibration_applied = true;
    EXPECT_TRUE(renders(inputs, draw_sensors_page, "sensors_calibrated"));
}

TEST(SensorsPage, MagnetometerLost)
{
    SensorsPageInputs inputs;
    inputs.status = vk::msg::VehicleStatus{};  // the IMU did not answer
    vk::msg::MagnetometerStatus mag{};
    mag.state = static_cast<std::uint8_t>(vk::msg::MagnetometerState::WaitingWhoAmI);
    mag.last_failure = static_cast<std::uint8_t>(vk::msg::MagnetometerFailure::Nack);
    mag.failures = 4096;
    inputs.magnetometer_status = mag;
    EXPECT_TRUE(renders(inputs, draw_sensors_page, "sensors_magnetometer_lost"));
}

TEST(SensorsPage, NoData)
{
    EXPECT_TRUE(renders(SensorsPageInputs{}, draw_sensors_page, "sensors_no_data"));
}

// ---- Links ----

vk::msg::LinkStatus link_status(std::uint8_t handshake, std::uint8_t scale, std::uint32_t drops, std::uint32_t lost,
                                std::uint32_t received)
{
    vk::msg::LinkStatus link{};
    link.handshake_state = handshake;
    link.rate_scale = scale;
    link.drops = drops;
    link.lost = lost;
    link.frames_received = received;
    return link;
}

LinksPageInputs links_inputs(vk::msg::Mode mode, std::uint8_t source)
{
    LinksPageInputs inputs;
    vk::msg::VehicleStatus status{};
    status.mode = static_cast<std::uint8_t>(mode);
    status.source = source;
    inputs.status = status;
    inputs.links.at(0) = link_status(static_cast<std::uint8_t>(vk::msg::HandshakeState::Connected), 100, 0, 0, 123456);
    inputs.links.at(1) = link_status(static_cast<std::uint8_t>(vk::msg::HandshakeState::PeerSilent), 61, 3, 12, 98);
    return inputs;
}

TEST(LinksPage, OneInControlAndOneDown)
{
    EXPECT_TRUE(renders(links_inputs(vk::msg::Mode::Test, 0), draw_links_page, "links_in_control_and_down"));
}

TEST(LinksPage, CompanionInControl)
{
    LinksPageInputs inputs = links_inputs(vk::msg::Mode::Companion, 1);
    inputs.links.at(1) = link_status(static_cast<std::uint8_t>(vk::msg::HandshakeState::Connected), 61, 3, 12, 98);
    EXPECT_TRUE(renders(inputs, draw_links_page, "links_companion"));
}

TEST(LinksPage, NoLinkInControl)
{
    EXPECT_TRUE(renders(links_inputs(vk::msg::Mode::Standby, 255), draw_links_page, "links_none_in_control"));
}

TEST(LinksPage, RemoteControlInControlMarksNoLink)
{
    EXPECT_TRUE(renders(links_inputs(vk::msg::Mode::Manual, 0x10), draw_links_page, "links_remote_control"));
}

TEST(LinksPage, WaitingAndNoData)
{
    LinksPageInputs inputs;
    inputs.links.at(0) = link_status(static_cast<std::uint8_t>(vk::msg::HandshakeState::Waiting), 100, 0, 0, 0);
    EXPECT_TRUE(renders(inputs, draw_links_page, "links_waiting_no_data"));
}

TEST(LinksPage, LargestCountsStayInsideTheMargin)
{
    LinksPageInputs inputs = links_inputs(vk::msg::Mode::Companion, 0);
    inputs.links.at(0) = link_status(static_cast<std::uint8_t>(vk::msg::HandshakeState::PeerSilent), 100, 4294967295U,
                                     4294967295U, 4294967295U);
    inputs.links.at(1) = link_status(static_cast<std::uint8_t>(vk::msg::HandshakeState::Waiting), 100, 4294967295U,
                                     4294967295U, 4294967295U);
    EXPECT_TRUE(renders(inputs, draw_links_page, "links_largest_counts"));
}

// ---- Remote control ----

vk::msg::ManualControlInput gamepad()
{
    vk::msg::ManualControlInput input{};
    input.connected = true;
    input.timestamp = 1'000'000;
    input.sticks = {-1.0F, 0.5F, 0.25F, -1.0F};
    input.triggers = {0.0F, 0.75F};
    input.buttons = static_cast<std::uint16_t>((1U << static_cast<unsigned>(vk::msg::GamepadButton::A)) |
                                               (1U << static_cast<unsigned>(vk::msg::GamepadButton::LeftShoulder)) |
                                               (1U << static_cast<unsigned>(vk::msg::GamepadButton::DpadUp)));
    return input;
}

RemoteControlPageInputs rc_inputs(const vk::msg::ManualControlInput& input, std::uint64_t now_us = 1'050'000)
{
    RemoteControlPageInputs inputs;
    inputs.input = input;
    inputs.now_us = now_us;
    return inputs;
}

TEST(RemoteControlPage, Connected)
{
    EXPECT_TRUE(renders(rc_inputs(gamepad()), draw_remote_control_page, "remote_control_connected"));
}

TEST(RemoteControlPage, CentredAndIdle)
{
    vk::msg::ManualControlInput input = gamepad();
    input.sticks = {0.0F, 0.0F, 0.0F, 0.0F};
    input.triggers = {0.0F, 0.0F};
    input.buttons = 0;
    EXPECT_TRUE(renders(rc_inputs(input), draw_remote_control_page, "remote_control_centred"));
}

TEST(RemoteControlPage, EveryButtonAndSticksAtTheLimits)
{
    vk::msg::ManualControlInput input = gamepad();
    input.sticks = {1.0F, 1.0F, -1.0F, -1.0F};
    input.triggers = {1.0F, 1.0F};
    input.buttons = 0xFFFF;
    EXPECT_TRUE(renders(rc_inputs(input), draw_remote_control_page, "remote_control_all_buttons"));
}

TEST(RemoteControlPage, Lost)
{
    vk::msg::ManualControlInput input = gamepad();
    input.connected = false;
    EXPECT_TRUE(renders(rc_inputs(input, 1'000'000 + 125'000'000), draw_remote_control_page, "remote_control_lost"));
}

TEST(RemoteControlPage, NeverConnected)
{
    EXPECT_TRUE(renders(RemoteControlPageInputs{}, draw_remote_control_page, "remote_control_never"));
}

// ---- System ----

vk::msg::WorkItemStatus work_item(std::string_view name, std::uint32_t max_us, std::uint32_t overruns)
{
    vk::msg::WorkItemStatus item{};
    set_name(item, name);
    item.run_time_max_us = max_us;
    item.overruns = overruns;
    return item;
}

TEST(SystemPage, TheWorstWorkItemIsTheOneThatOverranOrRanLongest)
{
    std::array<std::optional<vk::msg::WorkItemStatus>, 4> items;
    EXPECT_FALSE(worst_work_item(items));
    items.at(1) = work_item("a", 100, 0);
    EXPECT_EQ(worst_work_item(items), 1U);
    items.at(2) = work_item("b", 250, 0);
    EXPECT_EQ(worst_work_item(items), 2U);
    items.at(3) = work_item("c", 90, 1);
    EXPECT_EQ(worst_work_item(items), 3U);  // an overrun beats a long run
    items.at(0) = work_item("d", 80, 5);
    EXPECT_EQ(worst_work_item(items), 0U);  // the most overruns
    items.at(0) = work_item("d", 400, 1);
    EXPECT_EQ(worst_work_item(items), 0U);  // equal overruns: the longest run
    items.at(3) = work_item("c", 400, 1);
    EXPECT_EQ(worst_work_item(items), 0U);  // a tie: the lowest index
}

TEST(SystemPage, AWorkItemNameStopsAtTheFirstZero)
{
    EXPECT_EQ(work_item_name(work_item("local_display", 0, 0)), "local_display");
    vk::msg::WorkItemStatus full{};
    full.name.fill('x');
    EXPECT_EQ(work_item_name(full).size(), 16U);
}

SystemPageInputs system_inputs(bool dirty = false, std::uint32_t git_hash = 0x1A2B3C4DU,
                               vk::msg::ResetCause reset = vk::msg::ResetCause::PowerOn,
                               vk::msg::Fault fault = vk::msg::Fault::None)
{
    SystemPageInputs inputs;
    vk::msg::FirmwareInfo firmware{};
    firmware.version_major = 0;
    firmware.version_minor = 3;
    firmware.version_patch = 0;
    firmware.git_hash = git_hash;
    firmware.git_dirty = dirty;
    firmware.board_id = 22083;
    inputs.firmware = firmware;
    vk::msg::VehicleStatus status{};
    status.reset_cause = static_cast<std::uint8_t>(reset);
    status.fault = static_cast<std::uint8_t>(fault);
    inputs.status = status;
    vk::msg::ParameterStatus parameters{};
    parameters.parameter_count = 87;
    parameters.bytes_used = 1536;
    parameters.sector_size = 16384;
    inputs.parameters = parameters;
    inputs.work_items.at(0) = work_item("wq:ctrl", 450, 0);
    inputs.work_items.at(1) = work_item("local_display", 6800, 0);
    inputs.uptime_us = (3ULL * 3600ULL + 25ULL * 60ULL + 7ULL) * 1'000'000ULL;
    inputs.board_name = "c50c-v1_1";
    return inputs;
}

TEST(SystemPage, Clean)
{
    EXPECT_TRUE(renders(system_inputs(), draw_system_page, "system_clean"));
}

TEST(SystemPage, DirtyBuild)
{
    const SystemPageInputs inputs = system_inputs(true);
    EXPECT_TRUE(renders(inputs, draw_system_page, "system_dirty"));
}

TEST(SystemPage, FaultResetAnOverrunAndDays)
{
    SystemPageInputs inputs = system_inputs(false, 0, vk::msg::ResetCause::Fault, vk::msg::Fault::StackOverflow);
    inputs.work_items.at(2) = work_item("link0", 900, 12);
    inputs.uptime_us = (3ULL * 86400ULL + 5ULL * 3600ULL) * 1'000'000ULL;
    EXPECT_TRUE(renders(inputs, draw_system_page, "system_fault_overrun"));
}

TEST(SystemPage, NoData)
{
    EXPECT_TRUE(renders(SystemPageInputs{}, draw_system_page, "system_no_data"));
}

}  // namespace
