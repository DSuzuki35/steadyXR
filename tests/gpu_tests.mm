#include "vrst/core.hpp"
#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <obs.h>
#include <string>
#include <vector>

void set4(gs_effect_t *fx, const char *name, double x, double y, double z, double w) {
    vec4 v;
    vec4_set(&v, float(x), float(y), float(z), float(w));
    gs_effect_set_vec4(gs_effect_get_param_by_name(fx, name), &v);
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
            std::fprintf(stderr, "FAIL shader: %s\n", errors ? errors : "missing");
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
                pixels[i] = std::uint8_t(x);
                pixels[i + 1] = std::uint8_t(y);
                pixels[i + 2] = 73;
                pixels[i + 3] = 192;
            }
        const std::uint8_t *bytes = pixels.data();
        auto *texture = gs_texture_create(N, N, GS_RGBA, 1, &bytes, 0);
        int failures = 0;
        for (int test = 0; test < 6; ++test) {
            vrst::Settings settings;
            settings.output_16_9 = false;
            settings.stabilization_crop = 0;
            settings.horizontal_fov = 90;
            settings.vertical_fov = 90;
            vrst::Quat q;
            if (test == 1) {
                settings.stabilization_crop = 20;
                settings.input_left = 10;
                settings.input_top = 15;
                settings.input_right = 5;
            }
            if (test == 2) {
                settings.stabilization_crop = 30;
                q = vrst::axis_angle({0, 1, 0}, .08);
            }
            if (test == 3) {
                settings.stabilization_crop = 25;
                settings.output_16_9 = true;
                settings.framing_zoom = 10;
                settings.framing_x = 40;
                q = vrst::axis_angle({1, .2, .5}, .12);
            }
            if (test == 4) {
                settings.stabilization_crop = 10;
                q = vrst::axis_angle({1, 1, 1}, .8);
            }
            if (test == 5) {
                settings.automatic_fov = true;
                settings.stabilization_crop = 15;
                q = vrst::axis_angle({0, 0, 1}, .03);
            }
            auto warp = vrst::make_warp(N, N, settings, q, vrst::Projection{-.7, 1.3, .8, -1.1});
            auto *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
            if (!gs_texrender_begin(target, warp.width, warp.height)) {
                ++failures;
                continue;
            }
            vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 1, 0);
            gs_ortho(0, float(warp.width), 0, float(warp.height), -100, 100);
            gs_matrix_identity();
            gs_enable_blending(false);
            gs_set_cull_mode(GS_NEITHER); // Match OBS video renderer initialization.
            set4(effect, "input_projection", warp.input.left, warp.input.right, warp.input.up,
                 warp.input.down);
            set4(effect, "output_projection", warp.output.left, warp.output.right, warp.output.up,
                 warp.output.down);
            set4(effect, "correction", warp.correction.x, warp.correction.y, warp.correction.z,
                 warp.correction.w);
            set4(effect, "input_uv", warp.uv_left, warp.uv_right, warp.uv_top, warp.uv_bottom);
            gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
            while (gs_effect_loop(effect, "Draw"))
                gs_draw_sprite(texture, 0, warp.width, warp.height);
            gs_texrender_end(target);
            auto *stage = gs_stagesurface_create(warp.width, warp.height, GS_RGBA);
            gs_stage_texture(stage, gs_texrender_get_texture(target));
            std::uint8_t *data = nullptr;
            std::uint32_t stride = 0;
            int bad = 0;
            if (!gs_stagesurface_map(stage, &data, &stride)) {
                ++failures;
                gs_stagesurface_destroy(stage);
                gs_texrender_destroy(target);
                continue;
            }
            for (unsigned y = 0; y < warp.height; ++y)
                for (unsigned x = 0; x < warp.width; ++x) {
                    auto uv = vrst::map_uv(warp, (x + .5) / warp.width, (y + .5) / warp.height);
                    auto *p = data + y * stride + x * 4;
                    double ex = std::clamp(uv[0] * N - .5, 0., 255.),
                           ey = std::clamp(uv[1] * N - .5, 0., 255.);
                    if (std::abs(p[0] - ex) > 2 || std::abs(p[1] - ey) > 2 || std::abs(int(p[2]) - 73) > 1 ||
                        std::abs(int(p[3]) - 192) > 1)
                        ++bad;
                }
            gs_stagesurface_unmap(stage);
            gs_stagesurface_destroy(stage);
            gs_texrender_destroy(target);
            std::printf("%s GPU case %d: %u x %u, %d mismatched pixels\n", bad ? "FAIL" : "PASS", test,
                        warp.width, warp.height, bad);
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
