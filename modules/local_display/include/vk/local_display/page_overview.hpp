// SPDX-License-Identifier: GPL-3.0-or-later
// The overview page (spec 0016, ADR-0022): the mode and the arming state in
// large letters, the source in control, the emergency stop, actuator power,
// the reset cause, the last arming refusal or disarm reason, the heading and
// the forward speed.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/vehicle_status.hpp>

#include <optional>

namespace vk::local_display {

struct OverviewPageInputs {
    std::optional<msg::VehicleStatus> status;
    std::optional<msg::VehicleAttitude> attitude;  // the heading
    std::optional<msg::VehicleLocalOdometry> odometry;  // the forward speed
};

void draw_overview_page(Canvas& canvas, const OverviewPageInputs& inputs);

}  // namespace vk::local_display
