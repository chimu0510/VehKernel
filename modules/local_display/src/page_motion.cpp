// SPDX-License-Identifier: GPL-3.0-or-later
// The motion page of spec 0016 and ADR-0022.
#include <vk/local_display/page_motion.hpp>

#include <vk/local_display/text_buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>

namespace vk::local_display {

namespace {

constexpr int left = 1;
constexpr int speed_y = 9;
constexpr int unit_y = 26;
constexpr int column_x = 72;
constexpr int column_columns = 9;  // 54 pixels of text
constexpr float rad_to_deg = 180.0F / std::numbers::pi_v<float>;

// One wheel's row of bars is 6 pixels high and the rows are `pitch` apart: a
// tick at the target (5 high), the speed bar (3 high) and the duty bar (1 high).
constexpr int centre_x = 64;
constexpr int half_width = 60;  // the bar's length for full scale
constexpr int row_height = 6;

void column_row(Canvas& canvas, int y, std::string_view label, float value, unsigned decimals)
{
    TextBuffer number;
    number.add_fixed(value, decimals).right_align(column_columns - label.size());
    canvas.text(column_x, y, TextBuffer().add(label).add(number.view()).view());
}

// The pixel offset of `value` against full scale `full`, within +-half_width.
int bar_offset(float value, float full)
{
    const float fraction = std::clamp(value / full, -1.0F, 1.0F);
    return static_cast<int>(std::lround(fraction * static_cast<float>(half_width)));
}

// A bar from the centre line to `offset` pixels, `height` high from row `y`.
void bar(Canvas& canvas, int y, int height, int offset)
{
    if (offset >= 0) {
        canvas.fill_rect(centre_x, y, offset + 1, height);
    } else {
        canvas.fill_rect(centre_x + offset, y, -offset + 1, height);
    }
}

void draw_wheel(Canvas& canvas, int y, std::optional<float> speed, std::optional<float> target,
                std::optional<float> duty, float full)
{
    canvas.vline(centre_x - half_width, y + 1, 3);
    canvas.vline(centre_x + half_width, y + 1, 3);
    canvas.vline(centre_x, y, row_height);
    if (speed && std::isfinite(*speed)) {
        bar(canvas, y + 1, 3, bar_offset(*speed, full));
    }
    if (duty && std::isfinite(*duty)) {
        bar(canvas, y + 5, 1, bar_offset(*duty, 1.0F));
    }
    if (target && std::isfinite(*target)) {
        // The tick is dark over the speed bar and lit elsewhere, so it shows on both.
        const int x = centre_x + bar_offset(*target, full);
        canvas.vline(x, y, 5, !canvas.get(x, y + 2));
        canvas.pixel(x, y);
        canvas.pixel(x, y + 4);
    }
}

void draw_wheels(Canvas& canvas, const MotionPageInputs& inputs)
{
    const std::size_t wheels = inputs.frame.wheel_count;
    if (wheels == 0 || (!inputs.encoders && !inputs.outputs)) {
        canvas.text(left + 30, 46, wheels == 0 ? "NO FRAME" : "NO DATA");
        return;
    }
    const float full = inputs.frame.max_speed > 0.0F ? inputs.frame.max_speed : 1.0F;
    // Four wheels sit 7 pixels apart from y 36; two sit 14 apart from y 40.
    const int first_y = wheels > 2 ? 36 : 40;
    const int pitch = wheels > 2 ? 7 : 14;
    for (std::size_t wheel = 0; wheel < wheels && wheel < inputs.frame.outputs.size(); ++wheel) {
        const std::size_t output = static_cast<std::size_t>(inputs.frame.outputs.at(wheel)) - 1;
        std::optional<float> speed;
        std::optional<float> target;
        std::optional<float> duty;
        if (output < 4) {
            if (inputs.encoders && ((inputs.encoders->valid >> output) & 1U) != 0U) {
                speed = inputs.encoders->speed.at(output);
            }
            if (inputs.outputs) {
                target = inputs.outputs->target.at(output);
                duty = inputs.outputs->output.at(output);
            }
        }
        draw_wheel(canvas, first_y + (static_cast<int>(wheel) * pitch), speed, target, duty, full);
    }
}

}  // namespace

void draw_motion_page(Canvas& canvas, const MotionPageInputs& inputs)
{
    if (inputs.odometry) {
        const msg::VehicleLocalOdometry& o = *inputs.odometry;
        const float forward = o.velocity[0];
        canvas.big_text(left, speed_y, TextBuffer().add_fixed(forward, std::fabs(forward) >= 10.0F ? 1 : 2).view());
        column_row(canvas, 9, "YAW", o.yaw_rate * rad_to_deg, 1);
        column_row(canvas, 17, "N", o.position[0], 1);
        column_row(canvas, 25, "E", o.position[1], 1);
    } else {
        canvas.big_text(left, speed_y, "---");
        column_row(canvas, 9, "YAW", std::nanf(""), 1);
        column_row(canvas, 17, "N", std::nanf(""), 1);
        column_row(canvas, 25, "E", std::nanf(""), 1);
    }
    canvas.text(left, unit_y, "M/S");
    draw_wheels(canvas, inputs);
}

}  // namespace vk::local_display
