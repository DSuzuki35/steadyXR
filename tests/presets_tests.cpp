#include "presets.hpp"
#include <cstdio>
#include <cstring>
int main() {
    int errors = 0;
    auto check = [&](bool p, const char *n) {
        std::printf("%s %s\n", p ? "PASS" : "FAIL", n);
        errors += !p;
    };
    auto *s = obs_data_create();
    obs_data_set_default_double(s, "smoothing", 40);
    obs_data_set_double(s, "delay_ms", 83);
    obs_data_set_double(s, "horizontal_fov", 106);
    obs_data_set_bool(s, "separate_axes", true);
    obs_data_set_double(s, "smoothing_pitch", 67);
    obs_data_set_int(s, "mode", 2);
    obs_data_set_string(s, "gaming_pc", "192.168.1.23");
    obs_data_set_bool(s, "comparison", true);
    check(vrst_plugin::save_preset(s, "  iRacing / GT3  "), "save named tuning with trimmed name");
    auto names = vrst_plugin::preset_names(s);
    check(names.size() == 1 && names[0] == "iRacing / GT3", "list exact game/car name");
    obs_data_set_double(s, "smoothing", 3);
    obs_data_set_double(s, "delay_ms", 9);
    obs_data_set_double(s, "smoothing_pitch", 1);
    obs_data_set_int(s, "mode", 0);
    obs_data_set_string(s, "gaming_pc", "192.168.1.77");
    obs_data_set_bool(s, "comparison", false);
    check(vrst_plugin::load_preset(s, "iRacing / GT3"), "load saved preset");
    check(obs_data_get_double(s, "smoothing") == 40 && obs_data_get_double(s, "delay_ms") == 83 &&
              obs_data_get_double(s, "smoothing_pitch") == 67 && obs_data_get_bool(s, "separate_axes") &&
              obs_data_get_int(s, "mode") == 2,
          "round trip default values and advanced controls");
    check(std::strcmp(obs_data_get_string(s, "gaming_pc"), "192.168.1.77") == 0 &&
              !obs_data_get_bool(s, "comparison"),
          "preset preserves connection and A/B state");
    auto *copy = obs_data_create_from_json(obs_data_get_json(s));
    check(vrst_plugin::preset_names(copy).size() == 1 && vrst_plugin::load_preset(copy, "iRacing / GT3"),
          "presets survive OBS scene JSON serialization");
    check(!vrst_plugin::save_preset(s, "   ") && !vrst_plugin::save_preset(s, std::string(65, 'x')) &&
              !vrst_plugin::load_preset(s, "missing"),
          "reject empty oversized and absent names");
    check(vrst_plugin::save_preset(s, "iRacing / GT3") && vrst_plugin::preset_names(s).size() == 1,
          "overwrite explicit same name without duplicate");
    check(vrst_plugin::delete_preset(s, "iRacing / GT3") && vrst_plugin::preset_names(s).empty(),
          "delete selected preset");
    obs_data_release(copy);
    obs_data_release(s);
    return errors ? 1 : 0;
}
