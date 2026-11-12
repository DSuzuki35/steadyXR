#include "theme.hpp"
#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <dwmapi.h>
#include <initializer_list>
#include <uxtheme.h>
#if __has_include("steadyxr_version.h")
#include "steadyxr_version.h"
#endif

namespace vrst::preview {
namespace {
constexpr UINT reveal_control_message = WM_APP + 0x530;
constexpr COLORREF bg = RGB(18, 20, 23), panel = RGB(29, 31, 35), field = RGB(42, 45, 50),
                   border = RGB(64, 68, 75), text = RGB(244, 246, 248), muted = RGB(170, 176, 185),
                   accent = RGB(49, 108, 207), accent_text = RGB(182, 211, 255),
                   disabled = RGB(123, 128, 136);
constexpr COLORREF chrome = RGB(21, 21, 23), canvas = RGB(28, 28, 31), card = RGB(36, 36, 40),
                   rail = RGB(25, 25, 28), hairline = RGB(53, 53, 60), blue = RGB(94, 158, 255),
                   active_nav = RGB(34, 49, 71);
enum class ShellZone : unsigned { none, chrome, canvas, card, rail };
constexpr COLORREF zone_colors[]{chrome, canvas, card, rail};
ShellZone zone_at(RECT child) {
    if (child.top < 64)
        return ShellZone::chrome;
    if (child.left < 208)
        return ShellZone::rail;
    if (child.top >= 628)
        return ShellZone::chrome;
    if (child.top < 590 && (child.left >= 844 || child.top >= 196))
        return ShellZone::card;
    return ShellZone::canvas;
}
unsigned zone_of(HWND child) {
    return unsigned(reinterpret_cast<UINT_PTR>(GetPropW(child, L"SteadyXRShellZone")));
}
COLORREF control_background(HWND child) {
    const auto zone = zone_of(child);
    if (zone)
        return zone_colors[zone - 1];
    return GetPropW(child, L"SteadyXRSurface") ? bg : panel;
}
void fill(HDC dc, RECT r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void outline(HDC dc, RECT r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void rounded(HDC dc, RECT r, COLORREF color, COLORREF edge, int radius = 12) {
    const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, edge);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
}
void sidebar_icon(HDC dc, UINT id, int x, int y, UINT dpi, COLORREF color) {
    const int saved = SaveDC(dc);
    // Use the same child-local coordinates as the text. Native WM_PRINT may
    // combine a child offset with its own window origin and clipping region.
    const auto sx = [=](LONG v) { return x + MulDiv(v, dpi, 96); };
    const auto sy = [=](LONG v) { return y + MulDiv(v, dpi, 96); };
    auto pen = CreatePen(PS_SOLID, MulDiv(2, dpi, 96), color);
    SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    const auto stroke = [&](std::initializer_list<POINT> path) {
        const auto *first = path.begin();
        MoveToEx(dc, sx(first->x), sy(first->y), nullptr);
        for (auto point = first + 1; point != path.end(); ++point)
            LineTo(dc, sx(point->x), sy(point->y));
    };
    const auto frame = [&](int left, int top, int right, int bottom, int radius) {
        const int corner = MulDiv(radius, dpi, 96);
        RoundRect(dc, sx(left), sy(top), sx(right), sy(bottom), corner, corner);
    };
    const auto circle = [&](int left, int top, int right, int bottom) {
        Ellipse(dc, sx(left), sy(top), sx(right), sy(bottom));
    };
    switch (id) {
    case 1500: // Stream output
        frame(2, 3, 21, 16, 3);
        stroke({{11, 16}, {11, 20}});
        stroke({{6, 20}, {17, 20}});
        break;
    case 1501: // Camera
        stroke({{2, 7}, {6, 7}, {8, 4}, {14, 4}, {16, 7}, {20, 7}, {20, 19}, {2, 19}, {2, 7}});
        circle(7, 9, 15, 17);
        break;
    case 1502: // Framing
        stroke({{5, 2}, {5, 17}, {21, 17}});
        stroke({{2, 5}, {17, 5}, {17, 21}});
        break;
    case 1503: // Settings
        stroke({{2, 6}, {8, 6}});
        stroke({{13, 6}, {21, 6}});
        stroke({{2, 15}, {13, 15}});
        stroke({{18, 15}, {21, 15}});
        circle(7, 3, 14, 10);
        circle(12, 12, 19, 19);
        break;
    case 135: // Next preset
        stroke({{3, 8}, {11, 4}, {20, 8}, {11, 12}, {3, 8}});
        stroke({{3, 12}, {11, 16}, {20, 12}});
        stroke({{3, 16}, {11, 20}, {20, 16}});
        break;
    case 148: // Coffee
        frame(2, 7, 16, 18, 4);
        stroke({{16, 8}, {19, 8}, {20, 10}, {20, 13}, {18, 15}, {16, 15}});
        stroke({{2, 20}, {18, 20}});
        stroke({{7, 2}, {7, 5}});
        stroke({{12, 2}, {12, 5}});
        break;
    case 149: // Affiliate links
        stroke({{12, 4}, {19, 4}, {19, 11}});
        stroke({{19, 4}, {9, 14}});
        stroke({{9, 7}, {4, 7}, {4, 19}, {16, 19}, {16, 14}});
        break;
    }
    RestoreDC(dc, saved);
    DeleteObject(pen);
}
void label(HDC dc, HWND window, RECT r, COLORREF color, UINT flags, HFONT custom_font = nullptr) {
    wchar_t value[1024]{};
    GetWindowTextW(window, value, 1024);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    auto previous = SelectObject(
        dc, custom_font ? custom_font : reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    DrawTextW(dc, value, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, previous);
}
void checkbox(HDC dc, HWND window) {
    RECT r{};
    GetClientRect(window, &r);
    fill(dc, r, control_background(window));
    const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(GetParent(window), L"SteadyXRDpi")));
    const auto px = [dpi](int value) { return MulDiv(value, dpi ? dpi : 96, 96); };
    RECT box{px(2), (r.bottom - px(18)) / 2, px(20), (r.bottom + px(18)) / 2};
    const bool enabled = IsWindowEnabled(window),
               checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    rounded(dc, box, checked && enabled ? accent : field, checked && enabled ? blue : border, px(6));
    if (checked) {
        auto pen = CreatePen(PS_SOLID, px(2), enabled ? text : disabled);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, box.left + px(4), box.top + px(9), nullptr);
        LineTo(dc, box.left + px(8), box.top + px(13));
        LineTo(dc, box.left + px(14), box.top + px(5));
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    r.left = px(30);
    label(dc, window, r, enabled ? text : disabled, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        DrawFocusRect(dc, &r);
}
LRESULT CALLBACK check_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(w);
        checkbox(dc, window);
        if (message == WM_PAINT)
            EndPaint(window, &ps);
        return 0;
    }
    const auto result = DefSubclassProc(window, message, w, l);
    if (message == BM_SETCHECK || message == BM_SETSTATE || message == WM_ENABLE || message == WM_SETFOCUS ||
        message == WM_KILLFOCUS || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_KEYDOWN || message == WM_KEYUP || message == WM_MOUSELEAVE)
        InvalidateRect(window, nullptr, FALSE);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, check_proc, 1);
    return result;
}
LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    const bool tab = GetPropW(window, L"SteadyXRTab") != nullptr;
    if (tab && (message == WM_PAINT || message == WM_PRINTCLIENT)) {
        PAINTSTRUCT ps{};
        DRAWITEMSTRUCT item{};
        item.CtlType = ODT_BUTTON;
        item.CtlID = GetDlgCtrlID(window);
        item.itemAction = ODA_DRAWENTIRE;
        item.hwndItem = window;
        item.hDC = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(w);
        GetClientRect(window, &item.rcItem);
        if (!IsWindowEnabled(window))
            item.itemState |= ODS_DISABLED;
        if (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED)
            item.itemState |= ODS_SELECTED;
        if (GetFocus() == window)
            item.itemState |= ODS_FOCUS;
        if (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)
            item.itemState |= ODS_NOFOCUSRECT;
        SendMessageW(GetParent(window), WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
        if (message == WM_PAINT)
            EndPaint(window, &ps);
        return 0;
    }
    if (message == WM_MOUSEMOVE && !GetPropW(window, L"SteadyXRHover")) {
        SetPropW(window, L"SteadyXRHover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE) {
        RemovePropW(window, L"SteadyXRHover");
        InvalidateRect(window, nullptr, FALSE);
    }
    const auto result = DefSubclassProc(window, message, w, l);
    if (tab && (message == BM_SETCHECK || message == BM_SETSTATE || message == WM_ENABLE ||
                message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_UPDATEUISTATE))
        InvalidateRect(window, nullptr, FALSE);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, button_proc, 1);
    return result;
}
LRESULT CALLBACK combo_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        // The native combo still owns its edit, popup, keyboard and selection.
        const bool editable = (GetWindowLongPtrW(window, GWL_STYLE) & 3) == CBS_DROPDOWN;
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(w);
        RECT r{};
        GetClientRect(window, &r);
        const bool enabled = IsWindowEnabled(window);
        const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(GetParent(window), L"SteadyXRDpi")));
        const auto px = [dpi](int value) { return MulDiv(value, dpi ? dpi : 96, 96); };
        const int saved = SaveDC(dc);
        COMBOBOXINFO info{sizeof(info)};
        if (editable && GetComboBoxInfo(window, &info) && info.hwndItem) {
            RECT edit{};
            GetWindowRect(info.hwndItem, &edit);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&edit), 2);
            ExcludeClipRect(dc, edit.left, edit.top, edit.right, edit.bottom);
        }
        fill(dc, r, field);
        if (!editable) {
            RECT content = r;
            content.left += px(10);
            content.right -= px(30);
            label(dc, window, content, enabled ? text : disabled,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        outline(dc, r, GetFocus() == window || IsChild(window, GetFocus()) ? accent : border);
        auto pen = CreatePen(PS_SOLID, px(2), enabled ? muted : disabled);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, r.right - px(18), r.bottom / 2 - px(2), nullptr);
        LineTo(dc, r.right - px(14), r.bottom / 2 + px(2));
        LineTo(dc, r.right - px(10), r.bottom / 2 - px(2));
        SelectObject(dc, old);
        DeleteObject(pen);
        RestoreDC(dc, saved);
        if (message == WM_PAINT)
            EndPaint(window, &ps);
        return 0;
    }
    const auto result = DefSubclassProc(window, message, w, l);
    if (message == CB_SETCURSEL || message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS)
        InvalidateRect(window, nullptr, FALSE);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, combo_proc, 1);
    return result;
}
LRESULT CALLBACK focus_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    const auto result = DefSubclassProc(window, message, w, l);
    if (message == WM_SETFOCUS)
        SendMessageW(GetParent(window), reveal_control_message, reinterpret_cast<WPARAM>(window), 0);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, focus_proc, 1);
    return result;
}
} // namespace
void Theme::initialize() {
    auto font = [](int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    body = font(16, FW_NORMAL);
    title_ = font(32, FW_SEMIBOLD);
    heading_ = font(20, FW_SEMIBOLD);
    strong_ = font(16, FW_SEMIBOLD);
    caption_ = font(12, FW_SEMIBOLD);
    background_ = CreateSolidBrush(bg);
    panel_ = CreateSolidBrush(panel);
    field_ = CreateSolidBrush(field);
    for (size_t i = 0; i < shell_brushes_.size(); ++i)
        shell_brushes_[i] = CreateSolidBrush(zone_colors[i]);
    icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 64, 64, 0));
    small_icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 20, 20, 0));
}
Theme::~Theme() {
    for (const auto &fonts : fonts_) {
        DeleteObject(fonts.body);
        DeleteObject(fonts.heading);
        DeleteObject(fonts.title);
        DeleteObject(fonts.strong);
    }
    for (auto object : {body, title_, heading_, strong_, caption_})
        if (object)
            DeleteObject(object);
    for (auto brush : {background_, panel_, field_})
        if (brush)
            DeleteObject(brush);
    for (auto brush : shell_brushes_)
        if (brush)
            DeleteObject(brush);
    if (icon)
        DestroyIcon(icon);
    if (small_icon)
        DestroyIcon(small_icon);
}
DWORD Theme::style(const wchar_t *kind, DWORD value) const {
    if (!std::wcscmp(kind, L"STATIC"))
        return value | SS_NOPREFIX;
    if (!std::wcscmp(kind, L"BUTTON") && (value & BS_TYPEMASK) == BS_PUSHBUTTON)
        return (value & ~BS_TYPEMASK) | BS_OWNERDRAW;
    if (!std::wcscmp(kind, TRACKBAR_CLASSW))
        return (value & ~TBS_AUTOTICKS) | TBS_NOTICKS;
    return value;
}
void Theme::control(HWND window, const wchar_t *kind, bool heading) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(heading ? heading_ : body), TRUE);
    if (heading)
        SetPropW(window, L"HorizonHeading", reinterpret_cast<HANDLE>(1));
    SetWindowSubclass(window, focus_proc, 1, 0);
    if (!std::wcscmp(kind, L"COMBOBOX")) {
        SetWindowTheme(window, L"", L"");
        SetWindowSubclass(window, combo_proc, 1, 0);
    }
    if (!std::wcscmp(kind, L"BUTTON") &&
        (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX)
        SetWindowSubclass(window, check_proc, 1, 0);
    if (!std::wcscmp(kind, L"BUTTON")) {
        const auto type = GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK;
        if (type == BS_OWNERDRAW || type == BS_AUTORADIOBUTTON)
            SetWindowSubclass(window, button_proc, 1, 0);
    }
    if (!std::wcscmp(kind, TRACKBAR_CLASSW))
        SetWindowTheme(window, L"", L"");
}
void Theme::titlebar(HWND window) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
}
void Theme::scale_window(HWND window, UINT dpi) {
    if (!dpi)
        dpi = GetDpiForWindow(window);
    dpi = std::clamp(dpi, 96u, 480u);
    auto layout = std::find_if(layouts_.begin(), layouts_.end(),
                               [window](const Layout &entry) { return entry.window == window; });
    if (layout == layouts_.end()) {
        RECT client{};
        GetClientRect(window, &client);
        Layout entry{window, client.right, client.bottom, {}};
        for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT bounds{};
            GetWindowRect(child, &bounds);
            wchar_t kind[32]{};
            GetClassNameW(child, kind, 32);
            if (!_wcsicmp(kind, L"COMBOBOX") && (GetWindowLongPtrW(child, GWL_STYLE) & 3) != CBS_SIMPLE) {
                // Windows sizes a combo with its dropdown included, but GetWindowRect
                // describes only the closed field. Keep its real anchor and width.
                RECT dropped{};
                SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropped));
                const auto row = SendMessageW(child, CB_GETITEMHEIGHT, 0, 0);
                const LONG minimum = bounds.bottom - bounds.top + 6 * LONG(std::max<LRESULT>(1, row)) + 2;
                bounds.bottom = bounds.top + std::max(minimum, dropped.bottom - dropped.top);
            }
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
            if (GetPropW(window, L"SteadyXRMainShell"))
                SetPropW(child, L"SteadyXRShellZone", reinterpret_cast<HANDLE>(UINT_PTR(zone_at(bounds))));
            entry.children.push_back({child, bounds});
        }
        layouts_.push_back(std::move(entry));
        layout = std::prev(layouts_.end());
    }
    auto fonts =
        std::find_if(fonts_.begin(), fonts_.end(), [dpi](const Fonts &entry) { return entry.dpi == dpi; });
    if (fonts == fonts_.end()) {
        const auto scaled_font = [dpi](HFONT original) {
            LOGFONTW description{};
            GetObjectW(original, sizeof(description), &description);
            description.lfHeight = MulDiv(description.lfHeight, dpi, 96);
            return CreateFontIndirectW(&description);
        };
        fonts_.push_back(
            {dpi, scaled_font(body), scaled_font(heading_), scaled_font(title_), scaled_font(strong_)});
        fonts = std::prev(fonts_.end());
    }
    SetPropW(window, L"SteadyXRDpi", reinterpret_cast<HANDLE>(UINT_PTR(dpi)));
    for (const auto &child : layout->children) {
        const auto &r = child.bounds;
        if (GetPropW(child.window, L"SteadyXRSidebar") || GetPropW(child.window, L"SteadyXRPrimary") ||
            GetDlgCtrlID(child.window) == 103)
            SetPropW(child.window, L"SteadyXRStrongFont", fonts->strong);
        const auto font = GetPropW(child.window, L"SteadyXRPageTitle") ? fonts->title
                          : GetPropW(child.window, L"HorizonHeading")  ? fonts->heading
                                                                       : fonts->body;
        SendMessageW(child.window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetWindowPos(child.window, nullptr, MulDiv(r.left, dpi, 96), MulDiv(r.top, dpi, 96),
                     MulDiv(r.right - r.left, dpi, 96), MulDiv(r.bottom - r.top, dpi, 96),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT outer{0, 0, MulDiv(layout->width, dpi, 96), MulDiv(layout->height, dpi, 96)};
    // Nonclient metrics follow the actual monitor even when a diagnostic requests a different scale.
    AdjustWindowRectExForDpi(&outer, DWORD(GetWindowLongPtrW(window, GWL_STYLE)), GetMenu(window) != nullptr,
                             DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
    SetPropW(window, L"SteadyXRLayoutBusy", reinterpret_cast<HANDLE>(1));
    SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RemovePropW(window, L"SteadyXRLayoutBusy");
    if (GetPropW(window, L"SteadyXRMainShell") || GetPropW(window, L"SteadyXRScrollable")) {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            fit_window(window, monitor.rcWork);
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void Theme::fit_window(HWND window, const RECT &work_area) {
    const auto layout = std::find_if(layouts_.begin(), layouts_.end(),
                                     [window](const Layout &entry) { return entry.window == window; });
    if (layout == layouts_.end() || work_area.right <= work_area.left || work_area.bottom <= work_area.top)
        return;
    SetPropW(window, L"SteadyXRScrollable", reinterpret_cast<HANDLE>(1));
    const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
    const int width = MulDiv(layout->width, dpi ? dpi : 96, 96);
    const int height = MulDiv(layout->height, dpi ? dpi : 96, 96);
    const auto actual_dpi = GetDpiForWindow(window);
    const auto base_style = DWORD(GetWindowLongPtrW(window, GWL_STYLE)) & ~(WS_HSCROLL | WS_VSCROLL);
    RECT border{};
    AdjustWindowRectExForDpi(&border, base_style, GetMenu(window) != nullptr,
                             DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)), actual_dpi);
    const int border_width = border.right - border.left, border_height = border.bottom - border.top;
    const int available_width = work_area.right - work_area.left;
    const int available_height = work_area.bottom - work_area.top;
    const int scrollbar_width = GetSystemMetricsForDpi(SM_CXVSCROLL, actual_dpi);
    const int scrollbar_height = GetSystemMetricsForDpi(SM_CYHSCROLL, actual_dpi);
    bool horizontal{}, vertical{};
    for (unsigned pass = 0; pass < 3; ++pass) {
        horizontal = width > available_width - border_width - (vertical ? scrollbar_width : 0);
        vertical = height > available_height - border_height - (horizontal ? scrollbar_height : 0);
    }
    const int outer_width =
        std::min(available_width, width + border_width + (vertical ? scrollbar_width : 0));
    const int outer_height =
        std::min(available_height, height + border_height + (horizontal ? scrollbar_height : 0));
    RECT outer{};
    GetWindowRect(window, &outer);
    const int x = std::clamp(int(outer.left), int(work_area.left), int(work_area.right) - outer_width);
    const int y = std::clamp(int(outer.top), int(work_area.top), int(work_area.bottom) - outer_height);
    SetPropW(window, L"SteadyXRLayoutBusy", reinterpret_cast<HANDLE>(1));
    SetWindowLongPtrW(window, GWL_STYLE,
                      base_style | (horizontal ? WS_HSCROLL : 0) | (vertical ? WS_VSCROLL : 0));
    SetWindowPos(window, nullptr, x, y, outer_width, outer_height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    update_scroll_range(window);
    RemovePropW(window, L"SteadyXRLayoutBusy");
    reveal_control(window, GetFocus());
}
void Theme::update_scroll_range(HWND window) {
    const auto layout = std::find_if(layouts_.begin(), layouts_.end(),
                                     [window](const Layout &entry) { return entry.window == window; });
    if (layout == layouts_.end())
        return;
    const bool was_busy = GetPropW(window, L"SteadyXRLayoutBusy") != nullptr;
    SetPropW(window, L"SteadyXRLayoutBusy", reinterpret_cast<HANDLE>(1));
    const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
    const int width = MulDiv(layout->width, dpi ? dpi : 96, 96);
    const int height = MulDiv(layout->height, dpi ? dpi : 96, 96);
    const int scroll_x = GetScrollPos(window, SB_HORZ), scroll_y = GetScrollPos(window, SB_VERT);
    for (unsigned pass = 0; pass < 3; ++pass) {
        RECT client{};
        GetClientRect(window, &client);
        SCROLLINFO h{sizeof(h), SIF_RANGE | SIF_PAGE | SIF_POS, 0, width - 1, UINT(client.right), scroll_x};
        SCROLLINFO v{sizeof(v), SIF_RANGE | SIF_PAGE | SIF_POS, 0, height - 1, UINT(client.bottom), scroll_y};
        SetScrollInfo(window, SB_HORZ, &h, TRUE);
        SetScrollInfo(window, SB_VERT, &v, TRUE);
    }
    scroll_to(window, scroll_x, scroll_y);
    if (!was_busy)
        RemovePropW(window, L"SteadyXRLayoutBusy");
}
void Theme::scroll_to(HWND window, int x, int y) {
    const auto layout = std::find_if(layouts_.begin(), layouts_.end(),
                                     [window](const Layout &entry) { return entry.window == window; });
    if (layout == layouts_.end())
        return;
    SCROLLINFO horizontal{sizeof(horizontal), SIF_POS}, vertical{sizeof(vertical), SIF_POS};
    horizontal.nPos = x;
    vertical.nPos = y;
    SetScrollInfo(window, SB_HORZ, &horizontal, TRUE);
    SetScrollInfo(window, SB_VERT, &vertical, TRUE);
    x = GetScrollPos(window, SB_HORZ);
    y = GetScrollPos(window, SB_VERT);
    const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
    for (const auto &child : layout->children)
        SetWindowPos(child.window, nullptr, MulDiv(child.bounds.left, dpi ? dpi : 96, 96) - x,
                     MulDiv(child.bounds.top, dpi ? dpi : 96, 96) - y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void Theme::reveal_control(HWND window, HWND child) {
    if (!child || !IsChild(window, child) || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
        return;
    RECT client{}, bounds{};
    GetClientRect(window, &client);
    GetWindowRect(child, &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
    const int x = GetScrollPos(window, SB_HORZ), y = GetScrollPos(window, SB_VERT);
    const auto displacement = [](int first, int last, int available) {
        if (last - first > available)
            return first;
        return first < 0 ? first : last > available ? last - available : 0;
    };
    const int dx = displacement(bounds.left, bounds.right, client.right);
    const int dy = displacement(bounds.top, bounds.bottom, client.bottom);
    if (dx || dy)
        scroll_to(window, x + dx, y + dy);
}
void Theme::surface(HWND window, HDC dc, RECT card) {
    RECT r{};
    GetClientRect(window, &r);
    fill(dc, r, bg);
    const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
    if (dpi) {
        card.left = MulDiv(card.left, dpi, 96);
        card.top = MulDiv(card.top, dpi, 96);
        card.right = MulDiv(card.right, dpi, 96);
        card.bottom = MulDiv(card.bottom, dpi, 96);
    }
    rounded(dc, card, panel, border, 18);
}
bool Theme::message(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
    result = 0;
    if (GetPropW(window, L"SteadyXRScrollable")) {
        if (message == WM_SIZE && w != SIZE_MINIMIZED) {
            if (!GetPropW(window, L"SteadyXRLayoutBusy")) {
                update_scroll_range(window);
                reveal_control(window, GetFocus());
            }
            return true;
        }
        if (message == reveal_control_message) {
            reveal_control(window, reinterpret_cast<HWND>(w));
            return true;
        }
        if (message == WM_COMMAND && (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == CBN_SETFOCUS))
            reveal_control(window, reinterpret_cast<HWND>(l));
        if ((message == WM_HSCROLL || message == WM_VSCROLL) && !l) {
            const int axis = message == WM_HSCROLL ? SB_HORZ : SB_VERT;
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(window, axis, &info);
            int position = info.nPos;
            const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
            const int line = MulDiv(32, dpi ? dpi : 96, 96);
            switch (LOWORD(w)) {
            case SB_LINEUP:
                position -= line;
                break;
            case SB_LINEDOWN:
                position += line;
                break;
            case SB_PAGEUP:
                position -= int(info.nPage);
                break;
            case SB_PAGEDOWN:
                position += int(info.nPage);
                break;
            case SB_TOP:
                position = 0;
                break;
            case SB_BOTTOM:
                position = info.nMax;
                break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                position = info.nTrackPos;
                break;
            }
            scroll_to(window, axis == SB_HORZ ? position : GetScrollPos(window, SB_HORZ),
                      axis == SB_VERT ? position : GetScrollPos(window, SB_VERT));
            return true;
        }
        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
            const bool horizontal = message == WM_MOUSEHWHEEL || (LOWORD(w) & MK_SHIFT);
            const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
            const int distance = MulDiv(short(HIWORD(w)), MulDiv(96, dpi ? dpi : 96, 96), WHEEL_DELTA);
            scroll_to(window,
                      GetScrollPos(window, SB_HORZ) +
                          (horizontal ? (message == WM_MOUSEHWHEEL ? distance : -distance) : 0),
                      GetScrollPos(window, SB_VERT) - (horizontal ? 0 : distance));
            return true;
        }
        if (message == WM_EXITSIZEMOVE || message == WM_DISPLAYCHANGE) {
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
                fit_window(window, monitor.rcWork);
        }
    }
    if (message == WM_DPICHANGED) {
        const auto &suggested = *reinterpret_cast<RECT *>(l);
        SetWindowPos(window, nullptr, suggested.left, suggested.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        scale_window(window, HIWORD(w));
        return true;
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX) {
        auto dc = reinterpret_cast<HDC>(w);
        const auto child = reinterpret_cast<HWND>(l);
        const bool input = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        const auto zone = zone_of(child);
        const bool surface = GetPropW(child, L"SteadyXRSurface") != nullptr;
        SetTextColor(dc, !IsWindowEnabled(child)             ? disabled
                         : GetPropW(child, L"SteadyXRMuted") ? muted
                                                             : text);
        SetBkColor(dc, input ? field : control_background(child));
        result = reinterpret_cast<LRESULT>(input     ? field_
                                           : zone    ? shell_brushes_[zone - 1]
                                           : surface ? background_
                                                     : panel_);
        return true;
    }
    if (message == WM_ERASEBKGND) {
        result = 1;
        return true;
    }
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        auto dc = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(w);
        RECT r{};
        GetClientRect(window, &r);
        fill(dc, r, bg);
        const auto dpi = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
        const int saved = SaveDC(dc);
        if (dpi && dpi != 96) {
            r.right = MulDiv(r.right, 96, dpi);
            r.bottom = MulDiv(r.bottom, 96, dpi);
            SetMapMode(dc, MM_ANISOTROPIC);
            SetWindowExtEx(dc, 96, 96, nullptr);
            SetViewportExtEx(dc, dpi, dpi, nullptr);
        }
        const bool shell = GetPropW(window, L"SteadyXRMainShell") != nullptr;
        if (GetPropW(window, L"SteadyXRScrollable")) {
            const auto layout = std::find_if(layouts_.begin(), layouts_.end(), [window](const Layout &entry) {
                return entry.window == window;
            });
            if (layout != layouts_.end())
                r = {0, 0, layout->width, layout->height};
            OffsetViewportOrgEx(dc, -GetScrollPos(window, SB_HORZ), -GetScrollPos(window, SB_VERT), nullptr);
        }
        fill(dc, r, bg);
        if (shell) {
            fill(dc, r, canvas);
            fill(dc, {0, 0, r.right, 64}, chrome);
            fill(dc, {0, 64, r.right, 65}, hairline);
            fill(dc, {0, 65, 208, r.bottom}, rail);
            fill(dc, {207, 65, 208, r.bottom}, hairline);
            fill(dc, {208, 628, r.right, r.bottom}, chrome);
            fill(dc, {208, 628, r.right, 629}, hairline);

            rounded(dc, {240, 196, 820, 590}, card, hairline, 20);
            rounded(dc, {844, 104, 1208, 404}, card, hairline, 20);
            rounded(dc, {844, 422, 1208, 590}, card, hairline, 20);

            DrawIconEx(dc, 23, 16, icon, 32, 32, 0, nullptr, DI_NORMAL);
            SetBkMode(dc, TRANSPARENT);
            const auto caption = [&](const wchar_t *value, RECT bounds, HFONT font, COLORREF color,
                                     UINT align = DT_LEFT) {
                SelectObject(dc, font);
                SetTextColor(dc, color);
                DrawTextW(dc, value, -1, &bounds, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            };
            caption(L"SteadyXR", {65, 0, 191, 64}, heading_, text);
            fill(dc, {207, 20, 208, 44}, hairline);
            rounded(dc, {238, 28, 246, 36}, blue, blue, 8);
            caption(L"Stream Output", {257, 0, 510, 64}, strong_, text);
#ifdef STEADYXR_VERSION_STRING
            caption(L"v" STEADYXR_VERSION_STRING, {r.right - 112, 0, r.right - 24, 64}, body, muted,
                    DT_RIGHT);
#endif
            SetTextCharacterExtra(dc, 1);
            caption(L"WORKSPACE", {28, 86, 192, 113}, caption_, muted);
            caption(L"PROFILE", {28, 386, 192, 413}, caption_, muted);
            caption(L"SUPPORT", {28, 496, 192, 523}, caption_, muted);
            SetTextCharacterExtra(dc, 0);
        } else {
            rounded(dc, {12, 12, r.right - 12, r.bottom - 12}, panel, border, 10);
        }
        RestoreDC(dc, saved);
        if (message == WM_PAINT)
            EndPaint(window, &ps);
        return true;
    }
    if (message == WM_DRAWITEM) {
        const auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item.CtlType != ODT_BUTTON)
            return false;
        const bool primary = item.CtlID == 103 || GetPropW(item.hwndItem, L"SteadyXRPrimary"),
                   enabled = !(item.itemState & ODS_DISABLED);
        const bool tab = GetPropW(item.hwndItem, L"SteadyXRTab") != nullptr;
        const bool selected = tab && SendMessageW(item.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool hover = enabled && GetPropW(item.hwndItem, L"SteadyXRHover");
        const bool sidebar = GetPropW(item.hwndItem, L"SteadyXRSidebar") != nullptr;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const auto background = control_background(item.hwndItem);
        const auto dpi_value = UINT(reinterpret_cast<UINT_PTR>(GetPropW(window, L"SteadyXRDpi")));
        const UINT dpi = dpi_value ? dpi_value : 96;
        const auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
        const bool focus = (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT);
        COLORREF color = field, edge = RGB(70, 72, 80);
        if (primary && enabled) {
            color = pressed ? RGB(36, 83, 161) : hover ? RGB(61, 124, 222) : accent;
            edge = RGB(76, 133, 224);
        } else if (sidebar) {
            color = selected ? active_nav : hover || pressed ? RGB(43, 44, 49) : background;
            edge = selected ? RGB(53, 74, 105) : color;
        } else if (pressed || hover) {
            color = pressed ? RGB(47, 49, 54) : RGB(53, 56, 63);
        }
        fill(item.hDC, item.rcItem, background);
        rounded(item.hDC, item.rcItem, color, focus ? blue : edge, px(sidebar ? 14 : 12));
        auto label_rect = item.rcItem;
        if (sidebar) {
            if (selected) {
                RECT marker{px(1), item.rcItem.bottom / 2 - px(10), px(4), item.rcItem.bottom / 2 + px(10)};
                rounded(item.hDC, marker, blue, blue, px(3));
            }
            sidebar_icon(item.hDC, item.CtlID, item.rcItem.left + px(14),
                         (item.rcItem.top + item.rcItem.bottom - px(22)) / 2, dpi, selected ? blue : muted);
            label_rect.left += px(46);
            label_rect.right -= px(4);
        }
        const auto bold = (selected || primary)
                              ? static_cast<HFONT>(GetPropW(item.hwndItem, L"SteadyXRStrongFont"))
                              : nullptr;
        label(item.hDC, item.hwndItem, label_rect,
              !enabled            ? disabled
              : selected          ? accent_text
              : sidebar && !hover ? muted
                                  : text,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (sidebar ? DT_LEFT : DT_CENTER), bold);
        result = TRUE;
        return true;
    }
    if (message == WM_NOTIFY) {
        const auto *header = reinterpret_cast<NMHDR *>(l);
        wchar_t kind[64]{};
        GetClassNameW(header->hwndFrom, kind, 64);
        if (header->code != NM_CUSTOMDRAW || std::wcscmp(kind, TRACKBAR_CLASSW))
            return false;
        const auto &draw = *reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw.dwDrawStage == CDDS_PREPAINT) {
            // Native incremental paints may omit channel or even all item callbacks.
            // Draw one complete scene; the paint DC's clip limits it to the damage.
            const int saved = SaveDC(draw.hdc);
            RECT client{}, channel{}, thumb{};
            GetClientRect(header->hwndFrom, &client);
            IntersectClipRect(draw.hdc, client.left, client.top, client.right, client.bottom);
            const bool shell_slider = zone_of(header->hwndFrom) != 0;
            fill(draw.hdc, client, shell_slider ? control_background(header->hwndFrom) : panel);
            SendMessageW(header->hwndFrom, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            SendMessageW(header->hwndFrom, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            const bool enabled = IsWindowEnabled(header->hwndFrom);
            const int mid = (channel.top + channel.bottom) / 2;
            channel.top = mid - 2;
            channel.bottom = mid + 2;
            fill(draw.hdc, channel, border);
            const long long maximum = static_cast<int>(SendMessageW(header->hwndFrom, TBM_GETRANGEMAX, 0, 0));
            const long long minimum = static_cast<int>(SendMessageW(header->hwndFrom, TBM_GETRANGEMIN, 0, 0));
            const long long value = static_cast<int>(SendMessageW(header->hwndFrom, TBM_GETPOS, 0, 0));
            if (maximum > minimum) {
                channel.right = channel.left + LONG((channel.right - channel.left) * (value - minimum) /
                                                    (maximum - minimum));
                fill(draw.hdc, channel, enabled ? accent : disabled);
            }
            const bool focused = GetFocus() == header->hwndFrom &&
                                 !(SendMessageW(header->hwndFrom, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
            const int size = std::min(thumb.right - thumb.left, thumb.bottom - thumb.top);
            if (shell_slider) {
                const int left = (thumb.left + thumb.right - size) / 2;
                thumb = {left, mid - size / 2, left + size, mid - size / 2 + size};
            }
            const auto thumb_color = !enabled ? disabled : shell_slider ? blue : accent;
            rounded(draw.hdc, thumb, thumb_color, focused ? text : thumb_color, size);
            if (focused) {
                InflateRect(&client, -1, -1);
                DrawFocusRect(draw.hdc, &client);
            }
            RestoreDC(draw.hdc, saved);
            result = CDRF_SKIPDEFAULT;
        } else
            result = CDRF_DODEFAULT;
        return true;
    }
    return false;
}
} // namespace vrst::preview
