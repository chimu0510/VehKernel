// SPDX-License-Identifier: GPL-3.0-or-later
// Reads float parameters from the MCU at each connection with the parameter
// service (ADR-0012, spec 0015, protocol version 2 and later): the IMU noise
// parameters for the covariances of /imu/data, and COM_SP_TIMEOUT for the check
// of `cmd_vel_timeout`. A request that gets no reply is sent again. Free of
// rclcpp: it writes frames through a function and reads the clock it is given,
// so a test drives it with fakes.
#pragma once

#include <vk/link/frame.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vk::bridge {

class FloatParamReader {
public:
    // Sends a frame to the MCU; false when it could not be sent.
    using Send = std::function<bool(vk::link::FrameType, std::span<const std::uint8_t>)>;
    // Reports what happened, one sentence at a time, for the node to log.
    using Log = std::function<void(const std::string&)>;

    // `consequence` ends the sentence that says a parameter could not be read, such as
    // "its covariance stays unknown".
    FloatParamReader(std::vector<std::string> names, std::string consequence, Send send, Log log = {},
                     std::uint64_t retry_interval_us = 500'000)
        : names_(std::move(names)),
          consequence_(std::move(consequence)),
          send_(std::move(send)),
          log_(std::move(log)),
          retry_interval_us_(retry_interval_us),
          values_(names_.size()),
          settled_(names_.size(), false)
    {
    }

    // A new connection: forget the values, which the MCU may have changed, and ask again.
    void restart(std::uint64_t now_us);

    // Sends the requests again when the last ones went unanswered; call it every few milliseconds.
    void poll(std::uint64_t now_us);

    // Consumes a param_value or param_reject frame about one of the parameters;
    // false for any other frame.
    bool on_frame(const vk::link::Frame& frame, std::uint64_t now_us);

    // The value of the parameter at `index` in the names, empty until read or when refused.
    [[nodiscard]] std::optional<float> value(std::size_t index) const { return values_.at(index); }
    // Nothing more to ask: each parameter was read or refused.
    [[nodiscard]] bool complete() const;
    [[nodiscard]] std::uint32_t requests_sent() const { return requests_; }

private:
    void request(const std::string& name);
    void report(const std::string& text) const;
    [[nodiscard]] std::optional<std::size_t> index_of(std::string_view name) const;

    std::vector<std::string> names_;
    std::string consequence_;
    Send send_;
    Log log_;
    std::uint64_t retry_interval_us_;
    std::vector<std::optional<float>> values_;
    // A parameter is settled when the MCU answered, with a value or a refusal.
    std::vector<bool> settled_;
    bool active_ = false;
    std::uint64_t next_request_us_ = 0;
    std::uint32_t requests_ = 0;
    std::uint32_t unanswered_ = 0;
};

}  // namespace vk::bridge
