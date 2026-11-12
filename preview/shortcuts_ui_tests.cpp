#include "shortcut_bindings.hpp"
#include "theme.hpp"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>

using namespace vrst::preview;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
HWND find_child(HWND parent, const wchar_t *kind, const wchar_t *name) {
    return FindWindowExW(parent, nullptr, kind, name);
}
} // namespace

int main() try {
    // Exercise focus and tab navigation without showing the test window on the user's desktop.
    const auto desktop_name = L"SteadyXRShortcutTests" + std::to_wstring(GetCurrentProcessId());
    auto desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    check(desktop && SetThreadDesktop(desktop), "create isolated UI desktop");
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    const auto dir = std::filesystem::temp_directory_path() / desktop_name;
    std::filesystem::create_directories(dir);
    const auto file = dir / L"SteadyXR-shortcuts.ini";
    check(WritePrivateProfileStringW(L"Joystick", L"PauseResume",
                                     L"{12345678-1234-1234-0102-030405060708},13", file.c_str()),
          "create offline binding fixture");
    auto owner = CreateWindowW(L"STATIC", L"Shortcut test owner", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(owner != nullptr, "create owner");
    Theme theme;
    theme.initialize();
    ShortcutController controller;
    unsigned triggers = 0;
    controller.initialize(owner, theme, dir / L"preview.ini", [&](ShortcutAction) { ++triggers; });
    controller.open();
    const auto dialog = FindWindowW(L"SteadyXRJoystickShortcuts", nullptr);
    const auto actions = find_child(dialog, L"COMBOBOX", nullptr);
    const auto clear = find_child(dialog, L"BUTTON", L"Clear assignment");
    check(dialog && IsWindowVisible(dialog) && actions && clear, "open shortcut controls");
    check(SendMessageW(actions, CB_GETCOUNT, 0, 0) == 6, "all six actions must be offered");
    const auto pause =
        SendMessageW(actions, CB_FINDSTRINGEXACT, -1, reinterpret_cast<LPARAM>(L"Pause / resume"));
    check(pause != CB_ERR, "select pause action");
    SendMessageW(actions, CB_SETCURSEL, pause, 0);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(actions), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(actions));
    check(IsWindowEnabled(clear), "offline assignment can be cleared");
    SetFocus(actions);
    MSG msg{};
    msg.hwnd = actions;
    msg.message = WM_KEYDOWN;
    msg.wParam = VK_TAB;
    check(controller.route(msg) && GetFocus() != actions, "tab reaches the next control");
    controller.tick();
    SendMessageW(clear, BM_CLICK, 0, 0);
    check(!IsWindowEnabled(clear) && !ShortcutBindings(file).assignment(ShortcutAction::pause_resume),
          "clear button updates dialog and saved binding");
    SendMessageW(dialog, WM_CLOSE, 0, 0);
    check(!IsWindowVisible(dialog) && !controller.route(msg), "closed dialog releases keyboard navigation");
    controller.open();
    check(IsWindowVisible(dialog) && !IsWindowEnabled(clear), "reopen preserves selection and clear state");
    check(triggers == 0, "editing offline controls must never dispatch an action");
    DestroyWindow(owner);
    std::filesystem::remove(file);
    std::filesystem::remove(dir);
    std::cout << "Joystick shortcut UI passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
