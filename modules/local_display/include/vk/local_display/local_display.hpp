// SPDX-License-Identifier: GPL-3.0-or-later
// The local display as a work item on wq:slow (ADR-0022).
//
// Every 50 ms it draws the current page, or the menu while it is open, under
// the status bar, and lets the SSD1306 driver write every page of the screen
// that changed in the same run, so a key press is on the panel within one run
// and never half drawn. It subscribes only to what the pages and the status
// bar show: vehicle_status, vehicle_attitude, vehicle_local_odometry,
// wheel_encoders, actuator_outputs, link_status, manual_control_input,
// arming_checks, imu_sample (only to measure its rate), magnetometer_status
// and magnetometer_sample, firmware_info, parameter_status and the 16
// instances of work_item_status. All are read on the 200 ms tick, a few
// copies of small messages, so a run stays far below its budget.
// They are taken at most every 200 ms, every 100 ms while the attitude page
// shows, so values change at most 5 Hz, the horizon 10 Hz, and stay
// readable. The frame is redrawn only on that tick or when the menu (its page,
// scroll, screen, key state) or the "disarm first" notice changed, and at most
// four SSD1306 pages are sent per run; a run that changes nothing draws and
// writes nothing. A full screen costs
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
#include <vk/local_display/page_remote_control.hpp>
#include <vk/local_display/page_sensors.hpp>
#include <vk/msg/actuator_outputs.hpp>
#include <vk/msg/arming_checks.hpp>
#include <vk/msg/firmware_info.hpp>
#include <vk/msg/imu_sample.hpp>
#include <vk/msg/link_status.hpp>
#include <vk/msg/magnetometer_sample.hpp>
#include <vk/msg/magnetometer_status.hpp>
#include <vk/msg/manual_control_input.hpp>
#include <vk/msg/parameter_status.hpp>
#include <vk/msg/vehicle_attitude.hpp>
#include <vk/msg/vehicle_local_odometry.hpp>
#include <vk/msg/vehicle_status.hpp>
#include <vk/msg/wheel_encoders.hpp>
#include <vk/msg/work_item_status.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace vk::local_display {

namespace detail {

// One subscription to each instance of work_item_status.
template <std::size_t... Instance>
std::array<core::Subscription<msg::WorkItemStatus>, sizeof...(Instance)> work_item_subscriptions(
    std::index_sequence<Instance...> /*instances*/)
{
    return {core::Subscription<msg::WorkItemStatus>{Instance}...};
}

}  // namespace detail

// What the pages show that no topic carries: the board's name, and the port of each Link.
struct Identity {
    std::string_view board_name;
    std::array<std::string_view, msg::LinkStatus::instances> port_names{"SERIAL1", "SERIAL2"};
};

class LocalDisplay final : public core::WorkItem {
public:
    static constexpr std::uint32_t interval_us = 50'000;        // how soon a key press shows
    static constexpr std::uint64_t topic_interval_us = 200'000;  // values change at most 5 Hz (ADR-0022)
    static constexpr std::uint64_t attitude_interval_us = 100'000;  // the attitude page, 10 Hz
    static constexpr std::uint64_t manual_timeout_us = remote_control_timeout_us;  // the remote control is lost after this silence
    static constexpr std::uint64_t magnetometer_timeout_us = 1'000'000;  // a magnetometer sample older than this is not shown
    static constexpr std::size_t imu_reads_per_tick = 8;  // imu_sample's queue is shorter
    // SSD1306 pages sent per run: half the screen, so a run's busy-waiting on the
    // software bus stays well below the 10 ms budget; a full-screen change
    // takes two runs (100 ms), and the first half goes out in the run of the key press.
    static constexpr std::size_t pages_per_run = drivers::MonoFrame::pages / 2;

    // `menu`, if any, and `protection`, which the menu task may share, must run on the same work queue.
    LocalDisplay(drivers::Ssd1306& display, const LocalMenu* menu, DisplayProtection& protection,
                 core::WorkQueue& queue, Identity identity = {});

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
    core::Subscription<msg::ArmingChecks> arming_checks_{0};
    core::Subscription<msg::ImuSample> imu_{0};
    core::Subscription<msg::MagnetometerStatus> magnetometer_status_{0};
    core::Subscription<msg::MagnetometerSample> magnetometer_sample_{0};
    core::Subscription<msg::FirmwareInfo> firmware_{0};
    core::Subscription<msg::ParameterStatus> parameters_{0};
    std::array<core::Subscription<msg::WorkItemStatus>, msg::WorkItemStatus::instances> work_items_ =
        detail::work_item_subscriptions(std::make_index_sequence<msg::WorkItemStatus::instances>{});
    SampleRateMeter imu_rate_;
    std::uint64_t imu_read_ = 0;
    Inputs inputs_;
    std::uint64_t next_topics_us_ = 0;
    std::uint64_t manual_seen_us_ = 0;
    std::uint64_t magnetometer_seen_us_ = 0;
    // What the frame was last drawn from, besides the topic tick: a redraw is due when any differs.
    bool drawn_ = false;
    std::uint32_t drawn_menu_revision_ = 0;
    bool drawn_notice_ = false;
};

}  // namespace vk::local_display
