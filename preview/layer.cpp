// OpenXR implicit layer: observe submitted projections without altering runtime calls.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "transport.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <d3d11.h>
#include <deque>
#include <dxgi.h>
#include <memory>
#include <mutex>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace {
using namespace vrst::preview;
constexpr char layer_name[] = "XR_APILAYER_HORIZONLOCK_preview";
template <class T> struct Com {
    T *p{};
    Com() = default;
    ~Com() {
        reset();
    }
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    Com(Com &&o) noexcept : p(o.p) {
        o.p = nullptr;
    }
    Com &operator=(Com &&o) noexcept {
        if (this != &o) {
            reset();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    void reset() {
        if (p)
            p->Release();
        p = nullptr;
    }
    T **put() {
        reset();
        return &p;
    }
    T *operator->() const {
        return p;
    }
    explicit operator bool() const {
        return p != nullptr;
    }
};
struct Instance {
    XrInstance handle{};
    PFN_xrGetInstanceProcAddr gipa{};
    wchar_t application[128]{};
#define PROC(n) PFN_xr##n n{};
    PROC(DestroyInstance)
    PROC(CreateSession)
    PROC(DestroySession)
    PROC(CreateSwapchain) PROC(DestroySwapchain) PROC(EnumerateSwapchainImages) PROC(AcquireSwapchainImage)
        PROC(WaitSwapchainImage) PROC(ReleaseSwapchainImage) PROC(WaitFrame) PROC(EndFrame) PROC(PollEvent)
#undef PROC
};
struct Output {
    Com<ID3D11Texture2D> texture;
    Com<IDXGIKeyedMutex> key;
    HANDLE handle{};
    D3D11_TEXTURE2D_DESC desc{};
};
struct Session {
    Instance *instance{};
    XrSession handle{};
    std::uint64_t cookie{}, epoch{1}, generation{}, sequence{};
    RateLimiter limiter;
    XrSpace space{};
    XrTime pending_change{};
    bool pending_reference{}, connected{};
    std::uint32_t consumer{};
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    LUID adapter{};
    std::array<Output, 2> outputs;
};
struct Acquisition {
    std::uint32_t index{};
    bool waited{};
};
struct Swapchain {
    Session *session{};
    XrSwapchainCreateInfo info{};
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::deque<Acquisition> acquired;
    Com<ID3D11Texture2D> snapshot, scratch;
    bool snapshot_valid{};
};
std::uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::recursive_mutex state_mutex;
std::unordered_map<XrInstance, std::unique_ptr<Instance>> instances;
std::unordered_map<XrSession, std::unique_ptr<Session>> sessions;
std::unordered_map<XrSwapchain, std::unique_ptr<Swapchain>> swapchains;
Channel channel;
std::uint64_t next_cookie = 1, next_generation = 1;
Session *owner{};
PFN_xrGetInstanceProcAddr global_next{};
struct ChannelLock {
    bool held{};
    ChannelLock() {
        if (!channel.data())
            channel.open(false);
        held = channel.lock();
    }
    ~ChannelLock() {
        if (held)
            channel.unlock();
    }
    explicit operator bool() const {
        return held;
    }
};
bool process_alive(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    bool alive = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
    CloseHandle(process);
    return alive;
}
void status(Shared &shared, Session &s, std::uint32_t code, const wchar_t *text) {
    shared.producer_pid = GetCurrentProcessId();
    shared.producer_heartbeat_ms = GetTickCount64();
    shared.status_code = code;
    std::wcsncpy(shared.application, s.instance->application, 127);
    shared.application[127] = 0;
    std::wcsncpy(shared.status, text, 191);
    shared.status[191] = 0;
}
void invalidate(Session &s) {
    s.outputs = {};
    s.generation = ++next_generation;
    s.connected = false;
    s.limiter.reset();
    for (auto &item : swapchains)
        if (item.second->session == &s)
            item.second->snapshot_valid = false;
}
// Called only while the shared header mutex is held; never waits for a viewer.
bool eligible(Session &s, Shared &shared, std::uint64_t now) {
    if (owner && owner != &s)
        return false;
    if (shared.producer_pid && shared.producer_pid != GetCurrentProcessId() &&
        process_alive(shared.producer_pid))
        return false;
    if (!s.device) {
        status(shared, s, 2, L"Unsupported graphics backend: D3D11 required");
        return false;
    }
    if (!owner) {
        owner = &s;
        shared.ready = 0;
        invalidate(s);
    }
    const bool active = viewer_active(shared, now);
    if (!active) {
        if (s.connected) {
            shared.ready = 0;
            invalidate(s);
        }
        status(shared, s, shared.paused ? 3 : 1,
               shared.paused ? L"Preview paused" : L"Waiting for preview viewer");
        return false;
    }
    if (!s.connected || s.consumer != shared.consumer_pid) {
        shared.ready = 0;
        invalidate(s);
        s.connected = true;
        s.consumer = shared.consumer_pid;
    }
    if (shared.ready == 2) {
        invalidate(s);
        s.connected = true;
        s.consumer = shared.consumer_pid;
        shared.ready = 0;
    }
    status(shared, s, 0, L"Capturing submitted OpenXR views");
    if (shared.ready) {
        ++shared.dropped_frames;
        return false;
    }
    if (!s.limiter.due(monotonic_ns(), shared.fps))
        return false;
    return true;
}
void release_owner(Session &s) {
    if (owner && owner != &s)
        return;
    ChannelLock lock;
    if (lock && channel.data()->producer_pid == GetCurrentProcessId()) {
        channel.data()->ready = 0;
        channel.data()->producer_pid = 0;
        channel.data()->status_code = 1;
        std::wcsncpy(channel.data()->status, L"OpenXR session ended", 191);
    }
    owner = nullptr;
}
bool supported_format(std::int64_t format) {
    View v{};
    v.texture_handle = 1;
    v.width = v.height = 1;
    v.rect_width = v.rect_height = 1;
    v.format = static_cast<std::uint32_t>(format);
    return valid_view(v);
}
bool snapshot_before_release(Swapchain &sc) {
    Session &s = *sc.session;
    if (!s.device || !s.context || sc.acquired.empty() || !sc.acquired.front().waited ||
        !(sc.info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ||
        (sc.info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) ||
        !supported_format(sc.info.format))
        return false;
    ChannelLock lock;
    if (!lock || !eligible(s, *channel.data(), GetTickCount64()))
        return false;
    const auto index = sc.acquired.front().index;
    if (index >= sc.images.size() || !sc.images[index].texture)
        return false;
    auto *source = sc.images[index].texture;
    D3D11_TEXTURE2D_DESC src{};
    source->GetDesc(&src);
    if (!src.Width || !src.Height || src.Width > 16384 || src.Height > 16384 || !src.ArraySize ||
        src.ArraySize > 2048 || src.Width != sc.info.width || src.Height != sc.info.height ||
        src.ArraySize != sc.info.arraySize || sc.info.faceCount != 1)
        return false;
    D3D11_TEXTURE2D_DESC desc = src;
    desc.MipLevels = 1;
    desc.Format = static_cast<DXGI_FORMAT>(sc.info.format);
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    if (!sc.scratch && FAILED(s.device->CreateTexture2D(&desc, nullptr, sc.scratch.put())))
        return false;
    if (src.SampleDesc.Count > 1) {
        UINT support{};
        if (FAILED(s.device->CheckFormatSupport(desc.Format, &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE))
            return false;
    }
    for (UINT slice = 0; slice < src.ArraySize; ++slice) {
        const UINT source_sub = D3D11CalcSubresource(0, slice, src.MipLevels);
        if (src.SampleDesc.Count > 1)
            s.context->ResolveSubresource(sc.scratch.p, slice, source, source_sub, desc.Format);
        else
            s.context->CopySubresourceRegion(sc.scratch.p, slice, 0, 0, 0, source, source_sub, nullptr);
    }
    // Submit these reads before handing ownership back to the runtime.
    s.context->Flush();
    return true;
}
bool prepare_output(Session &s, Output &o, const D3D11_TEXTURE2D_DESC &source) {
    if (o.texture && o.desc.Width == source.Width && o.desc.Height == source.Height &&
        o.desc.Format == source.Format)
        return true;
    o = Output{};
    o.desc = source;
    o.desc.ArraySize = 1;
    o.desc.MipLevels = 1;
    o.desc.SampleDesc = {1, 0};
    o.desc.Usage = D3D11_USAGE_DEFAULT;
    o.desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    o.desc.CPUAccessFlags = 0;
    o.desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(s.device->CreateTexture2D(&o.desc, nullptr, o.texture.put())) ||
        FAILED(o.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(o.key.put()))))
        return false;
    Com<IDXGIResource> resource;
    if (FAILED(
            o.texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(resource.put()))) ||
        FAILED(resource->GetSharedHandle(&o.handle)))
        return false;
    s.generation = ++next_generation;
    return true;
}
void publish(Session &s, const XrFrameEndInfo &end) {
    ChannelLock lock;
    if (!lock || !eligible(s, *channel.data(), GetTickCount64()))
        return;
    Shared &shared = *channel.data();
    const XrCompositionLayerProjection *projection{};
    for (std::uint32_t i = 0; i < end.layerCount && end.layers; ++i)
        if (end.layers[i] && end.layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            projection = reinterpret_cast<const XrCompositionLayerProjection *>(end.layers[i]);
            break;
        }
    if (!projection || !projection->views || (projection->viewCount != 2 && projection->viewCount != 4)) {
        status(shared, s, 4, L"Waiting for supported stereo or quad projection");
        return;
    }
    const unsigned eye = std::min(shared.eye, 1u), count = projection->viewCount == 4 ? 2 : 1;
    Frame frame{};
    frame.view_count = count;
    frame.producer_pid = GetCurrentProcessId();
    frame.adapter = s.adapter;
    frame.session = s.cookie;
    frame.display_time = end.displayTime;
    if ((s.pending_reference && end.displayTime >= s.pending_change) ||
        (s.space && s.space != projection->space)) {
        ++s.epoch;
        s.pending_reference = false;
    }
    s.space = projection->space;
    frame.reference_epoch = s.epoch;
    Swapchain *sources[2]{};
    UINT slices[2]{};
    // Validate every selected view before allocating, locking or copying a frame.
    for (unsigned i = 0; i < count; ++i) {
        const auto &xr = projection->views[eye + i * 2];
        auto found = swapchains.find(xr.subImage.swapchain);
        if (found == swapchains.end())
            return;
        auto &sc = *found->second;
        if (sc.session != &s || !sc.snapshot_valid || !sc.snapshot ||
            xr.subImage.imageArrayIndex >= sc.info.arraySize)
            return;
        sources[i] = &sc;
        slices[i] = xr.subImage.imageArrayIndex;
        View &v = frame.views[i];
        v.texture_handle = 1;
        v.width = sc.info.width;
        v.height = sc.info.height;
        v.format = static_cast<std::uint32_t>(sc.info.format);
        v.x = xr.subImage.imageRect.offset.x;
        v.y = xr.subImage.imageRect.offset.y;
        v.rect_width = xr.subImage.imageRect.extent.width;
        v.rect_height = xr.subImage.imageRect.extent.height;
        v.orientation[0] = xr.pose.orientation.x;
        v.orientation[1] = xr.pose.orientation.y;
        v.orientation[2] = xr.pose.orientation.z;
        v.orientation[3] = xr.pose.orientation.w;
        const float angles[]{xr.fov.angleLeft, xr.fov.angleRight, xr.fov.angleUp, xr.fov.angleDown};
        for (unsigned j = 0; j < 4; ++j) {
            if (!std::isfinite(angles[j]) || std::abs(angles[j]) >= 1.5707963f)
                return;
            v.fov[j] = std::tan(angles[j]);
        }
        if (!valid_view(v)) {
            status(shared, s, 4, L"Unsupported projection metadata");
            return;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        sources[i]->snapshot->GetDesc(&desc);
        if (!prepare_output(s, s.outputs[i], desc)) {
            status(shared, s, 5, L"Unable to create shared preview textures");
            return;
        }
    }
    unsigned acquired = 0;
    bool reset_needed = false;
    for (; acquired < count; ++acquired) {
        auto &o = s.outputs[acquired];
        HRESULT hr = o.key->AcquireSync(0, 0);
        // ready is zero under the header mutex: key 1 can only be a discarded unread frame.
        if (hr == WAIT_TIMEOUT)
            hr = o.key->AcquireSync(1, 0);
        if (hr != S_OK) {
            reset_needed = hr != WAIT_TIMEOUT;
            break;
        }
    }
    if (acquired != count) {
        for (unsigned i = 0; i < acquired; ++i)
            if (FAILED(s.outputs[i].key->ReleaseSync(0)))
                reset_needed = true;
        if (reset_needed)
            invalidate(s);
        ++shared.dropped_frames;
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        auto &o = s.outputs[i];
        s.context->CopySubresourceRegion(o.texture.p, 0, 0, 0, 0, sources[i]->snapshot.p, slices[i], nullptr);
        frame.views[i].texture_handle = reinterpret_cast<std::uint64_t>(o.handle);
    }
    s.context->Flush();
    bool released = true;
    for (unsigned i = 0; i < count; ++i)
        if (FAILED(s.outputs[i].key->ReleaseSync(1)))
            released = false;
    if (!released) {
        invalidate(s);
        return;
    }
    frame.generation = s.generation;
    frame.sequence = ++s.sequence;
    frame.published_ms = GetTickCount64();
    shared.frame = frame;
    shared.ready = 1;
    ++shared.published_frames;
    s.limiter.published(monotonic_ns(), shared.fps);
}
#define STATE_LOCK std::lock_guard<std::recursive_mutex> guard(state_mutex)
XrResult XRAPI_CALL layer_destroy_instance(XrInstance handle) {
    STATE_LOCK;
    auto it = instances.find(handle);
    if (it == instances.end())
        return XR_ERROR_HANDLE_INVALID;
    const auto result = it->second->DestroyInstance(handle);
    if (XR_SUCCEEDED(result)) {
        for (auto si = sessions.begin(); si != sessions.end();) {
            if (si->second->instance == it->second.get()) {
                release_owner(*si->second);
                for (auto sc = swapchains.begin(); sc != swapchains.end();)
                    if (sc->second->session == si->second.get())
                        sc = swapchains.erase(sc);
                    else
                        ++sc;
                si = sessions.erase(si);
            } else
                ++si;
        }
        instances.erase(it);
    }
    return result;
}
XrResult XRAPI_CALL layer_create_session(XrInstance handle, const XrSessionCreateInfo *info, XrSession *out) {
    STATE_LOCK;
    auto it = instances.find(handle);
    if (it == instances.end())
        return XR_ERROR_HANDLE_INVALID;
    auto result = it->second->CreateSession(handle, info, out);
    if (XR_SUCCEEDED(result) && out) {
        try {
            auto s = std::make_unique<Session>();
            s->instance = it->second.get();
            s->handle = *out;
            s->cookie = next_cookie++;
            s->generation = ++next_generation;
            for (auto *next = info ? static_cast<const XrBaseInStructure *>(info->next) : nullptr; next;
                 next = next->next)
                if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    auto *binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR *>(next);
                    s->device.p = binding->device;
                    if (s->device) {
                        s->device->AddRef();
                        s->device->GetImmediateContext(s->context.put());
                        Com<IDXGIDevice> dx;
                        Com<IDXGIAdapter> adapter;
                        DXGI_ADAPTER_DESC desc{};
                        if (SUCCEEDED(s->device->QueryInterface(__uuidof(IDXGIDevice),
                                                                reinterpret_cast<void **>(dx.put()))) &&
                            SUCCEEDED(dx->GetAdapter(adapter.put())) && SUCCEEDED(adapter->GetDesc(&desc)))
                            s->adapter = desc.AdapterLuid;
                    }
                    break;
                }
            auto *ptr = s.get();
            sessions[*out] = std::move(s);
            ChannelLock lock;
            if (lock)
                eligible(*ptr, *channel.data(), GetTickCount64());
        } catch (...) {
        }
    }
    return result;
}
XrResult XRAPI_CALL layer_destroy_session(XrSession handle) {
    STATE_LOCK;
    auto it = sessions.find(handle);
    if (it == sessions.end())
        return XR_ERROR_HANDLE_INVALID;
    auto result = it->second->instance->DestroySession(handle);
    if (XR_SUCCEEDED(result)) {
        release_owner(*it->second);
        for (auto sc = swapchains.begin(); sc != swapchains.end();)
            if (sc->second->session == it->second.get())
                sc = swapchains.erase(sc);
            else
                ++sc;
        sessions.erase(it);
    }
    return result;
}
XrResult XRAPI_CALL layer_create_swapchain(XrSession handle, const XrSwapchainCreateInfo *info,
                                           XrSwapchain *out) {
    STATE_LOCK;
    auto it = sessions.find(handle);
    if (it == sessions.end())
        return XR_ERROR_HANDLE_INVALID;
    auto &s = *it->second;
    auto result = s.instance->CreateSwapchain(handle, info, out);
    if (XR_SUCCEEDED(result) && out && info) {
        try {
            auto sc = std::make_unique<Swapchain>();
            sc->session = &s;
            sc->info = *info;
            sc->info.next = nullptr;
            if (s.device && supported_format(info->format) &&
                (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
                uint32_t n{};
                if (XR_SUCCEEDED(s.instance->EnumerateSwapchainImages(*out, 0, &n, nullptr)) && n &&
                    n <= 256) {
                    sc->images.resize(n);
                    for (auto &image : sc->images)
                        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
                    uint32_t actual{};
                    if (XR_FAILED(s.instance->EnumerateSwapchainImages(
                            *out, n, &actual,
                            reinterpret_cast<XrSwapchainImageBaseHeader *>(sc->images.data()))) ||
                        actual > n)
                        sc->images.clear();
                    else
                        sc->images.resize(actual);
                }
            }
            swapchains[*out] = std::move(sc);
        } catch (...) {
        }
    }
    return result;
}
XrResult XRAPI_CALL layer_destroy_swapchain(XrSwapchain handle) {
    STATE_LOCK;
    auto it = swapchains.find(handle);
    if (it == swapchains.end())
        return XR_ERROR_HANDLE_INVALID;
    auto result = it->second->session->instance->DestroySwapchain(handle);
    if (XR_SUCCEEDED(result))
        swapchains.erase(it);
    return result;
}
XrResult XRAPI_CALL layer_acquire(XrSwapchain handle, const XrSwapchainImageAcquireInfo *info,
                                  uint32_t *index) {
    STATE_LOCK;
    auto it = swapchains.find(handle);
    if (it == swapchains.end())
        return XR_ERROR_HANDLE_INVALID;
    auto result = it->second->session->instance->AcquireSwapchainImage(handle, info, index);
    if (result == XR_SUCCESS && index) {
        try {
            it->second->acquired.push_back({*index, false});
        } catch (...) {
            it->second->acquired.clear();
        }
    }
    return result;
}
XrResult XRAPI_CALL layer_wait_image(XrSwapchain handle, const XrSwapchainImageWaitInfo *info) {
    PFN_xrWaitSwapchainImage next{};
    {
        STATE_LOCK;
        auto it = swapchains.find(handle);
        if (it == swapchains.end())
            return XR_ERROR_HANDLE_INVALID;
        next = it->second->session->instance->WaitSwapchainImage;
    }
    // Waiting for an image can require another application thread to finish a frame.
    auto result = next(handle, info);
    if (result == XR_SUCCESS) {
        STATE_LOCK;
        auto it = swapchains.find(handle);
        if (it != swapchains.end())
            for (auto &ac : it->second->acquired)
                if (!ac.waited) {
                    ac.waited = true;
                    break;
                }
    }
    return result;
}
XrResult XRAPI_CALL layer_release(XrSwapchain handle, const XrSwapchainImageReleaseInfo *info) {
    STATE_LOCK;
    auto it = swapchains.find(handle);
    if (it == swapchains.end())
        return XR_ERROR_HANDLE_INVALID;
    auto &sc = *it->second;
    bool captured = false;
    try {
        captured = snapshot_before_release(sc);
    } catch (...) {
    }
    auto result = sc.session->instance->ReleaseSwapchainImage(handle, info);
    if (XR_SUCCEEDED(result)) {
        if (!sc.acquired.empty())
            sc.acquired.pop_front();
        sc.snapshot_valid = captured;
        if (captured) {
            std::swap(sc.snapshot, sc.scratch);
        }
    } else
        sc.snapshot_valid = false;
    return result;
}
XrResult XRAPI_CALL layer_wait_frame(XrSession handle, const XrFrameWaitInfo *info, XrFrameState *out) {
    // xrWaitFrame can block inside the runtime: do not hold the layer state lock while waiting.
    PFN_xrWaitFrame next{};
    {
        STATE_LOCK;
        auto it = sessions.find(handle);
        if (it == sessions.end())
            return XR_ERROR_HANDLE_INVALID;
        next = it->second->instance->WaitFrame;
    }
    auto result = next(handle, info, out);

    return result;
}
XrResult XRAPI_CALL layer_end_frame(XrSession handle, const XrFrameEndInfo *info) {
    STATE_LOCK;
    auto it = sessions.find(handle);
    if (it == sessions.end())
        return XR_ERROR_HANDLE_INVALID;
    auto &s = *it->second;
    // Runtime sees the identical pointer and contents. Images here are layer-owned snapshots only.
    auto result = s.instance->EndFrame(handle, info);
    if (XR_SUCCEEDED(result) && info) {
        try {
            publish(s, *info);
        } catch (...) {
        }
    }
    return result;
}
XrResult XRAPI_CALL layer_poll(XrInstance handle, XrEventDataBuffer *event) {
    STATE_LOCK;
    auto it = instances.find(handle);
    if (it == instances.end())
        return XR_ERROR_HANDLE_INVALID;
    auto result = it->second->PollEvent(handle, event);
    if (result == XR_SUCCESS && event && event->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        auto *e = reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(event);
        auto s = sessions.find(e->session);
        if (s != sessions.end()) {
            s->second->pending_change = e->changeTime;
            s->second->pending_reference = true;
        }
    }
    return result;
}
XrResult XRAPI_CALL layer_gipa(XrInstance handle, const char *name, PFN_xrVoidFunction *out) {
    if (!name || !out)
        return XR_ERROR_VALIDATION_FAILURE;
    STATE_LOCK;
#define HOOK(n, f)                                                                                           \
    if (std::strcmp(name, n) == 0) {                                                                         \
        *out = reinterpret_cast<PFN_xrVoidFunction>(f);                                                      \
        return XR_SUCCESS;                                                                                   \
    }
    HOOK("xrGetInstanceProcAddr", layer_gipa)
    HOOK("xrDestroyInstance", layer_destroy_instance)
    HOOK("xrCreateSession", layer_create_session)
    HOOK("xrDestroySession", layer_destroy_session) HOOK("xrCreateSwapchain", layer_create_swapchain)
        HOOK("xrDestroySwapchain", layer_destroy_swapchain) HOOK("xrAcquireSwapchainImage", layer_acquire)
            HOOK("xrWaitSwapchainImage", layer_wait_image) HOOK("xrReleaseSwapchainImage", layer_release)
                HOOK("xrWaitFrame", layer_wait_frame) HOOK("xrEndFrame", layer_end_frame)
                    HOOK("xrPollEvent", layer_poll)
#undef HOOK
                        auto it = instances.find(handle);
    auto next = it == instances.end() ? global_next : it->second->gipa;
    if (next)
        return next(handle, name, out);
    *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL layer_create_instance(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *layer,
                                          XrInstance *out) {
    if (!info || !layer || !out || !layer->nextInfo || !layer->nextInfo->nextCreateApiLayerInstance ||
        !layer->nextInfo->nextGetInstanceProcAddr)
        return XR_ERROR_INITIALIZATION_FAILED;
    auto *next = layer->nextInfo;
    XrApiLayerCreateInfo forwarded = *layer;
    forwarded.nextInfo = next->next;
    const auto result = next->nextCreateApiLayerInstance(info, &forwarded, out);
    if (XR_FAILED(result))
        return result;
    STATE_LOCK;
    try {
        auto instance = std::make_unique<Instance>();
        instance->handle = *out;
        instance->gipa = next->nextGetInstanceProcAddr;
#define LOAD(n) instance->gipa(*out, "xr" #n, reinterpret_cast<PFN_xrVoidFunction *>(&instance->n));
        LOAD(DestroyInstance)
        LOAD(CreateSession)
        LOAD(DestroySession)
        LOAD(CreateSwapchain) LOAD(DestroySwapchain) LOAD(EnumerateSwapchainImages)
            LOAD(AcquireSwapchainImage) LOAD(WaitSwapchainImage) LOAD(ReleaseSwapchainImage) LOAD(WaitFrame)
                LOAD(EndFrame) LOAD(PollEvent)
#undef LOAD
                    MultiByteToWideChar(CP_UTF8, 0, info->applicationInfo.applicationName, -1,
                                        instance->application, 128);
        global_next = instance->gipa;
        instances[*out] = std::move(instance);
    } catch (...) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
    return result;
}
} // namespace
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loader, const char *name, XrNegotiateApiLayerRequest *request) {
    if (!loader || !request || !name || std::strcmp(name, layer_name) != 0 ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(*loader) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize != sizeof(*request) || loader->minInterfaceVersion > 1 ||
        loader->maxInterfaceVersion < 1 || loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) ||
        loader->minApiVersion > XR_CURRENT_API_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = 1;
    request->layerApiVersion = std::min(loader->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = layer_gipa;
    request->createApiLayerInstance = layer_create_instance;
    return XR_SUCCESS;
}
