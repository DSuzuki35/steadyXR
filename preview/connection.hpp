#pragma once
#include <optional>
#include <string>
#include <windows.h>

namespace vrst::preview {
inline constexpr wchar_t layer_registry_key[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
bool layer_installed(const std::wstring &directory, bool any_version = false,
                     const wchar_t *registry = layer_registry_key);
std::optional<std::wstring> compatible_layer_directory(const std::wstring &directory,
                                                       const wchar_t *registry = layer_registry_key);
LONG set_layer_connection(const std::wstring &directory, bool enable,
                          const wchar_t *registry = layer_registry_key);
} // namespace vrst::preview
