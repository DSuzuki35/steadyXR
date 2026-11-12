#include "framing_editor.hpp"
#include "crop_geometry.hpp"
#include "framing_card.hpp"
#include <commctrl.h>
#include <cwchar>
#include <windowsx.h>

namespace vrst::preview {
namespace {
constexpr int capture_id = 60, apply_id = 61, first_slider = 70;
constexpr double Options::*fields[]{&Options::crop, &Options::horizontal_offset, &Options::vertical_offset};
constexpr const wchar_t *names[]{L"Crop reserve", L"Horizontal framing", L"Vertical framing"};
void fill(HDC dc, RECT r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
} // namespace

struct FramingEditor::Impl {
    HWND owner{}, window{}, canvas{}, note{}, apply_button{}, sliders[3]{}, values[3]{};
    Theme *theme{};
    Renderer *renderer{};
    std::function<Options()> current;
    std::function<void(const Options &)> apply;
    std::function<bool()> can_capture;
    Options draft;
    FramingStill still;
    bool pending{}, moving{};
    std::optional<CropHandle> handle;
    POINT start{};
    FramingRegion drag_region{};
    FramingCard editor_card, main_card;
    std::uint64_t revision{};
    std::wstring message{L"Capture a full-eye screenshot from the running source."};

    ~Impl() {
        if (IsWindow(window))
            DestroyWindow(window);
    }
    void status(std::wstring text) {
        message = std::move(text);
        if (note)
            SetWindowTextW(note, message.c_str());
    }
    void reset_draft() {
        draft = current();
        const double aspect = still.pixels ? still.source_aspect : renderer->source_aspect();
        migrate_legacy_framing(draft, aspect > 0 ? aspect : 1., still.pixels ? still.eye : draft.eye);
    }
    void refresh() {
        if (!window)
            return;
        for (unsigned i = 0; i < 3; ++i) {
            const auto value = draft.*fields[i];
            wchar_t label[24]{};
            std::swprintf(label, std::size(label), L"%.1f%%", value);
            SetWindowTextW(values[i], label);
            SendMessageW(sliders[i], TBM_SETPOS, TRUE, std::lround(value * 10));
            EnableWindow(sliders[i], still.pixels != nullptr);
        }
        EnableWindow(apply_button, still.pixels != nullptr);
        EnableWindow(GetDlgItem(window, capture_id), !pending);
        InvalidateRect(canvas, nullptr, FALSE);
    }
    void capture() {
        if (pending || !renderer)
            return;
        if ((can_capture && !can_capture()) || !renderer->has_frame()) {
            status(L"No live image. Start the source, then capture a screenshot.");
            return;
        }
        if (renderer->source_eye() != current().eye) {
            status(L"Waiting for the selected eye. Capture again when its image arrives.");
            return;
        }
        pending = renderer->begin_framing_still();
        status(pending ? L"Capturing full-eye screenshot…" : L"Screenshot could not be captured. Try again.");
        if (window)
            EnableWindow(GetDlgItem(window, capture_id), !pending);
    }
    void tick() {
        if (!pending)
            return;
        const auto result = renderer->poll_framing_still(still);
        if (result == StillStatus::pending)
            return;
        pending = false;
        if (result == StillStatus::ready) {
            ++revision;
            if (window && GetCapture() == canvas)
                ReleaseCapture();
            status(std::wstring(still.window_source ? L"Window"
                                : still.eye         ? L"Right eye"
                                                    : L"Left eye") +
                   (still.views == 2 ? L" · Peripheral + focus" : L"") + L" · Screenshot captured");
        } else
            status(L"Screenshot transfer failed. Capture again when the source is ready.");
        refresh();
    }
    RECT image_rect() const {
        RECT area{};
        GetClientRect(canvas, &area);
        return framing_image_rect(area, still.source_aspect);
    }
    RECT crop_rect(RECT image) const {
        return framing_crop_rect(image, still.source_aspect, draft, still.eye);
    }
    void paint(HDC dc) {
        RECT bounds{};
        GetClientRect(canvas, &bounds);
        if (still.pixels) {
            editor_card.paint(dc, bounds, still, revision, draft, still.source_aspect, still.eye, true);
            return;
        }
        fill(dc, bounds, RGB(15, 18, 23));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(177, 179, 190));
        const auto font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(note, WM_GETFONT, 0, 0)));
        DrawTextW(dc, L"No screenshot yet", -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, font);
    }
    bool hit(POINT point, std::optional<CropHandle> &corner) const {
        corner.reset();
        if (!still.pixels)
            return false;
        auto crop = crop_rect(image_rect());
        const int grip = MulDiv(9, GetDpiForWindow(canvas), 96);
        const bool left = std::abs(point.x - crop.left) <= grip,
                   right = std::abs(point.x - crop.right) <= grip;
        const bool top = std::abs(point.y - crop.top) <= grip,
                   bottom = std::abs(point.y - crop.bottom) <= grip;
        if ((left || right) && (top || bottom)) {
            corner = left ? (top ? CropHandle::top_left : CropHandle::bottom_left)
                          : (top ? CropHandle::top_right : CropHandle::bottom_right);
            return true;
        }
        return PtInRect(&crop, point);
    }
    void drag(POINT point) {
        const auto image = image_rect();
        const auto dx = double(point.x - start.x) / (image.right - image.left);
        const auto dy = double(point.y - start.y) / (image.bottom - image.top);
        const auto r = handle ? resize_crop(drag_region, *handle, dx, dy, still.source_aspect, draft.aspect)
                              : move_crop(drag_region, dx, dy);
        crop_to_options(r, still.source_aspect, draft, still.eye);
        refresh();
    }
    static Impl *instance(HWND w, UINT m, LPARAM l) {
        if (m == WM_NCCREATE)
            SetWindowLongPtrW(
                w, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
        return reinterpret_cast<Impl *>(GetWindowLongPtrW(w, GWLP_USERDATA));
    }
    static LRESULT CALLBACK canvas_proc(HWND w, UINT m, WPARAM a, LPARAM l) {
        auto *p = instance(w, m, l);
        if (!p)
            return DefWindowProcW(w, m, a, l);
        const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PRINTCLIENT:
            p->paint(reinterpret_cast<HDC>(a));
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(w, &ps);
            p->paint(dc);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (p->hit(point, p->handle)) {
                p->moving = true;
                p->start = point;
                p->drag_region = crop_from_options(p->still.source_aspect, p->draft, p->still.eye);
                SetCapture(w);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (p->moving)
                p->drag(point);
            return 0;
        case WM_LBUTTONUP:
            if (p->moving) {
                p->drag(point);
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            p->moving = false;
            return 0;
        case WM_SETCURSOR: {
            POINT at{};
            GetCursorPos(&at);
            ScreenToClient(w, &at);
            std::optional<CropHandle> corner;
            if (p->hit(at, corner)) {
                const auto cursor = !corner ? IDC_SIZEALL
                                    : (*corner == CropHandle::top_left || *corner == CropHandle::bottom_right)
                                        ? IDC_SIZENWSE
                                        : IDC_SIZENESW;
                SetCursor(LoadCursor(nullptr, cursor));
                return TRUE;
            }
            break;
        }
        }
        return DefWindowProcW(w, m, a, l);
    }
    static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM l) {
        auto *p = instance(w, m, l);
        if (!p)
            return DefWindowProcW(w, m, a, l);
        if (m == WM_PAINT || m == WM_PRINTCLIENT) {
            PAINTSTRUCT ps{};
            const auto dc = m == WM_PAINT ? BeginPaint(w, &ps) : reinterpret_cast<HDC>(a);
            RECT bounds{};
            GetClientRect(w, &bounds);
            fill(dc, bounds, RGB(15, 15, 18));
            if (m == WM_PAINT)
                EndPaint(w, &ps);
            return 0;
        }
        if (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(a) == IDCANCEL)) {
            ShowWindow(w, SW_HIDE);
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(a) == capture_id) {
            p->capture();
            return 0;
        }
        if (m == WM_COMMAND && LOWORD(a) == apply_id && p->still.pixels) {
            auto live = p->current();
            for (auto field : fields)
                live.*field = p->draft.*field;
            live.crop_left = live.crop_right = live.crop_top = live.crop_bottom = 0;
            live.edge_reserve[0] = live.edge_reserve[1] = {};
            p->apply(live);
            p->status(L"Framing applied to stream output.");
            return 0;
        }
        if (m == WM_HSCROLL && l)
            for (unsigned i = 0; i < 3; ++i)
                if (reinterpret_cast<HWND>(l) == p->sliders[i]) {
                    p->draft.*fields[i] = SendMessageW(p->sliders[i], TBM_GETPOS, 0, 0) / 10.;
                    p->refresh();
                    return 0;
                }
        LRESULT result{};
        if (p->theme->message(w, m, a, l, result))
            return result;
        return DefWindowProcW(w, m, a, l);
    }
    HWND child(const wchar_t *kind, const wchar_t *label, DWORD style, int x, int y, int w, int h, int id = 0,
               bool heading = false) {
        auto hwnd =
            CreateWindowExW(0, kind, label, WS_VISIBLE | WS_CHILD | theme->style(kind, style), x, y, w, h,
                            window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        SetPropW(hwnd, L"SteadyXRSurface", reinterpret_cast<HANDLE>(1));
        theme->control(hwnd, kind, heading);
        return hwnd;
    }
    bool create() {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpfnWndProc = canvas_proc;
        wc.lpszClassName = L"SteadyXRFramingCanvas";
        RegisterClassW(&wc);
        wc.lpfnWndProc = proc;
        wc.lpszClassName = L"SteadyXRFramingEditor";
        RegisterClassW(&wc);
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        RECT bounds{0, 0, 882, 518}, parent{};
        AdjustWindowRectEx(&bounds, style, FALSE, WS_EX_CONTROLPARENT);
        GetWindowRect(owner, &parent);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"SteadyXR · Framing", style,
                                 parent.left + 40, parent.top + 40, bounds.right - bounds.left,
                                 bounds.bottom - bounds.top, owner, nullptr, wc.hInstance, this);
        if (!window)
            return false;
        theme->titlebar(window);
        child(L"STATIC", L"Full-eye screenshot", 0, 24, 20, 450, 34, 0, true);
        canvas =
            CreateWindowExW(0, L"SteadyXRFramingCanvas", L"Full eye and stream crop", WS_CHILD | WS_VISIBLE,
                            24, 66, 532, 390, window, nullptr, wc.hInstance, this);
        child(L"STATIC", L"Stream framing", 0, 582, 20, 274, 34, 0, true);
        for (unsigned i = 0; i < 3; ++i) {
            const int y = 72 + int(i) * 72;
            child(L"STATIC", names[i], 0, 582, y, 185, 23);
            values[i] = child(L"STATIC", L"", SS_RIGHT, 765, y, 91, 23);
            sliders[i] = child(TRACKBAR_CLASSW, names[i], TBS_HORZ | WS_TABSTOP, 580, y + 25, 278, 32,
                               first_slider + i);
            SendMessageW(sliders[i], TBM_SETRANGE, TRUE,
                         MAKELPARAM(i ? -1000 : 0, i ? 1000 : int(framing_crop_max * 10)));
            SendMessageW(sliders[i], TBM_SETPAGESIZE, 0, 10);
        }
        const auto hint =
            child(L"STATIC", L"Drag the frame to position it. Drag a corner to change the crop.", 0, 582, 294,
                  270, 62);
        SetPropW(hint, L"SteadyXRMuted", reinterpret_cast<HANDLE>(1));
        child(L"BUTTON", L"Capture screenshot", BS_PUSHBUTTON | WS_TABSTOP, 582, 363, 274, 42, capture_id);
        apply_button =
            child(L"BUTTON", L"Apply framing", BS_PUSHBUTTON | WS_TABSTOP, 582, 414, 274, 42, apply_id);
        SetPropW(apply_button, L"SteadyXRPrimary", reinterpret_cast<HANDLE>(1));
        note = child(L"STATIC", message.c_str(), 0, 24, 475, 832, 28);
        SetPropW(note, L"SteadyXRMuted", reinterpret_cast<HANDLE>(1));
        theme->scale_window(window);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            theme->fit_window(window, monitor.rcWork);
        return true;
    }
};

FramingEditor::FramingEditor() : impl_(new Impl) {}
FramingEditor::~FramingEditor() = default;
void FramingEditor::initialize(HWND owner, Theme &theme, Renderer &renderer, std::function<Options()> current,
                               std::function<void(const Options &)> apply,
                               std::function<bool()> can_capture) {
    impl_->owner = owner;
    impl_->theme = &theme;
    impl_->renderer = &renderer;
    impl_->current = std::move(current);
    impl_->apply = std::move(apply);
    impl_->can_capture = std::move(can_capture);
}
void FramingEditor::open() {
    auto &p = *impl_;
    if (!p.renderer || (!p.window && !p.create()))
        return;
    if (!IsWindowVisible(p.window))
        p.reset_draft();
    p.refresh();
    ShowWindow(p.window, SW_SHOWNORMAL);
    SetForegroundWindow(p.window);
    if (!p.still.pixels && !p.pending)
        p.capture();
}
void FramingEditor::capture() {
    impl_->capture();
}
void FramingEditor::tick() {
    auto &p = *impl_;
    p.tick();
    if (p.window && IsWindowVisible(p.window)) {
        const double aspect = p.current().aspect;
        if (aspect != p.draft.aspect) {
            p.draft.aspect = aspect;
            InvalidateRect(p.canvas, nullptr, FALSE);
        }
    }
}
bool FramingEditor::route(MSG &msg) {
    const auto w = impl_->window;
    return w && IsWindowVisible(w) && (msg.hwnd == w || IsChild(w, msg.hwnd)) && IsDialogMessageW(w, &msg);
}
const std::wstring &FramingEditor::status() const {
    return impl_->message;
}
void FramingEditor::paint_card(HDC dc, RECT target, const Options &options, double source_aspect,
                               int selected_eye) const {
    impl_->main_card.paint(dc, target, impl_->still, impl_->revision, options, source_aspect,
                           unsigned(std::clamp(selected_eye, 0, 1)));
}
bool FramingEditor::has_still() const {
    return impl_->still.pixels != nullptr;
}
std::uint64_t FramingEditor::still_revision() const {
    return impl_->revision;
}
std::wstring FramingEditor::still_description() const {
    const auto &still = impl_->still;
    if (!still.pixels)
        return {};
    if (still.window_source)
        return L"Window capture";
    return std::wstring(still.eye ? L"Right eye" : L"Left eye") +
           (still.views == 2 ? L" · Peripheral + focus" : L"");
}
} // namespace vrst::preview
