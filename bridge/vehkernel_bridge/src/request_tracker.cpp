// SPDX-License-Identifier: GPL-3.0-or-later
// The mode, arming and emergency stop requests of the Bridge's services (ADR-0002,
// ADR-0005, spec 0015).
#include <vehkernel_bridge/request_tracker.hpp>

#include <vk/msg/enums.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace vk::bridge {

namespace {

using vk::link::BodyReader;
using vk::link::Frame;
using vk::link::FrameType;

constexpr FrameType request_type(Command command)
{
    switch (command) {
    case Command::Mode:
        return FrameType::ModeRequest;
    case Command::Arming:
        return FrameType::ArmingRequest;
    case Command::EmergencyStop:
        break;
    }
    return FrameType::EstopRequest;
}

constexpr const char* command_text(Command command)
{
    switch (command) {
    case Command::Mode:
        return "mode";
    case Command::Arming:
        return "arming";
    case Command::EmergencyStop:
        break;
    }
    return "emergency stop";
}

// "WRONG_MODE" as "wrong mode".
std::string words(std::string_view name)
{
    std::string text(name);
    std::replace(text.begin(), text.end(), '_', ' ');
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string mode_refusal(std::uint8_t reason)
{
    switch (static_cast<vk::msg::ModeResult>(reason)) {
    case vk::msg::ModeResult::Armed:
        return "the mode can be entered only while disarmed";
    case vk::msg::ModeResult::UnknownMode:
        return "the MCU does not know this mode";
    case vk::msg::ModeResult::WrongSource:
        return "this mode cannot be requested by this Link";
    case vk::msg::ModeResult::Ok:
        break;
    }
    return "reason " + std::to_string(reason);
}

std::string arming_refusal(std::uint8_t reason)
{
    const std::string_view name = vk::msg::enum_name(static_cast<vk::msg::ArmingResult>(reason));
    return name.empty() ? "reason " + std::to_string(reason) : words(name);
}

}  // namespace

bool RequestTracker::send(Command command, std::uint8_t argument)
{
    const std::array<std::uint8_t, 1> body{argument};
    return send_ && send_(request_type(command), body);
}

std::optional<std::uint8_t> RequestTracker::argument_of(Command command) const
{
    const auto& slot = slots_[index(command)];
    if (!slot.has_value()) {
        return std::nullopt;
    }
    return slot->argument;
}

void RequestTracker::finish(Command command, const Reply& reply)
{
    // Take `done` out first: it may submit again.
    auto slot = std::move(slots_[index(command)]);
    slots_[index(command)].reset();
    if (slot.has_value() && slot->done) {
        slot->done(reply);
    }
}

void RequestTracker::submit(Command command, std::uint8_t argument, Done done, std::uint64_t now_us)
{
    if (pending(command)) {
        done(Reply{Outcome::Busy, std::string("another ") + command_text(command) + " request is still waiting for its answer",
                   std::nullopt});
        return;
    }
    if (!send(command, argument)) {
        done(Reply{Outcome::NotSent, "the Link is not connected", std::nullopt});
        return;
    }
    slots_[index(command)] = Pending{argument, std::move(done), now_us + config_.timeout_us, now_us + config_.retry_interval_us};
}

void RequestTracker::poll(std::uint64_t now_us)
{
    for (const Command command : {Command::Mode, Command::Arming, Command::EmergencyStop}) {
        auto& slot = slots_[index(command)];
        if (!slot.has_value()) {
            continue;
        }
        if (now_us >= slot->deadline_us) {
            finish(command, Reply{Outcome::TimedOut, std::string("no answer from the MCU to the ") + command_text(command) + " request",
                                  std::nullopt});
        } else if (now_us >= slot->next_send_us) {
            slot->next_send_us = now_us + config_.retry_interval_us;
            send(command, slot->argument);
        }
    }
}

bool RequestTracker::on_frame(const Frame& frame)
{
    BodyReader reader(frame.body);
    switch (frame.type) {
    case FrameType::ModeReply: {
        const std::uint8_t mode = reader.u8();
        const std::optional<std::uint8_t> asked = argument_of(Command::Mode);
        if (reader.ok() && asked.has_value()) {
            const std::uint8_t wanted = *asked;
            finish(Command::Mode, mode == wanted ? Reply{Outcome::Accepted, {}, mode}
                                                 : Reply{Outcome::Refused,
                                                         "the vehicle is in mode " + mode_name(mode) + ", not " + mode_name(wanted),
                                                         mode});
        }
        return true;
    }
    case FrameType::ModeReject: {
        const std::uint8_t reason = reader.u8();
        const std::uint8_t mode = reader.u8();
        if (reader.ok() && pending(Command::Mode)) {
            finish(Command::Mode, Reply{Outcome::Refused, "mode change refused: " + mode_refusal(reason), mode});
        }
        return true;
    }
    case FrameType::ArmingReply: {
        const std::uint8_t armed = reader.u8();
        (void)reader.u32();  // the arming checks skipped
        const std::optional<std::uint8_t> asked = argument_of(Command::Arming);
        if (reader.ok() && asked.has_value()) {
            const bool matches = (armed != 0) == (*asked != 0);
            finish(Command::Arming, matches ? Reply{Outcome::Accepted, {}, armed}
                                            : Reply{Outcome::Refused, armed != 0 ? "the vehicle is armed" : "the vehicle is disarmed", armed});
        }
        return true;
    }
    case FrameType::ArmingReject: {
        const std::uint8_t reason = reader.u8();
        if (reader.ok() && pending(Command::Arming)) {
            finish(Command::Arming, Reply{Outcome::Refused, "arming refused: " + arming_refusal(reason), std::nullopt});
        }
        return true;
    }
    case FrameType::EstopReply: {
        const std::uint8_t engaged = reader.u8();
        const std::optional<std::uint8_t> asked = argument_of(Command::EmergencyStop);
        if (reader.ok() && asked.has_value()) {
            const bool matches = (engaged != 0) == (*asked != 0);
            finish(Command::EmergencyStop,
                   matches ? Reply{Outcome::Accepted, {}, engaged}
                           : Reply{Outcome::Refused, engaged != 0 ? "the emergency stop is engaged" : "the emergency stop is released", engaged});
        }
        return true;
    }
    default:
        return false;
    }
}

void RequestTracker::fail_all()
{
    for (const Command command : {Command::Mode, Command::Arming, Command::EmergencyStop}) {
        if (pending(command)) {
            finish(command, Reply{Outcome::LinkLost, "the Link went down before the MCU answered", std::nullopt});
        }
    }
}

std::string mode_name(std::uint8_t value)
{
    const std::string_view name = vk::msg::enum_name(static_cast<vk::msg::Mode>(value));
    return name.empty() ? "mode " + std::to_string(value) : std::string(name);
}

ModeChoice resolve_mode(std::string_view name, std::int32_t value)
{
    std::optional<std::uint8_t> by_name;
    if (!name.empty()) {
        for (const auto& entry : vk::msg::EnumValues<vk::msg::Mode>::values) {
            const bool same = std::equal(name.begin(), name.end(), entry.name.begin(), entry.name.end(),
                                         [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); });
            if (same) {
                by_name = static_cast<std::uint8_t>(entry.value);
            }
        }
        if (!by_name.has_value()) {
            return {std::nullopt, "unknown mode name '" + std::string(name) + "'"};
        }
    }
    std::optional<std::uint8_t> by_value;
    if (value != -1) {
        for (const auto& entry : vk::msg::EnumValues<vk::msg::Mode>::values) {
            if (static_cast<std::int32_t>(entry.value) == value) {
                by_value = static_cast<std::uint8_t>(entry.value);
            }
        }
        if (!by_value.has_value()) {
            return {std::nullopt, "unknown mode value " + std::to_string(value)};
        }
    }
    if (by_name.has_value() && by_value.has_value() && *by_name != *by_value) {
        return {std::nullopt, "mode_name " + std::string(name) + " and mode_value " + std::to_string(value) + " name different modes"};
    }
    if (by_name.has_value()) {
        return {by_name, {}};
    }
    if (by_value.has_value()) {
        return {by_value, {}};
    }
    return {std::nullopt, "give the mode by mode_name or by mode_value"};
}

}  // namespace vk::bridge
