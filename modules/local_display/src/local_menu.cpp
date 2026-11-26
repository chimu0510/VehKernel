// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/local_menu.hpp>

#include <vk/local_display/icons.hpp>
#include <vk/local_display/page_arming_checks.hpp>
#include <vk/local_display/pages.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace vk::local_display {

namespace {


std::string_view result_name(core::ParamResult result)
{
    switch (result) {
    case core::ParamResult::Ok:
        return "SAVED";
    case core::ParamResult::UnknownName:
        return "UNKNOWN";
    case core::ParamResult::WrongType:
        return "WRONG_TYPE";
    case core::ParamResult::OutOfRange:
        return "OUT_OF_RANGE";
    case core::ParamResult::LockedWhileArmed:
        return "LOCKED";
    }
    return "?";
}

class Text {
public:
    Text& add(std::string_view text)
    {
        for (const char c : text) {
            if (out_.size < out_.text.size()) {
                out_.text.at(out_.size++) = c;
            }
        }
        return *this;
    }

    Text& add(std::uint64_t value)
    {
        std::array<char, 20> digits{};
        std::size_t count = 0;
        do {
            digits.at(count++) = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (count > 0) {
            add(std::string_view(&digits.at(--count), 1));
        }
        return *this;
    }

    [[nodiscard]] const ValueText& result() const { return out_; }

private:
    ValueText out_;
};

// Decimals a step needs, from 0 to 4: 0.1 needs 1, 0.01 needs 2.
unsigned decimals_of(float step)
{
    unsigned decimals = 0;
    auto scaled = static_cast<double>(step);
    while (decimals < 4 && std::fabs(scaled - std::round(scaled)) > 1e-6 * std::fabs(scaled)) {
        scaled *= 10.0;
        ++decimals;
    }
    return decimals;
}

std::ptrdiff_t enum_position(const core::ParamInfo& info, std::int32_t value)
{
    const auto found = std::ranges::find(info.enum_values, value);
    return found == info.enum_values.end() ? -1 : found - info.enum_values.begin();
}

}  // namespace

ValueText format_value(const core::ParamInfo& info, const core::MenuParam& entry, core::ParamValue value)
{
    Text text;
    if (!info.enum_values.empty()) {
        const std::ptrdiff_t position = enum_position(info, value.as_int());
        if (position >= 0 && static_cast<std::size_t>(position) < entry.value_names.size()) {
            return text.add(entry.value_names[static_cast<std::size_t>(position)]).result();
        }
    }
    if (info.type == core::ParamType::Int32) {
        const std::int32_t number = value.as_int();
        if (number < 0) {
            text.add("-");
        }
        return text.add(static_cast<std::uint64_t>(number < 0 ? -static_cast<std::int64_t>(number) : number)).result();
    }
    const unsigned decimals = decimals_of(entry.step.as_float());
    // A table rather than std::pow, which would cost the firmware 3 KB of Flash.
    constexpr std::array<double, 5> scales{1.0, 10.0, 100.0, 1000.0, 10000.0};
    const double scale = scales[decimals];
    const double rounded = std::round(static_cast<double>(value.as_float()) * scale);
    if (!std::isfinite(rounded)) {
        return text.add("---").result();
    }
    if (rounded < 0.0) {
        text.add("-");
    }
    const auto units = static_cast<std::uint64_t>(std::fabs(rounded));
    const auto whole_scale = static_cast<std::uint64_t>(scale);
    text.add(units / whole_scale);
    if (decimals > 0) {
        text.add(".");
        const std::uint64_t fraction = units % whole_scale;
        for (std::uint64_t digit = whole_scale / 10; digit > 0; digit /= 10) {
            text.add((fraction / digit) % 10);
        }
    }
    return text.result();
}

LocalMenu::LocalMenu(core::Parameters& parameters, std::span<const core::MenuParam> menu)
    : parameters_(parameters), menu_(menu.first(std::min(menu.size(), max_entries)))
{
    for (std::size_t i = 0; i < menu_.size(); ++i) {
        boot_values_.at(i) = parameters_.get(menu_[i].index);
        // A group is a run of entries with the same group name.
        if (group_count_ == 0 || groups_.at(group_count_ - 1).name != menu_[i].group) {
            if (group_count_ == groups_.size()) {
                break;  // more groups than the menu can hold: the rest stay out
            }
            groups_.at(group_count_++) = Group{.name = menu_[i].group, .first = i, .size = 0};
        }
        ++groups_.at(group_count_ - 1).size;
    }
}

std::uint16_t LocalMenu::item() const
{
    if ((state_ != MenuState::List && state_ != MenuState::Edit) || position_ >= menu_.size()) {
        return no_item;
    }
    return menu_[position_].index;
}

bool LocalMenu::needs_reboot(std::size_t position) const
{
    if (position >= menu_.size()) {
        return false;
    }
    const core::ParamInfo& info = parameters_.info(menu_[position].index);
    return (info.flags & core::param_flags::reboot_required) != 0 &&
           parameters_.get(menu_[position].index) != boot_values_.at(position);
}

bool LocalMenu::reboot_required() const
{
    for (std::size_t i = 0; i < menu_.size(); ++i) {
        if (needs_reboot(i)) {
            return true;
        }
    }
    return false;
}

bool LocalMenu::offers(MenuAction action) const
{
    if (action != MenuAction::Times10) {
        return true;
    }
    return position_ < menu_.size() && parameters_.info(entry().index).enum_values.empty();
}

void LocalMenu::on_key(KeyEvent event, std::uint64_t now_us, bool armed)
{
    if (event == KeyEvent::None) {
        return;
    }
    last_key_us_ = now_us;
    if (event == KeyEvent::Press || event == KeyEvent::Release) {
        key_down_ = event == KeyEvent::Press;
        return;
    }
    // A Double or a Long still has the key down; a Click came after the release.
    key_down_ = event == KeyEvent::Double || event == KeyEvent::Long;
    if (armed && state_ != MenuState::Closed) {
        close();  // a race between an arm request and a double click
    }
    switch (state_) {
    case MenuState::Closed:
        if (event == KeyEvent::Click) {
            if (page_ == Page::ArmingChecks && arming_checks_can_scroll(arming_checks_scroll_)) {
                arming_checks_scroll_ = arming_checks_scrolled(arming_checks_scroll_);
            } else {
                page_ = next_page(page_);
                arming_checks_scroll_ = 0;
            }
        } else if (event == KeyEvent::Long) {
            page_ = Page::Overview;
            arming_checks_scroll_ = 0;
        } else if (event == KeyEvent::Double) {
            if (armed || group_count_ == 0) {
                notice_until_us_ = now_us + notice_us;
            } else {
                state_ = MenuState::Groups;
                group_ = 0;
                last_result_.reset();
            }
        }
        return;
    case MenuState::Groups:
        groups_key(event);
        return;
    case MenuState::List:
        last_result_.reset();
        list_key(event);
        return;
    case MenuState::Edit:
        edit_key(event, now_us);
        return;
    }
}

void LocalMenu::groups_key(KeyEvent event)
{
    if (event == KeyEvent::Click) {
        group_ = (group_ + 1) % group_count_;
    } else if (event == KeyEvent::Double) {
        state_ = MenuState::List;
        position_ = groups_.at(group_).first;
    } else if (event == KeyEvent::Long) {
        close();
    }
}

void LocalMenu::list_key(KeyEvent event)
{
    const Group& group = groups_.at(group_);
    if (event == KeyEvent::Click) {
        position_ = group.first + ((position_ - group.first + 1) % group.size);
    } else if (event == KeyEvent::Double) {
        state_ = MenuState::Edit;
        action_ = MenuAction::Increase;
        times_ten_ = false;
        result_until_us_ = 0;
        staged_ = parameters_.get(entry().index);
    } else if (event == KeyEvent::Long) {
        state_ = MenuState::Groups;
    }
}

void LocalMenu::next_action()
{
    do {
        action_ = static_cast<MenuAction>((static_cast<std::size_t>(action_) + 1) % (static_cast<std::size_t>(MenuAction::Save) + 1));
    } while (!offers(action_));
}

void LocalMenu::edit_key(KeyEvent event, std::uint64_t now_us)
{
    if (result_showing()) {
        // Any gesture ends the display of the result and returns to the list.
        result_until_us_ = 0;
        state_ = MenuState::List;
        return;
    }
    if (event == KeyEvent::Click) {
        next_action();
    } else if (event == KeyEvent::Double) {
        run_action(now_us);
    } else if (event == KeyEvent::Long) {
        state_ = MenuState::List;  // discards the staged value
    }
}

void LocalMenu::run_action(std::uint64_t now_us)
{
    switch (action_) {
    case MenuAction::Increase:
        step(1);
        return;
    case MenuAction::Decrease:
        step(-1);
        return;
    case MenuAction::Times10:
        times_ten_ = !times_ten_;
        return;
    case MenuAction::Default:
        staged_ = parameters_.info(entry().index).default_value;
        return;
    case MenuAction::Save:
        last_result_ = parameters_.set(entry().index, staged_);
        last_parameter_ = entry().index;
        result_until_us_ = now_us + result_show_us;
        return;
    }
}

void LocalMenu::step(int direction)
{
    const core::ParamInfo& info = parameters_.info(entry().index);
    const int count = times_ten_ ? 10 : 1;
    if (!info.enum_values.empty()) {
        const std::ptrdiff_t last = static_cast<std::ptrdiff_t>(info.enum_values.size()) - 1;
        const std::ptrdiff_t position = std::clamp<std::ptrdiff_t>(
            std::max<std::ptrdiff_t>(enum_position(info, staged_.as_int()), 0) + direction, 0, last);
        staged_ = core::ParamValue::from_int(info.enum_values[static_cast<std::size_t>(position)]);
        return;
    }
    if (info.type == core::ParamType::Int32) {
        const std::int64_t next = static_cast<std::int64_t>(staged_.as_int()) +
                                  (static_cast<std::int64_t>(direction) * count * entry().step.as_int());
        staged_ = core::ParamValue::from_int(static_cast<std::int32_t>(
            std::clamp<std::int64_t>(next, info.min.as_int(), info.max.as_int())));
        return;
    }
    // Numbers stay on the grid of steps from the minimum, so 0.1 + 0.1 shows 0.2.
    const double minimum = info.min.as_float();
    const double size = entry().step.as_float();
    const double steps = std::round((static_cast<double>(staged_.as_float()) - minimum) / size) + (direction * count);
    const double next = std::clamp(minimum + (steps * size), minimum, static_cast<double>(info.max.as_float()));
    staged_ = core::ParamValue::from_float(static_cast<float>(next));
}

void LocalMenu::tick(std::uint64_t now_us, bool armed)
{
    if (state_ == MenuState::Closed) {
        return;
    }
    if (armed || now_us - last_key_us_ >= idle_close_us) {
        close();
        return;
    }
    if (result_showing() && now_us >= result_until_us_) {
        result_until_us_ = 0;
        state_ = MenuState::List;
    }
}

void LocalMenu::close()
{
    state_ = MenuState::Closed;
    group_ = 0;
    position_ = 0;
    action_ = MenuAction::Increase;
    times_ten_ = false;
    result_until_us_ = 0;
}

namespace {

// The screen's layout, in pixels, below the status bar: one pixel of margin on
// the right and at the bottom, so the image can move by a pixel (display
// protection) without losing any of it.
constexpr int left = 1;
constexpr int right = 125;      // the last column of text
constexpr int title_y = page_top + 1;
constexpr int rule_y = title_y + 8;
constexpr int first_row_y = rule_y + 2;
constexpr int row_pitch = 9;    // a row of text and the highlight around it
constexpr int visible_rows = 4;
constexpr int hint_y = 55;
constexpr int box_y = 44;
constexpr int box_height = 10;
constexpr int now_y = 28;  // the edit screen's NOW and NEW lines
constexpr int new_y = 36;

int text_right(Canvas& canvas, int y, std::string_view text, bool on = true)
{
    const int x = right + 1 - text_width(text.size());
    canvas.text(x, y, text, on);
    return x;
}

// A row of the lists: `label` left, `value` right, highlighted when selected.
void draw_row(Canvas& canvas, int y, std::string_view label, std::string_view value, bool selected)
{
    if (selected) {
        canvas.fill_rect(0, y - 1, right + 2, row_pitch);
    }
    canvas.text(left, y, label, !selected);
    text_right(canvas, y, value, !selected);
}

void draw_title(Canvas& canvas, std::string_view title, std::string_view right_text)
{
    canvas.text(left, title_y, title);
    if (!right_text.empty()) {
        text_right(canvas, title_y, right_text);
    }
    canvas.hline(0, rule_y, right + 2);
}

// "n/m" for the title.
ValueText count_text(std::size_t n, std::size_t m)
{
    return Text().add(static_cast<std::uint64_t>(n)).add("/").add(static_cast<std::uint64_t>(m)).result();
}

// The first row of a window of `visible_rows` that shows `cursor` of `count`.
std::size_t window_start(std::size_t cursor, std::size_t count)
{
    const auto rows = static_cast<std::size_t>(visible_rows);
    if (count <= rows || cursor < rows) {
        return 0;
    }
    return std::min(cursor + 1 - rows, count - rows);
}

void draw_groups(const LocalMenu& menu, Canvas& canvas)
{
    draw_title(canvas, "SETTINGS", count_text(menu.group() + 1, menu.group_count()).view());
    const std::size_t first = window_start(menu.group(), menu.group_count());
    for (std::size_t row = 0; row < static_cast<std::size_t>(visible_rows) && first + row < menu.group_count(); ++row) {
        const std::size_t group = first + row;
        const ValueText size = Text().add("(").add(static_cast<std::uint64_t>(menu.group_size(group))).add(")").result();
        draw_row(canvas, first_row_y + (static_cast<int>(row) * row_pitch), menu.group_name(group), size.view(),
                 group == menu.group());
    }
    canvas.text(left, hint_y, "2X:OPEN HOLD:CLOSE");
}

void draw_list(const LocalMenu& menu, Canvas& canvas)
{
    const std::size_t group = menu.group();
    const std::size_t first = menu.group_first(group);
    const std::size_t size = menu.group_size(group);
    draw_title(canvas, menu.group_name(group), count_text(menu.position() - first + 1, size).view());
    const core::Parameters& parameters = menu.parameters();
    const std::size_t start = window_start(menu.position() - first, size);
    for (std::size_t row = 0; row < static_cast<std::size_t>(visible_rows) && start + row < size; ++row) {
        const std::size_t position = first + start + row;
        const core::MenuParam& entry = menu.entries()[position];
        const bool selected = position == menu.position();
        const int y = first_row_y + (static_cast<int>(row) * row_pitch);
        const ValueText value = format_value(parameters.info(entry.index), entry, parameters.get(entry.index));
        draw_row(canvas, y, parameters.info(entry.index).name, value.view(), selected);
        if (menu.needs_reboot(position)) {
            // A reboot is needed for the value to take effect.
            const int x = right + 1 - text_width(value.view().size()) - icon_size - 2;
            canvas.bitmap(x, y, icon(Icon::Warning), !selected);
        }
    }
    if (const std::optional<core::ParamResult> saved = menu.last_result(); saved.has_value()) {
        canvas.text(left, hint_y, result_name(saved.value()));
    } else {
        canvas.text(left, hint_y, "2X:EDIT HOLD:BACK");
    }
}

std::string_view action_label(const LocalMenu& menu, MenuAction action)
{
    const bool enumeration = !menu.parameters().info(menu.entries()[menu.position()].index).enum_values.empty();
    switch (action) {
    case MenuAction::Increase:
        return enumeration ? "NEXT" : "+";
    case MenuAction::Decrease:
        return enumeration ? "PREV" : "-";
    case MenuAction::Times10:
        return "X10";
    case MenuAction::Default:
        return "DEF";
    case MenuAction::Save:
        return "SAVE";
    }
    return "";
}

void draw_edit(const LocalMenu& menu, Canvas& canvas)
{
    const core::MenuParam& entry = menu.entries()[menu.position()];
    const core::Parameters& parameters = menu.parameters();
    const core::ParamInfo& info = parameters.info(entry.index);
    draw_title(canvas, info.name, entry.unit);

    if (info.enum_values.empty()) {
        Text range;
        range.add("RANGE ").add(format_value(info, entry, info.min).view()).add(" TO ").add(format_value(info, entry, info.max).view());
        canvas.text(left, first_row_y, range.result().view());
    } else {
        canvas.text(left, first_row_y, "CHOOSE ONE");
    }
    canvas.text(left, now_y, Text().add("NOW  ").add(format_value(info, entry, parameters.get(entry.index)).view()).result().view());
    canvas.text(left, new_y, Text().add("NEW  ").add(format_value(info, entry, menu.staged()).view()).result().view());
    if (info.enum_values.empty()) {
        text_right(canvas, new_y, menu.times_ten() ? "STEP X10" : "STEP X1");
    }

    const std::optional<core::ParamResult> saved = menu.last_result();
    if (menu.result_showing() && saved.has_value()) {
        // The result of SAVE, in place of the actions.
        canvas.fill_rect(0, box_y, right + 2, box_height);
        const std::string_view result = result_name(saved.value());
        canvas.text(left + 2, box_y + 2,
                    saved.value() == core::ParamResult::Ok ? result : Text().add("REFUSED: ").add(result).result().view(),
                    false);
        if (menu.needs_reboot(menu.position())) {
            canvas.text(left, hint_y, "REBOOT NEEDED");
        }
        return;
    }
    int x = left;
    for (std::size_t i = 0; i <= static_cast<std::size_t>(MenuAction::Save); ++i) {
        const auto action = static_cast<MenuAction>(i);
        if (!menu.offers(action)) {
            continue;
        }
        const std::string_view label = action_label(menu, action);
        const int width = text_width(label.size()) + 4;
        const bool selected = action == menu.action();
        if (selected) {
            canvas.fill_rect(x, box_y, width, box_height);
        } else {
            canvas.rect(x, box_y, width, box_height);
        }
        canvas.text(x + 2, box_y + 2, label, !selected);
        x += width + 3;
    }
    canvas.text(left, hint_y, menu.needs_reboot(menu.position()) ? "REBOOT NEEDED" : "2X:DO HOLD:DISCARD");
}

}  // namespace

void render_menu(const LocalMenu& menu, Canvas& canvas)
{
    switch (menu.state()) {
    case MenuState::Closed:
        return;
    case MenuState::Groups:
        draw_groups(menu, canvas);
        return;
    case MenuState::List:
        draw_list(menu, canvas);
        return;
    case MenuState::Edit:
        draw_edit(menu, canvas);
        return;
    }
}

void draw_key_indicator(const LocalMenu& menu, Canvas& canvas)
{
    if (menu.key_down()) {
        canvas.fill_circle(123, 59, 2);
    }
}

}  // namespace vk::local_display
