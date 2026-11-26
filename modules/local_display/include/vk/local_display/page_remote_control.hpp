// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's remote control page (spec 0016, ADR-0022, spec 0010):
// two circles with the gamepad's stick positions, a bar for each trigger and
// the names of the buttons pressed; `NOT CONNECTED` and the time since the
// remote control was lost when it is gone.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/manual_control_input.hpp>

#include <cstdint>
#include <optional>

namespace vk::local_display {

// The remote control is lost when nothing arrives for this long (it publishes
// at least 20 times a second while connected, spec 0010).
inline constexpr std::uint64_t remote_control_timeout_us = 1'000'000;

struct RemoteControlPageInputs {
    // The latest manual_control_input ever received. When `connected` is false its timestamp is when the
    // remote control was lost; when it is older than the timeout, that is when it was last heard.
    std::optional<msg::ManualControlInput> input;
    // The time now on the clock of the input's timestamp, for the time since it was lost.
    std::uint64_t now_us = 0;
};

// Draws the page into the 56 pixels below the status bar.
void draw_remote_control_page(Canvas& canvas, const RemoteControlPageInputs& inputs);

}  // namespace vk::local_display
