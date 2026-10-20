#include "director.hpp"
#include "renderer.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace vrst;
using namespace vrst::preview;
namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool close_to(double a, double b, double epsilon = 1e-6) {
    return std::abs(a - b) < epsilon;
}
Frame sample(std::uint64_t sequence, double yaw = 0) {
    Frame f{};
    f.session = f.generation = f.reference_epoch = f.producer_pid = 1;
    f.sequence = sequence;
    f.display_time = 1000000000 + sequence * 16666667;
    f.view_count = 1;
    auto q = axis_angle({0, 1, 0}, yaw);
    auto &v = f.views[0];
    v.width = v.height = v.rect_width = v.rect_height = 512;
    v.orientation[0] = float(q.x);
    v.orientation[1] = float(q.y);
    v.orientation[2] = float(q.z);
    v.orientation[3] = float(q.w);
    return f;
}
Warp square(double roll = 0) {
    Warp w;
    w.correction = axis_angle({0, 0, 1}, roll);
    return w;
}
void defaults_are_inert_and_settings_are_bounded() {
    DirectorController director;
    auto w = square(.2);
    director.update(w, {}, 1000000000, 1, 0);
    check(close_to(w.output.left, -1) && close_to(w.output.right, 1) && close_to(w.output.up, 1) &&
              close_to(w.output.down, -1),
          "disabled options preserve exact projection");
    DirectorSettings bad;
    bad.lookahead_frames = 2.6;
    bad.auto_crop_max_percent = 100;
    bad.auto_crop_release_ms = -10;
    bad.speed_fov_percent = std::numeric_limits<double>::quiet_NaN();
    bad.speed_fov_full_kph = 900;
    bad.speed_fov_response_ms = std::numeric_limits<double>::infinity();
    bad.telemetry_game = 9;
    const auto safe = sanitize_director_settings(bad);
    check(safe.lookahead_frames == 3 && safe.auto_crop_max_percent == 30 &&
              safe.auto_crop_release_ms == 100 && safe.speed_fov_percent == 6 &&
              safe.speed_fov_full_kph == 400 && safe.speed_fov_response_ms == 1000 &&
              safe.telemetry_game == 0,
          "invalid settings become bounded finite values");
}
void future_poses_are_averaged_without_antipodal_cancellation() {
    Frame frames[]{sample(1, 0), sample(2, .2), sample(3, .4)};
    for (float &value : frames[1].views[0].orientation)
        value = -value;
    const auto mean = centered_orientation(frames, 3);
    check(angle_between(mean, axis_angle({0, 1, 0}, .2)) < 1e-6,
          "centered future turn has no causal lag or quaternion sign cancellation");
    frames[1].views[0].orientation[0] = std::numeric_limits<float>::quiet_NaN();
    check(valid(centered_orientation(frames, 3)), "invalid pose cannot poison mean");
    check(angle_between(centered_orientation(nullptr, 0), {}) < 1e-6, "empty history is neutral");
}
void automatic_crop_fits_rotated_corners_and_releases_slowly() {
    DirectorSettings settings;
    settings.auto_crop_enabled = true;
    settings.auto_crop_max_percent = 30;
    DirectorController controller;
    auto w = square(pi / 12); // 15 degrees: keep <= 1/(cos(15)+sin(15)) = 0.81649658.
    controller.update(w, settings, 1000000000, 1);
    check(close_to(controller.status().auto_crop_percent, 18.350342, 1e-4),
          "auto crop finds minimum symmetric shrink from final corners");
    check(corners_inside(w), "automatic crop uses captured pixels");
    auto released = square();
    controller.update(released, settings, 1016666667, 2);
    check(controller.status().auto_crop_percent > 18 && controller.status().auto_crop_percent < 18.350342,
          "crop attacks immediately but releases slowly");
    const auto before = released.output;
    for (int i = 0; i < 100; ++i) {
        auto repeated = square();
        controller.update(repeated, settings, 1016666667, 2);
        check(close_to(repeated.output.left, before.left), "repeated draw does not advance crop release");
    }
    auto gap = square();
    controller.update(gap, settings, 60000000000, 3);
    check(controller.status().auto_crop_percent > 14,
          "long timestamp gap is bounded and cannot snap crop to baseline");
    settings.auto_crop_enabled = false;
    auto disabled = square();
    controller.update(disabled, settings, 60000000000, 3);
    check(disabled.output.left == -1 && controller.status().auto_crop_percent == 0,
          "disabling auto crop takes effect on same frame");
}
void crop_limit_and_offset_use_final_output_bounds() {
    DirectorSettings settings;
    settings.auto_crop_enabled = true;
    settings.auto_crop_max_percent = 5;
    DirectorController controller;
    auto w = square(pi / 12);
    controller.update(w, settings, 1000000000, 1);
    check(controller.status().auto_crop_percent == 5 && close_to(w.output.left, -.95),
          "impossible crop is clamped without exceeding configured maximum");
    controller.reset();
    settings.auto_crop_max_percent = 30;
    w = square();
    w.output = {-.5, .5, 1.1, -.1}; // Vertical center .5: requires one sixth shrink.
    controller.update(w, settings, 1000000000, 1);
    check(close_to(controller.status().auto_crop_percent, 16.6666667, 1e-4) && corners_inside(w),
          "crop handles asymmetric final framing and vertical offset");
}
void speed_framing_obeys_threshold_invalid_fade_and_frame_clock() {
    DirectorSettings settings;
    settings.speed_fov_enabled = true;
    settings.speed_fov_percent = 10;
    settings.speed_fov_full_kph = 200;
    settings.speed_fov_response_ms = 100;
    DirectorController controller;
    Warp w;
    for (unsigned i = 1; i <= 60; ++i) {
        w = square();
        controller.update(w, settings, 1000000000 + i * 16666667ULL, i, 0);
    }
    check(controller.status().speed_crop_percent > 9.99, "stopped car approaches configured zoom");
    const double stopped = controller.status().speed_crop_percent;
    w = square();
    controller.update(w, settings, 2000000020, 60, 200);
    check(controller.status().speed_crop_percent == stopped, "duplicate sequence never advances speed");
    for (unsigned i = 61; i <= 120; ++i) {
        w = square();
        controller.update(w, settings, 1000000000 + i * 16666667ULL, i, 200);
    }
    check(controller.status().speed_crop_percent < .001, "full speed returns to baseline framing");
    for (unsigned i = 121; i <= 180; ++i) {
        w = square();
        controller.update(w, settings, 1000000000 + i * 16666667ULL, i, 100);
    }
    check(close_to(controller.status().speed_crop_percent, 5, .001), "half speed produces half zoom");
    w = square();
    controller.update(w, settings, 1000000000 + 181 * 16666667ULL, 181,
                      std::numeric_limits<double>::quiet_NaN());
    check(!controller.status().speed_available && controller.status().speed_crop_percent > 0 &&
              controller.status().speed_crop_percent < 5,
          "invalid telemetry fades toward neutral without NaN or snap");
    controller.update(w, settings, 1000000000 + 181 * 16666667ULL, 181, 0, true);
    check(controller.status().speed_crop_percent == 0, "spectator bypass clears transient speed crop");
}
void image_crop_preserves_center_and_bounds_correction() {
    DirectorSettings settings;
    settings.auto_crop_enabled = true;
    settings.auto_crop_max_percent = 30;
    ImageWarp warp;
    // 15-degree rotation about UV center.
    warp.row_x = {.9659258263, -.2588190451, .1464466094};
    warp.row_y = {.2588190451, .9659258263, -.1123724357};
    DirectorController director;
    director.update_image(warp, settings, 1000000000, 1);
    check(close_to(director.status().auto_crop_percent, 18.350342, 1e-4),
          "affine image uses real correction corners for auto crop");
    check(close_to(warp.row_x[2] + .5 * (warp.row_x[0] + warp.row_x[1]), .5) &&
              close_to(warp.row_y[2] + .5 * (warp.row_y[0] + warp.row_y[1]), .5),
          "image crop preserves final framing center");
    settings.auto_crop_max_percent = 5;
    director.reset();
    warp.row_x = {.9659258263, -.2588190451, .1464466094};
    warp.row_y = {.2588190451, .9659258263, -.1123724357};
    ImageWarp neutral;
    director.update_image(warp, settings, 1000000000, 1, -1, false, &neutral);
    for (double x : {0., 1.})
        for (double y : {0., 1.})
            for (const auto &row : {warp.row_x, warp.row_y}) {
                const double uv = row[0] * x + row[1] * y + row[2];
                check(uv >= -1e-8 && uv <= 1 + 1e-8, "exhausted image crop reduces correction to captured UV");
            }
    check(warp.correction_fraction < 1 && director.status().auto_crop_percent == 5,
          "image fallback respects crop cap");
}
void lookahead_target_is_optional_and_obeys_bypass() {
    FrameStabilizer camera;
    Options options;
    options.smoothing = 0;
    options.crop = 30;
    options.aspect = 1;
    options.mirror = false;
    auto frame = sample(1);
    const auto target = axis_angle({0, 1, 0}, .1);
    check(angle_between(camera.update(frame, options, &target).warp.correction, {}) < 1e-6,
          "disabled lookahead preserves ordinary causal correction");
    options.director.lookahead_enabled = true;
    check(angle_between(camera.update(frame, options, &target).warp.correction, target) < 1e-6,
          "lookahead supplies the camera target associated with displayed image");
    options.stabilize = false;
    check(angle_between(camera.update(frame, options, &target).warp.correction, {}) < 1e-6,
          "stabilization bypass suppresses lookahead target");
    options.stabilize = true;
    options.spectator.bypass = true;
    check(angle_between(camera.update(frame, options, &target).warp.correction, {}) < 1e-6,
          "spectator bypass suppresses lookahead target");
}
void composition_crops_before_limiting_and_ab_preserves_history() {
    FrameStabilizer camera;
    Options options;
    options.smoothing = 0;
    options.crop = 0;
    options.aspect = 1;
    options.mirror = false;
    options.director.lookahead_enabled = true;
    options.director.auto_crop_enabled = true;
    options.director.auto_crop_max_percent = 30;
    const auto target = axis_angle({0, 0, 1}, pi / 12);
    auto frame = sample(1);
    options.compare_original = true; // Drawing A first must not feed an identity target into crop state.
    const auto original = camera.update(frame, options, &target);
    const auto original_status = camera.director_status();
    options.compare_original = false;
    const auto enhanced = camera.update(frame, options, &target);
    check(close_to(original_status.auto_crop_percent, 18.350342, 1e-4) &&
              close_to(camera.director_status().auto_crop_percent, original_status.auto_crop_percent),
          "A/B first draw advances the enhanced crop once");
    check(angle_between(enhanced.warp.correction, target) < 1e-6 && corners_inside(enhanced.warp),
          "auto crop retains desired camera correction before fallback limiting");
    check(angle_between(original.warp.correction, {}) < 1e-6 &&
              close_to(original.warp.output.left, enhanced.warp.output.left),
          "A/B preserves auto framing while using original camera");
    const Quat neutral{};
    const auto relaxed = camera.update(sample(2), options, &neutral);
    const auto once = camera.director_status().auto_crop_percent;
    options.compare_original = true;
    for (int i = 0; i < 30; ++i)
        camera.update(sample(2), options, &neutral);
    check(camera.director_status().auto_crop_percent == once &&
              relaxed.warp.output.left > enhanced.warp.output.left - .01,
          "repeated A/B never accelerates crop release");
    camera.reset();
    check(camera.director_status().auto_crop_percent == 0, "reset clears director transients");
}
void natural_motion_keeps_future_target_and_original_gaze_alignment() {
    FrameStabilizer future, causal;
    Options options;
    options.smoothing = 0;
    options.crop = 30;
    options.aspect = 1;
    options.mirror = false;
    options.spectator.natural_enabled = true;
    options.director.lookahead_enabled = true;
    double difference = 0;
    for (unsigned n = 1; n <= 90; ++n) {
        const double source_yaw = .04 * std::sin(n * .1);
        auto frame = sample(n, source_yaw);
        const auto target = axis_angle({0, 1, 0}, source_yaw + .1);
        const auto a = future.update(frame, options, &target);
        const auto b = causal.update(frame, options);
        difference = std::max(difference, angle_between(a.warp.correction, b.warp.correction));
        check(angle_between(a.warp.correction, future.update(frame, options, &target).warp.correction) < 1e-6,
              "repeated natural lookahead draw must preserve target");
    }
    check(difference > .09, "natural motion must retain lookahead target instead of discarding future poses");
    FrameStabilizer gaze;
    options.spectator.gaze_enabled = true;
    options.spectator.gaze_strength = 100;
    options.spectator.gaze_neutral_degrees = 0;
    options.spectator.gaze_dwell_ms = 0;
    options.spectator.gaze_response_ms = 100;
    const auto target = axis_angle({0, 1, 0}, .4);
    Composition output;
    for (unsigned n = 1; n <= 240; ++n) {
        auto frame = sample(n, .3);
        frame.gaze.reference_from_eye = axis_angle({0, 1, 0}, -.2);
        frame.gaze.direction = rotate(frame.gaze.reference_from_eye, {0, 0, -1});
        frame.gaze.reference_epoch = frame.reference_epoch;
        frame.gaze.sample_time_ns = frame.display_time;
        frame.gaze.flags = GazeValid | GazeTracked | GazeTimeKnown;
        output = gaze.update(frame, options, &target);
    }
    check(angle_between(output.warp.correction, {}) < 1e-4,
          "gaze maps through captured source pose while natural motion follows the future pose");
}
} // namespace
int main() {
    try {
        defaults_are_inert_and_settings_are_bounded();
        future_poses_are_averaged_without_antipodal_cancellation();
        automatic_crop_fits_rotated_corners_and_releases_slowly();
        crop_limit_and_offset_use_final_output_bounds();
        speed_framing_obeys_threshold_invalid_fade_and_frame_clock();
        image_crop_preserves_center_and_bounds_correction();
        lookahead_target_is_optional_and_obeys_bypass();
        composition_crops_before_limiting_and_ab_preserves_history();
        natural_motion_keeps_future_target_and_original_gaze_alignment();
        std::cout << "director tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
