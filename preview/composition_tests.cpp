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
int main() {
    try {
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
