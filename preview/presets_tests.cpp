#include "presets.hpp"
#include <algorithm>
#include <commctrl.h>
#include <fstream>
#include <iostream>

using namespace vrst::preview;
namespace {
int failures{};
void check(bool condition, const char *description) {
    std::cout << (condition ? "PASS " : "FAIL ") << description << '\n';
    failures += !condition;
}
void put(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), std::streamsize(bytes.size()));
}
std::string get(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace
int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root =
        std::filesystem::path(temp) / (L"HorizonLock-presets-" + std::to_wstring(GetCurrentProcessId()) +
                                       L"-" + std::to_wstring(GetTickCount64()));
    const auto directory = root / L"Presets";
    std::filesystem::create_directory(root);
    PresetStore store(directory);
    std::wstring error;
    check(store.list(error).empty() && error.empty() && !std::filesystem::exists(directory),
          "opening an empty store does not create files");

    // Removing name validation would permit traversal or Windows device aliases.
    for (const auto &name : {L"",          L".",      L"..",       L"../escape", L"sub\\name", L"C:name",
                             L"a?b",       L"a*b",    L"a\"b",     L"a<b",       L"a>b",       L"a|b",
                             L"trail.",    L"trail ", L"CON",      L"con.ini",   L"AUX",       L"PRN",
                             L"NUL",       L"COM1",   L"com9.cfg", L"LPT1",      L"LPT9.txt",  L"COM\u00b9",
                             L"LPT\u00b2", L"CONIN$", L"CONOUT$",  L"CON .txt",  L"bad\nname", L"bad\tname"})
        check(!PresetStore::valid_name(name, error) && !error.empty(), "unsafe preset name rejected");
    check(!PresetStore::valid_name(std::wstring(65, L'x'), error), "preset name length bounded");
    for (const auto &name : {L"My race", L"Nordschleife 2", L"Gr\u00fcne H\u00f6lle", L"\u65e5\u672c",
                             L"COM10", L"Track.day", L"AUXiliary"})
        check(PresetStore::valid_name(name, error) && error.empty(), "friendly portable name accepted");

    const std::string snapshot =
        "; complete configuration\r\n[Preview]\r\nSmoothing=71\r\nCaptureSource=window\r\n"
        "[LeftEyeReserve]\r\nLeftTenths=31\r\n[RightEyeReserve]\r\nEnabled=1\r\n"
        "[Render]\r\nEnabled=1\r\nLeftEyeRightTenths=120\r\n[FutureVersion]\r\nUnknownKey=keep me\r\n";
    const std::string alternate = "[Preview]\nSmoothing=12\n[Render]\nEnabled=0\n";
    check(PresetStore::valid_snapshot(snapshot, error), "full INI snapshot accepted");
    check(PresetStore::valid_snapshot("\xef\xbb\xbf  [preview] \r\nFuture=retained\r\n", error),
          "UTF8 BOM and case-insensitive INI section accepted");
    check(!PresetStore::valid_snapshot("[Other]\nKey=2\n", error), "missing Preview section rejected");
    check(!PresetStore::valid_snapshot("value=[Preview]\n", error),
          "Preview text in a value is not a section");
    check(!PresetStore::valid_snapshot(std::string(262145, 'x') + "\n[Preview]\n", error),
          "oversized snapshot rejected");
    check(!PresetStore::valid_snapshot(std::string("[Preview]\n\0bad", 15), error),
          "binary snapshot rejected");
    const std::string utf16 =
        std::string("\xff\xfe", 2) + std::string("[\0P\0r\0e\0v\0i\0e\0w\0]\0\r\0\n\0K\0=\0"
                                                 "1\0\r\0\n\0",
                                                 32);
    check(PresetStore::valid_snapshot(utf16, error), "UTF16 Windows INI snapshot accepted");
    check(store.save(L"Gr\u00fcne H\u00f6lle", snapshot, false, error), "save creates new named preset");
    const auto restored = store.read(L"Gr\u00fcne H\u00f6lle", error);
    check(restored && *restored == snapshot, "round trip preserves Render and unknown keys byte for byte");
    check(!store.save(L"Gr\u00fcne H\u00f6lle", alternate, false, error) && !error.empty(),
          "save new cannot overwrite an existing preset");
    check(get(directory / L"Gr\u00fcne H\u00f6lle.ini") == snapshot,
          "rejected duplicate leaves original unchanged");
    check(store.save(L"Gr\u00fcne H\u00f6lle", alternate, true, error) &&
              get(directory / L"Gr\u00fcne H\u00f6lle.ini") == alternate,
          "explicit overwrite replaces complete snapshot");
    check(!store.save(L"Missing", snapshot, true, error), "overwrite requires an existing preset");
    check(!store.save(L"../Escape", snapshot, false, error) && !std::filesystem::exists(root / L"Escape.ini"),
          "save cannot escape the preset folder");
    check(!store.save(L"Malformed", "not an INI", false, error) &&
              !std::filesystem::exists(directory / L"Malformed.ini"),
          "malformed save creates no preset");
    check(store.save(L"Unicode INI", utf16, false, error) && get(directory / L"Unicode INI.ini") == utf16,
          "Unicode INI retains original encoding");
    check(store.save(L"Readonly", snapshot, false, error), "readonly fixture saved");
    const auto readonly = directory / L"Readonly.ini";
    SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY);
    check(!store.save(L"Readonly", alternate, true, error) && !error.empty() && get(readonly) == snapshot,
          "failed overwrite preserves the existing complete snapshot");
    check(!store.remove(L"Readonly", error) && !error.empty(), "failed delete reports its error");
    SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_NORMAL);
    const auto locked = CreateFileW(readonly.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE && !store.save(L"Readonly", alternate, true, error) &&
              !error.empty() && get(readonly) == snapshot,
          "failed atomic replacement leaves the original snapshot intact");
    if (locked != INVALID_HANDLE_VALUE)
        CloseHandle(locked);
    bool no_temporary_files = true;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        no_temporary_files = no_temporary_files && entry.path().extension() != L".tmp";
    check(no_temporary_files, "failed atomic replacement cleans its temporary snapshot");

    std::filesystem::create_directories(directory);
    put(directory / L"Broken.ini", "[Other]\nData=bad\n");
    put(directory / L"Ignored.tmp", snapshot);
    std::filesystem::create_directory(directory / L"Directory.ini");
    check(!store.read(L"Broken", error) && !error.empty(), "malformed preset cannot be applied");
    check(!store.read(L"Missing", error) && !error.empty(), "missing preset does not return empty success");
    const auto names = store.list(error);
    check(error.empty() && names.size() == 4 &&
              std::find(names.begin(), names.end(), L"Gr\u00fcne H\u00f6lle") != names.end() &&
              std::find(names.begin(), names.end(), L"Directory") == names.end(),
          "listing includes named INI files only");
    check(store.remove(L"Gr\u00fcne H\u00f6lle", error) &&
              !std::filesystem::exists(directory / L"Gr\u00fcne H\u00f6lle.ini"),
          "delete removes the selected saved preset");
    check(!store.remove(L"Missing", error) && !error.empty(), "deleting missing preset reports failure");
    put(root / L"NotDirectory", "blocked");
    PresetStore blocked(root / L"NotDirectory");
    check(!blocked.save(L"Race", snapshot, false, error) && !error.empty(),
          "unwritable preset folder reports failure");
    check(blocked.list(error).empty() && !error.empty(),
          "inaccessible list is not reported as empty success");

    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    Theme theme;
    theme.initialize();
    const auto parent = CreateWindowExW(0, L"STATIC", L"Presets test owner", WS_OVERLAPPED, 0, 0, 800, 600,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        PresetsDialog dialog;
        std::string current = snapshot, applied;
        bool apply_allowed = true;
        std::optional<BuiltinPreset> builtin;
        dialog.initialize(parent, theme, directory,
                          {[&] { return current; },
                           [&](const std::string &bytes) {
                               if (apply_allowed)
                                   applied = bytes;
                               return apply_allowed;
                           },
                           [&](BuiltinPreset value) { builtin = value; }});
        dialog.open(false);
        const auto window = dialog.handle();
        check(window && !IsWindowVisible(window), "preset dialog can initialize without showing a window");
        if (window) {
            const auto command = [&](int id) {
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
            };
            const auto edit = GetDlgItem(window, PresetsDialog::name_id);
            SetWindowTextW(edit, L"Dialog race");
            command(PresetsDialog::save_id);
            check(get(directory / L"Dialog race.ini") == snapshot,
                  "Save new stores the full current configuration");
            current = alternate;
            command(PresetsDialog::save_id);
            check(get(directory / L"Dialog race.ini") == snapshot, "Save new in the dialog cannot overwrite");
            command(PresetsDialog::overwrite_id);
            check(get(directory / L"Dialog race.ini") == alternate,
                  "Overwrite selected stores current configuration");
            command(PresetsDialog::load_id);
            check(applied == alternate, "Load applies selected complete snapshot");
            apply_allowed = false;
            applied.clear();
            command(PresetsDialog::load_id);
            wchar_t failure_status[512]{};
            GetWindowTextW(GetDlgItem(window, PresetsDialog::status_id), failure_status, 512);
            check(applied.empty() && std::wstring(failure_status).find(L"Could not") != std::wstring::npos,
                  "failed apply is reported instead of a successful load");
            apply_allowed = true;
            for (const auto &entry : {std::pair{PresetsDialog::natural_id, BuiltinPreset::natural},
                                      std::pair{PresetsDialog::comfort_id, BuiltinPreset::stream_comfort},
                                      std::pair{PresetsDialog::strong_id, BuiltinPreset::strong_leveling}}) {
                command(entry.first);
                check(builtin == entry.second, "built-in selection invokes the corresponding tuning preset");
            }
            command(PresetsDialog::delete_id);
            check(!std::filesystem::exists(directory / L"Dialog race.ini"),
                  "Delete selected removes that custom preset");
            SetWindowTextW(edit, L"../Outside");
            command(PresetsDialog::save_id);
            check(!std::filesystem::exists(root / L"Outside.ini"), "dialog rejects a path as a preset name");
            wchar_t status[512]{};
            GetWindowTextW(GetDlgItem(window, PresetsDialog::status_id), status, 512);
            check(*status != 0, "dialog exposes operation feedback");
        }
    }
    DestroyWindow(parent);

    // Only this test's directly created children are removed; there is no recursive cleanup.
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        std::filesystem::remove(entry.path());
    std::filesystem::remove(directory);
    std::filesystem::remove(root / L"NotDirectory");
    std::filesystem::remove(root);
    return failures ? 1 : 0;
}
