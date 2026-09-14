#include "openxr_source_settings.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
    std::cout << "PASS: " << message << '\n';
}
int main() {
    auto *s = obs_data_create();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("steadyxr-obs-settings-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    try {
        vrst_plugin::openxr_defaults(s);
        auto c = vrst_plugin::openxr_settings(s);
        check(c.options.output_width == 3840 && c.options.output_height == 2160 && c.fps == 60 && !c.paused &&
                  c.options.stabilize && c.options.smoothing == 40 && c.options.crop == 10 &&
                  c.options.mirror && c.options.eye == 0 && c.options.aspect == 16. / 9.,
              "native source defaults");
        obs_data_set_double(s, "smoothing", std::numeric_limits<double>::quiet_NaN());
        obs_data_set_double(s, "crop", -23);
        obs_data_set_double(s, "mirror_limit", 999);
        obs_data_set_double(s, "vertical_offset", std::numeric_limits<double>::infinity());
        obs_data_set_int(s, "output_width", -1);
        obs_data_set_int(s, "output_height", 10000);
        obs_data_set_double(s, "aspect", 0);
        obs_data_set_int(s, "fps", 47);
        c = vrst_plugin::openxr_settings(s);
        check(c.options.smoothing == 40 && c.options.crop == 0 && c.options.mirror_limit == 50 &&
                  c.options.vertical_offset == 0 && c.options.output_width == 320 &&
                  c.options.output_height == 4320 && c.options.aspect == .25 && c.fps == 60,
              "scene JSON values are finite and clamped");
        obs_data_set_int(s, "eye", 1);
        obs_data_set_bool(s, "right_reserve_enabled", true);
        obs_data_set_double(s, "right_reserve_left", 3.4);
        obs_data_set_double(s, "right_reserve_right", 92);
        c = vrst_plugin::openxr_settings(s);
        check(c.options.eye == 1 && c.options.edge_reserve[1].enabled &&
                  c.options.edge_reserve[1].left == 3.4 && c.options.edge_reserve[1].right == 35 &&
                  !c.options.edge_reserve[0].enabled,
              "selected eye retains independent sanitized reserves");
        const std::string legacy = "[Preview]\r\nSmoothing=63\r\nCrop=14\r\nEye=1\r\nFPS=90\r\nMirror=0\r\n["
                                   "Render]\r\nEnabled=1\r\nLeft=22\r\n";
        const auto old = dir / "legacy.ini";
        {
            std::ofstream f(old, std::ios::binary);
            f << legacy;
        }
        obs_data_set_bool(s, "render_enabled", true);
        obs_data_set_double(s, "render_left", 17);
        std::string error;
        check(vrst_plugin::openxr_import(s, old.wstring(), error), "literal legacy INI imports");
        c = vrst_plugin::openxr_settings(s);
        check(c.options.smoothing == 63 && c.options.crop == 14 && c.options.eye == 1 && c.fps == 90 &&
                  !c.options.mirror && c.options.edge_reserve[1].left == 7,
              "legacy values and half-crop reserve fallback");
        const std::string modern =
            "[Preview]\nSmoothing=99\nSmoothingTenths=427\nCropTenths=123\nCropLeftTenths="
            "24\nCropRightTenths=35\nCropTopTenths=46\nCropBottomTenths=57\nHorizonTenths="
            "611\nAdaptiveHorizon=0\nMirrorLimitTenths=141\nMirrorSoftnessTenths=753\nOutputWidth="
            "1280\nOutputHeight=720\nCompareMode=2\nAspectNumerator=4\nAspectDenominator="
            "3\nVerticalOffsetTenths=-675\nEye="
            "1\n[LeftEyeReserve]\nEnabled=1\nLeftTenths=34\nRightTenths=87\nTopTenths=56\nBottomTenths=123\n["
            "RightEyeReserve]\nEnabled=1\nLeftTenths=20\nRightTenths=200\nTopTenths=50\nBottomTenths=50\n["
            "Render]\nEnabled=0\nLeft=29\n";
        const auto current = dir / "modern.ini";
        {
            std::ofstream f(current, std::ios::binary);
            f << modern;
        }
        check(vrst_plugin::openxr_import(s, current.wstring(), error), "literal modern INI imports");
        check(obs_data_get_int(s, "compare_mode") == 2,
              "saved side-by-side comparison mode imports from the shared Preview key");
        c = vrst_plugin::openxr_settings(s);
        check(c.options.smoothing == 42.7 && c.options.crop == 12.3 && c.options.crop_left == 2.4 &&
                  c.options.crop_right == 3.5 && c.options.crop_top == 4.6 && c.options.crop_bottom == 5.7 &&
                  c.options.horizon_lock == 61.1 && !c.options.adaptive_horizon &&
                  c.options.mirror_limit == 14.1 && c.options.mirror_softness == 75.3 &&
                  c.options.output_width == 1280 && c.options.output_height == 720 &&
                  c.options.aspect == 4. / 3. && c.options.vertical_offset == -67.5 &&
                  c.options.edge_reserve[0].left == 3.4 && c.options.edge_reserve[0].bottom == 12.3 &&
                  c.options.edge_reserve[1].right == 20,
              "all modern render options import with exact tenths");
        std::ifstream f(current, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), {});
        check(bytes == modern && obs_data_get_bool(s, "render_enabled") &&
                  obs_data_get_double(s, "render_left") == 17,
              "import is read-only and does not touch render-area settings");
        const auto invalid = dir / "invalid.ini";
        {
            std::ofstream f(invalid);
            f << "[Render]\nEnabled=1\n";
        }
        check(!vrst_plugin::openxr_import(s, invalid.wstring(), error) && !error.empty() &&
                  vrst_plugin::openxr_settings(s).options.smoothing == 42.7,
              "invalid import leaves source settings unchanged");
        auto *props = obs_properties_create();
        vrst_plugin::openxr_add_properties(props);
        check(obs_properties_get(props, "right_reserve_left") && obs_properties_get(props, "output_width") &&
                  obs_properties_get(props, "vertical_offset") &&
                  obs_properties_get(props, "mirror_softness"),
              "native properties expose advanced rendering controls");
        auto *compare = obs_properties_get(props, "compare_mode");
        check(compare && obs_property_list_item_count(compare) == 3,
              "native properties expose original, split and side-by-side comparison modes");
        auto *roundtrip = obs_data_create();
        c.options.compare_mode = 1;
        c.options.compare_original = true;
        vrst_plugin::openxr_store(roundtrip, c);
        const auto restored = vrst_plugin::openxr_settings(roundtrip);
        check(restored.options.compare_mode == 1 && !restored.options.compare_original &&
                  !obs_data_has_user_value(roundtrip, "compare_original"),
              "comparison layout persists while active comparison always remains transient");
        obs_data_set_int(roundtrip, "compare_mode", 99);
        check(vrst_plugin::openxr_settings(roundtrip).options.compare_mode == 2,
              "unknown comparison mode is bounded before rendering");
        obs_data_release(roundtrip);
        obs_properties_destroy(props);
        obs_data_release(s);
        // Deliberately retain the tiny isolated fixtures for review.
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        obs_data_release(s);
        return 1;
    }
}
