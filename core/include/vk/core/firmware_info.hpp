// SPDX-License-Identifier: GPL-3.0-or-later
// The running firmware's identity (ADR-0023): the entry point sets it once
// before the scheduler starts, and the timing monitor publishes it on the
// firmware_info topic once a second.
#pragma once

#include <vk/msg/firmware_info.hpp>

#include <optional>

namespace vk::core {

// Sets the identity to publish; its timestamp is set at each publication.
void set_firmware_info(const msg::FirmwareInfo& info);

// The identity set, or none before set_firmware_info.
[[nodiscard]] std::optional<msg::FirmwareInfo> firmware_info();

}  // namespace vk::core
