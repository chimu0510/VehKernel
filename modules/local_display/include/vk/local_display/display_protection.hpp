// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's protection of the panel (spec 0016, ADR-0022): an OLED
// that shows the same image marks.
//
// While disarmed the display dims after DSP_DIM_S seconds without a key press
// and turns off after DSP_OFF_S (0 never does). While armed it is never dimmed
// or off, and the idle time restarts when the vehicle disarms. The local menu
// does not count as idle. Every two minutes the image moves by one pixel along
// a four-step cycle, when DSP_SHIFT is on, armed or not. A key press on a
// dimmed or dark display only wakes it. Arming or disarming, a mode change,
// the emergency stop engaging, a fault, an arming refusal and a takeover by
// the remote control wake it too and restart the idle time.
//
// The class decides; the LocalDisplay work item applies the result to the
// SSD1306 driver. It has no clock, bus or FreeRTOS: the time comes with each
// call, so it is tested on the development host.
#pragma once

#include <vk/msg/vehicle_status.hpp>

#include <cstdint>
#include <optional>

namespace vk::local_display {

// DSP_BRIGHT, DSP_DIM_S, DSP_OFF_S and DSP_SHIFT.
struct ProtectionSettings {
    int bright_percent = 100;  // 10 to 100
    int dim_seconds = 60;      // 0 never dims
    int off_seconds = 600;     // 0 never turns off
    bool shift = true;
};

enum class DisplayLevel : std::uint8_t {
    Awake,
    Dimmed,
    Off,
};

class DisplayProtection {
public:
    static constexpr std::uint64_t shift_interval_us = 120'000'000;  // two minutes
    static constexpr int dim_percent = 10;                           // of the full contrast
    static constexpr std::uint8_t full_contrast = 0xEF;              // the vendor firmware's

    // Runs every display run: applies the idle time and the shift timer.
    // `menu_open` holds the idle time still while the menu is in use.
    void update(std::uint64_t now_us, bool armed, bool menu_open, const ProtectionSettings& settings);

    // A key press at `now_us`: restarts the idle time. Returns true when the
    // display was dimmed or off, so the press only woke it and must not act.
    bool on_key_press(std::uint64_t now_us);

    // A wake event of the vehicle: wakes the display and restarts the idle time.
    void wake(std::uint64_t now_us);

    // The next vehicle_status, in order: wakes the display when arming or
    // disarming, the mode, the emergency stop engaging, a new fault, a
    // different arming refusal or a takeover by the remote control changed.
    // The first one only records the state.
    void on_status(std::uint64_t now_us, const msg::VehicleStatus& status);

    [[nodiscard]] DisplayLevel level() const { return level_; }
    // The contrast to set on the panel, 0 to 255, for the settings of the last update().
    [[nodiscard]] std::uint8_t contrast() const { return contrast_; }
    [[nodiscard]] bool display_on() const { return level_ != DisplayLevel::Off; }
    // The image offset, each 0 or 1: the step of the cycle (0,0), (1,0), (1,1), (0,1).
    [[nodiscard]] int shift_x() const { return shift_x_; }
    [[nodiscard]] int shift_y() const { return shift_y_; }

    // The contrast of `percent` of the full contrast.
    [[nodiscard]] static constexpr std::uint8_t contrast_of(int percent)
    {
        return static_cast<std::uint8_t>((full_contrast * percent + 50) / 100);
    }

private:
    bool started_ = false;  // update() has run once
    std::uint64_t last_activity_us_ = 0;
    std::uint64_t next_shift_us_ = shift_interval_us;
    DisplayLevel level_ = DisplayLevel::Awake;
    std::uint8_t contrast_ = full_contrast;
    std::uint8_t normal_contrast_ = full_contrast;  // DSP_BRIGHT's, at the last update()
    unsigned shift_step_ = 0;
    int shift_x_ = 0;
    int shift_y_ = 0;
    std::optional<msg::VehicleStatus> status_;
};

}  // namespace vk::local_display
