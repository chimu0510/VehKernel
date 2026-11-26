// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/drivers/ssd1306.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace vk::drivers {

namespace {

// The vendor firmware's initialisation (WHEELTEC_C50X 2026.05.29, oled.c,
// OLED_Init), commands with their arguments: display off; clock 0x80; 64
// rows; no offset; start line 0; charge pump on; page addressing; segment
// remap; normal COM scan; alternative COM pins; contrast 0xEF; pre-charge
// 0xF1; VCOMH 0x30; follow RAM; not inverted; display on.
constexpr std::array<std::uint8_t, 25> init_sequence{
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x02, 0xA1,
    0xC0, 0xDA, 0x12, 0x81, 0xEF, 0xD9, 0xF1, 0xDB, 0x30, 0xA4, 0xA6, 0xAF};

std::uint8_t reverse_bits(std::uint8_t byte)
{
    std::uint8_t out = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        out = static_cast<std::uint8_t>((out << 1U) | ((byte >> bit) & 1U));
    }
    return out;
}

}  // namespace

void MonoFrame::set(std::size_t x, std::size_t y, bool lit)
{
    if (x >= width || y >= height) {
        return;
    }
    auto& byte = bytes.at(y / 8).at(x);
    const auto mask = static_cast<std::uint8_t>(1U << (y % 8));
    byte = static_cast<std::uint8_t>(lit ? (byte | mask) : (byte & ~mask));
}

bool MonoFrame::get(std::size_t x, std::size_t y) const
{
    if (x >= width || y >= height) {
        return false;
    }
    return ((bytes.at(y / 8).at(x) >> (y % 8)) & 1U) != 0;
}

std::size_t Ssd1306::update(std::uint64_t now_us, std::size_t max_pages)
{
    if (state_ == State::Idle) {
        device_.set_reset(true);
        reset_until_us_ = now_us + reset_time_us;
        state_ = State::Resetting;
        return 0;
    }
    if (state_ == State::Resetting) {
        if (now_us < reset_until_us_) {
            return 0;
        }
        device_.set_reset(false);
        initialise();
        state_ = State::Ready;
    }
    if (settings_changed_) {
        apply_settings();
    }
    std::size_t written = 0;
    for (std::size_t scanned = 0; scanned < MonoFrame::pages && written < max_pages; ++scanned) {
        const std::size_t page = next_page_;
        next_page_ = (next_page_ + 1) % MonoFrame::pages;
        if (stale_.at(page) || shifted(page) != shown_.bytes.at(page)) {
            write_page(page);
            ++written;
        }
    }
    return written;
}

void Ssd1306::initialise()
{
    device_.write_commands(init_sequence);
    // The sequence leaves the vendor's contrast and the display on.
    settings_changed_ = contrast_ != default_contrast || !display_on_;
    // The RAM holds whatever it held before the reset.
    stale_.fill(true);
    next_page_ = 0;
}

void Ssd1306::write_page(std::size_t page)
{
    // The vendor firmware's coordinates put frame row y at RAM row 63 - y:
    // frame page p is RAM page 7 - p with its bits reversed, in the same columns.
    const auto ram_page = static_cast<std::uint8_t>(MonoFrame::pages - 1 - page);
    const std::array<std::uint8_t, 3> address{static_cast<std::uint8_t>(0xB0U | ram_page), 0x00, 0x10};
    device_.write_commands(address);
    std::array<std::uint8_t, MonoFrame::width> data{};
    const auto source = shifted(page);
    for (std::size_t column = 0; column < MonoFrame::width; ++column) {
        data.at(column) = reverse_bits(source.at(column));
    }
    device_.write_data(data);
    shown_.bytes.at(page) = source;
    stale_.at(page) = false;
}

void Ssd1306::set_contrast(std::uint8_t contrast)
{
    if (contrast != contrast_) {
        contrast_ = contrast;
        settings_changed_ = true;
    }
}

void Ssd1306::set_display_on(bool on)
{
    if (on != display_on_) {
        display_on_ = on;
        settings_changed_ = true;
    }
}

void Ssd1306::set_offset(int dx, int dy)
{
    dx = std::clamp(dx, -max_offset, max_offset);
    dy = std::clamp(dy, -max_offset, max_offset);
    if (dx != offset_x_ || dy != offset_y_) {
        offset_x_ = dx;
        offset_y_ = dy;
        // Every page changes; the next update() writes them as it scans.
    }
}

void Ssd1306::apply_settings()
{
    // 0x81 is the contrast command, 0xAE and 0xAF turn the display off and on.
    const std::array<std::uint8_t, 3> commands{0x81, contrast_, static_cast<std::uint8_t>(display_on_ ? 0xAF : 0xAE)};
    device_.write_commands(commands);
    settings_changed_ = false;
}

std::array<std::uint8_t, MonoFrame::width> Ssd1306::shifted(std::size_t page) const
{
    const auto& current = frame_.bytes.at(page);
    if (offset_x_ == 0 && offset_y_ == 0) {
        return current;
    }
    // Rows: this page's bits come from `dy` rows higher in the frame, which
    // spans this page and the one above (dy > 0) or below (dy < 0).
    const std::array<std::uint8_t, MonoFrame::width> zeros{};
    const auto& above = page > 0 ? frame_.bytes.at(page - 1) : zeros;
    const auto& below = page + 1 < MonoFrame::pages ? frame_.bytes.at(page + 1) : zeros;
    std::array<std::uint8_t, MonoFrame::width> out{};
    constexpr int columns = static_cast<int>(MonoFrame::width);
    for (int column = 0; column < columns; ++column) {
        const int source = column - offset_x_;
        if (source < 0 || source >= columns) {
            continue;
        }
        const auto s = static_cast<std::size_t>(source);
        unsigned bits = 0;
        if (offset_y_ >= 0) {
            const auto dy = static_cast<unsigned>(offset_y_);
            bits = (static_cast<unsigned>(current.at(s)) << dy) | (static_cast<unsigned>(above.at(s)) >> (8U - dy));
        } else {
            const auto dy = static_cast<unsigned>(-offset_y_);
            bits = (static_cast<unsigned>(current.at(s)) >> dy) | (static_cast<unsigned>(below.at(s)) << (8U - dy));
        }
        out.at(static_cast<std::size_t>(column)) = static_cast<std::uint8_t>(bits & 0xFFU);
    }
    return out;
}

}  // namespace vk::drivers
