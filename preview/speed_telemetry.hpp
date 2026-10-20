#pragma once

#include <memory>

namespace vrst::preview {

enum class SpeedGame : unsigned { automatic = 0, iracing = 1, lmu = 2 };

struct SpeedReading {
    bool valid{};
    double speed_kph{};
    SpeedGame game{SpeedGame::automatic};
};

// Owned and sampled by the capture consumer thread. Call reset when disabled.
class SpeedTelemetry {
public:
    SpeedTelemetry();
    ~SpeedTelemetry();
    SpeedTelemetry(const SpeedTelemetry &) = delete;
    SpeedTelemetry &operator=(const SpeedTelemetry &) = delete;
    SpeedReading sample(SpeedGame selection = SpeedGame::automatic);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrst::preview
