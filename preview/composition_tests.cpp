#include "renderer.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace vrst;
using namespace vrst::preview;
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
Frame frame(std::uint64_t sequence, double angle = 0) {
    Frame f{};
    f.generation = f.session = f.reference_epoch = 1;
    f.sequence = sequence;
    f.display_time = 1000000000LL + sequence * 16666667;
    f.producer_pid = 1;
    f.view_count = 2;
    const auto q = axis_angle({0, 1, 0}, angle);
    for (auto &v : f.views) {
        v.texture_handle = 1;
        v.width = v.height = v.rect_width = v.rect_height = 512;
        v.format = 28;
        v.orientation[0] = float(q.x);
        v.orientation[1] = float(q.y);
        v.orientation[2] = float(q.z);
        v.orientation[3] = float(q.w);
    }
    f.views[1].fov[0] = f.views[1].fov[3] = -.25f;
    f.views[1].fov[1] = f.views[1].fov[2] = .25f;
    return f;
}
Frame oriented_frame(std::uint64_t sequence, Quat q) {
    auto f = frame(sequence);
    for (auto &v : f.views) {
        v.orientation[0] = float(q.x);
        v.orientation[1] = float(q.y);
        v.orientation[2] = float(q.z);
        v.orientation[3] = float(q.w);
    }
    return f;
}
int main() {
    try {
        {
            Options tuning;
            tuning.crop = 40;
            tuning.smoothing = 0;
            tuning.separate_smoothing = true;
            auto correction = [&](Vec3 axis) {
                FrameStabilizer camera;
                camera.update(frame(1), tuning);
                const auto output = camera.update(oriented_frame(2, axis_angle(axis, .05)), tuning);
                check(output.valid, "axis test needs valid camera output");
                return angle_between(output.warp.correction, {});
            };
            tuning.horizontal_smoothing = 100;
            tuning.vertical_smoothing = 0;
            check(correction({0, 1, 0}) > .04 && correction({1, 0, 0}) < 1e-6,
                  "horizontal smoothing must leave pitch free");
            std::swap(tuning.horizontal_smoothing, tuning.vertical_smoothing);
            check(correction({1, 0, 0}) > .04 && correction({0, 1, 0}) < 1e-6,
                  "vertical smoothing must leave yaw free");
            tuning.separate_smoothing = false;
            check(correction({1, 0, 0}) < 1e-6, "simple smoothing must ignore saved axis values");
        }
        {
            Options tuning;
            tuning.smoothing = 0;
            tuning.crop = 40;
            tuning.mirror = false;
            tuning.pitch_lock = 100;
            FrameStabilizer pitch;
            const auto anchor = axis_angle({0, 1, 0}, .3);
            check(angle_between(pitch.update(oriented_frame(1, anchor), tuning).warp.correction, {}) < 1e-6,
                  "pitch calibration must preserve the first forward view");
            const auto moved = multiply(axis_angle({0, 1, 0}, .6), axis_angle({1, 0, 0}, .06));
            const auto result = pitch.update(oriented_frame(2, moved), tuning);
            const auto forward = rotate(multiply(moved, result.warp.correction), {0, 0, -1});
            check(std::abs(forward.y) < .01 && angle_between(result.warp.correction, {}) > .04,
                  "pitch strength must suppress small up/down movements near the calibrated view");
            check(std::abs(std::atan2(-forward.x, -forward.z) - .6) < 1e-6,
                  "pitch correction must retain current yaw");
            const auto repeat = pitch.update(oriented_frame(2, moved), tuning);
            check(angle_between(result.warp.correction, repeat.warp.correction) < 1e-6,
                  "redrawing a pitch frame must not move its anchor or change correction");
            pitch.reset_horizon();
            check(angle_between(pitch.update(oriented_frame(2, moved), tuning).warp.correction, {}) < 1e-6,
                  "Set forward view must recalibrate pitch immediately on the retained frame");
            auto reset = oriented_frame(3, axis_angle({1, 0, 0}, -.3));
            reset.reference_epoch++;
            check(angle_between(pitch.update(reset, tuning).warp.correction, {}) < 1e-6,
                  "reference changes must discard a stale pitch anchor");
        }
        {
            Options options;
            options.smoothing = 0;
            options.crop = 40;
            options.mirror = false;
            for (double hemisphere : {0., pi}) {
                const auto neutral = multiply(axis_angle({0, 0, 1}, hemisphere),
                                              axis_angle({1, 0, 0}, .2));
                for (double movement : {-.8, -.08, .08, .8}) {
                    FrameStabilizer pitch;
                    options.pitch_lock = 100;
                    pitch.update(oriented_frame(1, neutral), options);
                    const auto moved = multiply(neutral, axis_angle({1, 0, 0}, movement));
                    const auto result = pitch.update(oriented_frame(2, moved), options);
                    const auto target = multiply(moved, result.warp.correction);
                    const double residual = angle_between(neutral, target);
                    check(result.valid && valid(result.warp.correction) && corners_inside(result.warp),
                          "pitch correction must stay finite and inside available source margins");
                    check(residual < std::abs(movement) &&
                              (std::abs(movement) < .1 ? residual < .02 : residual > .65),
                          "pitch must damp near-anchor motion and release large looks in either hemisphere");
                }
            }
            FrameStabilizer disabled, reference;
            options.pitch_lock = 0;
            disabled.update(oriented_frame(1, {}), options);
            reference.update(oriented_frame(1, {}), options);
            const auto input = oriented_frame(2, axis_angle({1, 0, 0}, .1));
            const auto ordinary = reference.update(input, options);
            for (double invalid : {0., -100., double(NAN), double(INFINITY)}) {
                options.pitch_lock = invalid;
                check(angle_between(disabled.update(input, options).warp.correction,
                                    ordinary.warp.correction) < 1e-6,
                      "disabled or invalid pitch strengths must preserve ordinary smoothing");
            }
            options.pitch_lock = 100;
            options.compare_original = true;
            check(angle_between(disabled.update(input, options).warp.correction, {}) < 1e-6,
                  "original comparison must bypass pitch correction");
            options.compare_original = false;
            options.stabilize = false;
            check(angle_between(disabled.update(input, options).warp.correction, {}) < 1e-6,
                  "stabilization bypass must bypass pitch correction");
        }
        {
            FrameStabilizer camera;
            Options options;
            options.stabilize = false;
            options.crop = 20;
            options.aspect = 1;
            options.horizontal_offset = -100;
            const auto left = camera.update(frame(1), options).warp;
            options.horizontal_offset = 100;
            const auto right = camera.update(frame(1), options).warp;
            check(std::abs(map_uv(left, 0, .5)[0]) < 1e-6 &&
                      std::abs(map_uv(right, 1, .5)[0] - 1) < 1e-6 &&
                      std::abs(map_uv(left, 1, .5)[0] - .8) < 1e-6,
                  "horizontal placement must span the captured reserve without changing zoom");
        }
        {
            Options options;
            options.smoothing = 0;
            options.pitch_lock = 100;
            options.crop = 0;
            options.aspect = 1;
            options.mirror = false;
            FrameStabilizer limited;
            limited.update(frame(1), options);
            const auto result = limited.update(oriented_frame(2, axis_angle({1, 0, 0}, .08)), options);
            check(result.valid && corners_inside(result.warp) && result.warp.correction_fraction < .01,
                  "insufficient pitch margins must limit correction and report the missing fraction");
            options.mirror = true;
            options.mirror_limit = 1;
            const auto mirrored = limited.update(oriented_frame(2, axis_angle({1, 0, 0}, .08)), options);
            check(mirrored.warp.correction_fraction > .01 && mirrored.warp.correction_fraction < 1,
                  "pitch correction must respect the finite mirror allowance");
            for (double u : {0., 1.})
                for (double v : {0., 1.}) {
                    const auto uv = map_uv(mirrored.warp, u, v);
                    check(uv[0] >= -.010001 && uv[0] <= 1.010001 && uv[1] >= -.010001 && uv[1] <= 1.010001,
                          "pitch-corrected corners must stay within the configured mirror allowance");
                }
            FrameStabilizer vertical;
            const auto pole = axis_angle({1, 0, 0}, pi / 2);
            check(angle_between(vertical.update(oriented_frame(1, pole), options).warp.correction, {}) < 1e-6,
                  "vertical startup must defer pitch calibration");
            const auto forward = axis_angle({1, 0, 0}, .2);
            check(angle_between(vertical.update(oriented_frame(2, forward), options).warp.correction, {}) < 1e-6,
                  "deferred pitch calibration must use the first unambiguous forward view");
            options.crop = 40;
            options.mirror_limit = 10;
            for (unsigned i = 3; i <= 183; ++i) {
                const auto q = axis_angle({1, 0, 0}, (i - 3) * pi / 180);
                const auto composed = vertical.update(oriented_frame(i, q), options);
                check(composed.valid && valid(composed.warp.correction) &&
                          angle_between(composed.warp.correction, {}) <= 7.50001 * pi / 180,
                      "pitch must remain finite and bounded while crossing a vertical pole");
            }
        }
        {
            FrameStabilizer ordinary, legacy;
            Options options;
            options.smoothing = 65;
            options.crop = 30;
            for (unsigned i = 1; i <= 120; ++i) {
                auto input = frame(i, std::sin(i * .11) * .18);
                options.spectator.gaze_enabled = i > 60;
                input.gaze.flags = GazeValid | GazeTracked | GazeTimeKnown;
                input.gaze.sample_time_ns = input.display_time;
                input.gaze.reference_epoch = input.reference_epoch;
                input.gaze.direction = rotate(axis_angle({0, 1, 0}, .08), {0, 0, -1});
                options.spectator.natural_enabled = false;
                const auto expected = ordinary.update(input, options);
                options.spectator.natural_enabled = true;
                const auto actual = legacy.update(input, options);
                check(angle_between(actual.warp.correction, expected.warp.correction) < 1e-6,
                      "legacy natural setting must not replace ordinary standalone smoothing");
            }
        }
        {
            FrameStabilizer camera;
            Options gaze;
            gaze.smoothing = 0;
            gaze.crop = 35;
            gaze.mirror = false;
            gaze.stabilize = false;
            gaze.spectator.gaze_enabled = true;
            gaze.spectator.gaze_strength = 100;
            auto input = frame(1);
            for (unsigned i = 1; i <= 180; ++i) {
                input = frame(i);
                input.gaze.flags = GazeValid | GazeTracked | GazeTimeKnown;
                input.gaze.sample_time_ns = input.display_time;
                input.gaze.reference_epoch = input.reference_epoch;
                input.gaze.direction = rotate(axis_angle({0, 1, 0}, .3), {0, 0, -1});
                camera.update(input, gaze);
            }
            auto enhanced = camera.update(input, gaze);
            check(angle_between(enhanced.warp.correction, {}) > .05,
                  "gaze works independently of existing stabilization");
            gaze.compare_original = true;
            check(angle_between(camera.update(input, gaze).warp.correction, {}) < 1e-6,
                  "camera A/B uses original geometry");
            gaze.compare_effects = true;
            check(angle_between(camera.update(input, gaze).warp.correction, enhanced.warp.correction) < 1e-6,
                  "effects A/B preserves enhanced camera geometry without advancing frame");
            gaze.compare_original = false;
            gaze.compare_effects = false;
            gaze.crop = 0;
            check(angle_between(camera.update(input, gaze).warp.correction, {}) < 1e-6,
                  "gaze respects horizontal crop limit");
            gaze.mirror = true;
            gaze.mirror_limit = 0;
            check(angle_between(camera.update(input, gaze).warp.correction, {}) < 1e-6,
                  "new camera honors zero mirror allowance even with mirror enabled");
            gaze.mirror_limit = 2;
            const auto bounded = camera.update(input, gaze).warp;
            for (double x : {0., 1.})
                for (double y : {0., 1.}) {
                    const auto uv = map_uv(bounded, x, y);
                    check(uv[0] >= -.020001 && uv[0] <= 1.020001 && uv[1] >= -.020001 && uv[1] <= 1.020001,
                          "gaze stays inside finite mirror allowance");
                }
        }
        {
            FrameStabilizer camera;
            Options o;
            o.smoothing = 0;
            o.crop = 20;
            o.aspect = 1;
            o.mirror = false;
            o.horizon_lock = 100;
            o.adaptive_horizon = false;
            o.spectator.gaze_enabled = true;
            o.spectator.gaze_strength = 100;
            o.spectator.gaze_horizontal_degrees = 20;
            Composition result;
            for (unsigned n = 1; n <= 180; ++n) {
                auto f = frame(n);
                const auto roll = axis_angle({0, 0, 1}, .08);
                for (auto &v : f.views) {
                    v.orientation[0] = float(roll.x);
                    v.orientation[1] = float(roll.y);
                    v.orientation[2] = float(roll.z);
                    v.orientation[3] = float(roll.w);
                }
                f.gaze.flags = GazeValid | GazeTracked | GazeTimeKnown;
                f.gaze.sample_time_ns = f.display_time;
                f.gaze.reference_epoch = f.reference_epoch;
                f.gaze.reference_from_eye = roll;
                f.gaze.direction = rotate(axis_angle({0, 1, 0}, .6), {0, 0, -1});
                result = camera.update(f, o);
            }
            const auto target = multiply(axis_angle({0, 0, 1}, .08), result.warp.correction);
            check(std::abs(rotate(target, {1, 0, 0}).y) < 1e-3,
                  "crop exhaustion preserves head leveling within 0.06 degrees while reducing gaze");
        }
        {
            FrameStabilizer camera;
            Options options;
            camera.learn_chat();
            for (unsigned i = 1; i <= 270; ++i) {
                auto input = frame(i);
                input.gaze.flags = GazeValid | GazeTracked | GazeTimeKnown;
                input.gaze.sample_time_ns = input.display_time;
                input.gaze.reference_epoch = input.reference_epoch;
                camera.update(input, options);
            }
            check(camera.spectator_status().zone_valid,
                  "explicit calibration with framing disabled survives first frame");
            check(!camera.spectator_status().in_chat_zone &&
                      camera.spectator_status().gaze_state == GazeState::disabled,
                  "chat coverage becomes unavailable when eye acquisition is not requested");
            auto reset = frame(271);
            reset.reference_epoch = 2;
            camera.update(reset, options);
            check(!camera.spectator_status().zone_valid,
                  "reference reset clears learned zone even while disabled");
        }
        {
            Options edges;
            edges.smoothing = 0;
            edges.aspect = 1;
            edges.eye = 1; // The current UI eye must not reinterpret a retained left image.
            edges.edge_reserve[0] = {true, 2, 20, 5, 5};
            edges.edge_reserve[1] = {true, 20, 2, 5, 5};
            auto wide = frame(1);
            wide.views[0].fov[0] = -2;
            wide.views[0].fov[1] = 2;
            FrameStabilizer stabilizer;
            const auto left = stabilizer.update(wide, edges);
            check(left.valid && std::abs(left.warp.output.left + 1.8) < 1e-6 &&
                      std::abs(left.warp.output.right) < 1e-6,
                  "aspect fit must keep the captured left eye's two-percent left reserve bias");
            auto right_frame = wide;
            right_frame.eye = 1;
            const auto right = stabilizer.update(right_frame, edges);
            check(right.valid && std::abs(right.warp.output.left) < 1e-6 &&
                      std::abs(right.warp.output.right - 1.8) < 1e-6,
                  "captured right eye must select its own mirrored reserve profile");
            edges.crop_left = 10;
            edges.crop_right = 20;
            edges.aspect = 2;
            const auto trimmed = stabilizer.update(wide, edges).warp.output;
            check(std::abs(trimmed.left + 1.608) < 1e-6 && std::abs(trimmed.right - .576) < 1e-6,
                  "framing trims must remain percentages of the reserved projection");
            edges.smoothing = 100;
            edges.aspect = 1;
            stabilizer.reset();
            stabilizer.update(frame(1), edges);
            const auto moving = stabilizer.update(frame(2, .08), edges);
            check(angle_between(moving.warp.correction, {}) > .01, "eye reset fixture needs motion");
            auto switched = frame(2, .08); // Same sequence/time can occur in a new eye snapshot.
            switched.eye = 1;
            check(angle_between(stabilizer.update(switched, edges).warp.correction, {}) < 1e-6,
                  "eye switch must discard the other eye's smoothing history");
            auto bad_eye = frame(3);
            bad_eye.eye = 2;
            check(!stabilizer.update(bad_eye, edges).valid, "invalid captured eye must not select a profile");
        }
        {
            Options edges;
            edges.smoothing = 100;
            edges.mirror = false;
            edges.aspect = .25;
            edges.edge_reserve[0] = {true, 2, 20, 20, 20};
            FrameStabilizer stabilizer;
            stabilizer.update(frame(1), edges);
            const auto fitted = stabilizer.update(frame(2, .4), edges);
            check(fitted.warp.correction_fraction == 1 && angle_between(fitted.warp.correction, {}) > .35 &&
                      corners_inside(fitted.warp),
                  "final aspect framing must supply correction room before OpenXR motion limiting");
            stabilizer.reset();
            edges.smoothing = 0;
            edges.aspect = 1;
            edges.edge_reserve[0] = {true, 5, 5, 5, 5};
            edges.horizon_lock = 100;
            auto tilted = frame(1);
            auto q = axis_angle({0, 0, 1}, pi / 4);
            for (auto &v : tilted.views) {
                v.orientation[2] = float(q.z);
                v.orientation[3] = float(q.w);
            }
            const auto limited = stabilizer.update(tilted, edges);
            check(limited.warp.correction_fraction > 0 && limited.warp.correction_fraction < 1 &&
                      corners_inside(limited.warp) &&
                      angle_between(limited.focus_correction, limited.warp.correction) < 1e-6,
                  "horizon correction and focus view must respect final asymmetric reserve bounds");
            edges.mirror = true;
            edges.horizon_lock = 50;
            q = axis_angle({0, 0, 1}, 5 * pi / 180);
            for (auto &v : tilted.views) {
                v.orientation[2] = float(q.z);
                v.orientation[3] = float(q.w);
            }
            stabilizer.reset();
            const auto adaptive = stabilizer.update(tilted, edges);
            check(std::abs(angle_between(multiply(q, adaptive.warp.correction), {}) * 180 / pi - .53125) <
                      1e-5,
                  "enabled reserve must retain the adaptive horizon response");
        }
        {
            Options symmetric;
            symmetric.crop_left = 10;
            symmetric.crop_bottom = 12;
            symmetric.vertical_offset = 30;
            FrameStabilizer stabilizer;
            for (double aspect : {1., 16. / 9.}) {
                symmetric.aspect = aspect;
                symmetric.edge_reserve[0].enabled = false;
                const auto legacy = stabilizer.update(frame(1), symmetric).warp;
                symmetric.edge_reserve[0].enabled = true;
                const auto separate = stabilizer.update(frame(1), symmetric).warp;
                for (double u : {0., 1.})
                    for (double v : {0., 1.}) {
                        const auto a = map_uv(legacy, u, v), b = map_uv(separate, u, v);
                        check(std::abs(a[0] - b[0]) < 1e-9 && std::abs(a[1] - b[1]) < 1e-9,
                              "five-percent edges must match ten-percent uniform framing");
                    }
            }
            symmetric = {};
            symmetric.edge_reserve[0] = {true, 0, 0, 0, 0};
            const auto zero = stabilizer.update(frame(1), symmetric).warp;
            check(std::abs(map_uv(zero, .5, .5)[0] - .5) < 1e-9 &&
                      std::abs(map_uv(zero, .5, .5)[1] - .5) < 1e-9,
                  "zero edge reserves must keep centered finite aspect framing");
        }
        {
            // The observed iRacing feed is wider than16:9: only the stabilization
            // reserve leaves vertical room, not the aspect-ratio fit itself.
            FrameStabilizer stabilizer;
            auto wide = frame(1);
            wide.views[0].fov[0] = -1.194938f;
            wide.views[0].fov[1] = 1.315188f;
            wide.views[0].fov[2] = .678389f;
            wide.views[0].fov[3] = -.678389f;
            Options offset;
            offset.crop = 7.8;
            offset.stabilize = false;
            offset.mirror = false;
            offset.vertical_offset = -100;
            const auto highest = stabilizer.update(wide, offset).warp;
            offset.vertical_offset = 100;
            const auto lowest = stabilizer.update(wide, offset).warp;
            check(std::abs(map_uv(highest, .5, 0)[1]) < 1e-6 &&
                      std::abs(map_uv(lowest, .5, 1)[1] - 1) < 1e-6 &&
                      map_uv(lowest, .5, .5)[1] - map_uv(highest, .5, .5)[1] > .07,
                  "Y offset must use the captured vertical reserve even without aspect-fit slack");
        }
        {
            Options offset;
            offset.crop = 0;
            offset.crop_top = 10;
            offset.crop_bottom = 20;
            offset.aspect = 2;
            offset.vertical_offset = -100;
            auto top = framing_region(1, offset);
            offset.vertical_offset = 100;
            auto bottom = framing_region(1, offset);
            offset.vertical_offset = 0;
            auto center = framing_region(1, offset);
            check(std::abs(top.top) < 1e-9 && std::abs(top.bottom - .5) < 1e-9 &&
                      std::abs(bottom.top - .5) < 1e-9 && std::abs(bottom.bottom - 1) < 1e-9 &&
                      std::abs(center.top - .2) < 1e-9 && std::abs(center.bottom - .7) < 1e-9,
                  "vertical offset must use full captured boundaries without changing zero-offset framing or "
                  "zoom");
            offset.aspect = .5;
            offset.crop_top = offset.crop_bottom = 0;
            offset.vertical_offset = -100;
            top = framing_region(1, offset);
            offset.vertical_offset = 100;
            bottom = framing_region(1, offset);
            check(top.top == bottom.top && top.bottom == bottom.bottom,
                  "vertical offset must have no effect without vertical aspect slack");
            offset = {};
            offset.aspect = 2;
            offset.edge_reserve[0] = {true, 0, 0, 2, 20};
            FrameStabilizer stabilizer;
            auto sample = [&](double value) {
                offset.vertical_offset = value;
                return stabilizer.update(frame(1), offset).warp;
            };
            const auto biased = sample(0), highest = sample(-100), lowest = sample(100);
            check(std::abs(map_uv(biased, .5, .5)[1] - .2954545454545) < 1e-6 &&
                      std::abs(map_uv(highest, 0, 0)[1]) < 1e-6 &&
                      std::abs(map_uv(lowest, 1, 1)[1] - 1) < 1e-6,
                  "vertical offset must move from asymmetric base bias to each available boundary");
            const auto midpoint = sample(50);
            check(std::abs(map_uv(midpoint, .5, .5)[1] -
                           (map_uv(biased, .5, .5)[1] + map_uv(lowest, .5, .5)[1]) / 2) < 1e-9 &&
                      std::abs((highest.output.up - highest.output.down) -
                               (lowest.output.up - lowest.output.down)) < 1e-9,
                  "offset interpolation must be continuous and preserve zoom");
            for (double invalid : {NAN, INFINITY}) {
                const auto sane = sample(invalid);
                check(std::abs(sane.output.up - biased.output.up) < 1e-9 &&
                          std::abs(sane.output.down - biased.output.down) < 1e-9,
                      "nonfinite vertical offset must use the unshifted framing");
            }
            check(std::abs(sample(-1000).output.up - highest.output.up) < 1e-9 &&
                      std::abs(sample(1000).output.down - lowest.output.down) < 1e-9,
                  "vertical offset must clamp within the permitted framing range");
        }
        Options comfort;
        comfort.crop_left = 7.5;
        comfort.crop_right = 12;
        comfort.crop_top = 3;
        comfort.crop_bottom = 4;
        comfort.aspect = 21. / 9.;
        comfort.output_width = 2560;
        comfort.output_height = 1080;
        comfort.stabilize = comfort.mirror = comfort.adaptive_horizon = false;
        apply_stream_comfort(comfort);
        check(comfort.smoothing == 60 && comfort.horizon_lock == 50 && comfort.crop == 12 &&
                  comfort.mirror_limit == 12 && comfort.mirror_softness == 70 && comfort.stabilize &&
                  comfort.mirror && comfort.adaptive_horizon,
              "Stream Comfort must apply the complete intended stabilization preset");
        check(comfort.crop_left == 7.5 && comfort.crop_right == 12 && comfort.crop_top == 3 &&
                  comfort.crop_bottom == 4 && comfort.aspect == 21. / 9. && comfort.output_width == 2560 &&
                  comfort.output_height == 1080,
              "Stream Comfort must preserve custom framing and output dimensions");
        Options framing;
        framing.crop_left = 10;
        framing.crop_right = 20;
        framing.crop_top = 5;
        framing.crop_bottom = 15;
        framing.aspect = 2;
        auto region = framing_region(2, framing);
        check(std::abs(region.left - .1) < 1e-9 && std::abs(region.right - .8) < 1e-9 &&
                  std::abs(region.top - .1) < 1e-9 && std::abs(region.bottom - .8) < 1e-9,
              "asymmetric crop and aspect fit preserve center and scale");
        auto rolled = frame(1);
        auto base = multiply(axis_angle({0, 1, 0}, .4), axis_angle({1, 0, 0}, .3));
        auto source = multiply(base, axis_angle({0, 0, 1}, .5));
        for (auto &v : rolled.views) {
            v.orientation[0] = float(source.x);
            v.orientation[1] = float(source.y);
            v.orientation[2] = float(source.z);
            v.orientation[3] = float(source.w);
        }
        Options level;
        level.adaptive_horizon = false;
        level.horizon_lock = 100;
        FrameStabilizer ls;
        auto leveled = ls.update(rolled, level);
        check(angle_between(multiply(source, leveled.warp.correction), base) < 1e-6,
              "100 percent horizon removes roll and preserves yaw/pitch");
        level.horizon_lock = 50;
        auto half = ls.update(rolled, level);
        check(angle_between(multiply(source, half.warp.correction),
                            multiply(base, axis_angle({0, 0, 1}, .25))) < 1e-6,
              "50 percent horizon cancels half roll on repeated frame");
        level.horizon_lock = 0;
        check(angle_between(ls.update(rolled, level).warp.correction, {}) < 1e-6,
              "zero horizon preserves baseline orientation");
        level.horizon_lock = 100;
        auto pole = frame(2);
        auto pq = multiply(axis_angle({1, 0, 0}, pi / 2), axis_angle({0, 0, 1}, .8));
        for (auto &v : pole.views) {
            v.orientation[0] = float(pq.x);
            v.orientation[1] = float(pq.y);
            v.orientation[2] = float(pq.z);
            v.orientation[3] = float(pq.w);
        }
        ls.reset();
        check(angle_between(ls.update(pole, level).warp.correction, {}) < 1e-6,
              "vertical pole must fade ambiguous leveling without a jump");
        // A submitted layer space can have the opposite Y hemisphere. Its
        // upright image must not be turned upside down by absolute leveling.
        auto posed = [](std::uint64_t seq, Quat q) {
            auto f = frame(seq);
            for (auto &v : f.views) {
                v.orientation[0] = float(q.x);
                v.orientation[1] = float(q.y);
                v.orientation[2] = float(q.z);
                v.orientation[3] = float(q.w);
            }
            return f;
        };
        for (double tilt : {-.15, .15}) {
            FrameStabilizer inverted;
            Options strength;
            strength.adaptive_horizon = false;
            strength.smoothing = 0;
            const auto basis = axis_angle({0, 0, 1}, pi);
            const auto neutral = multiply(basis, base);
            const auto tilted = multiply(neutral, axis_angle({0, 0, 1}, tilt));
            inverted.update(posed(1, neutral), strength);
            for (double percent : {0., 25., 50., 75., 100.}) {
                strength.horizon_lock = percent;
                const auto result = inverted.update(posed(2, tilted), strength);
                const auto expected = multiply(neutral, axis_angle({0, 0, 1}, tilt * (1 - percent / 100)));
                check(angle_between(multiply(tilted, result.warp.correction), expected) < 1e-6,
                      "inverted layer space must level upright at every slider strength");
            }
            strength.horizon_lock = 50;
            const auto other = multiply(neutral, axis_angle({0, 0, 1}, -tilt));
            const auto result = inverted.update(posed(3, other), strength);
            check(angle_between(multiply(other, result.warp.correction),
                                multiply(neutral, axis_angle({0, 0, 1}, -tilt * .5))) < 1e-6,
                  "crossing inverted neutral must not jump across the atan2 branch cut");
            auto recentered = posed(4, source);
            recentered.reference_epoch++;
            strength.horizon_lock = 100;
            check(angle_between(multiply(source, inverted.update(recentered, strength).warp.correction),
                                base) < 1e-6,
                  "reference reset must discard old horizon hemisphere");
        }
        // Captured read-only from Dan's running iRacing session while looking
        // normally ahead: old code interpreted this as +175.444 degrees roll.
        const auto actual = normalized(Quat{.617033, -.0105413, .784674, .0587049});
        // Starting OBS while the headset hangs upside down chooses the opposite
        // hemisphere. An explicit horizon reset with the headset upright must
        // recover at all strengths, without waiting for a new frame/session.
        for (const auto upright : {Quat{}, actual}) {
            FrameStabilizer mounted;
            Options reset_options;
            reset_options.smoothing = 0;
            reset_options.horizon_lock = 100;
            const auto hanging = multiply(upright, axis_angle({0, 0, 1}, pi));
            mounted.update(posed(1, hanging), reset_options);
            const auto current = posed(2, upright);
            check(angle_between(mounted.update(current, reset_options).warp.correction, {}) > 170 * pi / 180,
                  "upside-down startup must reproduce the reported inverted horizon before reset");
            mounted.reset_horizon();
            FrameStabilizer fresh;
            for (double percent : {0., 25., 50., 75., 100.}) {
                reset_options.horizon_lock = percent;
                const auto recovered = mounted.update(current, reset_options);
                const auto expected = fresh.update(current, reset_options);
                check(angle_between(recovered.warp.correction, expected.warp.correction) < 1e-6,
                      "Reset horizon must recover upright leveling at every strength on the retained frame");
            }
        }
        {
            FrameStabilizer kept, reference;
            Options smooth_options;
            smooth_options.smoothing = 100;
            kept.update(frame(1), smooth_options);
            reference.update(frame(1), smooth_options);
            kept.update(frame(2, .08), smooth_options);
            reference.update(frame(2, .08), smooth_options);
            kept.reset_horizon();
            const auto next = kept.update(frame(3, .12), smooth_options);
            const auto expected = reference.update(frame(3, .12), smooth_options);
            check(angle_between(next.warp.correction, {}) > .01 &&
                      angle_between(next.warp.correction, expected.warp.correction) < 1e-6,
                  "Reset horizon must preserve the running smoothing trajectory");
            kept.reset_horizon();
            smooth_options.smoothing = 0;
            smooth_options.horizon_lock = 100;
            kept.update(posed(4, pq), smooth_options);
            const auto recovered = kept.update(posed(5, actual), smooth_options);
            check(angle_between(recovered.warp.correction, {}) < 5 * pi / 180,
                  "a reset while looking vertically must defer selection until an unambiguous pose");
        }
        {
            FrameStabilizer compared, uninterrupted;
            Options tuning;
            tuning.smoothing = 100;
            tuning.horizon_lock = 75;
            tuning.crop_left = 7;
            tuning.vertical_offset = 35;
            tuning.edge_reserve[0] = {true, 2, 20, 5, 7};
            compared.update(frame(1), tuning);
            uninterrupted.update(frame(1), tuning);
            for (unsigned i = 2; i < 8; ++i) {
                const auto input =
                    posed(i, multiply(axis_angle({0, 1, 0}, i * .03), axis_angle({0, 0, 1}, .1)));
                tuning.compare_original = true;
                const auto original = compared.update(input, tuning);
                tuning.compare_original = false;
                const auto stable = uninterrupted.update(input, tuning);
                check(angle_between(original.warp.correction, {}) < 1e-6,
                      "A/B Original must bypass smoothing and horizon correction");
                check(original.warp.output.left == stable.warp.output.left &&
                          original.warp.output.right == stable.warp.output.right &&
                          original.warp.output.up == stable.warp.output.up &&
                          original.warp.output.down == stable.warp.output.down,
                      "A/B must preserve the exact crop, aspect, reserve and vertical framing");
            }
            const auto resumed = posed(8, multiply(axis_angle({0, 1, 0}, .24), axis_angle({0, 0, 1}, .1)));
            check(angle_between(compared.update(resumed, tuning).warp.correction,
                                uninterrupted.update(resumed, tuning).warp.correction) < 1e-6,
                  "returning after consecutive A/B frames must use uninterrupted smoothing history");
        }
        FrameStabilizer live;
        Options live_options;
        live_options.adaptive_horizon = false;
        live_options.smoothing = 0;
        live_options.horizon_lock = 100;
        const auto live_result = live.update(posed(1, actual), live_options);
        const auto live_target = multiply(actual, live_result.warp.correction);
        check(angle_between(live_result.warp.correction, {}) < 5 * pi / 180 &&
                  std::abs(rotate(live_target, {1, 0, 0}).y) < 1e-6 &&
                  rotate(live_target, {0, 1, 0}).y < -.98,
              "recorded iRacing pose must correct less than five degrees and retain upright hemisphere");
        live_options.horizon_lock = 50;
        check(std::abs(angle_between(live.update(posed(1, actual), live_options).warp.correction, {}) -
                       angle_between(live_result.warp.correction, {}) * .5) < 1e-6,
              "recorded iRacing half strength must give half correction");
        // A large tilt must not silently choose a different upright hemisphere.
        FrameStabilizer large_tilt;
        live_options.horizon_lock = 0;
        large_tilt.update(posed(1, {}), live_options);
        live_options.horizon_lock = 50;
        const auto past_sideways = axis_angle({0, 0, 1}, 100 * pi / 180);
        check(
            angle_between(multiply(past_sideways,
                                   large_tilt.update(posed(2, past_sideways), live_options).warp.correction),
                          axis_angle({0, 0, 1}, 50 * pi / 180)) < 1e-6,
            "horizon hemisphere must remain fixed through a sideways head tilt");
        FrameStabilizer deferred;
        live_options.horizon_lock = 100;
        check(angle_between(deferred.update(posed(1, pq), live_options).warp.correction, {}) < 1e-6,
              "ambiguous vertical startup must defer horizon selection");
        check(angle_between(deferred.update(posed(2, actual), live_options).warp.correction,
                            live_result.warp.correction) < 1e-6,
              "horizon selection must recover when head returns from vertical");
        auto residual = [&](double degrees, double strength, bool adaptive, bool inverted = false) {
            FrameStabilizer filter;
            Options tuning;
            tuning.smoothing = 0;
            tuning.horizon_lock = strength;
            tuning.adaptive_horizon = adaptive;
            auto neutral = inverted ? axis_angle({0, 0, 1}, pi) : Quat{};
            filter.update(posed(1, neutral), tuning);
            auto tilted = multiply(neutral, axis_angle({0, 0, 1}, degrees * pi / 180));
            const auto result = filter.update(posed(2, tilted), tuning);
            const auto relative = multiply(conjugate(neutral), multiply(tilted, result.warp.correction));
            auto right = rotate(relative, {1, 0, 0});
            return std::atan2(right.y, right.x) * 180 / pi;
        };
        check(std::abs(residual(5, 50, true) - .53125) < 1e-5,
              "adaptive half strength should leave only about half a degree of a small five degree tilt");
        check(std::abs(residual(20, 50, true) - 6.85) < 1e-5,
              "adaptive curve should retain some natural movement for a larger tilt");
        for (bool inverted : {false, true}) {
            for (double tilt : {.01, 1., 5., 10., 20., 45., 80.}) {
                double previous = tilt;
                for (int strength = 0; strength <= 100; ++strength) {
                    const double current = residual(tilt, strength, true, inverted);
                    check(current >= -1e-5 && current <= previous + 1e-5,
                          "increasing adaptive strength must never reverse or amplify roll");
                    check(std::abs(current + residual(-tilt, strength, true, inverted)) < 1e-5,
                          "adaptive leveling must be symmetric in both layer hemispheres");
                    previous = current;
                }
                check(std::abs(residual(tilt, 0, true, inverted) - tilt) < 1e-5 &&
                          std::abs(residual(tilt, 100, true, inverted)) < 1e-5,
                      "adaptive endpoints must preserve bypass and full lock");
                check(std::abs(residual(tilt, 50, false, inverted) - tilt * .5) < 1e-5,
                      "disabling adaptive curve must restore the previous linear response");
            }
        }
        double previous_residual = residual(0, 50, true);
        for (int i = 1; i <= 800; ++i) {
            const double current = residual(i * .1, 50, true);
            check(current > previous_residual && current - previous_residual < .1,
                  "adaptive response must vary smoothly without a dead zone, reversal or sudden release");
            previous_residual = current;
        }
        auto wide = frame(3);
        wide.views[0].fov[0] = -2;
        wide.views[0].fov[1] = 2;
        framing.crop = 0;
        auto cropped_projection = ls.update(wide, framing).warp.output;
        check(std::abs(cropped_projection.left + 1.6) < 1e-6 &&
                  std::abs(cropped_projection.right - 1.2) < 1e-6 &&
                  std::abs(cropped_projection.up - .8) < 1e-6 &&
                  std::abs(cropped_projection.down + .6) < 1e-6,
              "OpenXR crop must preserve tangent coordinates and requested optical aspect");
        FrameStabilizer s;
        Options options;
        check(s.update(frame(1), options).valid, "valid first frame must compose");
        auto moving = s.update(frame(2, .08), options);
        check(angle_between(moving.warp.correction, {}) > .01, "motion must be smoothed");
        auto repeat = s.update(frame(2, .08), options);
        check(angle_between(moving.warp.correction, repeat.warp.correction) < 1e-6,
              "redraw must not advance smoothing");
        auto reset_frame = frame(3, .15);
        reset_frame.reference_epoch++;
        check(angle_between(s.update(reset_frame, options).warp.correction, {}) < 1e-6,
              "reference reset must discard old correction");
        auto focus = frame(4, .18);
        auto fq = axis_angle({0, 1, 0}, .21);
        focus.views[1].orientation[1] = float(fq.y);
        focus.views[1].orientation[3] = float(fq.w);
        auto composed = s.update(focus, options);
        const auto target = multiply({focus.views[0].orientation[0], focus.views[0].orientation[1],
                                      focus.views[0].orientation[2], focus.views[0].orientation[3]},
                                     composed.warp.correction);
        check(angle_between(composed.focus_correction, multiply(conjugate(fq), target)) < 1e-6,
              "inset must use its own submitted orientation");
        options.stabilize = false;
        check(angle_between(s.update(frame(5, .2), options).warp.correction, {}) < 1e-6,
              "bypass must preserve source orientation");
        auto invalid = frame(6);
        invalid.views[0].fov[0] = NAN;
        check(!s.update(invalid, options).valid, "invalid projection must not render");
        std::cout << "Composition: smoothing, focus pose, framing, horizon strengths and poles passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
