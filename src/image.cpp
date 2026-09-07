#include "vrst/image.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
namespace vrst {
namespace {
struct Point {
    double x{}, y{};
};
struct Match {
    Point a, b;
};
using Pyramid = std::vector<GrayImage>;
double finite(double x, double fallback, double lo, double hi) {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}
ImageSettings sanitize(ImageSettings s) {
    s.smoothing = finite(s.smoothing, 40, 0, 100);
    s.motion.amount = s.smoothing;
    s.motion.yaw = finite(s.motion.yaw, 40, 0, 100);
    s.motion.pitch = finite(s.motion.pitch, 40, 0, 100);
    s.motion.roll = finite(s.motion.roll, 40, 0, 100);
    s.motion.turn_follow = finite(s.motion.turn_follow, 0, 0, 100);
    s.motion.turn_speed = finite(s.motion.turn_speed, 90, 1, 360);
    s.max_shift_percent = finite(s.max_shift_percent, 1, 0, 5);
    s.max_roll_degrees = finite(s.max_roll_degrees, .5, 0, 5);
    s.region_left = finite(s.region_left, 10, 0, 90);
    s.region_right = finite(s.region_right, 10, 0, 90);
    s.region_top = finite(s.region_top, 50, 0, 90);
    s.region_bottom = finite(s.region_bottom, 10, 0, 90);
    return s;
}
bool same(const ImageSettings &a, const ImageSettings &b) {
    return a.smoothing == b.smoothing && a.motion.separate_axes == b.motion.separate_axes &&
           a.motion.yaw == b.motion.yaw && a.motion.pitch == b.motion.pitch &&
           a.motion.roll == b.motion.roll && a.motion.turn_follow == b.motion.turn_follow &&
           a.motion.turn_speed == b.motion.turn_speed && a.max_shift_percent == b.max_shift_percent &&
           a.max_roll_degrees == b.max_roll_degrees && a.track_roll == b.track_roll &&
           a.region_left == b.region_left && a.region_right == b.region_right &&
           a.region_top == b.region_top && a.region_bottom == b.region_bottom;
}
// Image translation is converted to an approximate angular speed using a 90-degree
// analysis field. Turn speed is a response control, not a calibrated FOV estimate.
double image_retention(double amount, double velocity, double &signed_speed, double &speed, double dt,
                       const MotionSettings &s) {
    velocity = std::clamp(velocity, -4 * s.turn_speed, 4 * s.turn_speed);
    const double blend = 1 - std::exp(-dt / .2);
    signed_speed += blend * (velocity - signed_speed);
    speed += blend * (std::abs(velocity) - speed);
    const double coherence = speed > 1e-9 ? std::abs(signed_speed) / speed : 0;
    const double follow = s.turn_follow / 100 * std::clamp(std::abs(signed_speed) / s.turn_speed, 0., 1.) *
                          std::clamp((coherence - .35) / .65, 0., 1.);
    const double strength = amount / 100, tau = .02 + .48 * strength * strength;
    return amount <= 0 ? 0 : std::exp(-dt * (1 + 12 * follow) / tau);
}

bool good(const GrayImage &f) {
    return f.width >= 32 && f.height >= 32 && f.width <= 1024 && f.height <= 1024 &&
           f.pixels.size() == size_t(f.width) * f.height &&
           (f.valid_pixels.empty() || f.valid_pixels.size() == f.pixels.size()) &&
           std::all_of(f.pixels.begin(), f.pixels.end(),
                       [](float x) { return std::isfinite(x) && x >= 0 && x <= 1; });
}
float sample(const GrayImage &f, double x, double y) {
    const unsigned ix = static_cast<unsigned>(x), iy = static_cast<unsigned>(y);
    double dx = x - ix, dy = y - iy;
    return float(
        (1 - dy) * ((1 - dx) * f.pixels[iy * f.width + ix] + dx * f.pixels[iy * f.width + ix + 1]) +
        dy * ((1 - dx) * f.pixels[(iy + 1) * f.width + ix] + dx * f.pixels[(iy + 1) * f.width + ix + 1]));
}
Pyramid pyramid(const GrayImage &f) {
    Pyramid p{f};
    // NaNs are internal sentinels only. Their propagation through the box
    // pyramid and bilinear patches excludes every footprint touching a mask.
    if (!f.valid_pixels.empty())
        for (size_t i = 0; i < f.pixels.size(); ++i)
            if (!f.valid_pixels[i])
                p[0].pixels[i] = std::numeric_limits<float>::quiet_NaN();
    while (p.size() < 3 && p.back().width >= 96 && p.back().height >= 64) {
        const auto &a = p.back();
        GrayImage b{a.width / 2, a.height / 2, {}, {}};
        b.pixels.resize(size_t(b.width) * b.height);
        for (unsigned y = 0; y < b.height; ++y)
            for (unsigned x = 0; x < b.width; ++x) {
                unsigned i = 2 * y * a.width + 2 * x;
                b.pixels[y * b.width + x] =
                    (a.pixels[i] + a.pixels[i + 1] + a.pixels[i + a.width] + a.pixels[i + a.width + 1]) *
                    .25f;
            }
        p.push_back(std::move(b));
    }
    return p;
}
struct Bounds {
    int left, right, top, bottom;
    bool contains(Point p) const {
        return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
    }
};
// A low feature count alone is not a cut: shadows, blur and a temporarily
// featureless cockpit should release the old correction rather than jump.
// Compare only the selected region, with exposure/contrast removed.
bool scene_discontinuity(const GrayImage &a, const GrayImage &b, Bounds bounds) {
    double sum_a = 0, sum_b = 0, aa = 0, bb = 0, ab = 0, count = 0;
    for (int y = bounds.top; y < bounds.bottom; y += 4)
        for (int x = bounds.left; x < bounds.right; x += 4) {
            const double av = a.pixels[size_t(y) * a.width + x];
            const double bv = b.pixels[size_t(y) * b.width + x];
            if (!std::isfinite(av) || !std::isfinite(bv))
                continue;
            sum_a += av;
            sum_b += bv;
            aa += av * av;
            bb += bv * bv;
            ab += av * bv;
            ++count;
        }
    if (count < 16)
        return false;
    const double mean_a = sum_a / count, mean_b = sum_b / count;
    const double va = std::max(0., aa / count - mean_a * mean_a);
    const double vb = std::max(0., bb / count - mean_b * mean_b);
    if (std::abs(mean_a - mean_b) > .35 || (vb < .000005 && (mean_b < .015 || mean_b > .985)))
        return true;
    if (va <= .0004 || vb <= .0004 || (ab / count - mean_a * mean_b) / std::sqrt(va * vb) >= .1)
        return false;
    // Same-pixel correlation also falls during ordinary head motion. Before
    // declaring a cut, look for the old texture at nearby offsets. This coarse
    // check is only a veto, never a motion estimate fed into stabilization.
    const int radius = int(std::min(a.width, a.height) * .10);
    for (int dy = -radius; dy <= radius; dy += 4)
        for (int dx = -radius; dx <= radius; dx += 4) {
            sum_a = sum_b = aa = bb = ab = count = 0;
            for (int y = std::max(bounds.top, bounds.top - dy);
                 y < std::min(bounds.bottom, bounds.bottom - dy); y += 8)
                for (int x = std::max(bounds.left, bounds.left - dx);
                     x < std::min(bounds.right, bounds.right - dx); x += 8) {
                    const double av = a.pixels[size_t(y) * a.width + x];
                    const double bv = b.pixels[size_t(y + dy) * b.width + x + dx];
                    if (!std::isfinite(av) || !std::isfinite(bv))
                        continue;
                    sum_a += av;
                    sum_b += bv;
                    aa += av * av;
                    bb += bv * bv;
                    ab += av * bv;
                    ++count;
                }
            const double variance_a = count * aa - sum_a * sum_a;
            const double variance_b = count * bb - sum_b * sum_b;
            if (count >= 16 && variance_a > 0 && variance_b > 0 &&
                (count * ab - sum_a * sum_b) / std::sqrt(variance_a * variance_b) > .35)
                return false;
        }
    return true;
}
double smoothstep(double x) {
    x = std::clamp(x, 0., 1.);
    return x * x * (3 - 2 * x);
}
ImageTransform scaled(ImageTransform value, double amount) {
    return {value.x * amount, value.y * amount, value.angle * amount};
}
std::vector<Point> features(const GrayImage &f, Bounds bounds) {
    struct Candidate {
        double score;
        Point p;
    };
    std::vector<Candidate> candidates;
    const int w = int(f.width);
    const auto [left, right, top, bottom] = bounds;
    // One strong corner per cell provides spatial coverage without quadratic suppression.
    for (int cy = top; cy < bottom; cy += 16)
        for (int cx = left; cx < right; cx += 16) {
            Candidate best{};
            for (int y = cy; y < std::min(cy + 16, bottom); y += 2)
                for (int x = cx; x < std::min(cx + 16, right); x += 2) {
                    double a = 0, b = 0, c = 0;
                    for (int j = -2; j <= 2; ++j)
                        for (int i = -2; i <= 2; ++i) {
                            const int k = (y + j) * w + x + i;
                            double gx = (f.pixels[k + 1] - f.pixels[k - 1]) * .5,
                                   gy = (f.pixels[k + w] - f.pixels[k - w]) * .5;
                            a += gx * gx;
                            b += gx * gy;
                            c += gy * gy;
                        }
                    double score = (a + c - std::sqrt((a - c) * (a - c) + 4 * b * b)) * .5;
                    if (score > best.score)
                        best = {score, {double(x), double(y)}};
                }
            if (best.score > .00015)
                candidates.push_back(best);
        }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    std::vector<Point> points;
    for (size_t i = 0; i < std::min(size_t(100), candidates.size()); ++i)
        points.push_back(candidates[i].p);
    return points;
}
std::optional<Point> track(const Pyramid &a, const Pyramid &b, Point p, Bounds bounds) {
    if (!bounds.contains(p))
        return std::nullopt;
    Point delta{};
    for (int level = int(a.size()) - 1; level >= 0; --level) {
        const auto &old = a[size_t(level)], &cur = b[size_t(level)];
        double scale = double(1 << level);
        Point o{p.x / scale, p.y / scale};
        if (level != int(a.size()) - 1) {
            delta.x *= 2;
            delta.y *= 2;
        }
        constexpr int radius = 3;
        constexpr unsigned n = 49;
        float values[n], gx[n], gy[n];
        unsigned k = 0;
        if (o.x < radius + 2 || o.y < radius + 2 || o.x >= old.width - radius - 2 ||
            o.y >= old.height - radius - 2)
            return std::nullopt;
        double aa = 0, bb = 0, cc = 0, mean_x = 0, mean_y = 0;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x, ++k) {
                values[k] = sample(old, o.x + x, o.y + y);
                gx[k] = (sample(old, o.x + x + 1, o.y + y) - sample(old, o.x + x - 1, o.y + y)) * .5f;
                gy[k] = (sample(old, o.x + x, o.y + y + 1) - sample(old, o.x + x, o.y + y - 1)) * .5f;
                mean_x += gx[k];
                mean_y += gy[k];
            }
        // Eliminate a free patch-brightness offset from the least-squares system.
        // A lighting change must not be interpreted as camera translation.
        mean_x /= n;
        mean_y /= n;
        for (unsigned i = 0; i < n; ++i) {
            gx[i] -= float(mean_x);
            gy[i] -= float(mean_y);
            aa += gx[i] * gx[i];
            bb += gx[i] * gy[i];
            cc += gy[i] * gy[i];
        }
        double determinant = aa * cc - bb * bb;
        if (!std::isfinite(determinant) || determinant < 1e-9)
            return std::nullopt;
        for (int iteration = 0; iteration < 12; ++iteration) {
            Point q{o.x + delta.x, o.y + delta.y};
            if (!bounds.contains({q.x * scale, q.y * scale}))
                return std::nullopt;
            if (q.x < radius + 1 || q.y < radius + 1 || q.x >= cur.width - radius - 2 ||
                q.y >= cur.height - radius - 2)
                return std::nullopt;
            double bx = 0, by = 0;
            k = 0;
            for (int y = -radius; y <= radius; ++y)
                for (int x = -radius; x <= radius; ++x, ++k) {
                    double error = sample(cur, q.x + x, q.y + y) - values[k];
                    bx += gx[k] * error;
                    by += gy[k] * error;
                }
            double dx = (cc * bx - bb * by) / determinant, dy = (aa * by - bb * bx) / determinant;
            if (!std::isfinite(dx) || !std::isfinite(dy) || std::abs(dx) > 5 || std::abs(dy) > 5)
                return std::nullopt;
            delta.x -= dx;
            delta.y -= dy;
            if (dx * dx + dy * dy < .0001)
                break;
        }
    }
    Point q{p.x + delta.x, p.y + delta.y};
    if (!bounds.contains(q))
        return std::nullopt;
    if (q.x < 4 || q.y < 4 || q.x >= b[0].width - 5 || q.y >= b[0].height - 5)
        return std::nullopt;
    // Apply the same free brightness offset as the optical-flow solve. Testing
    // unadjusted pixels here used to reject genuine matches after exposure changes.
    double offset = 0, error = 0;
    for (int y = -3; y <= 3; ++y)
        for (int x = -3; x <= 3; ++x)
            offset += sample(b[0], q.x + x, q.y + y) - sample(a[0], p.x + x, p.y + y);
    offset /= 49;
    for (int y = -3; y <= 3; ++y)
        for (int x = -3; x <= 3; ++x)
            error += std::abs(sample(a[0], p.x + x, p.y + y) - sample(b[0], q.x + x, q.y + y) + offset);
    if (!std::isfinite(error) || error / 49 > .06)
        return std::nullopt;
    return q;
}
Point transformed(ImageTransform t, Point p) {
    double c = std::cos(t.angle), s = std::sin(t.angle);
    return {c * p.x - s * p.y + t.x, s * p.x + c * p.y + t.y};
}
ImageTransform compose(ImageTransform a, ImageTransform b) {
    auto p = transformed(a, {b.x, b.y});
    return {p.x, p.y, a.angle + b.angle};
}
double distance(Point a, Point b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
ImageTransform fit(const std::vector<Match> &m, const std::vector<unsigned> &ids, bool roll) {
    Point a{}, b{};
    for (auto i : ids) {
        a.x += m[i].a.x;
        a.y += m[i].a.y;
        b.x += m[i].b.x;
        b.y += m[i].b.y;
    }
    a.x /= ids.size();
    a.y /= ids.size();
    b.x /= ids.size();
    b.y /= ids.size();
    double dot = 0, cross = 0;
    for (auto i : ids) {
        double ax = m[i].a.x - a.x, ay = m[i].a.y - a.y, bx = m[i].b.x - b.x, by = m[i].b.y - b.y;
        dot += ax * bx + ay * by;
        cross += ax * by - ay * bx;
    }
    ImageTransform t{};
    t.angle = roll ? std::atan2(cross, dot) : 0;
    auto q = transformed(t, a);
    t.x = b.x - q.x;
    t.y = b.y - q.y;
    return t;
}
std::pair<ImageTransform, std::vector<unsigned>> robust(const std::vector<Match> &m, bool roll) {
    std::vector<unsigned> best;
    ImageTransform chosen{};
    std::uint32_t rng = 0x5bd1e995;
    auto index = [&]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return unsigned(rng % m.size());
    };
    for (unsigned attempt = 0; attempt < 160; ++attempt) {
        unsigned i = index(), j = index();
        if (i == j)
            continue;
        if (roll && distance(m[i].a, m[j].a) < 15)
            continue;
        auto t = fit(m, {i, j}, roll);
        std::vector<unsigned> ids;
        for (unsigned k = 0; k < m.size(); ++k)
            if (distance(transformed(t, m[k].a), m[k].b) < .8)
                ids.push_back(k);
        if (ids.size() > best.size()) {
            best = std::move(ids);
            chosen = t;
        }
    }
    if (best.size() >= 2)
        chosen = fit(m, best, roll);
    return {chosen, best};
}
} // namespace
struct ImageStabilizer::Impl {
    Pyramid previous;
    ImageSettings settings;
    ImageResult last;
    ImageTransform correction;
    ImageTransform follow_velocity, follow_speed;
    std::uint64_t timestamp{};
    ImageTransform release_origin;
    std::uint64_t weak_since{};
    double acquisition{1};
};
ImageStabilizer::ImageStabilizer() : impl_(new Impl) {}
ImageStabilizer::~ImageStabilizer() = default;
void ImageStabilizer::reset() {
    *impl_ = Impl{};
}
ImageResult ImageStabilizer::process(const GrayImage &f, std::uint64_t time, const ImageSettings &raw) {
    const auto start = std::chrono::steady_clock::now();
    auto &state = *impl_;
    auto s = sanitize(raw);
    ImageResult result;
    result.width = f.width;
    result.height = f.height;
    result.time_ns = time;
    auto done = [&]() {
        result.processing_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        state.last = result;
        return result;
    };
    if (!good(f) || !time || s.region_left + s.region_right >= 95 || s.region_top + s.region_bottom >= 95) {
        reset();
        result.status = ImageStatus::invalid;
        return done();
    }
    if (time == state.timestamp && same(s, state.settings) && !state.previous.empty() &&
        state.previous[0].width == f.width && state.previous[0].height == f.height)
        return state.last;
    auto current = pyramid(f);
    if (state.previous.empty() || state.previous[0].width != f.width ||
        state.previous[0].height != f.height || !same(s, state.settings) || time <= state.timestamp ||
        time - state.timestamp > 200000000ULL) {
        state.previous = std::move(current);
        state.timestamp = time;
        state.settings = s;
        state.correction = {};
        state.follow_velocity = state.follow_speed = {};
        state.release_origin = {};
        state.weak_since = 0;
        state.acquisition = 1;
        return done();
    }
    result.previous_time_ns = state.timestamp;
    // Keep the complete coarse patch inside the chosen region, including its
    // gradient neighbors, bilinear extra sample and box-downsample footprint.
    const int guard = 6 * (1 << (current.size() - 1));
    Bounds bounds{int(std::ceil(f.width * s.region_left / 100)) + guard,
                  int(std::floor(f.width * (1 - s.region_right / 100))) - guard,
                  int(std::ceil(f.height * s.region_top / 100)) + guard,
                  int(std::floor(f.height * (1 - s.region_bottom / 100))) - guard};
    auto points = features(state.previous[0], bounds);
    std::vector<Match> matches;
    const Point center{(f.width - 1) * .5, (f.height - 1) * .5};
    for (auto p : points) {
        auto q = track(state.previous, current, p, bounds);
        if (!q)
            continue;
        auto back = track(current, state.previous, *q, bounds);
        if (!back || distance(p, *back) > .5)
            continue;
        matches.push_back({{p.x - center.x, p.y - center.y}, {q->x - center.x, q->y - center.y}});
    }
    result.tracked = unsigned(matches.size());
    result.status = ImageStatus::weak;
    if (matches.size() >= 8) {
        auto [motion, inliers] = robust(matches, s.track_roll);
        result.inliers = unsigned(inliers.size());
        double support = double(inliers.size()) / std::max(size_t(1), points.size());
        result.confidence = support;
        double error = 0;
        for (auto i : inliers)
            error += std::pow(distance(transformed(motion, matches[i].a), matches[i].b), 2);
        double rms = inliers.empty() ? 100 : std::sqrt(error / inliers.size());
        if (inliers.size() >= 8 && support >= .4 && rms < .5 &&
            std::hypot(motion.x, motion.y) < std::min(f.width, f.height) * .10 &&
            std::abs(motion.angle) < 8 * pi / 180) {
            result.status = ImageStatus::tracking;
            result.motion = motion;
            double strength = s.smoothing / 100, tau = .02 + .48 * strength * strength;
            double retention = s.smoothing <= 0 ? 0 : std::exp(-double(time - state.timestamp) * 1e-9 / tau);
            double horizontal = retention, vertical = retention, roll = retention;
            if (s.motion.separate_axes || s.motion.turn_follow > 0) {
                const double dt = double(time - state.timestamp) * 1e-9;
                horizontal = image_retention(s.motion.separate_axes ? s.motion.yaw : s.smoothing,
                                             motion.x / f.width * 90 / dt, state.follow_velocity.x,
                                             state.follow_speed.x, dt, s.motion);
                vertical = image_retention(s.motion.separate_axes ? s.motion.pitch : s.smoothing,
                                           motion.y / f.height * 90 / dt, state.follow_velocity.y,
                                           state.follow_speed.y, dt, s.motion);
                roll = image_retention(s.motion.separate_axes ? s.motion.roll : s.smoothing,
                                       motion.angle * 180 / pi / dt, state.follow_velocity.angle,
                                       state.follow_speed.angle, dt, s.motion);
            }
            state.weak_since = 0;
            state.acquisition = std::min(1., state.acquisition + double(time - state.timestamp) * 1e-9 / .10);
            auto correction = compose(scaled(motion, smoothstep(state.acquisition)), state.correction);
            correction.x = std::clamp(correction.x * horizontal, -double(f.width) * s.max_shift_percent / 100,
                                      f.width * s.max_shift_percent / 100);
            correction.y = std::clamp(correction.y * vertical, -double(f.height) * s.max_shift_percent / 100,
                                      f.height * s.max_shift_percent / 100);
            correction.angle = s.track_roll
                                   ? std::clamp(correction.angle * roll, -s.max_roll_degrees * pi / 180,
                                                s.max_roll_degrees * pi / 180)
                                   : 0;
            result.correction = correction;
        } else if (std::hypot(motion.x, motion.y) >= std::min(f.width, f.height) * .10)
            result.status = ImageStatus::scene_change;
    }
    if (result.status != ImageStatus::tracking && scene_discontinuity(state.previous[0], current[0], bounds))
        result.status = ImageStatus::scene_change;
    if (result.status != ImageStatus::tracking) {
        state.follow_velocity = state.follow_speed = {};
        state.acquisition = 0;
        if (result.status == ImageStatus::weak) {
            if (!state.weak_since) {
                state.weak_since = state.timestamp;
                state.release_origin = state.correction;
            }
            // Bound the entire release to 150 ms. Do not extrapolate unknown
            // motion or pass this fading offset to calibration as a measurement.
            const double fade = 1 - smoothstep(double(time - state.weak_since) * 1e-9 / .15);
            result.correction = scaled(state.release_origin, fade);
        } else {
            state.weak_since = 0;
            state.release_origin = {};
        }
    }
    state.correction = result.correction;
    state.previous = std::move(current);
    state.timestamp = time;
    state.settings = s;
    return done();
}
ImageWarp make_image_warp(std::uint32_t width, std::uint32_t height, const Settings &settings,
                          const ImageResult &result) {
    Settings framing = settings;
    framing.automatic_fov = false;
    framing.horizontal_fov = 90;
    framing.vertical_fov = 90;
    auto base = make_warp(width, height, framing);
    ImageWarp out;
    out.width = base.width;
    out.height = base.height;
    auto origin = map_uv(base, 0, 0), xr = map_uv(base, 1, 0), yr = map_uv(base, 0, 1);
    double sx = base.uv_right - base.uv_left, sy = base.uv_bottom - base.uv_top;
    ImageTransform desired = result.correction;
    if ((result.status != ImageStatus::tracking && result.status != ImageStatus::weak) || !result.width ||
        !result.height || !std::isfinite(desired.x) || !std::isfinite(desired.y) ||
        !std::isfinite(desired.angle))
        desired = {};
    auto compute = [&](double fraction) {
        double angle = desired.angle * fraction, c = std::cos(angle), s = std::sin(angle);
        double aw = result.width ? result.width : 1, ah = result.height ? result.height : 1;
        auto apply = [&](std::array<double, 2> uv) {
            double x = ((uv[0] - base.uv_left) / sx - .5) * aw, y = ((uv[1] - base.uv_top) / sy - .5) * ah;
            return std::array<double, 2>{
                base.uv_left + (.5 + (c * x - s * y + desired.x * fraction) / aw) * sx,
                base.uv_top + (.5 + (s * x + c * y + desired.y * fraction) / ah) * sy};
        };
        auto a = apply(origin), b = apply(xr), d = apply(yr);
        out.row_x = {b[0] - a[0], d[0] - a[0], a[0]};
        out.row_y = {b[1] - a[1], d[1] - a[1], a[1]};
        for (double u : {0., 1.})
            for (double v : {0., 1.}) {
                double x = out.row_x[0] * u + out.row_x[1] * v + out.row_x[2],
                       y = out.row_y[0] * u + out.row_y[1] * v + out.row_y[2];
                if (x < base.uv_left - 1e-9 || x > base.uv_right + 1e-9 || y < base.uv_top - 1e-9 ||
                    y > base.uv_bottom + 1e-9)
                    return false;
            }
        return true;
    };
    if (!compute(1) && !settings.mirror_edges) {
        double lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) {
            double mid = (lo + hi) / 2;
            if (compute(mid))
                lo = mid;
            else
                hi = mid;
        }
        out.correction_fraction = lo;
        compute(lo);
    }
    return out;
}
} // namespace vrst
