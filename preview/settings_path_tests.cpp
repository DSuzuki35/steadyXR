#include "settings_path.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace vrst::preview;
int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto directory =
        std::filesystem::path(temp) / (L"SteadyXR-settings-test-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(directory))
        return 1;
    const auto legacy = directory / L"preview.ini", current = directory / L"SteadyXR.ini";
    const std::string original =
        "\xEF\xBB\xBF[Preview]\r\nFPS=90\r\nUnknownFutureKey=keep me\r\n[Render]\r\nEnabled=1\r\n";
    auto bytes = [](const auto &path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    std::ofstream(legacy, std::ios::binary) << original;
    bool okay = capture_settings_path(directory.wstring()) == legacy.wstring();
    DWORD error{};
    okay = migrate_preview_settings(directory.wstring(), error) && error == ERROR_SUCCESS && okay;
    okay = bytes(current) == original && bytes(legacy) == original && okay;
    okay = capture_settings_path(directory.wstring()) == current.wstring() && okay;
    std::cout << "legacy settings copied byte-for-byte, including unknown keys and BOM: " << okay << '\n';
    std::ofstream(current, std::ios::binary | std::ios::trunc) << "[Preview]\r\nFPS=60\r\n";
    const auto chosen = bytes(current);
    const bool preserved = migrate_preview_settings(directory.wstring(), error) && bytes(current) == chosen;
    std::cout << "existing SteadyXR settings take precedence: " << preserved << '\n';
    okay = preserved && okay;
    std::filesystem::remove(current);
    std::filesystem::create_directory(current);
    const bool refuses = !migrate_preview_settings(directory.wstring(), error) && error != ERROR_SUCCESS;
    std::cout << "invalid destination reports failure without changing legacy settings: " << refuses << '\n';
    okay = refuses && bytes(legacy) == original && okay;
    std::filesystem::remove(current);
    std::filesystem::remove(legacy);
    const bool fresh = migrate_preview_settings(directory.wstring(), error) && error == ERROR_SUCCESS &&
                       !std::filesystem::exists(current);
    std::cout << "fresh install needs no fabricated settings file: " << fresh << '\n';
    okay = fresh && okay;
    std::filesystem::remove(directory);
    return okay ? 0 : 1;
}
