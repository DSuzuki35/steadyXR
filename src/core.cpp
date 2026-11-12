#include "vrst/core.hpp"
#include <algorithm>
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
MotionSettings sanitize_motion(MotionSettings s) {
    s.amount = safe(s.amount, 40, 0, 100);
    s.yaw = safe(s.yaw, 40, 0, 100);
    s.pitch = safe(s.pitch, 40, 0, 100);
    s.roll = safe(s.roll, 40, 0, 100);
    s.turn_follow = safe(s.turn_follow, 0, 0, 100);
    s.turn_speed = safe(s.turn_speed, 90, 1, 360);
    return s;
}
// Shortest rotation vector avoids Euler singularities and quaternion sign flips.
Vec3 rotation_vector(Quat q) {
    q = normalized(q);
    if (q.w < 0)
        q = {-q.x, -q.y, -q.z, -q.w};
    const double n = std::hypot(q.x, std::hypot(q.y, q.z));
    const double scale = n > 1e-12 ? 2 * std::atan2(n, q.w) / n : 2;
    return {q.x * scale, q.y * scale, q.z * scale};
}
double axis_retention(double amount, double velocity, double &signed_speed, double &speed, double dt,
                      const MotionSettings &s) {
    const double limit = s.turn_speed * pi / 180;
    velocity = std::clamp(velocity, -4 * limit, 4 * limit);
    const double blend = 1 - std::exp(-dt / .2);
    signed_speed += blend * (velocity - signed_speed);
    speed += blend * (std::abs(velocity) - speed);
    const double coherence = speed > 1e-9 ? std::abs(signed_speed) / speed : 0;
    const double follow = s.turn_follow / 100 * std::clamp(std::abs(signed_speed) / limit, 0., 1.) *
                          std::clamp((coherence - .35) / .65, 0., 1.);
    const double strength = amount / 100, tau = .02 + .48 * strength * strength;
    return amount <= 0 ? 0 : std::exp(-dt * (1 + 12 * follow) / tau);
}
Quat next_advanced(const Pose &prev, Quat smooth, const Pose &cur, const MotionSettings &s, Vec3 &velocity,
                   Vec3 &speed) {
    if (!s.separate_axes && s.turn_follow <= 0) {
        velocity = speed = {};
        return next_smooth(prev, smooth, cur, s.amount);
    }
    const double dt = double(cur.time_ns - prev.time_ns) * 1e-9;
    if (!(prev.flags & TrackingValid) || !(cur.flags & TrackingValid) || dt > .15 ||
        angle_between(prev.orientation, cur.orientation) > 45 * pi / 180) {
        velocity = speed = {};
        return cur.orientation;
    }
    const auto change = rotation_vector(multiply(conjugate(prev.orientation), cur.orientation));
    auto error = rotation_vector(multiply(conjugate(cur.orientation), smooth));
    // The correction is represented in the current eye's local pitch/yaw/roll axes.
    error.x *=
        axis_retention(s.separate_axes ? s.pitch : s.amount, change.x / dt, velocity.x, speed.x, dt, s);
    error.y *= axis_retention(s.separate_axes ? s.yaw : s.amount, change.y / dt, velocity.y, speed.y, dt, s);
    error.z *= axis_retention(s.separate_axes ? s.roll : s.amount, change.z / dt, velocity.z, speed.z, dt, s);
    const double angle = std::hypot(error.x, std::hypot(error.y, error.z));
    return normalized(multiply(cur.orientation, axis_angle(error, angle)));
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
Quat horizon_correction(Quat source, Quat target, double strength, double max_degrees) {
    if (!valid(source) || !valid(target))
        return {};
    source = normalized(source);
    target = normalized(target);
    const auto forward = rotate(target, {0, 0, -1});
    const auto up = rotate(target, {0, 1, 0});
    const auto right = rotate(target, {1, 0, 0});
    // World-up projected into the eye's right/up plane gives its roll error.
    // Roll becomes ambiguous when looking vertically; fade out near that pole.
    const double horizontal = std::hypot(up.y, right.y);
    const double fade = std::clamp((horizontal - .05) / .20, 0., 1.);
    const double limit = safe(max_degrees, 45, 0, 60) * pi / 180;
    const double roll =
        std::clamp(std::atan2(right.y, up.y), -limit, limit) * safe(strength, 0, 0, 100) / 100 * fade;
    target = normalized(multiply(axis_angle(forward, roll), target));
    return normalized(multiply(conjugate(source), target));
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
    Entry entry{p, p.orientation};
    if (!entries_.empty()) {
        entry.follow_velocity = entries_.back().follow_velocity;
        entry.follow_speed = entries_.back().follow_speed;
        entry.smooth = next_advanced(entries_.back().pose, entries_.back().smooth, p, motion_,
                                     entry.follow_velocity, entry.follow_speed);
    }
    entries_.push_back(entry);
    while (entries_.size() > 2048 ||
           (entries_.size() > 1 && p.time_ns - entries_.front().pose.time_ns > 6000000000ULL))
        entries_.pop_front();
    return true;
}
void PoseHistory::recompute() {
    if (entries_.empty())
        return;
    entries_.front().smooth = entries_.front().pose.orientation;
    entries_.front().follow_velocity = entries_.front().follow_speed = {};
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        auto &cur = entries_[i];
        const auto &prev = entries_[i - 1];
        cur.follow_velocity = prev.follow_velocity;
        cur.follow_speed = prev.follow_speed;
        cur.smooth =
            next_advanced(prev.pose, prev.smooth, cur.pose, motion_, cur.follow_velocity, cur.follow_speed);
    }
}
void PoseHistory::set_smoothing(double amount) {
    MotionSettings s;
    s.amount = amount;
    set_smoothing(s);
}
void PoseHistory::set_smoothing(const MotionSettings &raw) {
    const auto s = sanitize_motion(raw);
    if (s.amount == motion_.amount && s.separate_axes == motion_.separate_axes && s.yaw == motion_.yaw &&
        s.pitch == motion_.pitch && s.roll == motion_.roll && s.turn_follow == motion_.turn_follow &&
        s.turn_speed == motion_.turn_speed)
        return;
    motion_ = s;
    smoothing_ = s.amount;
    recompute();
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
    out.sample_time_ns = it->pose.time_ns;
    // A frame pose stays tied to its image. Only live tracking samples interpolate.
    if (!(it->pose.flags & FramePose) && next != entries_.end() && (next->pose.flags & TrackingValid) &&
        next->pose.time_ns - it->pose.time_ns < 100000000ULL) {
        double t = static_cast<double>(at - it->pose.time_ns) /
                   static_cast<double>(next->pose.time_ns - it->pose.time_ns);
        source = slerp(source, next->pose.orientation, t);
        target = slerp(target, next->smooth, t);
        out.interpolated = at != it->pose.time_ns;
        out.sample_time_ns = at;
    }
    out.valid = true;
    out.source_orientation = source;
    out.target_orientation = target;
    out.correction =
        smoothing_ <= 0 && !motion_.separate_axes ? Quat{} : normalized(multiply(conjugate(source), target));
    out.projection = it->pose.projection;
    out.time_ns = it->pose.time_ns;
    out.sequence = it->pose.sequence;
    out.flags = it->pose.flags;
    out.session = it->pose.session;
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
EdgeReserve sanitize_edge_reserve(EdgeReserve edges) {
    edges.left = safe(edges.left, 5, 0, 35);
    edges.right = safe(edges.right, 5, 0, 35);
    edges.top = safe(edges.top, 5, 0, 35);
    edges.bottom = safe(edges.bottom, 5, 0, 35);
    return edges;
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
    if (s.edge_reserve.enabled) {
        const auto reserve = sanitize_edge_reserve(s.edge_reserve);
        double fit_x = 1, fit_y = 1;
        if (s.output_16_9) {
            const double reserved_width = iw * (1 - (reserve.left + reserve.right) / 100);
            const double reserved_height = ih * (1 - (reserve.top + reserve.bottom) / 100);
            const double aspect = double(w.width) / w.height;
            fit_x = std::min(1., reserved_height * aspect / reserved_width);
            fit_y = std::min(1., reserved_width / aspect / reserved_height);
        }
        // Fit any additional aspect crop in the same proportion as the requested
        // reserves. A small left reserve must keep its preference for left scenery.
        auto bounds = [](double low, double high, double first, double last, double fit) {
            const double extent = high - low;
            const double a = first / 100, b = last / 100;
            const double extra = extent * (1 - a - b) * (1 - fit);
            const double bias = first + last > 0 ? first / (first + last) : .5;
            return std::array<double, 2>{low + extent * a + extra * bias,
                                         high - extent * b - extra * (1 - bias)};
        };
        const auto x = bounds(w.input.left, w.input.right, reserve.left, reserve.right, fit_x);
        const auto y = bounds(w.input.down, w.input.up, reserve.bottom, reserve.top, fit_y);
        const double zoom = 1 - safe(s.framing_zoom, 0, 0, 40) / 100;
        sx = (x[1] - x[0]) * zoom;
        sy = (y[1] - y[0]) * zoom;
        cx = (x[0] + x[1]) / 2;
        cy = (y[0] + y[1]) / 2;
        const double fx = safe(s.framing_x, 0, -100, 100) / 100;
        const double fy = -safe(s.framing_y, 0, -100, 100) / 100;
        cx += fx * (fx >= 0 ? w.input.right - cx - sx / 2 : cx - sx / 2 - w.input.left);
        cy += fy * (fy >= 0 ? w.input.up - cy - sy / 2 : cy - sy / 2 - w.input.down);
        w.output = {cx - sx / 2, cx + sx / 2, cy + sy / 2, cy - sy / 2};
    }
    limit_warp_correction(w, correction, s.mirror_edges);
    return w;
}
void limit_warp_correction(Warp &w, Quat correction, bool mirror_edges) {
    w.correction_fraction = 1;
    w.correction = valid(correction) ? normalized(correction) : Quat{};
    const auto fits = [&] {
        if (!mirror_edges)
            return corners_inside(w);
        // Reflection extends the captured image, not the eye's projection plane.
        // Keep a margin from its horizon so even extreme turns stay finite.
        for (double u : {0., 1.})
            for (double v : {0., 1.}) {
                const auto ray = rotate(w.correction, {w.output.left + u * (w.output.right - w.output.left),
                                                       w.output.up + v * (w.output.down - w.output.up), -1});
                if (ray.z > -.05)
                    return false;
            }
        return true;
    };
    if (!fits()) {
        Quat desired = w.correction;
        double lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) {
            double m = (lo + hi) / 2;
            w.correction = slerp({}, desired, m);
            if (fits())
                lo = m;
            else
                hi = m;
        }
        w.correction_fraction = lo;
        w.correction = slerp({}, desired, lo);
    }
}
} // namespace vrst
