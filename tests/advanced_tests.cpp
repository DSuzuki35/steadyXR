#include "vrst/advanced.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace vrst;
namespace {
unsigned checks{}, failures{};
constexpr std::uint64_t tick = 16666667, start = 1000000000;
void check(bool condition, const char *name) {
    ++checks;
    failures += !condition;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
}
bool near(double a, double b, double tolerance = 1e-7) {
    return std::isfinite(a) && std::abs(a - b) < tolerance;
}
Pose pose(unsigned i, Quat q) {
    Pose p;
    p.session = 1;
    p.sequence = i + 1;
    p.time_ns = start + tick * i;
    p.orientation = q;
    p.flags = TrackingValid | FramePose;
    return p;
}
PoseResult advance(PoseHistory &h, unsigned i, Quat q) {
    const auto p = pose(i, q);
    h.push(p);
    return h.sample(p.time_ns);
}
double yaw_lag(MotionSettings s, bool alternate = false, double sign = 1) {
    PoseHistory h;
    h.set_smoothing(s);
    PoseResult r;
    for (unsigned i = 0; i < 90; ++i)
        r = advance(h, i, axis_angle({0, 1, 0}, sign * (alternate ? (i % 2 ? .008 : -.008) : i * .01)));
    return angle_between(r.correction, {});
}
GrayImage frame(double dx = 0, double dy = 0, double angle = 0) {
    GrayImage f{320, 180, std::vector<float>(320 * 180)};
    const double c = std::cos(angle), s = std::sin(angle);
    for (unsigned y = 0; y < f.height; ++y)
        for (unsigned x = 0; x < f.width; ++x) {
            double px = x - 159.5 - dx, py = y - 89.5 - dy;
            double a = c * px + s * py + 159.5, b = -s * px + c * py + 89.5;
            f.pixels[y * f.width + x] = float(.5 + .13 * std::sin(.19 * a + .03 * b) +
                .12 * std::cos(.23 * b - .01 * a) + .11 * std::sin(.13 * a + .21 * b) +
                .10 * std::cos(.29 * a - .17 * b));
        }
    return f;
}
ImageSettings image_settings() {
    ImageSettings s;
    s.smoothing = 100;
    s.region_top = 10;
    s.max_shift_percent = 5;
    s.max_roll_degrees = 5;
    return s;
}
bool image_inside(const ImageWarp &w, const Settings &s) {
    for (double x : {0., 1.})
        for (double y : {0., 1.}) {
            const double u = w.row_x[0] * x + w.row_x[1] * y + w.row_x[2];
            const double v = w.row_y[0] * x + w.row_y[1] * y + w.row_y[2];
            if (!std::isfinite(u) || !std::isfinite(v) || u < s.input_left / 100 - 1e-8 ||
                u > 1 - s.input_right / 100 + 1e-8 || v < s.input_top / 100 - 1e-8 ||
                v > 1 - s.input_bottom / 100 + 1e-8)
                return false;
        }
    return true;
}
void pose_tests() {
    MotionSettings s;
    s.amount = 100;
    s.separate_axes = true;
    s.yaw = 100;
    s.pitch = s.roll = 0;
    PoseHistory yaw, pitch, roll;
    yaw.set_smoothing(s); pitch.set_smoothing(s); roll.set_smoothing(s);
    advance(yaw, 0, {}); advance(pitch, 0, {}); advance(roll, 0, {});
    auto y = advance(yaw, 1, axis_angle({0, 1, 0}, .1));
    auto p = advance(pitch, 1, axis_angle({1, 0, 0}, .1));
    auto r = advance(roll, 1, axis_angle({0, 0, 1}, .1));
    check(angle_between(y.correction, {}) > .09 && angle_between(p.correction, {}) < 1e-7 &&
              angle_between(r.correction, {}) < 1e-7, "pose axes isolate yaw from pitch and roll");
    s.amount = 0;
    yaw.set_smoothing(s);
    check(angle_between(yaw.sample(start + tick).correction, {}) > .09,
          "separate axes override a zero common amount");
    PoseHistory legacy, advanced_off;
    legacy.set_smoothing(63.);
    MotionSettings off; off.amount = 63; off.yaw = 0; off.pitch = 100; off.roll = 12;
    advanced_off.set_smoothing(off);
    bool exact = true;
    for (unsigned i = 0; i < 70; ++i) {
        const auto q = normalized(multiply(axis_angle({0, 1, 0}, .2 * std::sin(i * .2)),
            axis_angle({1, 0, 1}, .04 * std::cos(i * .12))));
        auto a = advance(legacy, i, q), b = advance(advanced_off, i, q);
        exact &= a.correction.x == b.correction.x && a.correction.y == b.correction.y &&
                 a.correction.z == b.correction.z && a.correction.w == b.correction.w;
    }
    check(exact, "advanced controls off preserve exact scalar legacy behavior");
    s = {}; s.amount = 100;
    const double normal = yaw_lag(s), jitter = yaw_lag(s, true);
    s.turn_follow = 100; s.turn_speed = 20;
    check(yaw_lag(s) < normal * .45 && yaw_lag(s, false, -1) < normal * .45,
          "signed follower accelerates sustained turns in both directions");
    check(yaw_lag(s, true) > jitter * .85, "signed follower retains alternating jitter suppression");
    PoseHistory h; h.set_smoothing(s);
    for (unsigned i = 0; i < 50; ++i) advance(h, i, axis_angle({0, 1, 0}, i * .01));
    auto gap = pose(100, axis_angle({0, 1, 0}, .7));
    h.push(gap);
    check(angle_between(h.sample(gap.time_ns).correction, {}) < 1e-7, "pose gap clears smooth and follow state");
    auto recenter = advance(h, 101, axis_angle({0, 1, 0}, 2));
    check(angle_between(recenter.correction, {}) < 1e-7, "pose recenter clears correction");
    s.separate_axes = true; s.pitch = 85; s.yaw = 65; s.roll = 95;
    h.clear(); h.set_smoothing(s);
    bool sane = true;
    for (unsigned i = 0; i < 150; ++i) {
        auto q = multiply(axis_angle({0, 1, 0}, i * .045), axis_angle({1, 0, 0}, 1.55 + .1 * std::sin(i)));
        if (i % 2) q = {-q.x, -q.y, -q.z, -q.w};
        auto a = advance(h, i, q);
        sane &= a.valid && valid(a.correction) && near(a.target_orientation.x * a.target_orientation.x +
            a.target_orientation.y * a.target_orientation.y + a.target_orientation.z * a.target_orientation.z +
            a.target_orientation.w * a.target_orientation.w, 1);
    }
    check(sane, "local quaternion smoothing stays normalized through poles and sign flips");
    h.set_smoothing(0.);
    check(angle_between(h.sample(start + tick * 149).correction, {}) < 1e-7,
          "legacy overload disables previous advanced settings");
}
void image_tests() {
    auto s = image_settings();
    s.smoothing = 0;
    ImageStabilizer tracker;
    auto warm = tracker.process(frame(), start, s);
    auto r = tracker.process(frame(2.4, -1.3, .008), start + tick, s);
    check(r.status == ImageStatus::tracking && near(r.motion.x, 2.4, .18) &&
          near(r.motion.y, -1.3, .18) && near(r.motion.angle, .008, .0015) && r.correction.x == 0,
          "measured image motion is exposed independently of smoothing");
    check(warm.time_ns == start && warm.previous_time_ns == 0 && r.time_ns == start + tick &&
              r.previous_time_ns == start, "measured motion carries captured frame interval");
    const auto duplicate = tracker.process(frame(2.4, -1.3, .008), start + tick, s);
    check(duplicate.time_ns == r.time_ns && duplicate.previous_time_ns == r.previous_time_ns &&
              duplicate.motion.x == r.motion.x, "duplicate processing preserves measured interval");
    GrayImage flat{320, 180, std::vector<float>(320 * 180, .5f)};
    r = tracker.process(flat, start + 2 * tick, s);
    check(r.status != ImageStatus::tracking && r.motion.x == 0 && r.motion.y == 0 && r.motion.angle == 0,
          "failed tracking exposes no measured motion");
    tracker.reset(); s = image_settings();
    s.motion.separate_axes = true; s.motion.yaw = 100; s.motion.pitch = s.motion.roll = 0;
    tracker.process(frame(), start, s);
    r = tracker.process(frame(2.4, -1.3, .008), start + tick, s);
    check(r.status == ImageStatus::tracking && r.correction.x > 2 && r.correction.y == 0 &&
              r.correction.angle == 0, "image axes isolate horizontal correction");
    s.motion.yaw = 0; s.motion.pitch = 100; s.motion.roll = 100;
    check(tracker.process(frame(), start + 2 * tick, s).status == ImageStatus::warming,
          "advanced image settings changes reset history");
    r = tracker.process(frame(2.4, -1.3, .008), start + 3 * tick, s);
    check(r.correction.x == 0 && r.correction.y < -1 && r.correction.angle > .006,
          "image axes independently enable vertical and roll correction");
    s = image_settings();
    ImageStabilizer ordinary, followed, alternate, alternate_followed;
    auto follow = s; follow.motion.turn_follow = 100; follow.motion.turn_speed = 10;
    ImageResult a, b, c, d;
    for (unsigned i = 0; i < 60; ++i) {
        const auto moving = frame(i * .7), jitter = frame(i % 2 ? .7 : -.7);
        a = ordinary.process(moving, start + i * tick, s);
        b = followed.process(moving, start + i * tick, follow);
        c = alternate.process(jitter, start + i * tick, s);
        d = alternate_followed.process(jitter, start + i * tick, follow);
    }
    check(a.status == ImageStatus::tracking && b.status == ImageStatus::tracking &&
              std::abs(b.correction.x) < std::abs(a.correction.x) * .5,
          "image follower accelerates sustained horizontal motion");
    check(std::abs(d.correction.x) > std::abs(c.correction.x) * .85,
          "image follower retains alternating jitter suppression");
}
void crop_tests() {
    CropSettings s; s.enabled = true; s.max_extra = 10; s.speed = 4;
    CropController c;
    check(near(c.update(20, 10, s, start), 10), "automatic crop starts at base");
    check(near(c.update(20, 10, s, start + 100000000), 10.4), "automatic crop rises at configured rate");
    check(near(c.update(20, 10, s, start + 100000000), 10.4), "duplicate crop time does not advance");
    check(near(c.update(10, 10, s, start + 200000000), 10.2), "automatic crop relaxes at half rate");
    check(near(c.update(30, 10, s, start + 5000000000), 10.2), "idle gap cannot force an instant zoom");
    c.reset();
    check(near(c.update(30, 10, s, start), 10), "crop reset clears accumulated extra");
    double out = 0;
    for (unsigned i = 1; i <= 150; ++i) out = c.update(100, 10, s, start + i * 100000000ULL);
    check(near(out, 20), "crop maximum extra is enforced");
    check(near(c.update(100, 38, s, start + 15100000000ULL), 38), "base setting changes reset crop");
    for (unsigned i = 152; i <= 170; ++i) out = c.update(100, 38, s, start + i * 100000000ULL);
    check(near(out, 40), "total crop never exceeds forty percent");
    s.enabled = false;
    check(near(c.update(100, 10, s, start), 10), "disabled crop is exact base identity");
    s.enabled = true; s.max_extra = 0;
    check(near(c.update(100, 10, s, start + tick), 10), "zero extra is exact base identity");
    s.max_extra = 10;
    check(near(c.update(std::numeric_limits<double>::quiet_NaN(), 10, s, start + 2 * tick), 10),
          "invalid crop requirement safely targets base");
    check(std::isfinite(c.update(30, std::numeric_limits<double>::infinity(), s, 0)),
          "invalid base and timestamp produce finite crop");
    Settings framing; framing.output_16_9 = false; framing.stabilization_crop = 2;
    ImageResult r; r.status = ImageStatus::tracking; r.width = 320; r.height = 180; r.correction.x = 16;
    const double image_need = required_image_crop(1920, 1080, framing, r, 20);
    check(near(image_need, 10, 1e-5), "image crop requirement fits known five-percent translation");
    r.status = ImageStatus::weak;
    check(near(required_image_crop(1920, 1080, framing, r, 20), image_need),
          "automatic crop reserves the correction still rendered during weak tracking");
    r.status = ImageStatus::tracking;
    const auto q = axis_angle({0, 1, 0}, 5 * pi / 180);
    framing.horizontal_fov = framing.vertical_fov = 90;
    const double pose_need = required_pose_crop(1920, 1080, framing, q, {}, 30);
    const double t = std::tan(5 * pi / 180);
    check(near(pose_need, 100 * (1 - (1 - t) / (1 + t)), 1e-5),
          "pose crop requirement fits analytic ninety-degree yaw projection");
    check(required_pose_crop(1920, 1080, framing, q, {}, 0) == 2 &&
              required_image_crop(1920, 1080, framing, r, 0) == 2, "zero-extra requirements preserve base exactly");
    check(required_pose_crop(0, 1080, framing, q, {}, 30) == 2 &&
              required_image_crop(1920, 0, framing, r, 30) == 2, "invalid dimensions retain base crop");
    bool safe = true;
    framing.input_left = 7; framing.input_right = 13; framing.input_top = 3; framing.input_bottom = 11;
    for (unsigned i = 0; i < 60; ++i) {
        framing.framing_x = 90 * std::sin(i); framing.framing_y = 75 * std::cos(i * .8);
        framing.output_16_9 = i % 2;
        r.correction = {50 * std::sin(i), 30 * std::cos(i), .2 * std::sin(i * .3)};
        auto use = framing;
        use.stabilization_crop = required_image_crop(3840, 2160, framing, r, 9);
        safe &= use.stabilization_crop >= 2 && use.stabilization_crop <= 11 &&
                image_inside(make_image_warp(3840, 2160, use, r), use);
        const auto desired = axis_angle({1, 2, 1}, .7 * std::sin(i));
        use.stabilization_crop = required_pose_crop(3840, 2160, framing, desired, {}, 9);
        safe &= use.stabilization_crop >= 2 && use.stabilization_crop <= 11 &&
                corners_inside(make_warp(3840, 2160, use, desired));
    }
    check(safe, "bounded extra crop retains corner safety through existing correction limiters");
}
} // namespace
int main() {
    pose_tests(); image_tests(); crop_tests();
    std::printf("%u/%u advanced checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
