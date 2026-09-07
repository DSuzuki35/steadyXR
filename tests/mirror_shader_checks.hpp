#pragma once
#include "vrst/core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <obs-module.h>
#include <vector>
// Native pixel readback against an independent bilinear CPU oracle. Called
// while the pairing harness owns the OBS graphics context.
inline int mirror_shader_checks() {
    int failures = 0;
    const auto check = [&](bool pass, const char *label) {
        std::printf("%s: %s\n", pass ? "PASS" : "FAIL", label);
        failures += !pass;
    };
    constexpr unsigned N = 64;
    std::vector<uint8_t> pixels(N * N * 4);
    for (unsigned y = 0; y < N; ++y)
        for (unsigned x = 0; x < N; ++x) {
            const unsigned i = (y * N + x) * 4;
            const bool trimmed = x < 8 || x >= 56 || y < 8 || y >= 56;
            pixels[i] = trimmed ? 255 : uint8_t(x * 3);
            pixels[i + 1] = trimmed ? 0 : uint8_t(y * 3);
            pixels[i + 2] = trimmed ? 255 : 40;
            pixels[i + 3] = 255;
        }
    const uint8_t *data = pixels.data();
    auto *texture = gs_texture_create(N, N, GS_RGBA, 1, &data, 0);
    auto *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    auto *stage = gs_stagesurface_create(N, N, GS_RGBA);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    for (bool pose_shader : {false, true}) {
        char *path = obs_module_file(pose_shader ? "stabilize.effect" : "image-stabilize.effect");
        char *error = nullptr;
        auto *effect = gs_effect_create_from_file(path, &error);
        bfree(path);
        bfree(error);
        check(effect != nullptr, "mirror test effect compiles");
        if (!effect)
            continue;
        const auto vector = [&](const char *name, double x, double y, double z, double w) {
            auto *param = gs_effect_get_param_by_name(effect, name);
            const float values[] = {float(x), float(y), float(z), float(w)};
            if (param)
                gs_effect_set_val(param, values, sizeof(values));
        };
        for (int kind = 0; kind < (pose_shader ? 1 : 3); ++kind) {
            auto *mirror = gs_effect_get_param_by_name(effect, "mirror_edges");
            check(mirror != nullptr, "shader exposes mirror control");
            if (mirror)
                gs_effect_set_bool(mirror, true);
            gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
            vector("input_uv", .125, .875, .125, .875);
            vector("trim_uv", .125, .125, .75, .75);
            vector("source_texel", 1. / N, 1. / N, 0, 0);
            vector("input_projection", -1, 1, 1, -1);
            vector("output_projection", -1, 1, 1, -1);
            vector("pose_input", -1, 1, 1, -1);
            vector("pose_output", -1, 1, 1, -1);
            vector("pose_uv", .125, .875, .125, .875);
            vector("footprint", 1. / N, 1. / N, 0, 0);
            const auto q = vrst::axis_angle({.3, 1, .2}, .28);
            vector("correction", q.x, q.y, q.z, q.w);
            vector("pose_correction", q.x, q.y, q.z, q.w);
            // Full affine residual extends beyond all four edges; hybrid also
            // crosses outside the intermediate view before the pose transform.
            const std::array<double, 3> rx{1.4, .15, -.3}, ry{-.1, 1.35, -.15};
            vector("row_x", rx[0], rx[1], rx[2], 0);
            vector("row_y", ry[0], ry[1], ry[2], 0);
            const bool analysis = !pose_shader && kind == 2;
            const char *technique = pose_shader || kind == 0 ? "Draw"
                                    : kind == 1              ? "DrawHybrid"
                                                             : "AnalyzeHybrid";
            gs_texrender_reset(output);
            if (!gs_texrender_begin(output, N, N)) {
                check(false, "mirror readback target begins");
                continue;
            }
            gs_ortho(0, N, 0, N, -100, 100);
            gs_matrix_identity();
            while (gs_effect_loop(effect, technique))
                gs_draw_sprite(texture, 0, N, N);
            gs_texrender_end(output);
            gs_stage_texture(stage, gs_texrender_get_texture(output));
            uint8_t *mapped = nullptr;
            uint32_t stride = 0;
            if (!gs_stagesurface_map(stage, &mapped, &stride)) {
                check(false, "mirror readback maps");
                continue;
            }
            int bad = 0, valid = 0, invalid = 0;
            auto project = [&](double u, double v) {
                const auto ray = vrst::rotate(q, {-1 + 2 * u, 1 - 2 * v, -1});
                return std::array<double, 2>{.125 + (.5 + ray.x / std::max(.05, -ray.z) * .5) * .75,
                                             .125 + (.5 - ray.y / std::max(.05, -ray.z) * .5) * .75};
            };
            auto inside = [&](const std::array<double, 2> &uv) {
                return uv[0] >= .125 + .5 / N && uv[0] <= .875 - .5 / N && uv[1] >= .125 + .5 / N &&
                       uv[1] <= .875 - .5 / N;
            };
            for (unsigned y = 0; y < N; ++y)
                for (unsigned x = 0; x < N; ++x) {
                    const double u = (x + .5) / N, v = (y + .5) / N;
                    const auto *actual = mapped + y * stride + 4 * x;
                    if (analysis) {
                        bool real = true;
                        for (double dy : {-.375, -.125, .125, .375})
                            for (double dx : {-.375, -.125, .125, .375})
                                real &= inside(project(u + dx / N, v + dy / N));
                        if (real)
                            ++valid;
                        else
                            ++invalid;
                        bad += real ? actual[3] != 255 : actual[3] != 0;
                        continue;
                    }
                    std::array<double, 2> uv{rx[0] * u + rx[1] * v + rx[2], ry[0] * u + ry[1] * v + ry[2]};
                    if (pose_shader)
                        uv = project(u, v);
                    else if (kind == 1)
                        uv = project(uv[0], uv[1]);
                    for (auto &value : uv) {
                        // Triangle wave reflects around the trimmed boundaries.
                        double local = (value - .125) / .75;
                        local -= 2 * std::floor(local / 2);
                        if (local > 1)
                            local = 2 - local;
                        value = std::clamp(.125 + .75 * local, .125 + .5 / N, .875 - .5 / N);
                    }
                    const double expected_r = (uv[0] * N - .5) * 3;
                    const double expected_g = (uv[1] * N - .5) * 3;
                    bad += std::abs(actual[0] - expected_r) > 2 || std::abs(actual[1] - expected_g) > 2 ||
                           actual[2] != 40 || actual[3] != 255;
                }
            gs_stagesurface_unmap(stage);
            if (analysis) {
                std::printf("Analysis mask: real %d, excluded %d, mismatches %d\n", valid, invalid, bad);
                check(valid > 100 && invalid > 100 && bad == 0,
                      "hybrid analysis excludes every footprint outside real trimmed pixels");
            } else {
                std::printf("Mirrored pixel mismatches (%s): %d\n", pose_shader ? "pose" : technique, bad);
                check(bad == 0, pose_shader ? "pose shader reflects actual edge pixels without trim bleed"
                                : kind == 0
                                    ? "image shader reflects all four edges and corners without trim bleed"
                                    : "hybrid composes both warps before reflecting original source pixels");
            }
        }
        gs_effect_destroy(effect);
    }
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
    gs_stagesurface_destroy(stage);
    gs_texrender_destroy(output);
    gs_texture_destroy(texture);
    return failures;
}
