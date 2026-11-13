#include "output_fallback_settings.hpp"
#include "theme.hpp"
#include <commctrl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace vrst::preview;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
} // namespace

int main() try {
    const auto name = L"SteadyXRFallbackSettingsTest" + std::to_wstring(GetCurrentProcessId());
    const auto ini = (std::filesystem::temp_directory_path() / (name + L".ini")).wstring();
    const auto defaults = read_output_fallback_settings(ini);
    check(defaults.mode == FallbackMode::waiting && !defaults.cockpit_only && defaults.monitor_name.empty(),
          "legacy settings retain waiting screen, all OpenXR frames, and primary monitor");
    const OutputFallbackSettings saved{FallbackMode::monitor, true, LR"(\\.\DISPLAY9999)"};
    check(write_output_fallback_settings(ini, saved), "write settings");
    const auto current = read_output_fallback_settings(ini);
    check(current.mode == saved.mode && current.cockpit_only && current.monitor_name == saved.monitor_name,
          "round-trip includes an offline monitor");
    WritePrivateProfileStringW(L"OutputFallback", L"Mode", L"invalid", ini.c_str());
    check(read_output_fallback_settings(ini).mode == FallbackMode::waiting,
          "unknown mode uses waiting screen");
    check(write_output_fallback_settings(ini, {FallbackMode::hide}), "save hide mode");
    check(read_output_fallback_settings(ini).mode == FallbackMode::hide &&
              read_output_fallback_settings(ini).monitor_name.empty(),
          "save primary and hide mode");
    std::filesystem::remove(ini);

    auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    check(desktop && SetThreadDesktop(desktop), "isolated UI desktop");
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HWND owner = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    check(owner != nullptr, "create owner");
    Theme theme;
    theme.initialize();
    OutputFallbackDialog dialog;
    OutputFallbackSettings result{};
    unsigned applies{};
    const auto applied = [&](OutputFallbackSettings value) {
        result = value;
        ++applies;
    };
    dialog.open(owner, theme, saved, applied);
    auto window = FindWindowW(L"SteadyXROutputFallback", nullptr);
    auto mode = FindWindowExW(window, nullptr, L"COMBOBOX", nullptr);
    auto monitor = FindWindowExW(window, mode, L"COMBOBOX", nullptr);
    check(window && mode && monitor && IsWindowVisible(window) && IsWindowEnabled(owner), "modeless dialog");
    check(SendMessageW(mode, CB_GETCOUNT, 0, 0) == 3 && IsWindowEnabled(monitor) &&
              SendMessageW(monitor, CB_GETCURSEL, 0, 0) > 0,
          "offline monitor remains selected");
    const auto cockpit = FindWindowExW(window, nullptr, L"BUTTON",
                                       L"Use fallback outside the car in iRacing and Le Mans Ultimate");
    check(cockpit && SendMessageW(cockpit, BM_GETCHECK, 0, 0) == BST_CHECKED, "load cockpit preference");
    for (UINT dpi : {96u, 144u, 192u}) {
        theme.scale_window(window, dpi);
        for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t kind[32]{}, title[256]{};
            GetClassNameW(child, kind, 32);
            if (_wcsicmp(kind, L"STATIC") && child != cockpit)
                continue;
            GetWindowTextW(child, title, 256);
            RECT bounds{};
            GetClientRect(child, &bounds);
            RECT text_rect = bounds;
            if (child == cockpit)
                text_rect.left += MulDiv(30, dpi, 96);
            auto dc = GetDC(child);
            auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0)));
            DrawTextW(dc, title, -1, &text_rect,
                      DT_CALCRECT | DT_NOPREFIX | (child == cockpit ? DT_SINGLELINE : DT_WORDBREAK));
            SelectObject(dc, old);
            ReleaseDC(child, dc);
            check(text_rect.bottom <= bounds.bottom && text_rect.right <= bounds.right,
                  "fallback dialog text fits at 100%, 150% and 200% DPI");
        }
    }
    SendMessageW(window, WM_DISPLAYCHANGE, 0, 0);
    SendMessageW(GetDlgItem(window, IDOK), BM_CLICK, 0, 0);
    check(applies == 1 && result.mode == saved.mode && result.cockpit_only &&
              result.monitor_name == saved.monitor_name,
          "Apply preserves explicit offline selection");
    dialog.open(owner, theme, defaults, applied);
    check(!IsWindowEnabled(monitor), "waiting mode disables monitor selection");
    SendMessageW(mode, CB_SETCURSEL, 2, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(mode), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(mode));
    check(IsWindowEnabled(monitor) && SendMessageW(monitor, CB_GETCURSEL, 0, 0) == 0,
          "clone defaults to primary");
    SendMessageW(window, WM_CLOSE, 0, 0);
    check(applies == 1 && !IsWindowVisible(window), "closing discards edits");
    DestroyWindow(owner);
    std::cout << "Output fallback settings passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
