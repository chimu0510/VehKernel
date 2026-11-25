// SPDX-License-Identifier: GPL-3.0-or-later
// Reads the IMU's noise parameters, EKF_GYR_NOISE and EKF_ACC_NOISE, from the MCU
// at each connection with the parameter service (ADR-0012, spec 0015, protocol
// version 2 and later), for the covariances of /imu/data. A request that gets no
// reply is sent again. Free of rclcpp: it writes frames through a function and
// reads the clock it is given, so a test drives it with fakes.
#pragma once

#include <vk/link/frame.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace vk::bridge {

class ImuNoiseReader {
public:
    static constexpr const char* gyro_name = "EKF_GYR_NOISE";
    static constexpr const char* accel_name = "EKF_ACC_NOISE";

    // Sends a frame to the MCU; false when it could not be sent.
    using Send = std::function<bool(vk::link::FrameType, std::span<const std::uint8_t>)>;
    // Reports what happened, one sentence at a time, for the node to log.
    using Log = std::function<void(const std::string&)>;

    explicit ImuNoiseReader(Send send, Log log = {}, std::uint64_t retry_interval_us = 500'000)
        : send_(std::move(send)), log_(std::move(log)), retry_interval_us_(retry_interval_us)
    {
    }

    // A new connection: forget the values, which the MCU may have changed, and ask again.
    void restart(std::uint64_t now_us);

    // Sends a request again when the last one went unanswered; call it every few milliseconds.
    void poll(std::uint64_t now_us);

    // Consumes a param_value or param_reject frame about one of the two parameters;
    // false for any other frame.
    bool on_frame(const vk::link::Frame& frame, std::uint64_t now_us);

    [[nodiscard]] std::optional<float> gyro() const { return gyro_; }
    [[nodiscard]] std::optional<float> accel() const { return accel_; }
    // Nothing more to ask: each parameter was read or refused.
    [[nodiscard]] bool complete() const { return gyro_settled_ && accel_settled_; }
    [[nodiscard]] std::uint32_t requests_sent() const { return requests_; }

private:
    void request(const char* name);
    void report(const std::string& text) const;

    Send send_;
    Log log_;
    std::uint64_t retry_interval_us_;
    std::optional<float> gyro_;
    std::optional<float> accel_;
    // A parameter is settled when the MCU answered, with a value or a refusal.
    bool gyro_settled_ = false;
    bool accel_settled_ = false;
    bool active_ = false;
    std::uint64_t next_request_us_ = 0;
    std::uint32_t requests_ = 0;
    std::uint32_t unanswered_ = 0;
};

}  // namespace vk::bridge
