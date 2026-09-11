#include "connection.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace vrst::preview;
int main(int argc, char **argv) {
    const auto key = L"SOFTWARE\\HorizonLockVR-tests\\Connection-" + std::to_wstring(GetCurrentProcessId());
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root =
        std::filesystem::path(temp) / (L"HorizonLock-connection-" + std::to_wstring(GetCurrentProcessId()));
    const auto old = root / L"old", current = root / L"current", different = root / L"different",
               missing = root / L"missing", disabled = root / L"disabled", unrelated = root / L"unrelated",
               legacy = root / L"legacy", trusted = root / L"trusted", tampered = root / L"tampered";
    std::filesystem::create_directories(old);
    std::filesystem::create_directories(current);
    std::filesystem::create_directories(different);
    std::filesystem::create_directories(missing);
    std::filesystem::create_directories(disabled);
    std::filesystem::create_directories(unrelated);
    std::filesystem::create_directories(legacy);
    std::filesystem::create_directories(trusted);
    std::filesystem::create_directories(tampered);
    for (const auto &dir : {old, current, disabled}) {
        std::ofstream(dir / L"XR_APILAYER_STEADYXR_preview.json") << "{}";
        std::ofstream(dir / L"SteadyXR-OpenXR.dll") << "fixture";
    }
    std::ofstream(different / L"XR_APILAYER_STEADYXR_preview.json") << "{}";
    std::ofstream(different / L"SteadyXR-OpenXR.dll") << "different fixture";
    std::ofstream(missing / L"XR_APILAYER_STEADYXR_preview.json") << "{}";
    std::ofstream(unrelated / L"OtherLayer.json") << "{}";
    std::ofstream(unrelated / L"SteadyXR-OpenXR.dll") << "fixture";
    std::ofstream(legacy / L"XR_APILAYER_HORIZONLOCK_preview.json") << "{}";
    std::ofstream(legacy / L"horizonlock-openxr-layer.dll") << "unknown legacy fixture";
    HKEY handle{};
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &handle, nullptr);
    const wchar_t malformed[] = L"An unrelated malformed registration must not hide following layers";
    RegSetValueExW(handle, L"Unrelated-malformed-entry", 0, REG_SZ, reinterpret_cast<const BYTE *>(malformed),
                   sizeof(malformed));
    DWORD zero = 0;
    const auto old_path = (old / L"XR_APILAYER_STEADYXR_preview.json").wstring();
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
    std::ofstream(old / L"preview.ini") << "[Render]\nEnabled=1\n";
    const auto fallback_settings = compatible_layer_settings_path(current.wstring(), key.c_str());
    okay = fallback_settings && std::filesystem::path(*fallback_settings) == old / L"preview.ini" && okay;
    std::filesystem::remove(old / L"preview.ini");
    const auto current_settings = compatible_layer_settings_path(current.wstring(), key.c_str());
    const bool new_settings_path =
        current_settings && std::filesystem::path(*current_settings) == old / L"SteadyXR.ini";
    std::cout << "current DLL settings use SteadyXR.ini adjacent to actual registered layer: "
              << new_settings_path << '\n';
    okay = new_settings_path && okay;

    const auto current_path = (current / L"XR_APILAYER_STEADYXR_preview.json").wstring();
    RegSetValueExW(handle, current_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    const auto compatible_first = compatible_layer_directory(current.wstring(), key.c_str());
    const bool respects_order = compatible_first && std::filesystem::equivalent(*compatible_first, old);
    std::cout << "first enabled registration wins over the current directory: " << respects_order << '\n';
    okay = okay && respects_order;
    RegDeleteValueW(handle, current_path.c_str());

    const auto different_path = (different / L"XR_APILAYER_STEADYXR_preview.json").wstring();
    RegSetValueExW(handle, different_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero),
                   sizeof(zero));
    RegDeleteValueW(handle, old_path.c_str());
    okay = okay && !compatible_layer_directory(current.wstring(), key.c_str());
    RegDeleteValueW(handle, different_path.c_str());

    const auto legacy_path = (legacy / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
    RegSetValueExW(handle, legacy_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    const bool legacy_discovered = layer_installed(legacy.wstring(), false, key.c_str()) &&
                                   layer_installed(current.wstring(), true, key.c_str()) &&
                                   !compatible_layer_directory(current.wstring(), key.c_str());
    std::cout << "legacy filename is discovered but unknown legacy DLL is incompatible: " << legacy_discovered
              << '\n';
    okay = legacy_discovered && okay;
    std::filesystem::copy_file(current / L"SteadyXR-OpenXR.dll", legacy / L"horizonlock-openxr-layer.dll",
                               std::filesystem::copy_options::overwrite_existing);
    const auto renamed_settings = compatible_layer_settings_path(current.wstring(), key.c_str());
    const bool renamed_current =
        renamed_settings && std::filesystem::path(*renamed_settings) == legacy / L"SteadyXR.ini";
    std::cout << "byte-identical current DLL under legacy manifest still uses current settings: "
              << renamed_current << '\n';
    okay = renamed_current && okay;
    std::ofstream(legacy / L"horizonlock-openxr-layer.dll") << "unknown legacy fixture";
    RegDeleteValueW(handle, legacy_path.c_str());

    const auto missing_path = (missing / L"XR_APILAYER_STEADYXR_preview.json").wstring();
    RegSetValueExW(handle, missing_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    okay = okay && !compatible_layer_directory(current.wstring(), key.c_str());
    RegDeleteValueW(handle, missing_path.c_str());

    DWORD one = 1;
    const auto disabled_path = (disabled / L"XR_APILAYER_STEADYXR_preview.json").wstring();
    RegSetValueExW(handle, disabled_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&one), sizeof(one));
    okay = okay && !compatible_layer_directory(current.wstring(), key.c_str());
    RegDeleteValueW(handle, disabled_path.c_str());

    RegSetValueExW(handle, old_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    RegSetValueExW(handle, legacy_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    const auto result = set_layer_connection(current.wstring(), true, key.c_str());
    const bool upgraded = result == ERROR_SUCCESS && layer_installed(current.wstring(), false, key.c_str()) &&
                          get(old_path.c_str()) == 1 && get(legacy_path.c_str()) == 1 &&
                          get(current_path.c_str()) == 0 && get(unrelated_path.c_str()) == 0;
    std::cout << "new install disables both filename generations and preserves unrelated layers: " << upgraded
              << '\n';
    okay = upgraded && okay;
    BYTE raw_after[sizeof(malformed)]{};
    DWORD raw_type = 0, raw_size = sizeof(raw_after);
    const bool raw_preserved = RegQueryValueExW(handle, L"Unrelated-malformed-entry", nullptr, &raw_type,
                                                raw_after, &raw_size) == ERROR_SUCCESS &&
                               raw_type == REG_SZ && raw_size == sizeof(malformed) &&
                               memcmp(raw_after, malformed, sizeof(malformed)) == 0;
    std::cout << "unrelated registry type and bytes are preserved: " << raw_preserved << '\n';
    okay = raw_preserved && okay;
    RegSetValueExW(handle, legacy_path.c_str(), 0, REG_DWORD, reinterpret_cast<BYTE *>(&zero), sizeof(zero));
    okay = set_layer_connection(current.wstring(), false, key.c_str()) == ERROR_SUCCESS && okay;
    okay = !layer_installed(current.wstring(), true, key.c_str()) && get(unrelated_path.c_str()) == 0 && okay;
    const bool disabled_generations = get(legacy_path.c_str()) == 1 && get(current_path.c_str()) == 1;
    std::cout << "disconnect disables both filename generations: " << disabled_generations << '\n';
    okay = disabled_generations && okay;
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
    const auto machine_settings = compatible_layer_settings_path(current.wstring(), scopes);
    const bool machine_first = machine_directory && std::filesystem::equivalent(*machine_directory, old) &&
                               machine_settings &&
                               std::filesystem::path(*machine_settings) == old / L"SteadyXR.ini" &&
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
    RegSetValueExW(machine_handle, legacy_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero),
                   sizeof(zero));
    const bool legacy_machine = machine_layer_installed(legacy.wstring(), false, scopes) &&
                                !compatible_layer_directory(current.wstring(), scopes) &&
                                set_layer_connection(current.wstring(), true, scopes) == ERROR_ACCESS_DENIED;
    std::cout << "legacy machine registration is guarded and cannot use masked new user settings: "
              << legacy_machine << '\n';
    okay = legacy_machine && okay;
    RegDeleteValueW(machine_handle, legacy_path.c_str());
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

    // Optional original DLL is read only. Hash checks use copies in this private fixture.
    if (argc > 1) {
        const auto trusted_path = (trusted / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
        const auto tampered_path = (tampered / L"XR_APILAYER_HORIZONLOCK_preview.json").wstring();
        std::ofstream(std::filesystem::path(trusted_path)) << "{}";
        std::ofstream(std::filesystem::path(tampered_path)) << "{}";
        std::filesystem::copy_file(std::filesystem::path(argv[1]), trusted / L"horizonlock-openxr-layer.dll");
        std::filesystem::copy_file(std::filesystem::path(argv[1]),
                                   tampered / L"horizonlock-openxr-layer.dll");
        std::ofstream(tampered / L"horizonlock-openxr-layer.dll", std::ios::binary | std::ios::app) << '!';
        RegSetValueExW(handle, trusted_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero),
                       sizeof(zero));
        const auto trusted_directory = compatible_layer_directory(current.wstring(), key.c_str());
        const auto trusted_settings = compatible_layer_settings_path(current.wstring(), key.c_str());
        const bool trusted_compatible =
            trusted_directory && std::filesystem::equivalent(*trusted_directory, trusted) &&
            trusted_settings && std::filesystem::path(*trusted_settings) == trusted / L"preview.ini";
        std::cout << "exact trusted legacy capture stays compatible and keeps adjacent preview.ini: "
                  << trusted_compatible << '\n';
        okay = trusted_compatible && okay;
        RegSetValueExW(machine_handle, trusted_path.c_str(), 0, REG_DWORD,
                       reinterpret_cast<const BYTE *>(&zero), sizeof(zero));
        RegSetValueExW(handle, current_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero),
                       sizeof(zero));
        const auto trusted_machine_settings = compatible_layer_settings_path(current.wstring(), scopes);
        const bool trusted_machine_first =
            trusted_machine_settings &&
            std::filesystem::path(*trusted_machine_settings) == trusted / L"preview.ini";
        std::cout << "trusted legacy machine capture keeps its settings ahead of current user layer: "
                  << trusted_machine_first << '\n';
        okay = trusted_machine_first && okay;
        RegDeleteValueW(machine_handle, trusted_path.c_str());
        RegSetValueExW(handle, current_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one),
                       sizeof(one));
        RegDeleteValueW(handle, trusted_path.c_str());
        RegSetValueExW(handle, tampered_path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero),
                       sizeof(zero));
        const bool tampered_rejected = !compatible_layer_directory(current.wstring(), key.c_str());
        std::cout << "changed trusted legacy DLL is rejected: " << tampered_rejected << '\n';
        okay = tampered_rejected && okay;
        RegDeleteValueW(handle, tampered_path.c_str());
    } else {
        std::cout << "trusted legacy hash integration: skipped (pass original DLL path to test)\n";
    }
    RegCloseKey(machine_handle);
    RegCloseKey(handle);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    // Exact test-owned files only; never recurse through arbitrary temp paths.
    for (const auto &dir :
         {old, current, different, missing, disabled, unrelated, legacy, trusted, tampered}) {
        std::filesystem::remove(dir / L"XR_APILAYER_HORIZONLOCK_preview.json");
        std::filesystem::remove(dir / L"horizonlock-openxr-layer.dll");
        std::filesystem::remove(dir / L"XR_APILAYER_STEADYXR_preview.json");
        std::filesystem::remove(dir / L"OtherLayer.json");
        std::filesystem::remove(dir / L"SteadyXR-OpenXR.dll");
        std::filesystem::remove(dir);
    }
    std::filesystem::remove(root);
    return okay ? 0 : 1;
}
