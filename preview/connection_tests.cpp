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
    const auto old = root / L"old", current = root / L"current", different = root / L"different",
               missing = root / L"missing", disabled = root / L"disabled", unrelated = root / L"unrelated";
    std::filesystem::create_directories(old);
    std::filesystem::create_directories(current);
    std::filesystem::create_directories(different);
    std::filesystem::create_directories(missing);
    std::filesystem::create_directories(disabled);
    std::filesystem::create_directories(unrelated);
    for (const auto &dir : {old, current, disabled}) {
        std::ofstream(dir / L"XR_APILAYER_HORIZONLOCK_preview.json") << "{}";
        std::ofstream(dir / L"horizonlock-openxr-layer.dll") << "fixture";
    }
    std::ofstream(different / L"XR_APILAYER_HORIZONLOCK_preview.json") << "{}";
    std::ofstream(different / L"horizonlock-openxr-layer.dll") << "different fixture";
    std::ofstream(missing / L"XR_APILAYER_HORIZONLOCK_preview.json") << "{}";
    std::ofstream(unrelated / L"OtherLayer.json") << "{}";
    std::ofstream(unrelated / L"horizonlock-openxr-layer.dll") << "fixture";
    HKEY handle{};
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &handle, nullptr);
    const wchar_t malformed[] = L"An unrelated malformed registration must not hide following layers";
    RegSetValueExW(handle, L"Unrelated-malformed-entry", 0, REG_SZ, reinterpret_cast<const BYTE *>(malformed),
                   sizeof(malformed));
    DWORD zero = 0;
    const auto old_path = (old / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    RegSetValueExW(handle, old_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    const auto unrelated_path = (unrelated / L"OtherLayer.json").wstring();
    RegSetValueExW(handle, unrelated_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero),
                   sizeof(zero));
    const auto get = [&](const wchar_t *name) {
        DWORD value = 99, size = sizeof(value);
        RegQueryValueExW(handle, name, nullptr, nullptr, reinterpret_cast<BYTE *>(&value), &size);
        return value;
    };
    bool okay = layer_installed(current.wstring(), true, key.c_str()) &&
                !layer_installed(current.wstring(), false, key.c_str());
    std::cout << "unrelated oversized registry values do not hide the enabled layer: " << okay << '\n';

    const auto compatible_old = compatible_layer_directory(current.wstring(), key.c_str());
    okay = okay && compatible_old && std::filesystem::equivalent(*compatible_old, old);

    const auto current_path = (current / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    RegSetValueExW(handle, current_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    const auto compatible_first = compatible_layer_directory(current.wstring(), key.c_str());
    const bool respects_order = compatible_first && std::filesystem::equivalent(*compatible_first, old);
    std::cout << "first enabled registration wins over the current directory: " << respects_order << '\n';
    okay = okay && respects_order;
    RegDeleteValueW(handle, current_path.c_str());

    const auto different_path = (different / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    RegSetValueExW(handle, different_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero),
                   sizeof(zero));
    RegDeleteValueW(handle, old_path.c_str());
    okay = okay && !compatible_layer_directory(current.wstring(), key.c_str());
    RegDeleteValueW(handle, different_path.c_str());

    const auto missing_path = (missing / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    RegSetValueExW(handle, missing_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    okay = okay && !compatible_layer_directory(current.wstring(), key.c_str());
    RegDeleteValueW(handle, missing_path.c_str());

    DWORD one = 1;
    const auto disabled_path = (disabled / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    RegSetValueExW(handle, disabled_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&one), sizeof(one));
    okay = okay && !compatible_layer_directory(current.wstring(), key.c_str());
    RegDeleteValueW(handle, disabled_path.c_str());

    RegSetValueExW(handle, old_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    const auto result = set_layer_connection(current.wstring(), true, key.c_str());
    okay = okay && result == ERROR_SUCCESS && layer_installed(current.wstring(), false, key.c_str()) &&
           get(old_path.c_str()) == 1 && get(unrelated_path.c_str()) == 0;
    std::cout << "upgrade replaces previous Horizon Lock layer, preserves other layers: " << okay << '\n';
    okay = set_layer_connection(current.wstring(), false, key.c_str()) == ERROR_SUCCESS && okay;
    okay = !layer_installed(current.wstring(), true, key.c_str()) && get(unrelated_path.c_str()) == 0 && okay;
    // Missing releases cannot disable the working installed layer.
    set_layer_connection(old.wstring(), true, key.c_str());
    okay = set_layer_connection((root / L"missing").wstring(), true, key.c_str()) == ERROR_FILE_NOT_FOUND &&
           get(old_path.c_str()) == 0 && okay;

    // Both injected scopes remain under this process's private HKCU fixture.
    const auto machine_key = key + L"\\Machine";
    HKEY machine_handle{};
    RegCreateKeyExW(HKEY_CURRENT_USER, machine_key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                    nullptr, &machine_handle, nullptr);
    const LayerRegistryRoots scopes{HKEY_CURRENT_USER, machine_key.c_str(), HKEY_CURRENT_USER, key.c_str()};
    RegSetValueExW(machine_handle, old_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero),
                   sizeof(zero));
    RegSetValueExW(handle, old_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one), sizeof(one));
    RegSetValueExW(handle, current_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero),
                   sizeof(zero));
    const auto machine_directory = compatible_layer_directory(current.wstring(), scopes);
    const bool machine_first = machine_directory && std::filesystem::equivalent(*machine_directory, old) &&
                               layer_installed(current.wstring(), true, scopes) &&
                               !layer_installed(current.wstring(), false, scopes) &&
                               machine_layer_installed(current.wstring(), true, scopes);
    std::cout << "machine registration wins over a duplicate current user layer: " << machine_first << '\n';
    okay = machine_first && okay;
    const bool private_user_only = layer_installed(current.wstring(), false, key.c_str());
    std::cout << "custom subkey overload stays within the private user scope: " << private_user_only << '\n';
    okay = private_user_only && okay;
    const bool no_machine_mutations =
        set_layer_connection(current.wstring(), true, scopes) == ERROR_ACCESS_DENIED &&
        set_layer_connection(current.wstring(), false, scopes) == ERROR_ACCESS_DENIED &&
        get(current_path.c_str()) == 0 && get(old_path.c_str()) == 1;
    std::cout << "machine edits require editor and leave user registration untouched: "
              << no_machine_mutations << '\n';
    okay = no_machine_mutations && okay;

    RegDeleteValueW(machine_handle, old_path.c_str());
    RegSetValueExW(machine_handle, different_path.c_str(), 0, REG_DWORD,
                   reinterpret_cast<const BYTE *>(&zero), sizeof(zero));
    const bool incompatible_machine = !compatible_layer_directory(current.wstring(), scopes);
    std::cout << "incompatible machine registration cannot resolve to masked user settings: "
              << incompatible_machine << '\n';
    okay = incompatible_machine && okay;
    RegDeleteValueW(machine_handle, different_path.c_str());
    RegSetValueExW(machine_handle, missing_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero),
                   sizeof(zero));
    const bool missing_machine = machine_layer_installed(current.wstring(), true, scopes) &&
                                 set_layer_connection(current.wstring(), true, scopes) == ERROR_ACCESS_DENIED;
    std::cout << "broken machine registration also requires editor repair: " << missing_machine << '\n';
    okay = missing_machine && okay;
    RegDeleteValueW(machine_handle, missing_path.c_str());
    RegSetValueExW(machine_handle, old_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one),
                   sizeof(one));
    const auto user_directory = compatible_layer_directory(current.wstring(), scopes);
    const bool disabled_machine = !machine_layer_installed(current.wstring(), true, scopes) &&
                                  user_directory && std::filesystem::equivalent(*user_directory, current) &&
                                  set_layer_connection(current.wstring(), false, scopes) == ERROR_SUCCESS;
    std::cout << "disabled machine layer allows current-user connection changes: " << disabled_machine
              << '\n';
    okay = disabled_machine && okay;
    RegCloseKey(machine_handle);
    RegCloseKey(handle);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    // Exact test-owned files only; never recurse through arbitrary temp paths.
    for (const auto &dir : {old, current, different, missing, disabled, unrelated}) {
        std::filesystem::remove(dir / L"XR_APILAYER_HORIZONLOCK_preview.json");
        std::filesystem::remove(dir / L"OtherLayer.json");
        std::filesystem::remove(dir / L"horizonlock-openxr-layer.dll");
        std::filesystem::remove(dir);
    }
    std::filesystem::remove(root);
    return okay ? 0 : 1;
}
