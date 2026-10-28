#include "connection.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
using namespace vrst::preview;
namespace {
struct Value { std::wstring name; DWORD type; std::vector<BYTE> data; };
void put(HKEY key, const std::wstring &name, DWORD value) {
    RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
}
std::vector<Value> read(HKEY hive, const std::wstring &path) {
    HKEY key{}; std::vector<Value> result;
    if (RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key)) return result;
    for (DWORD i = 0;; ++i) {
        wchar_t name[32768]{}; DWORD count = 32768, type{}, size{};
        if (RegEnumValueW(key, i, name, &count, nullptr, &type, nullptr, &size) == ERROR_NO_MORE_ITEMS) break;
        Value value{name, type, std::vector<BYTE>(size)};
        RegQueryValueExW(key, name, nullptr, &value.type, value.data.data(), &size);
        result.push_back(value);
    }
    RegCloseKey(key); return result;
}
DWORD get(const std::vector<Value> &values, const std::wstring &name) {
    for (const auto &value : values) if (value.name == name && value.type == REG_DWORD && value.data.size() == 4) {
        DWORD number{}; memcpy(&number, value.data.data(), 4); return number;
    }
    return 99;
}
bool same(const std::vector<Value> &a, const std::vector<Value> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].type != b[i].type || a[i].data != b[i].data) return false;
    return true;
}
}
int main() {
    const auto private_key = L"SOFTWARE\\SteadyXR-tests\\Connection-" + std::to_wstring(GetCurrentProcessId());
    const auto machine_key = private_key + L"\\Machine", user_key = private_key + L"\\User";
    const LayerRegistryRoots roots{HKEY_CURRENT_USER, machine_key.c_str(), HKEY_CURRENT_USER, user_key.c_str()};
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto root = std::filesystem::path(temp) / (L"SteadyXR-connection-" + std::to_wstring(GetCurrentProcessId()));
    const auto current = root / L"current", old = root / L"old", legacy = root / L"legacy", vector = root / L"vector";
    for (const auto &dir : {current, old, legacy, vector}) std::filesystem::create_directories(dir);
    for (const auto &dir : {current, old}) {
        std::ofstream(dir / L"XR_APILAYER_STEADYXR_preview.json") << "{}";
        std::ofstream(dir / L"SteadyXR-OpenXR.dll") << "matching current fixture";
    }
    std::ofstream(legacy / L"XR_APILAYER_HORIZONLOCK_preview.json") << "{}";
    std::ofstream(legacy / L"horizonlock-openxr-layer.dll") << "incompatible fixture";
    const auto current_path = (current / L"XR_APILAYER_STEADYXR_preview.json").wstring();
    const auto old_path = (old / L"XR_APILAYER_STEADYXR_preview.json").wstring();
    const auto legacy_path = (legacy / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    // A nonstandard filename catches matching by basename instead of the manifest's layer identity.
    const auto vector_path = (vector / L"custom-name.json").wstring();
    std::ofstream(std::filesystem::path(vector_path)) << R"({"api_layer":{"name":"XR_APILAYER_DIENERTECH_VECTORXR","library_path":"VectorXR.dll"}})";
    HKEY machine{}, user{};
    RegCreateKeyExW(HKEY_CURRENT_USER, machine_key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &machine, nullptr);
    RegCreateKeyExW(HKEY_CURRENT_USER, user_key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr);
    bool okay = true;
    const auto check = [&](const char *label, bool value) { std::cout << label << ": " << value << '\n'; okay = value && okay; };
    put(user, old_path, 0); put(user, legacy_path, 0);
    check("compatible per-user DLL is discovered before migration", compatible_layer_directory(current.wstring(), roots) == old.wstring());
    put(machine, L"first unrelated.json", 0); put(machine, vector_path, 0); put(machine, L"disabled unrelated.json", 7);
    const BYTE malformed[]{1, 0, 2, 0, 3};
    RegSetValueExW(machine, L"malformed unrelated", 0, REG_BINARY, malformed, sizeof(malformed));
    const auto unrelated = read(HKEY_CURRENT_USER, machine_key);
    HKEY child{};
    RegCreateKeyExW(machine, L"vendor-child", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &child, nullptr);
    put(child, L"keep", 77);
    const auto users_before_failure = read(HKEY_CURRENT_USER, user_key);
    check("failed reorder rolls back staged machine registration", set_layer_connection(current.wstring(), true, roots) != ERROR_SUCCESS && same(unrelated, read(HKEY_CURRENT_USER, machine_key)) && same(users_before_failure, read(HKEY_CURRENT_USER, user_key)));
    RegCloseKey(child);
    RegDeleteKeyW(machine, L"vendor-child");
    const auto install_result = set_layer_connection(current.wstring(), true, roots);
    std::cout << "install result: " << install_result << '\n';
    check("install migrates even a byte-identical per-user DLL", install_result == ERROR_SUCCESS);
    auto installed = read(HKEY_CURRENT_USER, machine_key), users = read(HKEY_CURRENT_USER, user_key);
    check("only current bundled DLL is globally enabled", get(installed, current_path) == 0 && get(users, old_path) == 1 && get(users, legacy_path) == 1 && get(users, current_path) == 99 && machine_layer_installed(current.wstring(), false, roots));
    check("bundled layer immediately precedes enabled VectorXR", installed.size() == 5 && installed[0].name == L"first unrelated.json" && installed[1].name == current_path && installed[2].name == vector_path);
    auto filtered = installed;
    filtered.erase(std::remove_if(filtered.begin(), filtered.end(), [&](const Value &v) { return v.name == current_path; }), filtered.end());
    check("unrelated order, enable flags, types and bytes preserved", same(filtered, unrelated));
    check("reinstall is idempotent", set_layer_connection(current.wstring(), true, roots) == ERROR_SUCCESS && same(installed, read(HKEY_CURRENT_USER, machine_key)));
    check("missing bundle cannot replace working installation", set_layer_connection((root / L"missing").wstring(), true, roots) == ERROR_FILE_NOT_FOUND && same(installed, read(HKEY_CURRENT_USER, machine_key)));
    // Failure in the second scope must leave the staged machine changes uncommitted.
    const LayerRegistryRoots invalid_user{HKEY_CURRENT_USER, machine_key.c_str(), reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0x1234)), L"Invalid"};
    check("failed user migration rolls back machine upgrade", set_layer_connection(old.wstring(), true, invalid_user) != ERROR_SUCCESS && same(installed, read(HKEY_CURRENT_USER, machine_key)));
    check("disconnect disables both scopes without changing unrelated values", set_layer_connection(current.wstring(), false, roots) == ERROR_SUCCESS && get(read(HKEY_CURRENT_USER, machine_key), current_path) == 1 && !layer_installed(current.wstring(), true, roots));
    // Reopen handles: an atomic replacement can retire a previous key handle.
    RegCloseKey(machine); machine = nullptr;
    RegOpenKeyExW(HKEY_CURRENT_USER, machine_key.c_str(), 0, KEY_ALL_ACCESS, &machine);
    RegDeleteValueW(machine, vector_path.c_str());
    auto before = read(HKEY_CURRENT_USER, machine_key);
    check("without VectorXR an existing global position is preserved", set_layer_connection(current.wstring(), true, roots) == ERROR_SUCCESS);
    auto after = read(HKEY_CURRENT_USER, machine_key);
    check("existing global order remains stable", before.size() == after.size() && before[1].name == current_path && after[1].name == current_path);
    check("new release upgrades old global registration", set_layer_connection(old.wstring(), true, roots) == ERROR_SUCCESS && get(read(HKEY_CURRENT_USER, machine_key), current_path) == 1 && get(read(HKEY_CURRENT_USER, machine_key), old_path) == 0);
    check("exact bundled release resolves after upgrade", compatible_layer_directory(old.wstring(), roots) == old.wstring());
    check("installed settings are available for upgrade independently of binary compatibility", installed_layer_settings_path(roots) == (old / L"SteadyXR.ini").wstring());
    std::ofstream(old / L"SteadyXR-OpenXR.dll") << "previous incompatible release";
    check("incompatible global DLL cannot resolve to a masked per-user copy", !compatible_layer_directory(current.wstring(), roots));
    check("incompatible prior release still exposes settings for explicit migration", installed_layer_settings_path(roots) == (old / L"SteadyXR.ini").wstring());
    RegCloseKey(machine); RegCloseKey(user);
    RegDeleteTreeW(HKEY_CURRENT_USER, private_key.c_str());
    for (const auto &dir : {current, old, legacy, vector}) {
        for (const auto *filename : {L"XR_APILAYER_STEADYXR_preview.json", L"SteadyXR-OpenXR.dll", L"XR_APILAYER_HORIZONLOCK_preview.json", L"horizonlock-openxr-layer.dll", L"custom-name.json"}) std::filesystem::remove(dir / filename);
        std::filesystem::remove(dir);
    }
    std::filesystem::remove(root);
    return okay ? 0 : 1;
}
