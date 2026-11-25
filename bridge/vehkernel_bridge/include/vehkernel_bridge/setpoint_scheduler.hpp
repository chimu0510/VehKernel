// SPDX-License-Identifier: GPL-3.0-or-later
// When the Bridge sends zero setpoints (ADR-0005, ADR-0027, spec 0015): while the
// vehicle is armed in COMPANION with this Link in control and no /cmd_vel has
// arrived for `cmd_vel_timeout`, one every `period`. ROS2 falling silent then
// stops the vehicle and keeps it armed, because COMPANION does not count zero
// setpoints as idle; the Bridge or the Link falling silent still trips the
// setpoint timeout on the MCU. It is a pure state machine over the time it is
// given, free of rclcpp, so a test drives it with a fake clock.
#pragma once

#include <cstdint>
#include <optional>

namespace vk::bridge {

struct SchedulerConfig {
    std::uint64_t cmd_vel_timeout_us = 250'000;
    // 10 Hz, as the spec says.
    std::uint64_t period_us = 100'000;
};

// The MCU stops and disarms when a gap between setpoints of the source in control
// exceeds COM_SP_TIMEOUT. The zero setpoints start `cmd_vel_timeout` after the last
// /cmd_vel and then come one `period` apart, so the first zero setpoint of a silence
// arrives later than the last /cmd_vel by about `cmd_vel_timeout` plus the Link's and
// the poll's delay. One zero setpoint period of margin covers those delays; a
// `cmd_vel_timeout` that leaves less than that against COM_SP_TIMEOUT loses the race
// and a silence from ROS2 disarms the vehicle (seen in SITL at 0.5 s against 0.5 s).
inline constexpr std::uint64_t timeout_margin_us = 100'000;

// Whether `cmd_vel_timeout_us` is well below `com_sp_timeout_s`, in seconds.
[[nodiscard]] constexpr bool cmd_vel_timeout_is_safe(std::uint64_t cmd_vel_timeout_us, float com_sp_timeout_s)
{
    return static_cast<double>(cmd_vel_timeout_us + timeout_margin_us) < static_cast<double>(com_sp_timeout_s) * 1e6;
}

class SetpointScheduler {
public:
    // `configured_source` is this Link's instance as the MCU numbers the source in
    // control, when the operator set it; it overrides what the scheduler learns.
    explicit SetpointScheduler(SchedulerConfig config = {},
                               std::optional<std::uint8_t> configured_source = std::nullopt)
        : config_(config), configured_source_(configured_source)
    {
    }

    // A new connection, or none: forget the status and the learned instance.
    void reset();

    // The MCU accepted this Bridge's own request for COMPANION on this connection: entering
    // a mode makes the requesting source the source in control (ADR-0005), so the
    // `source` of the next vehicle_status that says COMPANION is this Link's instance.
    // A status that arrived before the acknowledgement, or one that still says another
    // mode, teaches nothing.
    void on_companion_accepted();

    // A /cmd_vel arrived and was sent on. One dropped for not being finite does not
    // count: the vehicle is not being driven, so the zero setpoints go on.
    void on_cmd_vel(std::uint64_t now_us);

    // The latest vehicle_status, as it arrived at `now_us`.
    void on_status(std::uint8_t mode, bool armed, std::uint8_t source, std::uint64_t now_us);

    // True when a zero setpoint is due at `now_us`; it then counts as sent.
    [[nodiscard]] bool zero_due(std::uint64_t now_us);

    // Armed, in COMPANION, with this Link in control, by the latest status.
    [[nodiscard]] bool driving() const { return driving_; }

    // This Link's instance: the configured one, else the learned one, empty until known.
    // No zero setpoint is sent while it is empty, so a Bridge that did not take the
    // vehicle into COMPANION itself does not keep a vehicle armed that another host armed.
    [[nodiscard]] std::optional<std::uint8_t> own_source() const
    {
        return configured_source_.has_value() ? configured_source_ : learned_source_;
    }

private:
    SchedulerConfig config_;
    std::optional<std::uint8_t> configured_source_;
    std::optional<std::uint8_t> learned_source_;
    bool awaiting_source_ = false;
    bool driving_ = false;
    // The later of the last /cmd_vel and the moment `driving_` became true: the
    // silence counts from there, so a vehicle armed with nothing yet sent gets its
    // first zero setpoint one timeout after arming.
    std::optional<std::uint64_t> silent_since_;
    std::optional<std::uint64_t> last_zero_us_;
};

}  // namespace vk::bridge
