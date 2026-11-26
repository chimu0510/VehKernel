// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's system page (spec 0016, ADR-0022, ADR-0023): the
// firmware's version and commit with a `*` for a dirty build, the board's ID
// and name, the uptime, the reset cause and fault, the work item with the
// longest run or any overrun, and the parameter count and storage use.
#pragma once

#include <vk/local_display/graphics.hpp>
#include <vk/msg/firmware_info.hpp>
#include <vk/msg/parameter_status.hpp>
#include <vk/msg/vehicle_status.hpp>
#include <vk/msg/work_item_status.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace vk::local_display {

struct SystemPageInputs {
    std::optional<msg::FirmwareInfo> firmware;
    // The reset cause and the fault.
    std::optional<msg::VehicleStatus> status;
    std::optional<msg::ParameterStatus> parameters;
    // The latest work_item_status of each instance of the topic.
    std::array<std::optional<msg::WorkItemStatus>, msg::WorkItemStatus::instances> work_items;
    // The time since boot, from the clock the timestamps use.
    std::uint64_t uptime_us = 0;
    // The board's name; firmware_info carries only its ID.
    std::string_view board_name;
};

// The index of the work item to show: the one with the most overruns when any
// overran, else the one with the longest run; nothing if none is known. A tie
// goes to the longest run, then to the lowest index.
[[nodiscard]] std::optional<std::size_t> worst_work_item(std::span<const std::optional<msg::WorkItemStatus>> items);

// A work item's name: its characters up to the first zero.
[[nodiscard]] std::string_view work_item_name(const msg::WorkItemStatus& item);

// Draws the page into the 56 pixels below the status bar.
void draw_system_page(Canvas& canvas, const SystemPageInputs& inputs);

}  // namespace vk::local_display
