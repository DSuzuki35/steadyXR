#include "connection.hpp"
#include "settings_path.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <utility>
#include <vector>

namespace vrst::preview {
namespace {
constexpr wchar_t manifest[] = L"XR_APILAYER_STEADYXR_preview.json";
constexpr wchar_t legacy_manifest[] = L"XR_APILAYER_HORIZONLOCK_preview.json";
constexpr wchar_t capture_dll[] = L"SteadyXR-OpenXR.dll";
constexpr wchar_t legacy_capture_dll[] = L"horizonlock-openxr-layer.dll";
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
bool legacy_entry(const Entry &entry) {
    return _wcsicmp(std::filesystem::path(entry.name).filename().c_str(), legacy_manifest) == 0;
}
std::filesystem::path registered_dll(const Entry &entry) {
    return std::filesystem::path(entry.name).parent_path() /
           (legacy_entry(entry) ? legacy_capture_dll : capture_dll);
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
        const auto filename = std::filesystem::path(name).filename();
        if (type != REG_DWORD || size != sizeof(DWORD) ||
            (_wcsicmp(filename.c_str(), manifest) != 0 && _wcsicmp(filename.c_str(), legacy_manifest) != 0))
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
    // Private-subkey callers model the machine scope under HKCU without touching OpenXR.
    if (registry && _wcsicmp(registry, layer_registry_key) == 0)
        return {};
    return {HKEY_CURRENT_USER, registry, nullptr, nullptr};
}
bool usable(const Entry &entry) {
    return entry.value == 0 && file_present(entry.name) && file_present(registered_dll(entry));
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
           (any_version || same_path(std::filesystem::path(entry->name).parent_path().wstring(), directory));
}
bool machine_layer_installed(const std::wstring &directory, bool any_version) {
    return machine_layer_installed(directory, any_version, LayerRegistryRoots{});
}
bool machine_layer_installed(const std::wstring &directory, bool any_version,
                             const LayerRegistryRoots &roots) {
    std::vector<Entry> found;
    if (read_scope(roots.machine_hive, roots.machine_key, found) != ERROR_SUCCESS)
        return false;
    return std::any_of(found.begin(), found.end(), [&](const Entry &entry) {
        return entry.value == 0 &&
               (any_version ||
                same_path(std::filesystem::path(entry.name).parent_path().wstring(), directory));
    });
}
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const LayerRegistryRoots &roots) {
    const auto settings = compatible_layer_settings_path(directory, roots);
    return settings ? std::optional<std::wstring>{std::filesystem::path(*settings).parent_path().wstring()}
                    : std::nullopt;
}
std::optional<std::wstring> compatible_layer_settings_path(const std::wstring &directory,
                                                           const LayerRegistryRoots &roots) {
    const auto entry = effective_entry(roots);
    if (!entry)
        return std::nullopt;
    const auto registered_directory = std::filesystem::path(entry->name).parent_path().lexically_normal();
    const auto dll = registered_dll(*entry);
    if (same_file_contents(std::filesystem::path(directory) / capture_dll, dll))
        return capture_settings_path(registered_directory.wstring());
    // IPC v4 requires an explicit upgrade from every older capture DLL.
    return std::nullopt;
}
std::optional<std::wstring> installed_layer_settings_path(const LayerRegistryRoots &roots) {
    const auto entry = effective_entry(roots);
    return entry ? std::optional<std::wstring>{capture_settings_path(
                       std::filesystem::path(entry->name).parent_path().wstring())}
                 : std::nullopt;
}
namespace {
struct RawValue {
    std::wstring name;
    DWORD type{};
    std::vector<BYTE> bytes;
};
bool own_value(const RawValue &value) {
    const auto filename = std::filesystem::path(value.name).filename();
    return _wcsicmp(filename.c_str(), manifest) == 0 || _wcsicmp(filename.c_str(), legacy_manifest) == 0;
}
DWORD number(const RawValue &value) {
    DWORD result = 1;
    if (value.type == REG_DWORD && value.bytes.size() == sizeof(result))
        memcpy(&result, value.bytes.data(), sizeof(result));
    return result;
}
RawValue dword(std::wstring name, DWORD value) {
    RawValue result{std::move(name), REG_DWORD, std::vector<BYTE>(sizeof(value))};
    memcpy(result.bytes.data(), &value, sizeof(value));
    return result;
}
bool vector_layer(const RawValue &value) {
    if (number(value) != 0)
        return false;
    // Registry names are manifest paths; VectorXR can use any JSON filename.
    std::error_code error;
    if (std::filesystem::file_size(value.name, error) > 1024 * 1024 || error)
        return false;
    std::ifstream input(std::filesystem::path(value.name), std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    static const std::regex name(R"("name"\s*:\s*"XR_APILAYER_DIENERTECH_VECTORXR")");
    return std::regex_search(json, name);
}
LONG raw_values(HKEY key, std::vector<RawValue> &values) {
    DWORD count{}, max_name{}, max_bytes{};
    auto result = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count,
                                  &max_name, &max_bytes, nullptr, nullptr);
    if (result != ERROR_SUCCESS)
        return result;
    std::vector<wchar_t> name(max_name + 1);
    for (DWORD i = 0; i < count; ++i) {
        RawValue value;
        DWORD length = static_cast<DWORD>(name.size()), size = max_bytes;
        value.bytes.resize(size);
        result = RegEnumValueW(key, i, name.data(), &length, nullptr, &value.type, value.bytes.data(), &size);
        if (result != ERROR_SUCCESS)
            return result;
        value.name.assign(name.data(), length);
        value.bytes.resize(size);
        values.push_back(std::move(value));
    }
    return ERROR_SUCCESS;
}
bool same_values(const std::vector<RawValue> &a, const std::vector<RawValue> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].type != b[i].type || a[i].bytes != b[i].bytes)
            return false;
    return true;
}
struct RegistryKey {
    HKEY handle{};
    ~RegistryKey() { if (handle) RegCloseKey(handle); }
    void close() { if (handle) RegCloseKey(handle); handle = nullptr; }
};
struct Scope {
    HKEY hive{};
    const wchar_t *path{};
    RegistryKey key;
    bool existed{};
    std::vector<RawValue> before, desired, expected;
};
LONG open_scope(Scope &scope, bool create) {
    if (!scope.hive || !scope.path)
        return ERROR_SUCCESS;
    auto result = RegOpenKeyExW(scope.hive, scope.path, 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &scope.key.handle);
    scope.existed = result == ERROR_SUCCESS;
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
        if (!create)
            return ERROR_SUCCESS;
        result = RegCreateKeyExW(scope.hive, scope.path, 0, nullptr, 0,
                                 KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                                 nullptr, &scope.key.handle, nullptr);
    }
    if (result == ERROR_SUCCESS)
        result = raw_values(scope.key.handle, scope.before);
    scope.expected = scope.desired = scope.before;
    return result;
}
LONG set_raw(HKEY key, const RawValue &value) {
    return RegSetValueExW(key, value.name.c_str(), 0, value.type, value.bytes.data(),
                          static_cast<DWORD>(value.bytes.size()));
}
LONG read_path(HKEY hive, const wchar_t *path, std::vector<RawValue> &values) {
    RegistryKey key;
    auto result = RegOpenKeyExW(hive, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key.handle);
    return result == ERROR_SUCCESS ? raw_values(key.handle, values) : result;
}
LONG replace_order(Scope &scope, const std::vector<RawValue> &desired) {
    const std::wstring path(scope.path);
    const auto slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return ERROR_INVALID_PARAMETER;
    const auto parent_path = path.substr(0, slash), leaf = path.substr(slash + 1);
    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto staging = L"SteadyXR-staging-" + suffix, recovery = L"SteadyXR-recovery-" + suffix;
    RegistryKey parent, source, target;
    auto result = RegOpenKeyExW(scope.hive, parent_path.c_str(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                                &parent.handle);
    if (result != ERROR_SUCCESS)
        return result;
    result = RegOpenKeyExW(parent.handle, leaf.c_str(), 0, KEY_READ | DELETE | KEY_WOW64_64KEY, &source.handle);
    if (result != ERROR_SUCCESS)
        return result;
    DWORD subkeys{};
    result = RegQueryInfoKeyW(source.handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr);
    if (result != ERROR_SUCCESS || subkeys)
        return result == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : result;
    DWORD disposition{};
    result = RegCreateKeyExW(parent.handle, staging.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                             nullptr, &target.handle, &disposition);
    if (result != ERROR_SUCCESS)
        return result;
    if (disposition != REG_CREATED_NEW_KEY)
        return ERROR_ALREADY_EXISTS;
    for (const auto &value : desired) {
        result = set_raw(target.handle, value);
        if (result != ERROR_SUCCESS)
            break;
    }
    // Source already has desired contents. Copy security without disturbing staged value order.
    if (result == ERROR_SUCCESS)
        result = RegCopyTreeW(source.handle, nullptr, target.handle);
    std::vector<RawValue> staged, original;
    if (result == ERROR_SUCCESS)
        result = raw_values(target.handle, staged);
    if (result == ERROR_SUCCESS && !same_values(staged, desired))
        result = ERROR_INVALID_DATA;
    if (result == ERROR_SUCCESS)
        result = raw_values(source.handle, original);
    if (result == ERROR_SUCCESS && !same_values(original, scope.expected))
        result = ERROR_TRANSACTIONAL_CONFLICT;
    target.close(); source.close(); scope.key.close();
    bool retained = false, activated = false;
    if (result == ERROR_SUCCESS) {
        result = RegRenameKey(parent.handle, leaf.c_str(), recovery.c_str());
        retained = result == ERROR_SUCCESS;
    }
    if (result == ERROR_SUCCESS) {
        result = RegRenameKey(parent.handle, staging.c_str(), leaf.c_str());
        activated = result == ERROR_SUCCESS;
    }
    if (result == ERROR_SUCCESS) {
        std::vector<RawValue> written;
        result = read_path(scope.hive, scope.path, written);
        if (result == ERROR_SUCCESS && !same_values(written, desired))
            result = ERROR_INVALID_DATA;
    }
    if (result != ERROR_SUCCESS && retained) {
        if (activated && RegRenameKey(parent.handle, leaf.c_str(), staging.c_str()) != ERROR_SUCCESS)
            return ERROR_TRANSACTIONAL_CONFLICT;
        if (RegRenameKey(parent.handle, recovery.c_str(), leaf.c_str()) != ERROR_SUCCESS)
            return ERROR_TRANSACTIONAL_CONFLICT;
    }
    if (result == ERROR_SUCCESS) {
        scope.expected = desired;
        // Refuse to delete a retained key if an external installer changed it through an old handle.
        std::vector<RawValue> retained_values;
        const auto retained_path = parent_path + L"\\" + recovery;
        const auto read = read_path(scope.hive, retained_path.c_str(), retained_values);
        if (read != ERROR_SUCCESS || !same_values(retained_values, original))
            return ERROR_TRANSACTIONAL_CONFLICT;
        result = RegDeleteKeyExW(parent.handle, recovery.c_str(), KEY_WOW64_64KEY, 0);
    } else if (!activated) {
        RegDeleteKeyExW(parent.handle, staging.c_str(), KEY_WOW64_64KEY, 0);
    }
    const auto opened = RegOpenKeyExW(scope.hive, scope.path, 0,
                                      KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &scope.key.handle);
    return result == ERROR_SUCCESS ? opened : result;
}
LONG write_scope(Scope &scope, const std::vector<RawValue> &desired) {
    if (same_values(scope.expected, desired))
        return ERROR_SUCCESS;
    if (!scope.key.handle) {
        const auto opened = RegOpenKeyExW(scope.hive, scope.path, 0,
                                         KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &scope.key.handle);
        if (opened != ERROR_SUCCESS)
            return opened;
    }
    std::vector<RawValue> latest;
    auto result = raw_values(scope.key.handle, latest);
    if (result != ERROR_SUCCESS || !same_values(latest, scope.expected))
        return result == ERROR_SUCCESS ? ERROR_TRANSACTIONAL_CONFLICT : result;
    for (size_t i = 0; i < scope.expected.size();) {
        const auto &name = scope.expected[i].name;
        if (std::any_of(desired.begin(), desired.end(), [&](const RawValue &value) { return value.name == name; })) {
            ++i;
            continue;
        }
        result = RegDeleteValueW(scope.key.handle, name.c_str());
        if (result != ERROR_SUCCESS)
            return result;
        scope.expected.erase(scope.expected.begin() + i);
    }
    for (const auto &value : desired) {
        const auto prior = std::find_if(scope.expected.begin(), scope.expected.end(), [&](const RawValue &v) {
            return v.name == value.name;
        });
        if (prior != scope.expected.end() && prior->type == value.type && prior->bytes == value.bytes)
            continue;
        result = set_raw(scope.key.handle, value);
        if (result != ERROR_SUCCESS)
            return result;
        if (prior == scope.expected.end())
            scope.expected.push_back(value);
        else
            *prior = value;
    }
    if (!same_values(scope.expected, desired))
        return replace_order(scope, desired);
    latest.clear();
    result = raw_values(scope.key.handle, latest);
    return result != ERROR_SUCCESS ? result : same_values(latest, desired) ? ERROR_SUCCESS : ERROR_TRANSACTIONAL_CONFLICT;
}
void plan_scope(Scope &scope, const std::wstring &path, bool enable, bool machine) {
    bool had_current = false;
    for (auto &value : scope.desired) {
        if (!own_value(value))
            continue;
        const bool current = same_path(value.name, path);
        had_current |= current;
        value = dword(value.name, machine && enable && current ? 0 : 1);
    }
    if (!machine || !enable)
        return;
    if (!had_current)
        scope.desired.push_back(dword(path, 0));
    auto vector = std::find_if(scope.desired.begin(), scope.desired.end(), vector_layer);
    if (vector != scope.desired.end()) {
        auto current = std::find_if(scope.desired.begin(), scope.desired.end(), [&](const RawValue &value) {
            return same_path(value.name, path);
        });
        auto value = *current;
        scope.desired.erase(current);
        vector = std::find_if(scope.desired.begin(), scope.desired.end(), vector_layer);
        scope.desired.insert(vector, std::move(value));
    }
}
} // namespace
LONG set_layer_connection(const std::wstring &directory, bool enable, const LayerRegistryRoots &roots) {
    const auto path = (std::filesystem::path(directory) / manifest).wstring();
    if (enable && (!file_present(path) || !file_present(std::filesystem::path(directory) / capture_dll)))
        return ERROR_FILE_NOT_FOUND;
    if (!roots.machine_hive || !roots.machine_key)
        return ERROR_INVALID_PARAMETER;
    const auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\SteadyXR-LayerRegistryWrites");
    if (!mutex)
        return GetLastError();
    const auto wait = WaitForSingleObject(mutex, 10000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return ERROR_BUSY;
    }
    Scope machine{roots.machine_hive, roots.machine_key}, user{roots.user_hive, roots.user_key};
    auto result = open_scope(machine, enable);
    if (result == ERROR_SUCCESS)
        result = open_scope(user, false);
    if (result == ERROR_SUCCESS) {
        plan_scope(machine, path, enable, true);
        plan_scope(user, path, enable, false);
        result = write_scope(machine, machine.desired);
        if (result == ERROR_SUCCESS)
            result = write_scope(user, user.desired);
        if (result != ERROR_SUCCESS) {
            const auto user_rollback = write_scope(user, user.before);
            const auto machine_rollback = write_scope(machine, machine.before);
            if (user_rollback != ERROR_SUCCESS || machine_rollback != ERROR_SUCCESS)
                result = ERROR_TRANSACTIONAL_CONFLICT;
        }
    }
    ReleaseMutex(mutex); CloseHandle(mutex);
    return result;
}

bool layer_installed(const std::wstring &directory, bool any_version, const wchar_t *registry) {
    return layer_installed(directory, any_version, registry_roots(registry));
}
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const wchar_t *registry) {
    return compatible_layer_directory(directory, registry_roots(registry));
}
std::optional<std::wstring> compatible_layer_settings_path(const std::wstring &directory,
                                                           const wchar_t *registry) {
    return compatible_layer_settings_path(directory, registry_roots(registry));
}
LONG set_layer_connection(const std::wstring &directory, bool enable, const wchar_t *registry) {
    return set_layer_connection(directory, enable, registry_roots(registry));
}
} // namespace vrst::preview
