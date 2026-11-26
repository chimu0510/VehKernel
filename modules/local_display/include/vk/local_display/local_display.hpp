// SPDX-License-Identifier: GPL-3.0-or-later
// The local display as a work item on wq:slow (ADR-0022).
//
// Every 50 ms it draws the current page, or the menu while it is open, under
// the status bar, and lets the SSD1306 driver write every page of the screen
// that changed in the same run, so a key press is on the panel within one run
// and never half drawn. It subscribes only to what the pages and the status
// bar show: vehicle_status, vehicle_attitude, vehicle_local_odometry,
// wheel_encoders, actuator_outputs, link_status and manual_control_input.
// They are taken at most every 200 ms, every 100 ms while the attitude page
// shows, so values change at most 5 Hz, the horizon 10 Hz, and stay
// readable. A run that changes nothing writes nothing; a full screen costs
// about 4.5 ms of busy-waiting on the C50C's GPIO lines, on the lowest-priority queue, so
// it never delays wq:ctrl or a Link. The menu runs on the same work queue,
// so it never changes during a run. It publishes nothing: producing modules
// never know it exists, and a display that is missing changes nothing.
#pragma once

#include <vk/core/bus.hpp>
#include <vk/core/work_queue.hpp>
#include <vk/drivers/ssd1306.hpp>
#include <vk/local_display/display_protection.hpp>
#include <vk/local_display/local_menu.hpp>
#include <vk/local_display/pages.hpp>
#include <vk/msg/actuator_outputs.hpp>
#include <vk/msg/link_status.hpp>
#include <vk/msg/manual_control_input.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/vehicle_status.hpp>
#include <vk/msg/wheel_encoders.hpp>

#include <array>
#include <cstdint>

namespace vk::local_display {

class LocalDisplay final : public core::WorkItem {
public:
    static constexpr std::uint32_t interval_us = 50'000;        // how soon a key press shows
    static constexpr std::uint64_t topic_interval_us = 200'000;  // values change at most 5 Hz (ADR-0022)
    static constexpr std::uint64_t attitude_interval_us = 100'000;  // the attitude page, 10 Hz
    static constexpr std::uint64_t manual_timeout_us = 1'000'000;  // the remote control is lost after this silence

    // `menu`, if any, and `protection`, which the menu task may share, must run on the same work queue.
    LocalDisplay(drivers::Ssd1306& display, const LocalMenu* menu, DisplayProtection& protection,
                 core::WorkQueue& queue);

    // Starts the periodic runs.
    void start();

protected:
    void run() override;

private:
    drivers::Ssd1306& display_;
    const LocalMenu* menu_;
    DisplayProtection& protection_;
    core::Subscription<msg::VehicleStatus> status_{0};
    core::Subscription<msg::VehicleAttitude> attitude_{0};
    core::Subscription<msg::VehicleLocalOdometry> odometry_{0};
    core::Subscription<msg::WheelEncoders> encoders_{0};
    core::Subscription<msg::ActuatorOutputs> outputs_{0};
    core::Subscription<msg::ManualControlInput> manual_{0};
    std::array<core::Subscription<msg::LinkStatus>, msg::LinkStatus::instances> links_{
        core::Subscription<msg::LinkStatus>{0}, core::Subscription<msg::LinkStatus>{1}};
    Inputs inputs_;
    std::uint64_t next_topics_us_ = 0;
    std::uint64_t manual_seen_us_ = 0;
};

}  // namespace vk::local_display
