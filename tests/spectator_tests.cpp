#include "vrst/spectator.hpp"
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
using namespace vrst;
namespace {
void check(bool b, const char *why) { if (!b) throw std::runtime_error(why); }
void near(Quat a, Quat b, double degrees = .001) {
    check(angle_between(a, b) * 180 / pi < degrees, "orientation differs");
}
Quat yaw(double d) { return axis_angle({0, 1, 0}, d * pi / 180); }
double yaw_degrees(Quat q) { const auto v = rotate(q, {0, 0, -1}); return std::atan2(-v.x, -v.z) * 180 / pi; }
struct Run {
    SpectatorController c;
    SpectatorSettings s;
    SpectatorInput i;
    Run() { i.session = 1; i.reference_epoch = 1; i.time_ns = 1000000000; }
    Quat step(double gaze_yaw = 20, double head_yaw = 0, double dt = .01) {
        i.time_ns += static_cast<std::uint64_t>(dt * 1e9); ++i.sequence;
        i.source_orientation = i.base_target_orientation = yaw(head_yaw);
        i.gaze = {yaw(head_yaw), rotate(yaw(gaze_yaw), {0, 0, -1}),
                  static_cast<std::int64_t>(i.time_ns), i.reference_epoch,
                  GazeValid | GazeTracked | GazeTimeKnown};
        return c.update(i, s);
    }
    Quat frames(int n, double gaze_yaw = 20, double head_yaw = 0, double dt = .01) {
        Quat q; for (int k = 0; k < n; ++k) q = step(gaze_yaw, head_yaw, dt); return q;
    }
    void learn(double center = 20) {
        c.begin_chat_calibration(999999999999999ULL); // UI and XR clocks intentionally differ.
        frames(405, center);
    }
};
double natural_fixture(int hz, bool jitter, bool compare_legacy) {
    Run r; r.s.natural_enabled = true;
    PoseHistory legacy; legacy.set_smoothing(65);
    double sum = 0; int count = 0;
    for (int k = 0; k <= 3 * hz; ++k) {
        const double t = double(k) / hz;
        const double head = jitter ? (k % 2 ? 1 : -1) : std::min(45., 45 * t);
        const auto q = r.step(0, head, 1. / hz);
        Pose p; p.session = 1; p.sequence = k; p.time_ns = r.i.time_ns; p.orientation = yaw(head);
        legacy.push(p);
        const auto baseline = legacy.sample(p.time_ns).target_orientation;
        if (t > .5 && (jitter || t < 1)) {
            sum += jitter ? std::abs(yaw_degrees(compare_legacy ? baseline : q))
                          : angle_between(compare_legacy ? baseline : q, yaw(head)) * 180 / pi;
            ++count;
        }
        if (!jitter) check(yaw_degrees(q) <= 45.001, "natural overshoot");
    }
    if (!jitter) near(r.c.update(r.i, r.s), yaw(45), .05);
    return sum / count;
}
void benchmark() {
    constexpr int n = 300000;
    for (bool enabled : {false, true}) {
        Run r; r.s.gaze_enabled = r.s.natural_enabled = enabled;
        double checksum = 0;
        auto start = std::chrono::steady_clock::now();
        for (int k = 0; k < n; ++k) checksum += r.step(20 + (k % 5) * .01, std::sin(k * .001) * 15).w;
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        std::cout << "benchmark " << (enabled ? "enabled" : "disabled") << ": " << ns / n
                  << " ns/frame; deterministic checksum=" << checksum << '\n';
    }
}
}
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--benchmark") { benchmark(); return 0; }
    if (argc > 1 && std::string(argv[1]) == "--diagnostic") {
        std::cout << "mean alternating jitter degrees: natural=" << natural_fixture(100, true, false)
                  << " legacy=" << natural_fixture(100, true, true) << '\n'
                  << "mean sustained turn lag degrees: natural=" << natural_fixture(100, false, false)
                  << " legacy=" << natural_fixture(100, false, true) << '\n'
                  << "mean turn lag at 50 Hz=" << natural_fixture(50, false, false)
                  << " at 100 Hz=" << natural_fixture(100, false, false) << '\n';
        return 0;
    }
    static_assert(std::is_standard_layout_v<GazeSample> && std::is_trivially_copyable_v<GazeSample>);
    std::vector<std::pair<const char *, std::function<void()>>> cases{
        {"disabled and master bypass preserve base target", [] {
            Run r; check(!r.s.gaze_enabled && !r.s.chat_enabled && !r.s.natural_enabled, "enabled default");
            r.i.source_orientation = yaw(30); r.i.base_target_orientation = yaw(10);
            near(r.c.update(r.i, r.s), yaw(10));
            r.s.gaze_enabled = r.s.natural_enabled = true; r.s.bypass = true;
            r.i.time_ns += 10000000; ++r.i.sequence; near(r.c.update(r.i, r.s), yaw(10));
        }},
        {"disabled output preserves base quaternion components exactly", [] {
            Run r; r.i.base_target_orientation = {.1, .2, .3, .9};
            const auto q = r.c.update(r.i, r.s);
            check(q.x == .1 && q.y == .2 && q.z == .3 && q.w == .9, "disabled path changed baseline components");
        }},
        {"sustained fixation biases framing within angular travel", [] {
            Run r; r.s.gaze_enabled = true; r.s.gaze_strength = 100;
            near(r.frames(15), {}); const auto q = r.frames(150);
            check(yaw_degrees(q) > 7 && yaw_degrees(q) <= 8.001, "fixation/travel failed");
        }},
        {"saccades never qualify and neutral region stays neutral", [] {
            Run r; r.s.gaze_enabled = true;
            for (int k = 0; k < 200; ++k) near(r.step(k % 10 < 5 ? 20 : -20), {});
            near(r.frames(100, 2), {});
        }},
        {"stale invalid unknown and future gaze are explicit and recover smoothly", [] {
            Run r; r.s.gaze_enabled = true; const auto active = r.frames(150);
            check(angle_between(active, {}) > .01, "no initial bias");
            r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.sample_time_ns -= 200000000;
            const auto first = r.c.update(r.i, r.s);
            check(r.c.status().gaze_state == GazeState::stale, "stale not reported");
            check(angle_between(first, active) < .02, "loss snapped");
            for (int k = 0; k < 200; ++k) { r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.flags = 0; r.c.update(r.i, r.s); }
            near(r.c.update(r.i, r.s), {}, .01);
            r.step(); r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.sample_time_ns = 0;
            r.i.gaze.flags &= ~GazeTimeKnown; r.c.update(r.i, r.s);
            check(!r.c.status().gaze_time_known && r.c.status().gaze_state == GazeState::fixating, "unknown time hidden");
            r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.flags |= GazeTimeKnown;
            r.i.gaze.sample_time_ns = static_cast<std::int64_t>(r.i.time_ns + 10000000); r.c.update(r.i, r.s);
            check(r.c.status().gaze_state == GazeState::stale, "future gaze accepted");
        }},
        {"eye and submitted projection reference frames compose without double head turn", [] {
            Run r; r.s.gaze_enabled = true; r.s.gaze_strength = 100;
            for (int k = 0; k < 180; ++k) {
                r.i.time_ns += 10000000; ++r.i.sequence;
                r.i.source_orientation = r.i.base_target_orientation = yaw(60);
                r.i.gaze = {yaw(20), rotate(yaw(20), {0, 0, -1}), static_cast<std::int64_t>(r.i.time_ns), 1, 7};
                near(r.c.update(r.i, r.s), yaw(60));
            }
            // Eye looks 12 degrees left of source in stable space; base lags by 5.
            for (int k = 0; k < 180; ++k) {
                r.i.time_ns += 10000000; ++r.i.sequence; r.i.base_target_orientation = yaw(55);
                r.i.gaze.direction = rotate(yaw(32), {0, 0, -1}); r.i.gaze.sample_time_ns = r.i.time_ns;
                r.c.update(r.i, r.s);
            }
            check(yaw_degrees(r.c.update(r.i, r.s)) > 62, "gaze composed in wrong reference");
        }},
        {"vertical travel is independently bounded", [] {
            Run r; r.s.gaze_enabled = true; r.s.gaze_strength = 100;
            r.step(0); r.i.gaze.direction = rotate(axis_angle({1, 0, 0}, .5), {0, 0, -1});
            SpectatorController c; Quat q;
            for (int k = 0; k < 200; ++k) { r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.sample_time_ns = r.i.time_ns; q = c.update(r.i, r.s); }
            const auto ray = rotate(q, {0, 0, -1});
            const double p = std::asin(ray.y) * 180 / pi; check(p > 2.9 && p <= 3.001, "vertical bound");
        }},
        {"repeated render sequence or timestamps never advance state", [] {
            Run r; r.s.gaze_enabled = true; auto q = r.frames(40);
            for (int k = 0; k < 100; ++k) near(r.c.update(r.i, r.s), q);
            r.i.time_ns += 10000000; near(r.c.update(r.i, r.s), q);
            r.i.time_ns -= 10000000; ++r.i.sequence; near(r.c.update(r.i, r.s), q);
        }},
        {"chat calibration countdown works with gaze and exclusion disabled", [] {
            Run r; r.c.begin_chat_calibration(123);
            r.frames(200); check(r.c.status().calibration_phase == ChatCalibrationPhase::countdown, "countdown skipped");
            r.frames(150); check(r.c.status().calibration_phase == ChatCalibrationPhase::acquiring, "not acquiring");
            r.frames(55); check(r.c.status().zone_valid, "disabled gaze prevented explicit calibration");
            check(r.c.status().calibration_samples >= 20, "insufficient calibration accepted");
        }},
        {"chat fixed reference survives head rotation and removes only gaze bias", [] {
            Run r; r.learn(); check(r.c.status().zone_valid, "learning failed");
            r.s.gaze_enabled = r.s.chat_enabled = true; r.frames(150, -20);
            r.frames(180, 20, 10);
            check(r.c.status().in_chat_zone && r.c.status().gaze_state == GazeState::excluded, "zone moved with head");
            near(r.c.update(r.i, r.s), yaw(10), .02);
            r.frames(150, -20, 10); check(!r.c.status().in_chat_zone, "zone never exited");
            check(angle_between(r.c.update(r.i, r.s), yaw(10)) > .01, "outside gaze never reacquired");
        }},
        {"calibration tolerates isolated outliers but rejects scattered reacquisition", [] {
            Run r; r.c.begin_chat_calibration(0); r.frames(302);
            for (int k = 0; k < 105; ++k) r.step(k % 20 == 0 ? -60 : 20);
            check(r.c.status().zone_valid, "isolated outlier spoiled calibration");
            r.c.begin_chat_calibration(0); r.frames(302);
            for (int k = 0; k < 105; ++k) r.step(k % 2 ? 50 : -50);
            check(r.c.status().calibration_phase == ChatCalibrationPhase::failed && r.c.status().zone_valid, "failed relearn lost prior zone");
            r.s.chat_enabled = true; r.step(20); check(r.c.status().in_chat_zone, "old center replaced");
        }},
        {"calibration robustly rejects the first sample when it is an outlier", [] {
            Run r; r.c.begin_chat_calibration(0); r.frames(300);
            r.step(-70); r.frames(105, 20);
            check(r.c.status().zone_valid, "first outlier poisoned robust center");
            r.s.chat_enabled = true; r.step(20); check(r.c.status().in_chat_zone, "robust center wrong");
        }},
        {"chat boundary hysteresis and reentry dwell avoid framing flicker", [] {
            Run r; r.learn(); r.s.chat_enabled = r.s.gaze_enabled = true;
            r.frames(5, 20); check(r.c.status().in_chat_zone, "center outside");
            r.frames(5, 28.8); check(r.c.status().in_chat_zone, "hysteresis absent");
            r.step(31); check(!r.c.status().in_chat_zone && r.c.status().gaze_state == GazeState::fixating,
                              "exit skipped reentry dwell");
            r.s.gaze_enabled = false; r.frames(25, 20);
            check(r.c.status().in_chat_zone && r.c.status().gaze_state == GazeState::disabled,
                  "disabled framing retains excluded state");
        }},
        {"gaze fixation timing and response are stable at different cadences", [] {
            Run slow, fast; slow.s.gaze_enabled = fast.s.gaze_enabled = true;
            const auto a = slow.frames(30, 20, 0, .02), b = fast.frames(60, 20, 0, .01);
            near(a, b, .15);
        }},
        {"calibration rejects invalid duplicate and stale samples", [] {
            Run r; r.c.begin_chat_calibration(0); r.frames(302);
            for (int k = 0; k < 105; ++k) {
                r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.flags = 0; r.c.update(r.i, r.s);
            }
            check(!r.c.status().zone_valid && r.c.status().calibration_phase == ChatCalibrationPhase::failed, "invalid calibration accepted");
            r.c.begin_chat_calibration(0); r.frames(302);
            const auto stamp = r.i.gaze.sample_time_ns;
            for (int k = 0; k < 105; ++k) { r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.sample_time_ns = stamp; r.c.update(r.i, r.s); }
            check(!r.c.status().zone_valid, "repeated samples calibrated");
        }},
        {"cancel clear reset and session or epoch invalidate transient anchor", [] {
            Run r; r.learn(); check(r.c.status().zone_valid, "no zone");
            r.c.begin_chat_calibration(0); r.c.cancel_chat_calibration();
            check(r.c.status().calibration_phase == ChatCalibrationPhase::idle && r.c.status().zone_valid, "cancel lost anchor");
            r.i.reference_epoch = 2; r.step(); check(!r.c.status().zone_valid, "epoch retained anchor");
            r.learn(); r.i.session = 2; r.step(); check(!r.c.status().zone_valid, "session retained anchor");
            r.learn(); r.c.clear_chat_zone(); check(!r.c.status().zone_valid, "clear failed");
            r.learn(); r.c.reset(); check(!r.c.status().zone_valid, "reset retained anchor");
        }},
        {"invalid gaze values never poison camera and sanitization bounds tuning", [] {
            Run r; r.s.gaze_enabled = true; r.frames(100);
            r.i.time_ns += 10000000; ++r.i.sequence; r.i.gaze.direction.x = NAN;
            check(valid(r.c.update(r.i, r.s)), "NaN poisoned camera");
            r.s.gaze_strength = INFINITY; r.s.gaze_horizontal_degrees = 1000; r.s.gaze_dwell_ms = -1;
            r.s.chat_height_degrees = NAN; r.s.natural_max_correction_degrees = -100;
            const auto s = sanitize_spectator_settings(r.s);
            check(std::isfinite(s.gaze_strength) && s.gaze_horizontal_degrees <= 30 && s.gaze_dwell_ms >= 0 &&
                  std::isfinite(s.chat_height_degrees) && s.natural_max_correction_degrees >= 0, "sanitize failed");
        }},
        {"natural attenuates alternating jitter compared with existing smoother", [] {
            const double n = natural_fixture(100, true, false), old = natural_fixture(100, true, true);
            check(n < .15 && n < old * 1.3, "natural admitted alternating jitter");
        }},
        {"natural follows sustained turn faster than existing smoother and settles", [] {
            check(natural_fixture(100, false, false) < natural_fixture(100, false, true) * .8, "natural turn too delayed");
        }},
        {"natural bounds initial acceleration for an abrupt small head movement", [] {
            Run r; r.s.natural_enabled = true; r.step(0, 0);
            const auto q = r.step(0, 10);
            check(angle_between(q, {}) * 180 / pi <= .145, "natural acceleration unbounded");
            near(r.frames(300, 0, 10), yaw(10), .01);
        }},
        {"zero natural suppression and zero per-axis amount preserve source", [] {
            Run r; r.s.natural_enabled = true; r.s.natural_suppression = 0;
            r.step(0, 0); near(r.step(0, 10), yaw(10));
            Run axis; axis.s.natural_enabled = true; axis.s.natural_yaw = 0;
            axis.step(0, 0); near(axis.step(0, 10), yaw(10));
        }},
        {"natural elapsed-time response is stable across frame cadence", [] {
            const auto slow = natural_fixture(50, false, false), fast = natural_fixture(100, false, false);
            check(std::abs(slow - fast) < 1, "natural depends on timestep");
        }},
        {"natural resets at gaps and reference changes and avoids yaw wrap discontinuity", [] {
            Run r; r.s.natural_enabled = true;
            near(r.step(0, 179), yaw(179));
            const auto wrap = r.step(0, -179); check(angle_between(wrap, yaw(179)) < .04, "yaw wrap snapped");
            near(r.step(0, 80, .5), yaw(80));
            r.i.reference_epoch = 2; near(r.step(0, 30), yaw(30));
            for (int k = 0; k < 100; ++k) {
                const auto q = r.step(0, 30 + k * .5);
                check(angle_between(q, r.i.source_orientation) * 180 / pi <= 15.001, "correction exceeded bound");
            }
        }}
    };
    int failures = 0;
    for (auto &t : cases) try { t.second(); std::cout << "PASS " << t.first << '\n'; }
    catch (const std::exception &e) { ++failures; std::cerr << "FAIL " << t.first << ": " << e.what() << '\n'; }
    std::cout << cases.size() - failures << '/' << cases.size() << " passed\n";
    return failures ? 1 : 0;
}
