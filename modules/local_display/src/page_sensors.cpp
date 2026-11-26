// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/page_sensors.hpp>

#include <vk/local_display/page_text.hpp>
#include <vk/local_display/text_buffer.hpp>
#include <vk/msg/enums.hpp>

#include <cmath>
#include <cstddef>
#include <string_view>

namespace vk::local_display {

void SampleRateMeter::add(std::uint64_t timestamp_us, std::uint64_t published)
{
    if (!started_ || timestamp_us < last_us_ || published < last_published_) {
        started_ = true;
        first_us_ = timestamp_us;
        last_us_ = timestamp_us;
        first_published_ = published;
        last_published_ = published;
        return;
    }
    last_us_ = timestamp_us;
    last_published_ = published;
    const std::uint64_t span = last_us_ - first_us_;
    if (span >= window_us) {
        const auto samples = static_cast<double>(last_published_ - first_published_);
        rate_hz_ = static_cast<float>(samples * 1e6 / static_cast<double>(span));
        first_us_ = last_us_;
        first_published_ = last_published_;
    }
}

bool magnetometer_calibration_applied(const params::Cal& cal)
{
    const params::Cal defaults{};
    return cal.mag_xoff != defaults.mag_xoff || cal.mag_yoff != defaults.mag_yoff || cal.mag_zoff != defaults.mag_zoff ||
           cal.mag_xscale != defaults.mag_xscale || cal.mag_yscale != defaults.mag_yscale ||
           cal.mag_zscale != defaults.mag_zscale;
}

float magnetic_field_strength(const msg::MagnetometerSample& sample)
{
    return std::sqrt((sample.field[0] * sample.field[0]) + (sample.field[1] * sample.field[1]) +
                     (sample.field[2] * sample.field[2]));
}

void draw_sensors_page(Canvas& canvas, const SensorsPageInputs& inputs)
{
    using TB = TextBuffer;

    // The IMU: whether it answered, and the rate its samples arrive at.
    TB imu;
    imu.add("IMU ");
    imu.add(!inputs.status ? "?" : (inputs.status->board_verified ? "ID OK" : "NO ID"));
    imu.pad_to(11);
    if (inputs.imu_rate_hz && std::isfinite(*inputs.imu_rate_hz)) {
        imu.add(static_cast<std::uint64_t>(std::lround(*inputs.imu_rate_hz))).add(" HZ");
    } else {
        imu.add("--- HZ");
    }
    page::draw_row(canvas, 0, imu.view());

    // The magnetometer: its setup state, and what it has delivered.
    if (!inputs.magnetometer_status) {
        page::draw_row(canvas, 1, "MAG NO DATA");
    } else {
        const auto& status = *inputs.magnetometer_status;
        const std::string_view state = msg::enum_name(static_cast<msg::MagnetometerState>(status.state));
        page::draw_row(canvas, 1, TB().add("MAG ").add(state.empty() ? "?" : state).view());
        page::draw_row(canvas, 2, TB().add("SMP ").add(status.samples).pad_to(12).add("FAIL ").add(status.failures).view());
        const std::string_view failure = msg::enum_name(static_cast<msg::MagnetometerFailure>(status.last_failure));
        page::draw_row(canvas, 3, TB().add("LAST ").add(failure.empty() ? "?" : failure).view());
    }
    TB field;
    field.add("FIELD ");
    if (inputs.magnetometer_sample) {
        field.add_fixed(magnetic_field_strength(*inputs.magnetometer_sample), 1).add(" UT");
    } else {
        field.add("---");
    }
    page::draw_row(canvas, 4, field.view());

    page::draw_row(canvas, 5, inputs.magnetometer_calibration_applied ? "CAL APPLIED" : "CAL NOT APPLIED");
}

}  // namespace vk::local_display
