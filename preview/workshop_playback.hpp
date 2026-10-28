#pragma once
#include <cstdint>
namespace vrst::preview {
// Media timestamps are 100 ns; anchor deadlines instead of accumulating UI delays.
struct PlaybackClock {
    std::uint64_t wall{};
    std::int64_t media{};
    void start(std::uint64_t now, std::int64_t timestamp) {
        wall = now;
        media = timestamp;
    }
    bool due(std::uint64_t now, std::int64_t timestamp) const {
        return now >= wall && timestamp >= media && (now - wall) * 10000 >= std::uint64_t(timestamp - media);
    }
};
} // namespace vrst::preview
