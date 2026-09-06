#include "vrst/calibration.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vrst;
namespace {
void check(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
void near(double actual, double expected, double tolerance) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          "expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}
struct Angles {
    double yaw{}, pitch{};
};
struct Recording {
    std::vector<CalibrationPose> poses;
    std::vector<CalibrationMotion> motions;
};
Angles varied(double t) {
    return {.10 * std::sin(1.1 * t + .12 * t * t) + .045 * std::sin(4.9 * t + .023 * t * t * t) +
                .018 * std::sin(11.4 * t),
            .08 * std::sin(1.7 * t + .09 * t * t + .4) + .04 * std::sin(6.1 * t + .019 * t * t * t) +
                .015 * std::sin(10.3 * t)};
}
// Independent fixture: project the previous camera's central world ray through
// the current yaw/pitch rotation matrix and a known pinhole camera. No solver or
// core quaternion helpers are used to calculate the observed image shifts.
Recording recording(double delay_ms = 83, double hfov = 103, double vfov = 79,
                    const std::function<Angles(double)> &trajectory = varied, double duration = 12,
                    unsigned pose_rate = 250, unsigned video_rate = 60) {
    Recording out;
    constexpr std::uint64_t epoch = 5000000000ULL;
    const auto timestamp = [](double seconds) {
        return epoch + static_cast<std::uint64_t>(std::llround(seconds * 1e9));
    };
    for (unsigned i = 0; i <= duration * pose_rate; ++i) {
        const auto a = trajectory(static_cast<double>(i) / pose_rate);
        const double cy = std::cos(a.yaw / 2), sy = std::sin(a.yaw / 2);
        const double cp = std::cos(a.pitch / 2), sp = std::sin(a.pitch / 2);
        out.poses.push_back(
            {timestamp(static_cast<double>(i) / pose_rate), {cy * sp, sy * cp, -sy * sp, cy * cp}});
    }
    for (unsigned i = static_cast<unsigned>(std::ceil(1.2 * video_rate)) + 1;
         i <= (duration - .5) * video_rate; ++i) {
        const double previous = static_cast<double>(i - 1) / video_rate,
                     current = static_cast<double>(i) / video_rate;
        const auto a = trajectory(previous - delay_ms / 1000.);
        const auto b = trajectory(current - delay_ms / 1000.);
        const double world_x = -std::sin(a.yaw) * std::cos(a.pitch);
        const double world_y = std::sin(a.pitch);
        const double world_z = -std::cos(a.yaw) * std::cos(a.pitch);
        const double camera_x = std::cos(b.yaw) * world_x - std::sin(b.yaw) * world_z;
        const double yaw_z = std::sin(b.yaw) * world_x + std::cos(b.yaw) * world_z;
        const double camera_y = std::cos(b.pitch) * world_y + std::sin(b.pitch) * yaw_z;
        const double camera_z = -std::sin(b.pitch) * world_y + std::cos(b.pitch) * yaw_z;
        out.motions.push_back({timestamp(previous), timestamp(current),
                               camera_x / -camera_z / (2 * std::tan(hfov * pi / 360)),
                               -camera_y / -camera_z / (2 * std::tan(vfov * pi / 360)), .95});
    }
    return out;
}
CalibrationResult solve(const Recording &data) {
    return calibrate(data.poses, data.motions);
}
void rejected(const Recording &data) {
    const auto r = solve(data);
    check(!r.valid, "invalid recording was accepted");
    check(!r.reason.empty(), "rejection must explain how to retry");
    check(std::isfinite(r.delay_ms) && std::isfinite(r.horizontal_fov) && std::isfinite(r.vertical_fov) &&
              std::isfinite(r.confidence),
          "rejection exposed nonfinite values");
}
void recovered(const Recording &data, double delay, double hfov, double vfov) {
    const auto r = solve(data);
    check(r.valid, "expected recovery: " + r.reason);
    near(r.delay_ms, delay, 3);
    near(r.horizontal_fov, hfov, 5);
    near(r.vertical_fov, vfov, 5);
    check(r.confidence > .5 && r.confidence <= 1, "confidence must express strong supporting data");
    check(r.samples >= 400 && r.samples <= data.motions.size(), "incorrect supporting sample count");
    check(!r.reason.empty(), "successful result must describe its approximation");
}
} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"recovers an irregular trajectory delayed by 83 ms", [] { recovered(recording(), 83, 103, 79); }},
        {"search includes both delay endpoints",
         [] {
             recovered(recording(0, 92, 86), 0, 92, 86);
             recovered(recording(1000, 118, 95), 1000, 118, 95);
         }},
        {"recovers sub-grid delay in mild observation noise",
         [] {
             auto d = recording(327.5, 110, 82);
             for (std::size_t i = 0; i < d.motions.size(); ++i) {
                 d.motions[i].x += .00008 * std::sin(29.73 * i);
                 d.motions[i].y += .00008 * std::sin(13.87 * i);
             }
             recovered(d, 327.5, 110, 82);
         }},
        {"recovers left/right followed by up/down with some weak observations",
         [] {
             auto d = recording(83, 103, 79, [](double t) {
                 const auto a = varied(t);
                 return Angles{a.yaw * std::clamp(6 - t, 0., 1.), a.pitch * std::clamp(t - 5, 0., 1.)};
             });
             for (std::size_t i = 0; i < d.motions.size(); i += 3)
                 d.motions[i].confidence = .1;
             recovered(d, 83, 103, 79);
         }},
        {"recovers a full collection near both input limits",
         [] { recovered(recording(83, 103, 79, varied, 30, 333, 133), 83, 103, 79); }},
        {"uses relative rotations across arbitrary tracking origin and quaternion signs",
         [] {
             auto d = recording();
             const auto origin = multiply(axis_angle({0, 1, 0}, 3.12), axis_angle({0, 0, 1}, .7));
             for (std::size_t i = 0; i < d.poses.size(); ++i) {
                 auto q = multiply(origin, d.poses[i].orientation);
                 const double scale = i % 2 ? -1.1 : 1.1;
                 d.poses[i].orientation = {scale * q.x, scale * q.y, scale * q.z, scale * q.w};
             }
             recovered(d, 83, 103, 79);
         }},
        {"timestamp arithmetic preserves precision near uint64 limits",
         [] {
             auto d = recording();
             constexpr auto offset = std::numeric_limits<std::uint64_t>::max() - 20000000000ULL;
             for (auto &p : d.poses)
                 p.time_ns += offset;
             for (auto &m : d.motions) {
                 m.previous_time_ns += offset;
                 m.time_ns += offset;
             }
             recovered(d, 83, 103, 79);
         }},
        {"rejects no observations and short recordings",
         [] {
             rejected({});
             auto d = recording();
             d.motions.resize(20);
             rejected(d);
         }},
        {"rejects static footage and headset",
         [] { rejected(recording(83, 103, 79, [](double) { return Angles{}; })); }},
        {"requires movement on both axes",
         [] {
             rejected(recording(83, 103, 79, [](double t) { return Angles{varied(t).yaw, 0}; }));
             rejected(recording(83, 103, 79, [](double t) { return Angles{0, varied(t).pitch}; }));
         }},
        {"rejects weak texture confidence",
         [] {
             auto d = recording();
             for (auto &m : d.motions)
                 m.confidence = .1;
             rejected(d);
         }},
        {"rejects independent image noise",
         [] {
             auto d = recording();
             for (std::size_t i = 0; i < d.motions.size(); ++i) {
                 d.motions[i].x = .005 * std::sin(19.13 * i);
                 d.motions[i].y = .005 * std::sin(37.39 * i);
             }
             rejected(d);
         }},
        {"rejects reversed image direction on either axis",
         [] {
             for (const bool horizontal : {false, true}) {
                 auto d = recording();
                 for (auto &m : d.motions)
                     (horizontal ? m.x : m.y) *= -1;
                 rejected(d);
             }
         }},
        {"rejects unavailable pose overlap",
         [] {
             auto d = recording();
             for (auto &p : d.poses)
                 p.time_ns += 100000000000ULL;
             rejected(d);
         }},
        {"rejects a tracking gap even when its endpoints are valid",
         [] {
             auto d = recording();
             d.poses.erase(d.poses.begin() + 1100, d.poses.begin() + 1200);
             rejected(d);
         }},
        {"rejects a video gap and pose recenter",
         [] {
             auto d = recording();
             d.motions[300].previous_time_ns -= 500000000ULL;
             rejected(d);
             d = recording();
             d.poses[1500].orientation = axis_angle({0, 1, 0}, 2);
             rejected(d);
         }},
        {"rejects overlapping motion intervals that overcount evidence",
         [] {
             auto d = recording();
             d.motions[4].previous_time_ns = d.motions[3].previous_time_ns;
             rejected(d);
         }},
        {"rejects ambiguous periodic delay peaks",
         [] {
             rejected(recording(83, 103, 79, [](double t) {
                 return Angles{.1 * std::sin(2 * pi * t / .4), .08 * std::cos(2 * pi * t / .4)};
             }));
         }},
        {"rejects fast periodic aliases between coarse delay samples",
         [] {
             for (const double period : {.04, .05, .06, .08, .1, .2, .4, .8}) {
                 const auto d = recording(83, 103, 79, [period](double t) {
                     return Angles{.003 * std::sin(2 * pi * t / period),
                                   .003 * std::cos(2 * pi * t / period)};
                 });
                 const auto r = solve(d);
                 check(!r.valid, "accepted periodic delay aliases with period " + std::to_string(period) +
                                     ", delay " + std::to_string(r.delay_ms));
                 check(!r.reason.empty(), "ambiguous periodic delay must include a rejection reason");
             }
         }},
        {"rejects periodic aliases midway between coarse delay samples",
         [] {
             for (const double period : {.15, .2, .25, .3, .4}) {
                 rejected(recording(85, 103, 79, [period](double t) {
                     return Angles{.03 * std::sin(2 * pi * t / period), .03 * std::cos(2 * pi * t / period)};
                 }));
             }
         }},
        {"rejects constant turn with unidentifiable delay",
         [] { rejected(recording(83, 103, 79, [](double t) { return Angles{.06 * t, .04 * t}; })); }},
        {"rejects nonfinite invalid and unordered observations",
         [] {
             auto d = recording();
             d.poses[4].orientation.x = std::numeric_limits<double>::quiet_NaN();
             rejected(d);
             d = recording();
             d.poses[4].orientation = {0, 0, 0, 0};
             rejected(d);
             d = recording();
             d.poses[4].time_ns = d.poses[3].time_ns;
             rejected(d);
             d = recording();
             d.motions[4].x = std::numeric_limits<double>::infinity();
             rejected(d);
             d = recording();
             d.motions[4].confidence = std::numeric_limits<double>::quiet_NaN();
             rejected(d);
             d = recording();
             d.motions[4].confidence = 2;
             rejected(d);
             d = recording();
             d.motions[4].time_ns = d.motions[4].previous_time_ns;
             rejected(d);
             d = recording();
             std::swap(d.motions[3], d.motions[4]);
             rejected(d);
         }},
        {"rejects unphysical effective FOV", [] { rejected(recording(83, 5, 79)); }},
        {"bounds input storage and recording span",
         [] {
             auto d = recording();
             d.poses.resize(10001);
             rejected(d);
             d = recording();
             d.motions.resize(4001);
             rejected(d);
             d = recording();
             d.poses.back().time_ns += 40000000000ULL;
             rejected(d);
         }},
    };
    unsigned failures = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto &test : cases) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception &e) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << e.what() << '\n';
        }
    }
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << cases.size() << " calibration cases, " << failures << " failures, " << elapsed << " ms\n";
    return failures ? 1 : 0;
}
