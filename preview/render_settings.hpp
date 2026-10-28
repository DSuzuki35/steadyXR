#pragma once
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string>
#include <windows.h>

namespace vrst::preview {
// Percentages of each eye's original horizontal tangent-space width.
struct EyeExpansion {
    double left{}, right{};
};
struct RenderSettings {
    bool enabled{};
    EyeExpansion eyes[2]{};
    // Viewer-only editing preference. Older capture DLLs use the four existing amounts.
    bool symmetric{};
};
namespace render_settings_detail {
inline double percent(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 30.0) : 0.0;
}
inline double read_percent(const std::wstring &ini, const wchar_t *key) {
    wchar_t text[64]{};
    GetPrivateProfileStringW(L"Render", key, L"0", text, 64, ini.c_str());
    wchar_t *end{};
    double value = std::wcstod(text, &end);
    if (end == text)
        return 0;
    while (*end && std::iswspace(*end))
        ++end;
    return *end ? 0.0 : percent(value / 10.0);
}
inline constexpr const wchar_t *keys[2][2]{{L"LeftEyeLeftTenths", L"LeftEyeRightTenths"},
                                           {L"RightEyeLeftTenths", L"RightEyeRightTenths"}};
} // namespace render_settings_detail
inline RenderSettings read_render_settings(const std::wstring &ini) {
    RenderSettings settings{};
    if (ini.empty())
        return settings;
    settings.enabled = GetPrivateProfileIntW(L"Render", L"Enabled", 0, ini.c_str()) != 0;
    settings.symmetric = GetPrivateProfileIntW(L"Render", L"Symmetric", 0, ini.c_str()) != 0;
    for (unsigned eye = 0; eye < 2; ++eye) {
        settings.eyes[eye].left =
            render_settings_detail::read_percent(ini, render_settings_detail::keys[eye][0]);
        settings.eyes[eye].right =
            render_settings_detail::read_percent(ini, render_settings_detail::keys[eye][1]);
    }
    return settings;
}
inline bool write_render_settings(const std::wstring &ini, const RenderSettings &settings) {
    if (ini.empty())
        return false;
    bool ok = true;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const double values[]{settings.eyes[eye].left, settings.eyes[eye].right};
        for (unsigned side = 0; side < 2; ++side) {
            const auto value =
                std::to_wstring(std::lround(render_settings_detail::percent(values[side]) * 10.0));
            if (!WritePrivateProfileStringW(L"Render", render_settings_detail::keys[eye][side], value.c_str(),
                                            ini.c_str()))
                ok = false;
        }
    }
    if (!WritePrivateProfileStringW(L"Render", L"Enabled", settings.enabled ? L"1" : L"0", ini.c_str()))
        ok = false;
    if (!WritePrivateProfileStringW(L"Render", L"Symmetric", settings.symmetric ? L"1" : L"0", ini.c_str()))
        ok = false;
    return ok;
}
} // namespace vrst::preview
