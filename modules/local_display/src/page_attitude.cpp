// SPDX-License-Identifier: GPL-3.0-or-later
// The attitude page of spec 0016 and ADR-0022.
#include <vk/local_display/page_attitude.hpp>

#include <vk/estimation/math/quaternion.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/local_display/text_buffer.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>

namespace vk::local_display {

namespace {

// The horizon's frame is 56 pixels wide and, with the margin kept dark below
// it, 55 high; its drawing area is one pixel inside the frame.
constexpr int box_x = 1;
constexpr int box_y = page_top;
constexpr int box_w = 56;
constexpr int box_h = page_last - page_top + 1;
constexpr int inner_x = box_x + 1;
constexpr int inner_y = box_y + 1;
constexpr int inner_w = box_w - 2;
constexpr int inner_h = box_h - 2;

constexpr float centre_x = static_cast<float>(inner_x) + (static_cast<float>(inner_w) / 2.0F);
constexpr float centre_y = static_cast<float>(inner_y) + (static_cast<float>(inner_h) / 2.0F);
// The scale is plus and minus 30 degrees from the centre to the edge of the drawing area.
constexpr float scale_degrees = 30.0F;
constexpr float pixels_per_degree = (static_cast<float>(inner_h) / 2.0F) / scale_degrees;

constexpr int panel_x = 60;
constexpr int panel_columns = 11;  // 66 pixels of text
constexpr float rad_to_deg = 180.0F / std::numbers::pi_v<float>;

void draw_horizon(Canvas& canvas, float roll, float pitch)
{
    // The horizon is a line through the point `offset` pixels down the vehicle's
    // vertical from the centre, tilted by the roll: the world turns against
    // the vehicle, so a right roll raises the line's right end. Ground is
    // dotted, the line is solid.
    const float nx = std::sin(roll);
    const float ny = std::cos(roll);
    const float offset = pitch * rad_to_deg * pixels_per_degree;
    for (int y = inner_y; y < inner_y + inner_h; ++y) {
        for (int x = inner_x; x < inner_x + inner_w; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - centre_x;
            const float dy = static_cast<float>(y) + 0.5F - centre_y;
            const float distance = (dx * nx) + (dy * ny) - offset;
            if (std::fabs(distance) < 0.75F || (distance > 0.0F && ((x + y) % 2) == 0)) {
                canvas.pixel(x, y);
            }
        }
    }
}

// The fixed scale on the right of the drawing area: a mark every 10 degrees
// from -30 to 30, the middle one longer.
void draw_scale(Canvas& canvas)
{
    const int right = inner_x + inner_w - 1;
    for (int mark = -3; mark <= 3; ++mark) {
        const int y = static_cast<int>(std::lround(centre_y + (static_cast<float>(mark) * 10.0F * pixels_per_degree)));
        const int length = mark == 0 ? 6 : 3;
        canvas.fill_rect(right - length - 1, y - 1, length + 2, 3, false);
        canvas.hline(right - length + 1, y, length);
    }
}

// The fixed vehicle symbol: two wings and a centre block.
void draw_vehicle_symbol(Canvas& canvas)
{
    const int cx = static_cast<int>(centre_x);
    const int cy = static_cast<int>(centre_y);
    canvas.fill_rect(cx - 12, cy - 2, 24, 5, false);
    canvas.hline(cx - 11, cy, 7);
    canvas.hline(cx + 5, cy, 7);
    canvas.fill_rect(cx - 1, cy - 1, 3, 3);
}

std::string_view estimator_name(std::uint8_t id)
{
    switch (id) {
    case 1:
        return "MAH";
    case 2:
        return "EKF";
    default:
        return "???";
    }
}

void angle_row(Canvas& canvas, int y, std::string_view label, float radians)
{
    TextBuffer value;
    value.add_fixed(radians * rad_to_deg, 1).right_align(panel_columns - label.size());
    canvas.text(panel_x, y, TextBuffer().add(label).add(value.view()).view());
}

// "ACC  42 ---": a label, then the rejected and the fused count, three characters each.
void count_row(Canvas& canvas, int y, std::string_view label, std::uint32_t rejected, std::optional<std::uint32_t> fused)
{
    TextBuffer r;
    r.add_count(rejected).right_align(3);
    TextBuffer f;
    if (fused) {
        f.add_count(*fused);
    } else {
        f.add("---");
    }
    f.right_align(3);
    canvas.text(panel_x, y, TextBuffer().add(label).add(" ").add(r.view()).add(" ").add(f.view()).view());
}

}  // namespace

void draw_attitude_page(Canvas& canvas, const AttitudePageInputs& inputs)
{
    canvas.rect(box_x, box_y, box_w, box_h);
    if (!inputs.attitude) {
        canvas.text(box_x + 5, box_y + (box_h / 2) - 3, "NO DATA");
        angle_row(canvas, 9, "ROLL", std::nanf(""));
        angle_row(canvas, 17, "PITCH", std::nanf(""));
        angle_row(canvas, 25, "YAW", std::nanf(""));
        return;
    }
    const msg::VehicleAttitude& a = *inputs.attitude;
    const auto euler = estimation::Quaternion::from(a.q).euler();
    canvas.set_clip(inner_x, inner_y, inner_w, inner_h);
    draw_horizon(canvas, euler.roll, euler.pitch);
    draw_scale(canvas);
    draw_vehicle_symbol(canvas);
    canvas.clear_clip();

    angle_row(canvas, 9, "ROLL", euler.roll);
    angle_row(canvas, 17, "PITCH", euler.pitch);
    angle_row(canvas, 25, "YAW", euler.yaw);
    canvas.text(panel_x, 36, TextBuffer().add(estimator_name(a.estimator_id)).add(" REJ FUS").view());
    // The accelerometer's fused count is in no topic; only its rejected count is shown.
    count_row(canvas, 44, "ACC", a.accel_rejected, std::nullopt);
    count_row(canvas, 52, "MAG", a.mag_rejected, a.mag_fused);
}

}  // namespace vk::local_display
