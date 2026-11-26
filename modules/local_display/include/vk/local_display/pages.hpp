// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's pages (spec 0016, ADR-0022): graphics drawn on a Canvas
// from the latest topics, under a status bar.
//
// Drawing uses neither the bus nor FreeRTOS, so it is tested on the
// development host; the LocalDisplay work item feeds it.
//
// Layout: the status bar takes the top 8 pixel rows; a page uses the rows
// below it, page_top to page_last. The last row of the image and the last
// column stay dark, so the image can move by a pixel (display protection)
// without losing anything.
//
// How a page plugs in (the same for all eight):
//   1. Its header declares a small `<Name>PageInputs` struct that holds a
//      std::optional of each topic the page needs (nothing if never
//      published), plus any setting it reads, and
//      `void draw_<name>_page(Canvas&, const <Name>PageInputs&)`.
//   2. The page draws only on the area below the status bar, on a cleared
//      canvas, and handles missing data by drawing a dash or "NO DATA", never
//      by leaving garbage.
//   3. The shared `Inputs` below holds the latest of every topic any page
//      shows; render() fills the page's own struct from it and calls the
//      page. A page that needs a new topic adds one optional to `Inputs` and
//      one subscription to LocalDisplay, and nothing else changes.
//   4. Pages whose draw function does not exist yet render a placeholder
//      showing the page's name (placeholder_page()).
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/actuator_outputs.hpp>
#include <vk/msg/link_status.hpp>
#include <vk/msg/manual_control_input.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/vehicle_status.hpp>
#include <vk/msg/wheel_encoders.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vk::local_display {

// The pages in the order a click moves through them (spec 0016).
enum class Page : std::uint8_t {
    Overview,
    Attitude,
    Motion,
    ArmingChecks,
    Sensors,
    Links,
    RemoteControl,
    System,
};
inline constexpr std::size_t page_count = 8;

inline constexpr int status_bar_height = 8;
inline constexpr int page_top = status_bar_height;  // the first row a page uses
inline constexpr int page_last = 62;                // the last row a page uses

// The next page in turn, as a click of the user key moves.
Page next_page(Page page);
// The page's name in capitals, as the placeholder shows it.
std::string_view page_name(Page page);

// The wheels of the vehicle's frame (FRAME_TYPE) and the actuator output each
// drives, filled by LocalDisplay from the parameters.
struct WheelFrame {
    std::size_t wheel_count = 0;  // 0 if the frame is not known, 2 or 4
    std::array<std::uint8_t, 4> outputs{1, 2, 3, 4};  // actuator output of each wheel, 1 for output 1
    float max_speed = 0.0F;  // [rad/s] MOT_MAX_SPD, the full scale of a speed bar; 0 if unknown
};

// The latest of each topic any page or the status bar shows; nothing if never published.
struct Inputs {
    std::optional<msg::VehicleStatus> status;
    std::optional<msg::VehicleAttitude> attitude;
    std::optional<msg::VehicleLocalOdometry> odometry;
    std::optional<msg::WheelEncoders> encoders;
    std::optional<msg::ActuatorOutputs> outputs;
    std::array<std::optional<msg::LinkStatus>, msg::LinkStatus::instances> links;
    std::optional<msg::ManualControlInput> manual;  // nothing if the remote control sent nothing lately
    WheelFrame frame;
};

// A cleared canvas with `page` and its status bar drawn from `inputs`.
void render(Page page, const Inputs& inputs, Canvas& canvas);

// The page's name centred in the area below the status bar, for a page that
// has no drawing yet.
void placeholder_page(Canvas& canvas, Page page);

}  // namespace vk::local_display
