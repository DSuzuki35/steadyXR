#include "calibration_session.hpp"
#include <cmath>
#include <cstdio>
int main() {
    int failures = 0;
    auto check = [&](bool p, const char *n) {
        std::printf("%s %s\n", p ? "PASS" : "FAIL", n);
        failures += !p;
    };
    vrst_plugin::CalibrationSession session;
    const std::uint64_t epoch = 5000000000ULL;
    session.start(epoch);
    vrst::TrackingSnapshot pose;
    pose.pose.valid = true;
    pose.pose.flags = vrst::TrackingValid;
    pose.local_pose_time_ns = epoch + 10000000ULL;
    session.pose(pose, epoch + 20000000ULL);
    session.pose(pose, epoch + 20000000ULL);
    check(session.state().poses == 1, "duplicate sampled pose timestamps are not collected twice");
    pose.pose.flags |= vrst::DemoPose;
    pose.local_pose_time_ns += 10000000ULL;
    session.pose(pose, epoch + 30000000ULL);
    check(session.state().poses == 1, "synthetic sender demo cannot produce a live calibration");
    vrst::ImageResult r;
    r.time_ns = 100;
    r.width = 480;
    r.height = 270;
    session.image(r, epoch + 10000000ULL);
    r.status = vrst::ImageStatus::tracking;
    r.previous_time_ns = 100;
    r.time_ns = 200;
    r.motion = {2, 1, 0};
    r.confidence = .9;
    session.image(r, epoch + 30000000ULL);
    session.image(r, epoch + 30000000ULL);
    check(session.state().motions == 1, "displayed video motion uses capture time and is deduplicated");
    r.previous_time_ns = 999;
    r.time_ns = 300;
    session.image(r, epoch + 40000000ULL);
    check(session.state().motions == 1, "motion from a reset or missing prior image is rejected");
    session.finish();
    check(!session.state().active && !session.state().result.valid,
          "insufficient collection finishes without an applicable suggestion");
    session.start(epoch);
    session.pose(pose, epoch + 30000000000ULL);
    check(!session.active(), "collection automatically stops at 30 seconds");
    session.start(epoch);
    check(session.invalidate("source changed") && !session.active() && !session.state().result.valid,
          "source changes cancel collection and invalidate suggestions");

    // A complete observation path with different OBS and local clock epochs.
    // Independent small-angle fixture; varied frequencies avoid delay aliases.
    auto angles = [](double t) {
        return std::pair<double, double>{.12 * std::sin(1.1 * t + .12 * t * t) + .035 * std::sin(5.3 * t),
                                         .09 * std::sin(1.7 * t + .09 * t * t + .4) +
                                             .04 * std::sin(6.1 * t)};
    };
    auto orientation = [&](double t) {
        const auto [y, p] = angles(t);
        return vrst::multiply(vrst::axis_angle({0, 1, 0}, y), vrst::axis_angle({1, 0, 0}, p));
    };
    const double dt = 1. / 60;
    auto collect = [&](bool scene_cut = false) {
        session.start(epoch);
        session.source(1, 640, 480);
        for (unsigned i = 0; i < 900; ++i) {
            const auto time = epoch + std::uint64_t(std::llround(i * dt * 1e9));
            pose.pose.flags = vrst::TrackingValid;
            pose.local_pose_time_ns = time;
            pose.pose.source_orientation = orientation(i * dt);
            session.pose(pose, time);
            r.time_ns = 100000000000ULL + std::uint64_t(i) * 16666667;
            r.previous_time_ns = i ? r.time_ns - 16666667 : 0;
            r.status = i ? vrst::ImageStatus::tracking : vrst::ImageStatus::warming;
            auto a = angles((i ? i - 1 : 0) * dt - .083), b = angles(i * dt - .083);
            r.motion.x = (b.first - a.first) / (2 * std::tan(103 * vrst::pi / 360)) * r.width;
            r.motion.y = (b.second - a.second) / (2 * std::tan(79 * vrst::pi / 360)) * r.height;
            session.image(r, time);
        }
        if (scene_cut) {
            r.previous_time_ns = r.time_ns;
            r.time_ns += 16666667;
            r.status = vrst::ImageStatus::scene_change;
            session.image(r, epoch + 15000000000ULL);
        }
        session.finish();
        return session.state().result;
    };
    const auto result = collect();
    std::printf("Session suggestion %.1fms %.1fx%.1f: %s\n", result.delay_ms, result.horizontal_fov,
                result.vertical_fov, result.reason.c_str());
    check(result.valid && std::abs(result.delay_ms - 83) <= 3 && std::abs(result.horizontal_fov - 103) < 5 &&
              std::abs(result.vertical_fov - 79) < 5,
          "capture-time collection recovers known delay and FOV across distinct OBS clock epoch");
    session.invalidate("settings changed");
    check(!session.state().result.valid, "previous valid suggestion is invalidated after settings change");
    session.finish();
    check(!session.state().result.valid && session.state().poses == 0 && session.state().motions == 0,
          "Finish cannot resurrect observations invalidated by settings or source changes");
    check(collect().valid, "fresh calibration after invalidation recovers again");
    session.source(1, 640, 480);
    check(session.state().result.valid, "same source preserves completed suggestion");
    session.source(1, 800, 600);
    check(!session.state().result.valid, "source resize invalidates completed suggestion before Apply");
    session.finish();
    check(!session.state().result.valid, "Finish cannot restore a suggestion from old source dimensions");
    check(!collect(true).valid, "detected scene cut discards calibration instead of mixing camera views");
    return failures ? 1 : 0;
}
