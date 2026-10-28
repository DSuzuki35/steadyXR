#include "advanced.hpp"
#include <commctrl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace vrst::preview;
namespace {
void check(bool okay, const char *message) {
    if (!okay) throw std::runtime_error(message);
}
void command(HWND panel, int id) { SendMessageW(panel, WM_COMMAND, id, 0); }
void slider(HWND panel, int id, int value) {
    auto control = GetDlgItem(panel, id);
    SendMessageW(control, TBM_SETPOS, TRUE, value);
    SendMessageW(panel, WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(control));
}
void toggle(HWND panel, bool enabled) {
    SendMessageW(GetDlgItem(panel, AdvancedSettings::symmetric_id), BM_SETCHECK,
                 enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    command(panel, AdvancedSettings::symmetric_id);
}
void expect(const RenderSettings &s, double ll, double lr, double rl, double rr) {
    check(s.eyes[0].left == ll && s.eyes[0].right == lr &&
          s.eyes[1].left == rl && s.eyes[1].right == rr, "wrong per-eye render amounts");
}
}
int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto folder = std::filesystem::path(temp) /
        (L"SteadyXR-symmetry-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    const auto ini = (folder / L"settings.ini").wstring();
    int result = 0;
    try {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        Theme theme;
        theme.initialize();
        Options options;
        RenderSettings original;
        original.enabled = true;
        original.eyes[0] = {1.2, 10.0};
        original.eyes[1] = {9.8, 2.3};
        check(write_render_settings(ini, original), "write original settings");
        AdvancedSettings ui;
        ui.initialize(nullptr, theme, options, ini, [] {}, [](std::wstring) {});
        ui.open(1, false);
        auto panel = ui.handle();
        check(IsWindow(GetDlgItem(panel, AdvancedSettings::symmetric_id)), "Symmetric checkbox missing");
        expect(ui.draft(), 1.2, 10, 9.8, 2.3); // Old files retain asymmetric settings.
        toggle(panel, true);
        expect(ui.draft(), 2.3, 9.8, 9.8, 2.3); // Selected right eye wins on enable.
        slider(panel, AdvancedSettings::render_left_id, 157);
        slider(panel, AdvancedSettings::render_right_id, 41);
        expect(ui.draft(), 4.1, 15.7, 15.7, 4.1);
        check(read_render_settings(ini).eyes[0].right == 10, "draft unexpectedly saved");
        ui.open(0, false);
        slider(panel, AdvancedSettings::render_right_id, 300);
        slider(panel, AdvancedSettings::render_left_id, 0);
        expect(ui.draft(), 0, 30, 30, 0); // Left eye edits mirror in the other direction.
        command(panel, AdvancedSettings::save_render_id);
        expect(read_render_settings(ini), 0, 30, 30, 0); // Old signed DLL reads these ordinary keys.
        check(GetPrivateProfileIntW(L"Render", L"Symmetric", 0, ini.c_str()) == 1,
              "Symmetric preference not persisted");
        AdvancedSettings restored;
        restored.initialize(nullptr, theme, options, ini, [] {}, [](std::wstring) {});
        restored.open(1, false);
        auto second = restored.handle();
        check(SendMessageW(GetDlgItem(second, AdvancedSettings::symmetric_id), BM_GETCHECK, 0, 0) == BST_CHECKED,
              "Symmetric preference not restored");
        toggle(second, false);
        slider(second, AdvancedSettings::render_left_id, 80);
        expect(restored.draft(), 0, 30, 8, 0); // Unlock must leave the other eye alone.
        command(second, AdvancedSettings::save_render_id);
        check(GetPrivateProfileIntW(L"Render", L"Symmetric", 1, ini.c_str()) == 0,
              "unchecked preference not persisted");
        command(second, AdvancedSettings::inward_id);
        expect(restored.draft(), 0, 10, 10, 0);
        slider(second, AdvancedSettings::render_left_id, 123);
        expect(restored.draft(), 0, 12.3, 12.3, 0); // Inward preset also locks the pair.
        command(second, AdvancedSettings::save_render_id);
        // A complete preset (INI copy) must retain the lock and mirror direction.
        const auto preset = (folder / L"preset.ini").wstring();
        std::filesystem::copy_file(ini, preset);
        ui.reload(preset);
        ui.open(0, false);
        slider(panel, AdvancedSettings::render_left_id, 51);
        expect(ui.draft(), 5.1, 12.3, 12.3, 5.1);
        RECT client{};
        GetClientRect(panel, &client);
        for (auto child = GetWindow(panel, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT r{};
            GetWindowRect(child, &r);
            MapWindowPoints(nullptr, panel, reinterpret_cast<POINT *>(&r), 2);
            check(r.left >= 0 && r.top >= 0 && r.right <= client.right && r.bottom <= client.bottom,
                  "render control clipped outside dialog");
        }
        std::cout << "PASS: mirror linkage, both eyes, unlock, save/reopen, presets and layout\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(folder);
    return result;
}
