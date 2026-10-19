#pragma once
#include <vrst/spectator.hpp>
#include <windows.h>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string>

namespace vrst::preview {
namespace optional_settings_io {
inline double read_number(const std::wstring &ini, const wchar_t *key, double fallback) {
    wchar_t text[96]{};
    const auto initial = std::to_wstring(fallback);
    GetPrivateProfileStringW(L"Preview", key, initial.c_str(), text, 96, ini.c_str());
    wchar_t *end{};
    const double value = std::wcstod(text, &end);
    if (end == text || !std::isfinite(value)) return fallback;
    while (*end && std::iswspace(*end)) ++end;
    return *end ? fallback : value;
}
inline bool write_number(const std::wstring &ini, const wchar_t *key, double value) {
    wchar_t text[64]{};
    std::swprintf(text, 64, L"%.9g", value);
    return WritePrivateProfileStringW(L"Preview", key, text, ini.c_str()) != FALSE;
}
}
struct SpectatorBoolField { const wchar_t *key, *label; bool SpectatorSettings::*member; };
struct SpectatorNumberField {
    const wchar_t *key, *label;
    double SpectatorSettings::*member;
    double minimum, maximum;
};
inline constexpr SpectatorBoolField spectator_bool_fields[]{
    {L"SpectatorBypass", L"Bypass all spectator camera adjustments", &SpectatorSettings::bypass},
    {L"SpectatorGazeEnabled", L"Enable gaze framing", &SpectatorSettings::gaze_enabled},
    {L"SpectatorChatEnabled", L"Exclude learned chat zone from gaze framing", &SpectatorSettings::chat_enabled},
    {L"SpectatorNaturalEnabled", L"Enable natural camera smoothing", &SpectatorSettings::natural_enabled},
};
inline constexpr SpectatorNumberField spectator_number_fields[]{
    {L"SpectatorGazeStrength", L"Gaze strength (%)", &SpectatorSettings::gaze_strength, 0, 100},
    {L"SpectatorGazeDwellMs", L"Fixation dwell (ms)", &SpectatorSettings::gaze_dwell_ms, 0, 2000},
    {L"SpectatorGazeNeutralDegrees", L"Neutral zone (degrees)", &SpectatorSettings::gaze_neutral_degrees, 0, 20},
    {L"SpectatorGazeHorizontalDegrees", L"Horizontal limit (degrees)", &SpectatorSettings::gaze_horizontal_degrees, 0, 30},
    {L"SpectatorGazeVerticalDegrees", L"Vertical limit (degrees)", &SpectatorSettings::gaze_vertical_degrees, 0, 20},
    {L"SpectatorGazeResponseMs", L"Gaze response (ms)", &SpectatorSettings::gaze_response_ms, 30, 2000},
    {L"SpectatorChatWidthDegrees", L"Chat width (degrees)", &SpectatorSettings::chat_width_degrees, 1, 120},
    {L"SpectatorChatHeightDegrees", L"Chat height (degrees)", &SpectatorSettings::chat_height_degrees, 1, 90},
    {L"SpectatorNaturalSuppression", L"Tremor suppression (%)", &SpectatorSettings::natural_suppression, 0, 100},
    {L"SpectatorNaturalFollow", L"Intentional follow (%)", &SpectatorSettings::natural_follow, 0, 100},
    {L"SpectatorNaturalSettlingMs", L"Settling (ms)", &SpectatorSettings::natural_settling_ms, 30, 1000},
    {L"SpectatorNaturalMaxCorrectionDegrees", L"Maximum correction (degrees)", &SpectatorSettings::natural_max_correction_degrees, 0, 45},
    {L"SpectatorNaturalYaw", L"Yaw influence (%)", &SpectatorSettings::natural_yaw, 0, 100},
    {L"SpectatorNaturalPitch", L"Pitch influence (%)", &SpectatorSettings::natural_pitch, 0, 100},
    {L"SpectatorNaturalRoll", L"Roll influence (%)", &SpectatorSettings::natural_roll, 0, 100},
};
inline SpectatorSettings read_spectator_settings(const std::wstring &ini) {
    SpectatorSettings s;
    for (const auto &f : spectator_bool_fields)
        s.*f.member = optional_settings_io::read_number(ini, f.key, 0) == 1;
    for (const auto &f : spectator_number_fields)
        s.*f.member = optional_settings_io::read_number(ini, f.key, s.*f.member);
    return sanitize_spectator_settings(s);
}
inline bool write_spectator_settings(const std::wstring &ini, SpectatorSettings s) {
    if (ini.empty()) return false;
    s = sanitize_spectator_settings(s);
    bool okay = true;
    for (const auto &f : spectator_bool_fields)
        okay = optional_settings_io::write_number(ini, f.key, s.*f.member ? 1 : 0) && okay;
    for (const auto &f : spectator_number_fields)
        okay = optional_settings_io::write_number(ini, f.key, s.*f.member) && okay;
    return okay;
}
} // namespace vrst::preview
