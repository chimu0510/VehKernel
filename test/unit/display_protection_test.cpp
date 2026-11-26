// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's protection of the panel (spec 0016, ADR-0022, seam C):
// dimming, turning off, waking by a key press (which is consumed) and by each
// vehicle event, never while armed, the menu not counting as idle, DSP_* at 0
// and the pixel cycle; then the work items together on a simulated panel.
#include <vk/core/bus.hpp>
#include <vk/core/vehicle_control.hpp>
#include <vk/core/work_queue.hpp>
#include <vk/drivers/ssd1306.hpp>
#include <vk/hal/display_bus.hpp>
#include <vk/local_display/display_protection.hpp>
#include <vk/local_display/local_display.hpp>
#include <vk/local_display/local_menu.hpp>
#include <vk/local_display/local_menu_task.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/msg/arming_state.hpp>
#include <vk/msg/vehicle_status.hpp>
#include <vk/params/params.hpp>
#include <vk/sim/ssd1306.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <array>
#include <cstddef>
#include <string>

namespace {

using vk::local_display::DisplayLevel;
using vk::local_display::DisplayProtection;
using vk::local_display::ProtectionSettings;

constexpr std::uint64_t second = 1'000'000;

// ---------------------------------------------------------------------------
// The decisions, with the time passed in.

class ProtectionTest : public ::testing::Test {
protected:
    void update(std::uint64_t seconds, bool armed = false, bool menu_open = false)
    {
        protection.update(seconds * second, armed, menu_open, settings);
    }

    ProtectionSettings settings;  // 100 %, dim at 60 s, off at 600 s, shift on
    DisplayProtection protection;
};

TEST_F(ProtectionTest, StartsAwakeAtTheNormalContrast)
{
    update(0);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    EXPECT_TRUE(protection.display_on());
    EXPECT_EQ(protection.contrast(), DisplayProtection::full_contrast);
}

TEST_F(ProtectionTest, DimsToAboutTenPercentAfterDspDimSecondsAndTurnsOffAfterDspOffSeconds)
{
    update(0);
    update(59);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    protection.update((60 * second) - 1, false, false, settings);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(60);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
    EXPECT_TRUE(protection.display_on());
    EXPECT_EQ(protection.contrast(), DisplayProtection::contrast_of(10));
    EXPECT_EQ(protection.contrast(), 24);
    update(599);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
    update(600);
    EXPECT_EQ(protection.level(), DisplayLevel::Off);
    EXPECT_FALSE(protection.display_on());
}

TEST_F(ProtectionTest, DspBrightSetsTheNormalContrastAndDimmingNeverRaisesIt)
{
    settings.bright_percent = 50;
    update(0);
    EXPECT_EQ(protection.contrast(), DisplayProtection::contrast_of(50));
    update(60);
    EXPECT_EQ(protection.contrast(), DisplayProtection::contrast_of(10));
    settings.bright_percent = 10;
    update(0);
    EXPECT_EQ(protection.contrast(), DisplayProtection::contrast_of(10));
    settings.bright_percent = 3;  // below the parameter's range: held at 10 %
    update(0);
    EXPECT_EQ(protection.contrast(), DisplayProtection::contrast_of(10));
    settings.bright_percent = 100;
    update(0);
    EXPECT_EQ(protection.contrast(), DisplayProtection::full_contrast);
}

TEST_F(ProtectionTest, AKeyPressOnADimmedDisplayOnlyWakesItAndRestartsTheIdleTime)
{
    update(0);
    update(70);
    ASSERT_EQ(protection.level(), DisplayLevel::Dimmed);
    EXPECT_TRUE(protection.on_key_press(70 * second));  // consumed
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(70);
    EXPECT_EQ(protection.contrast(), DisplayProtection::full_contrast);
    update(129);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(130);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
}

TEST_F(ProtectionTest, AKeyPressOnADarkDisplayOnlyWakesIt)
{
    update(0);
    update(700);
    ASSERT_EQ(protection.level(), DisplayLevel::Off);
    EXPECT_TRUE(protection.on_key_press(700 * second));
    EXPECT_TRUE(protection.display_on());
    update(700);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
}

TEST_F(ProtectionTest, AKeyPressOnAnAwakeDisplayActsAndRestartsTheIdleTime)
{
    update(0);
    update(50);
    EXPECT_FALSE(protection.on_key_press(50 * second));
    update(109);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(110);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
}

TEST_F(ProtectionTest, NeverDimmedOrOffWhileArmedAndTheIdleTimeStartsAtTheDisarm)
{
    update(0);
    update(1000, true);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    EXPECT_EQ(protection.contrast(), DisplayProtection::full_contrast);
    update(5000, true);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(5059);  // disarmed: the idle time counts from the last update while armed
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(5060);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
}

TEST_F(ProtectionTest, AnAlreadyDarkDisplayWakesWhenTheVehicleIsArmed)
{
    update(0);
    update(700);
    ASSERT_EQ(protection.level(), DisplayLevel::Off);
    update(701, true);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    EXPECT_TRUE(protection.display_on());
}

TEST_F(ProtectionTest, TheMenuDoesNotCountAsIdle)
{
    update(0);
    update(500, false, true);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(1000, false, true);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(1059);  // the menu closed at 1000 s
    update(1059, false, false);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
    update(1060);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
}

TEST_F(ProtectionTest, ZeroNeverDimsAndZeroNeverTurnsOff)
{
    settings.dim_seconds = 0;
    update(0);
    update(599);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);  // no dimming, off not yet
    update(600);
    EXPECT_EQ(protection.level(), DisplayLevel::Off);  // off alone
    settings.dim_seconds = 60;
    settings.off_seconds = 0;
    update(0);
    protection.on_key_press(1'000 * second);
    update(1'000);
    update(1'060);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
    update(1'000'000);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);  // never dark
    settings.dim_seconds = 0;
    update(2'000'000);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);  // neither: always on
    EXPECT_EQ(protection.contrast(), DisplayProtection::full_contrast);
}

TEST_F(ProtectionTest, ADisplayThatWouldBeOffBeforeItDimsIsOff)
{
    settings.dim_seconds = 100;
    settings.off_seconds = 30;
    update(0);
    update(30);
    EXPECT_EQ(protection.level(), DisplayLevel::Off);
}

// The vehicle events that wake the display.
vk::msg::VehicleStatus quiet_status()
{
    vk::msg::VehicleStatus status{};
    status.mode = 1;
    status.source = 0;
    return status;
}

class WakeEventTest : public ProtectionTest {
protected:
    // Dims the display, feeds `before` then `after`, and tells whether it woke.
    bool wakes(const vk::msg::VehicleStatus& before, const vk::msg::VehicleStatus& after)
    {
        DisplayProtection fresh;
        fresh.update(0, false, false, settings);
        fresh.on_status(0, before);
        fresh.update(100 * second, false, false, settings);
        EXPECT_EQ(fresh.level(), DisplayLevel::Dimmed);
        fresh.on_status(100 * second, after);
        const bool woke = fresh.level() == DisplayLevel::Awake;
        if (woke) {
            // The idle time restarted at the event.
            fresh.update(159 * second, false, false, settings);
            EXPECT_EQ(fresh.level(), DisplayLevel::Awake);
            fresh.update(160 * second, false, false, settings);
            EXPECT_EQ(fresh.level(), DisplayLevel::Dimmed);
        }
        return woke;
    }
};

TEST_F(WakeEventTest, ArmingAndDisarmingWake)
{
    auto armed = quiet_status();
    armed.armed = true;
    EXPECT_TRUE(wakes(quiet_status(), armed));
    EXPECT_TRUE(wakes(armed, quiet_status()));
}

TEST_F(WakeEventTest, AModeChangeWakes)
{
    auto changed = quiet_status();
    changed.mode = 2;
    EXPECT_TRUE(wakes(quiet_status(), changed));
}

TEST_F(WakeEventTest, TheEmergencyStopEngagingWakesAndItsReleaseDoesNot)
{
    auto engaged = quiet_status();
    engaged.emergency_stop = true;
    EXPECT_TRUE(wakes(quiet_status(), engaged));
    EXPECT_FALSE(wakes(engaged, quiet_status()));
}

TEST_F(WakeEventTest, ANewFaultWakes)
{
    auto fault = quiet_status();
    fault.fault = 2;
    EXPECT_TRUE(wakes(quiet_status(), fault));
    EXPECT_FALSE(wakes(fault, fault));
    auto other = fault;
    other.fault = 3;
    EXPECT_TRUE(wakes(fault, other));
}

TEST_F(WakeEventTest, AnArmingRefusalWakes)
{
    auto refused = quiet_status();
    refused.arming_refusal = 4;
    EXPECT_TRUE(wakes(quiet_status(), refused));
    EXPECT_FALSE(wakes(refused, refused));  // the same, still pending
    EXPECT_FALSE(wakes(refused, quiet_status()));
}

TEST_F(WakeEventTest, ATakeoverByTheRemoteControlWakes)
{
    auto remote = quiet_status();
    remote.source = vk::core::remote_control;
    EXPECT_TRUE(wakes(quiet_status(), remote));
    EXPECT_FALSE(wakes(remote, quiet_status()));
}

TEST_F(WakeEventTest, UnchangedStatusAndOtherFieldsDoNotWake)
{
    auto other = quiet_status();
    other.board_verified = true;
    other.reset_cause = 3;
    other.output_watchdog_trips = 9;
    EXPECT_FALSE(wakes(quiet_status(), other));
    EXPECT_FALSE(wakes(quiet_status(), quiet_status()));
}

TEST_F(WakeEventTest, TheFirstStatusOnlyRecordsTheState)
{
    protection.update(0, false, false, settings);
    protection.update(100 * second, false, false, settings);
    ASSERT_EQ(protection.level(), DisplayLevel::Dimmed);
    auto faulted = quiet_status();
    faulted.fault = 2;
    faulted.armed = true;
    protection.on_status(100 * second, faulted);
    EXPECT_EQ(protection.level(), DisplayLevel::Dimmed);
}

// The pixel cycle.
TEST_F(ProtectionTest, TheImageMovesByAPixelEveryTwoMinutesAlongAFourStepCycle)
{
    update(0);
    EXPECT_EQ(protection.shift_x(), 0);
    EXPECT_EQ(protection.shift_y(), 0);
    protection.update(DisplayProtection::shift_interval_us - 1, false, false, settings);
    EXPECT_EQ(protection.shift_x(), 0);
    constexpr std::array<std::array<int, 2>, 5> expected{{{1, 0}, {1, 1}, {0, 1}, {0, 0}, {1, 0}}};
    for (std::size_t step = 0; step < expected.size(); ++step) {
        protection.on_key_press((step + 1) * DisplayProtection::shift_interval_us);  // awake, whatever the time
        protection.update((step + 1) * DisplayProtection::shift_interval_us, false, false, settings);
        EXPECT_EQ(protection.shift_x(), expected.at(step).at(0)) << step;
        EXPECT_EQ(protection.shift_y(), expected.at(step).at(1)) << step;
    }
}

TEST_F(ProtectionTest, TheImageMovesWhileArmedToo)
{
    update(0, true);
    update(120, true);
    EXPECT_EQ(protection.shift_x(), 1);
    EXPECT_EQ(protection.shift_y(), 0);
    update(240, true);
    EXPECT_EQ(protection.shift_y(), 1);
    EXPECT_EQ(protection.level(), DisplayLevel::Awake);
}

TEST_F(ProtectionTest, DspShiftOffHoldsTheImageStill)
{
    settings.shift = false;
    update(0);
    update(120);
    update(240);
    EXPECT_EQ(protection.shift_x(), 0);
    EXPECT_EQ(protection.shift_y(), 0);
    settings.shift = true;  // switched on: the cycle is where it would be
    update(240);
    EXPECT_EQ(protection.shift_x(), 1);
    EXPECT_EQ(protection.shift_y(), 1);
}

TEST_F(ProtectionTest, ALateCallAdvancesOneStepAndKeepsTheTwoMinuteRhythmAfterwards)
{
    update(0);
    update(1000);  // far past several intervals
    EXPECT_EQ(protection.shift_x(), 1);
    EXPECT_EQ(protection.shift_y(), 0);
    update(1119);
    EXPECT_EQ(protection.shift_y(), 0);
    update(1120);
    EXPECT_EQ(protection.shift_y(), 1);
}

// ---------------------------------------------------------------------------
// The work items together, on a simulated panel and a fake clock.

std::uint64_t fake_now = 0;
std::uint64_t fake_clock() noexcept
{
    return fake_now;
}
bool key_is_down = false;
bool read_key()
{
    return key_is_down;
}

class ProtectedDisplayTest : public ::testing::Test {
protected:
    using LocalDisplay = vk::local_display::LocalDisplay;
    using LocalMenuTask = vk::local_display::LocalMenuTask;

    void SetUp() override
    {
        fake_now = 1'000'000;
        key_is_down = false;
        vk::params::dsp = vk::params::Dsp{};
        vk::params::dsp.dim_s = 5;
        vk::params::dsp.off_s = 10;
        arming.publish(vk::msg::ArmingState{});
        status.publish(quiet_status());
        display.start();
        menu_task.start();
        step(500'000);
    }

    void TearDown() override { vk::params::dsp = vk::params::Dsp{}; }

    // Runs the queue every 10 ms for `duration_us`.
    void step(std::uint64_t duration_us)
    {
        for (std::uint64_t end = fake_now + duration_us; fake_now < end;) {
            fake_now += 10'000;
            queue.process();
        }
    }
    // The key held down for `down_us`, then released and left for `after_us`.
    void press(std::uint64_t down_us, std::uint64_t after_us = 400'000)
    {
        key_is_down = true;
        step(down_us);
        key_is_down = false;
        step(after_us);
    }
    void publish_status(const vk::msg::VehicleStatus& value)
    {
        status.publish(value);
        step(300'000);  // the display reads topics every 200 ms
    }

    vk::core::WorkQueue queue{"wq:test", 1, &fake_clock};
    vk::sim::Ssd1306 panel;
    vk::drivers::Ssd1306 driver{vk::hal::DisplayDevice(panel)};
    DisplayProtection protection;
    LocalMenuTask menu_task{{.key_pressed = &read_key, .protection = &protection}, vk::params::parameters(),
                            vk::params::menu(), queue};
    LocalDisplay display{driver, &menu_task.menu(), protection, queue};
    vk::core::Publication<vk::msg::ArmingState> arming{0};
    vk::core::Publication<vk::msg::VehicleStatus> status{0};
};

TEST_F(ProtectedDisplayTest, TheContrastFollowsDspBrightAndTheDisplayDimsThenTurnsOff)
{
    EXPECT_TRUE(panel.displaying());
    EXPECT_EQ(panel.contrast(), 0xEF);
    step(5'000'000);
    EXPECT_TRUE(panel.displaying());
    EXPECT_EQ(panel.contrast(), 24);
    const std::string dimmed_image = panel.pbm();
    EXPECT_NE(dimmed_image.find_first_not_of('\0', dimmed_image.find("64\n") + 3), std::string::npos);  // drawn
    step(6'000'000);
    EXPECT_FALSE(panel.displaying());
    // The image is kept in the controller's RAM while the display is off.
    bool kept = false;
    for (std::size_t page = 0; page < 8 && !kept; ++page) {
        for (std::size_t column = 0; column < 128 && !kept; ++column) {
            kept = panel.ram(page, column) != 0;
        }
    }
    EXPECT_TRUE(kept);
}

TEST_F(ProtectedDisplayTest, AKeyPressOnADimmedDisplayOnlyWakesItAndOneOnADarkOneToo)
{
    step(6'000'000);
    ASSERT_EQ(panel.contrast(), 24);
    // A click would change the page; this press only wakes the display.
    press(100'000);
    EXPECT_EQ(panel.contrast(), 0xEF);
    EXPECT_EQ(menu_task.menu().page(), vk::local_display::Page::Overview);
    // Awake, the next click acts.
    press(100'000);
    ASSERT_EQ(menu_task.menu().page(), vk::local_display::Page::Attitude);

    step(11'000'000);
    ASSERT_FALSE(panel.displaying());
    press(100'000);
    EXPECT_TRUE(panel.displaying());
    EXPECT_EQ(menu_task.menu().page(), vk::local_display::Page::Attitude);

    // A long press that wakes the display does not act either: it would return to the overview.
    step(11'000'000);
    ASSERT_FALSE(panel.displaying());
    press(1'200'000);
    EXPECT_TRUE(panel.displaying());
    EXPECT_EQ(menu_task.menu().page(), vk::local_display::Page::Attitude);
    press(100'000);
    EXPECT_EQ(menu_task.menu().page(), vk::local_display::Page::Motion);
}

TEST_F(ProtectedDisplayTest, AVehicleEventWakesTheDisplay)
{
    step(6'000'000);
    ASSERT_EQ(panel.contrast(), 24);
    auto changed = quiet_status();
    changed.mode = 2;
    publish_status(changed);
    EXPECT_EQ(panel.contrast(), 0xEF);
    step(11'000'000);
    ASSERT_FALSE(panel.displaying());
    auto engaged = changed;
    engaged.emergency_stop = true;
    publish_status(engaged);
    EXPECT_TRUE(panel.displaying());
    EXPECT_EQ(panel.contrast(), 0xEF);
}

TEST_F(ProtectedDisplayTest, WhileArmedTheDisplayStaysOnAndBright)
{
    auto armed = quiet_status();
    armed.armed = true;
    publish_status(armed);
    step(60'000'000);
    EXPECT_TRUE(panel.displaying());
    EXPECT_EQ(panel.contrast(), 0xEF);
    auto disarmed = armed;
    disarmed.armed = false;
    publish_status(disarmed);
    EXPECT_EQ(panel.contrast(), 0xEF);
    step(5'000'000);
    EXPECT_EQ(panel.contrast(), 24);
}

TEST_F(ProtectedDisplayTest, TheMenuIsNotIdle)
{
    press(100'000, 100'000);
    press(100'000, 600'000);  // a double click opens the menu
    ASSERT_EQ(menu_task.menu().state(), vk::local_display::MenuState::Groups);
    for (int i = 0; i < 2; ++i) {
        step(9'000'000);  // past DSP_DIM_S, short of the menu's 30 s timeout
        EXPECT_EQ(panel.contrast(), 0xEF);
        press(100'000);   // a click keeps the menu open
    }
}

TEST_F(ProtectedDisplayTest, DspSettingsAtZeroNeverDimOrTurnOffAndDspBrightSetsTheContrast)
{
    vk::params::dsp.dim_s = 0;
    vk::params::dsp.off_s = 0;
    vk::params::dsp.bright = 50;
    step(100'000'000);
    EXPECT_TRUE(panel.displaying());
    EXPECT_EQ(panel.contrast(), DisplayProtection::contrast_of(50));
}

TEST_F(ProtectedDisplayTest, TheImageMovesByAPixelAfterTwoMinutesAndBackWhenDspShiftIsOff)
{
    vk::params::dsp.dim_s = 0;
    vk::params::dsp.off_s = 0;
    // A lit pixel of the unshifted image, away from the edges.
    bool found = false;
    std::size_t lit_x = 0;
    std::size_t lit_y = 0;
    for (std::size_t y = 0; y < 62 && !found; ++y) {
        for (std::size_t x = 0; x < 126 && !found; ++x) {
            if (panel.pixel(x, y)) {
                found = true;
                lit_x = x;
                lit_y = y;
            }
        }
    }
    ASSERT_TRUE(found);
    step(121'000'000);
    EXPECT_EQ(driver.offset_x(), 1);
    EXPECT_EQ(driver.offset_y(), 0);
    EXPECT_TRUE(panel.pixel(lit_x + 1, lit_y));
    vk::params::dsp.shift = 0;
    step(300'000);
    EXPECT_EQ(driver.offset_x(), 0);
    EXPECT_TRUE(panel.pixel(lit_x, lit_y));
}

}  // namespace
