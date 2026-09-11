#include "connection.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
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
bool same_file_contents(const std::filesystem::path &a, const std::filesystem::path &b) {
    std::error_code ec;
    const auto a_size = std::filesystem::file_size(a, ec);
    if (ec)
        return false;
    const auto b_size = std::filesystem::file_size(b, ec);
    if (ec || a_size != b_size)
        return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right)
        return false;
    std::array<char, 64 * 1024> left_bytes{}, right_bytes{};
    while (left && right) {
        left.read(left_bytes.data(), static_cast<std::streamsize>(left_bytes.size()));
        right.read(right_bytes.data(), static_cast<std::streamsize>(right_bytes.size()));
        const auto count = left.gcount();
        if (count != right.gcount() ||
            !std::equal(left_bytes.begin(), left_bytes.begin() + count, right_bytes.begin()))
            return false;
    }
    return left.eof() && right.eof();
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
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const wchar_t *registry) {
    const auto requested_directory = std::filesystem::path(directory).lexically_normal();
    const auto requested_dll = requested_directory / L"horizonlock-openxr-layer.dll";
    if (!file_present(requested_dll))
        return std::nullopt;
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, registry, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return std::nullopt;
    std::vector<Entry> found;
    const auto result = entries(key, found);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return std::nullopt;
    std::optional<std::wstring> fallback;
    for (const auto &entry : found) {
        const auto registered_manifest = std::filesystem::path(entry.name);
        const auto registered_directory = registered_manifest.parent_path().lexically_normal();
        const auto registered_dll = registered_directory / L"horizonlock-openxr-layer.dll";
        if (entry.value != 0 || !file_present(registered_manifest) || !file_present(registered_dll) ||
            !same_file_contents(requested_dll, registered_dll))
            continue;
        if (same_path(registered_directory.wstring(), requested_directory.wstring()))
            return registered_directory.wstring();
        if (!fallback)
            fallback = registered_directory.wstring();
    }
    return fallback;
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
