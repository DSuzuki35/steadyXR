#include "window_capture.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dwmapi.h>
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                             \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (0)
namespace {
LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM p, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(w, &ps);
        RECT r{};
        GetClientRect(w, &r);
        HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(dc, &r, blue);
        DeleteObject(blue);
        RECT left{7, 7, r.right / 2, r.bottom - 7}, right{r.right / 2, 7, r.right - 7, r.bottom - 7};
        HBRUSH red = CreateSolidBrush(RGB(255, 0, 0)), green = CreateSolidBrush(RGB(0, 255, 0));
        FillRect(dc, &left, red);
        FillRect(dc, &right, green);
        DeleteObject(red);
        DeleteObject(green);
        EndPaint(w, &ps);
        return 0;
    }
    return DefWindowProcW(w, m, p, l);
}
void pump() {
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
void check_pixels(ID3D11Device *d, ID3D11Texture2D *image, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    CHECK(desc.Width == width && desc.Height == height);
    CHECK(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(desc.Usage == D3D11_USAGE_DEFAULT && desc.CPUAccessFlags == 0);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, &staging)));
    ComPtr<ID3D11DeviceContext> c;
    d->GetImmediateContext(&c);
    c->CopyResource(staging.Get(), image);
    D3D11_MAPPED_SUBRESOURCE map{};
    CHECK(SUCCEEDED(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
    auto pixel = [&](unsigned x, unsigned y) {
        return *reinterpret_cast<const std::uint32_t *>(static_cast<const char *>(map.pData) +
                                                        y * map.RowPitch + x * 4) &
               0xffffffu;
    };
    if (pixel(2, 2) != 0xff || pixel(width / 4, height / 2) != 0xff0000 ||
        pixel(width * 3 / 4, height / 2) != 0xff00)
        std::fprintf(stderr, "pixels corner %06x left %06x right %06x size%ux%u\n", pixel(2, 2),
                     pixel(width / 4, height / 2), pixel(width * 3 / 4, height / 2), width, height);
    CHECK(pixel(2, 2) == 0xff);
    CHECK(pixel(width / 4, height / 2) == 0xff0000);
    CHECK(pixel(width * 3 / 4, height / 2) ==
          0xff00); // Avoid the compositor-rounded corner while checking the bottom client edge.
    CHECK(pixel(width - 12, height - 3) == 0xff);
    c->Unmap(staging.Get(), 0);
}
std::uint64_t await_frame(WindowCapture &capture, ID3D11Device *device, HWND window) {
    RECT client{};
    CHECK(GetClientRect(window, &client));
    POINT top{client.left, client.top}, bottom{client.right, client.bottom};
    CHECK(ClientToScreen(window, &top));
    CHECK(ClientToScreen(window, &bottom));
    client.right = bottom.x - top.x;
    client.bottom = bottom.y - top.y;
    auto until = GetTickCount64() + 5000;
    ComPtr<ID3D11Texture2D> texture;
    std::uint64_t time{};
    while (GetTickCount64() < until) {
        pump();
        if (capture.poll(texture, time)) {
            CHECK(time > 0);
            check_pixels(device, texture.Get(), client.right, client.bottom);
            return time;
        }
        Sleep(10);
    }
    std::fwprintf(stderr, L"capture: %ls\n", capture.error().c_str());
    CHECK(false);
    return 0;
}
} // namespace
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = L"HorizonLockWindowCaptureTest";
    CHECK(RegisterClassW(&wc));
    RECT rect{0, 0, 320, 180};
    CHECK(AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOACTIVATE));
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Horizon Lock capture test (owned)",
                                  WS_OVERLAPPEDWINDOW, 40, 40, rect.right - rect.left, rect.bottom - rect.top,
                                  nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    DwmFlush();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                      &device, &level, &context)));
    WindowCapture capture;
    CHECK(!capture.start(nullptr, window));
    CHECK(!capture.running());
    CHECK(capture.start(device.Get(), window));
    CHECK(capture.running());
    auto first = await_frame(capture, device.Get(), window);
    auto listed = WindowCapture::windows();
    CHECK(std::none_of(listed.begin(), listed.end(),
                       [&](const CaptureWindow &w) { return w.handle == window; }));
    CHECK(SetWindowPos(window, nullptr, 0, 0, 500, 350, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    InvalidateRect(window, nullptr, TRUE);
    UpdateWindow(window);
    DwmFlush();
    auto resized = await_frame(capture, device.Get(), window);
    CHECK(resized > first);
    ShowWindow(window, SW_MINIMIZE);
    pump();
    ComPtr<ID3D11Texture2D> output;
    std::uint64_t time{};
    for (int i = 0; i < 10; ++i) {
        CHECK(!capture.poll(output, time));
        Sleep(10);
    }
    CHECK(capture.running());
    CHECK(!capture.error().empty());
    ShowWindow(window, SW_SHOWNOACTIVATE);
    InvalidateRect(window, nullptr, TRUE);
    UpdateWindow(window);
    DwmFlush();
    CHECK(await_frame(capture, device.Get(), window) > resized);
    capture.stop();
    CHECK(!capture.running());
    CHECK(!capture.poll(output, time));
    for (int i = 0; i < 3; ++i) {
        CHECK(capture.start(device.Get(), window));
        await_frame(capture, device.Get(), window);
        capture.stop();
        CHECK(!capture.running());
    }
    DWORD base_handles{};
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &base_handles));
    auto base_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
         base_user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int i = 0; i < 8; ++i) {
        CHECK(capture.start(device.Get(), window));
        await_frame(capture, device.Get(), window);
        capture.stop();
    }
    Sleep(100);
    pump();
    DWORD handles{};
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
    CHECK(handles <= base_handles + 12);
    CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= base_gdi + 2);
    CHECK(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= base_user + 2);
    // Exercise a legacy DPI-unaware source window as well as our per-monitor-aware source.
    auto prior = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    CHECK(prior);
    HWND legacy =
        CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Horizon Lock legacy DPI test (owned)",
                        WS_OVERLAPPEDWINDOW, 90, 90, 360, 240, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(legacy);
    SetThreadDpiAwarenessContext(prior);
    ShowWindow(legacy, SW_SHOWNOACTIVATE);
    UpdateWindow(legacy);
    DwmFlush();
    CHECK(capture.start(device.Get(), legacy));
    await_frame(capture, device.Get(), legacy);
    capture.stop();
    DestroyWindow(legacy);
    CHECK(capture.start(device.Get(), window));
    DestroyWindow(window);
    pump();
    CHECK(!capture.poll(output, time));
    CHECK(!capture.running());
    CHECK(!capture.error().empty());
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::puts("PASS: WGC owned-window GPU capture; exact client crop; resize; minimize/resume; stop/restart; "
              "closed window; candidate filtering");
}
