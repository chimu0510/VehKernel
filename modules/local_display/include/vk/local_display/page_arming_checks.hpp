// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's arming checks page (spec 0016, ADR-0022): one line per
// check of `arming_result`, with a check mark when it passes, a cross when it
// would refuse an arm request now, or `SKP` when it is skipped, from the
// commander's `arming_checks` topic. The checks are more than the page has
// lines for, so a click scrolls and, at the end, moves on.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/arming_checks.hpp>
#include <vk/msg/enums.hpp>

#include <cstddef>
#include <optional>

namespace vk::local_display {

struct ArmingChecksPageInputs {
    std::optional<msg::ArmingChecks> checks;
    // The first line shown, 0 for the first check; clamped to the last page.
    std::size_t scroll = 0;
};

// The checks of `arming_result`: every value but OK, one line each; bit n of
// the topic's masks is the check of value n.
inline constexpr std::size_t arming_checks_line_count = msg::EnumValues<msg::ArmingResult>::values.size() - 1;
// How many of them fit the page.
inline constexpr std::size_t arming_checks_lines_fit = 6;

// The largest first line, so that the last page ends with the last check.
[[nodiscard]] constexpr std::size_t arming_checks_max_scroll()
{
    return arming_checks_line_count > arming_checks_lines_fit ? arming_checks_line_count - arming_checks_lines_fit : 0;
}

// Whether a click at `scroll` scrolls rather than moves on.
[[nodiscard]] constexpr bool arming_checks_can_scroll(std::size_t scroll)
{
    return scroll < arming_checks_max_scroll();
}

// The first line after a click that scrolls: a page further, at most the last.
[[nodiscard]] constexpr std::size_t arming_checks_scrolled(std::size_t scroll)
{
    const std::size_t next = scroll + arming_checks_lines_fit;
    return next < arming_checks_max_scroll() ? next : arming_checks_max_scroll();
}

// Draws the page into the 56 pixels below the status bar.
void draw_arming_checks_page(Canvas& canvas, const ArmingChecksPageInputs& inputs);

}  // namespace vk::local_display
