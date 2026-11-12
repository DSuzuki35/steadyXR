#include "vrst/core.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace vrst;
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void near(double a, double b, double eps = 1e-6) {
    if (std::abs(a - b) > eps || !std::isfinite(a))
        throw std::runtime_error("expected " + std::to_string(b) + ", got " + std::to_string(a));
}
Pose pose(std::uint64_t seq, std::uint64_t ns, double yaw, std::uint32_t flags = TrackingValid | FramePose) {
    Pose p;
    p.session = 42;
    p.sequence = seq;
    p.time_ns = ns;
    p.orientation = axis_angle({0, 1, 0}, yaw * pi / 180);
    p.flags = flags;
    return p;
}
int main() {
    std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"horizon leveling removes a sustained head tilt",
         [] {
             auto source = axis_angle({0, 0, 1}, 25 * pi / 180);
             auto corrected = multiply(source, horizon_correction(source, source, 100, 45));
             near(angle_between(corrected, {}), 0);
         }},
        {"horizon leveling preserves yaw and pitch look direction",
         [] {
             auto target = multiply(multiply(axis_angle({0, 1, 0}, .7), axis_angle({1, 0, 0}, -.3)),
                                    axis_angle({0, 0, 1}, .4));
             auto corrected = multiply(target, horizon_correction(target, target, 100, 45));
             auto before = rotate(target, {0, 0, -1}), after = rotate(corrected, {0, 0, -1});
             near(before.x, after.x);
             near(before.y, after.y);
             near(before.z, after.z);
             near(rotate(corrected, {1, 0, 0}).y, 0);
         }},
        {"horizon strength and maximum tilt correction are independent",
         [] {
             auto source = axis_angle({0, 0, 1}, 30 * pi / 180);
             near(angle_between(multiply(source, horizon_correction(source, source, 50, 60)), {}),
                  15 * pi / 180);
             near(angle_between(multiply(source, horizon_correction(source, source, 100, 10)), {}),
                  20 * pi / 180);
         }},
        {"horizon disabled preserves ordinary smoothing",
         [] {
             auto source = axis_angle({0, 1, 0}, .4), target = axis_angle({0, 1, 0}, .2);
             near(angle_between(horizon_correction(source, target, 0), multiply(conjugate(source), target)),
                  0);
         }},
        {"vertical gaze avoids undefined horizon roll",
         [] {
             auto source = multiply(axis_angle({1, 0, 0}, pi / 2), axis_angle({0, 0, 1}, .4));
             auto correction = horizon_correction(source, source, 100, 45);
             check(valid(correction), "vertical gaze nonfinite");
             near(angle_between(correction, {}), 0);
         }},
        {"pose sampling supplies matching source and target for leveling",
         [] {
             PoseHistory h;
             h.set_smoothing(0);
             auto p = pose(1, 1000000000, 0);
             p.orientation = axis_angle({0, 0, 1}, .3);
             h.push(p);
             auto r = h.sample(p.time_ns);
             check(r.valid, "missing pose");
             near(angle_between(r.source_orientation, p.orientation), 0);
             near(angle_between(r.target_orientation, p.orientation), 0);
             near(angle_between(multiply(r.source_orientation,
                                         horizon_correction(r.source_orientation, r.target_orientation, 100)),
                                {}),
                  0);
         }},
        {"quaternion rotates eye rays with correct handedness",
         [] {
             auto v = rotate(axis_angle({0, 1, 0}, pi / 2), {0, 0, -1});
             near(v.x, -1);
             near(v.y, 0);
             near(v.z, 0);
         }},
        {"quaternion inverse restores direction",
         [] {
             auto q = axis_angle({1, 2, 3}, .7);
             auto v = rotate(conjugate(q), rotate(q, {.2, .4, -1}));
             near(v.x, .2);
             near(v.y, .4);
             near(v.z, -1);
         }},
        {"slerp takes shortest arc across yaw wrap",
         [] {
             auto q =
                 slerp(axis_angle({0, 1, 0}, 179 * pi / 180), axis_angle({0, 1, 0}, -179 * pi / 180), .5);
             auto v = rotate(q, {0, 0, -1});
             near(v.x, 0);
             near(v.z, 1);
         }},
        {"projection uses tangent half angles",
         [] {
             auto p = symmetric_projection(90, 90);
             near(p.left, -1);
             near(p.right, 1);
             near(p.up, 1);
             near(p.down, -1);
             check(!valid(Projection{1, -1, 1, -1}), "reversed projection accepted");
         }},
        {"identity mapping preserves all image corners",
         [] {
             Settings s;
             s.stabilization_crop = 0;
             s.output_16_9 = false;
             auto w = make_warp(1000, 1000, s);
             auto a = map_uv(w, 0, 0);
             auto b = map_uv(w, 1, 1);
             near(a[0], 0);
             near(a[1], 0);
             near(b[0], 1);
             near(b[1], 1);
         }},
        {"input trim changes sampled pixel region",
         [] {
             Settings s;
             s.stabilization_crop = 0;
             s.output_16_9 = false;
             s.input_left = 10;
             s.input_right = 20;
             s.input_top = 5;
             s.input_bottom = 15;
             auto w = make_warp(1000, 1000, s);
             auto a = map_uv(w, 0, 0);
             auto b = map_uv(w, 1, 1);
             near(a[0], .1);
             near(a[1], .05);
             near(b[0], .8);
             near(b[1], .85);
             check(w.width == 700 && w.height == 800, "trim dimensions");
         }},
        {"16:9 framing crops a square vertically without stretching",
         [] {
             Settings s;
             s.stabilization_crop = 0;
             auto w = make_warp(1600, 1600, s);
             check(w.width == 1600 && w.height == 900, "aspect dimensions");
             auto a = map_uv(w, 0, 0);
             auto b = map_uv(w, 1, 1);
             near(a[0], 0);
             near(a[1], .21875);
             near(b[1], .78125);
         }},
        {"16:9 remains exact after fractional input trim",
         [] {
             Settings s;
             s.input_left = .1;
             auto w = make_warp(3840, 2160, s);
             check(w.width * 9 == w.height * 16, "rounded output is not 16:9");
         }},
        {"yaw correction maps center to the correct source pixel",
         [] {
             Settings s;
             s.horizontal_fov = 90;
             s.vertical_fov = 90;
             s.output_16_9 = false;
             s.stabilization_crop = 30;
             auto w = make_warp(1000, 1000, s, axis_angle({0, 1, 0}, 5 * pi / 180));
             auto uv = map_uv(w, .5, .5);
             near(uv[0], .4562556682);
             near(uv[1], .5);
         }},
        {"large rotations are limited to available rendered pixels",
         [] {
             Settings s;
             s.framing_x = 60;
             s.framing_y = -40;
             auto w = make_warp(3840, 2160, s, axis_angle({1, 1, 1}, .8));
             check(w.correction_fraction < 1, "correction was not clamped");
             check(corners_inside(w), "black edge exposed");
         }},
        {"zero margin cannot sustain a yaw correction",
         [] {
             Settings s;
             s.output_16_9 = false;
             s.stabilization_crop = 0;
             auto w = make_warp(1000, 1000, s, axis_angle({0, 1, 0}, .2));
             check(w.correction_fraction < .0001, "unavailable pixels requested");
         }},
        {"edge reserves retain two percent left and twenty percent right",
         [] {
             Settings s;
             s.horizontal_fov = s.vertical_fov = 90;
             s.output_16_9 = false;
             s.stabilization_crop = 40; // Enabled edges replace the uniform reserve.
             s.edge_reserve = {true, 2, 20, 10, 15};
             const auto w = make_warp(1000, 1000, s);
             const auto a = map_uv(w, 0, 0), b = map_uv(w, 1, 1);
             near(a[0], .02);
             near(b[0], .8);
             near(a[1], .1);
             near(b[1], .85);
         }},
        {"asymmetric reserves limit correction against the actual available side",
         [] {
             Settings s;
             s.horizontal_fov = s.vertical_fov = 90;
             s.output_16_9 = false;
             s.edge_reserve = {true, 2, 20, 20, 20};
             const auto toward_left = make_warp(1000, 1000, s, axis_angle({0, 1, 0}, .12));
             const auto toward_right = make_warp(1000, 1000, s, axis_angle({0, 1, 0}, -.12));
             check(toward_left.correction_fraction < .2 && toward_right.correction_fraction > .99,
                   "asymmetric reserve was centered before correction limiting");
             check(corners_inside(toward_left) && corners_inside(toward_right), "reserve exposed black edge");
             s.mirror_edges = true;
             const auto reflected = make_warp(1000, 1000, s, axis_angle({0, 1, 0}, 1.3));
             check(reflected.correction_fraction < 1, "reflected projection crossed its horizon");
             for (double u : {0., 1.})
                 for (double v : {0., 1.}) {
                     const auto uv = map_uv(reflected, u, v);
                     check(std::isfinite(uv[0]) && std::isfinite(uv[1]), "reflected reserve became nonfinite");
                 }
         }},
        {"symmetric edge reserves match legacy uniform crop",
         [] {
             Settings s;
             s.horizontal_fov = s.vertical_fov = 90;
             s.framing_zoom = 12;
             for (bool widescreen : {false, true}) {
                 s.output_16_9 = widescreen;
                 s.edge_reserve.enabled = false;
                 const auto legacy = make_warp(2000, 1600, s, axis_angle({0, 1, 0}, .03));
                 s.edge_reserve.enabled = true;
                 const auto separate = make_warp(2000, 1600, s, axis_angle({0, 1, 0}, .03));
                 near(separate.output.left, legacy.output.left);
                 near(separate.output.right, legacy.output.right);
                 near(separate.output.up, legacy.output.up);
                 near(separate.output.down, legacy.output.down);
                 near(separate.correction_fraction, legacy.correction_fraction);
             }
         }},
        {"invalid edge values are finite defaults or clamped to thirty five percent",
         [] {
             Settings s;
             s.horizontal_fov = s.vertical_fov = 90;
             s.output_16_9 = false;
             s.edge_reserve = {true, -10, 80, NAN, INFINITY};
             const auto w = make_warp(1000, 1000, s);
             const auto a = map_uv(w, 0, 0), b = map_uv(w, 1, 1);
             near(a[0], 0);
             near(b[0], .65);
             near(a[1], .05);
             near(b[1], .95);
         }},
        {"90 to 60 selects actual source frames instead of interpolating",
         [] {
             PoseHistory h;
             h.set_smoothing(0);
             h.push(pose(0, 1000000000, 0));
             h.push(pose(1, 1011111111, 10));
             h.push(pose(2, 1022222222, 20));
             h.push(pose(3, 1033333333, 30));
             check(h.sample(1016666667).sequence == 1, "wrong 60Hz frame");
             check(h.sample(1033333333).sequence == 3, "wrong next frame");
         }},
        {"live timing preserves the earlier packet and reports the interpolated target",
         [] {
             PoseHistory h;
             h.set_smoothing(0);
             h.push(pose(10, 6000000000ULL, 0, TrackingValid));
             h.push(pose(11, 6020000000ULL, 20, TrackingValid));
             const auto r = h.sample(6010000000ULL);
             check(r.valid && r.session == 42 && r.sequence == 10, "source packet identity lost");
             check(r.time_ns == 6000000000ULL, "interpolation replaced raw packet timestamp");
             check(r.interpolated && r.sample_time_ns == 6010000000ULL,
                   "effective interpolation timestamp missing");
             near(angle_between(r.source_orientation, axis_angle({0, 1, 0}, 10 * pi / 180)), 0);
         }},
        {"frame timing remains tied to the selected image packet",
         [] {
             PoseHistory h;
             h.push(pose(10, 6000000000ULL, 0));
             h.push(pose(11, 6020000000ULL, 20));
             const auto r = h.sample(6010000000ULL);
             check(r.valid && r.session == 42 && r.sequence == 10, "frame packet identity lost");
             check(!r.interpolated && r.time_ns == 6000000000ULL && r.sample_time_ns == 6000000000ULL,
                   "frame pose falsely labeled interpolated");
             near(angle_between(r.source_orientation, {}), 0);
         }},
        {"live timing reports interpolation only between eligible samples",
         [] {
             struct Case {
                 std::uint64_t next_time, sample_time, expected_time;
                 std::uint32_t next_flags;
             };
             for (const auto &c : {Case{6020000000ULL, 6000000000ULL, 6000000000ULL, TrackingValid},
                                   Case{6020000000ULL, 6030000000ULL, 6020000000ULL, TrackingValid},
                                   Case{6020000000ULL, 6010000000ULL, 6000000000ULL, 0},
                                   Case{6100000000ULL, 6010000000ULL, 6000000000ULL, TrackingValid}}) {
                 PoseHistory h;
                 h.push(pose(10, 6000000000ULL, 0, TrackingValid));
                 h.push(pose(11, c.next_time, 20, c.next_flags));
                 const auto r = h.sample(c.sample_time);
                 check(r.valid && !r.interpolated && r.time_ns == c.expected_time &&
                           r.sample_time_ns == c.expected_time,
                       "held or exact live sample falsely labeled interpolated");
             }
         }},
        {"unavailable poses expose no previous packet timing",
         [] {
             PoseHistory h;
             h.push(pose(10, 6000000000ULL, 0, TrackingValid));
             h.push(pose(11, 6020000000ULL, 20, 0));
             for (auto at : {5990000000ULL, 6020000000ULL, 6300000000ULL}) {
                 const auto r = h.sample(at);
                 check(!r.valid && !r.session && !r.time_ns && !r.sample_time_ns && !r.interpolated,
                       "unavailable pose retained timing metadata");
             }
             h.clear();
             const auto r = h.sample(6000000000ULL);
             check(!r.valid && !r.session && !r.sample_time_ns, "clear retained pose timing");
         }},
        {"smoothing suppresses head jitter and zero disables it",
         [] {
             PoseHistory h;
             h.set_smoothing(100);
             h.push(pose(1, 1000000000, 0));
             h.push(pose(2, 1010000000, 10));
             auto a = h.sample(1010000000);
             check(a.valid, "missing sample");
             double d = angle_between(a.correction, {}) * 180 / pi;
             check(d > 9 && d < 10, "jitter not damped");
             h.set_smoothing(0);
             near(angle_between(h.sample(1010000000).correction, {}), 0);
         }},
        {"recenter resets smoothing instead of panning across it",
         [] {
             PoseHistory h;
             h.push(pose(1, 1000000000, 0));
             h.push(pose(2, 1010000000, 70));
             near(angle_between(h.sample(1010000000).correction, {}), 0);
         }},
        {"tracking gaps and invalid samples do not retain old correction",
         [] {
             PoseHistory h;
             h.push(pose(1, 1000000000, 0));
             h.push(pose(2, 1010000000, 10));
             h.push(pose(3, 1020000000, 10, 0));
             check(!h.sample(1020000000).valid, "tracking invalid ignored");
             h.push(pose(4, 1030000000, 10));
             near(angle_between(h.sample(1030000000).correction, {}), 0);
             check(!h.sample(2000000000).valid, "stale sample accepted");
         }},
        {"pose history rejects duplicates reorder and nonfinite data",
         [] {
             PoseHistory h;
             check(h.push(pose(2, 1010000000, 0)), "first pose rejected");
             check(!h.push(pose(1, 1000000000, 1)), "reordered accepted");
             check(!h.push(pose(2, 1010000000, 1)), "duplicate accepted");
             auto p = pose(3, 1020000000, 2);
             p.orientation.w = std::numeric_limits<double>::quiet_NaN();
             check(!h.push(p), "NaN accepted");
         }},
        {"sender restart clears old session samples",
         [] {
             PoseHistory h;
             h.push(pose(1, 1000000000, 0));
             auto p = pose(1, 2000000000, 30);
             p.session = 43;
             h.push(p);
             check(h.size() == 1, "old session retained");
             const auto r = h.sample(2000000000);
             check(r.valid && r.session == 43 && r.sample_time_ns == 2000000000,
                   "restart retained previous session timing");
         }}};
    int failed = 0;
    for (auto &c : cases) {
        try {
            c.second();
            std::cout << "PASS " << c.first << '\n';
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "FAIL " << c.first << ": " << e.what() << '\n';
        }
    }
    std::cout << cases.size() - failed << "/" << cases.size() << " passed\n";
    return failed ? 1 : 0;
}
