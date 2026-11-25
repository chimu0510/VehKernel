// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge's real clock: the monotonic clock of the companion computer (ADR-0002).
#pragma once

#include <vehkernel_bridge/transport.hpp>

#include <chrono>
#include <cstdint>

namespace vk::bridge {

class SteadyClock final : public Clock {
public:
    [[nodiscard]] std::uint64_t now_us() const override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
};

}  // namespace vk::bridge
