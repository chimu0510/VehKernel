// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's Links page (spec 0016, ADR-0022, ADR-0002): per Link its
// port name, a handshake icon and its rate scale, then its drops, lost frames
// and frames received. A triangle marks the Link in control, and `CMP` the
// Link of the companion computer in COMPANION mode.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/link_status.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace vk::local_display {

struct LinksPageInputs {
    // The source in control and the mode, from vehicle_status.
    std::optional<msg::VehicleStatus> status;
    std::array<std::optional<msg::LinkStatus>, msg::LinkStatus::instances> links;
    // The port each Link runs on, as the board names it; up to seven characters fit.
    std::array<std::string_view, msg::LinkStatus::instances> port_names{"SERIAL1", "SERIAL2"};
};

// Draws the page into the 56 pixels below the status bar.
void draw_links_page(Canvas& canvas, const LinksPageInputs& inputs);

}  // namespace vk::local_display
