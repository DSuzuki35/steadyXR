#pragma once
#include "vrst/core.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vrst::wire {
constexpr std::uint16_t default_port = 42424;
enum class Type : std::uint16_t { sync_request = 1, sync_reply = 2, pose = 3 };
struct Message {
    Type type{Type::pose};
    std::uint64_t nonce{}, client_send{}, server_receive{}, server_send{}, session{};
    Pose pose{};
};
std::vector<std::uint8_t> encode(const Message& message);
std::optional<Message> decode(const std::uint8_t* data, std::size_t length);
} // namespace vrst::wire
