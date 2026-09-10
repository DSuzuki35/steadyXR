#include "window_capture.hpp"
#include <dwmapi.h>
#include <roapi.h>
#include <winstring.h>
// MinGW 20260826 aliases WinRT boolean and BYTE to unsigned char, causing its
// unused IReference<boolean> specialization to collide with IReference<BYTE>.
// This translation unit never uses either boxed type; suppress the duplicate.
#ifdef __MINGW32__
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <algorithm>
#include <cwchar>
#include <sstream>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace vrst::preview {
using Microsoft::WRL::ComPtr;
namespace Capture = ABI::Windows::Graphics::Capture;
namespace Direct3D = ABI::Windows::Graphics::DirectX::Direct3D11;
using DxgiAccess = Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
using Size = ABI::Windows::Graphics::SizeInt32;
constexpr auto pixel_format = ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;
namespace {
template <class T> HRESULT factory(const wchar_t *name, ComPtr<T> &out) {
    HSTRING text{};
    HRESULT hr = WindowsCreateString(name, static_cast<UINT32>(std::wcslen(name)), &text);
    if (SUCCEEDED(hr)) {
        hr = RoGetActivationFactory(text, __uuidof(T),
                                    reinterpret_cast<void **>(out.ReleaseAndGetAddressOf()));
        WindowsDeleteString(text);
    }
    return hr;
}
template <class T> void close(ComPtr<T> &value) {
    if (value) {
        ComPtr<ABI::Windows::Foundation::IClosable> closable;
        if (SUCCEEDED(value.As(&closable)))
            closable->Close();
        value.Reset();
    }
}
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous{SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    ~DpiScope() {
        if (previous)
            SetThreadDpiAwarenessContext(previous);
    }
};
bool physical_client(HWND window, const Size &content, D3D11_BOX &box) {
    DpiScope physical;
    RECT client{};
    POINT top{}, bottom{};
    if (!GetClientRect(window, &client))
        return false;
    top = {client.left, client.top};
    bottom = {client.right, client.bottom};
    if (!ClientToScreen(window, &top) || !ClientToScreen(window, &bottom))
        return false;
    RECT bounds[2]{};
    const bool extended =
        SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds[0], sizeof(RECT)));
    const bool outer = GetWindowRect(window, &bounds[1]);
    for (unsigned i = 0; i < 2; ++i) {
        if (!(i ? outer : extended))
            continue;
        const RECT &r = bounds[i];
        // No estimated scale factors: frame pixels and physical bounds must agree.
        if (r.right - r.left != content.Width || r.bottom - r.top != content.Height)
            continue;
        const LONG left = top.x - r.left, up = top.y - r.top, right = bottom.x - r.left,
                   down = bottom.y - r.top;
        if (left < 0 || up < 0 || right <= left || down <= up || right > content.Width ||
            down > content.Height)
            return false;
        box = {static_cast<UINT>(left),  static_cast<UINT>(up),   0,
               static_cast<UINT>(right), static_cast<UINT>(down), 1};
        return true;
    }
    // Borderless capture may already contain precisely the client area.
    if (bottom.x - top.x == content.Width && bottom.y - top.y == content.Height && content.Width > 0 &&
        content.Height > 0) {
        box = {0, 0, 0, static_cast<UINT>(content.Width), static_cast<UINT>(content.Height), 1};
        return true;
    }
    return false;
}
BOOL CALLBACK enumerate(HWND window, LPARAM parameter) {
    auto &result = *reinterpret_cast<std::vector<CaptureWindow> *>(parameter);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (!IsWindowVisible(window) || pid == GetCurrentProcessId() || GetAncestor(window, GA_ROOT) != window)
        return TRUE;
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (style & WS_EX_TOOLWINDOW)
        return TRUE;
    DWORD cloaked{};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;
    const int length = GetWindowTextLengthW(window);
    if (length <= 0 || length > 32767)
        return TRUE;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return TRUE;
    std::wstring title(length + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    if (copied <= 0)
        return TRUE;
    title.resize(copied);
    result.push_back({window, std::move(title)});
    return TRUE;
}
} // namespace
struct WindowCapture::Impl {
    bool apartment{}, active{}, suspended{};
    HWND window{};
    DWORD process{};
    Size size{};
    std::uint64_t last_time{};
    std::wstring error;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<Direct3D::IDirect3DDevice> winrt_device;
    ComPtr<Capture::IGraphicsCaptureItem> item;
    ComPtr<Capture::IDirect3D11CaptureFramePool> pool;
    ComPtr<Capture::IGraphicsCaptureSession> session;
    ~Impl() {
        stop();
        if (apartment)
            RoUninitialize();
    }
    void stop() {
        close(session);
        close(pool);
        item.Reset();
        winrt_device.Reset();
        output.Reset();
        context.Reset();
        device.Reset();
        active = false;
        suspended = false;
        window = nullptr;
        last_time = 0;
    }
    bool fail(const wchar_t *stage, HRESULT hr) {
        std::wostringstream message;
        message << stage << L" (0x" << std::hex << static_cast<unsigned long>(hr) << L")";
        error = message.str();
        stop();
        return false;
    }
    bool create_pool() {
        HRESULT hr = item->get_Size(&size);
        if (FAILED(hr) || size.Width <= 0 || size.Height <= 0)
            return fail(L"Window has no capturable area", FAILED(hr) ? hr : E_INVALIDARG);
        ComPtr<Capture::IDirect3D11CaptureFramePoolStatics2> statics;
        hr = factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", statics);
        if (FAILED(hr))
            return fail(L"Free-threaded window capture unavailable", hr);
        hr = statics->CreateFreeThreaded(winrt_device.Get(), pixel_format, 2, size, &pool);
        if (FAILED(hr))
            return fail(L"Create capture frame pool", hr);
        hr = pool->CreateCaptureSession(item.Get(), &session);
        if (FAILED(hr))
            return fail(L"Create window capture session", hr);
        ComPtr<Capture::IGraphicsCaptureSession2> cursor;
        if (SUCCEEDED(session.As(&cursor)))
            cursor->put_IsCursorCaptureEnabled(false);
        hr = session->StartCapture();
        if (FAILED(hr))
            return fail(L"Start window capture", hr);
        suspended = false;
        return true;
    }
};
WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}
WindowCapture::~WindowCapture() = default;
bool WindowCapture::start(ID3D11Device *device, HWND window) {
    auto &p = *impl_;
    p.stop();
    p.error.clear();
    if (!device || !IsWindow(window) || GetAncestor(window, GA_ROOT) != window) {
        p.error = L"Select an existing top-level preview window";
        return false;
    }
    if (!p.apartment) {
        HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
        if (SUCCEEDED(hr))
            p.apartment = true;
        else if (hr != RPC_E_CHANGED_MODE)
            return p.fail(L"Initialize Windows capture", hr);
    }
    p.device = device;
    p.device->GetImmediateContext(&p.context);
    p.window = window;
    GetWindowThreadProcessId(window, &p.process);
    ComPtr<IDXGIDevice> dxgi;
    HRESULT hr =
        device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(dxgi.GetAddressOf()));
    if (FAILED(hr))
        return p.fail(L"D3D11 capture device", hr);
    ComPtr<IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &inspectable);
    if (FAILED(hr))
        return p.fail(L"Wrap D3D11 capture device", hr);
    hr = inspectable.As(&p.winrt_device);
    if (FAILED(hr))
        return p.fail(L"Windows capture D3D11 interface", hr);
    ComPtr<IGraphicsCaptureItemInterop> interop;
    hr = factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", interop);
    if (FAILED(hr))
        return p.fail(L"Windows graphics capture unavailable", hr);
    hr = interop->CreateForWindow(window, __uuidof(Capture::IGraphicsCaptureItem),
                                  reinterpret_cast<void **>(p.item.GetAddressOf()));
    if (FAILED(hr))
        return p.fail(L"Capture selected window", hr);
    p.active = true;
    if (IsIconic(window) || !IsWindowVisible(window)) {
        p.suspended = true;
        p.error = L"Preview window is minimized or hidden";
        return true;
    }
    return p.create_pool();
}
void WindowCapture::stop() {
    impl_->stop();
    impl_->error.clear();
}
bool WindowCapture::running() const {
    return impl_->active;
}
std::wstring WindowCapture::error() const {
    return impl_->error;
}
std::vector<CaptureWindow> WindowCapture::windows() {
    std::vector<CaptureWindow> result;
    EnumWindows(enumerate, reinterpret_cast<LPARAM>(&result));
    return result;
}
bool WindowCapture::poll(ComPtr<ID3D11Texture2D> &texture, std::uint64_t &time_ns) {
    auto &p = *impl_;
    texture.Reset();
    time_ns = 0;
    if (!p.active)
        return false;
    DWORD process{};
    GetWindowThreadProcessId(p.window, &process);
    if (!IsWindow(p.window) || process != p.process) {
        p.error = L"Captured window was closed";
        p.stop();
        return false;
    }
    if (IsIconic(p.window) || !IsWindowVisible(p.window)) {
        if (!p.suspended) {
            close(p.session);
            close(p.pool);
            p.suspended = true;
        }
        p.error = L"Preview window is minimized or hidden";
        return false;
    }
    if (p.suspended && !p.create_pool())
        return false;
    HRESULT hr = p.device->GetDeviceRemovedReason();
    if (FAILED(hr))
        return p.fail(L"Capture GPU device was removed", hr);
    ComPtr<Capture::IDirect3D11CaptureFrame> frame;
    hr = p.pool->TryGetNextFrame(&frame);
    if (FAILED(hr))
        return p.fail(L"Read captured frame", hr);
    if (!frame)
        return false;
    // Drain a bounded pool to minimize delay; retire every skipped frame explicitly.
    for (int i = 0; i < 2; ++i) {
        ComPtr<Capture::IDirect3D11CaptureFrame> newer;
        hr = p.pool->TryGetNextFrame(&newer);
        if (FAILED(hr)) {
            close(frame);
            return p.fail(L"Read latest captured frame", hr);
        }
        if (!newer)
            break;
        close(frame);
        frame = std::move(newer);
    }
    Size content{};
    ABI::Windows::Foundation::TimeSpan stamp{};
    hr = frame->get_ContentSize(&content);
    if (SUCCEEDED(hr))
        hr = frame->get_SystemRelativeTime(&stamp);
    if (FAILED(hr)) {
        close(frame);
        return p.fail(L"Read capture metadata", hr);
    }
    if (content.Width <= 0 || content.Height <= 0) {
        close(frame);
        return false;
    }
    if (content.Width != p.size.Width || content.Height != p.size.Height) {
        close(frame);
        p.size = content;
        hr = p.pool->Recreate(p.winrt_device.Get(), pixel_format, 2, p.size);
        if (FAILED(hr))
            return p.fail(L"Resize capture frame pool", hr);
        p.error = L"Resizing preview capture";
        return false;
    }
    D3D11_BOX crop{};
    if (!physical_client(p.window, content, crop)) {
        close(frame);
        p.error = L"Waiting for matching window/client pixel bounds";
        return false;
    }
    if (stamp.Duration <= 0 || static_cast<std::uint64_t>(stamp.Duration) * 100 <= p.last_time) {
        close(frame);
        return false;
    }
    ComPtr<Direct3D::IDirect3DSurface> surface;
    ComPtr<DxgiAccess> access;
    ComPtr<ID3D11Texture2D> source;
    hr = frame->get_Surface(&surface);
    if (SUCCEEDED(hr))
        hr = surface.As(&access);
    if (SUCCEEDED(hr))
        hr =
            access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(source.GetAddressOf()));
    if (FAILED(hr)) {
        source.Reset();
        access.Reset();
        surface.Reset();
        close(frame);
        return p.fail(L"Read capture GPU texture", hr);
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (crop.right > desc.Width || crop.bottom > desc.Height) {
        source.Reset();
        access.Reset();
        surface.Reset();
        close(frame);
        p.error = L"Waiting for resized capture texture";
        return false;
    }
    const UINT width = crop.right - crop.left, height = crop.bottom - crop.top;
    D3D11_TEXTURE2D_DESC current{};
    if (p.output)
        p.output->GetDesc(&current);
    if (!p.output || current.Width != width || current.Height != height || current.Format != desc.Format) {
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = desc.MiscFlags = 0;
        p.output.Reset();
        hr = p.device->CreateTexture2D(&desc, nullptr, &p.output);
        if (FAILED(hr)) {
            source.Reset();
            access.Reset();
            surface.Reset();
            close(frame);
            return p.fail(L"Create private capture texture", hr);
        }
    }
    p.context->CopySubresourceRegion(p.output.Get(), 0, 0, 0, 0, source.Get(), 0, &crop);
    p.context->Flush();
    // No frame-pool surface remains referenced after returning ownership to WGC.
    source.Reset();
    access.Reset();
    surface.Reset();
    close(frame);
    texture = p.output;
    time_ns = static_cast<std::uint64_t>(stamp.Duration) * 100;
    p.last_time = time_ns;
    p.error.clear();
    return true;
}
} // namespace vrst::preview
