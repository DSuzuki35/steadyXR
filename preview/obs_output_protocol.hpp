// SPDX-License-Identifier: MIT
// Copyright (c) 2026 SteadyXR contributors
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace steadyxr::obs_output {
inline constexpr wchar_t mapping_name[] = L"Local\\SteadyXR.ObsOutput.v1";
inline constexpr wchar_t mutex_name[] = L"Local\\SteadyXR.ObsOutput.v1.Lock";
inline constexpr std::uint32_t magic = 0x53584F42; // SXOB
inline constexpr std::uint32_t protocol_version = 1;
inline constexpr std::uint32_t flag_active = 1;
inline constexpr std::uint64_t stale_after_ms = 3000;
inline constexpr std::uint64_t producer_key = 0;
inline constexpr std::uint64_t consumer_key = 1;
// Latest-frame broadcast: producer first tries key 0, then key 1 (both timeout
// zero) to replace an unread frame. Consumers acquire key 1, copy to a private
// GPU texture, flush, and release key 1 so other sources can read a paused frame.
// Legacy single consumers releasing key 0 remain compatible. Never write while
// another device holds the keyed mutex; do not use infinite waits.

// Local-session IPC, little-endian Windows x64. Guard EVERY metadata read and
// write with the named mutex, using timeout zero on rendering threads. After
// WAIT_ABANDONED the producer must replace the entire record before publishing;
// consumers reject the record. No pointers, HANDLE typedefs, bools or size_t.
// The mapping owner remains alive for the output session; it clears active on
// stop. epoch changes on EVERY producer/session start (e.g. QPC counter).
// generation changes whenever the texture is replaced. frame_sequence counts
// successful rendered frames, heartbeat_ms is GetTickCount64 even while idle.
struct alignas(8) Metadata {
    std::uint32_t signature{magic};
    std::uint32_t version{protocol_version};
    std::uint32_t struct_size{sizeof(Metadata)};
    std::uint32_t flags{};
    std::uint32_t producer_pid{};
    std::uint32_t reserved{};
    std::uint64_t epoch{};
    std::uint64_t generation{};
    std::uint64_t shared_handle{}; // Legacy IDXGIResource::GetSharedHandle (NOT NT handle).
    std::uint64_t frame_sequence{};
    std::uint64_t heartbeat_ms{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t adapter_luid_low{};
    std::int32_t adapter_luid_high{};
};
static_assert(std::is_standard_layout_v<Metadata> && std::is_trivially_copyable_v<Metadata>);
static_assert(sizeof(Metadata) == 80);
static_assert(offsetof(Metadata, epoch) == 24);
static_assert(offsetof(Metadata, width) == 64);

inline bool valid(const Metadata &m, std::uint64_t now_ms) {
    return m.signature == magic && m.version == protocol_version && m.struct_size == sizeof(Metadata) &&
           (m.flags & flag_active) && m.producer_pid && m.epoch && m.generation && m.shared_handle &&
           m.shared_handle <= UINT32_MAX && m.width >= 1 && m.width <= 7680 && m.height >= 1 &&
           m.height <= 4320 && now_ms >= m.heartbeat_ms && now_ms - m.heartbeat_ms <= stale_after_ms;
}
} // namespace steadyxr::obs_output
