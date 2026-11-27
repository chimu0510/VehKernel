// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/page_links.hpp>

#include <vk/core/vehicle_control.hpp>
#include <vk/local_display/icons.hpp>
#include <vk/local_display/page_text.hpp>
#include <vk/local_display/text_buffer.hpp>
#include <vk/msg/enums.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vk::local_display {

namespace {

constexpr int marker_x = page::left;
constexpr int name_x = page::left + 7;
constexpr int icon_x = name_x + 7 * 6 + 2;
constexpr int state_x = icon_x + 10;
// The state text is at most "SILENT" (6 cells), so `CMP` starts after a gap.
constexpr int companion_x = state_x + 7 * 6;
constexpr int detail_x = name_x;

// A filled triangle pointing right, 4 pixels wide and 7 high, at (x, y).
void draw_in_control_marker(Canvas& canvas, int x, int y)
{
    canvas.vline(x, y, 7);
    canvas.vline(x + 1, y + 1, 5);
    canvas.vline(x + 2, y + 2, 3);
    canvas.pixel(x + 3, y + 3);
}

// The Link's handshake state as the first row shows it.
std::string_view handshake_text(std::uint8_t state)
{
    switch (static_cast<msg::HandshakeState>(state)) {
        case msg::HandshakeState::Connected:
            return "UP";
        case msg::HandshakeState::PeerSilent:
            return "SILENT";
        default:
            return "WAIT";
    }
}

// "RX <frames> RATE <n>%". When both do not fit, the rate stays whole at the
// right end and the frame count is cut, so the rate is never the part lost.
void draw_receive_row(Canvas& canvas, std::size_t row, std::uint32_t frames, std::uint8_t rate_scale)
{
    const auto room = static_cast<std::size_t>((page::right + 1 - detail_x) / 6);
    const TextBuffer received = TextBuffer().add("RX ").add(frames);
    const TextBuffer rate = TextBuffer().add("RATE ").add(static_cast<std::uint32_t>(rate_scale)).add("%");
    const std::size_t received_room = room - rate.view().size() - 1;
    if (received.view().size() <= received_room) {
        page::draw_row(canvas, row, TextBuffer().add(received.view()).add(" ").add(rate.view()).view(), detail_x);
        return;
    }
    page::draw_row(canvas, row, received.view().substr(0, received_room), detail_x);
    page::draw_row(canvas, row, rate.view(), detail_x + static_cast<int>(room - rate.view().size()) * 6);
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
            canvas.text(state_x, y, "NONE");
            continue;
        }
        const bool up = link->handshake_state == static_cast<std::uint8_t>(msg::HandshakeState::Connected);
        canvas.bitmap(icon_x, y, icon(up ? Icon::LinkUp : Icon::LinkDown));
        canvas.text(state_x, y, handshake_text(link->handshake_state));
        if (in_control && companion) {
            canvas.text(companion_x, y, "CMP");
        }
        page::draw_row(canvas, first + 1, TB().add("DROP ").add(link->drops).add(" LOST ").add(link->lost).view(), detail_x);
        draw_receive_row(canvas, first + 2, link->frames_received, link->rate_scale);
    }
}

}  // namespace vk::local_display
