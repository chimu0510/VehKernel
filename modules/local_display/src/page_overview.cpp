// SPDX-License-Identifier: GPL-3.0-or-later
// The overview page of spec 0016 and ADR-0022.
#include <vk/local_display/page_overview.hpp>

#include <vk/core/vehicle_control.hpp>
#include <vk/estimation/math/quaternion.hpp>
#include <vk/local_display/font.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/local_display/text_buffer.hpp>
#include <vk/msg/enums.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>

namespace vk::local_display {

namespace {

constexpr int left = 1;
constexpr int mode_y = 9;
constexpr int arming_y = 18;
constexpr int first_line_y = 28;
constexpr int line_pitch = 8;
constexpr std::size_t second_column = 10;  // the second column of a line of text, in characters

// Text in the 5 x 7 font, each pixel two wide: the large letters of the page.
// The large digit font has no letters, so the mode and the arming state are
// drawn this way.
void wide_text(Canvas& canvas, int x, int y, std::string_view text)
{
    for (const char c : text) {
        const Glyph& g = glyph(c);
        for (std::size_t column = 0; column < font_width; ++column) {
            for (std::size_t row = 0; row < font_height; ++row) {
                if (((g.at(column) >> row) & 1U) != 0U) {
                    const int px = x + (static_cast<int>(column) * 2);
                    canvas.pixel(px, y + static_cast<int>(row));
                    canvas.pixel(px + 1, y + static_cast<int>(row));
                }
            }
        }
        x += 12;
    }
}

template <class Enum>
std::string_view short_name(std::uint8_t value)
{
    const std::string_view name = msg::enum_short_name(static_cast<Enum>(value));
    return name.empty() ? "?" : name;
}

void put_line(Canvas& canvas, int line, std::size_t column, std::string_view text)
{
    canvas.text(left + text_width(column), first_line_y + (line * line_pitch), text);
}

std::string_view source_name(std::uint8_t source, TextBuffer& scratch)
{
    if (core::is_link(source)) {
        return scratch.add("LINK").add(source).view();
    }
    return source == core::remote_control ? "RC" : "NONE";
}

// The heading in whole degrees from north, 0 to 359.
std::uint32_t heading_degrees(const msg::VehicleAttitude& attitude)
{
    const float yaw = estimation::Quaternion::from(attitude.q).euler().yaw;
    double degrees = std::fmod(static_cast<double>(yaw) * 180.0 / std::numbers::pi, 360.0);
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    const auto whole = static_cast<std::uint32_t>(std::lround(degrees));
    return whole >= 360 ? 0 : whole;
}

}  // namespace

void draw_overview_page(Canvas& canvas, const OverviewPageInputs& inputs)
{
    if (inputs.status) {
        const msg::VehicleStatus& s = *inputs.status;
        wide_text(canvas, left, mode_y, short_name<msg::Mode>(s.mode));
        wide_text(canvas, left, arming_y, s.armed ? "ARMED" : "DISARMED");

        TextBuffer source;
        put_line(canvas, 0, 0, TextBuffer().add("SRC ").add(source_name(s.source, source)).view());
        put_line(canvas, 0, second_column, TextBuffer().add("ESTOP ").add(s.emergency_stop ? "ON" : "OFF").view());
        put_line(canvas, 1, 0, TextBuffer().add("PWR ").add(short_name<msg::ActuatorPower>(s.actuator_power)).view());
        put_line(canvas, 1, second_column,
                 TextBuffer().add("RST ").add(short_name<msg::ResetCause>(s.reset_cause)).view());
        if (s.arming_refusal != static_cast<std::uint8_t>(msg::ArmingResult::Ok)) {
            put_line(canvas, 2, 0, TextBuffer().add("REFUSED ").add(short_name<msg::ArmingResult>(s.arming_refusal)).view());
        } else {
            put_line(canvas, 2, 0, TextBuffer().add("DISARM ").add(short_name<msg::DisarmReason>(s.disarm_reason)).view());
        }
    } else {
        wide_text(canvas, left, mode_y, "NO DATA");
    }

    TextBuffer heading;
    heading.add("HDG ");
    if (inputs.attitude) {
        heading.add(heading_degrees(*inputs.attitude));
    } else {
        heading.add("---");
    }
    put_line(canvas, 3, 0, heading.view());
    TextBuffer speed;
    speed.add("V ");
    if (inputs.odometry) {
        speed.add_fixed(inputs.odometry->velocity[0], 2).add(" M/S");
    } else {
        speed.add("--- M/S");
    }
    put_line(canvas, 3, second_column, speed.view());
}

}  // namespace vk::local_display
