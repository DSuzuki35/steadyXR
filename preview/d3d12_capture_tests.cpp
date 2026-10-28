#include "d3d12_capture.hpp"
#include "d3d12_capture_lifetime.hpp"
#include "transport.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11_4.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <limits>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace vrst::preview;

#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                        \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (0)

namespace {
void check_hr(HRESULT hr, const char *operation) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL %s: HRESULT 0x%08lx\n", operation, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}
#define REQUIRE_HR(x) check_hr((x), #x)

void device_failure_preserves_the_other_devices_obligations() {
    using detail::CaptureProgress;
    // Losing the private companion cannot release application-queue work still in flight.
    CaptureProgress reader_lost{4, 2, false, true};
    CHECK(!reader_lost.capture_finished(5));
    CHECK(!reader_lost.finished(5, 3));
    CHECK(!reader_lost.capture_finished(4, true));
    CHECK(reader_lost.reads_finished(3, true, true));
    reader_lost.capture_completed = 5;
    CHECK(reader_lost.finished(5, 3));

    // Losing the application device likewise cannot free a snapshot the companion still reads.
    CaptureProgress capture_lost{4, 2, true, false};
    CHECK(!capture_lost.finished(5, 3));
    CHECK(!capture_lost.finished(5, 2, false, true));
    CHECK(!capture_lost.finished(5, 2, false, false, true));
    CHECK(capture_lost.capture_finished(5, true));
    capture_lost.read_completed = 3;
    CHECK(capture_lost.finished(5, 3));

    // A sentinel fence value is not a completion measurement or proof of either device's removal.
    const auto unknown = std::numeric_limits<std::uint64_t>::max();
    CaptureProgress unknown_capture{unknown, 3, false, false};
    CaptureProgress unknown_read{5, unknown, false, false};
    CHECK(!unknown_capture.finished(5, 3));
    CHECK(!unknown_read.finished(5, 3));
    unknown_capture.reader_removed = true;
    CHECK(!unknown_capture.finished(5, 3));
    unknown_read.capture_removed = true;
    CHECK(!unknown_read.finished(5, 3));
    CaptureProgress no_work{unknown, unknown, false, false};
    CHECK(no_work.finished(0, 0));

    CaptureProgress both_lost{0, 0, true, true};
    CHECK(both_lost.finished(5, 3, true, true, true));
    std::puts("PASS device loss and unknown fences preserve the other device's pending work");
}

class LifetimeProbe final : public IUnknown {
  public:
    explicit LifetimeProbe(std::atomic<bool> &destroyed) : destroyed_(destroyed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, __uuidof(IUnknown)))
            return E_NOINTERFACE;
        *out = static_cast<IUnknown *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references_;
        if (!count)
            delete this;
        return count;
    }

  private:
    ~LifetimeProbe() {
        destroyed_.store(true);
    }
    std::atomic<ULONG> references_{1};
    std::atomic<bool> &destroyed_;
};
void observe_source_lifetime(ID3D12Resource *source, std::atomic<bool> &destroyed) {
    constexpr GUID key{0xb8ab01a7, 0x1df2, 0x4c3e, {0xad, 0xb1, 0x11, 0x4a, 0x57, 0x48, 0x90, 0x63}};
    auto *probe = new LifetimeProbe(destroyed);
    const HRESULT result = source->SetPrivateDataInterface(key, probe);
    probe->Release();
    REQUIRE_HR(result);
}

struct Graphics {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> messages;
    UINT64 fence_value{};
    HANDLE event{};
    struct Commands {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12DescriptorHeap> descriptors;
    };
    std::vector<Commands> submitted;

    explicit Graphics(bool warp) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            std::puts("D3D12 debug validation enabled");
        } else {
            std::puts("D3D12 debug validation unavailable; pixel and synchronization checks remain active");
        }
        REQUIRE_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        if (warp) {
            REQUIRE_HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        } else {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                REQUIRE_HR(adapter->GetDesc1(&desc));
                if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                                nullptr)))
                    break;
                adapter.Reset();
            }
            CHECK(adapter);
        }
        REQUIRE_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        REQUIRE_HR(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        REQUIRE_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        device.As(&messages);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(event);
    }
    ~Graphics() {
        drain();
        CloseHandle(event);
    }
    void drain() {
        const auto target = ++fence_value;
        REQUIRE_HR(queue->Signal(fence.Get(), target));
        REQUIRE_HR(fence->SetEventOnCompletion(target, event));
        CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
        submitted.clear();
    }
    ComPtr<ID3D12Resource> source(DXGI_FORMAT format, UINT arrays = 1, UINT mips = 1, UINT samples = 1) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 16;
        desc.Height = 12;
        desc.DepthOrArraySize = static_cast<UINT16>(arrays);
        desc.MipLevels = static_cast<UINT16>(mips);
        desc.Format = format;
        desc.SampleDesc.Count = samples;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> result;
        REQUIRE_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                   IID_PPV_ARGS(&result)));
        return result;
    }
    void paint(ID3D12Resource *source, DXGI_FORMAT typed, const std::array<float, 4> &color,
               bool vary_slices = false, bool accent = false) {
        const auto desc = source->GetDesc();
        Commands commands;
        REQUIRE_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&commands.allocator)));
        REQUIRE_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commands.allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&commands.list)));
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = desc.DepthOrArraySize * desc.MipLevels;
        REQUIRE_HR(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&commands.descriptors)));
        auto handle = commands.descriptors->GetCPUDescriptorHandleForHeapStart();
        const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT slice = 0; slice < desc.DepthOrArraySize; ++slice) {
            for (UINT mip = 0; mip < desc.MipLevels; ++mip) {
                D3D12_RENDER_TARGET_VIEW_DESC view{};
                view.Format = typed;
                if (desc.SampleDesc.Count > 1) {
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    view.Texture2DMSArray.FirstArraySlice = slice;
                    view.Texture2DMSArray.ArraySize = 1;
                } else {
                    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MipSlice = mip;
                    view.Texture2DArray.FirstArraySlice = slice;
                    view.Texture2DArray.ArraySize = 1;
                }
                device->CreateRenderTargetView(source, &view, handle);
                auto actual = color;
                if (vary_slices && slice == 1)
                    actual = {.75f, .25f, .5f, 1.f};
                if (mip)
                    actual = {0.f, 1.f, 0.f, 1.f};
                commands.list->ClearRenderTargetView(handle, actual.data(), 0, nullptr);
                if (accent && !mip) {
                    const float blue[]{0.f, 0.f, 1.f, 1.f};
                    const D3D12_RECT rect{2, 3, 7, 7};
                    commands.list->ClearRenderTargetView(handle, blue, 1, &rect);
                }
                handle.ptr += increment;
            }
        }
        REQUIRE_HR(commands.list->Close());
        ID3D12CommandList *lists[]{commands.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        submitted.push_back(std::move(commands));
    }
    void validate_debug_messages() {
        drain();
        CHECK(device->GetDeviceRemovedReason() == S_OK);
        if (!messages)
            return;
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T length{};
            REQUIRE_HR(messages->GetMessage(i, nullptr, &length));
            std::vector<unsigned char> buffer(length);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
            REQUIRE_HR(messages->GetMessage(i, message, &length));
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                std::fprintf(stderr, "D3D12 validation: %s\n", message->pDescription);
                CHECK(false);
            }
        }
        messages->ClearStoredMessages();
    }
};

using Color = std::array<float, 4>;
constexpr Color original{.25f, .5f, .75f, 1.f};
constexpr Color other_slice{.75f, .25f, .5f, 1.f};
constexpr Color overwritten{1.f, 0.f, 0.f, 1.f};
constexpr Color blue{0.f, 0.f, 1.f, 1.f};
constexpr std::array<DXGI_FORMAT, 6> formats{DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                             DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                             DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM};

float half_value(unsigned short value) {
    const unsigned exponent = (value >> 10) & 31;
    const unsigned mantissa = value & 1023;
    const float magnitude =
        exponent ? std::ldexp(1.f + mantissa / 1024.f, int(exponent) - 15) : std::ldexp(float(mantissa), -24);
    return value & 0x8000 ? -magnitude : magnitude;
}
Color pixel(const unsigned char *data, DXGI_FORMAT format) {
    Color result{};
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        unsigned short values[4];
        std::memcpy(values, data, sizeof(values));
        for (unsigned i = 0; i < 4; ++i)
            result[i] = half_value(values[i]);
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        unsigned value{};
        std::memcpy(&value, data, sizeof(value));
        result = {float(value & 1023) / 1023.f, float((value >> 10) & 1023) / 1023.f,
                  float((value >> 20) & 1023) / 1023.f, float(value >> 30) / 3.f};
    } else {
        const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        result = {data[bgra ? 2 : 0] / 255.f, data[1] / 255.f, data[bgra ? 0 : 2] / 255.f, data[3] / 255.f};
    }
    return result;
}
void expect_color(Color actual, Color expected, DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        // Literal sRGB encodings of the fixture's .25, .5 and .75 linear clears.
        for (unsigned i = 0; i < 3; ++i) {
            if (expected[i] == .25f)
                expected[i] = 137.f / 255.f;
            else if (expected[i] == .5f)
                expected[i] = 188.f / 255.f;
            else if (expected[i] == .75f)
                expected[i] = 225.f / 255.f;
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (std::abs(actual[i] - expected[i]) > .006f) {
            std::fprintf(stderr, "format %u component %u: expected %.6f, got %.6f\n", unsigned(format), i,
                         expected[i], actual[i]);
            CHECK(false);
        }
    }
}
ComPtr<ID3D11Texture2D> readback(D3D12Capture &capture, const D3D12Capture::SnapshotPtr &snapshot) {
    auto *texture = D3D12Capture::texture(snapshot);
    CHECK(texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    CHECK(desc.SampleDesc.Count == 1 && desc.MipLevels == 1);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    REQUIRE_HR(capture.device()->CreateTexture2D(&desc, nullptr, &staging));
    CHECK(capture.before_read(snapshot));
    capture.context()->CopyResource(staging.Get(), texture);
    CHECK(capture.finish_reads());
    return staging;
}
void expect_image(D3D12Capture &capture, ID3D11Texture2D *staging, DXGI_FORMAT format, UINT arrays,
                  Color expected = original, bool varied = false, bool accent = false) {
    D3D11_TEXTURE2D_DESC desc{};
    staging->GetDesc(&desc);
    CHECK(desc.Width == 16 && desc.Height == 12 && desc.ArraySize == arrays && desc.Format == format);
    for (UINT slice = 0; slice < arrays; ++slice) {
        D3D11_MAPPED_SUBRESOURCE map{};
        REQUIRE_HR(
            capture.context()->Map(staging, D3D11CalcSubresource(0, slice, 1), D3D11_MAP_READ, 0, &map));
        const unsigned bytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        for (UINT y = 0; y < desc.Height; ++y) {
            for (UINT x = 0; x < desc.Width; ++x) {
                auto want = varied && slice == 1 ? other_slice : expected;
                if (accent && x >= 2 && x < 7 && y >= 3 && y < 7)
                    want = blue;
                const auto *address =
                    static_cast<const unsigned char *>(map.pData) + y * map.RowPitch + x * bytes;
                expect_color(pixel(address, format), want, format);
            }
        }
        capture.context()->Unmap(staging, D3D11CalcSubresource(0, slice, 1));
    }
    CHECK(capture.device()->GetDeviceRemovedReason() == S_OK);
}
void check_transport_format(DXGI_FORMAT format) {
    View view{};
    view.texture_handle = 1;
    view.width = 16;
    view.height = 12;
    view.rect_width = 16;
    view.rect_height = 12;
    view.format = format;
    CHECK(valid_view(view));
}
void formats_retain_release_time_pixels(Graphics &graphics, D3D12Capture &capture) {
    // A copy deferred until end-frame would read the red runtime overwrite.
    for (const auto format : formats) {
        check_transport_format(format);
        D3D12Capture::Pool pool;
        auto source = graphics.source(format);
        graphics.paint(source.Get(), format, original, false, true);
        auto snapshot = capture.capture(pool, source.Get(), format);
        CHECK(snapshot);
        graphics.paint(source.Get(), format, overwritten);
        graphics.drain();
        auto copied = readback(capture, snapshot);
        expect_image(capture, copied.Get(), format, 1, original, false, true);
        snapshot.reset();
        capture.collect();
    }
    graphics.validate_debug_messages();
    std::puts("PASS six typed formats retain release-time pixels after runtime overwrite");
}
DXGI_FORMAT typeless_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    }
}
void arrays_mips_and_typeless_are_selected_correctly(Graphics &graphics, D3D12Capture &capture) {
    // Wrong source subresource stride reads the green lower mip instead of slice 1.
    for (const auto format : formats) {
        D3D12Capture::Pool pool;
        auto source = graphics.source(typeless_format(format), 2, 3);
        graphics.paint(source.Get(), format, original, true, true);
        auto snapshot = capture.capture(pool, source.Get(), format);
        CHECK(snapshot);
        graphics.paint(source.Get(), format, overwritten);
        graphics.drain();
        auto copied = readback(capture, snapshot);
        expect_image(capture, copied.Get(), format, 2, original, true, true);
        snapshot.reset();
        capture.collect();
    }
    graphics.validate_debug_messages();
    std::puts("PASS typeless sources preserve typed format and both array slices at mip 0");
}
void multisample_resolves_every_slice(Graphics &graphics, D3D12Capture &capture) {
    // CopyTextureRegion on MSAA or resolving only slice 0 cannot satisfy these readbacks.
    for (const auto format : formats) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{};
        support.Format = format;
        support.SampleCount = 4;
        REQUIRE_HR(graphics.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support,
                                                        sizeof(support)));
        CHECK(support.NumQualityLevels > 0);
        D3D12Capture::Pool pool;
        auto source = graphics.source(format, 2, 1, 4);
        graphics.paint(source.Get(), format, original, true, true);
        auto snapshot = capture.capture(pool, source.Get(), format);
        CHECK(snapshot);
        graphics.paint(source.Get(), format, overwritten);
        graphics.drain();
        auto copied = readback(capture, snapshot);
        expect_image(capture, copied.Get(), format, 2, original, true, true);
        snapshot.reset();
        capture.collect();
    }
    graphics.validate_debug_messages();
    std::puts("PASS 4x MSAA resolves both array slices for all six transport formats");
}
double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void pool_backpressure_and_reuse_do_not_wait_for_gpu(Graphics &graphics, D3D12Capture &capture) {
    // A blocked application queue makes accidental CPU waits and premature slot reuse observable.
    auto source = graphics.source(DXGI_FORMAT_R8G8B8A8_UNORM);
    graphics.paint(source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, original);
    graphics.drain();
    ComPtr<ID3D12Fence> gate;
    REQUIRE_HR(graphics.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    REQUIRE_HR(graphics.queue->Wait(gate.Get(), 1));
    D3D12Capture::Pool pool;
    std::array<D3D12Capture::SnapshotPtr, 3> held;
    for (auto &snapshot : held) {
        snapshot = capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(snapshot);
    }
    auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 100; ++i)
        CHECK(!capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
    CHECK(milliseconds_since(start) < 250);
    start = std::chrono::steady_clock::now();
    auto copied = readback(capture, held[0]);
    CHECK(milliseconds_since(start) < 250);
    // The companion copy is queued but cannot execute until the D3D12 gate opens.
    held[0].reset();
    start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 100; ++i) {
        capture.collect();
        CHECK(!capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
    }
    CHECK(milliseconds_since(start) < 250);
    REQUIRE_HR(gate->Signal(1));
    graphics.drain();
    expect_image(capture, copied.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);

    graphics.paint(source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, overwritten);
    graphics.drain();
    D3D12Capture::SnapshotPtr replacement;
    start = std::chrono::steady_clock::now();
    do {
        capture.collect();
        replacement = capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!replacement)
            Sleep(1);
    } while (!replacement && milliseconds_since(start) < 2000);
    CHECK(replacement);
    auto newest = readback(capture, replacement);
    expect_image(capture, newest.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, overwritten);
    // Held snapshots remain immutable while a released slot is reused.
    auto older = readback(capture, held[1]);
    expect_image(capture, older.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    replacement.reset();
    held = {};
    capture.collect();
    graphics.validate_debug_messages();
    std::puts("PASS bounded pool drops promptly, defers in-flight reuse, and preserves held snapshots");
}
void companion_reads_pin_slots_after_capture_completion(Graphics &graphics, D3D12Capture &capture) {
    auto source = graphics.source(DXGI_FORMAT_R8G8B8A8_UNORM);
    graphics.paint(source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, original);
    D3D12Capture::Pool pool;
    std::array<D3D12Capture::SnapshotPtr, 3> held;
    for (auto &snapshot : held) {
        snapshot = capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(snapshot);
    }
    graphics.drain(); // All capture fences are already complete before the reader is blocked.

    ComPtr<ID3D12Fence> gate;
    REQUIRE_HR(graphics.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate)));
    HANDLE shared{};
    REQUIRE_HR(graphics.device->CreateSharedHandle(gate.Get(), nullptr, GENERIC_ALL, nullptr, &shared));
    ComPtr<ID3D11Device5> reader_device;
    ComPtr<ID3D11DeviceContext4> reader_context;
    ComPtr<ID3D11Fence> reader_gate;
    REQUIRE_HR(capture.device()->QueryInterface(IID_PPV_ARGS(&reader_device)));
    REQUIRE_HR(capture.context()->QueryInterface(IID_PPV_ARGS(&reader_context)));
    const HRESULT opened = reader_device->OpenSharedFence(shared, IID_PPV_ARGS(&reader_gate));
    CHECK(CloseHandle(shared));
    REQUIRE_HR(opened);
    REQUIRE_HR(reader_context->Wait(reader_gate.Get(), 1));
    auto start = std::chrono::steady_clock::now();
    auto copied = readback(capture, held[0]);
    held[0].reset();
    CHECK(milliseconds_since(start) < 250);

    // This has no unfinished D3D12 capture to hide a missing companion-read lifetime check.
    start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 100; ++i) {
        capture.collect();
        CHECK(!capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
    }
    CHECK(milliseconds_since(start) < 250);
    graphics.paint(source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, overwritten);
    graphics.drain(); // Blocking the private reader must not add a wait to the application's queue.
    CHECK(gate->GetCompletedValue() == 0);
    CHECK(!capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));

    REQUIRE_HR(gate->Signal(1));
    expect_image(capture, copied.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    D3D12Capture::SnapshotPtr replacement;
    start = std::chrono::steady_clock::now();
    do {
        capture.collect();
        replacement = capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!replacement)
            Sleep(1);
    } while (!replacement && milliseconds_since(start) < 2000);
    CHECK(replacement);
    auto newest = readback(capture, replacement);
    expect_image(capture, newest.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, overwritten);
    replacement.reset();
    held = {};
    capture.collect();
    graphics.validate_debug_messages();
    std::puts("PASS a blocked companion read pins completed captures while the application queue progresses");
}
void invalid_inputs_do_not_damage_capture(Graphics &graphics, D3D12Capture &capture) {
    D3D12Capture::Pool pool;
    auto source = graphics.source(DXGI_FORMAT_R8G8B8A8_UNORM);
    {
        D3D12Capture unopened;
        CHECK(!unopened.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
        CHECK(!unopened.before_read({}));
        unopened.collect();
        CHECK(!unopened.initialize(nullptr, graphics.queue.Get()));
        CHECK(!unopened.healthy());
    }
    {
        D3D12Capture invalid;
        CHECK(!invalid.initialize(graphics.device.Get(), nullptr));
        CHECK(!invalid.healthy());
    }
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> copy_queue;
        REQUIRE_HR(graphics.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&copy_queue)));
        D3D12Capture invalid;
        CHECK(!invalid.initialize(graphics.device.Get(), copy_queue.Get()));
    }
    CHECK(!D3D12Capture::texture({}));
    CHECK(!capture.before_read({}));
    CHECK(!capture.capture(pool, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM));
    CHECK(!capture.capture(pool, source.Get(), DXGI_FORMAT_UNKNOWN));
    CHECK(!capture.capture(pool, source.Get(), DXGI_FORMAT_R32_FLOAT));
    CHECK(!capture.capture(pool, source.Get(), DXGI_FORMAT_B8G8R8A8_UNORM));
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 256;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        REQUIRE_HR(graphics.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)));
        CHECK(!capture.capture(pool, buffer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
    }
    {
        // D3D12 devices are singletons per adapter, so a different adapter is required.
        ComPtr<ID3D12Device> foreign_device;
        ComPtr<IDXGIAdapter1> other_adapter;
        REQUIRE_HR(graphics.factory->EnumWarpAdapter(IID_PPV_ARGS(&other_adapter)));
        REQUIRE_HR(
            D3D12CreateDevice(other_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&foreign_device)));
        if (foreign_device.Get() == graphics.device.Get()) {
            foreign_device.Reset();
            other_adapter.Reset();
            for (UINT i = 0; graphics.factory->EnumAdapters1(i, &other_adapter) != DXGI_ERROR_NOT_FOUND;
                 ++i) {
                if (SUCCEEDED(D3D12CreateDevice(other_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                IID_PPV_ARGS(&foreign_device))) &&
                    foreign_device.Get() != graphics.device.Get())
                    break;
                foreign_device.Reset();
                other_adapter.Reset();
            }
        }
        if (foreign_device) {
            CHECK(foreign_device.Get() != graphics.device.Get());
            D3D12_COMMAND_QUEUE_DESC queue_desc{};
            queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> foreign_queue;
            REQUIRE_HR(foreign_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&foreign_queue)));
            D3D12Capture wrong_binding;
            CHECK(!wrong_binding.initialize(graphics.device.Get(), foreign_queue.Get()));
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            const auto desc = source->GetDesc();
            ComPtr<ID3D12Resource> foreign_source;
            REQUIRE_HR(foreign_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                               IID_PPV_ARGS(&foreign_source)));
            CHECK(!capture.capture(pool, foreign_source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
        } else {
            std::puts("SKIP cross-device rejection: no second D3D12 adapter available");
        }
    }
    CHECK(capture.healthy());
    graphics.paint(source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, original);
    auto snapshot = capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(snapshot);
    auto copied = readback(capture, snapshot);
    expect_image(capture, copied.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    snapshot.reset();
    capture.collect();
    graphics.validate_debug_messages();
    std::puts("PASS invalid bindings, formats, and empty inputs reject safely; capture remains usable");
}
void pending_snapshot_and_pool_teardown_are_safe(Graphics &graphics) {
    // Destroying the swapchain pool must not invalidate a candidate still used by end-frame.
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        D3D12Capture capture;
        CHECK(capture.initialize(graphics.device.Get(), graphics.queue.Get()));
        auto source = graphics.source(DXGI_FORMAT_R8G8B8A8_UNORM);
        graphics.paint(source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, original);
        graphics.drain();
        D3D12Capture::SnapshotPtr snapshot;
        {
            D3D12Capture::Pool pool;
            snapshot = capture.capture(pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            CHECK(snapshot);
        }
        auto copied = readback(capture, snapshot);
        expect_image(capture, copied.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        snapshot.reset();
        capture.collect();
    }
    graphics.validate_debug_messages();
    // Keep capture work blocked past the bounded destructor wait so quarantine is mandatory.
    ComPtr<ID3D12Fence> gate;
    REQUIRE_HR(graphics.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    auto source = graphics.source(DXGI_FORMAT_R8G8B8A8_UNORM);
    std::atomic<bool> source_destroyed{};
    observe_source_lifetime(source.Get(), source_destroyed);
    graphics.paint(source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, original);
    graphics.drain();
    auto capture = std::make_unique<D3D12Capture>();
    CHECK(capture->initialize(graphics.device.Get(), graphics.queue.Get()));
    auto pool = std::make_unique<D3D12Capture::Pool>();
    REQUIRE_HR(graphics.queue->Wait(gate.Get(), 1));
    auto snapshot = capture->capture(*pool, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(snapshot);
    source.Reset(); // The pending capture must retain the runtime resource until execution completes.
    const auto start = std::chrono::steady_clock::now();
    snapshot.reset();
    pool.reset();
    capture.reset();
    const double teardown_ms = milliseconds_since(start);
    CHECK(teardown_ms >= 200 && teardown_ms < 1500);
    CHECK(gate->GetCompletedValue() == 0);
    CHECK(!source_destroyed.load());
    D3D12Capture collector;
    CHECK(collector.initialize(graphics.device.Get(), graphics.queue.Get()));
    while (milliseconds_since(start) < 400)
        Sleep(1);
    collector.collect();
    CHECK(!source_destroyed.load());
    REQUIRE_HR(gate->Signal(1));
    graphics.drain();
    collector.collect();
    CHECK(source_destroyed.load());
    graphics.validate_debug_messages();
    std::puts("PASS quarantine retains pending sources past teardown and later collection releases them");
}
} // namespace

int main(int argc, char **argv) {
    device_failure_preserves_the_other_devices_obligations();
    const bool warp = argc < 2 || std::strcmp(argv[1], "--hardware") != 0;
    Graphics graphics(warp);
    D3D12Capture capture;
    CHECK(capture.initialize(graphics.device.Get(), graphics.queue.Get()));
    CHECK(capture.healthy() && capture.device() && capture.context());
    formats_retain_release_time_pixels(graphics, capture);
    arrays_mips_and_typeless_are_selected_correctly(graphics, capture);
    multisample_resolves_every_slice(graphics, capture);
    pool_backpressure_and_reuse_do_not_wait_for_gpu(graphics, capture);
    companion_reads_pin_slots_after_capture_completion(graphics, capture);
    invalid_inputs_do_not_damage_capture(graphics, capture);
    pending_snapshot_and_pool_teardown_are_safe(graphics);
    std::puts("D3D12 native capture tests passed");
}
