#include "theme.hpp"
#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <dwmapi.h>
#include <uxtheme.h>

namespace vrst::preview {
namespace {
constexpr COLORREF bg = RGB(16, 21, 29), panel = RGB(24, 31, 42), field = RGB(33, 43, 56),
                   border = RGB(53, 66, 82), text = RGB(231, 239, 248), muted = RGB(153, 172, 191),
                   accent = RGB(63, 218, 231), disabled = RGB(112, 128, 144);
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
void label(HDC dc, HWND window, RECT r, COLORREF color, UINT flags) {
    wchar_t value[1024]{};
    GetWindowTextW(window, value, 1024);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    auto previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    DrawTextW(dc, value, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, previous);
}
void checkbox(HDC dc, HWND window) {
    RECT r{};
    GetClientRect(window, &r);
    fill(dc, r, GetPropW(window, L"SteadyXRSurface") ? bg : panel);
    RECT box{2, (r.bottom - 18) / 2, 20, (r.bottom + 18) / 2};
    const bool enabled = IsWindowEnabled(window),
               checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    fill(dc, box, checked && enabled ? accent : field);
    outline(dc, box, enabled ? accent : border);
    if (checked) {
        auto pen = CreatePen(PS_SOLID, 2, enabled ? bg : disabled);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, box.left + 4, box.top + 9, nullptr);
        LineTo(dc, box.left + 8, box.top + 13);
        LineTo(dc, box.left + 14, box.top + 5);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    r.left = 30;
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
        fill(dc, r, field);
        if (!editable) {
            RECT content = r;
            content.left += 10;
            content.right -= 30;
            label(dc, window, content, enabled ? text : disabled,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        RECT arrow{r.right - 26, 1, r.right - 1, r.bottom - 1};
        fill(dc, arrow, field);
        outline(dc, r, GetFocus() == window ? accent : border);
        auto pen = CreatePen(PS_SOLID, 2, enabled ? muted : disabled);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, arrow.left + 8, r.bottom / 2 - 2, nullptr);
        LineTo(dc, arrow.left + 12, r.bottom / 2 + 2);
        LineTo(dc, arrow.left + 16, r.bottom / 2 - 2);
        SelectObject(dc, old);
        DeleteObject(pen);
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
} // namespace
void Theme::initialize() {
    auto font = [](int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    body = font(16, FW_NORMAL);
    title_ = font(27, FW_SEMIBOLD);
    heading_ = font(20, FW_SEMIBOLD);
    background_ = CreateSolidBrush(bg);
    panel_ = CreateSolidBrush(panel);
    field_ = CreateSolidBrush(field);
    icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 64, 64, 0));
    small_icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 20, 20, 0));
}
Theme::~Theme() {
    for (auto object : {body, title_, heading_})
        if (object)
            DeleteObject(object);
    for (auto brush : {background_, panel_, field_})
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
void Theme::surface(HWND window, HDC dc, RECT card) {
    RECT r{};
    GetClientRect(window, &r);
    fill(dc, r, bg);
    rounded(dc, card, panel, border, 18);
}
bool Theme::message(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
    result = 0;
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX) {
        auto dc = reinterpret_cast<HDC>(w);
        const auto child = reinterpret_cast<HWND>(l);
        const bool input = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        const bool surface = GetPropW(child, L"SteadyXRSurface") != nullptr;
        SetTextColor(dc, !IsWindowEnabled(child)             ? disabled
                         : GetPropW(child, L"SteadyXRMuted") ? muted
                                                             : text);
        SetBkColor(dc, input ? field : surface ? bg : panel);
        result = reinterpret_cast<LRESULT>(input ? field_ : surface ? background_ : panel_);
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
        surface(window, dc, {24, 164, r.right - 24, 550});
        DrawIconEx(dc, 24, 18, icon, 52, 52, 0, nullptr, DI_NORMAL);
        auto old = SelectObject(dc, title_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, text);
        RECT title{92, 16, 580, 50};
        DrawTextW(dc, L"SteadyXR", -1, &title, DT_SINGLELINE);
        SelectObject(dc, body);
        SetTextColor(dc, muted);
        RECT tagline{94, 52, 590, 78};
        DrawTextW(dc, L"A steady view. Every corner.", -1, &tagline, DT_SINGLELINE);
        SelectObject(dc, old);
        if (message == WM_PAINT)
            EndPaint(window, &ps);
        return true;
    }
    if (message == WM_DRAWITEM) {
        const auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item.CtlType != ODT_BUTTON)
            return false;
        const bool primary = item.CtlID == 103, enabled = !(item.itemState & ODS_DISABLED);
        const bool tab = GetPropW(item.hwndItem, L"SteadyXRTab") != nullptr;
        const bool selected = tab && SendMessageW(item.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool hover = enabled && GetPropW(item.hwndItem, L"SteadyXRHover");
        const bool surface = GetPropW(item.hwndItem, L"SteadyXRSurface") != nullptr;
        auto color = primary && enabled ? accent : tab && !selected ? bg : field;
        if (hover && !primary)
            color = RGB(43, 57, 73);
        if (item.itemState & ODS_SELECTED)
            color = primary ? RGB(33, 174, 188) : border;
        fill(item.hDC, item.rcItem, surface ? bg : panel);
        rounded(item.hDC, item.rcItem, color,
                (primary && enabled) || selected ? accent
                : tab && !hover                  ? bg
                                                 : border);
        label(item.hDC, item.hwndItem, item.rcItem,
              !enabled        ? disabled
              : primary       ? bg
              : selected      ? accent
              : tab && !hover ? muted
                              : text,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
            auto focus = item.rcItem;
            InflateRect(&focus, -4, -4);
            DrawFocusRect(item.hDC, &focus);
        }
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
            fill(draw.hdc, client, panel);
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
            fill(draw.hdc, thumb, enabled ? accent : disabled);
            if (GetFocus() == header->hwndFrom &&
                !(SendMessageW(header->hwndFrom, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
                DrawFocusRect(draw.hdc, &client);
            RestoreDC(draw.hdc, saved);
            result = CDRF_SKIPDEFAULT;
        } else
            result = CDRF_DODEFAULT;
        return true;
    }
    return false;
}
} // namespace vrst::preview
