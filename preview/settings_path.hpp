#pragma once
#include <string>
#include <windows.h>

namespace vrst::preview {
inline std::wstring capture_settings_path(const std::wstring &directory) {
    const auto current = directory + L"\\SteadyXR.ini";
    const auto legacy = directory + L"\\preview.ini";
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return current;
    const auto old_attributes = GetFileAttributesW(legacy.c_str());
    return old_attributes != INVALID_FILE_ATTRIBUTES && !(old_attributes & FILE_ATTRIBUTE_DIRECTORY)
               ? legacy
               : current;
}
// Copy unchanged bytes once; keep legacy settings available to an older registered layer.
// The capture layer only calls the read-only path selector above, never this migration.
inline bool migrate_preview_settings(const std::wstring &directory, DWORD &error) {
    error = ERROR_SUCCESS;
    const auto current = directory + L"\\SteadyXR.ini";
    const auto legacy = directory + L"\\preview.ini";
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            error = ERROR_DIRECTORY;
            return false;
        }
        return true;
    }
    error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        return false;
    const auto old_attributes = GetFileAttributesW(legacy.c_str());
    if (old_attributes == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return false;
        error = ERROR_SUCCESS;
        return true;
    }
    if (old_attributes & FILE_ATTRIBUTE_DIRECTORY) {
        error = ERROR_DIRECTORY;
        return false;
    }
    if (CopyFileW(legacy.c_str(), current.c_str(), TRUE)) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    return false;
}
} // namespace vrst::preview
