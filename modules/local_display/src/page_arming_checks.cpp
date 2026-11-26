// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/page_arming_checks.hpp>

#include <vk/local_display/icons.hpp>
#include <vk/local_display/page_text.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace vk::local_display {

namespace {

constexpr int marker_x = page::left;
constexpr int name_x = page::left + 22;

// The scroll bar's column and extent: the right-most column a page may use.
constexpr int bar_x = page::right;

void draw_scroll_bar(Canvas& canvas, std::size_t first)
{
    const int track = page::bottom - page::top + 1;
    const int thumb = std::max(3, track * static_cast<int>(arming_checks_lines_fit) / static_cast<int>(arming_checks_line_count));
    const int max_scroll = static_cast<int>(arming_checks_max_scroll());
    const int at = max_scroll > 0 ? (track - thumb) * static_cast<int>(first) / max_scroll : 0;
    canvas.vline(bar_x, page::top + at, thumb);
}

}  // namespace

void draw_arming_checks_page(Canvas& canvas, const ArmingChecksPageInputs& inputs)
{
    if (!inputs.checks) {
        page::draw_row(canvas, 0, "ARMING CHECKS");
        page::draw_row(canvas, 1, "NO DATA");
        return;
    }
    const std::size_t first = std::min(inputs.scroll, arming_checks_max_scroll());
    for (std::size_t row = 0; row < arming_checks_lines_fit && first + row < arming_checks_line_count; ++row) {
        // The line's check is the value of arming_result after OK.
        const std::size_t value = first + row + 1;
        const std::uint32_t bit = 1U << value;
        const int y = page::row_y(row);
        if ((inputs.checks->skipped & bit) != 0) {
            canvas.text(marker_x, y, "SKP");
        } else {
            canvas.bitmap(marker_x, y, icon((inputs.checks->failing & bit) != 0 ? Icon::Cross : Icon::Check));
        }
        const std::string_view name = msg::enum_short_name(static_cast<msg::ArmingResult>(value));
        canvas.text(name_x, y, name.empty() ? "?" : name);
    }
    if (arming_checks_line_count > arming_checks_lines_fit) {
        draw_scroll_bar(canvas, first);
    }
}

}  // namespace vk::local_display
