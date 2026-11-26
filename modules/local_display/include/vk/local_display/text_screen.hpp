// SPDX-License-Identifier: GPL-3.0-or-later
// A 21 x 8 text screen in the 5 x 7 font (ADR-0022). The local display's pages
// draw graphics since spec 0016; the examples still render their text with it.
//
// It uses neither the bus nor FreeRTOS, so it is tested on the development host.
#pragma once

#include <vk/drivers/ssd1306.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace vk::local_display {

// 21 columns by 8 lines of text, the font's cells on a 128 x 64 display.
class TextScreen {
public:
    static constexpr std::size_t columns = 21;
    static constexpr std::size_t lines = 8;

    TextScreen() { clear(); }

    // Fills every cell with spaces.
    void clear();
    // Writes `text` from `column` of `line`; what does not fit is cut off.
    void put(std::size_t line, std::size_t column, std::string_view text);
    // Writes `text` ending at the last column of `line`.
    void put_right(std::size_t line, std::string_view text);
    [[nodiscard]] std::string_view line(std::size_t line) const;

    // Draws every cell into `frame`, one pixel in from the left edge.
    void draw(drivers::MonoFrame& frame) const;

    friend bool operator==(const TextScreen&, const TextScreen&) = default;

private:
    std::array<std::array<char, columns>, lines> text_{};
};

}  // namespace vk::local_display
