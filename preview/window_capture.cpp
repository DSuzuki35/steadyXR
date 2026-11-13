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
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <thread>
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
// Capture shutdown may need messages on the creating UI thread. Keep it off that
// thread, and allow only one active capture / retired resource bundle per process.
struct ClosedSignal {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ClosedSignal() {
        if (event)
            CloseHandle(event);
    }
};
using ClosedDelegate =
    ABI::Windows::Foundation::ITypedEventHandler<Capture::GraphicsCaptureItem *, IInspectable *>;
class ClosedHandler final : public ClosedDelegate {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<ClosedSignal> signal;

  public:
    explicit ClosedHandler(std::shared_ptr<ClosedSignal> value) : signal(std::move(value)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) || iid == __uuidof(ClosedDelegate)) {
            *out = static_cast<ClosedDelegate *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs;
        if (!left)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Capture::IGraphicsCaptureItem *, IInspectable *) override {
        SetEvent(signal->event);
        return S_OK;
    }
};
struct RetiredCapture {
    HWND window{};
    std::shared_ptr<ClosedSignal> closed;
    ULONGLONG started{GetTickCount64()};
    ComPtr<Capture::IDirect3D11CaptureFramePool> pool;
    ComPtr<Capture::IGraphicsCaptureSession> session;
    ComPtr<Capture::IGraphicsCaptureItem> item;
    ComPtr<Direct3D::IDirect3DDevice> winrt_device;
    ComPtr<ID3D11Device> device;
    std::atomic<bool> done{false};
};
std::mutex lifecycle_mutex;
void *active_capture{};
std::shared_ptr<RetiredCapture> retired_capture;
bool cleanup_pending() {
    std::lock_guard<std::mutex> guard(lifecycle_mutex);
    return retired_capture && !retired_capture->done.load(std::memory_order_acquire);
}
std::wstring cleanup_status() {
    std::lock_guard<std::mutex> guard(lifecycle_mutex);
    if (retired_capture && !retired_capture->done.load(std::memory_order_acquire) &&
        GetTickCount64() - retired_capture->started >= 5000)
        return L"Windows capture shutdown stalled; restart SteadyXR Preview (iRacing can stay running)";
    return L"Finishing previous capture shutdown; retrying";
}
bool claim_capture(void *owner) {
    std::lock_guard<std::mutex> guard(lifecycle_mutex);
    if ((active_capture && active_capture != owner) ||
        (retired_capture && !retired_capture->done.load(std::memory_order_acquire)))
        return false;
    retired_capture.reset();
    active_capture = owner;
    return true;
}
void release_capture(void *owner) {
    std::lock_guard<std::mutex> guard(lifecycle_mutex);
    if (active_capture == owner)
        active_capture = nullptr;
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
    result.push_back({window, std::move(title), pid});
    return TRUE;
}

// Desktop Duplication has no WGC recording border. Acquire and cross-adapter
// readback are both polled with zero wait, on the same UI thread as the renderer.
struct DesktopCapture {
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Device> capture_device, render_device;
    ComPtr<ID3D11DeviceContext> capture_context, render_context;
    ComPtr<ID3D11Texture2D> output, staging;
    ComPtr<ID3D11ShaderResourceView> cursor;
    MonitorFrameInfo info{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    bool pointer_visible{}, transfer_pending{};
    std::uint64_t stamp{};

    HRESULT start(ID3D11Device *device, HMONITOR monitor) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> render_adapter;
        DXGI_ADAPTER_DESC render_desc{};
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (FAILED(hr) || FAILED(hr = dxgi->GetAdapter(&render_adapter)) ||
            FAILED(hr = render_adapter->GetDesc(&render_desc)))
            return hr;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return hr;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(hr = factory->EnumAdapters1(i, &adapter)))
                return hr;
            for (UINT j = 0;; ++j) {
                ComPtr<IDXGIOutput> candidate;
                hr = adapter->EnumOutputs(j, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                DXGI_OUTPUT_DESC desc{};
                if (FAILED(hr) || FAILED(hr = candidate->GetDesc(&desc)))
                    return hr;
                if (desc.Monitor != monitor || !desc.AttachedToDesktop)
                    continue;
                DXGI_ADAPTER_DESC1 adapter_desc{};
                if (FAILED(hr = adapter->GetDesc1(&adapter_desc)))
                    return hr;
                render_device = device;
                device->GetImmediateContext(&render_context);
                if (adapter_desc.AdapterLuid.LowPart == render_desc.AdapterLuid.LowPart &&
                    adapter_desc.AdapterLuid.HighPart == render_desc.AdapterLuid.HighPart) {
                    capture_device = render_device;
                    capture_context = render_context;
                } else {
                    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                           &capture_device, nullptr, &capture_context);
                    if (FAILED(hr))
                        return hr;
                }
                ComPtr<IDXGIOutput1> selected;
                if (FAILED(hr = candidate.As(&selected)) ||
                    FAILED(hr = selected->DuplicateOutput(capture_device.Get(), &duplication)))
                    return hr;
                DXGI_OUTDUPL_DESC mode{};
                duplication->GetDesc(&mode);
                info.rotation = mode.Rotation;
                return S_OK;
            }
        }
    }
    HRESULT update_pointer(const DXGI_OUTDUPL_FRAME_INFO &frame) {
        if (frame.LastMouseUpdateTime.QuadPart) {
            info.pointer = frame.PointerPosition.Position;
            pointer_visible = frame.PointerPosition.Visible != FALSE;
        }
        if (frame.PointerShapeBufferSize) {
            std::vector<BYTE> bytes(frame.PointerShapeBufferSize);
            UINT required{};
            HRESULT hr =
                duplication->GetFramePointerShape(UINT(bytes.size()), bytes.data(), &required, &shape);
            if (FAILED(hr))
                return hr;
            const bool mono = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
            const UINT height = mono ? shape.Height / 2 : shape.Height;
            if (!shape.Width || !height || shape.Width > 16384 || height > 16384 ||
                std::uint64_t(shape.Pitch) * shape.Height > bytes.size() ||
                (mono ? (shape.Width + 7) / 8 : shape.Width * 4) > shape.Pitch)
                return E_UNEXPECTED;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = shape.Width;
            d.Height = height;
            d.ArraySize = d.MipLevels = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.Usage = D3D11_USAGE_IMMUTABLE;
            D3D11_SUBRESOURCE_DATA data{bytes.data(), shape.Pitch, 0};
            std::vector<std::uint32_t> mask;
            if (mono) {
                mask.resize(std::size_t(d.Width) * height);
                for (UINT y = 0; y < height; ++y) {
                    const auto *and_row = bytes.data() + std::size_t(y) * shape.Pitch;
                    const auto *xor_row = and_row + std::size_t(height) * shape.Pitch;
                    for (UINT x = 0; x < d.Width; ++x) {
                        const auto bit = 0x80u >> (x % 8);
                        mask[std::size_t(y) * d.Width + x] = (and_row[x / 8] & bit ? 0xff000000u : 0) |
                                                             (xor_row[x / 8] & bit ? 0x00ffffffu : 0);
                    }
                }
                data = {mask.data(), d.Width * 4, 0};
            }
            ComPtr<ID3D11Texture2D> bitmap;
            cursor.Reset();
            if (FAILED(hr = render_device->CreateTexture2D(&d, &data, &bitmap)) ||
                FAILED(hr = render_device->CreateShaderResourceView(bitmap.Get(), nullptr, &cursor)) ||
                FAILED(hr = output->SetPrivateDataInterface(monitor_pointer_guid, cursor.Get())))
                return hr;
            info.pointer_width = d.Width;
            info.pointer_height = d.Height;
        }
        info.pointer_type = pointer_visible && cursor ? shape.Type : 0;
        return output->SetPrivateData(monitor_info_guid, sizeof(info), &info);
    }
    HRESULT copy_frame(IDXGIResource *resource, const DXGI_OUTDUPL_FRAME_INFO &frame) {
        ComPtr<ID3D11Texture2D> source;
        HRESULT hr = resource->QueryInterface(IID_PPV_ARGS(&source));
        if (FAILED(hr))
            return hr;
        if (!output) {
            D3D11_TEXTURE2D_DESC d{};
            source->GetDesc(&d);
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = d.MiscFlags = 0;
            if (FAILED(hr = render_device->CreateTexture2D(&d, nullptr, &output)))
                return hr;
            if (capture_device.Get() != render_device.Get()) {
                d.Usage = D3D11_USAGE_STAGING;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                d.BindFlags = 0;
                if (FAILED(hr = capture_device->CreateTexture2D(&d, nullptr, &staging)))
                    return hr;
            }
        }
        if (FAILED(hr = update_pointer(frame)))
            return hr;
        // A pointer-only update changes metadata and reuses the last desktop pixels.
        if (frame.LastPresentTime.QuadPart || !stamp) {
            capture_context->CopyResource(staging ? staging.Get() : output.Get(), source.Get());
            transfer_pending = staging != nullptr;
            if (transfer_pending)
                capture_context->Flush();
        }
        LARGE_INTEGER frequency{}, now{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&now);
        stamp = std::uint64_t(double(now.QuadPart) * 1.e9 / double(frequency.QuadPart));
        return S_OK;
    }
    HRESULT poll() {
        if (!transfer_pending) {
            DXGI_OUTDUPL_FRAME_INFO frame{};
            ComPtr<IDXGIResource> source;
            HRESULT hr = duplication->AcquireNextFrame(0, &frame, &source);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                return S_FALSE;
            if (FAILED(hr))
                return hr;
            hr = copy_frame(source.Get(), frame);
            source.Reset();
            const HRESULT released = duplication->ReleaseFrame();
            if (FAILED(hr) || FAILED(released))
                return FAILED(hr) ? hr : released;
        }
        if (transfer_pending) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr =
                capture_context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
                return S_FALSE;
            if (FAILED(hr))
                return hr;
            render_context->UpdateSubresource(output.Get(), 0, nullptr, mapped.pData, mapped.RowPitch, 0);
            capture_context->Unmap(staging.Get(), 0);
            transfer_pending = false;
        }
        return render_device->GetDeviceRemovedReason();
    }
};
} // namespace
struct WindowCapture::Impl {
    std::unique_ptr<DesktopCapture> desktop;
    bool apartment{}, active{}, suspended{}, restarting{};
    HWND window{};
    DWORD process{};
    Size size{};
    std::uint64_t last_time{};
    std::shared_ptr<ClosedSignal> closed;
    EventRegistrationToken closed_token{};
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
    void retire() {
        if (!pool && !session)
            return;
        auto job = std::make_shared<RetiredCapture>();
        // MinGW WRL move-assignment copies; Detach explicitly transfers ownership.
        job->pool.Attach(pool.Detach());
        job->session.Attach(session.Detach());
        job->item = item;
        job->winrt_device = winrt_device;
        job->device = device;
        job->window = window;
        job->closed = closed;
        {
            std::lock_guard<std::mutex> guard(lifecycle_mutex);
            retired_capture = job;
        }
        std::thread([job] {
            const HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
            // A destroyed HWND is not yet necessarily acknowledged by the capture service.
            if (job->closed && !IsWindow(job->window))
                WaitForSingleObject(job->closed->event, 2000);
            // The item owns its signal-only handler until final release. Keeping it
            // registered also covers source closure after resize or suspension.
            close(job->pool);
            close(job->session);
            job->item.Reset();
            job->winrt_device.Reset();
            job->device.Reset();
            if (SUCCEEDED(init))
                RoUninitialize();
            job->done.store(true, std::memory_order_release);
        }).detach();
    }
    void stop() {
        desktop.reset();
        active = false;
        suspended = false;
        restarting = false;
        retire();
        release_capture(this);
        closed.reset();
        closed_token = {};
        item.Reset();
        winrt_device.Reset();
        output.Reset();
        context.Reset();
        device.Reset();
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
    bool create_pool(const Size *desired = nullptr) {
        HRESULT hr = S_OK;
        if (desired)
            size = *desired;
        else
            hr = item->get_Size(&size);
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
        restarting = false;
        return true;
    }
};
WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}
WindowCapture::~WindowCapture() = default;
bool WindowCapture::start_monitor(ID3D11Device *device, HMONITOR monitor) {
    auto &p = *impl_;
    stop();
    if (!device || !monitor) {
        p.error = L"Select a connected display";
        return false;
    }
    p.desktop = std::make_unique<DesktopCapture>();
    const HRESULT hr = p.desktop->start(device, monitor);
    if (FAILED(hr))
        return p.fail(L"Start borderless desktop duplication on selected display", hr);
    p.active = true;
    return true;
}
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
    if (!claim_capture(&p)) {
        p.error = cleanup_status();
        return false;
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
    p.closed = std::make_shared<ClosedSignal>();
    if (!p.closed->event)
        return p.fail(L"Create capture closure signal", E_OUTOFMEMORY);
    ComPtr<ClosedDelegate> handler;
    handler.Attach(new ClosedHandler(p.closed));
    hr = p.item->add_Closed(handler.Get(), &p.closed_token);
    if (FAILED(hr))
        return p.fail(L"Observe capture-window closure", hr);
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
    if (p.desktop) {
        const HRESULT hr = p.desktop->poll();
        if (FAILED(hr))
            return p.fail(L"Read selected display with desktop duplication", hr);
        if (hr == S_FALSE)
            return false;
        texture = p.desktop->output;
        time_ns = p.desktop->stamp;
        return true;
    }
    DWORD process{};
    GetWindowThreadProcessId(p.window, &process);
    if (!IsWindow(p.window) || process != p.process) {
        p.error = L"Captured window was closed";
        p.stop();
        return false;
    }
    if (IsIconic(p.window) || !IsWindowVisible(p.window)) {
        if (!p.suspended) {
            p.retire();
            p.suspended = true;
        }
        p.error = L"Preview window is minimized or hidden";
        return false;
    }
    if (p.suspended || p.restarting) {
        if (cleanup_pending()) {
            p.error = cleanup_status();
            return false;
        }
        const Size desired = p.size;
        if (!p.create_pool(p.restarting ? &desired : nullptr))
            return false;
    }
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
        // A new session requests a fresh snapshot even if the resized source is static.
        p.retire();
        p.restarting = true;
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
