#include "overlay.hpp"
#include "vrst/timing_overlay.hpp"
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace vrst::sender {
#ifdef _WIN32
namespace {
constexpr wchar_t window_class[] = L"VRStabilizerPhotoTiming";
constexpr auto refresh_interval = 1000000000ULL / 60;
// Scope DPI awareness to this window's operations, leaving the console alone.
class DpiScope {
  public:
    DpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~DpiScope() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
  private:
    DPI_AWARENESS_CONTEXT previous_{};
};
}
struct DesktopOverlay::Impl {
    HWND window{};
    Pose latest{};
    bool latest_present{};
    std::uint64_t refreshed_at{};
    bool rendered{}, dragging{};
    POINT drag_offset{};
    OverlayBitmap bitmap;
    std::vector<std::uint8_t> bgra;

    Impl() {
        const DpiScope dpi;
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = window_proc;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        klass.lpszClassName = window_class;
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Could not register the photo timing window.");
        TimingOverlayData empty;
        empty.sender_view = empty.connected = empty.clock_ready = true;
        set_bitmap(empty);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
            throw std::runtime_error("Could not locate the primary display for photo timing.");
        const auto &area = monitor.rcWork;
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                 window_class, L"VR Stabilizer photo timing - drag outside tracking region",
                                 WS_CAPTION | WS_SYSMENU,
                                 area.left + (area.right - area.left) * 15 / 100,
                                 area.top + (area.bottom - area.top) * 15 / 100,
                                 1, 1, nullptr, nullptr, klass.hInstance, this);
        if (!window)
            throw std::runtime_error("Could not create the photo timing window.");
        resize_to_bitmap();
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    ~Impl() {
        if (window)
            DestroyWindow(window);
    }
    void set_bitmap(const TimingOverlayData &data) {
        bitmap = render_timing_overlay(data);
        bgra = bitmap.rgba;
        for (std::size_t p = 0; p + 3 < bgra.size(); p += 4)
            std::swap(bgra[p], bgra[p + 2]);
    }
    void resize_to_bitmap() {
        if (!window || !bitmap.width || !bitmap.height)
            return;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        const auto &area = monitor.rcWork;
        const auto dpi = std::max(96U, GetDpiForWindow(window));
        // Keep it readable with DPI scaling, while retaining margins inside a
        // typical capture crop. Moving to another display adapts these bounds.
        const double scale = std::min({static_cast<double>(dpi) / 96.0,
            static_cast<double>((area.right - area.left) * 70 / 100) / bitmap.width,
            static_cast<double>((area.bottom - area.top) * 65 / 100) / bitmap.height});
        RECT size{0, 0, std::max(1L, static_cast<LONG>(bitmap.width * scale)),
                         std::max(1L, static_cast<LONG>(bitmap.height * scale))};
        AdjustWindowRectExForDpi(&size, WS_CAPTION | WS_SYSMENU, FALSE,
                                WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, dpi);
        RECT current{};
        GetWindowRect(window, &current);
        const auto width = size.right - size.left, height = size.bottom - size.top;
        const auto x = std::clamp(current.left, area.left, std::max(area.left, area.right - width));
        const auto y = std::clamp(current.top, area.top, std::max(area.top, area.bottom - height));
        if (current.right - current.left != width || current.bottom - current.top != height ||
            current.left != x || current.top != y)
            SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }
    void paint(HDC dc) const {
        if (!bitmap.width || !bitmap.height)
            return;
        RECT client{};
        GetClientRect(window, &client);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(bitmap.width);
        info.bmiHeader.biHeight = -static_cast<LONG>(bitmap.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, bitmap.width, bitmap.height,
                      bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PRINTCLIENT:
            self->paint(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            self->paint(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_NCLBUTTONDOWN:
            if (wparam == HTCLOSE) {
                DestroyWindow(window);
                return 0;
            }
            if (wparam != HTCAPTION)
                return 0;
            [[fallthrough]];
        case WM_LBUTTONDOWN: {
            POINT cursor{};
            RECT bounds{};
            GetCursorPos(&cursor);
            GetWindowRect(window, &bounds);
            self->drag_offset = {cursor.x - bounds.left, cursor.y - bounds.top};
            self->dragging = true;
            SetCapture(window);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (self->dragging) {
                POINT cursor{};
                GetCursorPos(&cursor);
                SetWindowPos(window, nullptr, cursor.x - self->drag_offset.x,
                             cursor.y - self->drag_offset.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (self->dragging) {
                self->dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            self->dragging = false;
            return 0;
        case WM_NCLBUTTONDBLCLK:
        case WM_NCRBUTTONDOWN:
        case WM_CONTEXTMENU:
            return 0; // Native move/system menus would pause the acquisition loop.
        case WM_DPICHANGED: {
            const auto &suggested = *reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window, nullptr, suggested.left, suggested.top,
                         suggested.right - suggested.left, suggested.bottom - suggested.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            self->resize_to_bitmap();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            self->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    void refresh(std::uint64_t now, std::uint64_t session) {
        const DpiScope dpi;
        MSG message{};
        // A busy UI queue may not starve pose sampling or UDP requests.
        for (unsigned count = 0; window && count < 32 &&
                PeekMessageW(&message, window, 0, 0, PM_REMOVE); ++count) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!window || (rendered && now >= refreshed_at && now - refreshed_at < refresh_interval))
            return;
        refreshed_at = now;
        rendered = true;
        if (latest_present && latest.session != session) {
            latest = {};
            latest_present = false;
        }
        TimingOverlayData data;
        data.sender_view = data.connected = data.clock_ready = true;
        data.now_ns = now;
        data.session = session;
        data.latest = latest;
        data.latest_present = latest_present;
        set_bitmap(data);
        resize_to_bitmap();
        InvalidateRect(window, nullptr, FALSE);
    }
};
#else
struct DesktopOverlay::Impl {};
#endif

DesktopOverlay::DesktopOverlay(bool enabled) {
    if (!enabled)
        return;
#ifdef _WIN32
    impl_ = std::make_unique<Impl>();
#else
    throw std::runtime_error("--overlay is available on Windows only.");
#endif
}
DesktopOverlay::~DesktopOverlay() = default;
void DesktopOverlay::published(const Pose &pose) {
#ifdef _WIN32
    if (impl_ && impl_->window) {
        impl_->latest = pose;
        impl_->latest_present = true;
    }
#else
    (void)pose;
#endif
}
void DesktopOverlay::refresh(std::uint64_t now_ns, std::uint64_t session) {
#ifdef _WIN32
    if (impl_)
        impl_->refresh(now_ns, session);
#else
    (void)now_ns;
    (void)session;
#endif
}
} // namespace vrst::sender
