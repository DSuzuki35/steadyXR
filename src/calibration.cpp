#include "vrst/calibration.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vrst {
namespace {
constexpr std::uint64_t max_delay_ns = 1000000000ULL;
constexpr std::uint64_t max_gap_ns = 100000000ULL;
constexpr std::uint64_t max_span_ns = 32000000000ULL;
constexpr unsigned min_samples = 90;

struct AxisFit {
    double weight{}, a{}, b{}, aa{}, bb{}, ab{}, speed_squared{}, travel{};
    void add(double predicted, double observed, double confidence, double dt) {
        weight += confidence;
        a += confidence * predicted;
        b += confidence * observed;
        aa += confidence * predicted * predicted;
        bb += confidence * observed * observed;
        ab += confidence * predicted * observed;
        speed_squared += confidence * (predicted / dt) * (predicted / dt);
        travel += std::abs(predicted);
    }
    bool excited() const {
        return weight > 0 && speed_squared / weight >= .04 * .04 && travel >= .15 &&
               bb / weight >= .0001 * .0001 && aa - a * a / weight > 1e-9;
    }
    double gain() const {
        return aa > 0 ? ab / aa : 0;
    }
    double error() const {
        return aa > 0 && bb > 0 ? std::clamp(1 - ab * ab / (aa * bb), 0., 1.) : 1;
    }
    double correlation() const {
        const double va = aa - a * a / weight, vb = bb - b * b / weight;
        const double covariance = ab - a * b / weight;
        return va > 1e-12 && vb > 1e-12 ? std::clamp(covariance / std::sqrt(va * vb), -1., 1.) : 0;
    }
};
struct Fit {
    double delay{}, score{std::numeric_limits<double>::infinity()};
    AxisFit x, y;
};

Quat orientation_at(const std::vector<CalibrationPose> &poses, std::uint64_t at) {
    const auto next =
        std::lower_bound(poses.begin(), poses.end(), at,
                         [](const CalibrationPose &p, std::uint64_t t) { return p.time_ns < t; });
    if (next == poses.begin() || next->time_ns == at)
        return next->orientation;
    const auto previous = next - 1;
    // Caller guarantees overlap and rejects tracking gaps before searching.
    const double fraction =
        static_cast<double>(at - previous->time_ns) / static_cast<double>(next->time_ns - previous->time_ns);
    return slerp(previous->orientation, next->orientation, fraction);
}

Fit fit_delay(const std::vector<CalibrationPose> &poses, const std::vector<CalibrationMotion> &motions,
              double delay_ms) {
    Fit fit;
    fit.delay = delay_ms;
    const auto delay_ns = static_cast<std::uint64_t>(std::llround(delay_ms * 1e6));
    for (const auto &m : motions) {
        const auto previous = orientation_at(poses, m.previous_time_ns - delay_ns);
        const auto current = orientation_at(poses, m.time_ns - delay_ns);
        // A stationary point on the previous central ray, expressed in the
        // current camera. This predicts the observed content shift, including
        // the screen's downward-positive Y convention.
        const auto ray = rotate(multiply(conjugate(current), previous), {0, 0, -1});
        if (ray.z >= -.9)
            return fit;
        const double dt = static_cast<double>(m.time_ns - m.previous_time_ns) * 1e-9;
        fit.x.add(ray.x / -ray.z, m.x, m.confidence, dt);
        fit.y.add(-ray.y / -ray.z, m.y, m.confidence, dt);
    }
    if (fit.x.excited() && fit.y.excited() && fit.x.gain() > 0 && fit.y.gain() > 0)
        fit.score = (fit.x.error() + fit.y.error()) / 2;
    return fit;
}

CalibrationResult failure(const std::string &reason, unsigned samples = 0) {
    CalibrationResult out;
    out.samples = samples;
    out.reason = reason;
    return out;
}
} // namespace

CalibrationResult calibrate(const std::vector<CalibrationPose> &input_poses,
                            const std::vector<CalibrationMotion> &input_motions) {
    // Check sizes before scanning or copying caller-owned data. The common
    // overlap below and fixed 1001 search evaluations bound computation.
    if (input_poses.size() > 10000 || input_motions.size() > 4000)
        return failure("Recording exceeds calibration limits; collect at most 30 seconds.");
    if (input_poses.size() < 2 || input_motions.size() < min_samples)
        return failure("Too few samples; record several seconds of varied left/right and up/down movement.");

    std::vector<CalibrationPose> poses;
    poses.reserve(input_poses.size());
    for (const auto &p : input_poses) {
        if (!valid(p.orientation) || (!poses.empty() && p.time_ns <= poses.back().time_ns))
            return failure("Invalid or unordered headset samples; restart calibration.");
        if (!poses.empty()) {
            if (p.time_ns - poses.back().time_ns > max_gap_ns)
                return failure("Headset tracking has gaps; restore a steady pose connection and retry.");
            if (angle_between(p.orientation, poses.back().orientation) > 20 * pi / 180)
                return failure("Headset tracking jumped or recentered; use small head movements and retry.");
        }
        poses.push_back({p.time_ns, normalized(p.orientation)});
    }
    if (poses.back().time_ns - poses.front().time_ns > max_span_ns)
        return failure("Recording exceeds calibration limits; collect at most 30 seconds.");

    std::vector<CalibrationMotion> motions;
    motions.reserve(input_motions.size());
    unsigned reliable = 0;
    std::uint64_t previous_end = 0;
    bool first = true;
    for (const auto &m : input_motions) {
        if (m.previous_time_ns >= m.time_ns || (!first && m.previous_time_ns < previous_end) ||
            !std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.confidence) || m.confidence < 0 ||
            m.confidence > 1 || std::abs(m.x) > .5 || std::abs(m.y) > .5)
            return failure("Invalid or unordered image-motion samples; restart calibration.");
        if (m.time_ns - m.previous_time_ns > max_gap_ns ||
            (!first && m.previous_time_ns > previous_end && m.previous_time_ns - previous_end > max_gap_ns))
            return failure("Video observations have gaps; restore steady capture and retry.");
        previous_end = m.time_ns;
        first = false;
        if (m.confidence < .5)
            continue;
        ++reliable;
        // Every candidate must use the same observations: partial overlap must
        // never make an unsupported delay win. Subtract only after comparing,
        // preserving nanosecond precision even with large monotonic epochs.
        if (m.previous_time_ns >= poses.front().time_ns &&
            m.previous_time_ns - poses.front().time_ns >= max_delay_ns && m.time_ns <= poses.back().time_ns)
            motions.push_back(m);
    }
    if (input_motions.back().time_ns - input_motions.front().previous_time_ns > max_span_ns)
        return failure("Recording exceeds calibration limits; collect at most 30 seconds.");
    if (reliable < min_samples || reliable * 2 < input_motions.size())
        return failure("Image texture or motion confidence is too weak; use a detailed stationary scene.",
                       reliable);
    const auto samples = static_cast<unsigned>(motions.size());
    if (samples < min_samples || samples * 2 < reliable)
        return failure(
            "Too little timestamp overlap with headset poses; check the connection and record longer.",
            samples);
    if (motions.back().time_ns - motions.front().previous_time_ns < 2000000000ULL)
        return failure("Recording is too short; collect several seconds of varied head movement.", samples);

    std::array<double, 1001> scores;
    Fit best;
    // Resolve every candidate at the same precision. Refining only the winner
    // made equally good periodic aliases between coarse samples look worse,
    // producing confident but arbitrary delays. The complete 1 ms grid also
    // avoids depending on the width of a coarse local minimum.
    for (unsigned delay = 0; delay < scores.size(); ++delay) {
        const auto fit = fit_delay(poses, motions, delay);
        scores[delay] = fit.score;
        if (fit.score < best.score)
            best = fit;
    }
    if (!std::isfinite(best.score))
        return failure("Insufficient matching movement on both axes or reversed image direction; repeat "
                       "varied left/right and up/down turns.",
                       samples);
    const double cx = best.x.correlation(), cy = best.y.correlation();
    if (cx < .9 || cy < .9 || best.x.error() > .2 || best.y.error() > .2)
        return failure("Image and headset motion do not correlate reliably; park the car and use small "
                       "varied head movements.",
                       samples);
    const double hfov = 2 * std::atan(.5 / best.x.gain()) * 180 / pi;
    const double vfov = 2 * std::atan(.5 / best.y.gain()) * 180 / pi;
    if (hfov < 10 || hfov > 170 || vfov < 10 || vfov > 170)
        return failure(
            "Estimated mirror FOV is outside the supported range; check the captured view and retry.",
            samples);

    double alternative = 1;
    for (unsigned delay = 0; delay < scores.size(); ++delay)
        if (std::abs(delay - best.delay) >= 50)
            alternative = std::min(alternative, scores[delay]);
    const double separation = alternative - best.score;
    if (separation < std::max(.01, best.score * .25))
        return failure("Delay is ambiguous; vary the speed and timing of left/right and up/down movements.",
                       samples);

    CalibrationResult out;
    out.valid = true;
    out.delay_ms = best.delay;
    out.horizontal_fov = hfov;
    out.vertical_fov = vfov;
    const double texture = best.x.weight / samples;
    const double uniqueness = std::clamp(separation / .08, 0., 1.);
    out.confidence = std::clamp(
        std::sqrt(cx * cy) * texture * std::min(1., samples / 180.) * (.7 + .3 * uniqueness), 0., 1.);
    out.samples = samples;
    out.reason =
        "Suggested delay and approximate effective mirror FOV; this does not measure headset optics.";
    return out;
}
} // namespace vrst
