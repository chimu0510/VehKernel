// SPDX-License-Identifier: GPL-3.0-or-later
// The Link over UDP, as SITL offers it (ADR-0018, ADR-0027): one frame per datagram.
#pragma once

#include <vehkernel_bridge/transport.hpp>

#include <cstdint>
#include <string>

namespace vk::bridge {

class UdpTransport final : public Transport {
public:
    // SITL's default Link address.
    static constexpr std::uint16_t default_port = 15600;

    UdpTransport(std::string address, std::uint16_t port);
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;
    UdpTransport(UdpTransport&&) = delete;
    UdpTransport& operator=(UdpTransport&&) = delete;

    bool open() override;
    void close() override;
    std::ptrdiff_t read(std::span<std::uint8_t> out) override;
    bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] const char* description() const override { return description_.c_str(); }

private:
    std::string address_;
    std::uint16_t port_;
    std::string description_;
    int socket_ = -1;
};

}  // namespace vk::bridge
