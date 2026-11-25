// SPDX-License-Identifier: GPL-3.0-or-later
// The topics this build of the Bridge knows, with their schema hashes and whether
// each is `@required` (ADR-0006), taken from the generated headers.
#pragma once

#include <vk/msg/topics.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace vk::bridge {

struct LocalTopic {
    std::string_view name;
    std::uint64_t schema_hash;
    bool required;
};

namespace detail {

template <class... Messages>
constexpr auto make_local_topics(vk::msg::TopicList<Messages...> /*list*/)
{
    return std::array<LocalTopic, sizeof...(Messages)>{
        LocalTopic{Messages::topic_name, Messages::schema_hash, Messages::required}...};
}

}  // namespace detail

inline constexpr auto local_topics = detail::make_local_topics(vk::msg::AllTopics{});

}  // namespace vk::bridge
