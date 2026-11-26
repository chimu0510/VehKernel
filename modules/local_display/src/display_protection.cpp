// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/display_protection.hpp>

#include <vk/core/vehicle_control.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace vk::local_display {

namespace {

constexpr std::uint64_t seconds_to_us(int seconds)
{
    return static_cast<std::uint64_t>(seconds) * 1'000'000U;
}

// The cycle of the image offset, one step every two minutes.
constexpr std::array<std::array<int, 2>, 4> shift_cycle{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};

}  // namespace

void DisplayProtection::update(std::uint64_t now_us, bool armed, bool menu_open, const ProtectionSettings& settings)
{
    if (!started_) {
        // The idle time and the shift timer count from the first run.
        started_ = true;
        last_activity_us_ = now_us;
        next_shift_us_ = now_us + shift_interval_us;
    }
    const int bright = std::clamp(settings.bright_percent, 10, 100);
    if (armed || menu_open) {
        // Never dimmed or off while armed; the menu is use, not idle. The idle
        // time restarts when either ends.
        level_ = DisplayLevel::Awake;
        last_activity_us_ = now_us;
    } else {
        const std::uint64_t idle_us = now_us - last_activity_us_;
        if (settings.off_seconds > 0 && idle_us >= seconds_to_us(settings.off_seconds)) {
            level_ = DisplayLevel::Off;
        } else if (settings.dim_seconds > 0 && idle_us >= seconds_to_us(settings.dim_seconds)) {
            level_ = DisplayLevel::Dimmed;
        } else {
            level_ = DisplayLevel::Awake;
        }
    }
    normal_contrast_ = contrast_of(bright);
    contrast_ = normal_contrast_;
    if (level_ == DisplayLevel::Dimmed) {
        contrast_ = std::min(contrast_, contrast_of(dim_percent));
    }

    if (now_us >= next_shift_us_) {
        shift_step_ = (shift_step_ + 1U) % shift_cycle.size();
        // Keeps the two-minute rhythm, unless the caller fell a whole interval behind.
        next_shift_us_ += shift_interval_us;
        if (next_shift_us_ <= now_us) {
            next_shift_us_ = now_us + shift_interval_us;
        }
    }
    if (settings.shift) {
        shift_x_ = shift_cycle.at(shift_step_).at(0);
        shift_y_ = shift_cycle.at(shift_step_).at(1);
    } else {
        shift_x_ = 0;
        shift_y_ = 0;
    }
}

bool DisplayProtection::on_key_press(std::uint64_t now_us)
{
    const bool asleep = level_ != DisplayLevel::Awake;
    wake(now_us);
    return asleep;
}

void DisplayProtection::wake(std::uint64_t now_us)
{
    last_activity_us_ = now_us;
    level_ = DisplayLevel::Awake;
    contrast_ = normal_contrast_;
}

void DisplayProtection::on_status(std::uint64_t now_us, const msg::VehicleStatus& status)
{
    if (status_) {
        const msg::VehicleStatus& before = *status_;
        const bool takeover = status.source == core::remote_control && before.source != core::remote_control;
        const bool fault = status.fault != 0 && status.fault != before.fault;
        const bool refusal = status.arming_refusal != 0 && status.arming_refusal != before.arming_refusal;
        const bool estop = status.emergency_stop && !before.emergency_stop;
        if (status.armed != before.armed || status.mode != before.mode || estop || fault || refusal || takeover) {
            wake(now_us);
        }
    }
    status_ = status;
}

}  // namespace vk::local_display
