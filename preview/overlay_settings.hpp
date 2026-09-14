#pragma once
#include "transport.hpp"
#include <algorithm>
#include <cwchar>

namespace vrst::preview {
inline std::uint64_t overlay_ini_number(const std::wstring &path, const wchar_t *key) {
    wchar_t text[32]{};
    GetPrivateProfileStringW(L"Preview", key, L"0", text, 32, path.c_str());
    wchar_t *end{};
    if (text[0] < L'0' || text[0] > L'9')
        return 0;
    const auto value = std::wcstoull(text, &end, 10);
    return end && !*end ? value : 0;
}
inline OverlaySelection read_overlay_settings(const std::wstring &path) {
    OverlaySelection s;
    const auto mode = overlay_ini_number(path, L"OverlayMode");
    s.mode = mode <= 2 ? unsigned(mode) : 0;
    s.mask = unsigned(overlay_ini_number(path, L"OverlayMask")) & 0xffff;
    s.producer_pid = unsigned(overlay_ini_number(path, L"OverlayProducerPID"));
    s.session = overlay_ini_number(path, L"OverlaySession");
    return s;
}
inline bool write_overlay_settings(const std::wstring &path, const OverlaySelection &s) {
    bool okay = true;
    auto write = [&](const wchar_t *key, std::uint64_t v) {
        okay = WritePrivateProfileStringW(L"Preview", key, std::to_wstring(v).c_str(), path.c_str()) && okay;
    };
    write(L"OverlayMode", s.mode);
    write(L"OverlayMask", s.mask);
    write(L"OverlayProducerPID", s.producer_pid);
    write(L"OverlaySession", s.session);
    return okay;
}
inline bool overlay_catalog_live(const OverlayCatalog &c) {
    const auto now = GetTickCount64();
    return c.producer_pid && c.session && c.updated_ms && c.updated_ms <= now && now - c.updated_ms < 3000;
}
inline bool overlay_scope_matches(const OverlaySelection &s, const OverlayCatalog &c) {
    return s.producer_pid == c.producer_pid && s.session == c.session;
}
} // namespace vrst::preview
