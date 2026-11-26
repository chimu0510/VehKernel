// SPDX-License-Identifier: GPL-3.0-or-later
// The page dispatcher of spec 0016 and ADR-0022.
#include <vk/local_display/pages.hpp>

#include <vk/local_display/page_attitude.hpp>
#include <vk/local_display/page_motion.hpp>
#include <vk/local_display/page_overview.hpp>
#include <vk/local_display/status_bar.hpp>

#include <cstddef>
#include <string_view>

namespace vk::local_display {

Page next_page(Page page)
{
    return static_cast<Page>((static_cast<std::size_t>(page) + 1) % page_count);
}

std::string_view page_name(Page page)
{
    switch (page) {
    case Page::Overview:
        return "OVERVIEW";
    case Page::Attitude:
        return "ATTITUDE";
    case Page::Motion:
        return "MOTION";
    case Page::ArmingChecks:
        return "ARMING CHECKS";
    case Page::Sensors:
        return "SENSORS";
    case Page::Links:
        return "LINKS";
    case Page::RemoteControl:
        return "REMOTE CONTROL";
    case Page::System:
        return "SYSTEM";
    }
    return "?";
}

void placeholder_page(Canvas& canvas, Page page)
{
    const std::string_view name = page_name(page);
    const int x = (static_cast<int>(drivers::MonoFrame::width) - text_width(name.size())) / 2;
    const int y = page_top + ((page_last - page_top + 1 - 7) / 2);
    canvas.text(x, y, name);
}

void render(Page page, const Inputs& inputs, Canvas& canvas)
{
    canvas.clear();
    switch (page) {
    case Page::Overview:
        draw_overview_page(canvas, {.status = inputs.status, .attitude = inputs.attitude, .odometry = inputs.odometry});
        break;
    case Page::Attitude:
        draw_attitude_page(canvas, {.attitude = inputs.attitude});
        break;
    case Page::Motion:
        draw_motion_page(canvas, {.odometry = inputs.odometry,
                                  .encoders = inputs.encoders,
                                  .outputs = inputs.outputs,
                                  .frame = inputs.frame});
        break;
    // Pages 4 to 8 are drawn by their own tickets; until then, a placeholder.
    case Page::ArmingChecks:
    case Page::Sensors:
    case Page::Links:
    case Page::RemoteControl:
    case Page::System:
        placeholder_page(canvas, page);
        break;
    }
    draw_status_bar(canvas, {.status = inputs.status, .links = inputs.links, .manual = inputs.manual, .page = page});
}

}  // namespace vk::local_display
