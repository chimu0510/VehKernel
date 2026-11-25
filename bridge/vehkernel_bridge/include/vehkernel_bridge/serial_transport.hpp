// SPDX-License-Identifier: GPL-3.0-or-later
// The Link over a serial device, raw 8N1 through termios (ADR-0002, ADR-0027).
#pragma once

#include <vehkernel_bridge/transport.hpp>

#include <string>

namespace vk::bridge {

class SerialTransport final : public Transport {
public:
    static constexpr int default_baud = 921600;

    SerialTransport(std::string device, int baud);
    ~SerialTransport() override;
    SerialTransport(const SerialTransport&) = delete;
    SerialTransport& operator=(const SerialTransport&) = delete;
    SerialTransport(SerialTransport&&) = delete;
    SerialTransport& operator=(SerialTransport&&) = delete;

    // Whether termios has a speed constant for this baud rate.
    [[nodiscard]] static bool baud_supported(int baud);

    bool open() override;
    void close() override;
    std::ptrdiff_t read(std::span<std::uint8_t> out) override;
    bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] const char* description() const override { return description_.c_str(); }

private:
    std::string device_;
    int baud_;
    std::string description_;
    int fd_ = -1;
};

}  // namespace vk::bridge
