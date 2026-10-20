#include "../preview/overlay_settings.hpp"
#include "../preview/visual_settings_io.hpp"
#include "../preview/director_settings_io.hpp"
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
        check(c.options.mirror_stretch == 0, "edge stretch defaults to existing mirror behavior");
        check(c.options.overlays.mode == 0, "overlay capture defaults to None");
        check(!c.options.director.lookahead_enabled && !c.options.director.auto_crop_enabled &&
                  !c.options.director.speed_fov_enabled && c.options.director.telemetry_game == 0,
              "director defaults to off with automatic telemetry selection");
        check(!c.options.spectator.gaze_enabled && !c.options.spectator.natural_enabled &&
                  !c.options.visual.active() && !c.options.compare_effects,
              "legacy OBS source retains disabled spectator and visual features");
        {
            using namespace vrst::preview;
            auto varied = c;
            for (const auto &f : spectator_bool_fields)
                varied.options.spectator.*f.member = true;
            for (const auto &f : spectator_number_fields)
                varied.options.spectator.*f.member = f.minimum + (f.maximum - f.minimum) * .37;
            for (const auto &f : director_bool_fields)
                varied.options.director.*f.member = true;
            for (const auto &f : director_number_fields)
                varied.options.director.*f.member = f.maximum;
            varied.options.director.telemetry_game = 2;
            for (const auto &f : visual_bool_fields)
                varied.options.visual.*f.member = true;
            for (const auto &f : visual_number_fields)
                varied.options.visual.*f.member = float(f.minimum + (f.maximum - f.minimum) * .37);
            varied.options.visual.metering_region = {.1f, .2f, .8f, .9f};
            varied.options.visual.input_transfer = VisualTransfer::linear;
            varied.options.visual.premultiplied_alpha = true;
            varied.options.compare_effects = true;
            auto *stored = obs_data_create();
            vrst_plugin::openxr_store(stored, varied);
            auto *scene = obs_data_create_from_json(obs_data_get_json(stored));
            const auto restored = vrst_plugin::openxr_settings(scene);
            auto *properties = obs_properties_create();
            vrst_plugin::openxr_add_properties(properties);
            auto key = [](const wchar_t *v) {
                std::string text;
                for (; *v; ++v)
                    text.push_back(char(*v));
                return text;
            };
            for (const auto &f : spectator_bool_fields)
                check(restored.options.spectator.*f.member &&
                          obs_properties_get(properties, key(f.key).c_str()),
                      "spectator toggle survives scene JSON and has a native property");
            for (const auto &f : spectator_number_fields)
                check(std::abs(restored.options.spectator.*f.member - varied.options.spectator.*f.member) <
                              1e-5 &&
                          obs_properties_get(properties, key(f.key).c_str()),
                      "spectator tuning roundtrips");
            for (const auto &f : director_bool_fields)
                check(restored.options.director.*f.member &&
                          obs_properties_get(properties, key(f.key).c_str()),
                      "director toggle survives scene JSON and has a native property");
            for (const auto &f : director_number_fields)
                check(std::abs(restored.options.director.*f.member - varied.options.director.*f.member) <
                              1e-5 &&
                          obs_properties_get(properties, key(f.key).c_str()),
                      "director tuning survives scene JSON and has a native property");
            auto *telemetry = obs_properties_get(properties, "DirectorTelemetryGame");
            check(restored.options.director.telemetry_game == 2 && telemetry &&
                      obs_property_list_item_count(telemetry) == 3,
                  "director telemetry selection persists and exposes all supported games");
            for (const auto &f : visual_bool_fields)
                check(restored.options.visual.*f.member && obs_properties_get(properties, key(f.key).c_str()),
                      "visual toggle survives scene JSON and has a native property");
            for (const auto &f : visual_number_fields)
                check(std::abs(restored.options.visual.*f.member - varied.options.visual.*f.member) < 1e-5 &&
                          obs_properties_get(properties, key(f.key).c_str()),
                      "visual tuning roundtrips");
            for (const auto &f : visual_region_fields)
                check(restored.options.visual.metering_region.*f.member ==
                              varied.options.visual.metering_region.*f.member &&
                          obs_properties_get(properties, key(f.key).c_str()),
                      "visual metering region roundtrips");
            check(restored.options.compare_effects &&
                      restored.options.visual.input_transfer == VisualTransfer::linear &&
                      restored.options.visual.premultiplied_alpha,
                  "comparison scope and transfer persist");
            const auto preset = (dir / "spectator-visual.ini").wstring();
            check(write_spectator_settings(preset, varied.options.spectator) &&
                      write_director_settings(preset, varied.options.director) &&
                      write_visual_settings(preset, varied.options.visual) &&
                      WritePrivateProfileStringW(L"Preview", L"CompareEffects", L"1", preset.c_str()),
                  "shared complete optional preset writes");
            std::string error;
            check(vrst_plugin::openxr_import(scene, preset, error), "complete optional preset imports");
            const auto imported = vrst_plugin::openxr_settings(scene);
            for (const auto &f : director_bool_fields)
                check(imported.options.director.*f.member, "shared director preset toggle imports");
            for (const auto &f : director_number_fields)
                check(std::abs(imported.options.director.*f.member - varied.options.director.*f.member) <
                          1e-5,
                      "shared director preset tuning imports");
            check(imported.options.director.telemetry_game == 2, "shared director telemetry imports");
            obs_data_set_double(scene, "DirectorLookaheadFrames", NAN);
            obs_data_set_double(scene, "DirectorAutoCropMaxPercent", 999);
            obs_data_set_double(scene, "DirectorSpeedFovPercent", -1);
            obs_data_set_int(scene, "DirectorTelemetryGame", 99);
            const auto invalid_director = vrst_plugin::openxr_settings(scene).options.director;
            check(invalid_director.lookahead_frames == 3 && invalid_director.auto_crop_max_percent == 30 &&
                      invalid_director.speed_fov_percent == 0 && invalid_director.telemetry_game == 0,
                  "invalid scene director values fall back or clamp safely");
            for (const auto &f : spectator_bool_fields)
                check(imported.options.spectator.*f.member, "shared spectator preset toggle imports");
            for (const auto &f : spectator_number_fields)
                check(std::abs(imported.options.spectator.*f.member - varied.options.spectator.*f.member) <
                          1e-5,
                      "shared spectator preset tuning imports");
            for (const auto &f : visual_number_fields)
                check(std::abs(imported.options.visual.*f.member - varied.options.visual.*f.member) < 1e-5,
                      "shared visual preset tuning imports");
            for (const auto &f : visual_bool_fields)
                check(imported.options.visual.*f.member, "shared visual preset toggle imports");
            for (const auto &f : visual_region_fields)
                check(std::abs(imported.options.visual.metering_region.*f.member -
                               varied.options.visual.metering_region.*f.member) < 1e-6,
                      "shared visual preset region imports");
            check(imported.options.compare_effects && imported.options.visual.premultiplied_alpha &&
                      imported.options.visual.input_transfer == VisualTransfer::linear,
                  "shared comparison scope and alpha import");
            obs_properties_destroy(properties);
            obs_data_release(scene);
            obs_data_release(stored);
        }
        c.options.overlays = {2, 0x8005, 1234, 18446744073709551000ull};
        vrst_plugin::openxr_store(s, c);
        auto restored_overlay = vrst_plugin::openxr_settings(s).options.overlays;
        check(restored_overlay.mode == 2 && restored_overlay.mask == 0x8005 &&
                  restored_overlay.producer_pid == 1234 &&
                  restored_overlay.session == 18446744073709551000ull,
              "OBS overlay selection preserves 64-bit session without floating-point loss");
        const auto overlay_ini = dir / "overlay-selection.ini";
        check(vrst::preview::write_overlay_settings(overlay_ini.wstring(), restored_overlay),
              "writes overlay INI selection");
        std::string overlay_error;
        check(vrst_plugin::openxr_import(s, overlay_ini.wstring(), overlay_error),
              "imports overlay selection INI");
        restored_overlay = vrst_plugin::openxr_settings(s).options.overlays;
        check(restored_overlay.mode == 2 && restored_overlay.mask == 0x8005 &&
                  restored_overlay.producer_pid == 1234 &&
                  restored_overlay.session == 18446744073709551000ull,
              "shared INI and OBS import preserve the complete selection snapshot");
        vrst::preview::OverlayCatalog scope{};
        scope.producer_pid = 1234;
        scope.session = restored_overlay.session;
        check(vrst::preview::overlay_scope_matches(restored_overlay, scope),
              "saved overlay selection matches its producer session");
        ++scope.session;
        check(!vrst::preview::overlay_scope_matches(restored_overlay, scope),
              "game restart invalidates saved individual selection");
        --scope.session;
        ++scope.producer_pid;
        check(!vrst::preview::overlay_scope_matches(restored_overlay, scope),
              "another producer cannot inherit individual selection");
        vrst_plugin::openxr_defaults(s);
        for (auto invalid :
             {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -5.}) {
            obs_data_set_double(s, "mirror_stretch", invalid);
            check(vrst_plugin::openxr_settings(s).options.mirror_stretch == 0,
                  "invalid/negative edge stretch safely becomes zero");
        }
        obs_data_set_double(s, "mirror_stretch", 150);
        check(vrst_plugin::openxr_settings(s).options.mirror_stretch == 100,
              "edge stretch clamps at 100 percent");
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
        obs_data_set_bool(s, "SpectatorGazeEnabled", true);
        obs_data_set_bool(s, "DirectorLookaheadEnabled", true);
        obs_data_set_bool(s, "DirectorAutoCropEnabled", true);
        obs_data_set_bool(s, "DirectorSpeedFovEnabled", true);
        obs_data_set_int(s, "DirectorTelemetryGame", 2);
        obs_data_set_bool(s, "VisualExposure", true);
        obs_data_set_bool(s, "compare_effects", true);
        std::string error;
        check(vrst_plugin::openxr_import(s, old.wstring(), error), "literal legacy INI imports");
        c = vrst_plugin::openxr_settings(s);
        check(!c.options.spectator.gaze_enabled && !c.options.spectator.natural_enabled &&
                  !c.options.director.lookahead_enabled && !c.options.director.auto_crop_enabled &&
                  !c.options.director.speed_fov_enabled && c.options.director.telemetry_game == 0 &&
                  !c.options.visual.active() && !c.options.compare_effects,
              "legacy preset import resets optional features to off even after enabled scene settings");
        check(c.options.smoothing == 63 && c.options.crop == 14 && c.options.eye == 1 && c.fps == 90 &&
                  !c.options.mirror && c.options.edge_reserve[1].left == 7 && c.options.mirror_stretch == 0,
              "legacy values and half-crop reserve fallback");
        const std::string modern =
            "[Preview]\nSmoothing=99\nSmoothingTenths=427\nCropTenths=123\nCropLeftTenths="
            "24\nCropRightTenths=35\nCropTopTenths=46\nCropBottomTenths=57\nHorizonTenths="
            "611\nAdaptiveHorizon=0\nMirrorLimitTenths=141\nMirrorSoftnessTenths=753\nMirrorStretchTenths="
            "642\nOutputWidth="
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
                  c.options.mirror_stretch == 64.2 && c.options.output_width == 1280 &&
                  c.options.output_height == 720 && c.options.aspect == 4. / 3. &&
                  c.options.vertical_offset == -67.5 && c.options.edge_reserve[0].left == 3.4 &&
                  c.options.edge_reserve[0].bottom == 12.3 && c.options.edge_reserve[1].right == 20,
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
                  obs_properties_get(props, "mirror_softness") &&
                  obs_properties_get(props, "mirror_stretch") &&
                  obs_properties_get(props, "mirror_stretch_help"),
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
        check(restored.options.mirror_stretch == 64.2,
              "edge stretch persists through native OBS settings serialization");
        auto *json = obs_data_create_from_json(obs_data_get_json(roundtrip));
        check(vrst_plugin::openxr_settings(json).options.mirror_stretch == 64.2,
              "edge stretch survives a JSON scene roundtrip");
        obs_data_release(json);
        const auto stretch_bounds = dir / "stretch-bounds.ini";
        for (const auto &value : {std::wstring(L"-15"), std::wstring(L"1500")}) {
            WritePrivateProfileStringW(L"Preview", L"MirrorStretchTenths", value.c_str(),
                                       stretch_bounds.c_str());
            check(vrst_plugin::openxr_import(s, stretch_bounds.wstring(), error) &&
                      vrst_plugin::openxr_settings(s).options.mirror_stretch == (value == L"-15" ? 0 : 100),
                  "imported edge stretch stays within the supported range");
        }
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
