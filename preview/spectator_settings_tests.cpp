#include <iostream>
#include <cmath>
#include <windows.h>
#if __has_include("spectator_settings.hpp") && __has_include("visual_settings_io.hpp") && __has_include("director_settings_io.hpp")
#include "spectator_settings.hpp"
#include "visual_settings_io.hpp"
#include "director_settings_io.hpp"
#include <stdexcept>
using namespace vrst::preview;
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
int main() {
    wchar_t temp[MAX_PATH]{}, name[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"sxr", 0, name);
    const std::wstring ini(name);
    try {
        auto camera = read_spectator_settings(ini);
        auto visual = read_visual_settings(ini);
        auto director = read_director_settings(ini);
        check(!director.lookahead_enabled && !director.auto_crop_enabled && !director.speed_fov_enabled,
              "legacy settings leave all director features off");
        check(!camera.gaze_enabled && !camera.chat_enabled && !camera.natural_enabled && !visual.active(), "missing keys must leave all effects off");
        WritePrivateProfileStringW(L"Preview", L"FutureSetting", L"retained", name);
        for (const auto &field : spectator_bool_fields) camera.*field.member = true;
        for (const auto &field : spectator_number_fields) camera.*field.member = field.maximum;
        check(write_spectator_settings(ini, camera), "write camera");
        auto restored = read_spectator_settings(ini);
        for (const auto &field : spectator_bool_fields) check(restored.*field.member, "camera boolean roundtrip");
        for (const auto &field : spectator_number_fields) check(std::abs(restored.*field.member - camera.*field.member) < .00001, "camera tuning roundtrip");
        for (const auto &field : director_bool_fields) director.*field.member = true;
        for (const auto &field : director_number_fields) director.*field.member = field.maximum;
        director.telemetry_game = 2;
        check(write_director_settings(ini, director), "write director");
        const auto restored_director = read_director_settings(ini);
        for (const auto &field : director_bool_fields)
            check(restored_director.*field.member, "director boolean roundtrip");
        for (const auto &field : director_number_fields)
            check(std::abs(restored_director.*field.member - director.*field.member) < .00001,
                  "director tuning roundtrip");
        check(restored_director.telemetry_game == 2, "director game roundtrip");
        for (const auto &field : visual_bool_fields) visual.*field.member = true;
        for (const auto &field : visual_number_fields) visual.*field.member = float(field.maximum);
        visual.metering_region = {.2f, .3f, .8f, .9f};
        visual.content_region = {.1f, .1f, .4f, .4f};
        visual.input_transfer = VisualTransfer::linear;
        visual.premultiplied_alpha = true;
        check(write_visual_settings(ini, visual), "write visual");
        const auto vr = read_visual_settings(ini);
        for (const auto &field : visual_bool_fields) check(vr.*field.member, "visual boolean roundtrip");
        for (const auto &field : visual_number_fields) check(std::abs(vr.*field.member - visual.*field.member) < .00001, "visual tuning roundtrip");
        check(std::abs(vr.metering_region.left - .2f) < .00001 && vr.metering_region.bottom == .9f, "metering roundtrip");
        check(vr.content_region.left == 0 && vr.content_region.right == 1, "content crop is transient");
        check(vr.input_transfer == VisualTransfer::linear && vr.premultiplied_alpha, "transfer roundtrip");
        wchar_t unknown[32]{};
        GetPrivateProfileStringW(L"Preview", L"FutureSetting", L"", unknown, 32, name);
        check(std::wstring(unknown) == L"retained", "preserve future keys");
        WritePrivateProfileStringW(L"Preview", L"SpectatorGazeStrength", L"nan", name);
        WritePrivateProfileStringW(L"Preview", L"SpectatorGazeDwellMs", L"123junk", name);
        WritePrivateProfileStringW(L"Preview", L"VisualExposureEv", L"-999", name);
        camera = read_spectator_settings(ini);
        check(camera.gaze_strength == 25 && camera.gaze_dwell_ms == 200, "invalid values use neutral defaults");
        check(read_visual_settings(ini).exposure_ev == -6, "finite values bounded");
        WritePrivateProfileStringW(L"Preview", L"DirectorLookaheadFrames", L"nan", name);
        WritePrivateProfileStringW(L"Preview", L"DirectorAutoCropReleaseMs", L"123junk", name);
        WritePrivateProfileStringW(L"Preview", L"DirectorSpeedFovPercent", L"-999", name);
        WritePrivateProfileStringW(L"Preview", L"DirectorTelemetryGame", L"99", name);
        WritePrivateProfileStringW(L"Preview", L"DirectorLookaheadEnabled", L"true", name);
        director = read_director_settings(ini);
        check(director.lookahead_frames == 3 && director.auto_crop_release_ms == 1200 &&
                  director.speed_fov_percent == 0 && director.telemetry_game == 0 &&
                  !director.lookahead_enabled,
              "invalid director values use defaults; finite tuning is bounded");
        check(!write_director_settings(L"", director), "empty director destination fails");
        DeleteFileW(name);
        std::cout << "spectator/visual/director settings passed\n";
        return 0;
    } catch (const std::exception &e) { DeleteFileW(name); std::cerr << e.what() << '\n'; return 1; }
}
#else
int main() { std::cerr << "FAIL: spectator/visual/director settings helpers unavailable\n"; return 1; }
#endif
