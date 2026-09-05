#include "vrst/core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
namespace vrst {
namespace {
double dot(Quat a, Quat b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
double safe(double v, double fallback, double lo, double hi) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
Quat next_smooth(const Pose &prev, Quat smooth, const Pose &cur, double amount) {
    const double dt = static_cast<double>(cur.time_ns - prev.time_ns) * 1e-9;
    if (amount <= 0 || !(prev.flags & TrackingValid) || !(cur.flags & TrackingValid) || dt > .15 ||
        angle_between(prev.orientation, cur.orientation) > 45 * pi / 180)
        return cur.orientation;
    double strength = amount / 100, tau = .02 + .48 * strength * strength;
    return slerp(smooth, cur.orientation, 1 - std::exp(-dt / tau));
}
} // namespace
bool valid(Quat q) {
    double n = dot(q, q);
    return std::isfinite(n) && n > .25 && n < 4;
}
Quat normalized(Quat q) {
    double n = std::sqrt(dot(q, q));
    if (!std::isfinite(n) || n < 1e-12)
        return {};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
Quat conjugate(Quat q) {
    return {-q.x, -q.y, -q.z, q.w};
}
Quat multiply(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat slerp(Quat a, Quat b, double t) {
    a = normalized(a);
    b = normalized(b);
    t = std::clamp(t, 0., 1.);
    double d = dot(a, b);
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > .9995)
        return normalized(
            {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    double theta = std::acos(std::clamp(d, -1., 1.)), s = std::sin(theta), x = std::sin((1 - t) * theta) / s,
           y = std::sin(t * theta) / s;
    return normalized({a.x * x + b.x * y, a.y * x + b.y * y, a.z * x + b.z * y, a.w * x + b.w * y});
}
Quat axis_angle(Vec3 a, double r) {
    double n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    if (n < 1e-12 || !std::isfinite(n) || !std::isfinite(r))
        return {};
    double k = std::sin(r / 2) / n;
    return {a.x * k, a.y * k, a.z * k, std::cos(r / 2)};
}
Vec3 rotate(Quat q, Vec3 v) {
    q = normalized(q);
    auto r = multiply(multiply(q, {v.x, v.y, v.z, 0}), conjugate(q));
    return {r.x, r.y, r.z};
}
double angle_between(Quat a, Quat b) {
    return 2 * std::acos(std::clamp(std::abs(dot(normalized(a), normalized(b))), 0., 1.));
}
bool valid(Projection p) {
    return std::isfinite(p.left) && std::isfinite(p.right) && std::isfinite(p.up) && std::isfinite(p.down) &&
           p.right - p.left > 1e-4 && p.up - p.down > 1e-4 && std::abs(p.left) < 100 &&
           std::abs(p.right) < 100 && std::abs(p.up) < 100 && std::abs(p.down) < 100;
}
Projection symmetric_projection(double h, double v) {
    double x = std::tan(safe(h, 100, 1, 175) * pi / 360), y = std::tan(safe(v, 90, 1, 175) * pi / 360);
    return {-x, x, y, -y};
}
void PoseHistory::clear() {
    entries_.clear();
}
bool PoseHistory::push(const Pose &in) {
    if (!valid(in.orientation) || !valid(in.projection) || !in.session || !in.time_ns)
        return false;
    if (!entries_.empty() && entries_.back().pose.session != in.session)
        clear();
    if (!entries_.empty() &&
        (in.time_ns <= entries_.back().pose.time_ns || in.sequence <= entries_.back().pose.sequence))
        return false;
    Pose p = in;
    p.orientation = normalized(p.orientation);
    Quat q = entries_.empty() ? p.orientation
                              : next_smooth(entries_.back().pose, entries_.back().smooth, p, smoothing_);
    entries_.push_back({p, q});
    while (entries_.size() > 2048 ||
           (entries_.size() > 1 && p.time_ns - entries_.front().pose.time_ns > 6000000000ULL))
        entries_.pop_front();
    return true;
}
void PoseHistory::recompute() {
    if (entries_.empty())
        return;
    entries_.front().smooth = entries_.front().pose.orientation;
    for (std::size_t i = 1; i < entries_.size(); ++i)
        entries_[i].smooth =
            next_smooth(entries_[i - 1].pose, entries_[i - 1].smooth, entries_[i].pose, smoothing_);
}
void PoseHistory::set_smoothing(double amount) {
    amount = safe(amount, 40, 0, 100);
    if (amount != smoothing_) {
        smoothing_ = amount;
        recompute();
    }
}
std::uint64_t PoseHistory::latest_time() const {
    return entries_.empty() ? 0 : entries_.back().pose.time_ns;
}
PoseResult PoseHistory::sample(std::uint64_t at) const {
    PoseResult out;
    if (entries_.empty() || at < entries_.front().pose.time_ns)
        return out;
    auto it = std::upper_bound(entries_.begin(), entries_.end(), at,
                               [](std::uint64_t t, const Entry &e) { return t < e.pose.time_ns; });
    auto next = it;
    --it;
    if (!(it->pose.flags & TrackingValid) || at - it->pose.time_ns > 100000000ULL)
        return out;
    Quat source = it->pose.orientation, target = it->smooth;
    // A frame pose stays tied to its image. Only live tracking samples interpolate.
    if (!(it->pose.flags & FramePose) && next != entries_.end() && (next->pose.flags & TrackingValid) &&
        next->pose.time_ns - it->pose.time_ns < 100000000ULL) {
        double t = static_cast<double>(at - it->pose.time_ns) /
                   static_cast<double>(next->pose.time_ns - it->pose.time_ns);
        source = slerp(source, next->pose.orientation, t);
        target = slerp(target, next->smooth, t);
    }
    out.valid = true;
    out.correction = smoothing_ <= 0 ? Quat{} : normalized(multiply(conjugate(source), target));
    out.projection = it->pose.projection;
    out.time_ns = it->pose.time_ns;
    out.sequence = it->pose.sequence;
    out.flags = it->pose.flags;
    return out;
}
std::array<double, 2> map_uv(const Warp &w, double u, double v) {
    auto r = rotate(w.correction, {w.output.left + u * (w.output.right - w.output.left),
                                   w.output.up + v * (w.output.down - w.output.up), -1});
    if (r.z >= -1e-8)
        return {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    double x = (r.x / -r.z - w.input.left) / (w.input.right - w.input.left),
           y = (w.input.up - r.y / -r.z) / (w.input.up - w.input.down);
    return {w.uv_left + x * (w.uv_right - w.uv_left), w.uv_top + y * (w.uv_bottom - w.uv_top)};
}
bool corners_inside(const Warp &w) {
    for (double u : {0., 1.})
        for (double v : {0., 1.}) {
            auto p = map_uv(w, u, v);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || p[0] < w.uv_left - 1e-9 ||
                p[0] > w.uv_right + 1e-9 || p[1] < w.uv_top - 1e-9 || p[1] > w.uv_bottom + 1e-9)
                return false;
        }
    return true;
}
Warp make_warp(std::uint32_t width, std::uint32_t height, const Settings &s, Quat correction,
               std::optional<Projection> automatic) {
    Warp w;
    w.uv_left = safe(s.input_left, 0, 0, 45) / 100;
    w.uv_right = 1 - safe(s.input_right, 0, 0, 45) / 100;
    w.uv_top = safe(s.input_top, 0, 0, 45) / 100;
    w.uv_bottom = 1 - safe(s.input_bottom, 0, 0, 45) / 100;
    double iw = std::max(2., width * (w.uv_right - w.uv_left)),
           ih = std::max(2., height * (w.uv_bottom - w.uv_top)), ow = iw, oh = ih;
    if (s.output_16_9) {
        ow = std::min(iw, ih * 16 / 9);
        oh = ow * 9 / 16;
    }
    w.width = std::max(2U, static_cast<std::uint32_t>(std::floor(ow + 1e-7)) & ~1U);
    w.height = std::max(2U, static_cast<std::uint32_t>(std::floor(oh + 1e-7)) & ~1U);
    if (s.output_16_9) {
        unsigned scale = std::max(1U, static_cast<unsigned>(std::floor(ow / 16 + 1e-7)));
        if (scale > 1)
            scale &= ~1U;
        w.width = 16 * scale;
        w.height = 9 * scale;
    }
    w.input = (s.automatic_fov && automatic && valid(*automatic))
                  ? *automatic
                  : symmetric_projection(s.horizontal_fov, s.vertical_fov);
    double keep =
        (1 - safe(s.stabilization_crop, 10, 0, 40) / 100) * (1 - safe(s.framing_zoom, 0, 0, 40) / 100);
    double sx = (w.input.right - w.input.left) * keep * ow / iw,
           sy = (w.input.up - w.input.down) * keep * oh / ih;
    double cx = (w.input.left + w.input.right) / 2, cy = (w.input.up + w.input.down) / 2;
    cx += safe(s.framing_x, 0, -100, 100) / 100 * ((w.input.right - w.input.left) - sx) / 2;
    cy -= safe(s.framing_y, 0, -100, 100) / 100 * ((w.input.up - w.input.down) - sy) / 2;
    w.output = {cx - sx / 2, cx + sx / 2, cy + sy / 2, cy - sy / 2};
    w.correction = valid(correction) ? normalized(correction) : Quat{};
    if (!corners_inside(w)) {
        Quat desired = w.correction;
        double lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) {
            double m = (lo + hi) / 2;
            w.correction = slerp({}, desired, m);
            if (corners_inside(w))
                lo = m;
            else
                hi = m;
        }
        w.correction_fraction = lo;
        w.correction = slerp({}, desired, lo);
    }
    return w;
}
bool ClockSync::observe(std::uint64_t t1, std::uint64_t t2, std::uint64_t t3, std::uint64_t t4) {
    if (t4 < t1 || t3 < t2)
        return false;
    double rtt = static_cast<double>(t4 - t1) - static_cast<double>(t3 - t2);
    if (rtt < 0 || rtt > 500000000.)
        return false;
    double offset = ((static_cast<double>(t2) - static_cast<double>(t1)) +
                     (static_cast<double>(t3) - static_cast<double>(t4))) /
                    2;
    samples_.push_back({offset, rtt, t4});
    while (samples_.size() > 32 || (!samples_.empty() && t4 - samples_.front().at > 15000000000ULL))
        samples_.pop_front();
    return true;
}
double ClockSync::offset_ns() const {
    if (samples_.empty())
        return 0;
    return std::min_element(samples_.begin(), samples_.end(), [](auto a, auto b) { return a.rtt < b.rtt; })
        ->offset;
}
double ClockSync::rtt_ms() const {
    if (samples_.empty())
        return 0;
    return std::min_element(samples_.begin(), samples_.end(), [](auto a, auto b) { return a.rtt < b.rtt; })
               ->rtt *
           1e-6;
}
std::uint64_t monotonic_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
} // namespace vrst
