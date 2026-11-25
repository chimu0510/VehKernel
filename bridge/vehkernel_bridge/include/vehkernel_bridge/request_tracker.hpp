// SPDX-License-Identifier: GPL-3.0-or-later
// The mode, arming and emergency stop requests of the Bridge's services (ADR-0002,
// ADR-0005, ADR-0027, spec 0015): each is sent, matched with its acknowledgement
// (mode_reply or mode_reject, arming_reply or arming_reject, estop_reply), sent
// again while none has come, and ended with the MCU's reason when refused or with
// a timeout. The requests are idempotent (ADR-0005), which is why a repeat is safe.
//
// It never waits: submit() returns at once and `done` is called from on_frame() or
// poll() when the request ends, so the node completes its service response then and
// its executor is not blocked. Free of rclcpp: it writes frames through a function
// and reads the clock it is given, so a test drives it with fakes.
#pragma once

#include <vk/link/frame.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vk::bridge {

enum class Command : std::uint8_t { Mode, Arming, EmergencyStop };

enum class Outcome : std::uint8_t {
    Accepted,   // the MCU did what was asked
    Refused,    // the MCU answered and did not
    TimedOut,   // no answer in time
    NotSent,    // not connected, or the protocol version lacks the frame
    Busy,       // a request of the same kind is still waiting for its answer
    LinkLost,   // the connection ended while waiting
};

struct Reply {
    Outcome outcome = Outcome::TimedOut;
    // In a sentence, empty when accepted: the MCU's reason when it refused.
    std::string reason;
    // The mode, the armed state or the emergency stop state the MCU reports; unset for
    // an outcome without an answer.
    std::optional<std::uint8_t> state;

    [[nodiscard]] bool accepted() const { return outcome == Outcome::Accepted; }
};

struct TrackerConfig {
    // A request goes again after this long without an answer (the host tools use
    // 0.25 s too).
    std::uint64_t retry_interval_us = 250'000;
    std::uint64_t timeout_us = 2'000'000;
};

class RequestTracker {
public:
    // Sends a frame to the MCU; false when it could not be sent.
    using Send = std::function<bool(vk::link::FrameType, std::span<const std::uint8_t>)>;
    using Done = std::function<void(const Reply&)>;

    explicit RequestTracker(Send send, TrackerConfig config = {}) : send_(std::move(send)), config_(config) {}

    // Sends the request: `argument` is the mode, 1 to arm and 0 to disarm, or 1 to engage
    // the emergency stop and 0 to release it. `done` is called exactly once, possibly
    // before submit() returns, when the request cannot be sent or is already pending.
    void submit(Command command, std::uint8_t argument, Done done, std::uint64_t now_us);

    // Sends requests again and ends those that ran out of time; call it every few milliseconds.
    void poll(std::uint64_t now_us);

    // Consumes the acknowledgement frames; false for any other frame. An acknowledgement
    // nothing waits for, a late repeat, is consumed and dropped.
    bool on_frame(const vk::link::Frame& frame);

    // The connection ended: every pending request ends with LinkLost.
    void fail_all();

    [[nodiscard]] bool pending(Command command) const { return slots_[index(command)].has_value(); }

private:
    struct Pending {
        std::uint8_t argument;
        Done done;
        std::uint64_t deadline_us;
        std::uint64_t next_send_us;
    };

    static constexpr std::size_t index(Command command) { return static_cast<std::size_t>(command); }
    bool send(Command command, std::uint8_t argument);
    // The argument of the pending request of `command`, empty when none is pending.
    [[nodiscard]] std::optional<std::uint8_t> argument_of(Command command) const;
    void finish(Command command, const Reply& reply);

    Send send_;
    TrackerConfig config_;
    std::array<std::optional<Pending>, 3> slots_;
};

// The result of resolving a mode request given by name or by value.
struct ModeChoice {
    std::optional<std::uint8_t> mode;
    // Why there is no mode, in a sentence.
    std::string error;
};

// Resolves the mode of a `SetMode` request against the schema's mode enum: `name`
// in any letter case, or `value` when the name is empty (-1 for none). Both given
// must agree.
[[nodiscard]] ModeChoice resolve_mode(std::string_view name, std::int32_t value);

// The name of a mode value as the schema has it, such as "COMPANION"; "mode 9" for
// one it does not name.
[[nodiscard]] std::string mode_name(std::uint8_t value);

}  // namespace vk::bridge
