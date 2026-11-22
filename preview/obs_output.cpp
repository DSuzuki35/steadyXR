#include "obs_output.hpp"
#include "obs_output_protocol.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace vrst::preview {
namespace ipc = steadyxr::obs_output;
using Microsoft::WRL::ComPtr;
namespace {
// This object is held for the whole session, without holding the metadata lock.
// It prevents a second app instance from replacing the live producer's record.
constexpr wchar_t owner_name[] = L"Local\\SteadyXR.ObsOutput.v1.Producer";
std::atomic<std::uint64_t> last_epoch{};
std::uint64_t new_epoch() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    auto previous = last_epoch.load();
    std::uint64_t next{};
    do {
        next = std::max(previous + 1, static_cast<std::uint64_t>(counter.QuadPart));
    } while (!last_epoch.compare_exchange_weak(previous, next));
    return next;
}
} // namespace

struct ObsOutputPublisher::Impl {
    HANDLE owner{}, mutex{}, mapping{};
    ipc::Metadata *metadata{};
    std::uint64_t epoch{};
    std::wstring error;
    ComPtr<ID3D11Device> adapter_device;
    LUID adapter_luid{};
    bool adapter_valid{};
    void refresh_adapter(ID3D11Device *current) {
        if (current == adapter_device.Get())
            return;
        // Retaining the identity prevents pointer reuse after renderer recovery.
        // Polling/heartbeat ticks need no DXGI calls for an unchanged device.
        adapter_device = current;
        adapter_luid = {};
        adapter_valid = false;
        ComPtr<IDXGIDevice> device;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (current && SUCCEEDED(current->QueryInterface(IID_PPV_ARGS(&device))) &&
            SUCCEEDED(device->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&description))) {
            adapter_luid = description.AdapterLuid;
            adapter_valid = true;
        }
    }
    bool lock() const {
        if (!mutex)
            return false;
        const DWORD result = WaitForSingleObject(mutex, 0);
        // The producer always writes a complete record, including after abandonment.
        return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
};

ObsOutputPublisher::ObsOutputPublisher() : impl_(std::make_unique<Impl>()) {}
ObsOutputPublisher::~ObsOutputPublisher() {
    close();
}
bool ObsOutputPublisher::open() {
    auto &p = *impl_;
    if (p.metadata)
        return true;
    p.owner = CreateMutexW(nullptr, FALSE, owner_name);
    const DWORD owner_error = GetLastError();
    if (!p.owner || owner_error == ERROR_ALREADY_EXISTS) {
        if (p.owner)
            CloseHandle(p.owner);
        p.owner = nullptr;
        p.error = L"Another SteadyXR instance is already publishing to OBS.";
        return false;
    }
    p.mutex = CreateMutexW(nullptr, FALSE, ipc::mutex_name);
    if (p.mutex)
        p.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(ipc::Metadata), ipc::mapping_name);
    if (p.mapping)
        p.metadata = static_cast<ipc::Metadata *>(
            MapViewOfFile(p.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Metadata)));
    if (!p.metadata || !p.lock()) {
        p.error = L"Could not open the OBS output metadata channel.";
        close();
        return false;
    }
    p.epoch = new_epoch();
    ipc::Metadata record;
    record.producer_pid = GetCurrentProcessId();
    record.epoch = p.epoch;
    record.heartbeat_ms = GetTickCount64();
    *p.metadata = record;
    ReleaseMutex(p.mutex);
    p.error.clear();
    return true;
}

bool ObsOutputPublisher::publish(const Renderer &renderer, unsigned, unsigned, bool visible) {
    auto &p = *impl_;
    if (!p.metadata)
        return false;
    ipc::Metadata record;
    record.producer_pid = GetCurrentProcessId();
    record.epoch = p.epoch;
    record.generation = renderer.output_generation();
    record.shared_handle = renderer.output_handle();
    record.frame_sequence = renderer.output_sequence();
    record.heartbeat_ms = GetTickCount64();
    // Use the actual allocation: a requested resize may not have rendered yet.
    record.width = renderer.output_width();
    record.height = renderer.output_height();
    p.refresh_adapter(renderer.device());
    if (p.adapter_valid) {
        record.adapter_luid_low = p.adapter_luid.LowPart;
        record.adapter_luid_high = p.adapter_luid.HighPart;
        if (visible && record.shared_handle && record.frame_sequence)
            record.flags = ipc::flag_active;
    }
    if (!p.lock())
        return false;
    *p.metadata = record;
    ReleaseMutex(p.mutex);
    return true;
}

void ObsOutputPublisher::close() {
    auto &p = *impl_;
    if (p.metadata && p.epoch && p.lock()) {
        ipc::Metadata stopped;
        stopped.producer_pid = GetCurrentProcessId();
        stopped.epoch = p.epoch;
        stopped.heartbeat_ms = GetTickCount64();
        *p.metadata = stopped;
        ReleaseMutex(p.mutex);
    }
    // A consumer holding the metadata lock cannot stall app shutdown. If its
    // lock prevents invalidation, the heartbeat watchdog expires the old record.
    if (p.metadata)
        UnmapViewOfFile(p.metadata);
    if (p.mapping)
        CloseHandle(p.mapping);
    if (p.mutex)
        CloseHandle(p.mutex);
    if (p.owner)
        CloseHandle(p.owner);
    p.metadata = nullptr;
    p.mapping = p.mutex = p.owner = nullptr;
    p.epoch = 0;
    p.adapter_device.Reset();
    p.adapter_luid = {};
    p.adapter_valid = false;
}
std::wstring ObsOutputPublisher::error() const {
    return impl_->error;
}
} // namespace vrst::preview
