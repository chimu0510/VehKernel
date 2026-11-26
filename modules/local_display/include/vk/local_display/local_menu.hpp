// SPDX-License-Identifier: GPL-3.0-or-later
// The local menu (ADR-0022, spec 0016): the user key's gestures become page
// changes, and while disarmed, edits of the menu parameters.
//
// Closed, a click switches pages, a long press returns to the first page and a
// double click opens the menu if the vehicle is disarmed; armed, it only shows
// that the menu needs the vehicle disarmed. The menu lists groups of menu
// parameters, then the parameters of a group, then a parameter's edit screen.
// A click moves to the next entry or action, wrapping around; a double click
// enters a group, opens an edit screen or runs the action under the caret; a
// long press goes back one level, and at the groups closes the menu back to
// the page it was opened from. An edit stages a value: + and - (or NEXT and
// PREV for an enumeration) step it by the parameter's menu step within the
// range, or through the values, x10 switches the step between one and ten
// steps, DEF stages the default, and SAVE writes the staged value through the
// parameter service, as a Link does, shows the result for a second and returns
// to the parameter list. A long press in an edit discards the staged value.
//
// The menu closes, discarding a staged edit, after 30 s without a key press,
// and at once when the vehicle is armed, which only a race between an arm
// request and a double click can cause, since arming is refused while it is
// open. It never arms, changes the mode or touches the emergency stop. It
// uses neither the bus nor FreeRTOS, so it is tested on the development host.
#pragma once

#include <vk/core/parameters.hpp>
#include <vk/local_display/graphics.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/local_display/user_key.hpp>
#include <vk/msg/enums.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace vk::local_display {

// Declared in schema/enum/menu_state.enum (ADR-0006).
using MenuState = msg::MenuState;

// The actions of an edit screen, in the order a click moves through them. For
// an enumeration Increase is NEXT and Decrease is PREV, and Times10 is left out.
enum class MenuAction : std::uint8_t {
    Increase,
    Decrease,
    Times10,
    Default,
    Save,
};

class LocalMenu {
public:
    static constexpr std::uint64_t idle_close_us = 30'000'000;  // a constant (ADR-0022)
    static constexpr std::uint64_t result_show_us = 1'000'000;  // how long an edit shows SAVED or the refusal
    static constexpr std::uint64_t notice_us = 2'000'000;       // how long "disarm first" shows
    static constexpr std::uint16_t no_item = 0xFFFF;
    static constexpr std::uint8_t no_result = 0xFF;
    static constexpr std::size_t max_groups = 12;

    // `menu` lists the menu parameters of `parameters`' table, ordered by group,
    // so a group's parameters are next to each other.
    LocalMenu(core::Parameters& parameters, std::span<const core::MenuParam> menu);

    // A key event at `now_us`, with the vehicle's arming state.
    void on_key(KeyEvent event, std::uint64_t now_us, bool armed);
    // Every run: closes the menu when idle for too long or when armed, and
    // ends the display of a save's result.
    void tick(std::uint64_t now_us, bool armed);

    [[nodiscard]] MenuState state() const { return state_; }
    [[nodiscard]] Page page() const { return page_; }
    // The first line shown on the arming checks page: a click scrolls it a page at a time, then moves on.
    [[nodiscard]] std::size_t arming_checks_scroll() const { return arming_checks_scroll_; }
    // Whether the key is down, from the last event: a press shows at once.
    [[nodiscard]] bool key_down() const { return key_down_; }
    // Counts every change of what the menu shows: a key event, the menu closing by itself, the result of a Save ending.
    // The display redraws when it differs from the one it last drew.
    [[nodiscard]] std::uint32_t revision() const { return revision_; }
    // Whether "the menu needs the vehicle disarmed" is to be shown at `now_us`.
    [[nodiscard]] bool disarm_notice(std::uint64_t now_us) const { return now_us < notice_until_us_; }

    // The groups, in order; the group under the cursor in the group list, or
    // the one whose parameters are listed.
    [[nodiscard]] std::size_t group_count() const { return group_count_; }
    [[nodiscard]] std::size_t group() const { return group_; }
    [[nodiscard]] std::string_view group_name(std::size_t group) const { return groups_.at(group).name; }
    [[nodiscard]] std::size_t group_size(std::size_t group) const { return groups_.at(group).size; }
    // The first entry of a group, an index into entries().
    [[nodiscard]] std::size_t group_first(std::size_t group) const { return groups_.at(group).first; }

    // The entry under the cursor in the list or edited, an index into entries().
    [[nodiscard]] std::size_t position() const { return position_; }
    // The parameter index of the entry under the cursor, or no_item at the groups or closed.
    [[nodiscard]] std::uint16_t item() const;
    [[nodiscard]] MenuAction action() const { return action_; }
    // Whether the step is ten menu steps (x10 switched on).
    [[nodiscard]] bool times_ten() const { return times_ten_; }
    [[nodiscard]] core::ParamValue staged() const { return staged_; }
    // Whether the edit shows the result of Save now.
    [[nodiscard]] bool result_showing() const { return result_until_us_ != 0; }
    // The result of the last Save and the parameter it wrote, until the next key press.
    [[nodiscard]] std::optional<core::ParamResult> last_result() const { return last_result_; }
    [[nodiscard]] std::uint16_t last_parameter() const { return last_parameter_; }
    // Whether the parameter at entry `position` is reboot_required and differs from its value when the menu was made.
    [[nodiscard]] bool needs_reboot(std::size_t position) const;
    [[nodiscard]] bool reboot_required() const;
    // Whether the action is offered for the entry being edited: Times10 only for a number.
    [[nodiscard]] bool offers(MenuAction action) const;

    [[nodiscard]] std::span<const core::MenuParam> entries() const { return menu_; }
    [[nodiscard]] const core::Parameters& parameters() const { return parameters_; }

private:
    static constexpr std::size_t max_entries = 32;

    struct Group {
        std::string_view name;
        std::size_t first = 0;
        std::size_t size = 0;
    };

    void close();
    void groups_key(KeyEvent event);
    void list_key(KeyEvent event);
    void edit_key(KeyEvent event, std::uint64_t now_us);
    void run_action(std::uint64_t now_us);
    void next_action();
    void step(int direction);
    [[nodiscard]] const core::MenuParam& entry() const { return menu_[position_]; }

    core::Parameters& parameters_;
    std::span<const core::MenuParam> menu_;
    std::array<core::ParamValue, max_entries> boot_values_{};
    std::array<Group, max_groups> groups_{};
    std::size_t group_count_ = 0;
    MenuState state_ = MenuState::Closed;
    Page page_ = Page::Overview;
    std::size_t arming_checks_scroll_ = 0;
    std::size_t group_ = 0;
    std::size_t position_ = 0;
    MenuAction action_ = MenuAction::Increase;
    bool times_ten_ = false;
    bool key_down_ = false;
    std::uint32_t revision_ = 0;
    core::ParamValue staged_{};
    std::optional<core::ParamResult> last_result_;
    std::uint16_t last_parameter_ = no_item;
    std::uint64_t last_key_us_ = 0;
    std::uint64_t result_until_us_ = 0;  // 0: the edit is not showing a result
    std::uint64_t notice_until_us_ = 0;
};

// The menu's screen, drawn on `canvas` over a cleared image, below the status
// bar's rows, which the caller draws (draw_status_bar): the group list, a
// group's parameters or the edit screen.
void render_menu(const LocalMenu& menu, Canvas& canvas);

// A dot at the bottom right corner while the key is down, so a press shows at
// once on every page and screen.
void draw_key_indicator(const LocalMenu& menu, Canvas& canvas);

// A parameter value as the menu shows it: an enum by name, a number with as
// many decimals as its menu step needs.
struct ValueText {
    std::array<char, 24> text{};
    std::size_t size = 0;

    [[nodiscard]] std::string_view view() const { return {text.data(), size}; }
};
ValueText format_value(const core::ParamInfo& info, const core::MenuParam& entry, core::ParamValue value);

}  // namespace vk::local_display
