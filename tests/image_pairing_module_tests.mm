#ifdef __APPLE__
#import <AppKit/AppKit.h>
#endif
#include "../plugin/image_filter.hpp"
#include "mirror_shader_checks.hpp"
#include <cstdio>
#include <string>

// This executable links the real ImageFilter implementation. The loaded
// plugin supplies its resource location, without exposing test hooks in it.
static obs_module_t *test_module{};
obs_module_t *obs_current_module(void) {
    return test_module;
}

namespace {
ImageFilter pipeline;
ImageRenderOptions options;
vrst::Settings settings;
vrst::ImageSettings image_settings;
bool rendered{};
bool fallback_requested{};
int failures{};
gs_texture_t *source_pattern{};
void check(bool pass, const char *message) {
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", message);
    failures += !pass;
}
const char *name(void *) {
    return "Timing pairing test";
}
void *create(obs_data_t *, obs_source_t *source) {
    return source;
}
void destroy(void *) {}
unsigned dimension(void *) {
    return 64;
}
void source_render(void *, gs_effect_t *) {
    if (source_pattern) {
        auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        const bool srgb = gs_framebuffer_srgb_enabled();
        gs_enable_framebuffer_srgb(true);
        gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), source_pattern);
        while (gs_effect_loop(effect, "Draw"))
            gs_draw_sprite(source_pattern, 0, 64, 64);
        gs_enable_framebuffer_srgb(srgb);
        return;
    }
    auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    vec4 color;
    vec4_set(&color, .3f, .4f, .8f, 1.f);
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, 64, 64);
}
void filter_render(void *source, gs_effect_t *) {
    rendered = fallback_requested ? pipeline.fallback(static_cast<obs_source_t *>(source), 64, 64, settings)
                                  : pipeline.render(static_cast<obs_source_t *>(source), 64, 64, settings,
                                                    image_settings, 1, options);
}
vrst::PoseResult pose(std::uint64_t sequence, std::uint64_t session = 41) {
    vrst::PoseResult result;
    result.valid = true;
    result.sequence = sequence;
    result.time_ns = sequence * 1000000;
    result.sample_time_ns = result.time_ns + 500000;
    result.interpolated = true;
    result.session = session;
    return result;
}
void exercise(void *data) {
    obs_enter_graphics();
    check(pipeline.load(), "real image shader loads for pose/video pairing");
    failures += mirror_shader_checks();
    auto *source = static_cast<obs_source_t *>(data);
    auto *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    auto draw = [&] {
        gs_texrender_reset(output);
        if (!gs_texrender_begin(output, 64, 64)) {
            check(false, "test output texture begins");
            return;
        }
        gs_ortho(0, 64, 0, 64, -100, 100);
        gs_matrix_identity();
        gs_set_cull_mode(GS_NEITHER);
        gs_enable_blending(true);
        obs_source_video_render(source);
        gs_texrender_end(output);
        check(rendered, "real image pipeline produces an output texture");
    };
    options.hybrid = true;
    options.pose = pose(11);
    draw();
    const auto first_capture = pipeline.capture_time_ns();
    check(first_capture && pipeline.displayed_pose().sequence == 11,
          "warm-up metadata belongs to the current captured image");
    options.pose = pose(99);
    draw();
    check(pipeline.displayed_pose().sequence == 11 && pipeline.capture_time_ns() == first_capture,
          "repeated render ignores a newly selected pose and retains the displayed image capture time");
    pipeline.tick();
    options.pose = pose(12);
    draw();
    auto displayed = pipeline.displayed_pose();
    check(displayed.sequence == 11 && displayed.time_ns == 11000000 && displayed.sample_time_ns == 11500000 &&
              displayed.interpolated && displayed.session == 41 &&
              pipeline.capture_time_ns() == first_capture,
          "delayed image carries its earlier packet, effective interpolation time and session together");
    pipeline.tick();
    options.pose = pose(13, 42);
    draw();
    check(pipeline.displayed_pose().sequence == 12 && pipeline.displayed_pose().session == 41,
          "session restart retains the actual old-session pose on the still-displayed buffered frame");
    options.pose = pose(100, 42);
    draw();
    check(pipeline.displayed_pose().sequence == 12 && pipeline.displayed_pose().session == 41,
          "a second view cannot relabel the buffered frame with the latest new-session packet");
    pipeline.tick();
    options.pose = {};
    draw();
    check(pipeline.displayed_pose().sequence == 13 && pipeline.displayed_pose().session == 42,
          "new-session pose becomes displayed with its own buffered image");
    pipeline.tick();
    draw();
    check(!pipeline.displayed_pose().valid,
          "missing tracking remains invalid when that image reaches the display slot");
    fallback_requested = true;
    draw();
    check(!pipeline.displayed_pose().valid && !pipeline.capture_time_ns(),
          "fallback output clears buffered pose and capture claims");
    fallback_requested = false;
    pipeline.clear();
    check(!pipeline.displayed_pose().valid && !pipeline.capture_time_ns(),
          "clearing image history removes displayed pose and capture metadata");
    options.hybrid = false;
    options.pose = pose(77);
    draw();
    check(!pipeline.displayed_pose().valid, "image-only output never exposes an applied pose");
    std::vector<uint8_t> pattern(64 * 64 * 4);
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            const unsigned i = (y * 64 + x) * 4;
            pattern[i] = uint8_t(x * 3);
            pattern[i + 1] = uint8_t(y * 3);
            pattern[i + 2] = 80;
            pattern[i + 3] = 255;
        }
    const uint8_t *pattern_data = pattern.data();
    source_pattern = gs_texture_create(64, 64, GS_RGBA, 1, &pattern_data, 0);
    // The entire exercise shares one OBS tick; a fresh source avoids reusing
    // the solid texture OBS cached during the earlier fallback metadata check.
    auto *original_source = source;
    source = obs_source_create("pairing-test-input", "mirror pixel input", nullptr, nullptr);
    auto *pixel_filter = obs_source_create("pairing-test-filter", "mirror pixel filter", nullptr, nullptr);
    obs_source_filter_add(source, pixel_filter);
    obs_source_inc_showing(source);
    auto *readback = gs_stagesurface_create(64, 64, GS_RGBA);
    const auto read_pixel = [&](unsigned x, unsigned y) {
        std::array<int, 4> color{-1, -1, -1, -1};
        gs_stage_texture(readback, gs_texrender_get_texture(output));
        uint8_t *mapped = nullptr;
        uint32_t stride = 0;
        if (gs_stagesurface_map(readback, &mapped, &stride)) {
            for (unsigned i = 0; i < 4; ++i)
                color[i] = mapped[y * stride + 4 * x + i];
            gs_stagesurface_unmap(readback);
        }
        return color;
    };
    pipeline.clear();
    settings.mirror_edges = true;
    options.hybrid = true;
    options.pose = pose(88);
    options.pose.correction = vrst::axis_angle({0, 1, 0}, .22);
    draw();
    const auto actual = read_pixel(2, 32);
    const auto expected_warp = vrst::make_warp(64, 64, settings, options.pose.correction);
    auto expected_uv = vrst::map_uv(expected_warp, 2.5 / 64, 32.5 / 64);
    for (auto &coordinate : expected_uv) {
        coordinate -= 2 * std::floor(coordinate / 2);
        if (coordinate > 1)
            coordinate = 2 - coordinate;
        coordinate = std::clamp(coordinate, .5 / 64, 1 - .5 / 64);
    }
    check(std::abs(actual[0] - (expected_uv[0] * 64 - .5) * 3) < 2 && actual[3] == 255,
          "real hybrid pipeline applies full pose and mirrored fill in its displayed frame");
    pipeline.clear();
    settings.input_left = settings.input_right = 20;
    options.hybrid = false;
    draw(); // Leave narrow trim/mirror shader state behind.
    settings.input_left = settings.input_right = 0;
    fallback_requested = true;
    draw();
    const auto fallback_pixel = read_pixel(2, 32);
    std::printf("Fallback edge RGBA: %d %d %d %d\n", fallback_pixel[0], fallback_pixel[1], fallback_pixel[2],
                fallback_pixel[3]);
    check(std::abs(fallback_pixel[0] - 6) <= 1 && fallback_pixel[3] == 255,
          "fallback sets fresh edge parameters rather than reusing earlier mirrored trim");
    fallback_requested = false;
    obs_source_dec_showing(source);
    obs_source_filter_remove(source, pixel_filter);
    obs_source_release(pixel_filter);
    obs_source_release(source);
    source = original_source;
    gs_stagesurface_destroy(readback);
    gs_texture_destroy(source_pattern);
    source_pattern = nullptr;
    gs_texrender_destroy(output);
    pipeline.destroy();
    obs_leave_graphics();
}
} // namespace

int main(int argc, char **argv) {
#ifdef __APPLE__
    @autoreleasepool
#endif
    {
#ifdef _WIN32
        if (argc != 5)
            return 2;
        const std::string obs_root = argv[3];
        const std::string graphics = obs_root + "/bin/64bit/libobs-d3d11.dll";
        if (!obs_startup("en-US", argv[4], nullptr))
            return 2;
        obs_add_data_path((obs_root + "/data/libobs/").c_str());
#else
        if (argc != 3)
            return 2;
        [NSApplication sharedApplication];
        if (!obs_startup("en-US", "/tmp/vrst-pairing-test", nullptr))
            return 2;
        obs_add_data_path("/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/Resources");
        const std::string graphics = "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";
#endif
        obs_video_info video{};
        video.graphics_module = graphics.c_str();
        video.base_width = video.output_width = 640;
        video.base_height = video.output_height = 480;
        video.fps_num = 60;
        video.fps_den = 1;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_BILINEAR;
        if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS ||
            obs_open_module(&test_module, argv[1], argv[2]) != MODULE_SUCCESS ||
            !obs_init_module(test_module))
            return 2;
        settings.output_16_9 = false;
        settings.stabilization_crop = 0;
        obs_source_info input{};
        input.id = "pairing-test-input";
        input.type = OBS_SOURCE_TYPE_INPUT;
        input.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
        input.get_name = name;
        input.create = create;
        input.destroy = destroy;
        input.get_width = input.get_height = dimension;
        input.video_render = source_render;
        obs_register_source(&input);
        auto filter_info = input;
        filter_info.id = "pairing-test-filter";
        filter_info.type = OBS_SOURCE_TYPE_FILTER;
        filter_info.video_render = filter_render;
        obs_register_source(&filter_info);
        auto *source = obs_source_create(input.id, "pairing input", nullptr, nullptr);
        auto *filter = obs_source_create(filter_info.id, "pairing filter", nullptr, nullptr);
        if (!source || !filter)
            return 2;
        obs_source_filter_add(source, filter);
        obs_source_inc_showing(source);
        obs_queue_task(OBS_TASK_GRAPHICS, exercise, source, true);
        obs_source_dec_showing(source);
        obs_source_filter_remove(source, filter);
        obs_source_release(filter);
        obs_source_release(source);
        obs_wait_for_destroy_queue();
        obs_shutdown();
        std::printf("%s: image pairing checks (%d failures)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
}
