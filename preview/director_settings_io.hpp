#pragma once
#include "director.hpp"
#include "spectator_settings.hpp"

namespace vrst::preview {
struct DirectorBoolField {
    const wchar_t *key, *label;
    bool DirectorSettings::*member;
};
struct DirectorNumberField {
    const wchar_t *key, *label;
    double DirectorSettings::*member;
    double minimum, maximum;
};
inline constexpr DirectorBoolField director_bool_fields[]{
    {L"DirectorLookaheadEnabled", L"Enable lookahead", &DirectorSettings::lookahead_enabled},
    {L"DirectorAutoCropEnabled", L"Enable automatic crop", &DirectorSettings::auto_crop_enabled},
    {L"DirectorSpeedFovEnabled", L"Enable speed framing", &DirectorSettings::speed_fov_enabled},
};
inline constexpr DirectorNumberField director_number_fields[]{
    {L"DirectorLookaheadFrames", L"Lookahead delay (frames)", &DirectorSettings::lookahead_frames, 1, 4},
    {L"DirectorAutoCropMaxPercent", L"Automatic crop maximum (%)", &DirectorSettings::auto_crop_max_percent, 0, 30},
    {L"DirectorAutoCropReleaseMs", L"Automatic crop release (ms)", &DirectorSettings::auto_crop_release_ms, 100, 5000},
    {L"DirectorSpeedFovPercent", L"Low-speed crop maximum (%)", &DirectorSettings::speed_fov_percent, 0, 15},
    {L"DirectorSpeedFovFullKph", L"Normal framing speed (km/h)", &DirectorSettings::speed_fov_full_kph, 30, 400},
    {L"DirectorSpeedFovResponseMs", L"Speed framing response (ms)", &DirectorSettings::speed_fov_response_ms, 100, 5000},
};
inline constexpr const wchar_t *director_telemetry_labels[]{L"Auto", L"iRacing", L"Le Mans Ultimate"};
inline unsigned director_telemetry_selection(double value) {
    return value == 1 ? 1u : value == 2 ? 2u : 0u;
}
inline DirectorSettings read_director_settings(const std::wstring &ini) {
    DirectorSettings s;
    for (const auto &f : director_bool_fields)
        s.*f.member = optional_settings_io::read_number(ini, f.key, 0) == 1;
    for (const auto &f : director_number_fields)
        s.*f.member = optional_settings_io::read_number(ini, f.key, s.*f.member);
    s.telemetry_game = director_telemetry_selection(
        optional_settings_io::read_number(ini, L"DirectorTelemetryGame", 0));
    return sanitize_director_settings(s);
}
inline bool write_director_settings(const std::wstring &ini, DirectorSettings s) {
    if (ini.empty())
        return false;
    s = sanitize_director_settings(s);
    bool okay = true;
    for (const auto &f : director_bool_fields)
        okay = optional_settings_io::write_number(ini, f.key, s.*f.member ? 1 : 0) && okay;
    for (const auto &f : director_number_fields)
        okay = optional_settings_io::write_number(ini, f.key, s.*f.member) && okay;
    return optional_settings_io::write_number(ini, L"DirectorTelemetryGame", s.telemetry_game) && okay;
}
} // namespace vrst::preview
