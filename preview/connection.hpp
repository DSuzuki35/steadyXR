#pragma once
#include <optional>
#include <string>
#include <windows.h>

namespace vrst::preview {
inline constexpr wchar_t layer_registry_key[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
// Scope injection keeps registry fixtures isolated; production defaults always use the 64-bit view.
struct LayerRegistryRoots {
    HKEY machine_hive = HKEY_LOCAL_MACHINE;
    const wchar_t *machine_key = layer_registry_key;
    HKEY user_hive = HKEY_CURRENT_USER;
    const wchar_t *user_key = layer_registry_key;
};
// A custom subkey uses only HKCU. The default checks native machine-then-user discovery order.
bool layer_installed(const std::wstring &directory, bool any_version = false,
                     const wchar_t *registry = layer_registry_key);
bool layer_installed(const std::wstring &directory, bool any_version, const LayerRegistryRoots &roots);
// Enabled machine registrations require the editor even when their files are missing.
bool machine_layer_installed(const std::wstring &directory, bool any_version = false);
bool machine_layer_installed(const std::wstring &directory, bool any_version,
                             const LayerRegistryRoots &roots);
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const wchar_t *registry = layer_registry_key);
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const LayerRegistryRoots &roots);
// Return settings only if the first enabled usable DLL is compatible: current builds use SteadyXR.ini,
// while the exact trusted legacy capture build still reads its adjacent preview.ini.
std::optional<std::wstring> compatible_layer_settings_path(const std::wstring &directory,
                                                          const wchar_t *registry = layer_registry_key);
std::optional<std::wstring> compatible_layer_settings_path(const std::wstring &directory,
                                                          const LayerRegistryRoots &roots);
LONG set_layer_connection(const std::wstring &directory, bool enable,
                          const wchar_t *registry = layer_registry_key);
LONG set_layer_connection(const std::wstring &directory, bool enable, const LayerRegistryRoots &roots);
} // namespace vrst::preview
