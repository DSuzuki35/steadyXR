#include "overlay.hpp"
#include "vrst/timing_overlay.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
HWND overlay_window() {
    struct Search { DWORD pid; HWND window; } search{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto &search = *reinterpret_cast<Search *>(param);
        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        wchar_t name[128]{};
        GetClassNameW(window, name, 128);
        if (pid == search.pid && std::wstring(name) == L"VRStabilizerPhotoTiming")
            search.window = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}
std::vector<unsigned char> capture(HWND window, unsigned &width, unsigned &height,
                                   const vrst::OverlayBitmap *reference = nullptr) {
    RECT rect{};
    GetClientRect(window, &rect);
    width = static_cast<unsigned>(rect.right);
    height = static_cast<unsigned>(rect.bottom);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    auto dc = CreateCompatibleDC(nullptr);
    void *bits{};
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    if (reference) {
        auto bgra = reference->rgba;
        for (std::size_t p = 0; p < bgra.size(); p += 4)
            std::swap(bgra[p], bgra[p + 2]);
        auto source_info = info;
        source_info.bmiHeader.biWidth = static_cast<LONG>(reference->width);
        source_info.bmiHeader.biHeight = -static_cast<LONG>(reference->height);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, width, height, 0, 0, reference->width, reference->height,
                      bgra.data(), &source_info, DIB_RGB_COLORS, SRCCOPY);
    } else
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    auto *bytes = static_cast<unsigned char *>(bits);
    std::vector<unsigned char> result(bytes, bytes + width * height * 4);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return result;
}
void expect_panel(HWND window, const vrst::TimingOverlayData &data) {
    unsigned width{}, height{};
    const auto actual = capture(window, width, height);
    const auto expected = vrst::render_timing_overlay(data);
    check(width && height, "native client area must be nonempty");
    const auto scaled_expected = capture(window, width, height, &expected);
    for (std::size_t p = 0; p < actual.size(); p += 4)
        check(actual[p] == scaled_expected[p] && actual[p + 1] == scaled_expected[p + 1] &&
              actual[p + 2] == scaled_expected[p + 2], "desktop pixels must represent the actual published pose/state");
}
}

int main() {
    try {
        const auto previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto foreground = GetForegroundWindow();
        {
            vrst::sender::DesktopOverlay disabled(false);
            disabled.refresh(1000000000ULL, 42);
            check(!overlay_window(), "default/headless sender must not create a window");
        }
        {
            vrst::sender::DesktopOverlay overlay(true);
            auto window = overlay_window();
            check(window && IsWindowVisible(window), "enabled overlay must create a visible desktop window");
            check(GetForegroundWindow() == foreground, "showing overlay must not steal focus");
            const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            check((style & WS_EX_NOACTIVATE) && (style & WS_EX_TOPMOST), "overlay must stay topmost without activation");
            check(SendMessageW(window, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "clicking overlay must not activate it");
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
            RECT rect{};
            GetWindowRect(window, &rect);
            check(rect.left >= monitor.rcWork.left && rect.top >= monitor.rcWork.top &&
                      rect.right <= monitor.rcWork.right && rect.bottom <= monitor.rcWork.bottom,
                  "photo overlay must begin inside primary monitor work area");
            vrst::TimingOverlayData initial;
            initial.sender_view = initial.connected = initial.clock_ready = true;
            initial.now_ns = 980000000ULL;
            initial.session = 42;
            overlay.refresh(initial.now_ns, initial.session);
            expect_panel(window, initial);

            vrst::Pose pose;
            pose.session = 42;
            pose.sequence = 981;
            pose.time_ns = 1000000000ULL;
            pose.flags = vrst::TrackingValid | vrst::FramePose | vrst::DemoPose;
            overlay.published(pose); // No UDP subscriber exists in this test.
            overlay.refresh(1010000000ULL, 42);
            vrst::TimingOverlayData data;
            data.sender_view = data.latest_present = data.connected = data.clock_ready = true;
            data.now_ns = 1010000000ULL;
            data.session = 42;
            data.latest = pose;
            expect_panel(window, data);

            unsigned width{}, height{};
            const auto before = capture(window, width, height);
            overlay.refresh(1010000001ULL, 42);
            check(before == capture(window, width, height), "refresh calls inside 1/60 second must not rerasterize");

            pose.flags = 0;
            pose.sequence = 982;
            pose.time_ns = 1030000000ULL;
            overlay.published(pose);
            overlay.refresh(1040000000ULL, 42);
            data.now_ns = 1040000000ULL;
            data.latest = pose;
            expect_panel(window, data);

            overlay.refresh(2040000000ULL, 42);
            data.now_ns = 2040000000ULL;
            expect_panel(window, data);
            overlay.refresh(2080000000ULL, 99);
            data.now_ns = 2080000000ULL;
            data.session = 99;
            data.latest_present = false;
            data.latest = {};
            expect_panel(window, data);

            // Native caption dragging must return immediately rather than enter
            // DefWindowProc's modal move loop and pause pose acquisition.
            const auto start = GetTickCount64();
            SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            check(GetTickCount64() - start < 100, "starting a drag must not block tracking");
            SendMessageW(window, WM_LBUTTONUP, 0, 0);
            check(GetForegroundWindow() == foreground, "painting and dragging overlay must not steal focus");
            PostMessageW(window, WM_CLOSE, 0, 0);
            overlay.refresh(2120000000ULL, 99);
            check(!IsWindow(window), "close must dismiss the overlay");
            overlay.published(pose);
            overlay.refresh(2160000000ULL, 42);
            check(!overlay_window(), "future poses must not reopen a dismissed overlay");
            MSG message{};
            check(!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), "overlay close must not stop the sender message loop");
        }
        {
            vrst::sender::DesktopOverlay overlay(true);
            check(overlay_window() != nullptr, "overlay class must permit a second lifecycle");
        }
        check(!overlay_window(), "destructor must clean up the native window");
        check(GetForegroundWindow() == foreground, "overlay lifecycle must preserve foreground window");
        SetThreadDpiAwarenessContext(previous_dpi);
        std::cout << "PASS headless default, native pixels for unpublished/subscriber-free/invalid/stale/reset states, refresh bound, focus, primary placement, nonblocking drag, close and resource lifecycle\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
