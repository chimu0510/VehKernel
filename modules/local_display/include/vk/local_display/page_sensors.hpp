// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's sensors page (spec 0016, ADR-0022): the IMU (whether it
// answered, its sample rate measured from `imu_sample`), the magnetometer
// (state, samples, failures, field strength) and whether a magnetometer
// calibration is applied.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/imu_sample.hpp>
#include <vk/msg/magnetometer_sample.hpp>
#include <vk/msg/magnetometer_status.hpp>
#include <vk/msg/vehicle_status.hpp>
#include <vk/params/params.hpp>

#include <cstdint>
#include <optional>

namespace vk::local_display {

struct SensorsPageInputs {
    // `board_verified`: the IMU answered (ADR-0010).
    std::optional<msg::VehicleStatus> status;
    // The IMU's sample rate in Hz, from SampleRateMeter; nothing before a rate is known.
    std::optional<float> imu_rate_hz;
    std::optional<msg::MagnetometerStatus> magnetometer_status;
    std::optional<msg::MagnetometerSample> magnetometer_sample;
    // Whether the CAL_MAG_* parameters differ from their defaults.
    bool magnetometer_calibration_applied = false;
};

// Measures a sample rate from the timestamps of the samples taken. imu_sample
// is a short queue, so a display that looks only now and then sees a few of
// the samples; each look therefore gives the newest sample's timestamp and the
// number of samples published so far (those read plus those missed). Over a
// window of at least one second the rate is the samples published divided by
// the time between the timestamps, and it is kept until the next window is
// full. A timestamp or count that goes back starts a new window.
class SampleRateMeter {
public:
    static constexpr std::uint64_t window_us = 1'000'000;

    void add(std::uint64_t timestamp_us, std::uint64_t published);

    [[nodiscard]] std::optional<float> rate_hz() const { return rate_hz_; }

private:
    bool started_ = false;
    std::uint64_t first_us_ = 0;
    std::uint64_t last_us_ = 0;
    std::uint64_t first_published_ = 0;
    std::uint64_t last_published_ = 0;
    std::optional<float> rate_hz_;
};

// Whether any CAL_MAG_* offset or scale differs from its default.
[[nodiscard]] bool magnetometer_calibration_applied(const params::Cal& cal);

// The magnetic field's strength in uT.
[[nodiscard]] float magnetic_field_strength(const msg::MagnetometerSample& sample);

// Draws the page into the 56 pixels below the status bar.
void draw_sensors_page(Canvas& canvas, const SensorsPageInputs& inputs);

}  // namespace vk::local_display
