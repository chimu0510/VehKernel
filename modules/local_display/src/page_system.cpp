// SPDX-License-Identifier: GPL-3.0-or-later
#include <vk/local_display/page_system.hpp>

#include <vk/local_display/page_text.hpp>
#include <vk/local_display/text_buffer.hpp>
#include <vk/msg/enums.hpp>

#include <algorithm>
#include <cstdint>

namespace vk::local_display {

std::optional<std::size_t> worst_work_item(std::span<const std::optional<msg::WorkItemStatus>> items)
{
    std::optional<std::size_t> worst;
    const msg::WorkItemStatus* worst_item = nullptr;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (!item) {
            continue;
        }
        if (worst_item == nullptr || (item->overruns != worst_item->overruns ? item->overruns > worst_item->overruns
                                                                              : item->run_time_max_us > worst_item->run_time_max_us)) {
            worst = index;
            worst_item = &*item;
        }
    }
    return worst;
}

std::string_view work_item_name(const msg::WorkItemStatus& item)
{
    const auto* begin = reinterpret_cast<const char*>(item.name.data());
    std::size_t size = 0;
    while (size < item.name.size() && begin[size] != '\0') {
        ++size;
    }
    return {begin, size};
}

void draw_system_page(Canvas& canvas, const SystemPageInputs& inputs)
{
    using TB = TextBuffer;

    TB firmware;
    firmware.add("FW ");
    if (inputs.firmware) {
        const auto& info = *inputs.firmware;
        firmware.add(info.version_major).add(".").add(info.version_minor).add(".").add(info.version_patch).add(" ");
        if (info.git_hash != 0) {
            page::add_hex32(firmware, info.git_hash);
        } else {
            firmware.add("--------");
        }
        if (info.git_dirty) {
            firmware.add("*");
        }
    } else {
        firmware.add("?");
    }
    page::draw_row(canvas, 0, firmware.view());

    TB board;
    board.add("BRD ");
    if (inputs.firmware) {
        board.add(inputs.firmware->board_id).add(" ").add(inputs.board_name);
    } else {
        board.add("?");
    }
    page::draw_row(canvas, 1, board.view());

    const std::uint64_t seconds = inputs.uptime_us / 1'000'000;
    TB uptime;
    uptime.add("UP ");
    if (seconds >= 86400) {
        uptime.add(static_cast<std::uint32_t>(seconds / 86400)).add("D ");
    }
    page::add_padded(uptime, (seconds % 86400) / 3600, 2).add(":");
    page::add_padded(uptime, (seconds % 3600) / 60, 2).add(":");
    page::add_padded(uptime, seconds % 60, 2);
    page::draw_row(canvas, 2, uptime.view());

    TB reset;
    if (!inputs.status) {
        reset.add("RST ?");
    } else if (inputs.status->fault != static_cast<std::uint8_t>(msg::Fault::None)) {
        const std::string_view fault = msg::enum_name(static_cast<msg::Fault>(inputs.status->fault));
        reset.add("FAULT ").add(fault.empty() ? "?" : fault);
    } else {
        const std::string_view cause = msg::enum_short_name(static_cast<msg::ResetCause>(inputs.status->reset_cause));
        reset.add("RST ").add(cause.empty() ? "?" : cause);
    }
    page::draw_row(canvas, 3, reset.view());

    TB timing;
    const auto worst = worst_work_item(inputs.work_items);
    const auto& slot = inputs.work_items.at(worst.value_or(0));
    if (!worst || !slot) {
        timing.add("WORK NO DATA");
    } else {
        timing.add(work_item_name(*slot)).add(" ");
        if (slot->overruns != 0) {
            timing.add("OVR ").add(slot->overruns);
        } else {
            timing.add_fixed(static_cast<float>(slot->run_time_max_us) / 1000.0F, 1).add("MS");
        }
    }
    page::draw_row(canvas, 4, timing.view());

    TB parameters;
    parameters.add("PARAM ");
    if (inputs.parameters) {
        const auto& status = *inputs.parameters;
        parameters.add(status.parameter_count).pad_to(11).add("STORE ");
        const std::uint32_t percent =
            status.sector_size != 0
                ? static_cast<std::uint32_t>(std::min<std::uint64_t>(100, (static_cast<std::uint64_t>(status.bytes_used) * 100) / status.sector_size))
                : 0;
        parameters.add(percent).add("%");
    } else {
        parameters.add("?");
    }
    page::draw_row(canvas, 5, parameters.view());
}

}  // namespace vk::local_display
