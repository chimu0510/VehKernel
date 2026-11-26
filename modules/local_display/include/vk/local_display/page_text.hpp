// SPDX-License-Identifier: GPL-3.0-or-later
// Layout and text helpers shared by the arming checks, sensors, Links, remote
// control and system pages (spec 0016, ADR-0022).
//
// A page draws only the 56 pixels below the 8-pixel status bar and keeps a
// one-pixel margin on every side of the image, so the image can move by a
// pixel against marking. Six lines of the 5 x 7 font fit that area.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/local_display/text_buffer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vk::local_display::page {

inline constexpr int status_bar_height = 8;
// The last pixel columns and rows a page may draw in; the margin is one pixel.
inline constexpr int left = 1;
inline constexpr int right = 126;
inline constexpr int top = 9;
inline constexpr int bottom = 62;

inline constexpr std::size_t columns = 21;  // 21 cells of 6 pixels from `left`
inline constexpr std::size_t rows = 6;

// The top of the text line `row` (0 to 5): the line's glyphs span seven pixels.
[[nodiscard]] constexpr int row_y(std::size_t row) { return 11 + (static_cast<int>(row) * 8); }

// What TextBuffer lacks for these pages: zero-padded numbers, a commit as
// eight hexadecimal digits and a duration. They append to a TextBuffer.

// `value` with `width` digits, padded with zeros.
inline TextBuffer& add_padded(TextBuffer& line, std::uint64_t value, std::size_t width)
{
    std::array<char, 20> digits{};
    const std::size_t count = width < digits.size() ? width : digits.size();
    for (std::size_t index = 0; index < count; ++index) {
        digits.at(count - 1 - index) = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    return line.add(std::string_view(digits.data(), count));
}

// The eight lowercase hexadecimal digits of `value`.
inline TextBuffer& add_hex32(TextBuffer& line, std::uint32_t value)
{
    std::array<char, 8> digits{};
    for (std::size_t index = 0; index < digits.size(); ++index) {
        const unsigned nibble = (value >> ((digits.size() - 1 - index) * 4U)) & 0xFU;
        digits.at(index) = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
    return line.add(std::string_view(digits.data(), digits.size()));
}

// Draws `text` on line `row` from pixel column `x`.
// cut off at the 21st column from `left`, so it stays inside the margin.
inline void draw_row(Canvas& canvas, std::size_t row, std::string_view text, int x = left)
{
    const auto room = static_cast<std::size_t>((right + 1 - x) / 6);
    canvas.text(x, row_y(row), text.substr(0, room));
}

// A duration as "42S", "3M 05S", "2H 03M" or "1D 02H", whichever is the
// coarsest that is not zero.
inline TextBuffer& add_duration(TextBuffer& line, std::uint64_t seconds)
{
    if (seconds < 60) {
        return line.add(seconds).add("S");
    }
    if (seconds < 3600) {
        line.add(seconds / 60).add("M ");
        return add_padded(line, seconds % 60, 2).add("S");
    }
    if (seconds < 86400) {
        line.add(seconds / 3600).add("H ");
        return add_padded(line, (seconds % 3600) / 60, 2).add("M");
    }
    line.add(seconds / 86400).add("D ");
    return add_padded(line, (seconds % 86400) / 3600, 2).add("H");
}

}  // namespace vk::local_display::page
