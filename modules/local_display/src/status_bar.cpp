// SPDX-License-Identifier: GPL-3.0-or-later
// The status bar of spec 0016 and ADR-0022.
#include <vk/local_display/status_bar.hpp>

#include <vk/core/vehicle_control.hpp>
#include <vk/local_display/icons.hpp>
#include <vk/msg/enums.hpp>

#include <cstddef>
#include <cstdint>

namespace vk::local_display {

namespace {

// The items' left edges, in pixels. The image keeps its last column and row dark.
constexpr int mode_x = 1;
constexpr int armed_x = 21;
constexpr int estop_x = 30;
constexpr int link_x = 41;       // the first Link; the next is link_pitch further
constexpr int link_pitch = 9;
constexpr int gamepad_x = 61;
constexpr int refusal_x = 72;
constexpr int fault_x = 81;
constexpr int dots_right = 126;  // the last column of the page dots
constexpr int dot_pitch = 4;

// An icon at `x`, in inverse video when `marked`.
void draw_icon(Canvas& canvas, int x, Icon which, bool marked = false)
{
    if (marked) {
        canvas.fill_rect(x, 0, icon_size, icon_size);
    }
    canvas.bitmap(x, 0, icon(which), !marked);
}

void draw_page_dots(Canvas& canvas, Page current)
{
    const int first = dots_right + 1 - (static_cast<int>(page_count) * dot_pitch) + 1;
    for (std::size_t page = 0; page < page_count; ++page) {
        const int x = first + (static_cast<int>(page) * dot_pitch);
        if (page == static_cast<std::size_t>(current)) {
            canvas.fill_rect(x - 1, 2, 3, 3);
        } else {
            canvas.pixel(x, 3);
        }
    }
}

}  // namespace

std::string_view mode_tag(std::uint8_t mode)
{
    switch (static_cast<msg::Mode>(mode)) {
    case msg::Mode::Standby:
        return "STB";
    case msg::Mode::Test:
        return "TST";
    case msg::Mode::Manual:
        return "MAN";
    case msg::Mode::Companion:
        return "CMP";
    }
    return "???";
}

void draw_status_bar(Canvas& canvas, const StatusBarInputs& inputs)
{
    canvas.fill_rect(0, 0, static_cast<int>(drivers::MonoFrame::width), status_bar_height, false);
    if (inputs.status) {
        const msg::VehicleStatus& s = *inputs.status;
        canvas.text(mode_x, 0, mode_tag(s.mode));
        if (s.armed) {
            draw_icon(canvas, armed_x, Icon::Armed);
        }
        if (s.emergency_stop) {
            draw_icon(canvas, estop_x, Icon::EmergencyStop);
        }
        if (s.arming_refusal != static_cast<std::uint8_t>(msg::ArmingResult::Ok)) {
            draw_icon(canvas, refusal_x, Icon::Warning);
        }
        if (s.fault != static_cast<std::uint8_t>(msg::Fault::None)) {
            draw_icon(canvas, fault_x, Icon::Fault);
        }
    } else {
        canvas.text(mode_x, 0, "---");
    }
    for (std::size_t instance = 0; instance < inputs.links.size(); ++instance) {
        const auto& link = inputs.links.at(instance);
        const bool up = link && link->handshake_state == static_cast<std::uint8_t>(msg::HandshakeState::Connected);
        const bool in_control = inputs.status && inputs.status->source == instance;
        draw_icon(canvas, link_x + (static_cast<int>(instance) * link_pitch), up ? Icon::LinkUp : Icon::LinkDown,
                  in_control);
    }
    if (inputs.manual && inputs.manual->connected) {
        draw_icon(canvas, gamepad_x, Icon::Gamepad,
                  inputs.status && inputs.status->source == core::remote_control);
    }
    draw_page_dots(canvas, inputs.page);
}

}  // namespace vk::local_display
