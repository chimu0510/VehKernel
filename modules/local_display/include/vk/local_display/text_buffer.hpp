// SPDX-License-Identifier: GPL-3.0-or-later
// A small fixed-size text builder for the local display's pages (spec 0016,
// ADR-0022): strings, integers and fixed-point numbers, no allocation and no
// printf, so the firmware pays for no formatting library.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vk::local_display {

class TextBuffer {
public:
    static constexpr std::size_t capacity = 32;

    TextBuffer& add(std::string_view text)
    {
        for (const char c : text) {
            if (size_ < capacity) {
                chars_.at(size_++) = c;
            }
        }
        return *this;
    }

    TextBuffer& add(std::uint64_t value)
    {
        std::array<char, 20> digits{};
        std::size_t count = 0;
        do {
            digits.at(count++) = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (count > 0) {
            add(std::string_view(&digits.at(--count), 1));
        }
        return *this;
    }

    // `value` with `decimals` (0 to 3) decimals, rounded; "---" if it is not
    // finite or beyond +-999999. A value that rounds to zero has no sign.
    TextBuffer& add_fixed(float value, unsigned decimals)
    {
        constexpr std::array<double, 4> scales{1.0, 10.0, 100.0, 1000.0};
        const double scale = scales.at(decimals < scales.size() ? decimals : scales.size() - 1);
        const double scaled = std::round(static_cast<double>(value) * scale);
        if (!std::isfinite(scaled) || std::fabs(scaled) > 999999.0 * scale) {
            return add("---");
        }
        if (scaled < 0.0) {
            add("-");
        }
        const auto units = static_cast<std::uint64_t>(std::fabs(scaled));
        const auto whole = static_cast<std::uint64_t>(scale);
        add(units / whole);
        if (decimals > 0) {
            add(".");
            const std::uint64_t fraction = units % whole;
            for (std::uint64_t digit = whole / 10; digit > 0; digit /= 10) {
                add((fraction / digit) % 10);
            }
        }
        return *this;
    }

    // A counter in at most three characters: 999, 12K, 345K, 6M, 78M.
    TextBuffer& add_count(std::uint32_t value)
    {
        if (value < 1000) {
            return add(value);
        }
        if (value < 1'000'000) {
            return add(value / 1000).add("K");
        }
        return add(value / 1'000'000).add("M");
    }

    // Pads with spaces to `width` characters, or does nothing if longer.
    TextBuffer& pad_to(std::size_t width)
    {
        while (size_ < width && size_ < capacity) {
            chars_.at(size_++) = ' ';
        }
        return *this;
    }

    // Inserts spaces in front so the text is `width` characters.
    TextBuffer& right_align(std::size_t width)
    {
        if (size_ >= width || width > capacity) {
            return *this;
        }
        const std::size_t shift = width - size_;
        for (std::size_t i = size_; i > 0; --i) {
            chars_.at(i - 1 + shift) = chars_.at(i - 1);
        }
        for (std::size_t i = 0; i < shift; ++i) {
            chars_.at(i) = ' ';
        }
        size_ = width;
        return *this;
    }

    [[nodiscard]] std::string_view view() const { return {chars_.data(), size_}; }

private:
    std::array<char, capacity> chars_{};
    std::size_t size_ = 0;
};

}  // namespace vk::local_display
