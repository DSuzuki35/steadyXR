#include "vrst/image.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace vrst;
namespace {
int checks{}, failures{};
void check(bool pass, const char *name) {
    ++checks;
    failures += !pass;
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
}
GrayImage frame(double dx, bool masked = false) {
    GrayImage f{480, 270, std::vector<float>(480 * 270)};
    if (masked)
        f.valid_pixels.assign(f.pixels.size(), 0);
    for (unsigned y = 0; y < f.height; ++y)
        for (unsigned x = 0; x < f.width; ++x) {
            const double a = x - dx, b = y;
            f.pixels[y * f.width + x] =
                float(.5 + .13 * std::sin(.19 * a + .03 * b) + .12 * std::cos(.23 * b - .01 * a) +
                      .11 * std::sin(.13 * a + .21 * b) + .10 * std::cos(.29 * a - .17 * b));
        }
    return f;
}
} // namespace
int main() {
    Settings s;
    s.stabilization_crop = 0;
    s.output_16_9 = false;
    const auto q = axis_angle({0, 1, 0}, .12);
    const auto limited = make_warp(1920, 1080, s, q);
    check(limited.correction_fraction < .001 && corners_inside(limited),
          "default zero crop still limits pose correction");
    s.mirror_edges = true;
    const auto mirrored = make_warp(1920, 1080, s, q);
    check(mirrored.correction_fraction == 1 && angle_between(mirrored.correction, q) < 1e-7 &&
              !corners_inside(mirrored),
          "mirror preserves pose correction beyond crop");
    check(mirrored.width == limited.width && mirrored.height == limited.height,
          "mirror does not change framing or output size");
    // Even mirror must stop before the eye projection becomes singular.
    const auto extreme = make_warp(1920, 1080, s, axis_angle({0, 1, 0}, 2.8));
    bool finite = extreme.correction_fraction < 1;
    for (double u : {0., .5, 1.})
        for (double v : {0., .5, 1.}) {
            const auto uv = map_uv(extreme, u, v);
            finite &= std::isfinite(uv[0]) && std::isfinite(uv[1]) && std::abs(uv[0]) < 100;
        }
    check(finite, "mirror retains a finite forward-facing pose projection at extreme turns");
    ImageResult r;
    r.status = ImageStatus::tracking;
    r.width = 480;
    r.height = 270;
    r.correction = {24, -9, .02};
    const auto iw = make_image_warp(1920, 1080, s, r);
    check(iw.correction_fraction == 1 &&
              std::abs(iw.row_x[0] * .5 + iw.row_x[1] * .5 + iw.row_x[2] - .55) < 1e-8,
          "mirror preserves image shift and roll without crop");
    s.input_left = 9;
    s.input_right = 13;
    s.input_top = 7;
    s.input_bottom = 5;
    r.correction = {24, 0, 0};
    const auto trim = make_image_warp(1920, 1080, s, r);
    check(std::abs(trim.row_x[2] - (.09 + .05 * .78)) < 1e-8 && trim.correction_fraction == 1,
          "mirror retains input trim as the reflection boundary");
    ImageSettings is;
    is.region_top = is.region_bottom = is.region_left = is.region_right = 0;
    is.smoothing = 85;
    is.max_shift_percent = 5;
    ImageStabilizer tracker;
    tracker.process(frame(0, true), 1000000000, is);
    auto masked = tracker.process(frame(2, true), 1016666667, is);
    check(masked.status == ImageStatus::weak && masked.tracked == 0 && masked.correction.x == 0,
          "synthetic textured pixels cannot drive optical flow");
    auto blank = frame(0, true);
    std::fill(blank.pixels.begin(), blank.pixels.end(), 0);
    masked = tracker.process(blank, 1033333334, is);
    check(masked.status == ImageStatus::weak, "losing real pixels is not a scene cut");
    tracker.reset();
    auto real0 = frame(0, true), real1 = frame(1.5, true);
    for (unsigned y = 0; y < real0.height; ++y)
        for (unsigned x = 180; x < real0.width; ++x) {
            real0.valid_pixels[y * real0.width + x] = 1;
            real1.valid_pixels[y * real1.width + x] = 1;
        }
    // Invalid content changes drastically while the genuine patch moves 1.5 px.
    for (unsigned y = 0; y < real0.height; ++y)
        for (unsigned x = 0; x < 180; ++x)
            real1.pixels[y * real1.width + x] = 1 - real0.pixels[y * real0.width + x];
    tracker.process(real0, 1000000000, is);
    masked = tracker.process(real1, 1016666667, is);
    check(masked.status == ImageStatus::tracking && std::abs(masked.motion.x - 1.5) < .15,
          "real interior motion remains trackable beside invalid reflected areas");
    real1.valid_pixels.resize(1);
    check(tracker.process(real1, 1050000000, is).status == ImageStatus::invalid,
          "malformed validity mask is rejected");
    // End-to-end CPU motion plus output mapping: zero crop used to discard all smoothing.
    tracker.reset();
    s = {};
    s.stabilization_crop = 0;
    s.output_16_9 = false;
    double without{}, with{}, prev_without{}, prev_with{};
    for (unsigned i = 0; i < 60; ++i) {
        const double jitter = i % 2 ? .7 : -.7;
        r = tracker.process(frame(jitter), 1000000000ULL + i * 16666667ULL, is);
        s.mirror_edges = false;
        auto a = make_image_warp(1920, 1080, s, r);
        s.mirror_edges = true;
        auto b = make_image_warp(1920, 1080, s, r);
        const double raw_out = jitter - (a.row_x[0] * .5 + a.row_x[1] * .5 + a.row_x[2] - .5) * 480;
        const double mirror_out = jitter - (b.row_x[0] * .5 + b.row_x[1] * .5 + b.row_x[2] - .5) * 480;
        if (i > 10) {
            without += std::pow(raw_out - prev_without, 2);
            with += std::pow(mirror_out - prev_with, 2);
        }
        prev_without = raw_out;
        prev_with = mirror_out;
    }
    std::printf("Zero-crop motion RMS: limited %.4f px, mirror %.4f px\n", std::sqrt(without / 49),
                std::sqrt(with / 49));
    check(with < without * .04, "mirror retains at least 80 percent jitter reduction when crop is exhausted");
    std::printf("%d/%d mirror checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
