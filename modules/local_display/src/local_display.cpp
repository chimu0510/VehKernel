// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/local_display.hpp>

#include <vk/local_display/graphics.hpp>
#include <vk/local_display/status_bar.hpp>
#include <vk/msg/enums.hpp>
#include <vk/params/params.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace vk::local_display {

namespace {

// Keeps the newest message of a subscription, if one arrived.
template <class T>
void take(core::Subscription<T>& subscription, std::optional<T>& latest)
{
    T message{};
    while (subscription.copy(message)) {
        latest = message;
    }
}

// The frame's wheels from the parameters; no wheels if the vehicle is not wheeled.
WheelFrame wheel_frame()
{
    WheelFrame frame;
#ifdef CONFIG_VEHICLE_MODEL_WHEELED
    frame.wheel_count = params::frame.type == static_cast<std::int32_t>(msg::FrameType::Skid4wd) ? 4 : 2;
    frame.outputs = {static_cast<std::uint8_t>(params::whl.left_out), static_cast<std::uint8_t>(params::whl.right_out),
                     static_cast<std::uint8_t>(params::whl.rear_l_out), static_cast<std::uint8_t>(params::whl.rear_r_out)};
#endif
#ifdef CONFIG_ACTUATOR_CONTROL
    frame.max_speed = params::mot.max_spd;
#endif
    return frame;
}

}  // namespace

LocalDisplay::LocalDisplay(drivers::Ssd1306& display, const LocalMenu* menu, DisplayProtection& protection,
                           core::WorkQueue& queue)
    : core::WorkItem("local_display", queue), display_(display), menu_(menu), protection_(protection)
{
}

void LocalDisplay::start()
{
    schedule_periodic(interval_us);
}

void LocalDisplay::run()
{
    const std::uint64_t now = queue().now_us();
    const Page page = menu_ != nullptr ? menu_->page() : Page::Overview;
    if (now >= next_topics_us_) {
        msg::VehicleStatus status{};
        while (status_.copy(status)) {
            protection_.on_status(now, status);
            inputs_.status = status;
        }
        take(attitude_, inputs_.attitude);
        take(odometry_, inputs_.odometry);
        take(encoders_, inputs_.encoders);
        take(outputs_, inputs_.outputs);
        msg::ManualControlInput manual{};
        while (manual_.copy(manual)) {
            inputs_.manual = manual;
            manual_seen_us_ = now;
        }
        // The remote control publishes at least 20 times a second while it is connected.
        if (inputs_.manual && now - manual_seen_us_ > manual_timeout_us) {
            inputs_.manual.reset();
        }
        inputs_.frame = wheel_frame();
        for (std::size_t instance = 0; instance < links_.size(); ++instance) {
            take(links_.at(instance), inputs_.links.at(instance));
        }
        // Keeps the 200 ms rhythm, unless the queue fell a whole interval behind.
        const std::uint64_t interval = page == Page::Attitude ? attitude_interval_us : topic_interval_us;
        next_topics_us_ += interval;
        if (next_topics_us_ <= now) {
            next_topics_us_ = now + interval;
        }
    }
    const bool menu_open = menu_ != nullptr && menu_->state() != MenuState::Closed;
    const bool armed = inputs_.status && inputs_.status->armed;
    protection_.update(now, armed, menu_open,
                       ProtectionSettings{.bright_percent = params::dsp.bright,
                                          .dim_seconds = params::dsp.dim_s,
                                          .off_seconds = params::dsp.off_s,
                                          .shift = params::dsp.shift != 0});
    display_.set_contrast(protection_.contrast());
    display_.set_display_on(protection_.display_on());
    display_.set_offset(protection_.shift_x(), protection_.shift_y());

    Canvas canvas(display_.frame());
    if (menu_open) {
        canvas.clear();
        render_menu(*menu_, canvas);
        draw_status_bar(canvas, {.status = inputs_.status, .links = inputs_.links, .manual = inputs_.manual, .page = page});
    } else {
        render(page, inputs_, canvas);
        if (menu_ != nullptr && menu_->disarm_notice(now)) {
            // The notice replaces the page's bottom line.
            canvas.fill_rect(0, page_last - 7, static_cast<int>(drivers::MonoFrame::width) - 1, 8, false);
            canvas.text(1, page_last - 7, "MENU NEEDS DISARMED");
        }
    }
    if (menu_ != nullptr) {
        draw_key_indicator(*menu_, canvas);
    }
    display_.update(now, drivers::MonoFrame::pages);
}

}  // namespace vk::local_display
