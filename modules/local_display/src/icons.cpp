// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's 8 x 8 icons and large digit font (spec 0016, ADR-0022),
// drawn for Vehkernel. Each picture is written as rows, top first, '#' lit;
// the constexpr functions below turn them into the Bitmap layout at compile
// time, and a static_assert refuses a row of the wrong length.
#include <vk/local_display/icons.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vk::local_display {

namespace {

// Packs `Height` rows of `Width` characters into bands of `Width` bytes.
template <std::size_t Width, std::size_t Height>
constexpr auto pack(const std::array<std::string_view, Height>& rows)
{
    static_assert(Height % 8 == 0);
    std::array<std::uint8_t, Width * (Height / 8)> out{};
    for (std::size_t row = 0; row < Height; ++row) {
        for (std::size_t column = 0; column < Width; ++column) {
            if (rows.at(row).at(column) == '#') {
                auto& byte = out.at(((row / 8) * Width) + column);
                byte = static_cast<std::uint8_t>(byte | (1U << (row % 8)));
            }
        }
    }
    return out;
}

// Whether every row of every picture is `Width` characters.
template <std::size_t Width, std::size_t Count, std::size_t Height>
constexpr bool rows_fit(const std::array<std::array<std::string_view, Height>, Count>& pictures)
{
    for (const auto& picture : pictures) {
        for (const auto row : picture) {
            if (row.size() != Width) {
                return false;
            }
        }
    }
    return true;
}

using IconRows = std::array<std::string_view, 8>;
using BigRows = std::array<std::string_view, 16>;

// clang-format off
constexpr std::array<IconRows, icon_count> icon_rows{{
    // Armed: a shield
    {".######.", "########", "########", "########", ".######.", "..####..", "...##...", "........"},
    // EmergencyStop: an octagon with a dark bar
    {"..####..", ".######.", "########", "##....##", "##....##", "########", ".######.", "..####.."},
    // LinkUp: signal bars, rising
    {"......##", "......##", "....####", "....####", "..######", "..######", "########", "########"},
    // LinkDown: a cross above a dashed baseline
    {"#.#.....", ".#......", "#.#.....", "........", "........", "........", "........", "#.#.#.#."},
    // Gamepad: a body with a direction pad and two buttons
    {"........", ".######.", "##.##.##", "#..##..#", "##.##.##", "########", "##....##", "#......#"},
    // Companion: a screen on a stand
    {"########", "#......#", "#......#", "#......#", "########", "...##...", "..####..", "........"},
    // Fault: a bolt
    {"....##..", "...##...", "..##....", ".######.", "...##...", "..##....", ".##.....", "........"},
    // Warning: a triangle with a mark
    {"...##...", "..#..#..", "..#..#..", ".#.##.#.", ".#.##.#.", "#......#", "########", "........"},
    // Check
    {"........", ".......#", "......##", "#....##.", "##..##..", ".####...", "..##....", "........"},
    // Cross
    {"#......#", "##....##", ".##..##.", "..####..", "..####..", ".##..##.", "##....##", "#......#"},
}};

// The large font: '0' to '9', then '-', '.', ':' and ' '.
constexpr std::size_t big_glyph_count = 14;
constexpr std::array<BigRows, big_glyph_count> big_rows{{
    {"..######..", ".########.", "###....###", "##......##", "##......##", "##......##", "##......##", "##......##",
     "##......##", "##......##", "##......##", "##......##", "##......##", "###....###", ".########.", "..######.."},  // 0
    {"...###....", "..####....", ".#####....", "....##....", "....##....", "....##....", "....##....", "....##....",
     "....##....", "....##....", "....##....", "....##....", "....##....", "....##....", ".########.", ".########."},  // 1
    {"..######..", ".########.", "###....###", "##......##", "........##", "........##", "........##", ".......###",
     "......###.", ".....###..", "....###...", "..###.....", ".###......", "###.......", "##########", "##########"},  // 2
    {"..######..", ".########.", "###....###", "##......##", "........##", "........##", ".......###", "....#####.",
     "....#####.", ".......###", "........##", "........##", "##......##", "###....###", ".########.", "..######.."},  // 3
    {"......##..", ".....###..", "....####..", "...##.##..", "..##..##..", ".##...##..", "##....##..", "##....##..",
     "##########", "##########", "......##..", "......##..", "......##..", "......##..", "......##..", "......##.."},  // 4
    {"##########", "##########", "##........", "##........", "##........", "##.#####..", "#########.", "###....###",
     "........##", "........##", "........##", "........##", "##......##", "###....###", ".########.", "..######.."},  // 5
    {"..######..", ".########.", "###....###", "##........", "##........", "##........", "##.#####..", "#########.",
     "###....###", "##......##", "##......##", "##......##", "##......##", "###....###", ".########.", "..######.."},  // 6
    {"##########", "##########", "........##", "........##", ".......###", ".......##.", "......###.", "......##..",
     ".....###..", ".....##...", "....###...", "....##....", "...###....", "...##.....", "...##.....", "...##....."},  // 7
    {"..######..", ".########.", "###....###", "##......##", "##......##", "###....###", ".########.", "..######..",
     ".########.", "###....###", "##......##", "##......##", "##......##", "###....###", ".########.", "..######.."},  // 8
    {"..######..", ".########.", "###....###", "##......##", "##......##", "##......##", "##......##", "###....###",
     ".#########", "..#####.##", "........##", "........##", "........##", "###....###", ".########.", "..######.."},  // 9
    {"..........", "..........", "..........", "..........", "..........", "..........", "..........", ".########.",
     ".########.", "..........", "..........", "..........", "..........", "..........", "..........", ".........."},  // -
    {"..........", "..........", "..........", "..........", "..........", "..........", "..........", "..........",
     "..........", "..........", "..........", "..........", "..........", "..........", "....##....", "....##...."},  // .
    {"..........", "..........", "..........", "....##....", "....##....", "..........", "..........", "..........",
     "..........", "..........", "..........", "....##....", "....##....", "..........", "..........", ".........."},  // :
    {"..........", "..........", "..........", "..........", "..........", "..........", "..........", "..........",
     "..........", "..........", "..........", "..........", "..........", "..........", "..........", ".........."},  // space
}};
// clang-format on

template <std::size_t Count, std::size_t Width, std::size_t Height>
constexpr auto pack_all(const std::array<std::array<std::string_view, Height>, Count>& pictures)
{
    std::array<std::array<std::uint8_t, Width * (Height / 8)>, Count> out{};
    for (std::size_t i = 0; i < Count; ++i) {
        out.at(i) = pack<Width, Height>(pictures.at(i));
    }
    return out;
}

static_assert(rows_fit<8>(icon_rows), "every icon row is 8 characters");
static_assert(rows_fit<big_font_width>(big_rows), "every large glyph row is 10 characters");

constexpr auto icon_data = pack_all<icon_count, 8, 8>(icon_rows);
constexpr auto big_data = pack_all<big_glyph_count, big_font_width, big_font_height>(big_rows);

constexpr std::size_t big_index(char c)
{
    if (c >= '0' && c <= '9') {
        return static_cast<std::size_t>(c - '0');
    }
    switch (c) {
    case '-': return 10;
    case '.': return 11;
    case ':': return 12;
    default: return 13;
    }
}

}  // namespace

Bitmap icon(Icon which)
{
    const auto index = static_cast<std::size_t>(which);
    if (index >= icon_count) {
        return {};
    }
    return {.width = icon_size, .height = icon_size, .data = icon_data.at(index)};
}

Bitmap big_glyph(char c)
{
    return {.width = big_font_width, .height = big_font_height, .data = big_data.at(big_index(c))};
}

}  // namespace vk::local_display
