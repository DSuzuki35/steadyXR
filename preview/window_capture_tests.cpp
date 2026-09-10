#include "window_capture.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dwmapi.h>
#include <functional>
#include <thread>
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
    if (m == WM_APP + 1) {
        auto prior = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
        HWND legacy = CreateWindowExW(WS_EX_NOACTIVATE, L"HorizonLockWindowCaptureTest",
                                      L"Horizon Lock legacy DPI test (owned)", WS_OVERLAPPEDWINDOW, 90, 90,
                                      360, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetThreadDpiAwarenessContext(prior);
        return reinterpret_cast<LRESULT>(legacy);
    }
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
// Use a separate, continuously pumping source process, as the simulator is.
struct SourceProcess {
    PROCESS_INFORMATION process{};
    HANDLE job{};
    HWND window{};
    SourceProcess() {
        wchar_t exe[MAX_PATH]{};
        CHECK(GetModuleFileNameW(nullptr, exe, MAX_PATH));
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --source-window";
        STARTUPINFOW info{};
        info.cb = sizeof(info);
        job = CreateJobObjectW(nullptr, nullptr);
        CHECK(job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
        CHECK(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &info, &process));
        CHECK(AssignProcessToJobObject(job, process.hProcess));
        CHECK(ResumeThread(process.hThread) != DWORD(-1));
        auto until = GetTickCount64() + 5000;
        do {
            EnumWindows(
                [](HWND window, LPARAM ptr) -> BOOL {
                    auto &self = *reinterpret_cast<SourceProcess *>(ptr);
                    DWORD pid{};
                    GetWindowThreadProcessId(window, &pid);
                    if (pid == self.process.dwProcessId && IsWindowVisible(window)) {
                        self.window = window;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(this));
            if (!window)
                Sleep(10);
        } while (!window && GetTickCount64() < until);
        CHECK(window);
    }
    ~SourceProcess() {
        PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
        CHECK(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
    }
};
void destructor_probe(HANDLE job, HWND source, bool close_source) {
    wchar_t exe[MAX_PATH]{};
    CHECK(GetModuleFileNameW(nullptr, exe, MAX_PATH));
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --destructor-probe " +
                           std::to_wstring(reinterpret_cast<std::uintptr_t>(source)) +
                           (close_source ? L" close" : L"");
    STARTUPINFOW info{};
    info.cb = sizeof(info);
    PROCESS_INFORMATION child{};
    CHECK(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                         nullptr, nullptr, &info, &child));
    CHECK(AssignProcessToJobObject(job, child.hProcess));
    CHECK(ResumeThread(child.hThread) != DWORD(-1));
    CHECK(WaitForSingleObject(child.hProcess, 8000) == WAIT_OBJECT_0);
    DWORD result{};
    CHECK(GetExitCodeProcess(child.hProcess, &result));
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CHECK(result == 0);
}
int destructor_main(HWND source, bool close_source) {
    ComPtr<ID3D11Device> device;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                      &device, nullptr, nullptr)));
    {
        // No other apartment owner; destroy immediately without waiting for a frame.
        WindowCapture sole;
        CHECK(sole.start(device.Get(), source));
        if (close_source)
            SendMessageW(source, WM_CLOSE, 0, 0);
    }
    std::fputs("WGC: sole capture destructor returned\n", stderr);
    return 0;
}
int source_main() {
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
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
void pump() {
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
bool start_capture(WindowCapture &capture, ID3D11Device *device, HWND window) {
    auto until = GetTickCount64() + 5000;
    do {
        if (capture.start(device, window))
            return true;
        pump();
        Sleep(5);
    } while (GetTickCount64() < until);
    std::fwprintf(stderr, L"start: %ls\n", capture.error().c_str());
    return false;
}
bool check_pixels(ID3D11Device *d, ID3D11Texture2D *image, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height)
        return false;
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
    // WGC can observe an intermediate compositor frame during GDI painting.
    const bool matched = pixel(2, 2) == 0xff && pixel(width / 4, height / 2) == 0xff0000 &&
                         pixel(width * 3 / 4, height / 2) == 0xff00 && pixel(width - 12, height - 3) == 0xff;
    c->Unmap(staging.Get(), 0);
    return matched;
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
        // The source was painted once; resizing must request a fresh static snapshot.
        pump();
        if (capture.poll(texture, time)) {
            CHECK(time > 0);
            if (check_pixels(device, texture.Get(), client.right, client.bottom))
                return time;
        }
        Sleep(10);
    }
    std::fwprintf(stderr, L"capture: %ls\n", capture.error().c_str());
    CHECK(false);
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--source-window") == 0)
        return source_main();
    if (argc > 2 && std::strcmp(argv[1], "--destructor-probe") == 0)
        return destructor_main(reinterpret_cast<HWND>(std::strtoull(argv[2], nullptr, 10)), argc > 3);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = L"HorizonLockWindowCaptureTest";
    CHECK(RegisterClassW(&wc));
    RECT rect{0, 0, 320, 180};
    CHECK(AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOACTIVATE));
    SourceProcess source;
    HWND window = source.window;
    CHECK(window);
    std::fputs("WGC: sole-instance destruction before first frame (live and closed source)\n", stderr);
    destructor_probe(source.job, window, false);
    HWND disposable = reinterpret_cast<HWND>(SendMessageW(window, WM_APP + 1, 0, 0));
    CHECK(disposable);
    ShowWindow(disposable, SW_SHOWNOACTIVATE);
    UpdateWindow(disposable);
    destructor_probe(source.job, disposable, true);
    CHECK(!IsWindow(disposable));
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
    CHECK(start_capture(capture, device.Get(), window));
    CHECK(capture.running());
    std::fputs("WGC: initial frame and client crop\n", stderr);
    auto first = await_frame(capture, device.Get(), window);
    auto listed = WindowCapture::windows();
    CHECK(std::any_of(listed.begin(), listed.end(),
                      [&](const CaptureWindow &w) { return w.handle == window; }));
    CHECK(SetWindowPos(window, nullptr, 0, 0, 500, 350, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    InvalidateRect(window, nullptr, TRUE);
    UpdateWindow(window);
    DwmFlush();
    std::fputs("WGC: resized frame\n", stderr);
    auto resized = await_frame(capture, device.Get(), window);
    CHECK(resized > first);
    std::fputs("WGC: minimize/resume\n", stderr);
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
    std::fputs("WGC: repeated start/stop and handle check\n", stderr);
    for (int i = 0; i < 3; ++i) {
        CHECK(start_capture(capture, device.Get(), window));
        await_frame(capture, device.Get(), window);
        capture.stop();
        CHECK(!capture.running());
    }
    DWORD base_handles{};
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &base_handles));
    auto base_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
         base_user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int i = 0; i < 8; ++i) {
        CHECK(start_capture(capture, device.Get(), window));
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
    HWND legacy = reinterpret_cast<HWND>(SendMessageW(window, WM_APP + 1, 0, 0));
    CHECK(legacy);
    ShowWindow(legacy, SW_SHOWNOACTIVATE);
    UpdateWindow(legacy);
    DwmFlush();
    CHECK(start_capture(capture, device.Get(), legacy));
    await_frame(capture, device.Get(), legacy);
    capture.stop();

    std::fputs("WGC: stop before first frame while source is alive\n", stderr);
    CHECK(start_capture(capture, device.Get(), window));
    capture.stop();
    CHECK(!capture.running());
    std::fputs("WGC: destroy source immediately after capture starts\n", stderr);
    CHECK(start_capture(capture, device.Get(), window));
    SendMessageW(window, WM_CLOSE, 0, 0);
    CHECK(!IsWindow(window));
    pump();
    CHECK(!capture.poll(output, time));
    CHECK(!capture.running());
    CHECK(!capture.error().empty());
    std::fputs("WGC: reuse after destroyed-source retirement\n", stderr);
    {
        WindowCapture next;
        CHECK(start_capture(next, device.Get(), legacy));
        await_frame(next, device.Get(), legacy);
        next.stop();
        next.stop();
    }
    SendMessageW(legacy, WM_CLOSE, 0, 0);
    CHECK(!IsWindow(legacy));
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::puts("PASS: WGC owned-window GPU capture; exact client crop; resize; minimize/resume; stop/restart; "
              "closed window; candidate filtering");
}
