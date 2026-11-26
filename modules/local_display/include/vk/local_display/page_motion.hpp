// SPDX-License-Identifier: GPL-3.0-or-later
// The motion page (spec 0016, ADR-0022): the forward speed in the large font,
// the yaw rate and the north and east position, and one bar pair per wheel of
// the frame: a filled bar for the wheel's speed with a tick at its target, and
// a thin bar for the output duty. Bars grow from a centre line, right for
// forward, to MOT_MAX_SPD at the end of the bar.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/msg/actuator_outputs.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/wheel_encoders.hpp>

#include <optional>

namespace vk::local_display {

struct MotionPageInputs {
    std::optional<msg::VehicleLocalOdometry> odometry;
    std::optional<msg::WheelEncoders> encoders;
    std::optional<msg::ActuatorOutputs> outputs;
    WheelFrame frame;
};

void draw_motion_page(Canvas& canvas, const MotionPageInputs& inputs);

}  // namespace vk::local_display
