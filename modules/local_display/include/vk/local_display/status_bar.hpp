// SPDX-License-Identifier: GPL-3.0-or-later
// The status bar (spec 0016, ADR-0022): the top 8 pixel rows of every page
// and of the menu. From left to right: the mode (`STB`, `TST`, `MAN` or
// `CMP`); the armed icon while armed; the emergency stop icon while engaged;
// an icon for each Link, a rising signal while it is connected, a cross while
// it is not, in inverse video for the one in control; the gamepad icon while
// the remote control is connected, in inverse video while it is in control; a
// warning icon while an arming refusal is pending and a fault icon while a
// fault is recorded; and at the right one dot per page, the current page's
// larger. Every item has its own place, so nothing moves when another
// appears.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/msg/link_status.hpp>
#include <vk/msg/manual_control_input.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace vk::local_display {

struct StatusBarInputs {
    std::optional<msg::VehicleStatus> status;
    std::array<std::optional<msg::LinkStatus>, msg::LinkStatus::instances> links;
    std::optional<msg::ManualControlInput> manual;
    Page page = Page::Overview;
};

void draw_status_bar(Canvas& canvas, const StatusBarInputs& inputs);

// The mode's three letters: STB, TST, MAN or CMP; "???" for a value the schema does not name.
std::string_view mode_tag(std::uint8_t mode);

}  // namespace vk::local_display
