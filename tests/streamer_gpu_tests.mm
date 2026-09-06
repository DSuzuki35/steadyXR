#include "image_filter.hpp"
#import <AppKit/AppKit.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

// Use the real OBS module solely to resolve its data files. The test filter
// drives ImageFilter directly so each captured pose is known exactly.
obs_module_t *test_module{};
extern "C" obs_module_t *obs_current_module(void) {
    return test_module;
}
namespace {
unsigned source_width = 640, source_height = 480, draws{};
float shift{}, alpha = 1;
double head_roll{};
unsigned color = 51;
bool textured{};
gs_texture_t *pattern{};
int failures{};
vrst::Settings settings;
vrst::ImageSettings image_settings;
ImageRenderOptions options;
std::uint64_t revision{1};
struct Pipeline {
    obs_source_t *source;
    ImageFilter image;
};
Pipeline *pipeline{};
void check(bool pass, const char *message) {
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", message);
    failures += !pass;
}
const char *input_name(void *) {
    return "Streamer GPU fixture";
}
void *input_create(obs_data_t *, obs_source_t *) {
    return &draws;
}
void input_destroy(void *) {
    obs_enter_graphics();
    gs_texture_destroy(pattern);
    pattern = nullptr;
    obs_leave_graphics();
}
unsigned input_width(void *) {
    return source_width;
}
unsigned input_height(void *) {
    return source_height;
}
void input_render(void *, gs_effect_t *) {
    ++draws;
    auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    vec4 c;
    vec4_set(&c, color / 255.f, .4f, .8f, alpha);
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &c);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, source_width, source_height);
    if (!textured)
        return;
    if (!pattern) {
        std::vector<uint8_t> pixels(640 * 480 * 4);
        for (unsigned y = 0; y < 480; ++y)
            for (unsigned x = 0; x < 640; ++x) {
                auto i = (y * 640 + x) * 4;
                pixels[i] = uint8_t(
                    255 * (.58 + .08 * std::sin(.19 * x + .03 * y) + .08 * std::cos(.23 * y - .01 * x) +
                           .08 * std::sin(.13 * x + .21 * y) + .08 * std::cos(.29 * x - .17 * y)));
                pixels[i + 1] = pixels[i + 2] = pixels[i];
                pixels[i + 3] = 255;
            }
        const uint8_t *data = pixels.data();
        pattern = gs_texture_create(640, 480, GS_RGBA, 1, &data, 0);
    }
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), pattern);
    gs_matrix_push();
    gs_matrix_translate3f(shift, 0, 0);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(pattern, 0, source_width, source_height);
    gs_matrix_pop();
    gs_enable_framebuffer_srgb(srgb);
}
void *filter_create(obs_data_t *, obs_source_t *source) {
    pipeline = new Pipeline{source};
    obs_enter_graphics();
    const bool loaded = pipeline->image.load();
    obs_leave_graphics();
    check(loaded, "streamer pipeline loads its actual GPU effect");
    return pipeline;
}
void filter_destroy(void *data) {
    auto *p = static_cast<Pipeline *>(data);
    obs_enter_graphics();
    p->image.destroy();
    obs_leave_graphics();
    delete p;
}
void filter_render(void *data, gs_effect_t *) {
    auto &p = *static_cast<Pipeline *>(data);
    check(p.image.render(p.source, source_width, source_height, settings, image_settings, revision, options),
          "buffered pipeline render succeeds");
}
unsigned filter_width(void *) {
    return vrst::make_warp(source_width, source_height, settings).width;
}
unsigned filter_height(void *) {
    return vrst::make_warp(source_width, source_height, settings).height;
}
struct Frame {
    obs_source_t *source;
    unsigned color_value{51};
    float shift_value{};
    bool compare{}, hybrid{}, valid{}, texture{};
    double pose_angle{};
    bool reset{};
    unsigned width{}, height{}, capture_draws{}, uncovered{};
    unsigned red[2]{}, alphas[2]{};
    std::uint64_t time[2]{};
    vrst::ImageResult result;
    double crop{};
    std::vector<uint8_t> patch[2];
};
void capture(void *data) {
    auto &f = *static_cast<Frame *>(data);
    color = f.color_value;
    shift = f.shift_value;
    textured = f.texture;
    options.hybrid = f.hybrid;
    options.pose.valid = f.valid;
    options.pose.correction = vrst::axis_angle({0, 1, 0}, f.pose_angle);
    options.pose.source_orientation = options.pose.target_orientation =
        vrst::axis_angle({0, 0, 1}, head_roll);
    if (f.reset)
        ++revision;
    obs_enter_graphics();
    pipeline->image.tick();
    const auto before = draws;
    f.width = filter_width(nullptr);
    f.height = filter_height(nullptr);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        options.comparison = f.compare && repeat;
        auto *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        auto *stage = gs_stagesurface_create(f.width, f.height, GS_RGBA);
        if (!gs_texrender_begin(output, f.width, f.height)) {
            check(false, "test output allocation");
            return;
        }
        vec4 zero;
        vec4_zero(&zero);
        gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
        gs_ortho(0, float(f.width), 0, float(f.height), -100, 100);
        gs_matrix_identity();
        gs_set_cull_mode(GS_NEITHER);
        gs_enable_blending(true);
        obs_source_video_render(f.source);
        gs_texrender_end(output);
        gs_stage_texture(stage, gs_texrender_get_texture(output));
        uint8_t *pixels{};
        uint32_t stride{};
        if (gs_stagesurface_map(stage, &pixels, &stride)) {
            f.red[repeat] = pixels[f.height / 2 * stride + f.width / 2 * 4];
            f.alphas[repeat] = pixels[f.height / 2 * stride + f.width / 2 * 4 + 3];
            for (unsigned y = 0; y < f.height; ++y)
                for (unsigned x = 0; x < f.width; ++x)
                    f.uncovered += pixels[y * stride + x * 4 + 3] < 254;
            for (unsigned y = f.height / 2 - 32; y < f.height / 2 + 32; ++y)
                for (unsigned x = f.width / 2 - 64; x < f.width / 2 + 64; ++x)
                    f.patch[repeat].push_back(pixels[y * stride + x * 4]);
            gs_stagesurface_unmap(stage);
        } else
            check(false, "GPU readback succeeds");
        gs_stagesurface_destroy(stage);
        gs_texrender_destroy(output);
        f.time[repeat] = pipeline->image.capture_time_ns();
    }
    f.capture_draws = draws - before;
    f.result = pipeline->image.result();
    f.crop = pipeline->image.effective_crop();
    obs_leave_graphics();
}
Frame frame(obs_source_t *source, unsigned value, bool hybrid, bool valid, double pose = 0,
            bool texture = false, float movement = 0, bool compare = false, bool reset = false) {
    const auto previous = obs_get_video_frame_time();
    while (obs_get_video_frame_time() == previous)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Frame result{source, value, movement, compare, hybrid, valid, texture, pose, reset};
    obs_queue_task(OBS_TASK_GRAPHICS, capture, &result, true);
    check(result.capture_draws == 1 && result.time[0] == result.time[1],
          "repeat draw captures once and retains displayed local frame timestamp");
    if (alpha == 1)
        check(!result.uncovered, "every hybrid/image output pixel stays covered and opaque");
    return result;
}
double energy(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    double value{};
    for (unsigned i = 0; i < a.size(); ++i)
        value += std::pow(double(a[i]) - b[i], 2);
    return value;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3)
            return 2;
        [NSApplication sharedApplication];
        if (!obs_startup("en-US", "/tmp/vrst-streamer-gpu-test", nullptr))
            return 2;
        obs_add_data_path("/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/Resources");
        obs_video_info video{};
        video.graphics_module = "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";
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
        check(obs_open_module(&test_module, argv[1], argv[2]) == MODULE_SUCCESS,
              "native OBS module provides effect resource directory");
        if (!test_module)
            return 2;
        obs_source_info input{};
        input.id = "streamer-gpu-input";
        input.type = OBS_SOURCE_TYPE_INPUT;
        input.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
        input.get_name = input_name;
        input.create = input_create;
        input.destroy = input_destroy;
        input.get_width = input_width;
        input.get_height = input_height;
        input.video_render = input_render;
        obs_register_source(&input);
        obs_source_info filter{};
        filter.id = "streamer-gpu-filter";
        filter.type = OBS_SOURCE_TYPE_FILTER;
        filter.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
        filter.get_name = input_name;
        filter.create = filter_create;
        filter.destroy = filter_destroy;
        filter.get_width = filter_width;
        filter.get_height = filter_height;
        filter.video_render = filter_render;
        obs_register_source(&filter);
        settings.output_16_9 = false;
        settings.stabilization_crop = 20;
        settings.horizontal_fov = settings.vertical_fov = 90;
        image_settings.region_top = 10;
        image_settings.smoothing = 100;
        auto *source = obs_source_create(input.id, "streamer test input", nullptr, nullptr);
        auto *fx = obs_source_create(filter.id, "streamer test filter", nullptr, nullptr);
        obs_source_filter_add(source, fx);
        const auto start = vrst::monotonic_ns();
        auto a = frame(source, 51, true, true, .06);
        auto b = frame(source, 153, true, true, -.06);
        auto c = frame(source, 204, true, true, .06);
        check(a.red[0] == b.red[0] && c.red[0] > b.red[0] + 80,
              "hybrid displays the preceding captured color with one-frame latency");
        check(a.time[0] >= start && a.time[0] == b.time[0] && c.time[0] > b.time[0] &&
                  c.time[0] <= vrst::monotonic_ns(),
              "calibration timestamps identify the displayed slot in the local monotonic clock");
        // With no residual amount, the displayed A/B difference must come from
        // the saved pose, even though this capture's new pose is identity.
        image_settings.smoothing = 0;
        auto posed = frame(source, 51, true, true, .06, true, 0, true, true);
        auto paired = frame(source, 51, true, true, 0, true, 0, true);
        auto identity = frame(source, 51, true, true, 0, true, 0, true);
        check(energy(posed.patch[0], posed.patch[1]) > 100000 &&
                  energy(posed.patch[0], paired.patch[0]) < 1000 &&
                  energy(identity.patch[0], identity.patch[1]) < 1000,
              "hybrid uses the saved pose once and A/B changes draw correction within the same tick");
        settings.horizon_level = 100;
        head_roll = .06;
        auto leveled = frame(source, 51, true, true, 0, true, 0, true, true);
        check(energy(leveled.patch[0], leveled.patch[1]) > 100000,
              "hybrid applies horizon leveling from the captured source and target orientations");
        settings.horizon_level = 0;
        head_roll = 0;
        settings.input_left = 10;
        settings.input_right = 5;
        settings.input_top = 10;
        settings.framing_zoom = 12;
        settings.framing_x = 30;
        settings.output_16_9 = true;
        auto trimmed = frame(source, 51, true, true, .04, true, 0, true, true);
        check(trimmed.width == 544 && trimmed.height == 306 && !trimmed.uncovered,
              "hybrid applies input trim once and final framing/aspect once");
        settings = {};
        settings.output_16_9 = false;
        settings.stabilization_crop = 20;
        image_settings.smoothing = 100;
        for (bool hybrid : {false, true}) {
            std::vector<uint8_t> previous[2];
            double motion[2]{};
            unsigned tracking{};
            for (unsigned i = 0; i < 18; ++i) {
                auto f = frame(source, 51, hybrid, hybrid, 0, true, i % 2 ? 2 : -2, true, i == 0);
                tracking += f.result.status == vrst::ImageStatus::tracking;
                if (i > 5)
                    for (unsigned k = 0; k < 2; ++k)
                        motion[k] += energy(f.patch[k], previous[k]);
                previous[0] = std::move(f.patch[0]);
                previous[1] = std::move(f.patch[1]);
            }
            std::printf("%s stabilized/A-B energy %.0f / %.0f\n", hybrid ? "hybrid" : "image", motion[0],
                        motion[1]);
            check(tracking >= 14 && motion[1] > 100000 && motion[0] < motion[1] * .25,
                  "A/B retains active image tracking and matches frame/crop with reduced stabilized jitter");
        }
        double measured[2]{};
        unsigned samples[2]{};
        for (unsigned run = 0; run < 2; ++run) {
            for (unsigned i = 0; i < 16; ++i) {
                const double movement = i % 2 ? 2 : -2;
                const double pose = run ? -std::atan(2 * movement / 640 * std::tan(50 * vrst::pi / 180)) : 0;
                auto f = frame(source, 51, true, true, pose, true, movement, true, i == 0);
                if (i > 3 && f.result.status == vrst::ImageStatus::tracking) {
                    measured[run] += std::abs(f.result.motion.x);
                    ++samples[run];
                }
            }
        }
        std::printf("Pose removal measured motion %.3f / %.3f (%u / %u samples)\n", measured[1], measured[0],
                    samples[1], samples[0]);
        check(samples[0] >= 10 && samples[1] >= 10 && measured[1] < measured[0] * .35,
              "hybrid estimates only the residual after known pose motion has been removed on the GPU");
        frame(source, 51, true, false, 0, true);
        auto lost = frame(source, 51, true, false, 0, true);
        check(lost.result.status == vrst::ImageStatus::warming,
              "valid-to-invalid pose transition resets residual image history at the displayed slot");
        frame(source, 51, true, false, 0, true);
        frame(source, 51, true, true, 0, true);
        auto resumed = frame(source, 51, true, true, 0, true);
        check(resumed.result.status == vrst::ImageStatus::warming,
              "invalid-to-valid pose transition resets residual history without double correction");
        settings.stabilization_crop = 1;
        options.crop = {true, 9, 20};
        double last_crop = 1;
        for (unsigned i = 0; i < 8; ++i) {
            auto f = frame(source, 51, true, true, .18, true, 0, true, i == 0);
            check(f.crop >= 1 && f.crop <= 10 && f.crop <= last_crop + 1.5,
                  "automatic hybrid crop changes gradually within configured bounds during A/B");
            last_crop = f.crop;
        }
        check(last_crop > 1.5, "automatic hybrid crop responds to the requested pose correction");
        settings.stabilization_crop = 0;
        options.crop = {true, 10, 30};
        image_settings.max_shift_percent = 5;
        last_crop = 0;
        for (unsigned i = 0; i < 14; ++i) {
            auto f = frame(source, 51, false, false, 0, true, i % 2 ? 4 : -4, true, i == 0);
            check(f.crop >= 0 && f.crop <= 10 && f.crop <= last_crop + 1.5,
                  "automatic image crop follows active correction while A/B preserves the displayed crop");
            last_crop = f.crop;
        }
        check(last_crop > .5, "automatic image crop responds to measured video motion");
        options.crop = {};
        source_width = 3840;
        source_height = 2160;
        auto four_k = frame(source, 51, true, true, .01, false, 0, true, true);
        auto four_k_analysis = frame(source, 51, true, true, .01, false, 0, true);
        check(four_k.width == 3840 && four_k.height == 2160 && !four_k.uncovered,
              "hybrid keeps full 4K output covered with bounded analysis storage");
        check(four_k_analysis.result.width == 480 && four_k_analysis.result.height == 270,
              "4K hybrid readback is bounded to 480 by 270 analysis pixels");
        source_width = 640;
        source_height = 480;
        alpha = .5;
        auto half = frame(source, 102, true, true, .03, false, 0, true, true);
        auto half_delayed = frame(source, 204, true, true, -.03, false, 0, true);
        check(std::abs(int(half.red[0]) - 51) <= 1 && std::abs(int(half.alphas[0]) - 128) <= 1 &&
                  half.red[0] == half.red[1] && half.red[0] == half_delayed.red[0] &&
                  half.alphas[0] == half_delayed.alphas[1],
              "hybrid correction, comparison and delayed draw preserve premultiplied alpha");
        obs_source_filter_remove(source, fx);
        obs_source_release(fx);
        obs_source_release(source);
        obs_wait_for_destroy_queue();
        obs_shutdown();
        std::printf("%s: streamer GPU pipeline (%d failures)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
}
