// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's 8 x 8 icons and its large digit font (spec 0016,
// ADR-0022), drawn for Vehkernel: constant arrays, no third-party data.
#pragma once

#include <vk/local_display/graphics.hpp>

#include <cstddef>
#include <cstdint>

namespace vk::local_display {

enum class Icon : std::uint8_t {
    Armed,
    EmergencyStop,
    LinkUp,
    LinkDown,
    Gamepad,
    Companion,
    Fault,
    Warning,
    Check,
    Cross,
};
inline constexpr std::size_t icon_count = 10;
inline constexpr int icon_size = 8;

// The icon as an 8 x 8 bitmap.
Bitmap icon(Icon icon);

inline constexpr int big_font_width = 10;
inline constexpr int big_font_height = 16;
inline constexpr int big_cell_width = 12;  // 10 pixels and 2 of space

// The large font's glyph of a character: '0' to '9', '-', '.', ':' or ' '; a
// blank for any other.
Bitmap big_glyph(char c);

}  // namespace vk::local_display
