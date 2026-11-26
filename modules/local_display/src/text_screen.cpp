// SPDX-License-Identifier: GPL-3.0-or-later
// The text screen of the examples (ADR-0022).
#include <vk/local_display/text_screen.hpp>

#include <vk/local_display/font.hpp>

#include <cstddef>
#include <string_view>

namespace vk::local_display {

void TextScreen::clear()
{
    for (auto& line : text_) {
        line.fill(' ');
    }
}

void TextScreen::put(std::size_t line, std::size_t column, std::string_view text)
{
    if (line >= lines) {
        return;
    }
    for (const char c : text) {
        if (column >= columns) {
            return;
        }
        text_.at(line).at(column++) = c;
    }
}

void TextScreen::put_right(std::size_t line, std::string_view text)
{
    const std::size_t width = text.size() < columns ? text.size() : columns;
    put(line, columns - width, text.substr(text.size() - width));
}

std::string_view TextScreen::line(std::size_t line) const
{
    return {text_.at(line).data(), columns};
}

void TextScreen::draw(drivers::MonoFrame& frame) const
{
    // A cell is as tall as a frame page and a glyph column's bits are its rows,
    // as a frame byte's are, so each glyph column is one byte of its line's page.
    static_assert(cell_height == 8 && lines == drivers::MonoFrame::pages);
    static_assert(1 + (columns * cell_width) <= drivers::MonoFrame::width);
    frame.clear();
    for (std::size_t line = 0; line < lines; ++line) {
        auto& page = frame.bytes.at(line);
        for (std::size_t column = 0; column < columns; ++column) {
            const Glyph& g = glyph(text_.at(line).at(column));
            const std::size_t x0 = 1 + (column * cell_width);
            for (std::size_t x = 0; x < font_width; ++x) {
                page.at(x0 + x) = g.at(x);
            }
        }
    }
}

}  // namespace vk::local_display
