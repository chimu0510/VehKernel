// SPDX-License-Identifier: GPL-3.0-or-later
// The attitude page (spec 0016, ADR-0022): an artificial horizon 56 pixels
// wide on the left, roll, pitch and yaw, the estimator and its accelerometer
// and magnetometer rejected and fused counts on the right.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/vehicle_attitude.hpp>

#include <optional>

namespace vk::local_display {

struct AttitudePageInputs {
    std::optional<msg::VehicleAttitude> attitude;
};

void draw_attitude_page(Canvas& canvas, const AttitudePageInputs& inputs);

}  // namespace vk::local_display
