// SPDX-License-Identifier: GPL-3.0-or-later
// Reads float parameters from the MCU (ADR-0012, spec 0015).
#include <vehkernel_bridge/float_param_reader.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

namespace vk::bridge {

namespace {

constexpr std::uint8_t param_type_float = 2;  // ADR-0012: 1 int32, 2 float32
// A request sent this many times without any answer is worth saying once.
constexpr std::uint32_t unanswered_warning = 6;

std::string_view read_name(vk::link::BodyReader& reader)
{
    const std::uint8_t length = reader.u8();
    const auto bytes = reader.bytes(length);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

bool FloatParamReader::complete() const
{
    return std::all_of(settled_.begin(), settled_.end(), [](bool settled) { return settled; });
}

std::optional<std::size_t> FloatParamReader::index_of(std::string_view name) const
{
    const auto found = std::find(names_.begin(), names_.end(), name);
    if (found == names_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - names_.begin());
}

void FloatParamReader::restart(std::uint64_t now_us)
{
    std::fill(values_.begin(), values_.end(), std::nullopt);
    std::fill(settled_.begin(), settled_.end(), false);
    active_ = true;
    requests_ = 0;
    unanswered_ = 0;
    next_request_us_ = now_us;
    poll(now_us);
}

void FloatParamReader::request(const std::string& name)
{
    std::array<std::uint8_t, 32> body{};
    vk::link::BodyWriter writer(body);
    writer.u8(static_cast<std::uint8_t>(name.size()));
    writer.bytes({reinterpret_cast<const std::uint8_t*>(name.data()), name.size()});
    if (send_ && send_(vk::link::FrameType::ParamGet, writer.written())) {
        ++requests_;
    }
}

void FloatParamReader::poll(std::uint64_t now_us)
{
    if (!active_ || complete() || now_us < next_request_us_) {
        return;
    }
    next_request_us_ = now_us + retry_interval_us_;
    if (++unanswered_ == unanswered_warning) {
        report("the MCU does not answer the requests for " + names_.front() + " and its companions; still trying");
    }
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (!settled_[i]) {
            request(names_[i]);
        }
    }
}

bool FloatParamReader::on_frame(const vk::link::Frame& frame, std::uint64_t now_us)
{
    if (!active_) {
        return false;
    }
    if (frame.type == vk::link::FrameType::ParamValue) {
        vk::link::BodyReader reader(frame.body);
        (void)reader.u16();  // index
        (void)reader.u16();  // count
        const std::uint8_t type = reader.u8();
        const std::uint32_t bits = reader.u32();
        (void)reader.u8();  // flags
        const std::string_view name = read_name(reader);
        if (!reader.ok()) {
            return false;
        }
        const std::optional<std::size_t> index = index_of(name);
        if (!index.has_value()) {
            return false;
        }
        float value{};
        std::memcpy(&value, &bits, sizeof(value));
        settled_[*index] = true;
        if (type != param_type_float || !std::isfinite(value) || value < 0.0F) {
            report(std::string(name) + " is not a usable value; " + consequence_);
            values_[*index] = std::nullopt;
            return true;
        }
        values_[*index] = value;
        report(std::string("read ") + std::string(name) + " = " + std::to_string(value));
        unanswered_ = 0;
        next_request_us_ = now_us + retry_interval_us_;
        return true;
    }
    if (frame.type == vk::link::FrameType::ParamReject) {
        vk::link::BodyReader reader(frame.body);
        const std::uint8_t reason = reader.u8();
        const std::string_view name = read_name(reader);
        const std::optional<std::size_t> index = reader.ok() ? index_of(name) : std::nullopt;
        if (!index.has_value()) {
            return false;
        }
        report(std::string("the MCU refused to read ") + std::string(name) + " (reason " + std::to_string(reason) +
               "); " + consequence_);
        // Asking again would get the same answer: the build lacks the parameter.
        values_[*index] = std::nullopt;
        settled_[*index] = true;
        return true;
    }
    return false;
}

void FloatParamReader::report(const std::string& text) const
{
    if (log_) {
        log_(text);
    }
}

}  // namespace vk::bridge
