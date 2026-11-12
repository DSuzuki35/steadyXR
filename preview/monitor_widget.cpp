#include "monitor_widget.hpp"
#include <algorithm>
#include <cwchar>
#include <windowsx.h>

namespace vrst::preview {
namespace {
constexpr RECT work{216, 75, 1216, 616};
constexpr RECT home{844, 104, 1208, 404};
constexpr long min_width = 240, min_height = 148;
constexpr int reset_id = 1;
constexpr COLORREF panel = RGB(36, 36, 40), edge = RGB(53, 53, 60);
constexpr COLORREF text = RGB(244, 246, 248), muted = RGB(170, 176, 185);

RECT bounded(long x, long y, long width, long height) {
    width = std::clamp(width, min_width, work.right - work.left);
    height = std::clamp(height, min_height, work.bottom - work.top);
    x = std::clamp(x, work.left, work.right - width);
    y = std::clamp(y, work.top, work.bottom - height);
    return {x, y, x + width, y + height};
}
void fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
} // namespace

struct MonitorWidget::Impl {
    HWND parent{}, hwnd{}, reset_button{};
    Theme *theme{};
    std::filesystem::path ini;
    std::function<void(HDC, RECT)> paint_canvas;
    std::wstring caption;
    HFONT body{}, heading{};
    RECT bounds{home}, original{};
    POINT down{};
    enum class Drag { none, move, bottom_left, bottom_right } drag{};
    bool changed{};

    ~Impl() {
        if (hwnd)
            DestroyWindow(hwnd);
        if (heading)
            DeleteObject(heading);
    }
    void load() {
        wchar_t buffer[128]{}, trailing{};
        long x{}, y{}, width{}, height{};
        if (!ini.empty() &&
            GetPrivateProfileStringW(L"FramingMonitor", L"Bounds", L"", buffer, 128, ini.c_str()) &&
            std::swscanf(buffer, L"%ld,%ld,%ld,%ld %lc", &x, &y, &width, &height, &trailing) == 4 &&
            width > 0 && height > 0)
            bounds = bounded(x, y, width, height);
    }
    void save() const {
        if (!ini.empty()) {
            wchar_t buffer[96]{};
            std::swprintf(buffer, std::size(buffer), L"%ld,%ld,%ld,%ld", bounds.left, bounds.top,
                          bounds.right - bounds.left, bounds.bottom - bounds.top);
            WritePrivateProfileStringW(L"FramingMonitor", L"Bounds", buffer, ini.c_str());
        }
    }
    int dpi() const {
        const auto scale = reinterpret_cast<UINT_PTR>(GetPropW(parent, L"SteadyXRDpi"));
        return scale ? int(scale) : 96;
    }
    int px(int value) const {
        return MulDiv(value, dpi(), 96);
    }
    RECT physical() const {
        RECT r{px(bounds.left), px(bounds.top), px(bounds.right), px(bounds.bottom)};
        OffsetRect(&r, -GetScrollPos(parent, SB_HORZ), -GetScrollPos(parent, SB_VERT));
        return r;
    }
    void place() {
        const auto r = physical();
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    }
    void stop() {
        if (drag == Drag::none)
            return;
        drag = Drag::none;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        if (changed)
            save();
        changed = false;
    }
    void reset() {
        changed = false;
        stop();
        bounds = home;
        place();
        save();
    }
    void font(HFONT new_body) {
        if (body == new_body && heading)
            return;
        body = new_body;
        if (heading)
            DeleteObject(heading);
        LOGFONTW spec{};
        GetObjectW(body, sizeof(spec), &spec);
        spec.lfWeight = FW_SEMIBOLD;
        spec.lfHeight = MulDiv(spec.lfHeight, 5, 4);
        heading = CreateFontIndirectW(&spec);
        if (reset_button)
            SendMessageW(reset_button, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    RECT client() const {
        RECT r{};
        GetClientRect(hwnd, &r);
        return r;
    }
    RECT canvas_rect() const {
        const auto r = client();
        return {px(20), px(58), r.right - px(20), r.bottom - px(54)};
    }
    void layout() {
        const auto r = client();
        if (reset_button)
            SetWindowPos(reset_button, nullptr, r.right - px(76), px(13), px(60), px(29),
                         SWP_NOACTIVATE | SWP_NOZORDER);
        SetPropW(hwnd, L"SteadyXRDpi", reinterpret_cast<HANDLE>(UINT_PTR(dpi())));
        // Round the actual child surface, so a moved monitor reveals the
        // dashboard underneath its corners, even above another control.
        HRGN mask = CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1, px(18), px(18));
        if (mask && !SetWindowRgn(hwnd, mask, TRUE))
            DeleteObject(mask);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    Drag hit(POINT p) const {
        const auto r = client();
        if (!PtInRect(&r, p))
            return Drag::none;
        if (p.y >= r.bottom - px(28)) {
            if (p.x < px(28))
                return Drag::bottom_left;
            if (p.x >= r.right - px(28))
                return Drag::bottom_right;
        }
        if (p.y < px(52) && p.x < r.right - px(80))
            return Drag::move;
        return Drag::none;
    }
    POINT dashboard_point(POINT local) const {
        MapWindowPoints(hwnd, parent, &local, 1);
        return {MulDiv(local.x + GetScrollPos(parent, SB_HORZ), 96, dpi()),
                MulDiv(local.y + GetScrollPos(parent, SB_VERT), 96, dpi())};
    }
    void update(POINT local) {
        const auto p = dashboard_point(local);
        const auto dx = p.x - down.x, dy = p.y - down.y;
        const auto w = original.right - original.left, h = original.bottom - original.top;
        RECT next = original;
        if (drag == Drag::move)
            next = bounded(original.left + dx, original.top + dy, w, h);
        else {
            next.bottom = std::clamp(original.bottom + dy, original.top + min_height, work.bottom);
            if (drag == Drag::bottom_left)
                next.left = std::clamp(original.left + dx, work.left, original.right - min_width);
            else
                next.right = std::clamp(original.right + dx, original.left + min_width, work.right);
        }
        if (!EqualRect(&bounds, &next)) {
            bounds = next;
            changed = true;
            place();
        }
    }
    void paint(HDC dc) const {
        const auto saved = SaveDC(dc);
        const auto r = client();
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, panel);
        SetDCPenColor(dc, edge);
        RoundRect(dc, 0, 0, r.right, r.bottom, px(18), px(18));
        // Grab dots identify the header, and both lower corners resize the widget.
        for (int y : {20, 25, 30})
            for (int x : {14, 19})
                fill(dc, {px(x), px(y), px(x + 2), px(y + 2)}, RGB(128, 136, 148));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, text);
        SelectObject(dc, heading ? heading : body);
        RECT title{px(32), px(9), r.right - px(82), px(46)};
        DrawTextW(dc, L"Framing monitor", -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        auto canvas = canvas_rect();
        if (paint_canvas && !IsRectEmpty(&canvas)) {
            const auto clip = SaveDC(dc);
            IntersectClipRect(dc, canvas.left, canvas.top, canvas.right, canvas.bottom);
            paint_canvas(dc, canvas);
            RestoreDC(dc, clip);
        }
        SelectObject(dc, body);
        SetTextColor(dc, muted);
        RECT label{px(30), canvas.bottom + px(7), r.right - px(30), r.bottom - px(8)};
        DrawTextW(dc, caption.c_str(), -1, &label,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetDCPenColor(dc, RGB(141, 151, 164));
        for (int size : {5, 10, 15}) {
            MoveToEx(dc, px(8 + size), r.bottom - px(8), nullptr);
            LineTo(dc, px(8), r.bottom - px(8 + size));
            MoveToEx(dc, r.right - px(8 + size), r.bottom - px(8), nullptr);
            LineTo(dc, r.right - px(8), r.bottom - px(8 + size));
        }
        RestoreDC(dc, saved);
    }
    static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM a, LPARAM l) {
        auto *p = reinterpret_cast<Impl *>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            p = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            p->hwnd = w;
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        }
        if (!p)
            return DefWindowProcW(w, msg, a, l);
        const POINT at{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        switch (msg) {
        case WM_WINDOWPOSCHANGING: {
            // The dashboard theme replays its original list of child bounds
            // on DPI/scroll changes. Project our current logical layout instead.
            auto *pos = reinterpret_cast<WINDOWPOS *>(l);
            const auto r = p->physical();
            pos->x = r.left;
            pos->y = r.top;
            pos->cx = r.right - r.left;
            pos->cy = r.bottom - r.top;
            break;
        }
        case WM_SIZE:
            p->layout();
            return 0;
        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(p->body);
        case WM_SETFONT:
            p->font(reinterpret_cast<HFONT>(a));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        case WM_PRINTCLIENT: {
            PAINTSTRUCT ps{};
            const auto dc = msg == WM_PAINT ? BeginPaint(w, &ps) : reinterpret_cast<HDC>(a);
            p->paint(dc);
            if (msg == WM_PAINT)
                EndPaint(w, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(a) == reset_id && HIWORD(a) == BN_CLICKED) {
                p->reset();
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            LRESULT result{};
            if (p->theme->message(w, msg, a, l, result))
                return result;
            break;
        }
        case WM_LBUTTONDOWN:
            p->drag = p->hit(at);
            if (p->drag != Drag::none) {
                p->original = p->bounds;
                p->down = p->dashboard_point(at);
                p->changed = false;
                SetCapture(w);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (p->drag != Drag::none) {
                p->update(at);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (p->drag != Drag::none) {
                p->update(at);
                p->stop();
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (p->hit(at) == Drag::move) {
                p->reset();
                return 0;
            }
            break;
        case WM_SETCURSOR: {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(w, &cursor);
            const auto mode = p->drag != Drag::none ? p->drag : p->hit(cursor);
            if (mode != Drag::none) {
                SetCursor(LoadCursor(nullptr, mode == Drag::move          ? IDC_SIZEALL
                                              : mode == Drag::bottom_left ? IDC_SIZENESW
                                                                          : IDC_SIZENWSE));
                return TRUE;
            }
            break;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            p->stop();
            break;
        case WM_NCDESTROY:
            p->hwnd = nullptr;
            SetWindowLongPtrW(w, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(w, msg, a, l);
    }
};

MonitorWidget::MonitorWidget() : impl_(new Impl) {}
MonitorWidget::~MonitorWidget() = default;
bool MonitorWidget::create(HWND parent, Theme &theme, const std::filesystem::path &ini,
                           std::function<void(HDC, RECT)> paint_canvas, int id) {
    auto &p = *impl_;
    if (p.hwnd || !parent)
        return false;
    p.parent = parent;
    p.theme = &theme;
    p.ini = ini;
    p.paint_canvas = std::move(paint_canvas);
    p.load();
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Impl::proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SteadyXRMonitorWidget";
    RegisterClassW(&wc);
    const auto r = p.physical();
    if (!CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Framing monitor",
                         WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, r.left, r.top,
                         r.right - r.left, r.bottom - r.top, parent, reinterpret_cast<HMENU>(INT_PTR(id)),
                         wc.hInstance, &p))
        return false;
    p.reset_button = CreateWindowExW(
        0, L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | WS_TABSTOP | theme.style(L"BUTTON", BS_PUSHBUTTON), 0,
        0, 0, 0, p.hwnd, reinterpret_cast<HMENU>(INT_PTR(reset_id)), wc.hInstance, nullptr);
    if (!p.reset_button) {
        DestroyWindow(p.hwnd);
        return false;
    }
    SetPropW(p.reset_button, L"SteadyXRShellZone", reinterpret_cast<HANDLE>(3));
    theme.control(p.reset_button, L"BUTTON");
    p.font(theme.body);
    p.layout();
    p.place();
    return true;
}
HWND MonitorWidget::window() const {
    return impl_->hwnd;
}
void MonitorWidget::set_caption(const std::wstring &text) {
    if (impl_->caption != text) {
        impl_->caption = text;
        invalidate();
    }
}
void MonitorWidget::invalidate() {
    if (impl_->hwnd)
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
void MonitorWidget::reset() {
    if (impl_->hwnd)
        impl_->reset();
}
} // namespace vrst::preview
