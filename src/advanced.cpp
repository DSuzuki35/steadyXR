#include "vrst/advanced.hpp"
#include <algorithm>
#include <cmath>
namespace vrst {
namespace {
double finite(double x, double fallback, double lo, double hi) {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}
double base_crop(const Settings &s) {
    return finite(s.stabilization_crop, 10, 0, 40);
}
template <class Fits> double required_crop(const Settings &s, double extra, Fits fits) {
    const double base = base_crop(s);
    const double ceiling = std::min(40., base + finite(extra, 10, 0, 40));
    if (ceiling == base || fits(base))
        return base;
    if (!fits(ceiling))
        return ceiling;
    double lo = base, hi = ceiling;
    for (unsigned i = 0; i < 26; ++i) {
        const double mid = (lo + hi) / 2;
        if (fits(mid))
            hi = mid;
        else
            lo = mid;
    }
    return hi;
}
} // namespace
void CropController::reset() {
    crop_ = base_ = 0;
    time_ns_ = 0;
}
double CropController::update(double required, double base, const CropSettings &s, std::uint64_t time) {
    base = finite(base, 10, 0, 40);
    const double ceiling = std::min(40., base + finite(s.max_extra, 10, 0, 40));
    if (!s.enabled || ceiling == base || !time || !time_ns_ || base != base_) {
        crop_ = base_ = base;
        time_ns_ = time;
        return crop_;
    }
    crop_ = std::clamp(crop_, base, ceiling);
    // A repeated/out-of-order sample or an idle interval cannot buy a burst of zoom.
    if (time <= time_ns_)
        return crop_;
    const double dt = double(time - time_ns_) * 1e-9;
    time_ns_ = time;
    if (dt > .25)
        return crop_;
    const double target = finite(required, base, base, ceiling);
    const double rate = finite(s.speed, 3, 0, 40) * (target < crop_ ? .5 : 1);
    crop_ += std::clamp(target - crop_, -rate * dt, rate * dt);
    return crop_;
}
double required_pose_crop(unsigned width, unsigned height, const Settings &s, Quat correction,
                          std::optional<Projection> automatic, double extra) {
    if (!width || !height || !valid(correction))
        return base_crop(s);
    return required_crop(s, extra, [&](double crop) {
        auto candidate = s;
        candidate.mirror_edges = false; // Prefer real pixels; reflect while crop catches up.
        candidate.stabilization_crop = crop;
        return make_warp(width, height, candidate, correction, automatic).correction_fraction == 1;
    });
}
double required_image_crop(unsigned width, unsigned height, const Settings &s, const ImageResult &r,
                           double extra) {
    if (!width || !height || (r.status != ImageStatus::tracking && r.status != ImageStatus::weak) ||
        !r.width || !r.height || !std::isfinite(r.correction.x) || !std::isfinite(r.correction.y) ||
        !std::isfinite(r.correction.angle))
        return base_crop(s);
    return required_crop(s, extra, [&](double crop) {
        auto candidate = s;
        candidate.mirror_edges = false;
        candidate.stabilization_crop = crop;
        return make_image_warp(width, height, candidate, r).correction_fraction == 1;
    });
}
} // namespace vrst
