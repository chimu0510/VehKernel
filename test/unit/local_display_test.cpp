// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's status bar and pages (spec 0016, ADR-0022): the image
// each shows for fixed topic values, against reference PBM files, and the work
// item's timing on a fake clock: a key press on the panel within one run,
// topic values at most 5 Hz, the attitude page 10 Hz, and only the SSD1306
// pages that changed sent.
// Run with VK_UPDATE_GOLDEN=1 to rewrite the images after an intended change,
// then review them.
#include <vk/core/bus.hpp>
#include <vk/core/vehicle_control.hpp>
#include <vk/core/work_queue.hpp>
#include <vk/drivers/ssd1306.hpp>
#include <vk/estimation/math/quaternion.hpp>
#include <vk/hal/display_bus.hpp>
#include <vk/local_display/display_protection.hpp>
#include <vk/local_display/graphics.hpp>
#include <vk/local_display/local_display.hpp>
#include <vk/local_display/local_menu.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/local_display/status_bar.hpp>
#include <vk/local_display/text_buffer.hpp>
#include <vk/local_display/user_key.hpp>
#include <vk/params/params.hpp>
#include <vk/sim/ssd1306.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>

namespace {

using vk::local_display::Canvas;
using vk::local_display::Inputs;
using vk::local_display::Page;
using vk::local_display::StatusBarInputs;
using vk::local_display::TextBuffer;

constexpr float rad = std::numbers::pi_v<float> / 180.0F;
constexpr float nan = std::numeric_limits<float>::quiet_NaN();

vk::msg::VehicleStatus make_status(vk::msg::Mode mode, bool armed)
{
    vk::msg::VehicleStatus status{};
    status.mode = static_cast<std::uint8_t>(mode);
    status.armed = armed;
    status.source = vk::core::no_source;
    status.board_verified = true;
    status.actuator_power = 3;
    status.reset_cause = 1;
    return status;
}

vk::msg::LinkStatus make_link(std::uint8_t state)
{
    vk::msg::LinkStatus link{};
    link.handshake_state = state;
    return link;
}

vk::msg::VehicleAttitude make_attitude(float roll_deg, float pitch_deg, float yaw_deg)
{
    vk::msg::VehicleAttitude attitude{};
    attitude.q = vk::estimation::Quaternion::from_euler({.roll = roll_deg * rad, .pitch = pitch_deg * rad, .yaw = yaw_deg * rad}).array();
    attitude.estimator_id = 2;
    attitude.accel_rejected = 42;
    attitude.mag_rejected = 1234;
    attitude.mag_fused = 56'789;
    return attitude;
}

vk::msg::VehicleLocalOdometry make_odometry()
{
    vk::msg::VehicleLocalOdometry odometry{};
    odometry.velocity = {0.35F, 0.0F, 0.0F};
    odometry.yaw_rate = -12.3F * rad;
    odometry.position = {-12.34F, 5.6F, 0.0F};
    return odometry;
}

// Speed, target and duty of four outputs: the first following its target, the second behind
// it, the third reversing, the fourth not controlled.
void fill_wheels(Inputs& inputs, std::size_t wheels)
{
    vk::msg::WheelEncoders encoders{};
    encoders.speed = {5.0F, 3.0F, -4.0F, 0.0F};
    encoders.valid = 0b0111;  // output 4 has no encoder
    vk::msg::ActuatorOutputs outputs{};
    outputs.output = {0.5F, 0.4F, -0.3F, 0.0F};
    outputs.target = {5.0F, 6.5F, -2.0F, nan};
    inputs.encoders = encoders;
    inputs.outputs = outputs;
    inputs.frame = {.wheel_count = wheels, .outputs = {1, 2, 3, 4}, .max_speed = 10.0F};
}

// A vehicle in TEST mode, disarmed, with one Link up, as most pages are shown.
Inputs sample_inputs()
{
    Inputs inputs;
    vk::msg::VehicleStatus status = make_status(vk::msg::Mode::Test, false);
    status.source = 1;
    status.emergency_stop = true;
    status.actuator_power = 2;
    status.arming_refusal = 2;
    status.disarm_reason = 3;
    status.reset_cause = 4;
    inputs.status = status;
    inputs.attitude = make_attitude(10.0F, -20.5F, 135.0F);
    inputs.odometry = make_odometry();
    inputs.links.at(0) = make_link(2);
    inputs.links.at(1) = make_link(1);
    fill_wheels(inputs, 2);
    return inputs;
}

// The image the panel shows once the driver has written every page of what `draw` draws.
std::string panel_image(const std::function<void(Canvas&)>& draw)
{
    vk::sim::Ssd1306 panel;
    vk::drivers::Ssd1306 driver{vk::hal::DisplayDevice(panel)};
    Canvas canvas(driver.frame());
    draw(canvas);
    driver.update(0);
    for (int i = 0; i < 4; ++i) {
        driver.update(vk::drivers::Ssd1306::reset_time_us);
    }
    return panel.pbm();
}

std::string page_image(Page page, const Inputs& inputs)
{
    return panel_image([&](Canvas& canvas) { vk::local_display::render(page, inputs, canvas); });
}

StatusBarInputs bar(std::optional<vk::msg::VehicleStatus> status, std::array<std::optional<vk::msg::LinkStatus>, 2> links,
                    std::optional<vk::msg::ManualControlInput> manual, Page page)
{
    return {.status = status, .links = links, .manual = manual, .page = page};
}

std::string bar_image(const StatusBarInputs& inputs)
{
    return panel_image([&](Canvas& canvas) {
        canvas.clear();
        vk::local_display::draw_status_bar(canvas, inputs);
    });
}

void expect_golden(std::string_view name, const std::string& image)
{
    const std::filesystem::path golden = std::filesystem::path(VK_GOLDEN_DIR) / (std::string(name) + ".pbm");
    const char* update = std::getenv("VK_UPDATE_GOLDEN");
    if (update != nullptr && std::string_view(update) == "1") {
        std::filesystem::create_directories(golden.parent_path());
        std::ofstream(golden, std::ios::binary) << image;
    }
    std::ifstream file(golden, std::ios::binary);
    ASSERT_TRUE(file) << "missing golden image " << golden << "; run with VK_UPDATE_GOLDEN=1";
    const std::string expected((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(image, expected) << golden;
}

// ---------------------------------------------------------------------------
// The pages in turn, the mode tags and the text builder.

TEST(LocalDisplayPages, EightPagesFollowEachOtherInTurn)
{
    using vk::local_display::next_page;
    EXPECT_EQ(vk::local_display::page_count, 8U);
    Page page = Page::Overview;
    for (const Page expected : {Page::Attitude, Page::Motion, Page::ArmingChecks, Page::Sensors, Page::Links,
                                Page::RemoteControl, Page::System, Page::Overview}) {
        page = next_page(page);
        EXPECT_EQ(page, expected);
    }
}

TEST(LocalDisplayStatusBar, ModeTags)
{
    using vk::local_display::mode_tag;
    EXPECT_EQ(mode_tag(0), "STB");
    EXPECT_EQ(mode_tag(1), "TST");
    EXPECT_EQ(mode_tag(2), "MAN");
    EXPECT_EQ(mode_tag(3), "CMP");
    EXPECT_EQ(mode_tag(9), "???");
}

TEST(LocalDisplayText, TheBuilderFormatsNumbers)
{
    EXPECT_EQ(TextBuffer().add_fixed(-0.04F, 1).view(), "0.0");  // no sign for a zero
    EXPECT_EQ(TextBuffer().add_fixed(-12.345F, 2).view(), "-12.35");
    EXPECT_EQ(TextBuffer().add_fixed(7.0F, 0).view(), "7");
    EXPECT_EQ(TextBuffer().add_fixed(nan, 1).view(), "---");
    EXPECT_EQ(TextBuffer().add_count(999).view(), "999");
    EXPECT_EQ(TextBuffer().add_count(12'345).view(), "12K");
    EXPECT_EQ(TextBuffer().add_count(4'000'000).view(), "4M");
    EXPECT_EQ(TextBuffer().add("AB").right_align(5).view(), "   AB");
    EXPECT_EQ(TextBuffer().add("AB").pad_to(4).add("C").view(), "AB  C");
}

// ---------------------------------------------------------------------------
// The status bar, in each mode and state, against reference images.

TEST(LocalDisplayImages, StatusBarStandbyDisarmedNothingConnected)
{
    expect_golden("bar_standby", bar_image(bar(make_status(vk::msg::Mode::Standby, false), {}, std::nullopt, Page::Overview)));
}

TEST(LocalDisplayImages, StatusBarTestArmedLink0ConnectedInControl)
{
    vk::msg::VehicleStatus status = make_status(vk::msg::Mode::Test, true);
    status.source = 0;
    expect_golden("bar_test_armed_link0",
                  bar_image(bar(status, {make_link(1), std::nullopt}, std::nullopt, Page::Attitude)));
}

TEST(LocalDisplayImages, StatusBarManualGamepadInControlLink1SilentEmergencyStop)
{
    vk::msg::VehicleStatus status = make_status(vk::msg::Mode::Manual, true);
    status.source = vk::core::remote_control;
    status.emergency_stop = true;
    vk::msg::ManualControlInput manual{};
    manual.connected = true;
    expect_golden("bar_manual_gamepad",
                  bar_image(bar(status, {make_link(1), make_link(2)}, manual, Page::Motion)));
}

TEST(LocalDisplayImages, StatusBarCompanionLink1InControlWithAnArmingRefusal)
{
    vk::msg::VehicleStatus status = make_status(vk::msg::Mode::Companion, false);
    status.source = 1;
    status.arming_refusal = 7;
    expect_golden("bar_companion_refusal",
                  bar_image(bar(status, {make_link(1), make_link(1)}, std::nullopt, Page::Links)));
}

TEST(LocalDisplayImages, StatusBarWithAFaultAndTheGamepadDisconnected)
{
    vk::msg::VehicleStatus status = make_status(vk::msg::Mode::Standby, false);
    status.fault = 1;
    status.arming_refusal = 11;
    vk::msg::ManualControlInput manual{};
    manual.connected = false;
    expect_golden("bar_fault", bar_image(bar(status, {}, manual, Page::System)));
}

TEST(LocalDisplayImages, StatusBarWithoutAStatus)
{
    expect_golden("bar_no_status", bar_image(bar(std::nullopt, {}, std::nullopt, Page::Sensors)));
}

// ---------------------------------------------------------------------------
// The pages, from fixed inputs.

TEST(LocalDisplayImages, Overview)
{
    expect_golden("overview", page_image(Page::Overview, sample_inputs()));
}

TEST(LocalDisplayImages, OverviewArmedInManualWithTheGamepadInControl)
{
    Inputs inputs = sample_inputs();
    vk::msg::VehicleStatus status = make_status(vk::msg::Mode::Manual, true);
    status.source = vk::core::remote_control;
    inputs.status = status;
    inputs.attitude = make_attitude(0.0F, 0.0F, -30.0F);  // 330 degrees from north
    vk::msg::VehicleLocalOdometry odometry = make_odometry();
    odometry.velocity[0] = -12.345F;
    inputs.odometry = odometry;
    expect_golden("overview_armed_manual", page_image(Page::Overview, inputs));
}

TEST(LocalDisplayImages, OverviewWithMissingData)
{
    expect_golden("overview_missing", page_image(Page::Overview, Inputs{}));
}

TEST(LocalDisplayImages, AttitudeLevel)
{
    Inputs inputs = sample_inputs();
    inputs.attitude = make_attitude(0.0F, 0.0F, 0.0F);
    expect_golden("attitude_level", page_image(Page::Attitude, inputs));
}

TEST(LocalDisplayImages, AttitudeRolledRight)
{
    Inputs inputs = sample_inputs();
    inputs.attitude = make_attitude(25.0F, 0.0F, 90.0F);
    expect_golden("attitude_rolled", page_image(Page::Attitude, inputs));
}

TEST(LocalDisplayImages, AttitudePitchedUp)
{
    Inputs inputs = sample_inputs();
    inputs.attitude = make_attitude(0.0F, 15.0F, -45.0F);
    expect_golden("attitude_pitched", page_image(Page::Attitude, inputs));
}

TEST(LocalDisplayImages, AttitudeRolledAndPitched)
{
    expect_golden("attitude_both", page_image(Page::Attitude, sample_inputs()));
}

TEST(LocalDisplayImages, AttitudePastTheScale)
{
    Inputs inputs = sample_inputs();
    vk::msg::VehicleAttitude attitude = make_attitude(-80.0F, -45.0F, 170.0F);
    attitude.estimator_id = 1;
    inputs.attitude = attitude;
    expect_golden("attitude_extreme", page_image(Page::Attitude, inputs));
}

TEST(LocalDisplayImages, AttitudeWithoutAnEstimate)
{
    Inputs inputs = sample_inputs();
    inputs.attitude.reset();
    expect_golden("attitude_missing", page_image(Page::Attitude, inputs));
}

TEST(LocalDisplayImages, MotionTwoWheels)
{
    expect_golden("motion_2wd", page_image(Page::Motion, sample_inputs()));
}

TEST(LocalDisplayImages, MotionFourWheels)
{
    Inputs inputs = sample_inputs();
    fill_wheels(inputs, 4);
    expect_golden("motion_4wd", page_image(Page::Motion, inputs));
}

TEST(LocalDisplayImages, MotionWithMissingOdometry)
{
    Inputs inputs = sample_inputs();
    inputs.odometry.reset();
    expect_golden("motion_no_odometry", page_image(Page::Motion, inputs));
}

TEST(LocalDisplayImages, MotionWithMissingWheelData)
{
    Inputs inputs = sample_inputs();
    inputs.encoders.reset();
    inputs.outputs.reset();
    expect_golden("motion_no_wheels", page_image(Page::Motion, inputs));
}

TEST(LocalDisplayImages, MotionWithoutAKnownFrame)
{
    Inputs inputs = sample_inputs();
    inputs.frame.wheel_count = 0;
    expect_golden("motion_no_frame", page_image(Page::Motion, inputs));
}

class LocalDisplayPlaceholders : public ::testing::TestWithParam<Page> {};

TEST_P(LocalDisplayPlaceholders, ShowThePageNameAndTheirPageDot)
{
    const Page page = GetParam();
    expect_golden("placeholder_" + std::to_string(static_cast<int>(page)), page_image(page, sample_inputs()));
}

std::string page_test_name(const ::testing::TestParamInfo<Page>& info)
{
    constexpr std::array<std::string_view, vk::local_display::page_count> names{
        "Overview", "Attitude", "Motion", "ArmingChecks", "Sensors", "Links", "RemoteControl", "System"};
    return std::string(names.at(static_cast<std::size_t>(info.param)));
}

INSTANTIATE_TEST_SUITE_P(Pages, LocalDisplayPlaceholders,
                         ::testing::Values(Page::ArmingChecks, Page::Sensors, Page::Links, Page::RemoteControl,
                                           Page::System),
                         page_test_name);

// ---------------------------------------------------------------------------
// The work item on a fake clock, fed the sample topics.

std::uint64_t fake_now = 0;

std::uint64_t fake_clock() noexcept
{
    return fake_now;
}

class LocalDisplayWorkItemTest : public ::testing::Test {
protected:
    using LocalDisplay = vk::local_display::LocalDisplay;

    void SetUp() override
    {
        fake_now = 1'000'000;
        publish(sample_inputs());
        display.start();
        step(400'000);  // reset, initialisation and the first screen
        panel.take_written_pages();
    }

    void publish(const Inputs& inputs)
    {
        if (inputs.status) {
            status.publish(*inputs.status);
        }
        if (inputs.attitude) {
            attitude.publish(*inputs.attitude);
        }
        if (inputs.odometry) {
            odometry.publish(*inputs.odometry);
        }
        if (inputs.encoders) {
            encoders.publish(*inputs.encoders);
        }
        if (inputs.outputs) {
            outputs.publish(*inputs.outputs);
        }
        if (const auto& first_link = inputs.links.at(0)) {
            link.publish(*first_link);
        }
    }

    // Runs the queue every run interval for `duration_us`; returns the runs that wrote the panel.
    int step(std::uint64_t duration_us, const Inputs* changing = nullptr)
    {
        int writes = 0;
        for (std::uint64_t end = fake_now + duration_us; fake_now < end;) {
            fake_now += LocalDisplay::interval_us;
            if (changing != nullptr && changing->attitude) {
                vk::msg::VehicleAttitude changed = *changing->attitude;
                changed.accel_rejected = static_cast<std::uint32_t>(fake_now / LocalDisplay::interval_us);
                attitude.publish(changed);
            }
            queue.process();
            writes += panel.take_written_pages() != 0 ? 1 : 0;
        }
        return writes;
    }

    // The image the work item draws for `page` from the sample topics, with the wheels of this build.
    std::string expected_image(Page page)
    {
        Inputs inputs = sample_inputs();
        inputs.links.at(1).reset();  // only the first Link is published here
        inputs.frame = display_frame();
        return page_image(page, inputs);
    }

    static vk::local_display::WheelFrame display_frame()
    {
        vk::local_display::WheelFrame frame;
#ifdef CONFIG_VEHICLE_MODEL_WHEELED
        frame.wheel_count = vk::params::frame.type == 1 ? 4 : 2;
        frame.outputs = {static_cast<std::uint8_t>(vk::params::whl.left_out),
                         static_cast<std::uint8_t>(vk::params::whl.right_out),
                         static_cast<std::uint8_t>(vk::params::whl.rear_l_out),
                         static_cast<std::uint8_t>(vk::params::whl.rear_r_out)};
#endif
#ifdef CONFIG_ACTUATOR_CONTROL
        frame.max_speed = vk::params::mot.max_spd;
#endif
        return frame;
    }

    vk::core::WorkQueue queue{"wq:test", 1, &fake_clock};
    vk::sim::Ssd1306 panel;
    vk::drivers::Ssd1306 driver{vk::hal::DisplayDevice(panel)};
    vk::local_display::LocalMenu menu{vk::params::parameters(), {}};
    vk::local_display::DisplayProtection protection;
    LocalDisplay display{driver, &menu, protection, queue};
    vk::core::Publication<vk::msg::VehicleStatus> status{0};
    vk::core::Publication<vk::msg::VehicleAttitude> attitude{0};
    vk::core::Publication<vk::msg::VehicleLocalOdometry> odometry{0};
    vk::core::Publication<vk::msg::WheelEncoders> encoders{0};
    vk::core::Publication<vk::msg::ActuatorOutputs> outputs{0};
    vk::core::Publication<vk::msg::LinkStatus> link{0};
};

TEST_F(LocalDisplayWorkItemTest, TheDisplayStartsOnTheOverview)
{
    EXPECT_EQ(panel.pbm(), expected_image(Page::Overview));
}

TEST_F(LocalDisplayWorkItemTest, AKeyPressIsWhollyOnThePanelAfterOneRun)
{
    menu.on_key(vk::local_display::KeyEvent::Click, fake_now, false);
    step(LocalDisplay::interval_us);
    EXPECT_EQ(panel.pbm(), expected_image(Page::Attitude));
}

TEST_F(LocalDisplayWorkItemTest, TheDisplayNeverChangesPageByItself)
{
    vk::msg::VehicleStatus faulty = make_status(vk::msg::Mode::Test, false);
    faulty.fault = 1;
    faulty.arming_refusal = 7;
    status.publish(faulty);
    step(3'000'000);
    // The page is still the overview; only the status bar's warning appears.
    EXPECT_EQ(menu.page(), Page::Overview);
    EXPECT_NE(panel.pbm(), expected_image(Page::Overview));
}

TEST_F(LocalDisplayWorkItemTest, OnlyTheSsd1306PagesThatChangedAreSent)
{
    // The overview's heading and speed are on the image's rows 52 to 58: pages 6 and 7.
    vk::msg::VehicleLocalOdometry faster = make_odometry();
    faster.velocity[0] = 1.23F;
    odometry.publish(faster);
    std::uint8_t written = 0;
    for (int run = 0; run < 8; ++run) {
        fake_now += LocalDisplay::interval_us;
        queue.process();
        written = static_cast<std::uint8_t>(written | panel.take_written_pages());
    }
    // Two of the eight pages: the panel is mounted upside down, so they are RAM pages 0 and 1.
    EXPECT_EQ(std::popcount(written), 2);
    EXPECT_EQ(written, 0b0000'0011);
}

TEST_F(LocalDisplayWorkItemTest, AnUnchangedScreenWritesNothing)
{
    step(1'000'000);
    EXPECT_EQ(step(1'000'000), 0);
}

TEST_F(LocalDisplayWorkItemTest, TopicValuesChangeAtMostFiveTimesASecond)
{
    // accel_rejected is shown on the attitude page only, which refreshes at 10 Hz; the overview shows
    // the odometry's speed, which changes before every run here and may change at most 5 Hz.
    vk::msg::VehicleLocalOdometry changing = make_odometry();
    int writes = 0;
    for (std::uint64_t end = fake_now + 1'000'000; fake_now < end;) {
        fake_now += LocalDisplay::interval_us;
        const std::uint64_t run = fake_now / LocalDisplay::interval_us;
        changing.velocity[0] = static_cast<float>(run) * 0.01F;
        odometry.publish(changing);
        queue.process();
        writes += panel.take_written_pages() != 0 ? 1 : 0;
    }
    EXPECT_GE(writes, 4);
    EXPECT_LE(writes, 5);
}

TEST_F(LocalDisplayWorkItemTest, TheAttitudePageChangesAtMostTenTimesASecond)
{
    menu.on_key(vk::local_display::KeyEvent::Click, fake_now, false);
    step(LocalDisplay::interval_us);
    panel.take_written_pages();
    const Inputs inputs = sample_inputs();
    const int writes = step(1'000'000, &inputs);
    EXPECT_GE(writes, 9);
    EXPECT_LE(writes, 10);
}

}  // namespace
