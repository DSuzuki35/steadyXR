#pragma once
#include "vrst/core.hpp"
#include <string>
#include <vector>

namespace vrst {
struct CalibrationPose {
    std::uint64_t time_ns{}; // Clock-corrected local monotonic time.
    Quat orientation;        // Right-handed HMD space: X right, Y up, -Z forward.
};
struct CalibrationMotion {
    std::uint64_t previous_time_ns{}, time_ns{}; // Capture times, not analysis completion times.
    double x{}, y{}, confidence{}; // Previous-to-current content shift / image width and height.
};
struct CalibrationResult {
    bool valid{};
    double delay_ms{}, horizontal_fov{}, vertical_fov{}, confidence{};
    unsigned samples{};
    std::string reason;
};
// Fits capture time minus delay (0..1000 ms). Positive head yaw/pitch produce
// positive content x/y. FOV is an effective captured-mirror approximation.
// Inputs must be chronological, contain <=10000 poses and <=4000 motions, and
// span <=32 seconds each. Invalid, weak, gapped or ambiguous data is rejected;
// only valid results should be offered for explicit application by the user.
CalibrationResult calibrate(const std::vector<CalibrationPose> &poses,
                            const std::vector<CalibrationMotion> &motions);
} // namespace vrst
