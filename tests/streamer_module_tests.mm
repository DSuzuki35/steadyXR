#ifdef __APPLE__
#import <AppKit/AppKit.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <obs-module.h>
#include <string>
#include <thread>
int main(int argc, char **argv) {
#ifdef __APPLE__
    @autoreleasepool
#endif
    {
#ifdef _WIN32
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
        if (!obs_startup("en-US", "/tmp/vrst-streamer-module-test", nullptr))
            return 2;
        obs_add_data_path("/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/Resources");
#endif
        obs_video_info v{};
#ifdef _WIN32
        v.graphics_module = graphics_module.c_str();
#else
        v.graphics_module = "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";
#endif
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
        obs_audio_info audio{};
        audio.samples_per_sec = 48000;
        audio.speakers = SPEAKERS_STEREO;
        if (!obs_reset_audio(&audio))
            return 2;
        obs_hotkey_enable_callback_rerouting(true);
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
        std::map<std::string, obs_hotkey_id> keys;
        obs_enum_hotkeys(
            [](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
                const std::string name = obs_hotkey_get_name(key);
                const auto dot = name.find('.');
                (*static_cast<std::map<std::string, obs_hotkey_id> *>(data))[name.substr(0, dot)] = id;
                return true;
            },
            &keys);
        check(keys.count("vrst_compare") && keys.count("vrst_reset") && keys.count("vrst_next_preset"),
              "all three filter hotkeys are registered in real OBS");
        if (keys.empty()) {
            std::printf("No OBS hotkeys were registered for the actual filter instance.\n");
            obs_properties_destroy(p);
            obs_data_release(s);
            obs_data_release(input);
            obs_source_release(filter);
            obs_shutdown();
            return 1;
        }
        obs_key_combination_t combination{INTERACT_CONTROL_KEY, OBS_KEY_F24};
        obs_hotkey_load_bindings(keys.at("vrst_compare"), &combination, 1);
        obs_source_save(filter);
        auto *bindings = obs_data_get_array(s, "hotkey_compare_bindings");
        check(bindings && obs_data_array_count(bindings) == 1, "OBS save persists filter hotkey bindings");
        obs_data_array_release(bindings);
        auto *scene_copy = obs_data_create_from_json(obs_data_get_json(s));
        auto *second = obs_source_create("vr-stabilizer", "second filter", scene_copy, nullptr);
        std::map<std::string, obs_hotkey_id> all_keys;
        obs_enum_hotkeys(
            [](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
                (*static_cast<std::map<std::string, obs_hotkey_id> *>(data))[obs_hotkey_get_name(key)] = id;
                return true;
            },
            &all_keys);
        check(all_keys.size() == 6, "two filter instances register unique hotkeys");
        const std::string second_key = std::string("vrst_compare.") + obs_source_get_uuid(second);
        auto *restored = obs_hotkey_save(all_keys.at(second_key));
        check(restored && obs_data_array_count(restored) == 1,
              "recreated filter restores bindings from scene JSON");
        obs_data_array_release(restored);
        obs_data_release(scene_copy);
        obs_source_release(second);
        obs_wait_for_destroy_queue();
        all_keys.clear();
        obs_enum_hotkeys(
            [](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
                (*static_cast<std::map<std::string, obs_hotkey_id> *>(data))[obs_hotkey_get_name(key)] = id;
                return true;
            },
            &all_keys);
        check(all_keys.size() == 3, "destroying a filter unregisters only its own hotkeys");
        const bool before_compare = obs_data_get_bool(s, "comparison");
        obs_hotkey_trigger_routed_callback(keys.at("vrst_compare"), false);
        check(obs_data_get_bool(s, "comparison") == before_compare, "comparison ignores hotkey release");
        obs_hotkey_trigger_routed_callback(keys.at("vrst_compare"), true);
        check(obs_data_get_bool(s, "comparison") != before_compare, "comparison toggles on hotkey press");
        obs_hotkey_trigger_routed_callback(keys.at("vrst_compare"), true);
        check(obs_data_get_bool(s, "comparison") == before_compare,
              "comparison toggles back without losing settings");
        obs_data_set_string(s, "preset_name", "Alpha");
        obs_data_set_double(s, "smoothing", 11);
        obs_data_set_string(s, "gaming_pc", "127.0.0.1");
        obs_data_set_int(s, "port", 54545);
        obs_source_update(filter, s);
        click("preset_save");
        obs_data_set_string(s, "preset_name", "Beta");
        obs_data_set_double(s, "smoothing", 77);
        obs_source_update(filter, s);
        click("preset_save");
        obs_data_set_int(s, "port", 56565);
        obs_source_update(filter, s);
        obs_hotkey_trigger_routed_callback(keys.at("vrst_next_preset"), false);
        check(obs_data_get_double(s, "smoothing") == 77, "next-preset ignores hotkey release");
        obs_hotkey_trigger_routed_callback(keys.at("vrst_next_preset"), true);
        check(obs_data_get_double(s, "smoothing") == 11 &&
                  std::string(obs_data_get_string(s, "preset_name")) == "Alpha",
              "next preset wraps from final saved name to first");
        check(obs_data_get_int(s, "port") == 56565, "preset hotkey preserves current sender port");
        obs_hotkey_trigger_routed_callback(keys.at("vrst_next_preset"), true);
        check(obs_data_get_double(s, "smoothing") == 77, "next preset advances deterministically");
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
