// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's drawing functions (spec 0016, ADR-0022).
#include <vk/local_display/font.hpp>
#include <vk/local_display/graphics.hpp>
#include <vk/local_display/icons.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vk::local_display {

namespace {

constexpr int image_width = static_cast<int>(drivers::MonoFrame::width);
constexpr int image_height = static_cast<int>(drivers::MonoFrame::height);

}  // namespace

void Canvas::set_clip(int x, int y, int w, int h)
{
    clip_x0_ = std::clamp(x, 0, image_width);
    clip_y0_ = std::clamp(y, 0, image_height);
    clip_x1_ = std::clamp(x + std::max(w, 0), clip_x0_, image_width);
    clip_y1_ = std::clamp(y + std::max(h, 0), clip_y0_, image_height);
}

void Canvas::clear_clip()
{
    clip_x0_ = 0;
    clip_y0_ = 0;
    clip_x1_ = image_width;
    clip_y1_ = image_height;
}

void Canvas::pixel(int x, int y, bool on)
{
    if (x < clip_x0_ || x >= clip_x1_ || y < clip_y0_ || y >= clip_y1_) {
        return;
    }
    frame_.set(static_cast<std::size_t>(x), static_cast<std::size_t>(y), on);
}

bool Canvas::get(int x, int y) const
{
    if (x < clip_x0_ || x >= clip_x1_ || y < clip_y0_ || y >= clip_y1_) {
        return false;
    }
    return frame_.get(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
}

void Canvas::clear()
{
    frame_.clear();
}

void Canvas::hline(int x, int y, int length, bool on)
{
    // Only the part inside the clip is visited, so a huge length costs nothing.
    const int from = std::max(x, clip_x0_);
    const int to = std::min(x + length, clip_x1_);
    for (int i = from; i < to; ++i) {
        pixel(i, y, on);
    }
}

void Canvas::vline(int x, int y, int length, bool on)
{
    const int from = std::max(y, clip_y0_);
    const int to = std::min(y + length, clip_y1_);
    for (int i = from; i < to; ++i) {
        pixel(x, i, on);
    }
}

void Canvas::line(int x0, int y0, int x1, int y1, bool on)
{
    // Bresenham's algorithm in integers; pixel() clips.
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        pixel(x0, y0, on);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        const int e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void Canvas::rect(int x, int y, int w, int h, bool on)
{
    if (w < 1 || h < 1) {
        return;
    }
    hline(x, y, w, on);
    hline(x, y + h - 1, w, on);
    vline(x, y, h, on);
    vline(x + w - 1, y, h, on);
}

void Canvas::fill_rect(int x, int y, int w, int h, bool on)
{
    if (w < 1 || h < 1) {
        return;
    }
    const int from = std::max(y, clip_y0_);
    const int to = std::min(y + h, clip_y1_);
    for (int row = from; row < to; ++row) {
        hline(x, row, w, on);
    }
}

void Canvas::circle(int cx, int cy, int radius, bool on)
{
    if (radius < 0) {
        return;
    }
    // The midpoint algorithm: one octant, mirrored into eight.
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        pixel(cx + x, cy + y, on);
        pixel(cx - x, cy + y, on);
        pixel(cx + x, cy - y, on);
        pixel(cx - x, cy - y, on);
        pixel(cx + y, cy + x, on);
        pixel(cx - y, cy + x, on);
        pixel(cx + y, cy - x, on);
        pixel(cx - y, cy - x, on);
        ++y;
        if (error < 0) {
            error += (2 * y) + 1;
        } else {
            --x;
            error += (2 * (y - x)) + 1;
        }
    }
}

void Canvas::fill_circle(int cx, int cy, int radius, bool on)
{
    if (radius < 0) {
        return;
    }
    // The same octant walk as circle(), filling the spans between mirrored points.
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        hline(cx - x, cy + y, (2 * x) + 1, on);
        hline(cx - x, cy - y, (2 * x) + 1, on);
        hline(cx - y, cy + x, (2 * y) + 1, on);
        hline(cx - y, cy - x, (2 * y) + 1, on);
        ++y;
        if (error < 0) {
            error += (2 * y) + 1;
        } else {
            --x;
            error += (2 * (y - x)) + 1;
        }
    }
}

void Canvas::bitmap(int x, int y, const Bitmap& bitmap, bool on, bool opaque)
{
    if (bitmap.width < 1 || bitmap.height < 1) {
        return;
    }
    const auto width = static_cast<std::size_t>(bitmap.width);
    for (int row = 0; row < bitmap.height; ++row) {
        const auto band = static_cast<std::size_t>(row / 8);
        for (int column = 0; column < bitmap.width; ++column) {
            const std::size_t index = (band * width) + static_cast<std::size_t>(column);
            if (index >= bitmap.data.size()) {
                break;  // data shorter than the size says: draw what it holds
            }
            const bool lit = ((bitmap.data[index] >> (row % 8)) & 1U) != 0;
            if (lit) {
                pixel(x + column, y + row, on);
            } else if (opaque) {
                pixel(x + column, y + row, !on);
            }
        }
    }
}

int Canvas::text(int x, int y, std::string_view text, bool on)
{
    for (const char c : text) {
        const Glyph& g = glyph(c);
        for (std::size_t column = 0; column < font_width; ++column) {
            for (std::size_t row = 0; row < font_height; ++row) {
                if (((g.at(column) >> row) & 1U) != 0) {
                    pixel(x + static_cast<int>(column), y + static_cast<int>(row), on);
                }
            }
        }
        x += static_cast<int>(cell_width);
    }
    return x;
}

int Canvas::big_text(int x, int y, std::string_view text, bool on)
{
    for (const char c : text) {
        bitmap(x, y, big_glyph(c), on);
        x += big_cell_width;
    }
    return x;
}

}  // namespace vk::local_display
