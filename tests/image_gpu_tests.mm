#include "vrst/image.hpp"
#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <obs.h>
#include <vector>
void set4(gs_effect_t *fx, const char *name, double x, double y, double z, double w) {
    const float values[]{float(x), float(y), float(z), float(w)};
    gs_effect_set_val(gs_effect_get_param_by_name(fx, name), values, sizeof(values));
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2)
            return 2;
        [NSApplication sharedApplication];
        graphics_t *graphics = nullptr;
        if (gs_create(&graphics, "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib", 0) !=
            GS_SUCCESS)
            return 2;
        gs_enter_context(graphics);
        char *errors = nullptr;
        auto *effect = gs_effect_create_from_file(argv[1], &errors);
        if (!effect) {
            std::fprintf(stderr, "Shader: %s\n", errors ? errors : "missing");
            bfree(errors);
            gs_leave_context();
            gs_destroy(graphics);
            return 1;
        }
        bfree(errors);
        constexpr unsigned N = 256;
        std::vector<std::uint8_t> pixels(N * N * 4);
        for (unsigned y = 0; y < N; ++y)
            for (unsigned x = 0; x < N; ++x) {
                auto i = (y * N + x) * 4;
                pixels[i] = x;
                pixels[i + 1] = y;
                pixels[i + 2] = 73;
                pixels[i + 3] = 192;
            }
        const std::uint8_t *bytes = pixels.data();
        auto *texture = gs_texture_create(N, N, GS_RGBA, 1, &bytes, 0);
        int failures = 0;
        for (int test = 0; test < 12; ++test) {
            vrst::Settings settings;
            settings.output_16_9 = false;
            settings.stabilization_crop = 0;
            vrst::ImageResult r;
            r.width = N;
            r.height = N;
            r.status = vrst::ImageStatus::tracking;
            if (test == 1) {
                settings.input_left = 10;
                settings.input_top = 15;
                settings.input_right = 5;
                settings.stabilization_crop = 20;
            }
            if (test == 2) {
                settings.stabilization_crop = 20;
                r.correction = {5, -3, 0};
            }
            if (test == 3) {
                settings.stabilization_crop = 20;
                r.correction = {0, 0, .06};
            }
            if (test == 4) {
                settings.stabilization_crop = 25;
                settings.output_16_9 = true;
                settings.framing_zoom = 10;
                settings.framing_x = 40;
                r.correction = {3, -2, .04};
            }
            if (test == 5) {
                settings.stabilization_crop = 10;
                r.correction = {150, 90, .6};
            }
            if (test == 6) {
                settings.stabilization_crop = 10;
                r.correction = {150, 90, .6};
                r.status = vrst::ImageStatus::weak;
            }
            const bool hybrid = test >= 8;
            const bool analysis = test == 7 || test == 11;
            vrst::Warp pose;
            if (hybrid) {
                // An asymmetric projection and noncommuting residual rotation
                // expose missing pose correction, reversed composition or a
                // pose correction accidentally applied twice.
                vrst::Settings first;
                first.output_16_9 = false;
                first.stabilization_crop = 18;
                first.input_left = 8;
                first.input_top = 12;
                first.automatic_fov = true;
                pose = vrst::make_warp(N, N, first, vrst::axis_angle({.2, 1, .3}, test == 10 ? .8 : .06),
                                       vrst::Projection{-.7, 1.3, .8, -1.1});
                settings.stabilization_crop = 15;
                r.correction = test == 8 ? vrst::ImageTransform{} : vrst::ImageTransform{4, -2, .05};
                if (!gs_effect_get_param_by_name(effect, "pose_correction")) {
                    std::printf("FAIL image GPU case%d: composed pose shader is missing\n", test);
                    ++failures;
                    continue;
                }
            }
            auto warp = vrst::make_image_warp(N, N, settings, r);
            unsigned width = analysis ? 64 : warp.width, height = analysis ? 64 : warp.height;
            auto *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
            if (!gs_texrender_begin(target, width, height)) {
                ++failures;
                gs_texrender_destroy(target);
                continue;
            }
            vec4 zero;
            vec4_zero(&zero);
            gs_clear(GS_CLEAR_COLOR, &zero, 1, 0);
            gs_ortho(0, float(width), 0, float(height), -100, 100);
            gs_matrix_identity();
            gs_enable_blending(false);
            gs_enable_framebuffer_srgb(false);
            gs_set_cull_mode(GS_NEITHER);
            set4(effect, "row_x", warp.row_x[0], warp.row_x[1], warp.row_x[2], 0);
            set4(effect, "row_y", warp.row_y[0], warp.row_y[1], warp.row_y[2], 0);
            set4(effect, "trim_uv", .1, .2, .8, .7);
            set4(effect, "footprint", .8 / width, .7 / height, 0, 0);
            if (gs_effect_get_param_by_name(effect, "pose_correction")) {
                set4(effect, "pose_input", -1, 1, 1, -1);
                set4(effect, "pose_output", -1, 1, 1, -1);
                set4(effect, "pose_correction", 0, 0, 0, 1);
                set4(effect, "pose_uv", 0, 1, 0, 1);
            }
            if (hybrid) {
                set4(effect, "pose_input", pose.input.left, pose.input.right, pose.input.up, pose.input.down);
                set4(effect, "pose_output", pose.output.left, pose.output.right, pose.output.up,
                     pose.output.down);
                set4(effect, "pose_correction", pose.correction.x, pose.correction.y, pose.correction.z,
                     pose.correction.w);
                set4(effect, "pose_uv", pose.uv_left, pose.uv_right, pose.uv_top, pose.uv_bottom);
                set4(effect, "footprint", 1. / width, 1. / height, 0, 0);
            }
            gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
            const char *technique =
                hybrid ? (analysis ? "AnalyzeHybrid" : "DrawHybrid") : (analysis ? "Analyze" : "Draw");
            while (gs_effect_loop(effect, technique))
                gs_draw_sprite(texture, 0, width, height);
            gs_texrender_end(target);
            auto *stage = gs_stagesurface_create(width, height, GS_RGBA);
            gs_stage_texture(stage, gs_texrender_get_texture(target));
            std::uint8_t *data = nullptr;
            std::uint32_t stride = 0;
            int bad = 0;
            if (gs_stagesurface_map(stage, &data, &stride)) {
                for (unsigned y = 0; y < height; ++y)
                    for (unsigned x = 0; x < width; ++x) {
                        double u = (x + .5) / width, v = (y + .5) / height;
                        double ex = std::clamp(
                            (warp.row_x[0] * u + warp.row_x[1] * v + warp.row_x[2]) * N - .5, 0., 255.);
                        double ey = std::clamp(
                            (warp.row_y[0] * u + warp.row_y[1] * v + warp.row_y[2]) * N - .5, 0., 255.);
                        if (hybrid) {
                            auto uv = vrst::map_uv(pose, analysis ? u : (ex + .5) / N,
                                                   analysis ? v : (ey + .5) / N);
                            ex = std::clamp(uv[0] * N - .5, 0., 255.);
                            ey = std::clamp(uv[1] * N - .5, 0., 255.);
                        }
                        auto *p = data + y * stride + x * 4;
                        if (analysis) {
                            double gray = hybrid ? .2126 * ex + .7152 * ey + .0722 * 73
                                                 : .2126 * ((.1 + .8 * u) * N - .5) +
                                                       .7152 * ((.2 + .7 * v) * N - .5) + .0722 * 73;
                            if (std::abs(p[0] - gray) > 2 || p[0] != p[1] || p[1] != p[2] || p[3] != 255)
                                ++bad;
                        } else if (std::abs(p[0] - ex) > 2 || std::abs(p[1] - ey) > 2 ||
                                   std::abs(int(p[2]) - 73) > 1 || std::abs(int(p[3]) - 192) > 1)
                            ++bad;
                    }
                gs_stagesurface_unmap(stage);
            } else
                ++bad;
            gs_stagesurface_destroy(stage);
            gs_texrender_destroy(target);
            std::printf("%s image GPU case%d (%s): %d mismatched pixels\n", bad ? "FAIL" : "PASS", test,
                        analysis ? "downsample" : "affine", bad);
            if (bad)
                ++failures;
        }
        gs_texture_destroy(texture);
        gs_effect_destroy(effect);
        gs_leave_context();
        gs_destroy(graphics);
        return failures ? 1 : 0;
    }
}
