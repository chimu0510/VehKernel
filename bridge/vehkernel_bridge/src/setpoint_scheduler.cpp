// SPDX-License-Identifier: GPL-3.0-or-later
// When the Bridge sends zero setpoints (ADR-0005, spec 0015).
#include <vehkernel_bridge/setpoint_scheduler.hpp>

#include <vk/msg/enums.hpp>

namespace vk::bridge {

namespace {
constexpr std::uint8_t no_source = 255;  // vehicle_status.source for none
}  // namespace

void SetpointScheduler::reset()
{
    driving_ = false;
    learned_source_.reset();
    awaiting_source_ = false;
    silent_since_.reset();
    last_zero_us_.reset();
}

void SetpointScheduler::on_companion_accepted()
{
    awaiting_source_ = true;
}

void SetpointScheduler::on_cmd_vel(std::uint64_t now_us)
{
    last_zero_us_.reset();
    if (driving_) {
        silent_since_ = now_us;
    }
}

void SetpointScheduler::on_status(std::uint8_t mode, bool armed, std::uint8_t source, std::uint64_t now_us)
{
    const bool companion = mode == static_cast<std::uint8_t>(vk::msg::Mode::Companion);
    if (awaiting_source_ && companion && source != no_source) {
        learned_source_ = source;
        awaiting_source_ = false;
    }
    const std::optional<std::uint8_t> own = own_source();
    const bool ours = own.has_value() && *own == source;
    const bool now_driving = armed && companion && ours;
    if (now_driving && !driving_) {
        // The silence counts from now, which is after any /cmd_vel that came while the
        // vehicle was not driving.
        silent_since_ = now_us;
        last_zero_us_.reset();
    }
    if (!now_driving) {
        silent_since_.reset();
        last_zero_us_.reset();
    }
    driving_ = now_driving;
}

bool SetpointScheduler::zero_due(std::uint64_t now_us)
{
    if (!driving_ || !silent_since_.has_value()) {
        return false;
    }
    if (now_us < *silent_since_ + config_.cmd_vel_timeout_us) {
        return false;
    }
    if (last_zero_us_.has_value() && now_us < *last_zero_us_ + config_.period_us) {
        return false;
    }
    last_zero_us_ = now_us;
    return true;
}

}  // namespace vk::bridge
