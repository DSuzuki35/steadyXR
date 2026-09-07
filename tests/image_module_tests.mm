#ifdef __APPLE__
#import <AppKit/AppKit.h>
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <obs-module.h>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> draws{0}, source_width{640}, source_height{480};
std::atomic<float> red{0.2f}, alpha{1.f};
int failures{};
std::atomic<bool> textured{false};
std::atomic<float> shift{0};
gs_texture_t *pattern{};
void check(bool pass, const char *message) {
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", message);
    failures += !pass;
}
const char *name(void *) {
    return "Image pipeline test input";
}
void *create(obs_data_t *, obs_source_t *) {
    return &draws;
}
void destroy(void *) {
    obs_enter_graphics();
    gs_texture_destroy(pattern);
    obs_leave_graphics();
}
unsigned width(void *) {
    return source_width.load();
}
unsigned height(void *) {
    return source_height.load();
}
void render(void *, gs_effect_t *) {
    auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    vec4 color;
    // Like OBS Color Source, emit straight alpha and inherit the caller's blend state.
    vec4_set(&color, red.load(), 0.4f, 0.8f, alpha.load());
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, source_width.load(), source_height.load());
    if (textured.load()) {
        if (!pattern) {
            const unsigned w = source_width.load(), h = source_height.load();
            std::vector<uint8_t> pixels(w * h * 4);
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const auto value = uint8_t(
                        255 * (.58 + .08 * std::sin(.19 * x + .03 * y) + .08 * std::cos(.23 * y - .01 * x) +
                               .08 * std::sin(.13 * x + .21 * y) + .08 * std::cos(.29 * x - .17 * y)));
                    const unsigned offset = (y * w + x) * 4;
                    pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = value;
                    pixels[offset + 3] = 255;
                }
            const uint8_t *data = pixels.data();
            pattern = gs_texture_create(w, h, GS_RGBA, 1, &data, 0);
        }
        const bool previous_srgb = gs_framebuffer_srgb_enabled();
        gs_enable_framebuffer_srgb(true);
        effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), pattern);
        gs_matrix_push();
        gs_matrix_translate3f(shift.load(), 0, 0);
        while (gs_effect_loop(effect, "Draw"))
            gs_draw_sprite(pattern, 0, source_width.load(), source_height.load());
        gs_matrix_pop();
        gs_enable_framebuffer_srgb(previous_srgb);
    }
    ++draws;
}
struct Capture {
    obs_source_t *source;
    unsigned draws{}, width{}, height{}, bad{};
    unsigned red[2]{}, alpha[2]{};
    std::uint64_t checksum{}, top_checksum{};
    unsigned dark_panel_pixels{}, bright_panel_pixels{};
    bool good{true};
    double elapsed_ms{};
    std::vector<uint8_t> patch;
    std::string preview_path;
};
void capture(void *data) {
    auto &c = *static_cast<Capture *>(data);
    const auto started = std::chrono::steady_clock::now();
    obs_enter_graphics();
    c.width = obs_source_get_width(c.source);
    c.height = obs_source_get_height(c.source);
    const auto before = draws.load();
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        auto *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        auto *stage = gs_stagesurface_create(c.width, c.height, GS_RGBA);
        if (!gs_texrender_begin(output, c.width, c.height)) {
            c.good = false;
            break;
        }
        vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, float(c.width), 0, float(c.height), -100, 100);
        gs_matrix_identity();
        gs_set_cull_mode(GS_NEITHER);
        gs_enable_blending(true);
        obs_source_video_render(c.source);
        gs_texrender_end(output);
        gs_stage_texture(stage, gs_texrender_get_texture(output));
        uint8_t *pixels = nullptr;
        uint32_t stride{};
        if (gs_stagesurface_map(stage, &pixels, &stride)) {
            if (!repeat && !c.preview_path.empty()) {
                if (auto *file = std::fopen(c.preview_path.c_str(), "wb")) {
                    std::fprintf(file, "P6\n%u %u\n255\n", c.width, c.height);
                    std::vector<uint8_t> row(c.width * 3);
                    for (unsigned y = 0; y < c.height; ++y) {
                        for (unsigned x = 0; x < c.width; ++x)
                            std::memcpy(row.data() + x * 3, pixels + y * stride + x * 4, 3);
                        std::fwrite(row.data(), 1, row.size(), file);
                    }
                    std::fclose(file);
                }
            }
            c.red[repeat] = pixels[(c.height / 2) * stride + (c.width / 2) * 4];
            c.alpha[repeat] = pixels[(c.height / 2) * stride + (c.width / 2) * 4 + 3];
            for (unsigned y = 0; y < c.height; ++y)
                for (unsigned x = 0; x < c.width; ++x) {
                    const auto *p = pixels + y * stride + x * 4;
                    if (!repeat) {
                        c.checksum = c.checksum * 131 + p[0] + 257 * p[1] + 65537 * p[2] + p[3];
                        if (y < c.height / 2)
                            c.top_checksum = c.top_checksum * 131 + p[0] + 257 * p[1] + 65537 * p[2] + p[3];
                        else {
                            c.dark_panel_pixels += p[0] < 40 && p[1] < 40 && p[2] < 40 && p[3] == 255;
                            c.bright_panel_pixels += p[0] > 220 && p[1] > 220 && p[2] > 220 && p[3] == 255;
                        }
                    }
                    if (p[3] < 254 || p[1] < 50 || p[2] < p[1])
                        ++c.bad;
                }
            if (!repeat)
                for (unsigned y = c.height / 2 - 32; y < c.height / 2 + 32; ++y)
                    for (unsigned x = c.width / 2 - 64; x < c.width / 2 + 64; ++x)
                        c.patch.push_back(pixels[y * stride + x * 4]);
            gs_stagesurface_unmap(stage);
        } else
            c.good = false;
        gs_stagesurface_destroy(stage);
        gs_texrender_destroy(output);
    }
    c.draws = draws.load() - before;
    obs_leave_graphics();
    c.elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}
Capture frame(obs_source_t *source, float value, bool opaque = true, const std::string &preview_path = {}) {
    red.store(value);
    Capture c{source};
    c.preview_path = preview_path;
    obs_queue_task(OBS_TASK_GRAPHICS, capture, &c, true);
    std::printf("Frame red %.2f: output %u/%u, draws %u, size %ux%u, uncovered %u, readback %.1f ms\n", value,
                c.red[0], c.red[1], c.draws, c.width, c.height, c.bad, c.elapsed_ms);
    if (opaque)
        check(c.good && !c.bad, "image output is opaque and covers every output pixel");
    else
        check(c.good, "semitransparent output can be read back");
    check(c.red[0] == c.red[1] && c.alpha[0] == c.alpha[1] && c.draws <= 1,
          "repeat render reuses frame and captures source at most once per video tick");
    return c;
}
} // namespace

int main(int argc, char **argv) {
#ifdef __APPLE__
    @autoreleasepool
#endif
    {
#ifdef _WIN32
        // Arguments supplied by test-module-windows.ps1. Keep test state away
        // from the user's OBS configuration and load the installed D3D11 backend.
        if (argc != 5)
            return 2;
        const std::string obs_root = argv[3];
        const std::string graphics_module = obs_root + "/bin/64bit/libobs-d3d11.dll";
        if (!obs_startup("en-US", argv[4], nullptr))
            return 2;
        obs_add_data_path((obs_root + "/data/libobs/").c_str());
#else
        if (argc != 3)
            return 2;
        [NSApplication sharedApplication];
        if (!obs_startup("en-US", "/tmp/vrst-image-module-test", nullptr))
            return 2;
        obs_add_data_path("/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/Resources");
#endif
        obs_video_info video{};
#ifdef _WIN32
        video.graphics_module = graphics_module.c_str();
#else
        video.graphics_module = "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";
#endif
        video.base_width = video.output_width = 640;
        video.base_height = video.output_height = 480;
        video.fps_num = 60;
        video.fps_den = 1;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_BILINEAR;
        if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
            return 2;
        obs_module_t *module{};
        check(obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS && obs_init_module(module),
              "actual native plugin loads under installed OBS with compatible API headers");
        if (!module)
            return 2;
        obs_source_info info{};
        info.id = "vrst-image-test-input";
        info.type = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
        info.get_name = name;
        info.create = create;
        info.destroy = destroy;
        info.get_width = width;
        info.get_height = height;
        info.video_render = render;
        obs_register_source(&info);
        auto *source = obs_source_create(info.id, "test input", nullptr, nullptr);
        auto *settings = obs_data_create();
        obs_data_set_int(settings, "mode", 1);
        obs_data_set_bool(settings, "output_16_9", false);
        obs_data_set_double(settings, "stabilization_crop", 0);
        obs_data_set_double(settings, "smoothing", 0);
        auto *filter = obs_source_create("vr-stabilizer", "test filter", settings, nullptr);
        check(source && filter, "image filter instance created with image shader");
        if (!source || !filter)
            return 2;
        obs_source_filter_add(source, filter);
        obs_source_inc_showing(source);
        auto *props = obs_source_properties(filter);
        obs_properties_apply_settings(props, settings);
        check(obs_property_visible(obs_properties_get(props, "image_max_shift")) &&
                  !obs_property_visible(obs_properties_get(props, "gaming_pc")) &&
                  !obs_property_visible(obs_properties_get(props, "horizontal_fov")) &&
                  !obs_property_visible(obs_properties_get(props, "delay_ms")) &&
                  !obs_property_visible(obs_properties_get(props, "horizon_level")),
              "image mode shows image controls and hides network, FOV, delay and horizon");
        const auto first = frame(source, .2f);
        const auto second = frame(source, .4f);
        const auto third = frame(source, .6f);
        const auto fourth = frame(source, .8f);
        check(first.red[0] == second.red[0] && third.red[0] > second.red[0] + 20 &&
                  fourth.red[0] > third.red[0] + 20,
              "warm-up displays current frame then each output displays the preceding captured frame");
        check(obs_property_button_clicked(obs_properties_get(props, "refresh_diagnostics"), filter) &&
                  std::strstr(obs_property_description(obs_properties_get(props, "diagnostics")),
                              "one OBS frame"),
              "image diagnostics explicitly report one-frame video delay");
        check(obs_property_button_clicked(obs_properties_get(props, "reset_smoothing"), filter),
              "reset callback succeeds");
        const auto reset = frame(source, .2f);
        check(reset.red[0] == first.red[0],
              "reset discards buffered frame and renders fresh identity warm-up");
        std::this_thread::sleep_for(std::chrono::milliseconds(65));
        const auto resumed = frame(source, .4f);
        check(resumed.red[0] == third.red[0],
              "resume after skipped render ticks discards older buffered video");
        obs_source_set_enabled(filter, false);
        obs_source_set_enabled(filter, true);
        const auto toggled = frame(source, .6f);
        check(toggled.red[0] == fourth.red[0],
              "disable/re-enable between ticks invalidates image history and buffer");
        obs_data_set_double(settings, "input_left", 12.5);
        obs_data_set_double(settings, "input_right", 12.5);
        obs_data_set_double(settings, "input_top", 10);
        obs_data_set_double(settings, "input_bottom", 10);
        obs_source_update(filter, settings);
        auto trimmed = frame(source, .2f);
        check(trimmed.width == 480 && trimmed.height == 384 && trimmed.red[0] == first.red[0],
              "input trim resets image buffer and preserves correct aspect and dimensions");
        obs_data_set_double(settings, "region_top", 30);
        obs_source_update(filter, settings);
        const auto region = frame(source, .6f);
        check(region.red[0] == fourth.red[0], "tracking region edit restarts with the current image");
        obs_data_set_int(settings, "mode", 0);
        obs_source_update(filter, settings);
        obs_properties_apply_settings(props, settings);
        check(obs_property_visible(obs_properties_get(props, "horizon_level")) &&
                  !obs_property_visible(obs_properties_get(props, "image_max_shift")),
              "pose mode restores horizon controls");
        frame(source, .4f);
        obs_data_set_int(settings, "mode", 1);
        obs_source_update(filter, settings);
        const auto mode = frame(source, .2f);
        check(mode.red[0] == first.red[0], "mode switch clears old image frames");
        source_width = 800;
        source_height = 600;
        const auto resized = frame(source, .6f);
        check(resized.width == 600 && resized.height == 480 && resized.red[0] == fourth.red[0],
              "source size change reallocates matching video and analysis slots");
        obs_data_set_double(settings, "horizontal_fov", 10);
        obs_data_set_double(settings, "vertical_fov", 170);
        obs_data_set_bool(settings, "automatic_fov", true);
        obs_data_set_double(settings, "delay_ms", 900);
        obs_source_update(filter, settings);
        const auto irrelevant = frame(source, .8f);
        check(irrelevant.width == resized.width && irrelevant.height == resized.height &&
                  irrelevant.red[0] == resized.red[0],
              "image mode ignores pose FOV and delay settings");
        // End-to-end moving texture: CPU creates only the test source texture;
        // the plugin downsamples, stages, tracks and warps its buffered GPU video.
        textured = true;
        for (const char *key : {"input_left", "input_right", "input_top", "input_bottom"})
            obs_data_set_double(settings, key, 0);
        obs_data_set_double(settings, "region_top", 10);
        obs_data_set_double(settings, "stabilization_crop", 10);
        double motion_energy[2]{};
        for (unsigned run = 0; run < 2; ++run) {
            obs_data_set_double(settings, "smoothing", run ? 100 : 0);
            obs_source_update(filter, settings);
            obs_property_button_clicked(obs_properties_get(props, "reset_smoothing"), filter);
            std::vector<uint8_t> previous;
            for (unsigned i = 0; i < 16; ++i) {
                shift = i % 2 ? 2.f : -2.f;
                auto image = frame(source, .2f);
                if (i > 5)
                    for (unsigned j = 0; j < previous.size(); ++j)
                        motion_energy[run] += std::pow(double(image.patch[j]) - previous[j], 2);
                previous = std::move(image.patch);
            }
        }
        std::printf("End-to-end jitter energies: identity %.0f, stabilized %.0f, ratio %.4f\n",
                    motion_energy[0], motion_energy[1], motion_energy[1] / motion_energy[0]);
        check(motion_energy[0] > 100000 && motion_energy[1] < motion_energy[0] * .25,
              "actual image filter reduces known alternating texture jitter by over 50% RMS");
        obs_property_button_clicked(obs_properties_get(props, "refresh_diagnostics"), filter);
        const char *diagnostics = obs_property_description(obs_properties_get(props, "diagnostics"));
        std::printf("Final image diagnostics: %s\n", diagnostics);
        check(std::strstr(diagnostics, "Tracking image features") && std::strstr(diagnostics, "360 x 270"),
              "GPU downsample and stage feed confident tracking at preserved analysis aspect ratio");
        // This functional check reads every 4K output pixel twice per sample.
        // That test-only readback can exceed 16.7 ms and intentionally trigger
        // the skipped-render reset. Give it a 33.3 ms period; the buffering,
        // repeated-render, jitter and skipped-tick checks above stay at 60 fps.
        video.fps_num = 30;
        check(obs_reset_video(&video) == OBS_VIDEO_SUCCESS,
              "4K full-pixel readback verification uses a controlled 30 fps test clock");
        source_width = 3840;
        source_height = 2160;
        frame(source, .2f);
        frame(source, .2f);
        const auto four_k = frame(source, .2f);
        obs_property_button_clicked(obs_properties_get(props, "refresh_diagnostics"), filter);
        diagnostics = obs_property_description(obs_properties_get(props, "diagnostics"));
        std::printf("4K image diagnostics: %s\n", diagnostics);
        check(
            four_k.width == 3840 && four_k.height == 2160 && std::strstr(diagnostics, "480 x 270"),
            "4K full-resolution video uses bounded 480x270 GPU analysis without changing output dimensions");
        textured = false;
        frame(source, .2f);
        const auto lost = frame(source, .2f);
        obs_property_button_clicked(obs_properties_get(props, "refresh_diagnostics"), filter);
        diagnostics = obs_property_description(obs_properties_get(props, "diagnostics"));
        std::printf("Texture-loss diagnostics: %s\n", diagnostics);
        check(lost.red[0] == first.red[0] && (std::strstr(diagnostics, "gently releasing correction") ||
                                              std::strstr(diagnostics, "Scene changed")),
              "texture loss reports weak features or a scene cut and displays its matching video frame");
        video.fps_num = 60;
        check(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "alpha and mode checks resume at 60 fps");
        source_width = 640;
        source_height = 480;
        alpha = .5f;
        obs_data_set_int(settings, "mode", 0);
        obs_source_update(filter, settings);
        auto changed_at = obs_get_video_frame_time();
        while (obs_get_video_frame_time() == changed_at)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto transparent_pose = frame(source, .4f, false);
        obs_data_set_int(settings, "mode", 1);
        obs_source_update(filter, settings);
        changed_at = obs_get_video_frame_time();
        while (obs_get_video_frame_time() == changed_at)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto transparent_image = frame(source, .4f, false);
        const auto transparent_delayed = frame(source, .4f, false);
        std::printf("Half-alpha red/alpha: pose %u/%u, image %u/%u, delayed %u/%u\n", transparent_pose.red[0],
                    transparent_pose.alpha[0], transparent_image.red[0], transparent_image.alpha[0],
                    transparent_delayed.red[0], transparent_delayed.alpha[0]);
        check(std::abs(int(transparent_pose.red[0]) - 51) <= 1 &&
                  std::abs(int(transparent_pose.alpha[0]) - 128) <= 1 &&
                  transparent_image.red[0] == transparent_pose.red[0] &&
                  transparent_image.alpha[0] == transparent_pose.alpha[0] &&
                  transparent_delayed.red[0] == transparent_pose.red[0] &&
                  transparent_delayed.alpha[0] == transparent_pose.alpha[0],
              "straight-alpha source is premultiplied once in image warm-up and delayed output, matching "
              "pose mode");
        alpha = 1.f;
        obs_data_set_double(settings, "smoothing", 0);
        obs_data_set_double(settings, "stabilization_crop", 0);
        obs_source_update(filter, settings);
        frame(source, .4f);
        const auto timing_default = frame(source, .4f);
        obs_data_set_bool(settings, "photo_timing_overlay", false);
        obs_source_update(filter, settings);
        const auto timing_off = frame(source, .4f);
        check(timing_default.checksum == timing_off.checksum && !timing_off.dark_panel_pixels,
              "default photo timing off preserves every output pixel");
        obs_data_set_bool(settings, "photo_timing_overlay", true);
        obs_source_update(filter, settings);
#ifdef _WIN32
        const std::string preview_path = std::string(argv[4]) + "/photo-timing-output.ppm";
#else
        const std::string preview_path = "/tmp/vrst-photo-timing-output.ppm";
#endif
        const auto timing_on = frame(source, .8f, false, preview_path);
        check(timing_on.dark_panel_pixels > 1000 && timing_on.bright_panel_pixels > 100 &&
                  timing_on.top_checksum == timing_off.top_checksum,
              "enabled photo timing draws readable text after stabilization inside the bottom of output");
        check(timing_on.red[0] == timing_off.red[0],
              "enabling photo timing preserves the displayed buffered frame");
        obs_data_set_bool(settings, "photo_timing_overlay", false);
        obs_source_update(filter, settings);
        const auto timing_removed = frame(source, .2f);
        check(!timing_removed.dark_panel_pixels && timing_removed.red[0] > timing_on.red[0] + 50,
              "disabling photo timing preserves buffered video and never burns the panel into analysis");
        for (int mode : {0, 2}) {
            obs_data_set_int(settings, "mode", mode);
            obs_data_set_bool(settings, "photo_timing_overlay", true);
            obs_data_set_bool(settings, "comparison", true);
            obs_source_update(filter, settings);
            const auto diagnostic = frame(source, .4f, false);
            check(diagnostic.dark_panel_pixels > 1000 && diagnostic.bright_panel_pixels > 100,
                  "photo timing also draws in pose and hybrid comparison output without tracking");
        }
        obs_data_set_bool(settings, "comparison", false);
        obs_data_set_int(settings, "mode", 1);
        obs_source_update(filter, settings);
        check(obs_property_button_clicked(obs_properties_get(props, "calibration_start"), filter),
              "photo timing can remain enabled while parked-car calibration starts");
        const auto calibration_overlay = frame(source, .4f, false);
        check(calibration_overlay.dark_panel_pixels > 1000 && calibration_overlay.bright_panel_pixels > 100,
              "photo timing draws after calibration video output");
        obs_data_set_bool(settings, "photo_timing_overlay", false);
        obs_source_update(filter, settings);
        obs_property_button_clicked(obs_properties_get(props, "refresh_diagnostics"), filter);
        check(!obs_property_enabled(obs_properties_get(props, "calibration_start")) &&
                  obs_property_enabled(obs_properties_get(props, "calibration_finish")),
              "turning photo timing off preserves an active image-only calibration");
        obs_property_button_clicked(obs_properties_get(props, "calibration_finish"), filter);
        obs_properties_destroy(props);
        obs_data_release(settings);
        obs_source_dec_showing(source);
        obs_source_filter_remove(source, filter);
        obs_source_release(filter);
        obs_source_release(source);
        obs_wait_for_destroy_queue();
        obs_shutdown();
        std::printf("%s: image module checks (%d failures)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
}
