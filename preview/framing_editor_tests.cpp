#include "framing_editor.hpp"
#include "framing_card.hpp"
#include <commctrl.h>
#include <string_view>
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
COLORREF card_pixel(FramingEditor &editor, LONG x, LONG y) {
    const auto monitor = editor.create_monitor({0, 0, 300, 300}, 17);
    check(monitor != nullptr, "create dashboard for paint test");
    const auto screen = GetDC(nullptr);
    const auto dc = CreateCompatibleDC(screen);
    const auto image = CreateCompatibleBitmap(screen, 300, 300);
    const auto previous = SelectObject(dc, image);
    SendMessageW(monitor, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    DestroyWindow(monitor);
    const auto color = GetPixel(dc, x, y);
    SelectObject(dc, previous);
    DeleteObject(image);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return color;
}
void monitor_gestures(HWND owner, FramingEditor &framing, Options &options, double aspect) {
    const RECT card{12, 20, 336, 208};
    const auto monitor = framing.create_monitor(card, 17);
    check(monitor != nullptr, "create embedded dashboard crop canvas");
    RECT client{};
    GetClientRect(monitor, &client);
    const auto image = framing_canvas_image(client, aspect, true);
    auto crop = [&] { return framing_crop_rect(image, aspect, options, options.eye); };
    auto gesture = [&](POINT at, int dx, int dy) {
        SendMessageW(monitor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(at.x, at.y));
        SendMessageW(monitor, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(at.x + dx, at.y + dy));
        SendMessageW(monitor, WM_LBUTTONUP, 0, MAKELPARAM(at.x + dx, at.y + dy));
        check(GetCapture() != monitor, "release the pointer after a dashboard gesture");
    };
    options.crop = 35;
    options.horizontal_offset = options.vertical_offset = 0;
    const auto initial = crop();
    gesture({1, 1}, 18, 12);
    auto result = crop();
    check(EqualRect(&initial, &result), "dashboard padding ignores dragging");
    gesture({(initial.left + initial.right) / 2, (initial.top + initial.bottom) / 2}, 9, 8);
    result = crop();
    check(result.left == initial.left + 9 && result.top == initial.top + 8 &&
          result.right == initial.right + 9 && result.bottom == initial.bottom + 8,
          "dragging inside immediately moves the stream framing");
    for (bool right : {false, true})
        for (bool bottom : {false, true}) {
            options.crop = 35;
            options.horizontal_offset = options.vertical_offset = 0;
            const auto before = crop();
            gesture({right ? before.right - 1 : before.left, bottom ? before.bottom - 1 : before.top},
                    right ? -13 : 13, bottom ? -8 : 8);
            const auto after = crop();
            check(after.right - after.left < before.right - before.left &&
                  after.bottom - after.top < before.bottom - before.top, "each blue corner resizes the crop");
            check((right ? before.left == after.left : before.right == after.right) &&
                  (bottom ? before.top == after.top : before.bottom == after.bottom),
                  "opposite crop corner stays anchored");
        }
    RECT location{};
    GetWindowRect(monitor, &location);
    MapWindowPoints(nullptr, owner, reinterpret_cast<POINT *>(&location), 2);
    check(EqualRect(&location, &card), "the canvas retains its dashboard bounds");
    DestroyWindow(monitor);
}
} // namespace
int main(int argc, char **argv) {
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        InitCommonControlsEx(&controls);
        const auto owner = CreateWindowW(L"STATIC", L"", WS_OVERLAPPED, 0, 0, 400, 400, nullptr, nullptr,
                                         GetModuleHandleW(nullptr), nullptr);
        check(owner != nullptr, "create owner");
        Theme theme;
        theme.initialize();
        Renderer renderer;
        Options options;
        bool applied{};
        FramingEditor framing;
        framing.initialize(
            owner, theme, renderer, [&] { return options; },
            [&](const Options &fresh) {
                options = fresh;
                applied = true;
            });
        if (argc == 2 && std::string_view(argv[1]) == "--monitor") {
            monitor_gestures(owner, framing, options, 1.);
            check(applied && !framing.has_still(), "dashboard editing works offline");
            DestroyWindow(owner);
            std::cout << "Dashboard crop movement and four-corner resize passed\n";
            return 0;
        }
        check(renderer.attach_offscreen(), "create renderer");
        framing.capture();
        check(framing.status().find(L"No live image") != std::wstring::npos, "missing source feedback");
        const Options original = options;
        options.aspect = 1;
        options.crop = 20;
        options.horizontal_offset = -100;
        const auto left_border = card_pixel(framing, 6, 150);
        options.horizontal_offset = 100;
        const auto shifted = card_pixel(framing, 6, 150);
        check(GetBValue(left_border) > GetRValue(left_border) + 100 && shifted != left_border,
              "schematic visibly moves the output rectangle to the selected source boundary");
        check(!framing.has_still() && framing.still_revision() == 0 && framing.still_description().empty(),
              "painting a framing card never captures a screenshot");
        options = original;
        check(renderer.demo(1), "provide a selected eye");
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
        const auto card_focus = card_pixel(framing, 150, 150);
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
        check(card_pixel(framing, 150, 150) != card_focus,
              "card replaces its cached bitmap even when image dimensions are unchanged");
        monitor_gestures(owner, framing, options, renderer.source_aspect());
        DestroyWindow(owner);
        std::cout << "Background screenshot, source loss, framing sliders, apply and reopen passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
