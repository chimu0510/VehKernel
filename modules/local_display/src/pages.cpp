// SPDX-License-Identifier: GPL-3.0-or-later
// The page dispatcher of spec 0016 and ADR-0022.
#include <vk/local_display/pages.hpp>

#include <vk/local_display/page_arming_checks.hpp>
#include <vk/local_display/page_attitude.hpp>
#include <vk/local_display/page_links.hpp>
#include <vk/local_display/page_motion.hpp>
#include <vk/local_display/page_overview.hpp>
#include <vk/local_display/page_remote_control.hpp>
#include <vk/local_display/page_sensors.hpp>
#include <vk/local_display/page_system.hpp>
#include <vk/local_display/status_bar.hpp>

#include <cstddef>
#include <string_view>

namespace vk::local_display {

Page next_page(Page page)
{
    return static_cast<Page>((static_cast<std::size_t>(page) + 1) % page_count);
}

void render(Page page, const Inputs& inputs, Canvas& canvas)
{
    canvas.clear();
    switch (page) {
    case Page::Overview:
        draw_overview_page(canvas, {.status = inputs.status, .attitude = inputs.attitude, .odometry = inputs.odometry});
        break;
    case Page::Attitude:
        draw_attitude_page(canvas, {.attitude = inputs.attitude});
        break;
    case Page::Motion:
        draw_motion_page(canvas, {.odometry = inputs.odometry,
                                  .encoders = inputs.encoders,
                                  .outputs = inputs.outputs,
                                  .frame = inputs.frame});
        break;
    case Page::ArmingChecks:
        draw_arming_checks_page(canvas, {.checks = inputs.arming_checks, .scroll = inputs.arming_checks_scroll});
        break;
    case Page::Sensors:
        draw_sensors_page(canvas, {.status = inputs.status,
                                   .imu_rate_hz = inputs.imu_rate_hz,
                                   .magnetometer_status = inputs.magnetometer_status,
                                   .magnetometer_sample = inputs.magnetometer_sample,
                                   .magnetometer_calibration_applied = inputs.magnetometer_calibration_applied});
        break;
    case Page::Links:
        draw_links_page(canvas, {.status = inputs.status, .links = inputs.links, .port_names = inputs.port_names});
        break;
    case Page::RemoteControl:
        draw_remote_control_page(canvas, {.input = inputs.manual_last, .now_us = inputs.uptime_us});
        break;
    case Page::System:
        draw_system_page(canvas, {.firmware = inputs.firmware,
                                  .status = inputs.status,
                                  .parameters = inputs.parameters,
                                  .work_items = inputs.work_items,
                                  .uptime_us = inputs.uptime_us,
                                  .board_name = inputs.board_name});
        break;
    }
    draw_status_bar(canvas, {.status = inputs.status, .links = inputs.links, .manual = inputs.manual, .page = page});
}

}  // namespace vk::local_display
