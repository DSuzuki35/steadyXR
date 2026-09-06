#import <AppKit/AppKit.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <obs-module.h>
#include <thread>
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3)
            return 2;
        [NSApplication sharedApplication];
        if (!obs_startup("en-US", "/tmp/vrst-streamer-module-test", nullptr))
            return 2;
        obs_add_data_path("/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/Resources");
        obs_video_info v{};
        v.graphics_module = "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";
        v.base_width = v.output_width = 640;
        v.base_height = v.output_height = 480;
        v.fps_num = 60;
        v.fps_den = 1;
        v.output_format = VIDEO_FORMAT_RGBA;
        v.colorspace = VIDEO_CS_709;
        v.range = VIDEO_RANGE_FULL;
        v.scale_type = OBS_SCALE_BILINEAR;
        if (obs_reset_video(&v) != OBS_VIDEO_SUCCESS)
            return 2;
        int fails = 0;
        auto check = [&](bool pass, const char *msg) {
            std::printf("%s %s\n", pass ? "PASS" : "FAIL", msg);
            fails += !pass;
        };
        obs_module_t *m{};
        if (obs_open_module(&m, argv[1], argv[2]) != MODULE_SUCCESS || !obs_init_module(m))
            return 2;
        auto *input = obs_data_create();
        obs_data_set_int(input, "mode", 2);
        auto *filter = obs_source_create("vr-stabilizer", "streamer tools", input, nullptr);
        if (!filter)
            return 2;
        auto *s = obs_source_get_settings(filter);
        auto *p = obs_source_properties(filter);
        obs_properties_apply_settings(p, s);
        auto exists = [&](const char *n) { return obs_properties_get(p, n) != nullptr; };
        check(obs_property_list_item_count(obs_properties_get(p, "mode")) == 3, "hybrid mode is selectable");
        check(exists("comparison") && exists("separate_axes") && exists("smoothing_pitch") &&
                  exists("turn_follow") && exists("auto_crop"),
              "comparison, axes, turn following and automatic crop controls exist");
        check(obs_property_visible(obs_properties_get(p, "image_max_shift")) &&
                  obs_property_visible(obs_properties_get(p, "gaming_pc")),
              "hybrid exposes both image and pose controls");
        check(!obs_data_get_bool(s, "auto_crop") && !obs_data_get_bool(s, "separate_axes") &&
                  !obs_data_get_bool(s, "comparison") && obs_data_get_double(s, "turn_follow") == 0,
              "new behavior defaults off for existing scenes");
        auto click = [&](const char *n) {
            auto *b = obs_properties_get(p, n);
            return b && obs_property_button_clicked(b, filter);
        };
        obs_data_set_string(s, "preset_name", "LMU / Porsche");
        obs_data_set_double(s, "delay_ms", 137);
        obs_data_set_double(s, "smoothing_pitch", 63);
        obs_source_update(filter, s);
        check(click("preset_save"), "save preset callback succeeds");
        obs_data_set_double(s, "delay_ms", 2);
        obs_data_set_double(s, "smoothing_pitch", 1);
        obs_source_update(filter, s);
        check(click("preset_load") && obs_data_get_double(s, "delay_ms") == 137 &&
                  obs_data_get_double(s, "smoothing_pitch") == 63,
              "load preset callback restores tuning");
        check(click("preset_delete"), "delete preset callback succeeds");
        check(exists("calibration_start") && exists("calibration_finish") && exists("calibration_apply"),
              "guided calibration controls exist");
        if (exists("calibration_apply"))
            check(!obs_property_enabled(obs_properties_get(p, "calibration_apply")),
                  "invalid calibration cannot be applied");
        check(click("calibration_start"), "calibration starts without altering saved algorithm");
        check(obs_data_get_int(s, "mode") == 2, "calibration keeps user's algorithm setting");
        check(click("calibration_finish"), "finish calibration handles insufficient observations");
        if (exists("calibration_apply"))
            check(!obs_property_enabled(obs_properties_get(p, "calibration_apply")) &&
                      obs_data_get_double(s, "delay_ms") == 137,
                  "failed solve preserves prior delay and stays unappliable");
        obs_properties_destroy(p);
        obs_data_release(s);
        obs_data_release(input);
        obs_source_release(filter);
        obs_shutdown();
        std::printf("Streamer module failures: %d\n", fails);
        return fails ? 1 : 0;
    }
}
