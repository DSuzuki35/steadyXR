#include "monitor_widget.hpp"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>

using namespace vrst::preview;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
RECT bounds(HWND w) {
    RECT r{};
    GetWindowRect(w, &r);
    MapWindowPoints(nullptr, GetParent(w), reinterpret_cast<POINT *>(&r), 2);
    return r;
}
void drag(HWND w, POINT from, int dx, int dy) {
    POINT end{from.x + dx, from.y + dy};
    ClientToScreen(w, &end);
    SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    POINT at = end;
    ScreenToClient(w, &at);
    SendMessageW(w, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(at.x, at.y));
    at = end;
    ScreenToClient(w, &at);
    SendMessageW(w, WM_LBUTTONUP, 0, MAKELPARAM(at.x, at.y));
}
} // namespace
int main() {
    const auto ini = std::filesystem::temp_directory_path() /
                     (L"SteadyXR-widget-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    try {
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&init);
        Theme theme;
        theme.initialize();
        const auto parent = CreateWindowW(L"STATIC", L"", WS_OVERLAPPED | WS_CLIPCHILDREN, 0, 0, 1280, 800,
                                          nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(parent != nullptr, "create dashboard");
        const auto sibling =
            CreateWindowW(L"BUTTON", L"Underlying control", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 216, 75,
                          410, 310, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(sibling != nullptr, "create overlapping dashboard control");
        MonitorWidget widget;
        int paints{};
        RECT canvas{};
        check(widget.create(parent, theme, ini,
                            [&](HDC dc, RECT r) {
                                ++paints;
                                canvas = r;
                                FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                            }),
              "create embedded widget");
        const auto w = widget.window();
        const RECT home{844, 104, 1208, 404};
        auto r = bounds(w);
        check(EqualRect(&r, &home) && GetParent(w) == parent && (GetWindowLongPtr(w, GWL_STYLE) & WS_CHILD),
              "default right rail child position");
        theme.scale_window(parent, 96);
        const auto dc = GetDC(w);
        SendMessageW(w, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        ReleaseDC(w, dc);
        check(paints && canvas.right - canvas.left == 324 && canvas.bottom - canvas.top == 188,
              "default widget draws original canvas dimensions");
        drag(w, {180, 130}, -200, 35);
        r = bounds(w);
        check(EqualRect(&r, &home) && !std::filesystem::exists(ini), "canvas does not drag or save layout");
        drag(w, {11, 288}, -192, 70);
        r = bounds(w);
        check(r.left == home.left - 192 && r.bottom == home.bottom + 70 && r.right == home.right &&
                  r.top == home.top,
              "bottom-left grip expands the default card inward while keeping its upper-right corner fixed");
        drag(w, {11, 358}, 1800, -800);
        r = bounds(w);
        check(r.left == home.right - 240 && r.bottom == home.top + 148 && r.right == home.right &&
                  r.top == home.top,
              "minimum size preserves the upper-right anchor");
        widget.reset();
        drag(w, {65, 23}, -900, -100);
        r = bounds(w);
        check(r.left == 216 && r.top == 75 && r.right == 580 && r.bottom == 375,
              "header moves widget within logical page bounds");
        check(GetWindow(w, GW_HWNDPREV) == nullptr, "moved widget stays above other dashboard content");
        drag(w, {359, 295}, 150, 90);
        r = bounds(w);
        check(r.left == 216 && r.top == 75 && r.right == 730 && r.bottom == 465,
              "bottom-right grip changes size without moving the header");
        theme.scale_window(parent, 144);
        r = bounds(w);
        check(r.left == 324 && r.top == 113 && r.right == 1095 && r.bottom == 698,
              "theme DPI replay scales custom placement rather than restoring the original rail bounds");
        RECT small{0, 0, 720, 480};
        theme.fit_window(parent, small);
        SetScrollPos(parent, SB_HORZ, 120, TRUE);
        SetScrollPos(parent, SB_VERT, 60, TRUE);
        LRESULT result{};
        theme.message(parent, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 0), 0, result);
        const auto scrolled = bounds(w);
        check(scrolled.left == 324 - GetScrollPos(parent, SB_HORZ) &&
                  scrolled.top == 113 - GetScrollPos(parent, SB_VERT),
              "widget follows the scrollable workspace origin");
        theme.scale_window(parent, 96);
        // Launch another independent dashboard using the same UI preference.
        const auto restored_parent = CreateWindowW(L"STATIC", L"", WS_OVERLAPPED, 0, 0, 1280, 800, nullptr,
                                                   nullptr, GetModuleHandleW(nullptr), nullptr);
        MonitorWidget restored;
        check(restored.create(restored_parent, theme, ini, {}), "open saved layout");
        auto persisted = bounds(restored.window());
        check(persisted.left == 216 && persisted.top == 75 && persisted.right == 730 &&
                  persisted.bottom == 465,
              "move and size are persisted in logical dashboard coordinates");
        drag(restored.window(), {510, 386}, 3000, 3000);
        persisted = bounds(restored.window());
        check(persisted.right == 1216 && persisted.bottom == 616, "expanded widget stays in workspace");
        SendMessageW(GetDlgItem(restored.window(), 1), BM_CLICK, 0, 0);
        persisted = bounds(restored.window());
        check(EqualRect(&persisted, &home), "visible reset button restores the right rail");
        drag(restored.window(), {360, 296}, -900, -900);
        persisted = bounds(restored.window());
        check(persisted.right - persisted.left == 240 && persisted.bottom - persisted.top == 148,
              "minimum size keeps the title, reset and resize grip reachable");
        SendMessageW(restored.window(), WM_LBUTTONDBLCLK, 0, MAKELPARAM(25, 25));
        persisted = bounds(restored.window());
        check(EqualRect(&persisted, &home), "double-click header restores default bounds");
        DestroyWindow(restored_parent);
        DestroyWindow(parent);
        DeleteFileW(ini.c_str());
        std::cout << "Embedded move, resize, overlay, DPI, scroll, restore and reset passed\n";
        return 0;
    } catch (const std::exception &e) {
        DeleteFileW(ini.c_str());
        std::cerr << e.what() << '\n';
        return 1;
    }
}
