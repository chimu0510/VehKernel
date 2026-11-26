// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/local_display.hpp>

#include <vk/local_display/graphics.hpp>
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
    if (now >= next_topics_us_) {
        msg::VehicleStatus status{};
        while (status_.copy(status)) {
            protection_.on_status(now, status);
            inputs_.status = status;
        }
        take(attitude_, inputs_.attitude);
        for (std::size_t instance = 0; instance < links_.size(); ++instance) {
            take(links_.at(instance), inputs_.links.at(instance));
        }
        // Keeps the 200 ms rhythm, unless the queue fell a whole interval behind.
        next_topics_us_ += topic_interval_us;
        if (next_topics_us_ <= now) {
            next_topics_us_ = now + topic_interval_us;
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
        render_menu(*menu_, canvas);
    } else {
        render(menu_ != nullptr ? menu_->page() : Page::Status, inputs_, screen_);
        if (menu_ != nullptr && menu_->disarm_notice(now)) {
            screen_.put(TextScreen::lines - 1, 0, "MENU NEEDS DISARMED  ");
        }
        screen_.draw(display_.frame());
    }
    if (menu_ != nullptr) {
        draw_key_indicator(*menu_, canvas);
    }
    display_.update(now, drivers::MonoFrame::pages);
}

}  // namespace vk::local_display
