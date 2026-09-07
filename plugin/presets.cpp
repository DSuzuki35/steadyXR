#include "presets.hpp"
#include <algorithm>
namespace vrst_plugin {
namespace {
std::string clean(const std::string &name) {
    const auto first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    auto result = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
    return result.size() <= 64 ? result : std::string{};
}
void copy_tuning(obs_data_t *to, obs_data_t *from) {
    auto has = [&](const char *key) {
        return obs_data_has_user_value(from, key) || obs_data_has_default_value(from, key);
    };
    for (const char *key :
         {"horizontal_fov",      "vertical_fov",    "delay_ms",           "smoothing",    "horizon_level",
          "horizon_max_degrees", "image_max_shift", "image_max_roll",     "region_left",  "region_right",
          "region_top",          "region_bottom",   "stabilization_crop", "framing_zoom", "framing_x",
          "framing_y",           "input_left",      "input_right",        "input_top",    "input_bottom",
          "smoothing_yaw",       "smoothing_pitch", "smoothing_roll",     "turn_follow",  "turn_speed",
          "auto_crop_extra",     "auto_crop_speed"})
        if (has(key))
            obs_data_set_double(to, key, obs_data_get_double(from, key));
    for (const char *key :
         {"automatic_fov", "image_track_roll", "output_16_9", "separate_axes", "auto_crop", "mirror_edges"})
        if (has(key))
            obs_data_set_bool(to, key, obs_data_get_bool(from, key));
    if (has("mode"))
        obs_data_set_int(to, "mode", obs_data_get_int(from, "mode"));
}
} // namespace
std::vector<std::string> preset_names(obs_data_t *settings) {
    std::vector<std::string> names;
    auto *bank = obs_data_get_obj(settings, "saved_presets");
    for (auto *item = obs_data_first(bank); item; obs_data_item_next(&item)) {
        const std::string name = obs_data_item_get_name(item);
        if (obs_data_item_gettype(item) == OBS_DATA_OBJECT && !clean(name).empty() && clean(name) == name)
            names.push_back(name);
        if (names.size() >= 32) {
            obs_data_item_release(&item);
            break;
        }
    }
    obs_data_release(bank);
    std::sort(names.begin(), names.end());
    return names;
}
bool save_preset(obs_data_t *settings, const std::string &raw) {
    const auto name = clean(raw);
    if (name.empty())
        return false;
    auto names = preset_names(settings);
    if (names.size() >= 32 && std::find(names.begin(), names.end(), name) == names.end())
        return false;
    auto *bank = obs_data_get_obj(settings, "saved_presets");
    if (!bank)
        bank = obs_data_create();
    auto *preset = obs_data_create();
    copy_tuning(preset, settings);
    obs_data_set_obj(bank, name.c_str(), preset);
    obs_data_set_obj(settings, "saved_presets", bank);
    obs_data_set_string(settings, "preset_name", name.c_str());
    obs_data_release(preset);
    obs_data_release(bank);
    return true;
}
bool load_preset(obs_data_t *settings, const std::string &raw) {
    const auto name = clean(raw);
    if (name.empty())
        return false;
    auto *bank = obs_data_get_obj(settings, "saved_presets");
    auto *preset = obs_data_get_obj(bank, name.c_str());
    const bool found = preset != nullptr;
    if (found) {
        copy_tuning(settings, preset);
        obs_data_set_string(settings, "preset_name", name.c_str());
    }
    obs_data_release(preset);
    obs_data_release(bank);
    return found;
}
bool delete_preset(obs_data_t *settings, const std::string &raw) {
    const auto name = clean(raw);
    if (name.empty())
        return false;
    auto *bank = obs_data_get_obj(settings, "saved_presets");
    auto *preset = obs_data_get_obj(bank, name.c_str());
    const bool found = preset != nullptr;
    if (found) {
        obs_data_erase(bank, name.c_str());
        obs_data_set_obj(settings, "saved_presets", bank);
    }
    obs_data_release(preset);
    obs_data_release(bank);
    return found;
}
std::string tuning_signature(obs_data_t *settings) {
    auto *tuning = obs_data_create();
    copy_tuning(tuning, settings);
    obs_data_set_string(tuning, "gaming_pc", obs_data_get_string(settings, "gaming_pc"));
    obs_data_set_int(tuning, "port", obs_data_get_int(settings, "port"));
    std::string result = obs_data_get_json(tuning);
    obs_data_release(tuning);
    return result;
}
std::string pipeline_signature(obs_data_t *settings) {
    auto *tuning = obs_data_create();
    copy_tuning(tuning, settings);
    if (obs_data_get_int(settings, "mode") == 1)
        for (const char *key : {"horizontal_fov", "vertical_fov", "automatic_fov", "delay_ms",
                                "horizon_level", "horizon_max_degrees"})
            obs_data_erase(tuning, key);
    std::string result = obs_data_get_json(tuning);
    obs_data_release(tuning);
    return result;
}
} // namespace vrst_plugin
