// SPDX-License-Identifier: GPL-3.0-or-later
// Reads the IMU's noise parameters from the MCU (ADR-0012, spec 0015).
#include <vehkernel_bridge/imu_noise.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

namespace vk::bridge {

namespace {

constexpr std::uint8_t param_type_float = 2;  // ADR-0012: 1 int32, 2 float32
// A request sent this many times without any answer is worth saying once.
constexpr std::uint32_t unanswered_warning = 6;

std::string_view read_name(vk::link::BodyReader& reader)
{
    const std::uint8_t length = reader.u8();
    const auto bytes = reader.bytes(length);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

void ImuNoiseReader::restart(std::uint64_t now_us)
{
    gyro_.reset();
    accel_.reset();
    gyro_settled_ = false;
    accel_settled_ = false;
    active_ = true;
    requests_ = 0;
    unanswered_ = 0;
    next_request_us_ = now_us;
    poll(now_us);
}

void ImuNoiseReader::request(const char* name)
{
    std::array<std::uint8_t, 32> body{};
    vk::link::BodyWriter writer(body);
    const std::string_view text(name);
    writer.u8(static_cast<std::uint8_t>(text.size()));
    writer.bytes({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    if (send_ && send_(vk::link::FrameType::ParamGet, writer.written())) {
        ++requests_;
    }
}

void ImuNoiseReader::poll(std::uint64_t now_us)
{
    if (!active_ || complete() || now_us < next_request_us_) {
        return;
    }
    next_request_us_ = now_us + retry_interval_us_;
    if (++unanswered_ == unanswered_warning) {
        report("the MCU does not answer the requests for the IMU noise parameters; still trying");
    }
    if (!gyro_settled_) {
        request(gyro_name);
    }
    if (!accel_settled_) {
        request(accel_name);
    }
}

bool ImuNoiseReader::on_frame(const vk::link::Frame& frame, std::uint64_t now_us)
{
    if (!active_) {
        return false;
    }
    if (frame.type == vk::link::FrameType::ParamValue) {
        vk::link::BodyReader reader(frame.body);
        (void)reader.u16();  // index
        (void)reader.u16();  // count
        const std::uint8_t type = reader.u8();
        const std::uint32_t bits = reader.u32();
        (void)reader.u8();  // flags
        const std::string_view name = read_name(reader);
        if (!reader.ok()) {
            return false;
        }
        const bool is_gyro = name == gyro_name;
        if (!is_gyro && name != accel_name) {
            return false;
        }
        float value{};
        std::memcpy(&value, &bits, sizeof(value));
        if (type != param_type_float || !std::isfinite(value) || value < 0.0F) {
            report(std::string(name) + " is not a usable noise value; its covariance stays unknown");
            (is_gyro ? gyro_ : accel_) = std::nullopt;
            (is_gyro ? gyro_settled_ : accel_settled_) = true;
            return true;
        }
        (is_gyro ? gyro_ : accel_) = value;
        (is_gyro ? gyro_settled_ : accel_settled_) = true;
        report(std::string("read ") + std::string(name) + " = " + std::to_string(value));
        unanswered_ = 0;
        next_request_us_ = now_us + retry_interval_us_;
        return true;
    }
    if (frame.type == vk::link::FrameType::ParamReject) {
        vk::link::BodyReader reader(frame.body);
        const std::uint8_t reason = reader.u8();
        const std::string_view name = read_name(reader);
        if (!reader.ok() || (name != gyro_name && name != accel_name)) {
            return false;
        }
        report(std::string("the MCU refused to read ") + std::string(name) + " (reason " + std::to_string(reason) +
               "); its covariance stays unknown");
        // Asking again would get the same answer: the build lacks the parameter.
        ((name == gyro_name) ? gyro_ : accel_) = std::nullopt;
        ((name == gyro_name) ? gyro_settled_ : accel_settled_) = true;
        return true;
    }
    return false;
}

void ImuNoiseReader::report(const std::string& text) const
{
    if (log_) {
        log_(text);
    }
}

}  // namespace vk::bridge
