// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/page_links.hpp>

#include <vk/core/vehicle_control.hpp>
#include <vk/local_display/icons.hpp>
#include <vk/local_display/page_text.hpp>
#include <vk/local_display/text_buffer.hpp>
#include <vk/msg/enums.hpp>

#include <cstdint>

namespace vk::local_display {

namespace {

constexpr int marker_x = page::left;
constexpr int name_x = page::left + 7;
constexpr int icon_x = name_x + 7 * 6 + 2;
constexpr int scale_x = icon_x + 10;
constexpr int companion_x = scale_x + 5 * 6;
constexpr int detail_x = name_x;

// A filled triangle pointing right, 4 pixels wide and 7 high, at (x, y).
void draw_in_control_marker(Canvas& canvas, int x, int y)
{
    canvas.vline(x, y, 7);
    canvas.vline(x + 1, y + 1, 5);
    canvas.vline(x + 2, y + 2, 3);
    canvas.pixel(x + 3, y + 3);
}

}  // namespace

void draw_links_page(Canvas& canvas, const LinksPageInputs& inputs)
{
    using TB = TextBuffer;
    const bool companion = inputs.status && inputs.status->mode == static_cast<std::uint8_t>(msg::Mode::Companion);
    for (std::size_t instance = 0; instance < inputs.links.size(); ++instance) {
        const std::size_t first = instance * 3;
        const int y = page::row_y(first);
        const bool in_control = inputs.status && core::is_link(inputs.status->source) && inputs.status->source == instance;
        if (in_control) {
            draw_in_control_marker(canvas, marker_x, y);
        }
        canvas.text(name_x, y, inputs.port_names.at(instance).substr(0, 7));
        const auto& link = inputs.links.at(instance);
        if (!link) {
            canvas.bitmap(icon_x, y, icon(Icon::LinkDown));
            canvas.text(scale_x, y, "NONE");
            continue;
        }
        const bool up = link->handshake_state == static_cast<std::uint8_t>(msg::HandshakeState::Connected);
        canvas.bitmap(icon_x, y, icon(up ? Icon::LinkUp : Icon::LinkDown));
        canvas.text(scale_x, y, TB().add(static_cast<std::uint32_t>(link->rate_scale)).add("%").view());
        if (in_control && companion) {
            canvas.text(companion_x, y, "CMP");
        }
        page::draw_row(canvas, first + 1, TB().add("DROP ").add(link->drops).add(" LOST ").add(link->lost).view(), detail_x);
        page::draw_row(canvas, first + 2, TB().add("RX ").add(link->frames_received).view(), detail_x);
    }
}

}  // namespace vk::local_display
