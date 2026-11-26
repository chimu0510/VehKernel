// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/page_remote_control.hpp>

#include <vk/local_display/icons.hpp>
#include <vk/local_display/page_text.hpp>
#include <vk/msg/enums.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vk::local_display {

namespace {

// The stick circles: 23 pixels across, in a 24 x 24 box, at the left and right
// edge of the page; the dot never leaves the circle.
constexpr int stick_radius = 11;
constexpr int stick_travel = 9;
constexpr int stick_y = page::top + 1 + stick_radius;
constexpr int left_stick_x = page::left + 1 + stick_radius;
constexpr int right_stick_x = page::right - 1 - stick_radius;

// The trigger bars, between the circles.
constexpr int bar_width = 8;
constexpr int bar_height = 23;
constexpr int bar_y = page::top + 2;
constexpr int left_bar_x = 44;
constexpr int right_bar_x = 76;

// The buttons, in the bit order of gamepad_button, with names short enough
// for every button to fit the two lines under the sticks.
struct ButtonName {
    msg::GamepadButton button;
    std::string_view name;
};
constexpr std::array<ButtonName, 15> button_names{{
    {.button = msg::GamepadButton::DpadUp, .name = "DU"},
    {.button = msg::GamepadButton::DpadDown, .name = "DD"},
    {.button = msg::GamepadButton::DpadLeft, .name = "DL"},
    {.button = msg::GamepadButton::DpadRight, .name = "DR"},
    {.button = msg::GamepadButton::Start, .name = "ST"},
    {.button = msg::GamepadButton::Back, .name = "BK"},
    {.button = msg::GamepadButton::LeftStick, .name = "LS"},
    {.button = msg::GamepadButton::RightStick, .name = "RS"},
    {.button = msg::GamepadButton::LeftShoulder, .name = "LB"},
    {.button = msg::GamepadButton::RightShoulder, .name = "RB"},
    {.button = msg::GamepadButton::Guide, .name = "GD"},
    {.button = msg::GamepadButton::A, .name = "A"},
    {.button = msg::GamepadButton::B, .name = "B"},
    {.button = msg::GamepadButton::X, .name = "X"},
    {.button = msg::GamepadButton::Y, .name = "Y"},
}};
constexpr std::size_t button_first_row = 4;

float clamp_unit(float value, float low)
{
    return std::isfinite(value) ? std::clamp(value, low, 1.0F) : 0.0F;
}

void draw_stick(Canvas& canvas, int cx, float x, float y)
{
    canvas.circle(cx, stick_y, stick_radius);
    canvas.pixel(cx, stick_y);
    const int dx = static_cast<int>(std::lround(clamp_unit(x, -1.0F) * static_cast<float>(stick_travel)));
    const int dy = static_cast<int>(std::lround(clamp_unit(y, -1.0F) * static_cast<float>(stick_travel)));
    // y is forward: up on the display.
    canvas.fill_circle(cx + dx, stick_y - dy, 1);
}

void draw_trigger(Canvas& canvas, int x, std::string_view label, float value)
{
    canvas.rect(x, bar_y, bar_width, bar_height);
    const int inner = bar_height - 2;
    const int fill = static_cast<int>(std::lround(clamp_unit(value, 0.0F) * static_cast<float>(inner)));
    canvas.fill_rect(x + 1, bar_y + 1 + inner - fill, bar_width - 2, fill);
    canvas.text(x + ((bar_width - static_cast<int>(label.size()) * 6) / 2) + 1, bar_y + bar_height + 3, label);
}

void draw_buttons(Canvas& canvas, std::uint16_t buttons)
{
    std::size_t row = button_first_row;
    TextBuffer line;
    std::size_t used = 0;
    for (const auto& entry : button_names) {
        if ((buttons & (1U << static_cast<unsigned>(entry.button))) == 0) {
            continue;
        }
        if (used != 0 && used + 1 + entry.name.size() > page::columns) {
            page::draw_row(canvas, row++, line.view());
            line = {};
            used = 0;
        }
        if (row >= button_first_row + 2) {
            break;
        }
        if (used != 0) {
            line.add(" ");
            ++used;
        }
        line.add(entry.name);
        used += entry.name.size();
    }
    if (used != 0 && row < button_first_row + 2) {
        page::draw_row(canvas, row, line.view());
    }
}

}  // namespace

void draw_remote_control_page(Canvas& canvas, const RemoteControlPageInputs& inputs)
{
    const bool silent = inputs.input && inputs.now_us > inputs.input->timestamp + remote_control_timeout_us;
    if (!inputs.input || !inputs.input->connected || silent) {
        canvas.bitmap(60, 18, icon(Icon::Gamepad));
        constexpr std::string_view message = "NOT CONNECTED";
        canvas.text((128 - text_width(message.size())) / 2, 30, message);
        if (inputs.input) {
            const std::uint64_t lost_us = inputs.now_us > inputs.input->timestamp ? inputs.now_us - inputs.input->timestamp : 0;
            TextBuffer line;
            line.add("LOST ");
            page::add_duration(line, lost_us / 1'000'000);
            canvas.text((128 - text_width(line.view().size())) / 2, 42, line.view());
        }
        return;
    }
    const auto& input = *inputs.input;
    draw_stick(canvas, left_stick_x, input.sticks[0], input.sticks[1]);
    draw_stick(canvas, right_stick_x, input.sticks[2], input.sticks[3]);
    draw_trigger(canvas, left_bar_x, "LT", input.triggers[0]);
    draw_trigger(canvas, right_bar_x, "RT", input.triggers[1]);
    draw_buttons(canvas, input.buttons);
}

}  // namespace vk::local_display
