// SPDX-License-Identifier: GPL-3.0-or-later
// The user key's gestures (ADR-0022, spec 0016), from samples taken every 10 ms.
//
// A level counts once three samples in a row agree (30 ms debounce). A key
// that reads pressed at boot is ignored until it has been released. Then:
//
// - Press: the key went down. It is reported at once, so the display can show
//   it, and is replaced by Double when it is the second press of a double click.
// - Release: the key came up.
// - Click: released, with no second press within double_click_us of the release.
//   It is reported when that time has passed.
// - Double: a second press within double_click_us of the first release. It is
//   reported when that press goes down.
// - Long: held for long_press_us, reported once, while still held.
//
// A key pressed again after Double or Long, or held on, gives nothing more
// until it has been released, so a key held down or stuck confirms at most
// once. The recognizer has no clock of its own: time comes with each sample.
#pragma once

#include <cstdint>

namespace vk::local_display {

enum class KeyEvent : std::uint8_t {
    None,
    Press,    // the key went down; not reported for the second press of a double click
    Release,  // the key came up
    Click,    // released, and no second press within the double click time
    Double,   // a second press within the double click time of the first release
    Long,     // held for the long press time, once per press
};

class UserKey {
public:
    static constexpr std::uint32_t sample_interval_us = 10'000;
    static constexpr unsigned debounce_samples = 3;
    static constexpr std::uint64_t long_press_us = 1'000'000;
    static constexpr std::uint64_t double_click_us = 250'000;  // a constant (ADR-0022)

    // One sample: whether the key reads pressed, at `now_us`.
    KeyEvent sample(std::uint64_t now_us, bool pressed);

    // Drops the gesture in progress: nothing more is reported for the press
    // that is down, or for a Click it would still give, until the key has been
    // released. The display protection uses it for a press that only woke the
    // display.
    void swallow();

    // Whether the debounced key is down.
    [[nodiscard]] bool down() const { return phase_ == Phase::Pressed || phase_ == Phase::Consumed; }

private:
    enum class Phase : std::uint8_t {
        Boot,      // nothing debounced yet
        Blocked,   // pressed at boot: waiting for a release
        Released,
        Pressed,   // first press, before the long press
        Waiting,   // released once, a second press or the end of the double click time pending
        Consumed,  // a Long, Double or swallowed press, down: nothing more until released
    };

    Phase phase_ = Phase::Boot;
    bool raw_ = false;
    unsigned agreeing_ = 0;  // samples in a row reading raw_
    std::uint64_t pressed_at_us_ = 0;
    std::uint64_t released_at_us_ = 0;
};

}  // namespace vk::local_display
