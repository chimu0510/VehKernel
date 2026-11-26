// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/user_key.hpp>

namespace vk::local_display {

KeyEvent UserKey::sample(std::uint64_t now_us, bool pressed)
{
    if (pressed == raw_) {
        if (agreeing_ < debounce_samples) {
            ++agreeing_;
        }
    } else {
        raw_ = pressed;
        agreeing_ = 1;
    }
    if (agreeing_ < debounce_samples) {
        // Not settled: the level stays what it was, so a pending long press still runs on time.
        pressed = down();
    }

    switch (phase_) {
    case Phase::Boot:
        if (agreeing_ >= debounce_samples) {
            phase_ = pressed ? Phase::Blocked : Phase::Released;
        }
        return KeyEvent::None;
    case Phase::Blocked:
        if (!pressed) {
            phase_ = Phase::Released;
        }
        return KeyEvent::None;
    case Phase::Released:
        if (pressed) {
            phase_ = Phase::Pressed;
            pressed_at_us_ = now_us;
            return KeyEvent::Press;
        }
        return KeyEvent::None;
    case Phase::Pressed:
        if (!pressed) {
            phase_ = Phase::Waiting;
            released_at_us_ = now_us;
            return KeyEvent::Release;
        }
        if (now_us - pressed_at_us_ >= long_press_us) {
            phase_ = Phase::Consumed;
            return KeyEvent::Long;
        }
        return KeyEvent::None;
    case Phase::Waiting:
        // The time is checked first, so a press at exactly the double click
        // time after the release is a new press, not a second one.
        if (now_us - released_at_us_ >= double_click_us) {
            phase_ = Phase::Released;
            return KeyEvent::Click;
        }
        if (pressed) {
            phase_ = Phase::Consumed;
            return KeyEvent::Double;
        }
        return KeyEvent::None;
    case Phase::Consumed:
        if (!pressed) {
            phase_ = Phase::Released;
            return KeyEvent::Release;
        }
        return KeyEvent::None;
    }
    return KeyEvent::None;
}

void UserKey::swallow()
{
    if (phase_ == Phase::Pressed) {
        phase_ = Phase::Consumed;
    } else if (phase_ == Phase::Waiting) {
        phase_ = Phase::Released;
    }
}

}  // namespace vk::local_display
