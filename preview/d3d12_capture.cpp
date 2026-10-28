#include "d3d12_capture.hpp"
#include "d3d12_capture_lifetime.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <limits>
#include <mutex>
#include <vector>
#include <wrl/client.h>

namespace vrst::preview {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::uint64_t byte_limit = 512ull * 1024 * 1024;
constexpr unsigned pool_limit = 3;
// Includes retired sessions: repeated creation cannot grow capture memory
// without bound when a device or application queue stops making progress.
std::atomic<std::uint64_t> allocated_bytes{};

struct Budget {
    std::uint64_t bytes{};
    ~Budget() { allocated_bytes.fetch_sub(bytes, std::memory_order_relaxed); }
    bool reserve(std::uint64_t requested) {
        auto current = allocated_bytes.load(std::memory_order_relaxed);
        while (requested <= byte_limit && current <= byte_limit - requested) {
            if (allocated_bytes.compare_exchange_weak(current, current + requested,
                                                       std::memory_order_relaxed)) {
                bytes = requested;
                return true;
            }
        }
        return false;
    }
};
DXGI_FORMAT family(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
bool typed(DXGI_FORMAT format) {
    return family(format) != DXGI_FORMAT_UNKNOWN && family(format) != format;
}
struct Context {
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D12Fence> captured12, read12;
    ComPtr<ID3D11Fence> captured11, read11;
    std::uint64_t next_capture{}, next_read{};
    bool fault{};
    detail::CaptureProgress completed;
    wchar_t error[192]{};

    bool fail(const wchar_t *message, HRESULT hr = S_OK, bool fatal = false) {
        if (FAILED(hr))
            std::swprintf(error, std::size(error), L"%ls (0x%08lx)", message,
                          static_cast<unsigned long>(hr));
        else {
            std::wcsncpy(error, message, std::size(error) - 1);
            error[std::size(error) - 1] = 0;
        }
        fault = fault || fatal;
        return false;
    }
    bool progress() {
        const auto capture = captured12 ? captured12->GetCompletedValue() : 0;
        const auto read = read11 ? read11->GetCompletedValue() : 0;
        // A shared fence may become unreadable when either participating device
        // fails. Only each device's own removal status discharges its work; a
        // failed companion must never release pending application-queue reads.
        completed.capture_removed = completed.capture_removed ||
            (device12 && FAILED(device12->GetDeviceRemovedReason()));
        completed.reader_removed = completed.reader_removed ||
            (device11 && FAILED(device11->GetDeviceRemovedReason()));
        // Preserve previously proven completion when a shared fence reports
        // UINT64_MAX. In particular read12's removal sentinel cannot prove that
        // the independent D3D11 companion has stopped reading the shared image.
        if (capture != UINT64_MAX)
            completed.capture_completed = std::max(completed.capture_completed, capture);
        if (read != UINT64_MAX)
            completed.read_completed = std::max(completed.read_completed, read);
        if (completed.capture_removed || completed.reader_removed ||
            capture == UINT64_MAX || read == UINT64_MAX) {
            fail(L"D3D12 capture device or shared fence is unavailable", S_OK, true);
            return false;
        }
        return true;
    }
};
} // namespace

struct D3D12Capture::Slot {
    Budget budget;
    std::shared_ptr<Context> context;
    ComPtr<ID3D12Resource> image12, source;
    ComPtr<ID3D11Texture2D> image11;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    D3D12_RESOURCE_DESC desc{};
    std::weak_ptr<const Snapshot> token;
    std::uint64_t captured{}, read{};
    bool pending_read{}, untracked_capture{}, untracked_read{};
    bool finished() const {
        return context->completed.finished(captured, read, untracked_capture, untracked_read, pending_read);
    }
};
struct D3D12Capture::PoolData {
    std::weak_ptr<Context> context;
    std::vector<std::shared_ptr<Slot>> slots;
};
struct D3D12Capture::Impl {
    std::shared_ptr<Context> context = std::make_shared<Context>();
    std::vector<std::shared_ptr<Slot>> slots, reads;

    // Deliberately not destroyed during DLL unload: unfinished GPU references
    // must survive an abnormal queue stall. Subsequent layer calls collect them.
    // The global byte budget still bounds these quarantined resources.
    static std::vector<std::unique_ptr<Impl>> &retired() {
        static auto *items = new std::vector<std::unique_ptr<Impl>>;
        return *items;
    }
    static std::mutex &retired_mutex() {
        static auto *mutex = new std::mutex;
        return *mutex;
    }
    void collect() {
        context->progress();
        for (auto &slot : slots)
            if (context->completed.capture_finished(slot->captured, slot->untracked_capture))
                slot->source.Reset();
        slots.erase(std::remove_if(slots.begin(), slots.end(), [&](const auto &slot) {
            return slot.use_count() == 1 && slot->finished();
        }), slots.end());
    }
    bool idle() {
        context->progress();
        return std::all_of(slots.begin(), slots.end(), [&](const auto &slot) {
            return slot->finished();
        });
    }
    static void collect_retired() {
        std::lock_guard<std::mutex> guard(retired_mutex());
        auto &items = retired();
        items.erase(std::remove_if(items.begin(), items.end(), [](const auto &item) {
            item->collect();
            return item->idle();
        }), items.end());
    }
    bool finish_reads() noexcept {
        if (reads.empty())
            return !context->fault;
        auto &c = *context;
        const auto value = ++c.next_read;
        const HRESULT hr = c.context11->Signal(c.read11.Get(), value);
        // Flush even on Signal failure, so a later retirement marker can cover
        // copies already queued by the caller on this immediate context.
        c.context11->Flush();
        for (auto &slot : reads) {
            slot->pending_read = false;
            slot->untracked_read = FAILED(hr);
            if (SUCCEEDED(hr))
                slot->read = value;
        }
        reads.clear();
        if (FAILED(hr))
            return c.fail(L"Unable to signal D3D11 snapshot reads", hr, true);
        return !c.fault;
    }
    // Only teardown may wait. No fence here depends on the external consumer.
    void seal() {
        if (!reads.empty())
            finish_reads();
        const bool capture_unknown = std::any_of(slots.begin(), slots.end(), [](const auto &slot) {
            return slot->untracked_capture;
        });
        if (capture_unknown && context->queue && context->captured12) {
            const auto value = ++context->next_capture;
            if (SUCCEEDED(context->queue->Signal(context->captured12.Get(), value)))
                for (auto &slot : slots)
                    if (slot->untracked_capture) {
                        slot->untracked_capture = false;
                        slot->captured = value;
                    }
        }
        const bool read_unknown = std::any_of(slots.begin(), slots.end(), [](const auto &slot) {
            return slot->untracked_read;
        });
        if (read_unknown && context->context11 && context->read11) {
            const auto value = ++context->next_read;
            const auto hr = context->context11->Signal(context->read11.Get(), value);
            context->context11->Flush();
            if (SUCCEEDED(hr))
                for (auto &slot : slots)
                    if (slot->untracked_read) {
                        slot->untracked_read = false;
                        slot->read = value;
                    }
        }
    }
    static void retire(std::unique_ptr<Impl> item) noexcept {
        if (!item)
            return;
        item->seal();
        const auto until = GetTickCount64() + 250;
        while (!item->idle() && GetTickCount64() < until)
            Sleep(1);
        item->collect();
        if (item->idle())
            return;
        try {
            std::lock_guard<std::mutex> guard(retired_mutex());
            retired().push_back(std::move(item));
        } catch (...) {
            // Allocation failure cannot make releasing live GPU objects safe.
            // The same global budget covers this last-resort quarantine.
            item.release();
        }
    }
};

D3D12Capture::Pool::Pool() : data_(std::make_unique<PoolData>()) {}
D3D12Capture::Pool::~Pool() = default;
D3D12Capture::Pool::Pool(Pool &&) noexcept = default;
D3D12Capture::Pool &D3D12Capture::Pool::operator=(Pool &&) noexcept = default;
D3D12Capture::Snapshot::Snapshot(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}
D3D12Capture::Snapshot::~Snapshot() = default;
D3D12Capture::D3D12Capture() : impl_(std::make_unique<Impl>()) {}
D3D12Capture::~D3D12Capture() { Impl::retire(std::move(impl_)); }

bool D3D12Capture::initialize(ID3D12Device *device, ID3D12CommandQueue *queue) {
    if (impl_->context->device12)
        return impl_->context->fail(L"D3D12 capture is already initialized");
    Impl::collect_retired();
    auto &c = *impl_->context;
    if (!device || !queue)
        return c.fail(L"Missing D3D12 graphics binding");
    const auto q = queue->GetDesc();
    ComPtr<ID3D12Device> queue_device;
    if (q.Type != D3D12_COMMAND_LIST_TYPE_DIRECT || q.NodeMask > 1 ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || queue_device.Get() != device)
        return c.fail(L"D3D12 capture requires the binding device's direct queue on node 0");
    c.device12 = device;
    c.queue = queue;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    if (FAILED(hr))
        return c.fail(L"Unable to find the D3D12 graphics adapter", hr, true);
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
                          &device11, nullptr, &context11);
    if (SUCCEEDED(hr))
        hr = device11.As(&c.device11);
    if (SUCCEEDED(hr))
        hr = context11.As(&c.context11);
    if (FAILED(hr))
        return c.fail(L"D3D12 capture requires a D3D11 companion device with fence support", hr, true);
    auto make_fence = [&](ComPtr<ID3D12Fence> &fence12, ComPtr<ID3D11Fence> &fence11) {
        HRESULT result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12));
        HANDLE shared{};
        if (SUCCEEDED(result))
            result = device->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
        if (SUCCEEDED(result))
            result = c.device11->OpenSharedFence(shared, IID_PPV_ARGS(&fence11));
        if (shared)
            CloseHandle(shared);
        return result;
    };
    hr = make_fence(c.captured12, c.captured11);
    if (SUCCEEDED(hr))
        hr = make_fence(c.read12, c.read11);
    if (FAILED(hr))
        return c.fail(L"Unable to share D3D12 capture fences", hr, true);
    c.error[0] = 0;
    return c.progress();
}
ID3D11Device *D3D12Capture::device() const { return impl_->context->device11.Get(); }
ID3D11DeviceContext *D3D12Capture::context() const { return impl_->context->context11.Get(); }
bool D3D12Capture::healthy() const {
    const auto &c = *impl_->context;
    return c.device12 && c.device11 && c.context11 && c.captured12 && c.read12 && !c.fault;
}
const wchar_t *D3D12Capture::error() const { return impl_->context->error; }
ID3D11Texture2D *D3D12Capture::texture(const SnapshotPtr &snapshot) {
    return snapshot ? snapshot->slot_->image11.Get() : nullptr;
}
void D3D12Capture::collect() {
    impl_->collect();
    Impl::collect_retired();
}

D3D12Capture::SnapshotPtr D3D12Capture::capture(Pool &pool, ID3D12Resource *source,
                                               DXGI_FORMAT format) {
    auto &c = *impl_->context;
    if (!healthy() || !source)
        return {};
    collect();
    if (!healthy())
        return {};
    const auto src = source->GetDesc();
    ComPtr<ID3D12Device> source_device;
    if (!typed(format) || family(src.Format) != family(format) ||
        src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !src.Width || src.Width > 16384 ||
        !src.Height || src.Height > 16384 || !src.DepthOrArraySize || src.DepthOrArraySize > 2048 ||
        !src.MipLevels || !src.SampleDesc.Count ||
        !(src.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        FAILED(source->GetDevice(IID_PPV_ARGS(&source_device))) || source_device.Get() != c.device12.Get()) {
        c.fail(L"Unsupported D3D12 snapshot resource or format");
        return {};
    }
    if (src.SampleDesc.Count > 1) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = format;
        if ((src.Format != format && src.Format != family(format)) ||
            FAILED(c.device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE)) {
            c.fail(L"Unsupported D3D12 multisample resolve format");
            return {};
        }
    }
    if (!pool.data_)
        pool.data_ = std::make_unique<PoolData>();
    auto &p = *pool.data_;
    if (p.context.lock() != impl_->context) {
        p.slots.clear();
        p.context = impl_->context;
    }
    if (!c.progress())
        return {};
    std::shared_ptr<Slot> slot;
    for (auto &candidate : p.slots) {
        if (candidate->token.expired() && candidate->finished()) {
            slot = candidate;
            break;
        }
    }
    if (!slot && p.slots.size() == pool_limit) {
        c.fail(L"D3D12 snapshot pool busy; capture skipped");
        return {};
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = src.Width;
    desc.Height = src.Height;
    desc.DepthOrArraySize = src.DepthOrArraySize;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (slot && (slot->desc.Width != desc.Width || slot->desc.Height != desc.Height ||
                 slot->desc.DepthOrArraySize != desc.DepthOrArraySize || slot->desc.Format != desc.Format)) {
        p.slots.erase(std::find(p.slots.begin(), p.slots.end(), slot));
        slot.reset();
        impl_->collect();
    }
    if (!slot) {
        slot = std::make_shared<Slot>();
        slot->context = impl_->context;
        slot->desc = desc;
        const auto allocation = c.device12->GetResourceAllocationInfo(0, 1, &desc);
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX ||
            !slot->budget.reserve(allocation.SizeInBytes)) {
            c.fail(L"D3D12 snapshot memory limit reached");
            return {};
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        HRESULT hr = c.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&slot->image12));
        HANDLE handle{};
        if (SUCCEEDED(hr))
            hr = c.device12->CreateSharedHandle(slot->image12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (SUCCEEDED(hr))
            hr = c.device11->OpenSharedResource1(handle, IID_PPV_ARGS(&slot->image11));
        if (handle)
            CloseHandle(handle);
        if (FAILED(hr)) {
            c.fail(L"Driver cannot share this D3D12 snapshot with D3D11", hr);
            return {};
        }
        D3D11_TEXTURE2D_DESC opened{};
        slot->image11->GetDesc(&opened);
        if (opened.Width != desc.Width || opened.Height != desc.Height || opened.Format != format ||
            opened.MipLevels != 1 || opened.ArraySize != desc.DepthOrArraySize || opened.SampleDesc.Count != 1) {
            c.fail(L"D3D12 shared snapshot format mismatch");
            return {};
        }
        hr = c.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot->allocator));
        if (SUCCEEDED(hr))
            hr = c.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot->allocator.Get(),
                                              nullptr, IID_PPV_ARGS(&slot->commands));
        if (SUCCEEDED(hr))
            hr = slot->commands->Close();
        if (FAILED(hr)) {
            c.fail(L"Unable to create D3D12 capture commands", hr);
            return {};
        }
        // Reserve both vectors before either takes ownership of the slot.
        p.slots.reserve(pool_limit);
        impl_->slots.reserve(impl_->slots.size() + 1);
        p.slots.push_back(slot);
        impl_->slots.push_back(slot);
    }
    // Allocate before opening a command list so an allocation failure cannot
    // leave the reusable allocator attached to an unfinished recording.
    std::vector<D3D12_RESOURCE_BARRIER> barriers(src.DepthOrArraySize);
    HRESULT hr = slot->allocator->Reset();
    if (SUCCEEDED(hr))
        hr = slot->commands->Reset(slot->allocator.Get(), nullptr);
    if (FAILED(hr)) {
        c.fail(L"Unable to reset idle D3D12 capture commands", hr, true);
        return {};
    }
    const bool multisample = src.SampleDesc.Count > 1;
    for (UINT slice = 0; slice < src.DepthOrArraySize; ++slice) {
        auto &barrier = barriers[slice];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.Subresource = slice * src.MipLevels;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = multisample ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                                   : D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    slot->commands->ResourceBarrier(UINT(barriers.size()), barriers.data());
    // Shared simultaneous-access snapshots promote from COMMON for the copy or
    // resolve and decay back to COMMON at the end of this ExecuteCommandLists.
    for (UINT slice = 0; slice < src.DepthOrArraySize; ++slice) {
        if (multisample)
            slot->commands->ResolveSubresource(slot->image12.Get(), slice, source,
                                               slice * src.MipLevels, format);
        else {
            D3D12_TEXTURE_COPY_LOCATION destination{}, input{};
            destination.pResource = slot->image12.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = slice;
            input.pResource = source;
            input.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            input.SubresourceIndex = slice * src.MipLevels;
            slot->commands->CopyTextureRegion(&destination, 0, 0, 0, &input, nullptr);
        }
    }
    for (auto &barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    slot->commands->ResourceBarrier(UINT(barriers.size()), barriers.data());
    hr = slot->commands->Close();
    if (FAILED(hr)) {
        c.fail(L"Unable to record D3D12 snapshot", hr, true);
        return {};
    }
    // Finish every allocation before submission; even a subsequent signal
    // failure leaves the backend holding all command and source references.
    auto snapshot = SnapshotPtr(new Snapshot(slot));
    slot->token = snapshot;
    slot->source = source;
    slot->captured = ++c.next_capture;
    slot->untracked_capture = true;
    ID3D12CommandList *list = slot->commands.Get();
    c.queue->ExecuteCommandLists(1, &list);
    hr = c.queue->Signal(c.captured12.Get(), slot->captured);
    if (FAILED(hr)) {
        c.fail(L"Unable to signal D3D12 snapshot", hr, true);
        return {};
    }
    slot->untracked_capture = false;
    c.error[0] = 0;
    return snapshot;
}

bool D3D12Capture::before_read(const SnapshotPtr &snapshot) {
    auto &c = *impl_->context;
    if (!healthy() || !snapshot || snapshot->slot_->context != impl_->context)
        return false;
    auto &slot = snapshot->slot_;
    if (slot->untracked_capture || slot->untracked_read)
        return c.fail(L"D3D12 snapshot synchronization is unavailable", S_OK, true);
    if (slot->pending_read)
        return true;
    // Keep the slot pinned before enqueuing any GPU operation.
    impl_->reads.push_back(slot);
    slot->pending_read = true;
    const HRESULT hr = c.context11->Wait(c.captured11.Get(), slot->captured);
    if (FAILED(hr))
        return c.fail(L"Unable to wait for D3D12 snapshot on companion device", hr, true);
    return true;
}
bool D3D12Capture::finish_reads() noexcept { return impl_->finish_reads(); }
} // namespace vrst::preview
