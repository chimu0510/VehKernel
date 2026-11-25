// SPDX-License-Identifier: GPL-3.0-or-later
// The IMU's noise parameters, EKF_GYR_NOISE and EKF_ACC_NOISE, read from the MCU
// at each connection (ADR-0012, spec 0015), for the covariances of /imu/data. A
// thin name for a FloatParamReader over the two parameters.
#pragma once

#include <vehkernel_bridge/float_param_reader.hpp>

#include <cstdint>
#include <optional>

namespace vk::bridge {

class ImuNoiseReader {
public:
    static constexpr const char* gyro_name = "EKF_GYR_NOISE";
    static constexpr const char* accel_name = "EKF_ACC_NOISE";

    using Send = FloatParamReader::Send;
    using Log = FloatParamReader::Log;

    explicit ImuNoiseReader(Send send, Log log = {}, std::uint64_t retry_interval_us = 500'000)
        : reader_({gyro_name, accel_name}, "its covariance stays unknown", std::move(send), std::move(log),
                  retry_interval_us)
    {
    }

    void restart(std::uint64_t now_us) { reader_.restart(now_us); }
    void poll(std::uint64_t now_us) { reader_.poll(now_us); }
    bool on_frame(const vk::link::Frame& frame, std::uint64_t now_us) { return reader_.on_frame(frame, now_us); }

    [[nodiscard]] std::optional<float> gyro() const { return reader_.value(0); }
    [[nodiscard]] std::optional<float> accel() const { return reader_.value(1); }
    [[nodiscard]] bool complete() const { return reader_.complete(); }
    [[nodiscard]] std::uint32_t requests_sent() const { return reader_.requests_sent(); }

private:
    FloatParamReader reader_;
};

}  // namespace vk::bridge
