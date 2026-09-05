#pragma once
#include "vrst/core.hpp"
#include <array>
#include <cmath>
#include <optional>

namespace vrst::sender {
inline Projection projection_from_raw(double left, double right, double top, double bottom) {
    // OpenVR names the -Y extent "top" and the +Y extent "bottom".
    // Preserve their signed values while mapping to our canonical up/down fields.
    return {left, right, bottom, top};
}

using Rotation = std::array<std::array<double, 3>, 3>;
inline std::optional<Quat> rotation_quaternion(const Rotation &m) {
    // OpenVR poses should be rigid transforms. Reject bad tracking matrices rather
    // than normalizing a zero, scaled, or reflected transform into a valid pose.
    for (int row = 0; row < 3; ++row) {
        for (int other = row; other < 3; ++other) {
            double dot = 0;
            for (int col = 0; col < 3; ++col)
                dot += m[row][col] * m[other][col];
            if (!std::isfinite(dot) || std::abs(dot - (row == other ? 1.0 : 0.0)) > .02)
                return std::nullopt;
        }
    }
    const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!std::isfinite(determinant) || determinant < .95 || determinant > 1.05)
        return std::nullopt;
    Quat q;
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const double s = 2 * std::sqrt(trace + 1);
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = 2 * std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]);
        q = {s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const double s = 2 * std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]);
        q = {(m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const double s = 2 * std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]);
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4, (m[1][0] - m[0][1]) / s};
    }
    if (!valid(q))
        return std::nullopt;
    return normalized(q);
}
enum class Source { invalid, frame, live, wait };
class FrameSelector {
  public:
    Source select(bool current_valid, std::optional<std::uint32_t> index, bool frame_valid,
                  std::uint64_t now) {
        const bool new_frame = index && (!last_index_ || *index != *last_index_);
        if (new_frame) {
            last_index_ = index;
            observed_at_ = now;
        }
        if (!current_valid)
            return Source::invalid;
        if (index && frame_valid) {
            if (new_frame)
                return Source::frame;
            if (now >= observed_at_ && now - observed_at_ < 100000000ULL)
                return Source::wait;
        }
        return Source::live;
    }

  private:
    std::optional<std::uint32_t> last_index_;
    std::uint64_t observed_at_{};
};
} // namespace vrst::sender
