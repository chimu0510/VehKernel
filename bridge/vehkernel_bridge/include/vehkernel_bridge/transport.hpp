// SPDX-License-Identifier: GPL-3.0-or-later
// The Bridge's byte transport and clock, the two seams of the session (ADR-0027,
// spec 0015 seam D): the session talks only to these, so a test drives it with a
// fake transport and a fake clock, and the serial and UDP transports stay thin.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vk::bridge {

// A monotonic clock in microseconds. Time synchronization (ADR-0002) and every
// timeout of the session read it, and nothing else.
class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    [[nodiscard]] virtual std::uint64_t now_us() const = 0;
};

// The result of reading from a transport.
inline constexpr std::ptrdiff_t transport_lost = -1;

// A non-blocking byte stream to one Link peer: a serial device, or a UDP socket
// to SITL. The session opens it again after it was lost.
class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    // Opens the device or the socket; false when it cannot be, for example when the
    // device is not plugged in. Opening an open transport closes it first.
    virtual bool open() = 0;
    virtual void close() = 0;

    // Reads what is there without waiting: the bytes read, 0 for none, and
    // transport_lost when the device is gone, so that the session opens it anew.
    virtual std::ptrdiff_t read(std::span<std::uint8_t> out) = 0;

    // Writes `data`; false only when the transport is lost. Bytes it cannot take
    // now are dropped, which the Link tolerates: its frames carry a CRC and a number.
    virtual bool write(std::span<const std::uint8_t> data) = 0;

    // What the log says about this transport, such as "serial /dev/ttyACM0 at 921600".
    [[nodiscard]] virtual const char* description() const = 0;
};

}  // namespace vk::bridge
