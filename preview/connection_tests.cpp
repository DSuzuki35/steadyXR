#include "connection.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace vrst::preview;
int main() {
    const auto key = L"SOFTWARE\\HorizonLockVR-tests\\Connection-" + std::to_wstring(GetCurrentProcessId());
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root =
        std::filesystem::path(temp) / (L"HorizonLock-connection-" + std::to_wstring(GetCurrentProcessId()));
    const auto old = root / L"old", current = root / L"current";
    std::filesystem::create_directories(old);
    std::filesystem::create_directories(current);
    for (const auto &dir : {old, current}) {
        std::ofstream(dir / L"XR_APILAYER_HORIZONLOCK_preview.json") << "{}";
        std::ofstream(dir / L"horizonlock-openxr-layer.dll") << "fixture";
    }
    HKEY handle{};
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &handle, nullptr);
    DWORD zero = 0;
    const auto old_path = (old / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    RegSetValueExW(handle, old_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    RegSetValueExW(handle, L"C:\\OtherLayer.json", 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero),
                   sizeof(zero));
    const auto get = [&](const wchar_t *name) {
        DWORD value = 99, size = sizeof(value);
        RegQueryValueExW(handle, name, nullptr, nullptr, reinterpret_cast<BYTE *>(&value), &size);
        return value;
    };
    bool okay = layer_installed(current.wstring(), true, key.c_str()) &&
                !layer_installed(current.wstring(), false, key.c_str());
    const auto result = set_layer_connection(current.wstring(), true, key.c_str());
    okay = okay && result == ERROR_SUCCESS && layer_installed(current.wstring(), false, key.c_str()) &&
           get(old_path.c_str()) == 1 && get(L"C:\\OtherLayer.json") == 0;
    std::cout << "upgrade replaces previous Horizon Lock layer, preserves other layers: " << okay << '\n';
    okay = set_layer_connection(current.wstring(), false, key.c_str()) == ERROR_SUCCESS && okay;
    okay = !layer_installed(current.wstring(), true, key.c_str()) && get(L"C:\\OtherLayer.json") == 0 && okay;
    // Missing releases cannot disable the working installed layer.
    set_layer_connection(old.wstring(), true, key.c_str());
    okay = set_layer_connection((root / L"missing").wstring(), true, key.c_str()) == ERROR_FILE_NOT_FOUND &&
           get(old_path.c_str()) == 0 && okay;
    RegCloseKey(handle);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    // Exact test-owned files only; never recurse through arbitrary temp paths.
    for (const auto &dir : {old, current}) {
        std::filesystem::remove(dir / L"XR_APILAYER_HORIZONLOCK_preview.json");
        std::filesystem::remove(dir / L"horizonlock-openxr-layer.dll");
        std::filesystem::remove(dir);
    }
    std::filesystem::remove(root);
    return okay ? 0 : 1;
}
