#include "theme.hpp"
#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Real native trackbars on a hidden parent: the test never activates a window.
struct Slider {
    vrst::preview::Theme theme;
    HWND parent{}, window{};
    unsigned prepaints{}, item_paints{}, scrolls{};
    static constexpr int width = 385, height = 40;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Slider *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Slider *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_NOTIFY) {
            const auto &draw = *reinterpret_cast<NMCUSTOMDRAW *>(l);
            if (draw.hdr.code == NM_CUSTOMDRAW) {
                self->prepaints += draw.dwDrawStage == CDDS_PREPAINT;
                self->item_paints += draw.dwDrawStage == CDDS_ITEMPREPAINT;
            }
            LRESULT result{};
            if (self->theme.message(window, message, w, l, result))
                return result;
        }
        if (self && message == WM_HSCROLL)
            ++self->scrolls;
        if (self) {
            LRESULT result{};
            if (self->theme.message(window, message, w, l, result))
                return result;
        }
        return DefWindowProcW(window, message, w, l);
    }

    Slider() {
        theme.initialize();
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"HorizonLockSliderPaintTest";
        RegisterClassW(&type);
        parent = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"", WS_POPUP, 0, 0, width, height,
                                 nullptr, nullptr, type.hInstance, this);
        check(parent != nullptr, "create hidden slider parent");
        window = CreateWindowW(TRACKBAR_CLASSW, L"", theme.style(TRACKBAR_CLASSW, WS_CHILD | WS_VISIBLE), 0,
                               0, width, height, parent, nullptr, type.hInstance, nullptr);
        check(window != nullptr, "create native slider");
        theme.control(window, TRACKBAR_CLASSW);
        SendMessageW(window, TBM_SETRANGEMIN, FALSE, -1000);
        SendMessageW(window, TBM_SETRANGEMAX, FALSE, 1000);
        SendMessageW(window, TBM_SETLINESIZE, 0, 1);
        SendMessageW(window, TBM_SETPAGESIZE, 0, 100);
        position(-500);
    }
    ~Slider() {
        if (parent)
            DestroyWindow(parent);
    }
    void position(int value) {
        SendMessageW(window, TBM_SETPOS, TRUE, value);
    }
    int position() const {
        return static_cast<int>(SendMessageW(window, TBM_GETPOS, 0, 0));
    }
    RECT rectangle(UINT message) const {
        RECT rectangle{};
        SendMessageW(window, message, 0, reinterpret_cast<LPARAM>(&rectangle));
        return rectangle;
    }
};

struct Canvas {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    Canvas() {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = Slider::width;
        info.bmiHeader.biHeight = -Slider::height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *pixels{};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        check(dc && bitmap, "create slider paint DIB");
        previous = SelectObject(dc, bitmap);
        clear();
    }
    ~Canvas() {
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    void clear() {
        SetDCBrushColor(dc, RGB(255, 0, 255));
        RECT rect{0, 0, Slider::width, Slider::height};
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void paint(const Slider &slider) {
        SendMessageW(slider.window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    }
    COLORREF pixel(int x, int y) const {
        return GetPixel(dc, x, y);
    }
    std::vector<COLORREF> pixels() const {
        std::vector<COLORREF> result;
        for (int y = 0; y < Slider::height; ++y)
            for (int x = 0; x < Slider::width; ++x)
                result.push_back(pixel(x, y));
        return result;
    }
};

void repeated_native_paint_keeps_track_and_thumb() {
    Slider slider;
    Canvas canvas;
    canvas.paint(slider);
    const auto channel = slider.rectangle(TBM_GETCHANNELRECT);
    const auto thumb = slider.rectangle(TBM_GETTHUMBRECT);
    const auto y = (channel.top + channel.bottom) / 2;
    const auto background = canvas.pixel(0, Slider::height - 1);
    check(background == RGB(29, 31, 35), "slider uses the approved dark surface");
    check(canvas.pixel((thumb.left + thumb.right) / 2, thumb.top + 2) == RGB(49, 108, 207),
          "slider uses the blue action accent");
    check(canvas.pixel(channel.right - 2, y) != background, "first native paint draws the track");
    check(canvas.pixel((thumb.left + thumb.right) / 2, thumb.top + 2) != background,
          "first native paint draws the thumb");
    const auto complete = canvas.pixels();
    slider.prepaints = slider.item_paints = 0;
    canvas.paint(slider);
    check(slider.prepaints == 1, "native repeated print reaches custom prepaint");
    check(canvas.pixels() == complete,
          "a native repaint with no dirty items must preserve the complete track and thumb");
    check(slider.item_paints == 0, "complete custom paint must not depend on native item callbacks");
}

void clipped_native_paint_restores_track_under_old_thumb() {
    Slider slider;
    Canvas incremental;
    incremental.paint(slider);
    const auto before = incremental.pixels();
    const auto old_thumb = slider.rectangle(TBM_GETTHUMBRECT);
    slider.position(500);
    const auto new_thumb = slider.rectangle(TBM_GETTHUMBRECT);
    RECT damage{};
    UnionRect(&damage, &old_thumb, &new_thumb);
    const int saved = SaveDC(incremental.dc);
    IntersectClipRect(incremental.dc, damage.left, damage.top, damage.right, damage.bottom);
    incremental.paint(slider);
    RestoreDC(incremental.dc, saved);
    const auto clipped = incremental.pixels();
    Canvas complete;
    // Native print has no dirty items now; it must still produce a whole slider.
    complete.paint(slider);
    const auto expected = complete.pixels();
    for (int y = 0; y < Slider::height; ++y) {
        for (int x = 0; x < Slider::width; ++x) {
            const auto index = static_cast<std::size_t>(y * Slider::width + x);
            const bool damaged = x >= damage.left && x < damage.right && y >= damage.top && y < damage.bottom;
            check(clipped[index] == (damaged ? expected[index] : before[index]),
                  "partial native paint restores damaged pixels and honors the HDC clip");
        }
    }
    const auto channel = slider.rectangle(TBM_GETCHANNELRECT);
    check(incremental.pixel((old_thumb.left + old_thumb.right) / 2, (channel.top + channel.bottom) / 2) !=
              incremental.pixel(0, Slider::height - 1),
          "moving the thumb must restore its old track segment");
}

void signed_range_fill_tracks_native_position() {
    Slider slider;
    Canvas canvas;
    for (const int value : {-1000, -500, 0, 500, 1000}) {
        slider.position(value);
        canvas.paint(slider);
        const auto channel = slider.rectangle(TBM_GETCHANNELRECT);
        const auto thumb = slider.rectangle(TBM_GETTHUMBRECT);
        const auto y = (channel.top + channel.bottom) / 2;
        const auto foreground = canvas.pixel((thumb.left + thumb.right) / 2, thumb.top + 2);
        const auto background = canvas.pixel(0, Slider::height - 1);
        for (const int fraction : {1, 3, 5, 7}) {
            const int x = channel.left + (channel.right - channel.left) * fraction / 8;
            const int sample_value = -1000 + fraction * 250;
            check(canvas.pixel(x, y) != background, "signed slider always retains its full track");
            check((canvas.pixel(x, y) == foreground) == (sample_value < value),
                  "negative, zero and positive positions fill the correct signed range fraction");
        }
    }
    EnableWindow(slider.window, FALSE);
    canvas.paint(slider);
    const auto disabled = canvas.pixels();
    canvas.paint(slider);
    check(canvas.pixels() == disabled, "disabled track and thumb survive repeated paints");
}

void painting_preserves_native_input_and_notifications() {
    Slider slider;
    Canvas canvas;
    canvas.paint(slider);
    SendMessageW(slider.window, WM_KEYDOWN, VK_RIGHT, 0);
    SendMessageW(slider.window, WM_KEYUP, VK_RIGHT, 0);
    check(slider.position() == -499, "custom painted slider retains native arrow-key movement");
    check(slider.scrolls != 0, "native keyboard changes still notify the parent");
    canvas.paint(slider);
    const auto thumb = slider.rectangle(TBM_GETTHUMBRECT);
    const auto x = (thumb.left + thumb.right) / 2;
    const auto y = (thumb.top + thumb.bottom) / 2;
    const auto notifications = slider.scrolls;
    SendMessageW(slider.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(slider.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 50, y));
    SendMessageW(slider.window, WM_LBUTTONUP, 0, MAKELPARAM(x + 50, y));
    check(slider.position() > -499, "custom painted slider retains native thumb dragging");
    check(slider.scrolls > notifications, "native drag changes still notify the parent");
}

void dpi_change_scales_controls_and_preserves_input() {
    Slider slider;
    const auto text = CreateWindowW(L"STATIC", L"DPI text", WS_CHILD, 0, 0, 100, 24, slider.parent, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    slider.theme.control(text, L"STATIC");
    RECT suggested{0, 0, 578, 60};
    LRESULT result{};
    check(slider.theme.message(slider.parent, WM_DPICHANGED, MAKELONG(144, 144),
                               reinterpret_cast<LPARAM>(&suggested), result),
          "theme handles per-monitor DPI changes");
    RECT bounds{};
    GetClientRect(slider.window, &bounds);
    check(bounds.right == 578 && bounds.bottom == 60, "DPI change scales native controls to 150 percent");
    LOGFONTW font{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(text, WM_GETFONT, 0, 0)), sizeof(font), &font);
    check(font.lfHeight == -24, "DPI change scales native control text");
    check(slider.position() == -500, "DPI change preserves slider value");
    suggested = {0, 0, Slider::width, Slider::height};
    slider.theme.message(slider.parent, WM_DPICHANGED, MAKELONG(96, 96), reinterpret_cast<LPARAM>(&suggested),
                         result);
    GetClientRect(slider.window, &bounds);
    check(bounds.right == Slider::width && bounds.bottom == Slider::height,
          "returning to 100 percent restores the original layout without drift");
}

void small_work_area_keeps_scaled_controls_reachable() {
    Slider fixture;
    const auto footer =
        CreateWindowW(L"BUTTON", L"Open preview", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 900, 700, 180, 40,
                      fixture.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    fixture.theme.control(footer, L"BUTTON");
    SetWindowPos(fixture.parent, nullptr, 0, 0, 1120, 760, SWP_NOZORDER | SWP_NOACTIVATE);
    SetPropW(fixture.parent, L"SteadyXRMainShell", reinterpret_cast<HANDLE>(1));
    fixture.theme.scale_window(fixture.parent, 144);
    const RECT available{20, 30, 1300, 750}; // A 1280 x 720 work area.
    fixture.theme.fit_window(fixture.parent, available);
    RECT outer{}, client{}, control{};
    GetWindowRect(fixture.parent, &outer);
    check(outer.left >= available.left && outer.top >= available.top && outer.right <= available.right &&
              outer.bottom <= available.bottom,
          "the scaled main form fits inside a 1280 x 720 work area");
    SCROLLINFO vertical{sizeof(vertical), SIF_ALL}, horizontal{sizeof(horizontal), SIF_ALL};
    GetScrollInfo(fixture.parent, SB_VERT, &vertical);
    GetScrollInfo(fixture.parent, SB_HORZ, &horizontal);
    check(vertical.nMax >= int(vertical.nPage) && horizontal.nMax >= int(horizontal.nPage),
          "overflowing scaled content exposes native scrollbars on both axes");
    SendMessageW(footer, WM_SETFOCUS, 0, 0);
    GetClientRect(fixture.parent, &client);
    GetWindowRect(footer, &control);
    MapWindowPoints(nullptr, fixture.parent, reinterpret_cast<POINT *>(&control), 2);
    check(control.left >= 0 && control.top >= 0 && control.right <= client.right &&
              control.bottom <= client.bottom,
          "keyboard focus reveals the footer control without shrinking its text");
    SendMessageW(fixture.window, WM_SETFOCUS, 0, 0);
    GetWindowRect(fixture.window, &control);
    MapWindowPoints(nullptr, fixture.parent, reinterpret_cast<POINT *>(&control), 2);
    check(control.left >= 0 && control.top >= 0 && control.right <= client.right &&
              control.bottom <= client.bottom,
          "focus can return to content at the top of the scrollable form");
    check(fixture.position() == -500, "scrolling never changes camera slider values");
}

// Activates the side-by-side common controls the shipped manifest selects, so the
// same checks cover both the user32 combo and the comctl32 version 6 combo.
struct VisualStyles {
    HANDLE context{INVALID_HANDLE_VALUE};
    ULONG_PTR cookie{};
    bool active{};
    VisualStyles() {
        wchar_t directory[MAX_PATH]{};
        GetSystemDirectoryW(directory, MAX_PATH);
        ACTCTXW request{sizeof(request)};
        request.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
        request.lpSource = L"shell32.dll";
        request.lpAssemblyDirectory = directory;
        request.lpResourceName = MAKEINTRESOURCEW(124);
        context = CreateActCtxW(&request);
        active = context != INVALID_HANDLE_VALUE && ActivateActCtx(context, &cookie);
        if (active) {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
            InitCommonControlsEx(&controls);
        }
    }
    ~VisualStyles() {
        if (active)
            DeactivateActCtx(0, cookie);
        if (context != INVALID_HANDLE_VALUE)
            ReleaseActCtx(context);
    }
};

// Native combos use the same window message routing as the application.
struct Combos {
    vrst::preview::Theme theme;
    HWND parent{}, list{}, editable{}, collapsed{}, footer{};
    unsigned selections{};
    static constexpr int list_id = 201, editable_id = 202, collapsed_id = 203;
    static constexpr int width = 900, height = 900;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Combos *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Combos *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (message == WM_COMMAND && HIWORD(w) == CBN_SELCHANGE)
                ++self->selections;
            LRESULT result{};
            if (self->theme.message(window, message, w, l, result))
                return result;
        }
        return DefWindowProcW(window, message, w, l);
    }
    HWND add(const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
        const auto window = CreateWindowExW(
            0, kind, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | theme.style(kind, style), x, y, w, h, parent,
            reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        check(window != nullptr, "create native combo fixture control");
        theme.control(window, kind);
        return window;
    }
    Combos() {
        theme.initialize();
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"SteadyXRComboPopupTest";
        RegisterClassW(&type);
        parent = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"", WS_POPUP, 40, 40, width, height,
                                 nullptr, nullptr, type.hInstance, this);
        check(parent != nullptr, "create hidden combo parent");
        list = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 240, 214, 500, 180, list_id);
        for (const auto text : {L"One", L"Two", L"Three", L"Four", L"Five", L"Six", L"Seven", L"Eight"})
            SendMessageW(list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(list, CB_SETCURSEL, 1, 0);
        editable = add(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_HASSTRINGS | WS_VSCROLL, 240, 300, 180, 200,
                       editable_id);
        for (const auto text : {L"16:9", L"21:9", L"32:9", L"4:3", L"1:1"})
            SendMessageW(editable, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SetWindowTextW(editable, L"37:10");
        // Created with only its closed height, like a control placed by a later layout pass.
        collapsed = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 240, 380, 200, 10, collapsed_id);
        for (const auto text : {L"A", L"B", L"C", L"D", L"E", L"F"})
            SendMessageW(collapsed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        footer = add(L"BUTTON", L"Open preview", BS_PUSHBUTTON, 700, 840, 180, 40, 204);
        theme.scale_window(parent, 96);
        ShowWindow(parent, SW_SHOWNOACTIVATE);
    }
    ~Combos() {
        if (parent)
            DestroyWindow(parent);
    }
    static HWND popup(HWND combo) {
        COMBOBOXINFO info{sizeof(info)};
        check(GetComboBoxInfo(combo, &info) && info.hwndList, "native combo exposes its popup list");
        return info.hwndList;
    }
    static int item_height(HWND combo) {
        return int(SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0));
    }
    // Opens the real popup and returns the height of the list the user would see.
    static int open(HWND combo) {
        SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
        check(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0, "native combo popup opens");
        check(IsWindowVisible(popup(combo)), "native combo popup list is shown");
        RECT bounds{};
        GetWindowRect(popup(combo), &bounds);
        return bounds.bottom - bounds.top;
    }
    static void close(HWND combo) {
        SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
        check(!SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0), "native combo popup closes");
    }
    // The popup must show `rows` whole items (or every item when there are fewer).
    static void expect_rows(HWND combo, int rows, const char *message) {
        const int count = int(SendMessageW(combo, CB_GETCOUNT, 0, 0));
        const int tall = open(combo);
        close(combo);
        if (tall < std::min(rows, count) * item_height(combo)) {
            std::cerr << "popup height " << tall << " item height " << item_height(combo) << '\n';
            check(false, message);
        }
    }
    void dpi(UINT value) {
        RECT suggested{};
        GetWindowRect(parent, &suggested);
        LRESULT result{};
        check(theme.message(parent, WM_DPICHANGED, MAKELONG(value, value),
                            reinterpret_cast<LPARAM>(&suggested), result),
              "theme handles combo DPI changes");
    }
    // Chooses a row with the mouse in the open popup, as a user would.
    void click(HWND combo, int row) {
        open(combo);
        RECT item{};
        const auto native = popup(combo);
        check(SendMessageW(native, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item)) != LB_ERR,
              "popup row is addressable");
        RECT client{};
        GetClientRect(native, &client);
        check(item.top >= 0 && item.bottom <= client.bottom, "popup row is inside the visible popup");
        const auto point = MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2);
        SendMessageW(native, WM_MOUSEMOVE, 0, point);
        SendMessageW(native, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(native, WM_LBUTTONUP, 0, point);
        if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0))
            SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
    }
};

void combo_popup_keeps_its_height_through_every_layout_pass() {
    Combos fixture;
    fixture.expect_rows(fixture.list, 5, "a new combo opens a usable popup");
    fixture.expect_rows(fixture.collapsed, 5,
                        "a combo created at its closed height still opens a usable popup");
    for (const UINT value : {120u, 144u, 192u, 96u, 144u}) {
        fixture.dpi(value);
        fixture.expect_rows(fixture.list, 5, "the popup keeps its height after a DPI change");
        fixture.expect_rows(fixture.editable, 5, "the editable popup keeps its height after a DPI change");
        fixture.expect_rows(fixture.collapsed, 5, "the closed-height combo keeps a popup after a DPI change");
    }
    // A small work area turns on scrolling; focus then repositions every child.
    const RECT available{20, 30, 820, 630};
    fixture.theme.fit_window(fixture.parent, available);
    for (unsigned pass = 0; pass < 3; ++pass) {
        SendMessageW(fixture.footer, WM_SETFOCUS, 0, 0);
        SendMessageW(fixture.parent, WM_VSCROLL, SB_TOP, 0);
        SendMessageW(fixture.parent, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(-WHEEL_DELTA)), 0);
        SendMessageW(fixture.parent, WM_SIZE, SIZE_RESTORED, 0);
    }
    fixture.expect_rows(fixture.list, 5, "the popup keeps its height after scrolling repositions the combo");
    fixture.expect_rows(fixture.editable, 5, "the editable popup keeps its height after scrolling");
    fixture.expect_rows(fixture.collapsed, 5, "the closed-height combo keeps a popup after scrolling");
    RECT closed{};
    GetWindowRect(fixture.list, &closed);
    check(closed.bottom - closed.top < 3 * fixture.item_height(fixture.list),
          "layout passes leave the closed combo at its native closed height");
    fixture.dpi(96);
    fixture.expect_rows(fixture.list, 5, "returning to 100 percent restores the original popup");
}

void combo_selection_survives_dpi_and_reposition() {
    Combos fixture;
    check(SendMessageW(fixture.list, CB_GETCURSEL, 0, 0) == 1, "initial selection");
    COMBOBOXINFO info{sizeof(info)};
    check(GetComboBoxInfo(fixture.editable, &info) && info.hwndItem, "aspect combo owns its native edit");
    SetFocus(info.hwndItem);
    check(GetFocus() == info.hwndItem, "aspect editor can receive keyboard focus");
    SetWindowPos(fixture.parent, nullptr, 95, 71, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    fixture.dpi(144);
    const RECT available{20, 30, 820, 630};
    fixture.theme.fit_window(fixture.parent, available);
    check(GetFocus() == info.hwndItem, "scaling and moving preserve aspect editor focus");
    SetFocus(fixture.footer);
    SetFocus(fixture.list);
    check(SendMessageW(fixture.list, CB_GETCURSEL, 0, 0) == 1, "layout passes preserve the selection");
    wchar_t value[32]{};
    GetWindowTextW(fixture.editable, value, 32);
    check(!std::wcscmp(value, L"37:10"), "layout passes preserve typed text in an editable combo");
    const auto before = fixture.selections;
    fixture.click(fixture.list, 4);
    check(SendMessageW(fixture.list, CB_GETCURSEL, 0, 0) == 4,
          "a row chosen in the scaled popup is selected");
    check(fixture.selections > before, "choosing a popup row still notifies the parent");
    fixture.click(fixture.editable, 2);
    GetWindowTextW(fixture.editable, value, 32);
    check(!std::wcscmp(value, L"32:9"), "choosing a row updates the editable combo text");
    SendMessageW(fixture.list, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(fixture.list, WM_KEYUP, VK_DOWN, 0);
    check(SendMessageW(fixture.list, CB_GETCURSEL, 0, 0) == 5, "keyboard selection still works when closed");
    fixture.dpi(96);
    check(SendMessageW(fixture.list, CB_GETCURSEL, 0, 0) == 5, "selection survives returning to 100 percent");
}
} // namespace

int main() {
    try {
        // Opening popup windows and giving the aspect edit real focus must never
        // interrupt keyboard or mouse input on the interactive desktop.
        const auto name = L"SteadyXRThemeTests" + std::to_wstring(GetCurrentProcessId());
        const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        check(desktop && SetThreadDesktop(desktop), "create a private desktop for native input tests");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        check(InitCommonControlsEx(&controls), "initialize native trackbars");
        repeated_native_paint_keeps_track_and_thumb();
        clipped_native_paint_restores_track_under_old_thumb();
        signed_range_fill_tracks_native_position();
        painting_preserves_native_input_and_notifications();
        dpi_change_scales_controls_and_preserves_input();
        small_work_area_keeps_scaled_controls_reachable();
        combo_popup_keeps_its_height_through_every_layout_pass();
        combo_selection_survives_dpi_and_reposition();
        {
            VisualStyles styles;
            check(styles.active, "activate version 6 common controls");
            combo_popup_keeps_its_height_through_every_layout_pass();
            combo_selection_survives_dpi_and_reposition();
        }
        std::cout << "Native slider, combo popup, DPI and focus tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
