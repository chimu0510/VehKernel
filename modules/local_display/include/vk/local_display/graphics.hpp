// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's drawing functions (spec 0016, ADR-0022) on the SSD1306
// driver's 128 x 64 image: pixels, lines, rectangles, circles, bitmaps and
// text, all clipped to the image and to an optional clip rectangle.
//
// No animation, grey levels or scrolling text: a pixel is lit or dark. Every
// function takes signed coordinates, so a shape may start or end outside the
// image, and draws only what falls inside; coordinates stay within +-16000.
// Nothing here allocates, uses the bus or FreeRTOS, so it is tested on the
// development host.
#pragma once

#include <vk/drivers/ssd1306.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vk::local_display {

// A monochrome bitmap in the frame's layout: its rows are cut into bands of
// eight, top band first; `data` holds the bands one after another, each as
// `width` bytes, left column first, and bit n of a byte is row n of its band.
// An 8 x 8 icon is one band, a 10 x 16 digit two.
struct Bitmap {
    int width = 0;
    int height = 0;
    std::span<const std::uint8_t> data;
};

class Canvas {
public:
    explicit Canvas(drivers::MonoFrame& frame) : frame_(frame) {}

    // Restricts all drawing to the rectangle at (x, y), `w` by `h` pixels,
    // cut to the image. clear_clip() lifts it.
    void set_clip(int x, int y, int w, int h);
    void clear_clip();

    // Lights, or with `on` false darkens, a pixel.
    void pixel(int x, int y, bool on = true);
    // Whether a pixel is lit; false outside the image and the clip.
    [[nodiscard]] bool get(int x, int y) const;
    // Darkens the whole image, ignoring the clip.
    void clear();

    // A horizontal or vertical line of `length` pixels from (x, y) right or down.
    void hline(int x, int y, int length, bool on = true);
    void vline(int x, int y, int length, bool on = true);
    // A straight line from (x0, y0) to (x1, y1), both ends included.
    void line(int x0, int y0, int x1, int y1, bool on = true);
    // A rectangle `w` by `h` pixels with its top left corner at (x, y); nothing
    // if `w` or `h` is below 1.
    void rect(int x, int y, int w, int h, bool on = true);
    void fill_rect(int x, int y, int w, int h, bool on = true);
    // A circle of `radius` pixels around the pixel (cx, cy), so 2 * radius + 1
    // pixels wide; nothing if `radius` is negative.
    void circle(int cx, int cy, int radius, bool on = true);
    void fill_circle(int cx, int cy, int radius, bool on = true);

    // A bitmap with its top left corner at (x, y). Lit bits light a pixel;
    // dark bits leave the image alone, or darken it with `opaque`.
    void bitmap(int x, int y, const Bitmap& bitmap, bool on = true, bool opaque = false);

    // Text in the 5 x 7 font, one glyph per 6 x 8 cell, the top left of the
    // first cell at (x, y). Returns the x after the last cell.
    int text(int x, int y, std::string_view text, bool on = true);
    // Text in the large 10 x 16 font (digits, '-', '.', ':' and space), one
    // glyph per 12 pixels, the top left at (x, y). Returns the x after it.
    int big_text(int x, int y, std::string_view text, bool on = true);

private:
    drivers::MonoFrame& frame_;
    int clip_x0_ = 0;
    int clip_y0_ = 0;
    int clip_x1_ = static_cast<int>(drivers::MonoFrame::width);   // exclusive
    int clip_y1_ = static_cast<int>(drivers::MonoFrame::height);  // exclusive
};

// The width of `count` characters of text() and big_text().
[[nodiscard]] constexpr int text_width(std::size_t count) { return static_cast<int>(count) * 6; }
[[nodiscard]] constexpr int big_text_width(std::size_t count) { return static_cast<int>(count) * 12; }

}  // namespace vk::local_display
