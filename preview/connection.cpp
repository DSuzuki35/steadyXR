#include "connection.hpp"
#include <filesystem>
#include <vector>

namespace vrst::preview {
namespace {
constexpr wchar_t manifest[] = L"XR_APILAYER_HORIZONLOCK_preview.json";
struct Entry {
    std::wstring name;
    DWORD value;
};
bool same_path(const std::wstring &a, const std::wstring &b) {
    const auto x = std::filesystem::path(a).lexically_normal().wstring();
    const auto y = std::filesystem::path(b).lexically_normal().wstring();
    return _wcsicmp(x.c_str(), y.c_str()) == 0;
}
bool file_present(const std::filesystem::path &path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}
LONG entries(HKEY key, std::vector<Entry> &out) {
    for (DWORD index = 0;; ++index) {
        wchar_t name[32768]{};
        DWORD length = 32768, value = 1, size = sizeof(value), type = 0;
        const auto result =
            RegEnumValueW(key, index, name, &length, nullptr, &type, reinterpret_cast<BYTE *>(&value), &size);
        if (result == ERROR_NO_MORE_ITEMS)
            return ERROR_SUCCESS;
        if (result != ERROR_SUCCESS)
            return result;
        if (type == REG_DWORD && size == sizeof(value) &&
            _wcsicmp(std::filesystem::path(name).filename().c_str(), manifest) == 0)
            out.push_back({name, value});
    }
}
LONG write(HKEY key, const std::wstring &name, DWORD value) {
    return RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value),
                          sizeof(value));
}
} // namespace
bool layer_installed(const std::wstring &directory, bool any_version, const wchar_t *registry) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, registry, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    std::vector<Entry> found;
    const auto result = entries(key, found);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return false;
    const auto path = (std::filesystem::path(directory) / manifest).wstring();
    for (const auto &entry : found)
        if (entry.value == 0 && (any_version || same_path(entry.name, path)) && file_present(entry.name) &&
            file_present(std::filesystem::path(entry.name).parent_path() / L"horizonlock-openxr-layer.dll"))
            return true;
    return false;
}
LONG set_layer_connection(const std::wstring &directory, bool enable, const wchar_t *registry) {
    const auto path = (std::filesystem::path(directory) / manifest).wstring();
    if (enable && (!file_present(path) ||
                   !file_present(std::filesystem::path(directory) / L"horizonlock-openxr-layer.dll")))
        return ERROR_FILE_NOT_FOUND;
    HKEY key{};
    auto result = RegCreateKeyExW(HKEY_CURRENT_USER, registry, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                                  nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS)
        return result;
    std::vector<Entry> before;
    result = entries(key, before);
    bool had_current = false;
    if (result == ERROR_SUCCESS) {
        for (const auto &entry : before) {
            const bool current = same_path(entry.name, path);
            had_current |= current;
            // Keep old files and registrations available for rollback; only enable one version.
            result = write(key, entry.name, enable && current ? 0 : 1);
            if (result != ERROR_SUCCESS)
                break;
        }
        if (result == ERROR_SUCCESS && enable && !had_current)
            result = write(key, path, 0);
        if (result != ERROR_SUCCESS) {
            for (const auto &entry : before)
                write(key, entry.name, entry.value);
            if (!had_current)
                RegDeleteValueW(key, path.c_str());
        }
    }
    RegCloseKey(key);
    return result;
}
} // namespace vrst::preview
