#include "connection.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <utility>
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
        DWORD length = 32768, size = 0, type = 0;
        auto result = RegEnumValueW(key, index, name, &length, nullptr, &type, nullptr, &size);
        if (result == ERROR_NO_MORE_ITEMS)
            return ERROR_SUCCESS;
        if (result != ERROR_SUCCESS)
            return result;
        if (type != REG_DWORD || size != sizeof(DWORD) ||
            _wcsicmp(std::filesystem::path(name).filename().c_str(), manifest) != 0)
            continue;
        DWORD value = 1;
        result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(&value), &size);
        if (result == ERROR_FILE_NOT_FOUND)
            continue;
        if (result != ERROR_SUCCESS)
            return result;
        if (type == REG_DWORD && size == sizeof(value))
            out.push_back({name, value});
    }
}
LONG read_scope(HKEY hive, const wchar_t *registry, std::vector<Entry> &out) {
    if (!hive || !registry)
        return ERROR_SUCCESS;
    HKEY key{};
    auto result = RegOpenKeyExW(hive, registry, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
        return ERROR_SUCCESS;
    if (result != ERROR_SUCCESS)
        return result;
    result = entries(key, out);
    RegCloseKey(key);
    return result;
}
LayerRegistryRoots registry_roots(const wchar_t *registry) {
    // Legacy private-subkey callers never inspect or modify the real machine registry.
    if (registry && _wcsicmp(registry, layer_registry_key) == 0)
        return {};
    return {nullptr, nullptr, HKEY_CURRENT_USER, registry};
}
bool usable(const Entry &entry) {
    return entry.value == 0 && file_present(entry.name) &&
           file_present(std::filesystem::path(entry.name).parent_path() / L"horizonlock-openxr-layer.dll");
}
std::optional<Entry> effective_entry(const LayerRegistryRoots &roots) {
    // Match the loader's discovery order. A later duplicate cannot choose the settings file.
    for (const auto &scope :
         {std::pair{roots.machine_hive, roots.machine_key}, std::pair{roots.user_hive, roots.user_key}}) {
        std::vector<Entry> found;
        if (read_scope(scope.first, scope.second, found) != ERROR_SUCCESS)
            return std::nullopt;
        for (const auto &entry : found)
            if (usable(entry))
                return entry;
    }
    return std::nullopt;
}
LONG write(HKEY key, const std::wstring &name, DWORD value) {
    return RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value),
                          sizeof(value));
}
} // namespace
bool layer_installed(const std::wstring &directory, bool any_version, const LayerRegistryRoots &roots) {
    const auto entry = effective_entry(roots);
    return entry &&
           (any_version || same_path(entry->name, (std::filesystem::path(directory) / manifest).wstring()));
}
bool machine_layer_installed(const std::wstring &directory, bool any_version) {
    return machine_layer_installed(directory, any_version, LayerRegistryRoots{});
}
bool machine_layer_installed(const std::wstring &directory, bool any_version,
                             const LayerRegistryRoots &roots) {
    std::vector<Entry> found;
    if (read_scope(roots.machine_hive, roots.machine_key, found) != ERROR_SUCCESS)
        return false;
    const auto path = (std::filesystem::path(directory) / manifest).wstring();
    return std::any_of(found.begin(), found.end(), [&](const Entry &entry) {
        return entry.value == 0 && (any_version || same_path(entry.name, path));
    });
}
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const LayerRegistryRoots &roots) {
    const auto entry = effective_entry(roots);
    if (!entry)
        return std::nullopt;
    const auto registered_directory = std::filesystem::path(entry->name).parent_path().lexically_normal();
    if (same_file_contents(std::filesystem::path(directory) / L"horizonlock-openxr-layer.dll",
                           registered_directory / L"horizonlock-openxr-layer.dll"))
        return registered_directory.wstring();
    return std::nullopt;
}
LONG set_layer_connection(const std::wstring &directory, bool enable, const LayerRegistryRoots &roots) {
    const auto path = (std::filesystem::path(directory) / manifest).wstring();
    if (enable && (!file_present(path) ||
                   !file_present(std::filesystem::path(directory) / L"horizonlock-openxr-layer.dll")))
        return ERROR_FILE_NOT_FOUND;
    std::vector<Entry> machine_entries;
    auto result = read_scope(roots.machine_hive, roots.machine_key, machine_entries);
    if (result != ERROR_SUCCESS)
        return result;
    if (std::any_of(machine_entries.begin(), machine_entries.end(),
                    [](const Entry &entry) { return entry.value == 0; }))
        return ERROR_ACCESS_DENIED;
    if (!roots.user_hive || !roots.user_key)
        return ERROR_INVALID_PARAMETER;
    HKEY key{};
    result = RegCreateKeyExW(roots.user_hive, roots.user_key, 0, nullptr, 0,
                             KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
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
bool layer_installed(const std::wstring &directory, bool any_version, const wchar_t *registry) {
    return layer_installed(directory, any_version, registry_roots(registry));
}
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const wchar_t *registry) {
    return compatible_layer_directory(directory, registry_roots(registry));
}
LONG set_layer_connection(const std::wstring &directory, bool enable, const wchar_t *registry) {
    return set_layer_connection(directory, enable, registry_roots(registry));
}
} // namespace vrst::preview
