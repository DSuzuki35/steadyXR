#include "framing_editor.hpp"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>

using namespace vrst::preview;
namespace {
void check(bool ok, const char *reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
HWND editor_window(HWND owner) {
    struct Lookup {
        HWND owner, found;
    } lookup{owner};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND w, LPARAM l) -> BOOL {
            auto &lookup = *reinterpret_cast<Lookup *>(l);
            wchar_t window_class[64]{};
            GetClassNameW(w, window_class, 64);
            if (GetWindow(w, GW_OWNER) == lookup.owner &&
                !std::wcscmp(window_class, L"SteadyXRFramingEditor")) {
                lookup.found = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&lookup));
    return lookup.found;
}
COLORREF card_pixel(const FramingEditor &editor, const Options &options, double aspect, int eye, LONG x,
                    LONG y) {
    const auto screen = GetDC(nullptr);
    const auto dc = CreateCompatibleDC(screen);
    const auto image = CreateCompatibleBitmap(screen, 300, 300);
    const auto previous = SelectObject(dc, image);
    editor.paint_card(dc, {0, 0, 300, 300}, options, aspect, eye);
    const auto color = GetPixel(dc, x, y);
    SelectObject(dc, previous);
    DeleteObject(image);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return color;
}
} // namespace
int main() {
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        InitCommonControlsEx(&controls);
        const auto owner = CreateWindowW(L"STATIC", L"", WS_OVERLAPPED, 0, 0, 400, 400, nullptr, nullptr,
                                         GetModuleHandleW(nullptr), nullptr);
        check(owner != nullptr, "create owner");
        Theme theme;
        theme.initialize();
        Renderer renderer;
        check(renderer.attach_offscreen(), "create renderer");
        Options options;
        bool applied{};
        FramingEditor framing;
        framing.initialize(
            owner, theme, renderer, [&] { return options; },
            [&](const Options &fresh) {
                options = fresh;
                applied = true;
            });
        framing.capture();
        check(framing.status().find(L"No live image") != std::wstring::npos, "missing source feedback");
        check(renderer.demo(1), "provide a selected eye");
        Options geometry;
        geometry.aspect = 1;
        geometry.crop = 20;
        geometry.horizontal_offset = -100;
        const auto left_border = card_pixel(framing, geometry, 1., 0, 6, 150);
        geometry.horizontal_offset = 100;
        const auto shifted = card_pixel(framing, geometry, 1., 0, 6, 150);
        check(GetBValue(left_border) > GetRValue(left_border) + 100 && shifted != left_border,
              "schematic visibly moves the output rectangle to the selected source boundary");
        check(!framing.has_still() && framing.still_revision() == 0 && framing.still_description().empty(),
              "painting a framing card never captures a screenshot");
        framing.capture();
        renderer.source_changed();
        const auto deadline = GetTickCount64() + 5000;
        while (framing.status().find(L"Capturing") == 0 && GetTickCount64() < deadline) {
            framing.tick();
            Sleep(1);
        }
        check(framing.status().find(L"Screenshot captured") != std::wstring::npos,
              "background screenshot completed");
        check(!editor_window(owner), "shortcut leaves editor closed");
        const auto card_focus = card_pixel(framing, options, 2.5, 1, 150, 150);
        check(framing.has_still() && framing.still_revision() == 1 &&
                  framing.still_description() == L"Left eye · Peripheral + focus" &&
                  GetGValue(card_focus) > 200 && GetRValue(card_focus) < 150,
              "card preserves captured optical image and eye metadata after the selected source changes");
        framing.open();
        const auto window = editor_window(owner);
        check(window && IsWindowVisible(window), "open a frozen reference after producer exit");
        const auto canvas = FindWindowExW(window, nullptr, L"SteadyXRFramingCanvas", nullptr);
        RECT bounds{};
        GetClientRect(canvas, &bounds);
        const auto target = GetDC(canvas);
        const auto dc = CreateCompatibleDC(target);
        const auto image = CreateCompatibleBitmap(target, bounds.right, bounds.bottom);
        const auto previous = SelectObject(dc, image);
        SendMessageW(canvas, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        const auto focus = GetPixel(dc, bounds.right / 2, bounds.bottom / 2);
        SelectObject(dc, previous);
        DeleteObject(image);
        DeleteDC(dc);
        ReleaseDC(canvas, target);
        check(focus != CLR_INVALID && GetGValue(focus) > 200 && GetRValue(focus) < 150,
              "editor canvas displays the composited focus at the center of the screenshot");
        const auto slider = GetDlgItem(window, 72);
        check(IsWindowEnabled(slider), "frozen screenshot remains editable");
        SendMessageW(slider, TBM_SETPOS, TRUE, -750);
        SendMessageW(window, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(slider));
        check(!applied && options.vertical_offset == 0, "slider updates draft");
        options.smoothing = 98;
        SendMessageW(window, WM_COMMAND, 61, 0);
        check(applied && options.vertical_offset == -75 && options.smoothing == 98,
              "apply edits framing and preserves latest unrelated live setting");
        SendMessageW(window, WM_CLOSE, 0, 0);
        framing.open();
        check(SendMessageW(slider, TBM_GETPOS, 0, 0) == -750, "reopen stored still and applied framing");
        SendMessageW(window, WM_COMMAND, 60, 0);
        check(framing.status().find(L"No live image") != std::wstring::npos && IsWindowEnabled(slider),
              "failed recapture retains the previous editable reference");
        check(framing.still_revision() == 1, "failed recapture keeps the displayed revision");
        options.eye = 1;
        check(renderer.demo(2, false, 1), "provide right single eye");
        framing.capture();
        const auto replace_deadline = GetTickCount64() + 5000;
        while (framing.still_revision() == 1 && GetTickCount64() < replace_deadline) {
            framing.tick();
            Sleep(1);
        }
        check(framing.still_revision() == 2 && framing.still_description() == L"Right eye",
              "new screenshot publishes its revision and captured eye together");
        check(card_pixel(framing, options, 1, 1, 150, 150) != card_focus,
              "card replaces its cached bitmap even when image dimensions are unchanged");
        DestroyWindow(owner);
        std::cout << "Background screenshot, source loss, framing sliders, apply and reopen passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
