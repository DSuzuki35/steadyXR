#pragma once
#include "spectator_settings.hpp"
#include "visual_settings.hpp"

namespace vrst::preview {
struct VisualBoolField { const wchar_t *key, *label; bool VisualSettings::*member; };
struct VisualNumberField { const wchar_t *key, *label; float VisualSettings::*member; double minimum, maximum; };
inline constexpr VisualBoolField visual_bool_fields[]{
    {L"VisualBypass", L"Bypass all visual effects", &VisualSettings::bypass},
    {L"VisualExposure", L"Enable manual exposure", &VisualSettings::exposure},
    {L"VisualAdaptation", L"Enable luminance adaptation", &VisualSettings::adaptation},
    {L"VisualBloom", L"Enable bloom", &VisualSettings::bloom},
    {L"VisualColor", L"Enable color grading", &VisualSettings::color},
};
inline constexpr VisualNumberField visual_number_fields[]{
    {L"VisualExposureEv", L"Exposure (EV)", &VisualSettings::exposure_ev, -6, 6},
    {L"VisualMinGain", L"Adaptation minimum gain", &VisualSettings::min_gain, .0625, 16},
    {L"VisualMaxGain", L"Adaptation maximum gain", &VisualSettings::max_gain, .0625, 16},
    {L"VisualTargetLuminance", L"Target luminance", &VisualSettings::target_luminance, .01, .8},
    {L"VisualBrightenRate", L"Brighten rate (/s)", &VisualSettings::brighten_rate, .01, 20},
    {L"VisualDarkenRate", L"Darken rate (/s)", &VisualSettings::darken_rate, .01, 20},
    {L"VisualTransitionStrength", L"Transition damping", &VisualSettings::transition_strength, 0, .3},
    {L"VisualTransitionRecovery", L"Transition recovery (/s)", &VisualSettings::transition_recovery, .1, 20},
    {L"VisualBloomThreshold", L"Bloom threshold", &VisualSettings::bloom_threshold, 0, 4},
    {L"VisualBloomKnee", L"Bloom soft knee", &VisualSettings::bloom_knee, 0, 1},
    {L"VisualBloomStrength", L"Bloom strength", &VisualSettings::bloom_strength, 0, 1},
    {L"VisualBloomRadius", L"Bloom radius (pixels)", &VisualSettings::bloom_radius, 0, 64},
    {L"VisualTemperature", L"Temperature", &VisualSettings::temperature, -1, 1},
    {L"VisualTint", L"Tint", &VisualSettings::tint, -1, 1},
    {L"VisualContrast", L"Contrast", &VisualSettings::contrast, 0, 2},
    {L"VisualSaturation", L"Saturation", &VisualSettings::saturation, 0, 2},
    {L"VisualShadows", L"Shadows", &VisualSettings::shadows, -1, 1},
    {L"VisualHighlights", L"Highlights", &VisualSettings::highlights, -1, 1},
};
struct VisualRegionField { const wchar_t *key, *label; float VisualRegion::*member; };
inline constexpr VisualRegionField visual_region_fields[]{
    {L"VisualMeteringLeft", L"Metering left (0-1)", &VisualRegion::left},
    {L"VisualMeteringTop", L"Metering top (0-1)", &VisualRegion::top},
    {L"VisualMeteringRight", L"Metering right (0-1)", &VisualRegion::right},
    {L"VisualMeteringBottom", L"Metering bottom (0-1)", &VisualRegion::bottom},
};
inline VisualSettings read_visual_settings(const std::wstring &ini) {
    VisualSettings s;
    for (const auto &f : visual_bool_fields) s.*f.member = optional_settings_io::read_number(ini, f.key, 0) == 1;
    for (const auto &f : visual_number_fields) s.*f.member = float(optional_settings_io::read_number(ini, f.key, s.*f.member));
    for (const auto &f : visual_region_fields) s.metering_region.*f.member = float(optional_settings_io::read_number(ini, f.key, s.metering_region.*f.member));
    const auto transfer = optional_settings_io::read_number(ini, L"VisualInputTransfer", 0);
    s.input_transfer = transfer == 1 ? VisualTransfer::linear : transfer == 2 ? VisualTransfer::unsupported : VisualTransfer::srgb;
    s.premultiplied_alpha = optional_settings_io::read_number(ini, L"VisualPremultipliedAlpha", 0) == 1;
    return s.bounded();
}
inline bool write_visual_settings(const std::wstring &ini, VisualSettings s) {
    if (ini.empty()) return false;
    s = s.bounded();
    bool okay = true;
    for (const auto &f : visual_bool_fields) okay = optional_settings_io::write_number(ini, f.key, s.*f.member ? 1 : 0) && okay;
    for (const auto &f : visual_number_fields) okay = optional_settings_io::write_number(ini, f.key, s.*f.member) && okay;
    for (const auto &f : visual_region_fields) okay = optional_settings_io::write_number(ini, f.key, s.metering_region.*f.member) && okay;
    okay = optional_settings_io::write_number(ini, L"VisualInputTransfer", int(s.input_transfer)) && okay;
    return optional_settings_io::write_number(ini, L"VisualPremultipliedAlpha", s.premultiplied_alpha ? 1 : 0) && okay;
}
} // namespace vrst::preview
