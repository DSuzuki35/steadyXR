#include "vrst/image.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace vrst;
namespace {
int failed = 0, count = 0;
void check(bool ok, const char *name) {
    ++count;
    if (!ok)
        ++failed;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
constexpr unsigned W = 320, H = 180;
constexpr std::uint64_t step = 16666667;
float texture(double x, double y) {
    return float(.5 + .13 * std::sin(.19 * x + .03 * y) + .12 * std::cos(.23 * y - .01 * x) +
                 .11 * std::sin(.13 * x + .21 * y) + .10 * std::cos(.29 * x - .17 * y));
}
GrayImage frame(double dx = 0, double dy = 0, double angle = 0, unsigned w = W, unsigned h = H) {
    GrayImage f{w, h, std::vector<float>(w * h)};
    double c = std::cos(angle), s = std::sin(angle);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            double px = x - (w - 1) * .5 - dx, py = y - (h - 1) * .5 - dy;
            f.pixels[y * w + x] = texture(c * px + s * py + (w - 1) * .5, -s * px + c * py + (h - 1) * .5);
        }
    return f;
}
GrayImage lighting_frame(double dx, double contrast, double offset = 0) {
    auto f = frame(dx);
    for (auto &pixel : f.pixels)
        pixel = float(.5 + (pixel - .5) * contrast + offset);
    return f;
}
ImageSettings config() {
    ImageSettings s;
    s.smoothing = 100;
    s.region_top = 10;
    s.region_bottom = 10;
    s.max_shift_percent = 4;
    s.max_roll_degrees = 3;
    return s;
}
} // namespace
int main() {
    {
        Settings framing;
        framing.edge_reserve = {true, 2, 20, 5, 5};
        struct Case {
            unsigned width;
            double sampled_width, sampled_height;
        };
        for (const auto &c : {Case{2000, 1560, 877.5}, Case{3000, 1600, 900}}) {
            const auto fitted = make_image_warp(c.width, 1000, framing, {});
            const double sampled_width = fitted.row_x[0] * c.width;
            const double sampled_height = fitted.row_y[1] * 1000;
            check(std::abs(sampled_width - c.sampled_width) < 1e-6 &&
                      std::abs(sampled_height - c.sampled_height) < 1e-6 &&
                      fitted.width * 9 == fitted.height * 16 &&
                      std::abs(sampled_width / sampled_height -
                               double(fitted.width) / fitted.height) < 1e-9,
                  "separate edge reserves must fit sampled pixels to the actual 16:9 output without stretching");
        }
    }
    {
        Settings framing;
        framing.output_16_9 = false;
        framing.stabilization_crop = 0;
        ImageResult result;
        result.width = result.height = 100;
        result.status = ImageStatus::tracking;
        result.correction.x = 20;
        const auto fitted = make_image_warp(100, 100, framing, result, {0, .5, 0, 1});
        check(std::abs(fitted.row_x[2] - .2) < 1e-6 && std::abs(fitted.row_x[0] - .5) < 1e-6 &&
                  fitted.correction_fraction == 1,
              "final image framing supplies correction room before motion limiting");
    }
    auto s = config();
    for (bool horizontal : {true, false}) {
        auto settings = s;
        settings.smoothing = 0;
        settings.motion.separate_axes = true;
        settings.motion.yaw = horizontal ? 100 : 0;
        settings.motion.pitch = horizontal ? 0 : 100;
        ImageStabilizer camera;
        camera.process(frame(), step, settings);
        const auto moved = camera.process(frame(2.4, -1.3), step * 2, settings);
        check(moved.status == ImageStatus::tracking &&
                  (horizontal ? moved.correction.x > 2 && moved.correction.y == 0
                              : moved.correction.y < -1 && moved.correction.x == 0),
              horizontal ? "window smoothing can correct horizontal movement alone"
                         : "window smoothing can correct vertical movement alone");
    }
    ImageStabilizer tracker;
    auto r = tracker.process(frame(), step, s);
    check(r.status == ImageStatus::warming && r.correction.x == 0, "first frame warms with identity");
    r = tracker.process(frame(2.4, -1.3), step * 2, s);
    std::printf("translation: %.4f %.4f %.5f confidence %.2f inliers %u/%u\n", r.correction.x, r.correction.y,
                r.correction.angle, r.confidence, r.inliers, r.tracked);
    check(r.status == ImageStatus::tracking && r.inliers >= 8, "textured translation tracked confidently");
    check(std::abs(r.correction.x - 2.4 * std::exp(-1. / 30)) < .18 &&
              std::abs(r.correction.y + 1.3 * std::exp(-1. / 30)) < .18,
          "subpixel translation has correct inverse-sampling sign and magnitude");
    tracker.reset();
    tracker.process(frame(), step, s);
    r = tracker.process(frame(.8, .4, .012), step * 2, s);
    std::printf("roll: %.4f %.4f %.5f confidence %.2f\n", r.correction.x, r.correction.y, r.correction.angle,
                r.confidence);
    check(r.status == ImageStatus::tracking &&
              std::abs(r.correction.angle - .012 * std::exp(-1. / 30)) < .0015,
          "roll estimate matches known synthetic rotation");
    tracker.reset();
    double sum = 0, motion_sum = 0, previous_output = 0;
    unsigned tracked = 0;
    for (unsigned i = 0; i < 70; ++i) {
        double jitter = (i % 2 ? .7 : -.7);
        r = tracker.process(frame(jitter), step * (i + 1), s);
        if (i > 10) {
            sum += std::pow(jitter - r.correction.x, 2);
            motion_sum += std::pow(jitter - r.correction.x - previous_output, 2);
            tracked += r.status == ImageStatus::tracking;
        }
        previous_output = jitter - r.correction.x;
    }
    std::printf("jitter RMS %.4f; tracked %u/59\n", std::sqrt(sum / 59), tracked);
    check(tracked >= 55 && std::sqrt(sum / 59) < .7 * .4,
          "alternating microjitter reduced by more than 60 percent");
    check(std::sqrt(motion_sum / 59) < 1.4 * .2,
          "frame-to-frame microjitter motion reduced by more than 80 percent");
    tracker.reset();
    s.max_shift_percent = 1;
    for (unsigned i = 0; i < 70; ++i)
        r = tracker.process(frame(i * .3), step * (i + 1), s);
    check(std::abs(r.correction.x) <= W * .01 + 1e-9 && 69 * .3 - r.correction.x > 17,
          "intentional pan continues and correction stays bounded");
    s.smoothing = 0;
    r = tracker.process(frame(23), step * 72, s);
    check(r.correction.x == 0 && r.correction.y == 0 && r.correction.angle == 0,
          "zero smoothing disables all image correction");
    s = config();
    tracker.reset();
    tracker.process(frame(), step, s);
    GrayImage flat{W, H, std::vector<float>(W * H, .5f)};
    r = tracker.process(flat, step * 2, s);
    check(r.status != ImageStatus::tracking && r.correction.x == 0, "texture loss bypasses correction");
    r = tracker.process(flat, step * 3, s);
    check(r.status != ImageStatus::tracking, "flat images never claim a good match");
    tracker.reset();
    tracker.process(frame(), step, s);
    r = tracker.process(frame(2), step * 30, s);
    check(r.status == ImageStatus::warming && r.correction.x == 0, "large frame gap resets temporal state");
    r = tracker.process(frame(3), step * 31, s);
    auto duplicate = tracker.process(frame(3), step * 31, s);
    check(r.correction.x == duplicate.correction.x, "repeated timestamp does not advance smoother");
    s.region_right = 60;
    r = tracker.process(frame(4), step * 32, s);
    check(r.status == ImageStatus::warming, "tracking region change resets history");
    tracker.reset();
    s = config();
    s.region_right = 60;
    tracker.process(frame(), step, s);
    auto mixed = frame(1.2, -.7);
    auto moving = frame(-7, 4);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = W / 2; x < W; ++x)
            mixed.pixels[y * W + x] = moving.pixels[y * W + x];
    r = tracker.process(mixed, step * 2, s);
    check(r.status == ImageStatus::tracking && std::abs(r.correction.x - 1.2 * std::exp(-1. / 30)) < .18,
          "selected cockpit region excludes independently moving scenery");
    tracker.reset();
    s = config();
    s.region_left = s.region_right = 20;
    s.region_top = 40;
    s.region_bottom = 20;
    bool isolated = true;
    for (unsigned i = 0; i < 15; ++i) {
        auto exterior = frame(i % 2 ? 2 : -2, 0, 0, 480, 270);
        for (unsigned y = 108; y < 216; ++y)
            for (unsigned x = 96; x < 384; ++x)
                exterior.pixels[y * 480 + x] = .5f;
        auto match = tracker.process(exterior, step * (i + 1), s);
        isolated &= match.status != ImageStatus::tracking && match.correction.x == 0;
    }
    check(isolated, "all tracking patch pixels stay inside the selected region");
    tracker.reset();
    s = config();
    tracker.process(frame(), step, s);
    auto outliers = frame(1.2, -.7);
    auto opposite = frame(-5, 3);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = W * 3 / 4; x < W; ++x)
            outliers.pixels[y * W + x] = opposite.pixels[y * W + x];
    r = tracker.process(outliers, step * 2, s);
    check(r.status == ImageStatus::tracking && std::abs(r.correction.x - 1.16) < .20,
          "robust fit rejects a moving object covering a quarter of the image");
    tracker.reset();
    tracker.process(frame(), step, s);
    auto brighter = frame();
    for (auto &pixel : brighter.pixels)
        pixel = std::min(1.f, pixel + .04f);
    r = tracker.process(brighter, step * 2, s);
    check(r.status != ImageStatus::tracking || std::hypot(r.correction.x, r.correction.y) < .10,
          "brightness change cannot create a large false camera motion");
    tracker.reset();
    tracker.process(lighting_frame(0, .8), step, s);
    r = tracker.process(lighting_frame(1.2, .8, .10), step * 2, s);
    check(r.status == ImageStatus::tracking && std::abs(r.motion.x - 1.2) < .15,
          "uniform exposure change preserves a genuine subpixel motion match");

    tracker.reset();
    for (unsigned i = 0; i < 12; ++i)
        r = tracker.process(lighting_frame(i * .5, .8), step * (i + 1), s);
    const auto before_loss = r;
    check(before_loss.status == ImageStatus::tracking && before_loss.correction.x > 3,
          "recovery fixture accumulates a visible correction before losing texture");
    r = tracker.process(lighting_frame(5.5, .025), step * 13, s);
    check(r.status == ImageStatus::weak && r.correction.x > before_loss.correction.x * .65 &&
              r.correction.x < before_loss.correction.x && r.motion.x == 0,
          "brief texture loss releases correction gently without inventing measured motion");
    Settings recovery_framing;
    recovery_framing.output_16_9 = false;
    recovery_framing.stabilization_crop = 20;
    const auto recovery_warp = make_image_warp(W, H, recovery_framing, r);
    check(recovery_warp.row_x[0] * .5 + recovery_warp.row_x[1] * .5 + recovery_warp.row_x[2] > .505,
          "weak-tracking release reaches the rendered warp instead of snapping to identity");
    double last_correction = r.correction.x;
    bool smooth_release = true;
    for (unsigned i = 14; i <= 25; ++i) {
        r = tracker.process(lighting_frame(5.5, .025), step * i, s);
        smooth_release &= r.status == ImageStatus::weak && r.correction.x >= 0 &&
                          r.correction.x <= last_correction + 1e-9 &&
                          last_correction - r.correction.x < before_loss.correction.x * .25;
        last_correction = r.correction.x;
    }
    check(smooth_release && r.correction.x == 0,
          "sustained weak tracking releases monotonically to zero within 200 ms");
    tracker.process(lighting_frame(5.5, .8), step * 26, s);
    r = tracker.process(lighting_frame(6.1, .8), step * 27, s);
    check(r.status == ImageStatus::tracking && r.motion.x > .4 && r.correction.x < .3,
          "reacquisition ramps correction in while preserving raw calibration motion");
    for (unsigned i = 28; i <= 40; ++i)
        r = tracker.process(lighting_frame(6.1 + .6 * (i - 27), .8), step * i, s);
    check(r.status == ImageStatus::tracking && r.correction.x > 2,
          "reacquisition restores useful stabilization after the short transition");
    auto unrelated = frame();
    std::uint32_t recovery_noise = 91827;
    for (auto &pixel : unrelated.pixels) {
        recovery_noise = recovery_noise * 1664525U + 1013904223U;
        pixel = float(recovery_noise >> 16) / 65535.f;
    }
    r = tracker.process(unrelated, step * 41, s);
    check(r.status == ImageStatus::scene_change && r.correction.x == 0 && r.correction.y == 0,
          "an unrelated scene clears accumulated correction immediately");
    tracker.reset();
    for (unsigned i = 0; i < 12; ++i)
        r = tracker.process(lighting_frame(i * .5, .8), step * (i + 1), s);
    const double before_turn = r.correction.x;
    r = tracker.process(lighting_frame(15.5, .5), step * 13, s);
    check(r.status == ImageStatus::weak && r.correction.x > before_turn * .65,
          "motion with a contrast drop stays a weak match rather than a false scene cut");
    tracker.reset();
    tracker.process(frame(), step, s);
    auto cut = frame();
    std::uint32_t noise = 125;
    for (auto &pixel : cut.pixels) {
        noise = noise * 1664525U + 1013904223U;
        pixel = float(noise >> 16) / 65535.f;
    }
    r = tracker.process(cut, step * 2, s);
    check(r.status != ImageStatus::tracking && r.correction.x == 0,
          "unrelated scene cut cannot retain previous correction");
    auto bad = frame();
    bad.pixels[7] = std::numeric_limits<float>::quiet_NaN();
    r = tracker.process(bad, step * 3, s);
    check(r.status == ImageStatus::invalid && r.correction.x == 0, "nonfinite pixels rejected");
    Settings framing;
    framing.stabilization_crop = 0;
    framing.output_16_9 = false;
    ImageResult result;
    result.width = W;
    result.height = H;
    result.status = ImageStatus::tracking;
    auto warp = make_image_warp(3840, 2160, framing, result);
    check(warp.width == 3840 && warp.height == 2160 && std::abs(warp.row_x[0] - 1) < 1e-9 &&
              std::abs(warp.row_y[1] - 1) < 1e-9,
          "image identity maps full source without FOV");
    framing.stabilization_crop = 20;
    result.correction = {2, -1, .01};
    warp = make_image_warp(3840, 2160, framing, result);
    check(std::abs(warp.row_x[0] * .5 + warp.row_x[1] * .5 + warp.row_x[2] - (.5 + 2. / W)) < 1e-9,
          "image warp uses analysis-pixel translation at output center");
    result.correction = {100, 100, .7};
    warp = make_image_warp(3840, 2160, framing, result);
    bool inside = true;
    for (double x : {0., 1.})
        for (double y : {0., 1.}) {
            double u = warp.row_x[0] * x + warp.row_x[1] * y + warp.row_x[2],
                   v = warp.row_y[0] * x + warp.row_y[1] * y + warp.row_y[2];
            inside &= u >= -1e-8 && u <= 1 + 1e-8 && v >= -1e-8 && v <= 1 + 1e-8;
        }
    check(inside && warp.correction_fraction < 1, "large image corrections limited to available crop");
    framing.input_left = 7;
    framing.input_bottom = 9;
    framing.output_16_9 = true;
    result.correction = {};
    warp = make_image_warp(3840, 2160, framing, result);
    check(warp.width > 0 && warp.height > 0 && warp.width * 9 == warp.height * 16,
          "trimmed image framing remains exactly 16:9");
    ImageStabilizer bench;
    s = config();
    auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 40; ++i)
        bench.process(frame(std::sin(i) * .6, 0, 0, 480, 270), step * (i + 1), s);
    double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 40;
    std::printf("Benchmark 480x270: %.2f ms/frame including synthetic frame generation\n", ms);
    std::printf("%d/%d image checks passed\n", count - failed, count);
    return failed ? 1 : 0;
}
