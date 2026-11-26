// SPDX-License-Identifier: GPL-3.0-or-later
// The user key's gestures and the local menu (ADR-0022, spec 0016, seam B):
// debounce, click, double click, long press, the 250 ms boundary, a held key
// and a key pressed at boot, over simulated time; and the menu: entering,
// moving, wrapping, going back, editing, x10, DEF, SAVE accepted and refused,
// reboot required, armed behaviour and the timeout, and what the menu screens
// show, against reference images.
// Run with VK_UPDATE_GOLDEN=1 to rewrite the images after an intended change,
// then review them.
#include <vk/core/bus.hpp>
#include <vk/core/parameters.hpp>
#include <vk/core/vehicle_control.hpp>
#include <vk/drivers/ssd1306.hpp>
#include <vk/hal/display_bus.hpp>
#include <vk/local_display/graphics.hpp>
#include <vk/local_display/local_menu.hpp>
#include <vk/local_display/page_arming_checks.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/local_display/status_bar.hpp>
#include <vk/local_display/user_key.hpp>
#include <vk/msg/arming_state.hpp>
#include <vk/sim/ssd1306.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using vk::core::MenuParam;
using vk::core::ParamInfo;
using vk::core::ParamResult;
using vk::core::ParamType;
using vk::core::ParamValue;
using vk::local_display::KeyEvent;
using vk::local_display::LocalMenu;
using vk::local_display::MenuAction;
using vk::local_display::MenuState;
using vk::local_display::Page;
using vk::local_display::UserKey;

constexpr std::uint64_t tick = UserKey::sample_interval_us;

// ---------------------------------------------------------------------------
// The gesture recognizer over simulated time.

class Segment {
public:
    constexpr Segment(std::uint64_t duration, bool level) : duration_ms(duration), pressed(level) {}
    std::uint64_t duration_ms;
    bool pressed;
};
class Timed {
public:
    constexpr Timed(std::uint64_t time, KeyEvent kind) : time_ms(time), event(kind) {}
    std::uint64_t time_ms;
    KeyEvent event;
    friend bool operator==(const Timed&, const Timed&) = default;
};

// Samples the key every 10 ms from `now_us`, at the level of each segment in turn.
std::vector<Timed> run(UserKey& key, std::uint64_t& now_us, std::initializer_list<Segment> script)
{
    std::vector<Timed> events;
    const std::uint64_t start = now_us;
    for (const Segment& segment : script) {
        for (std::uint64_t end = now_us + (segment.duration_ms * 1000); now_us < end; now_us += tick) {
            const KeyEvent event = key.sample(now_us, segment.pressed);
            if (event != KeyEvent::None) {
                events.emplace_back((now_us - start) / 1000, event);
            }
        }
    }
    return events;
}

// The recognizer, past its boot with the key released.
class UserKeyTest : public ::testing::Test {
protected:
    void SetUp() override { run(key, now, {{100, false}}); }

    UserKey key;
    std::uint64_t now = 0;
};

TEST_F(UserKeyTest, ALevelCountsAfterThreeSamplesInARow)
{
    EXPECT_TRUE(run(key, now, {{20, true}, {100, false}}).empty());  // two samples: a bounce
    // Three samples press, at the third, 20 ms in.
    EXPECT_EQ(run(key, now, {{30, true}}), (std::vector<Timed>{{20, KeyEvent::Press}}));
}

TEST_F(UserKeyTest, AClickIsReportedWhenNoSecondPressCameWithin250MsOfTheRelease)
{
    // Pressed 100 ms; the release counts 20 ms after the key came up, at 120 ms.
    EXPECT_EQ(run(key, now, {{100, true}, {400, false}}),
              (std::vector<Timed>{{20, KeyEvent::Press}, {120, KeyEvent::Release}, {370, KeyEvent::Click}}));
}

TEST_F(UserKeyTest, ADoubleClickIsReportedWhenTheSecondPressGoesDown)
{
    // The release counts at 120 ms; the second press, down from 200 ms, counts at 220 ms: 100 ms later.
    EXPECT_EQ(run(key, now, {{100, true}, {100, false}, {100, true}, {400, false}}),
              (std::vector<Timed>{{20, KeyEvent::Press},
                                  {120, KeyEvent::Release},
                                  {220, KeyEvent::Double},
                                  {320, KeyEvent::Release}}));
}

TEST_F(UserKeyTest, ASecondPress240MsAfterTheReleaseIsADoubleClickAndOneAt250MsIsAClickAndANewPress)
{
    // The release counts at 120 ms. A second press that counts at 360 ms is 240 ms later: inside the time.
    EXPECT_EQ(run(key, now, {{100, true}, {240, false}, {100, true}, {400, false}}),
              (std::vector<Timed>{{20, KeyEvent::Press},
                                  {120, KeyEvent::Release},
                                  {360, KeyEvent::Double},
                                  {460, KeyEvent::Release}}));
    run(key, now, {{500, false}});
    // One that would count at 370 ms, 250 ms later: the click is due first, and the press is a new one.
    EXPECT_EQ(run(key, now, {{100, true}, {250, false}, {100, true}, {500, false}}),
              (std::vector<Timed>{{20, KeyEvent::Press},
                                  {120, KeyEvent::Release},
                                  {370, KeyEvent::Click},
                                  {380, KeyEvent::Press},
                                  {470, KeyEvent::Release},
                                  {720, KeyEvent::Click}}));
}

TEST_F(UserKeyTest, ALongPressIsReportedWhileStillHeldAfterOneSecond)
{
    // The press counts at 20 ms; one second later, at 1020 ms, the long press is due.
    EXPECT_EQ(run(key, now, {{1010, true}}),
              (std::vector<Timed>{{20, KeyEvent::Press}}));
    EXPECT_EQ(run(key, now, {{50, true}}), (std::vector<Timed>{{10, KeyEvent::Long}}));
    // Released later: only the release, and no click after it.
    EXPECT_EQ(run(key, now, {{500, false}}), (std::vector<Timed>{{20, KeyEvent::Release}}));
}

TEST_F(UserKeyTest, APressReleasedJustBeforeOneSecondIsAClick)
{
    const auto events = run(key, now, {{960, true}, {400, false}});
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events.at(2).event, KeyEvent::Click);
}

TEST_F(UserKeyTest, AKeyHeldAfterADoubleClickGivesNoLongPress)
{
    const auto events = run(key, now, {{100, true}, {100, false}, {3000, true}, {400, false}});
    std::vector<KeyEvent> kinds;
    std::ranges::transform(events, std::back_inserter(kinds), [](const Timed& t) { return t.event; });
    EXPECT_EQ(kinds, (std::vector{KeyEvent::Press, KeyEvent::Release, KeyEvent::Double, KeyEvent::Release}));
}

TEST_F(UserKeyTest, AStuckKeyGivesOneLongPressAndNothingElse)
{
    const auto events = run(key, now, {{30'000, true}});
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events.at(0).event, KeyEvent::Press);
    EXPECT_EQ(events.at(1).event, KeyEvent::Long);
}

TEST_F(UserKeyTest, ASecondGestureNeedsTheKeyReleasedFirst)
{
    // A long press, released, then a click: each reported once.
    const auto events = run(key, now, {{1200, true}, {300, false}, {100, true}, {400, false}});
    std::vector<KeyEvent> kinds;
    std::ranges::transform(events, std::back_inserter(kinds), [](const Timed& t) { return t.event; });
    EXPECT_EQ(kinds, (std::vector{KeyEvent::Press, KeyEvent::Long, KeyEvent::Release, KeyEvent::Press,
                                  KeyEvent::Release, KeyEvent::Click}));
}

TEST_F(UserKeyTest, ABounceWhileHeldDoesNotEndThePress)
{
    // 10 ms of released reading inside a held key: fewer than three samples.
    const auto events = run(key, now, {{500, true}, {10, false}, {600, true}});
    std::vector<KeyEvent> kinds;
    std::ranges::transform(events, std::back_inserter(kinds), [](const Timed& t) { return t.event; });
    EXPECT_EQ(kinds, (std::vector{KeyEvent::Press, KeyEvent::Long}));
}

TEST_F(UserKeyTest, ADownKeyIsReported)
{
    EXPECT_FALSE(key.down());
    run(key, now, {{100, true}});
    EXPECT_TRUE(key.down());
    run(key, now, {{100, false}});
    EXPECT_FALSE(key.down());
}

TEST_F(UserKeyTest, ASwallowedPressGivesNoClickLongPressOrDoubleClick)
{
    EXPECT_EQ(run(key, now, {{40, true}}), (std::vector<Timed>{{20, KeyEvent::Press}}));
    key.swallow();
    // Held for two seconds, released and left for a while: only the release.
    const auto events = run(key, now, {{2000, true}, {600, false}});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.at(0).event, KeyEvent::Release);
    // The next click is ordinary.
    const auto next = run(key, now, {{100, true}, {400, false}});
    EXPECT_EQ(next.back().event, KeyEvent::Click);
}

TEST_F(UserKeyTest, SwallowingAfterTheReleaseDropsThePendingClick)
{
    run(key, now, {{100, true}, {40, false}});
    key.swallow();
    EXPECT_TRUE(run(key, now, {{500, false}}).empty());
}

TEST(UserKey, AKeyPressedAtBootIsIgnoredUntilReleased)
{
    UserKey key;
    std::uint64_t now = 0;
    EXPECT_TRUE(run(key, now, {{3000, true}}).empty());
    EXPECT_TRUE(run(key, now, {{100, false}}).empty());
    EXPECT_EQ(run(key, now, {{100, true}, {400, false}}),
              (std::vector<Timed>{{20, KeyEvent::Press}, {120, KeyEvent::Release}, {370, KeyEvent::Click}}));
}

TEST(UserKey, AKeyHeldAtBootWhileTheTimeIsLongGivesNoLongPress)
{
    UserKey key;
    std::uint64_t now = 0;
    EXPECT_TRUE(run(key, now, {{60'000, true}}).empty());
}

// ---------------------------------------------------------------------------
// The menu over a table of its own, sorted by name as the generator emits it.

struct Values {
    std::int32_t count = 4;
    float gain = 0.5F;
    std::int32_t lock = 0;
    std::int32_t mode = 0;
};
Values values;

constexpr std::array<std::int32_t, 2> lock_values{0, 1};
constexpr std::array<std::int32_t, 3> mode_values{0, 1, -1};
constexpr std::array<std::string_view, 2> lock_names{"OFF", "ON"};
constexpr std::array<std::string_view, 3> mode_names{"SLOW", "FAST", "OFF"};

ParamInfo int_info(std::string_view name, std::int32_t* value, std::int32_t def, std::int32_t min, std::int32_t max)
{
    return {.name = name,
            .hash = vk::core::param_hash(name),
            .type = ParamType::Int32,
            .flags = 0,
            .default_value = ParamValue::from_int(def),
            .min = ParamValue::from_int(min),
            .max = ParamValue::from_int(max),
            .enum_values = {},
            .bitmask = 0,
            .int_value = value,
            .float_value = nullptr};
}

std::array<ParamInfo, 4> make_table()
{
    ParamInfo count = int_info("M_COUNT", &values.count, 4, 0, 100);
    count.flags = vk::core::param_flags::reboot_required;
    const ParamInfo gain{.name = "M_GAIN",
                         .hash = vk::core::param_hash("M_GAIN"),
                         .type = ParamType::Float,
                         .flags = 0,
                         .default_value = ParamValue::from_float(0.5F),
                         .min = ParamValue::from_float(0.0F),
                         .max = ParamValue::from_float(1.0F),
                         .enum_values = {},
                         .bitmask = 0,
                         .int_value = nullptr,
                         .float_value = &values.gain};
    ParamInfo lock = int_info("M_LOCK", &values.lock, 0, 0, 1);
    lock.flags = vk::core::param_flags::locked_when_armed;
    lock.enum_values = lock_values;
    ParamInfo mode = int_info("M_MODE", &values.mode, 0, -1, 1);
    mode.enum_values = mode_values;
    return {count, gain, lock, mode};
}

// Two groups: ALPHA (M_COUNT, M_GAIN) and BETA (M_LOCK, M_MODE).
const std::array<MenuParam, 4> menu_entries{{
    {.index = 0, .step = ParamValue::from_int(3), .value_names = {}, .group = "ALPHA", .unit = "PCS"},
    {.index = 1, .step = ParamValue::from_float(0.1F), .value_names = {}, .group = "ALPHA", .unit = ""},
    {.index = 2, .step = {}, .value_names = lock_names, .group = "BETA", .unit = ""},
    {.index = 3, .step = {}, .value_names = mode_names, .group = "BETA", .unit = ""},
}};

void publish_armed(bool armed)
{
    vk::msg::ArmingState state{};
    state.armed = armed;
    vk::core::Publication<vk::msg::ArmingState>().publish(state);
}

class LocalMenuTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        values = Values{};
        publish_armed(false);
    }

    void TearDown() override { publish_armed(false); }

    void key(KeyEvent event, bool armed = false)
    {
        now += 100'000;
        menu.on_key(event, now, armed);
        menu.tick(now, armed);
    }
    void click(int times = 1)
    {
        for (int i = 0; i < times; ++i) {
            key(KeyEvent::Click);
        }
    }
    void double_click() { key(KeyEvent::Double); }
    void hold() { key(KeyEvent::Long); }

    // Opens the menu on the group list.
    void open() { double_click(); }
    // Opens the menu and edits the parameter at entry `position`: M_COUNT, M_GAIN, M_LOCK, M_MODE.
    void edit(std::size_t position)
    {
        open();
        click(position / 2 == 0 ? 0 : 1);
        double_click();
        click(static_cast<int>(position % 2));
        double_click();
        ASSERT_EQ(menu.state(), MenuState::Edit);
        ASSERT_EQ(menu.position(), position);
    }
    // Moves to `action` and runs it.
    void run_action(MenuAction action)
    {
        while (menu.action() != action) {
            click();
        }
        double_click();
    }

    std::array<ParamInfo, 4> table = make_table();
    vk::core::Parameters parameters{table};
    LocalMenu menu{parameters, menu_entries};
    std::uint64_t now = 0;
};

TEST_F(LocalMenuTest, ClosedAClickSwitchesPagesAndALongPressReturnsToTheFirst)
{
    EXPECT_EQ(menu.page(), Page::Overview);
    for (const Page page : {Page::Attitude, Page::Motion, Page::ArmingChecks, Page::Sensors, Page::Links,
                            Page::RemoteControl, Page::System, Page::Overview}) {
        if (menu.page() == Page::ArmingChecks) {
            // The arming checks page scrolls first (the next test), then a click moves on.
            while (vk::local_display::arming_checks_can_scroll(menu.arming_checks_scroll())) {
                click();
            }
        }
        click();
        EXPECT_EQ(menu.page(), page);
    }
    click();
    hold();
    EXPECT_EQ(menu.page(), Page::Overview);
    EXPECT_EQ(menu.state(), MenuState::Closed);
}

TEST_F(LocalMenuTest, AClickOnTheArmingChecksScrollsAPageAtATimeThenMovesOn)
{
    click(3);
    ASSERT_EQ(menu.page(), Page::ArmingChecks);
    EXPECT_EQ(menu.arming_checks_scroll(), 0U);
    std::size_t scroll = 0;
    while (vk::local_display::arming_checks_can_scroll(scroll)) {
        scroll = vk::local_display::arming_checks_scrolled(scroll);
        click();
        EXPECT_EQ(menu.page(), Page::ArmingChecks);
        EXPECT_EQ(menu.arming_checks_scroll(), scroll);
    }
    EXPECT_EQ(scroll, vk::local_display::arming_checks_max_scroll());
    click();
    EXPECT_EQ(menu.page(), Page::Sensors);
    EXPECT_EQ(menu.arming_checks_scroll(), 0U);
}

TEST_F(LocalMenuTest, ALongPressOnAScrolledArmingChecksPageReturnsToTheOverviewAndTheFirstLine)
{
    click(4);  // the arming checks page, one page down
    hold();
    EXPECT_EQ(menu.page(), Page::Overview);
    click(3);
    EXPECT_EQ(menu.arming_checks_scroll(), 0U);
}

TEST_F(LocalMenuTest, ADoubleClickOnAPageOpensTheGroupList)
{
    click();
    double_click();
    EXPECT_EQ(menu.state(), MenuState::Groups);
    EXPECT_EQ(menu.item(), LocalMenu::no_item);
    EXPECT_EQ(menu.group(), 0U);
    EXPECT_EQ(menu.group_count(), 2U);
    EXPECT_EQ(menu.group_name(0), "ALPHA");
    EXPECT_EQ(menu.group_name(1), "BETA");
    EXPECT_EQ(menu.group_size(1), 2U);
    EXPECT_EQ(menu.group_first(1), 2U);
}

TEST_F(LocalMenuTest, WhileArmedAClickChangesPageADoubleClickOnlyShowsANoticeAndALongPressReturnsToTheOverview)
{
    key(KeyEvent::Click, true);
    EXPECT_EQ(menu.page(), Page::Attitude);
    key(KeyEvent::Double, true);
    EXPECT_EQ(menu.state(), MenuState::Closed);
    EXPECT_TRUE(menu.disarm_notice(now));
    EXPECT_TRUE(menu.disarm_notice(now + LocalMenu::notice_us - 1));
    EXPECT_FALSE(menu.disarm_notice(now + LocalMenu::notice_us));
    EXPECT_EQ(menu.page(), Page::Attitude);
    key(KeyEvent::Long, true);
    EXPECT_EQ(menu.page(), Page::Overview);
    EXPECT_EQ(menu.state(), MenuState::Closed);
}

TEST_F(LocalMenuTest, ThePressAndTheReleaseShowAtOnce)
{
    EXPECT_FALSE(menu.key_down());
    key(KeyEvent::Press);
    EXPECT_TRUE(menu.key_down());
    key(KeyEvent::Release);
    EXPECT_FALSE(menu.key_down());
    key(KeyEvent::Double);
    EXPECT_TRUE(menu.key_down());  // the key is still down after a double click
    key(KeyEvent::Release);
    EXPECT_FALSE(menu.key_down());
    EXPECT_EQ(menu.state(), MenuState::Groups);
}

TEST_F(LocalMenuTest, TheGroupsWrapAndALongPressClosesBackToThePageItCameFrom)
{
    click(2);  // the motion page
    open();
    EXPECT_EQ(menu.group(), 0U);
    click();
    EXPECT_EQ(menu.group(), 1U);
    click();
    EXPECT_EQ(menu.group(), 0U);  // wraps
    hold();
    EXPECT_EQ(menu.state(), MenuState::Closed);
    EXPECT_EQ(menu.page(), Page::Motion);
}

TEST_F(LocalMenuTest, AGroupListsItsParametersWrapsAndALongPressGoesBackToTheGroups)
{
    open();
    click();  // BETA
    double_click();
    EXPECT_EQ(menu.state(), MenuState::List);
    EXPECT_EQ(menu.position(), 2U);
    EXPECT_EQ(menu.item(), 2);
    click();
    EXPECT_EQ(menu.position(), 3U);
    click();
    EXPECT_EQ(menu.position(), 2U);  // wraps inside the group, not into ALPHA
    hold();
    EXPECT_EQ(menu.state(), MenuState::Groups);
    EXPECT_EQ(menu.group(), 1U);
    EXPECT_EQ(menu.item(), LocalMenu::no_item);
}

TEST_F(LocalMenuTest, AnEditShowsTheActionsOfANumberAndAClickMovesThroughThem)
{
    edit(0);
    EXPECT_EQ(menu.action(), MenuAction::Increase);
    click();
    EXPECT_EQ(menu.action(), MenuAction::Decrease);
    click();
    EXPECT_EQ(menu.action(), MenuAction::Times10);
    click();
    EXPECT_EQ(menu.action(), MenuAction::Default);
    click();
    EXPECT_EQ(menu.action(), MenuAction::Save);
    click();
    EXPECT_EQ(menu.action(), MenuAction::Increase);  // wraps
    EXPECT_TRUE(menu.offers(MenuAction::Times10));
}

TEST_F(LocalMenuTest, AnEnumHasNextPrevDefAndSaveOnly)
{
    edit(3);
    EXPECT_FALSE(menu.offers(MenuAction::Times10));
    click();
    EXPECT_EQ(menu.action(), MenuAction::Decrease);
    click();
    EXPECT_EQ(menu.action(), MenuAction::Default);  // x10 skipped
    click();
    EXPECT_EQ(menu.action(), MenuAction::Save);
    click();
    EXPECT_EQ(menu.action(), MenuAction::Increase);
}

TEST_F(LocalMenuTest, ANumberStepsOnItsGridWithinTheRangeAndSaves)
{
    edit(1);  // M_GAIN, 0.5, step 0.1
    double_click();
    double_click();
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 0.7F);
    EXPECT_FLOAT_EQ(values.gain, 0.5F);  // nothing written yet
    for (int i = 0; i < 5; ++i) {
        double_click();
    }
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 1.0F);  // stops at the maximum
    run_action(MenuAction::Decrease);
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 0.9F);
    run_action(MenuAction::Save);
    EXPECT_FLOAT_EQ(values.gain, 0.9F);
    EXPECT_EQ(menu.last_result(), std::optional{ParamResult::Ok});
    EXPECT_EQ(menu.last_parameter(), 1);
}

TEST_F(LocalMenuTest, Times10StepsByTenMenuStepsAndSwitchesBack)
{
    edit(0);  // M_COUNT, 4, step 3, 0 to 100
    run_action(MenuAction::Times10);
    EXPECT_TRUE(menu.times_ten());
    run_action(MenuAction::Increase);
    EXPECT_EQ(menu.staged().as_int(), 34);
    double_click();
    EXPECT_EQ(menu.staged().as_int(), 64);
    run_action(MenuAction::Times10);
    EXPECT_FALSE(menu.times_ten());
    run_action(MenuAction::Increase);
    EXPECT_EQ(menu.staged().as_int(), 67);
    run_action(MenuAction::Times10);
    run_action(MenuAction::Increase);
    run_action(MenuAction::Increase);
    EXPECT_EQ(menu.staged().as_int(), 100);  // clamped
    run_action(MenuAction::Decrease);
    EXPECT_EQ(menu.staged().as_int(), 70);
    run_action(MenuAction::Save);
    EXPECT_EQ(values.count, 70);
}

TEST_F(LocalMenuTest, Times10OnANumberWithDecimalsStaysOnTheGrid)
{
    edit(1);
    run_action(MenuAction::Times10);
    run_action(MenuAction::Increase);
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 1.0F);  // 0.5 + 10 * 0.1 is past the maximum
    run_action(MenuAction::Decrease);
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 0.0F);
}

TEST_F(LocalMenuTest, EachEditStartsWithTheStepOfOne)
{
    edit(0);
    run_action(MenuAction::Times10);
    hold();  // back to the list
    double_click();
    EXPECT_FALSE(menu.times_ten());
}

TEST_F(LocalMenuTest, DefaultStagesTheDefaultAndSaveWritesIt)
{
    values.gain = 0.9F;
    edit(1);
    run_action(MenuAction::Default);
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 0.5F);
    EXPECT_FLOAT_EQ(values.gain, 0.9F);
    run_action(MenuAction::Save);
    EXPECT_FLOAT_EQ(values.gain, 0.5F);
}

TEST_F(LocalMenuTest, ALongPressInAnEditDiscardsTheStagedValueAndGoesBackToTheList)
{
    values.gain = 0.9F;
    edit(1);
    run_action(MenuAction::Decrease);
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 0.8F);
    hold();
    EXPECT_EQ(menu.state(), MenuState::List);
    EXPECT_EQ(menu.position(), 1U);
    EXPECT_FLOAT_EQ(values.gain, 0.9F);
    EXPECT_FALSE(menu.last_result());
    double_click();  // the edit starts from the value now, not the discarded one
    EXPECT_FLOAT_EQ(menu.staged().as_float(), 0.9F);
}

TEST_F(LocalMenuTest, AnEnumStepsThroughItsValuesAndStopsAtTheEnds)
{
    edit(3);  // M_MODE: SLOW, FAST, OFF
    double_click();
    EXPECT_EQ(menu.staged().as_int(), 1);
    double_click();
    EXPECT_EQ(menu.staged().as_int(), -1);
    double_click();
    EXPECT_EQ(menu.staged().as_int(), -1);
    run_action(MenuAction::Decrease);
    EXPECT_EQ(menu.staged().as_int(), 1);
    run_action(MenuAction::Increase);
    run_action(MenuAction::Save);
    EXPECT_EQ(values.mode, -1);
}

TEST_F(LocalMenuTest, SaveShowsTheResultForAboutASecondThenReturnsToTheList)
{
    edit(1);
    double_click();
    run_action(MenuAction::Save);
    EXPECT_EQ(menu.state(), MenuState::Edit);
    EXPECT_TRUE(menu.result_showing());
    EXPECT_EQ(menu.last_result(), std::optional{ParamResult::Ok});
    menu.tick(now + LocalMenu::result_show_us - 1, false);
    EXPECT_EQ(menu.state(), MenuState::Edit);
    menu.tick(now + LocalMenu::result_show_us, false);
    EXPECT_EQ(menu.state(), MenuState::List);
    EXPECT_FALSE(menu.result_showing());
    EXPECT_EQ(menu.last_result(), std::optional{ParamResult::Ok});  // until the next key press
    click();
    EXPECT_FALSE(menu.last_result());
}

TEST_F(LocalMenuTest, AGestureWhileTheResultShowsEndsItAtOnceWithoutRunningAnything)
{
    edit(1);
    run_action(MenuAction::Save);
    ASSERT_TRUE(menu.result_showing());
    double_click();
    EXPECT_EQ(menu.state(), MenuState::List);
    EXPECT_FLOAT_EQ(values.gain, 0.5F);
    EXPECT_EQ(menu.position(), 1U);
}

TEST_F(LocalMenuTest, ARefusedSaveShowsItsReason)
{
    // The parameter service sees the vehicle armed, as in the race between an
    // arm request and a double click.
    edit(2);
    double_click();
    publish_armed(true);
    run_action(MenuAction::Save);
    EXPECT_EQ(menu.last_result(), std::optional{ParamResult::LockedWhileArmed});
    EXPECT_EQ(values.lock, 0);
    EXPECT_TRUE(menu.result_showing());
}

TEST_F(LocalMenuTest, ThirtySecondsWithoutAKeyCloseTheMenuAndDiscardTheEdit)
{
    edit(1);
    run_action(MenuAction::Increase);
    menu.tick(now + LocalMenu::idle_close_us - 1, false);
    EXPECT_EQ(menu.state(), MenuState::Edit);
    menu.tick(now + LocalMenu::idle_close_us, false);
    EXPECT_EQ(menu.state(), MenuState::Closed);
    EXPECT_FLOAT_EQ(values.gain, 0.5F);
}

TEST_F(LocalMenuTest, AKeyPressRestartsTheIdleTime)
{
    open();
    key(KeyEvent::Press);
    menu.tick(now + LocalMenu::idle_close_us - 1, false);
    EXPECT_EQ(menu.state(), MenuState::Groups);
    menu.tick(now + LocalMenu::idle_close_us, false);
    EXPECT_EQ(menu.state(), MenuState::Closed);
}

TEST_F(LocalMenuTest, ArmingClosesTheMenuAtOnce)
{
    edit(1);
    menu.tick(now + 1, true);
    EXPECT_EQ(menu.state(), MenuState::Closed);
    key(KeyEvent::Double, true);  // and the menu does not open again while armed
    EXPECT_EQ(menu.state(), MenuState::Closed);
}

TEST_F(LocalMenuTest, AChangedRebootRequiredParameterIsMarked)
{
    EXPECT_FALSE(menu.reboot_required());
    edit(0);
    run_action(MenuAction::Increase);
    run_action(MenuAction::Save);
    EXPECT_TRUE(menu.reboot_required());
    EXPECT_TRUE(menu.needs_reboot(0));
    EXPECT_FALSE(menu.needs_reboot(1));
}

TEST(LocalMenu, WithoutMenuParametersADoubleClickOnlyShowsTheNotice)
{
    std::array<ParamInfo, 1> table{int_info("E_ONE", &values.count, 0, 0, 1)};
    vk::core::Parameters parameters{table};
    LocalMenu empty{parameters, {}};
    empty.on_key(KeyEvent::Double, 1'000'000, false);
    EXPECT_EQ(empty.state(), MenuState::Closed);
    EXPECT_EQ(empty.group_count(), 0U);
}

TEST(LocalMenuFormat, NumbersShowTheDecimalsOfTheirStep)
{
    std::int32_t integer = -12;
    float number = 0.05F;
    const ParamInfo int_param = int_info("F_INT", &integer, 0, -100, 100);
    ParamInfo float_param = int_param;
    float_param.type = ParamType::Float;
    const MenuParam hundredths{.index = 0, .step = ParamValue::from_float(0.01F), .value_names = {}, .group = {}, .unit = {}};
    const MenuParam tenths{.index = 0, .step = ParamValue::from_float(0.1F), .value_names = {}, .group = {}, .unit = {}};
    EXPECT_EQ(vk::local_display::format_value(int_param, hundredths, ParamValue::from_int(integer)).view(), "-12");
    EXPECT_EQ(vk::local_display::format_value(float_param, hundredths, ParamValue::from_float(number)).view(), "0.05");
    EXPECT_EQ(vk::local_display::format_value(float_param, tenths, ParamValue::from_float(1.0F)).view(), "1.0");
    EXPECT_EQ(vk::local_display::format_value(float_param, tenths, ParamValue::from_float(-2.25F)).view(), "-2.3");
}

// ---------------------------------------------------------------------------
// What the menu screens show, against reference images.

std::string panel_image(const LocalMenu& menu)
{
    vk::sim::Ssd1306 panel;
    vk::drivers::Ssd1306 driver{vk::hal::DisplayDevice(panel)};
    vk::local_display::Canvas canvas(driver.frame());
    canvas.clear();
    vk::local_display::render_menu(menu, canvas);
    // The status bar of a disarmed vehicle in STANDBY with no Link up, on the first page.
    vk::msg::VehicleStatus status{};
    status.source = vk::core::no_source;
    vk::local_display::draw_status_bar(canvas, {.status = status, .links = {}, .manual = std::nullopt, .page = Page::Overview});
    vk::local_display::draw_key_indicator(menu, canvas);
    driver.update(0);
    for (int i = 0; i < 4; ++i) {
        driver.update(vk::drivers::Ssd1306::reset_time_us);
    }
    return panel.pbm();
}

void expect_golden(std::string_view name, const std::string& image)
{
    const std::filesystem::path golden = std::filesystem::path(VK_MENU_GOLDEN_DIR) / (std::string(name) + ".pbm");
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

TEST_F(LocalMenuTest, TheGroupListMatchesItsImage)
{
    open();
    click();
    expect_golden("groups", panel_image(menu));
}

TEST_F(LocalMenuTest, ANumbersParameterListMatchesItsImage)
{
    values.count = 7;
    open();
    double_click();
    click();
    expect_golden("list_numbers", panel_image(menu));
}

TEST_F(LocalMenuTest, AnEnumsParameterListWithAKeyDownMatchesItsImage)
{
    open();
    click();
    double_click();
    key(KeyEvent::Press);
    expect_golden("list_enums_key_down", panel_image(menu));
}

TEST_F(LocalMenuTest, ANumbersEditScreenMatchesItsImage)
{
    edit(0);
    run_action(MenuAction::Times10);
    run_action(MenuAction::Increase);
    click();
    expect_golden("edit_number", panel_image(menu));
}

TEST_F(LocalMenuTest, AnEnumsEditScreenMatchesItsImage)
{
    edit(3);
    double_click();
    click(2);
    expect_golden("edit_enum", panel_image(menu));
}

TEST_F(LocalMenuTest, ASavedEditWithARebootRequiredMatchesItsImage)
{
    edit(0);
    run_action(MenuAction::Increase);
    run_action(MenuAction::Save);
    expect_golden("edit_saved_reboot", panel_image(menu));
}

TEST_F(LocalMenuTest, ARefusedEditMatchesItsImage)
{
    edit(2);
    double_click();
    publish_armed(true);
    run_action(MenuAction::Save);
    expect_golden("edit_refused", panel_image(menu));
}

TEST_F(LocalMenuTest, TheListAfterASaveShowsTheResultAndTheRebootMark)
{
    edit(0);
    run_action(MenuAction::Increase);
    run_action(MenuAction::Save);
    menu.tick(now + LocalMenu::result_show_us, false);
    ASSERT_EQ(menu.state(), MenuState::List);
    expect_golden("list_after_save", panel_image(menu));
}

}  // namespace
