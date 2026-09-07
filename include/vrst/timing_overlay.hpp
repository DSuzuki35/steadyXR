#pragma once
#include "vrst/core.hpp"
#include <string>
#include <vector>

namespace vrst {
// Original sender-clock values only; receiver wall/monotonic clock epochs must
// never be substituted into now_ns or either pose timestamp.
struct TimingOverlayData {
    bool sender_view{}, latest_present{}, connected{}, clock_ready{};
    bool pose_applied{}, comparison{}, calibrating{}, image_only{};
    std::uint64_t now_ns{}, session{}, latest_receive_age_ns{}, buffer_age_ns{};
    Pose latest;
    PoseResult selected;
    double delay_ms{}, rtt_ms{};
};
struct OverlayBitmap {
    unsigned width{}, height{};
    std::vector<std::uint8_t> rgba;
};
std::string format_timing_stamp(std::uint64_t ns);
std::vector<std::string> timing_overlay_lines(const TimingOverlayData &data);
OverlayBitmap render_timing_overlay(const TimingOverlayData &data, unsigned scale = 2);
} // namespace vrst
