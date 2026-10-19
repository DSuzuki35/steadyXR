// OpenXR implicit layer: optional render expansion and submitted projection capture.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "build_expiry.hpp"
#include "render_settings.hpp"
#include "settings_path.hpp"
#include "transport.hpp"
#include "gaze_capture.hpp"
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
#ifdef VRST_PREVIEW_TEST_CLOCK
extern "C" void vrst_preview_test_copy_queued();
#endif

namespace {
using namespace vrst::preview;
inline void record_copy_queued() {
#ifdef VRST_PREVIEW_TEST_CLOCK
    vrst_preview_test_copy_queued();
#endif
}
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
    RenderSettings render{}; // Immutable for the lifetime of this instance.
    GazeRuntime gaze;
#define PROC(n) PFN_xr##n n{};
    PROC(DestroyInstance)
    PROC(CreateSession)
    PROC(DestroySession)
    PROC(LocateViews)
    PROC(LocateSpace)
    PROC(EnumerateViewConfigurationViews)
    PROC(GetVisibilityMaskKHR)
    PROC(CreateSwapchain)
    PROC(DestroySwapchain)
    PROC(EnumerateSwapchainImages)
    PROC(AcquireSwapchainImage)
    PROC(WaitSwapchainImage) PROC(ReleaseSwapchainImage) PROC(WaitFrame) PROC(EndFrame) PROC(PollEvent)
#undef PROC
};
struct Output {
    Com<ID3D11Texture2D> texture;
    Com<IDXGIKeyedMutex> key;
    HANDLE handle{};
    D3D11_TEXTURE2D_DESC desc{};
    std::uint64_t copied_swapchain{}, copied_revision{};
    UINT copied_slice{};
};
struct Session {
    Instance *instance{};
    XrSession handle{};
    GazeCapture gaze;
    std::uint64_t cookie{}, epoch{1}, generation{}, sequence{};
    RateLimiter limiter;
    XrSpace space{};
    XrTime pending_change{};
    bool pending_reference{}, connected{};
    bool gaze_requested{}; // Refreshed by the existing capture eligibility check.
    std::uint32_t consumer{};
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    LUID adapter{};
    std::array<Output, 2> outputs;
    std::array<Output, max_overlay_layers> overlay_outputs;
    struct Identity {
        XrStructureType type{};
        std::uint64_t swapchain{};
        unsigned slice{};
        std::int32_t x{}, y{}, width{}, height{};
        bool ambiguous{}; // A collision cannot become distinguishable again within this session.
        bool operator==(const Identity &other) const {
            return type == other.type && swapchain == other.swapchain && slice == other.slice &&
                   x == other.x && y == other.y && width == other.width && height == other.height;
        }
    };
    std::vector<Identity> overlay_identities; // Never recycle a slot within this session.
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
    bool overlay_seen{};
    bool released_once{};
    std::uint64_t identity{};
    std::uint64_t snapshot_revision{};
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
std::uint64_t next_cookie = [] {
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}();
std::uint64_t next_generation = 1, next_swapchain_identity = 1;
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
    shared.render_enabled = s.instance->render.enabled ? 1u : 0u;
    for (unsigned eye = 0; eye < 2; ++eye) {
        shared.render_left[eye] = float(s.instance->render.eyes[eye].left);
        shared.render_right[eye] = float(s.instance->render.eyes[eye].right);
    }
    std::wcsncpy(shared.application, s.instance->application, 127);
    shared.application[127] = 0;
    std::wcsncpy(shared.status, text, 191);
    shared.status[191] = 0;
}
void invalidate(Session &s) {
    s.outputs = {};
    s.overlay_outputs = {};
    s.generation = ++next_generation;
    s.connected = false;
    s.limiter.reset();
    // Private snapshots retain the last successful release, including STATIC_IMAGE
    // swapchains that can never be acquired again. Only shared outputs are invalidated.
}
// Called only while the shared header mutex is held; never waits for a viewer.
bool eligible(Session &s, Shared &shared, std::uint64_t now) {
    s.gaze_requested = false;
    if (owner && owner != &s)
        return false;
    if (shared.producer_pid && shared.producer_pid != GetCurrentProcessId() &&
        process_alive(shared.producer_pid))
        return false;
    if (build_expired()) {
        shared.ready = 0;
        if (s.connected)
            invalidate(s);
        status(shared, s, 6, L"Preview build expired");
        return false;
    }
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
    s.gaze_requested = active && shared.gaze_requested != 0;
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
        channel.data()->overlay_catalog = {};
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
        (sc.info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) ||
        !supported_format(sc.info.format))
        return false;
    ChannelLock lock;
    const bool static_image = (sc.info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
    // Static content may be released before a viewer exists. This is its only legal
    // capture opportunity; never read a released runtime texture to recover later.
    bool capture = static_image;
    if (!capture && lock) {
        const auto now = GetTickCount64();
        capture = eligible(s, *channel.data(), now);
        // Publishing may be throttled or backpressured, but a known overlay can
        // update only once. Retain that fresh legal release for its next submission.
        if (!capture && (sc.overlay_seen || !sc.released_once))
            capture = owner == &s && channel.data()->producer_pid == GetCurrentProcessId() &&
                      viewer_active(*channel.data(), now);
    }
    if (build_expired() || !capture)
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
        record_copy_queued();
        if (src.SampleDesc.Count > 1)
            s.context->ResolveSubresource(sc.scratch.p, slice, source, source_sub, desc.Format);
        else
            s.context->CopySubresourceRegion(sc.scratch.p, slice, 0, 0, 0, source, source_sub, nullptr);
    }
    // Submit these reads before handing ownership back to the runtime.
    s.context->Flush();
    return true;
}
bool prepare_output(Session &s, Output &o, const D3D11_TEXTURE2D_DESC &source, bool base = true) {
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
    if (base)
        s.generation = ++next_generation;
    return true;
}
XrQuaternionf multiply(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrVector3f rotate(XrQuaternionf q, XrVector3f p) {
    auto r = multiply(multiply(q, {p.x, p.y, p.z, 0}), {-q.x, -q.y, -q.z, q.w});
    return {r.x, r.y, r.z};
}
bool normalized_pose(XrPosef &p) {
    auto &q = p.orientation;
    float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(norm) || norm < .25f || norm > 4 || !std::isfinite(p.position.x) ||
        !std::isfinite(p.position.y) || !std::isfinite(p.position.z))
        return false;
    const float n = std::sqrt(norm);
    q.x /= n;
    q.y /= n;
    q.z /= n;
    q.w /= n;
    return true;
}
XrPosef compose(XrPosef a, XrPosef b) {
    auto p = rotate(a.orientation, b.position);
    return {multiply(a.orientation, b.orientation),
            {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}
bool relative_pose(Session &s, XrSpace space, XrSpace base, XrTime time, XrPosef pose, XrPosef eye,
                   OverlayImage &image) {
    if (!normalized_pose(pose) || !normalized_pose(eye))
        return false;
    if (space != base) {
        XrSpaceLocation located{XR_TYPE_SPACE_LOCATION};
        constexpr auto valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (!s.instance->LocateSpace || XR_FAILED(s.instance->LocateSpace(space, base, time, &located)) ||
            (located.locationFlags & valid) != valid || !normalized_pose(located.pose))
            return false;
        pose = compose(located.pose, pose);
    }
    XrPosef inverse{};
    inverse.orientation = {-eye.orientation.x, -eye.orientation.y, -eye.orientation.z, eye.orientation.w};
    inverse.position = rotate(inverse.orientation, {-eye.position.x, -eye.position.y, -eye.position.z});
    pose = compose(inverse, pose);
    image.orientation[0] = pose.orientation.x;
    image.orientation[1] = pose.orientation.y;
    image.orientation[2] = pose.orientation.z;
    image.orientation[3] = pose.orientation.w;
    image.position[0] = pose.position.x;
    image.position[1] = pose.position.y;
    image.position[2] = pose.position.z;
    return true;
}
bool subimage_view(Session &s, const XrSwapchainSubImage &sub, View &view, Swapchain *&source) {
    const auto found = swapchains.find(sub.swapchain);
    if (found == swapchains.end())
        return false;
    auto &sc = *found->second;
    if (sc.session == &s)
        sc.overlay_seen = true;
    if (sc.session != &s || !sc.snapshot_valid || !sc.snapshot || sc.info.faceCount != 1 ||
        sub.imageArrayIndex >= sc.info.arraySize ||
        (sc.info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))
        return false;
    view.texture_handle = 1;
    view.width = sc.info.width;
    view.height = sc.info.height;
    view.format = std::uint32_t(sc.info.format);
    view.x = sub.imageRect.offset.x;
    view.y = sub.imageRect.offset.y;
    view.rect_width = sub.imageRect.extent.width;
    view.rect_height = sub.imageRect.extent.height;
    source = &sc;
    return valid_view(view);
}
bool projection_fov(const XrFovf &fov, View &view) {
    const float angles[]{fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
    for (unsigned j = 0; j < 4; ++j) {
        if (!std::isfinite(angles[j]) || std::abs(angles[j]) >= 1.5707963f)
            return false;
        view.fov[j] = std::tan(angles[j]);
    }
    return true;
}
Session::Identity overlay_identity(const XrCompositionLayerBaseHeader &layer) {
    Session::Identity identity{};
    identity.type = layer.type;
    const XrSwapchainSubImage *sub{};
    if (layer.type == XR_TYPE_COMPOSITION_LAYER_QUAD)
        sub = &reinterpret_cast<const XrCompositionLayerQuad &>(layer).subImage;
    else if (layer.type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR)
        sub = &reinterpret_cast<const XrCompositionLayerCylinderKHR &>(layer).subImage;
    else if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const auto &projection = reinterpret_cast<const XrCompositionLayerProjection &>(layer);
        if (projection.views && (projection.viewCount == 2 || projection.viewCount == 4))
            sub = &projection.views[0].subImage; // Identity remains stable when the captured eye changes.
    }
    if (sub) {
        const auto found = swapchains.find(sub->swapchain);
        if (found != swapchains.end())
            identity.swapchain = found->second->identity;
        identity.slice = sub->imageArrayIndex;
        identity.x = sub->imageRect.offset.x;
        identity.y = sub->imageRect.offset.y;
        identity.width = sub->imageRect.extent.width;
        identity.height = sub->imageRect.extent.height;
    }
    return identity;
}
// Enumerate metadata independently of selection. Unknown layers are never cast to
// extension structures. Identity includes a non-reused swapchain cookie, not a pointer.
bool collect_overlays(Session &s, Shared &shared, const XrFrameEndInfo &end,
                      const XrCompositionLayerProjection &primary, unsigned eye, Frame &frame,
                      Swapchain **sources, UINT *slices, Output **outputs) {
    OverlayCatalog catalog{};
    catalog.producer_pid = GetCurrentProcessId();
    catalog.session = s.cookie;
    catalog.updated_ms = GetTickCount64();
    std::vector<Session::Identity> seen;
    bool limited = end.layerCount > 256, skipped_primary = false;
    for (unsigned i = 0; i < std::min(end.layerCount, 256u) && end.layers; ++i) {
        const auto *layer = end.layers[i];
        if (!layer)
            continue;
        if (!skipped_primary && layer == reinterpret_cast<const XrCompositionLayerBaseHeader *>(&primary)) {
            skipped_primary = true;
            continue;
        }
        OverlayImage image{};
        const XrSwapchainSubImage *sub{};
        XrPosef pose{{0, 0, 0, 1}, {}};
        XrEyeVisibility visibility = XR_EYE_VISIBILITY_BOTH;
        const wchar_t *name = L"Unsupported OpenXR type";
        bool metadata = true;
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto &q = *reinterpret_cast<const XrCompositionLayerQuad *>(layer);
            image.type = 1;
            name = L"Quad";
            sub = &q.subImage;
            pose = q.pose;
            visibility = q.eyeVisibility;
            image.width = q.size.width;
            image.height = q.size.height;
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            const auto &c = *reinterpret_cast<const XrCompositionLayerCylinderKHR *>(layer);
            image.type = 2;
            name = L"Cylinder";
            sub = &c.subImage;
            pose = c.pose;
            visibility = c.eyeVisibility;
            image.radius = c.radius;
            image.central_angle = c.centralAngle;
            image.aspect_ratio = c.aspectRatio;
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto &p = *reinterpret_cast<const XrCompositionLayerProjection *>(layer);
            image.type = 3;
            name = p.viewCount == 4 ? L"Projection (context only)" : L"Projection";
            if (!p.views || (p.viewCount != 2 && p.viewCount != 4))
                metadata = false;
            else {
                // Quad-view context indices remain left=0/right=1; focus views are not a separate overlay.
                const auto &v = p.views[eye];
                sub = &v.subImage;
                pose = v.pose;
                metadata = projection_fov(v.fov, image.image);
            }
        }
        const auto key = overlay_identity(*layer);
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue; // One disabled catalog entry represents indistinguishable submissions.
        bool ambiguous = false;
        for (unsigned j = i + 1; j < std::min(end.layerCount, 256u); ++j)
            if (end.layers[j] && overlay_identity(*end.layers[j]) == key)
                ambiguous = true;
        seen.push_back(key);
        unsigned slot = 0;
        for (; slot < s.overlay_identities.size(); ++slot) {
            const auto &known = s.overlay_identities[slot];
            if (known == key)
                break;
        }
        if (slot == s.overlay_identities.size()) {
            if (slot == max_overlay_layers) {
                limited = true;
                continue;
            }
            s.overlay_identities.push_back(key);
        }
        if (ambiguous)
            s.overlay_identities[slot].ambiguous = true;
        image.slot = slot;
        auto &info = catalog.layers[catalog.count++];
        info.slot = slot;
        info.type = image.type;
        if (sub) {
            info.width = std::uint32_t(std::max(0, sub->imageRect.extent.width));
            info.height = std::uint32_t(std::max(0, sub->imageRect.extent.height));
        }
        const bool visible = visibility == XR_EYE_VISIBILITY_BOTH ||
                             (eye == 0 && visibility == XR_EYE_VISIBILITY_LEFT) ||
                             (eye == 1 && visibility == XR_EYE_VISIBILITY_RIGHT);
        Swapchain *source{};
        const wchar_t *reason = L"";
        if (!skipped_primary)
            reason = L"behind game projection";
        else if (s.overlay_identities[slot].ambiguous)
            reason = L"ambiguous shared image";
        else if (!image.type)
            reason = L"unsupported layer type";
        else if (layer->next)
            reason = L"unsupported layer extension chain";
        else if (!metadata || !sub)
            reason = L"unsupported view metadata";
        else if (!subimage_view(s, *sub, image.image, source))
            reason = L"image unavailable or unsupported";
        else {
            image.flags =
                ((layer->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? 1u : 0u) |
                ((layer->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 2u : 0u);
            // Catalog discovery does not require tracking or issue xrLocateSpace calls.
            info.supported = valid_overlay(image) ? 1u : 0u;
            if (!info.supported)
                reason = L"invalid geometry";
        }
        const bool selected =
            overlay_selected(shared.overlay_selection, catalog.producer_pid, s.cookie, slot);
        if (info.supported && visible && selected) {
            if (frame.overlay_count == max_overlay_images) {
                limited = true;
                reason = L"8-image capture limit";
            } else if (!relative_pose(s, layer->space, primary.space, end.displayTime, pose,
                                      primary.views[eye].pose, image))
                reason = L"space pose unavailable";
            else {
                std::copy(std::begin(image.orientation), std::end(image.orientation),
                          image.image.orientation);
                if (valid_overlay(image)) {
                    const auto n = frame.overlay_count++;
                    frame.overlays[n] = image;
                    sources[n] = source;
                    slices[n] = sub->imageArrayIndex;
                    outputs[n] = &s.overlay_outputs[slot];
                } else
                    reason = L"invalid relative pose";
            }
        } else if (info.supported && !visible)
            reason = L"other eye";
        std::swprintf(info.label, 128, L"%ls %ux%u [%u]%ls%ls", name, info.width, info.height, slot + 1,
                      *reason ? L" - " : L"", reason);
    }
    shared.overlay_catalog = catalog;
    return limited;
}
void publish(Session &s, const XrFrameEndInfo &end) {
    ChannelLock lock;
    if (!lock || !eligible(s, *channel.data(), GetTickCount64()))
        return;
    Shared &shared = *channel.data();
    shared.overlay_catalog = {};
    shared.overlay_catalog.producer_pid = GetCurrentProcessId();
    shared.overlay_catalog.session = s.cookie;
    shared.overlay_catalog.updated_ms = GetTickCount64();
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
    frame.eye = eye;
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
    Swapchain *sources[2 + max_overlay_images]{};
    UINT slices[2 + max_overlay_images]{};
    Output *outputs[2 + max_overlay_images]{&s.outputs[0], &s.outputs[1]};
    const bool overlay_limited = collect_overlays(s, shared, end, *projection, eye, frame, sources + count,
                                                  slices + count, outputs + count);
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
    const unsigned total = count + frame.overlay_count;
    if (overlay_limited)
        status(shared, s, 0, L"Capturing OpenXR views; overlay limit (16 slots / 8 images)");
    for (unsigned i = 0; i < total; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        sources[i]->snapshot->GetDesc(&desc);
        if (!prepare_output(s, *outputs[i], desc, i < count)) {
            status(shared, s, 5, L"Unable to create shared preview textures");
            return;
        }
    }
    unsigned acquired = 0;
    bool reset_needed = false;
    for (; acquired < total; ++acquired) {
        auto &o = *outputs[acquired];
        HRESULT hr = o.key->AcquireSync(0, 0);
        // ready is zero under the header mutex: key 1 can only be a discarded unread frame.
        if (hr == WAIT_TIMEOUT)
            hr = o.key->AcquireSync(1, 0);
        if (hr != S_OK) {
            reset_needed = hr != WAIT_TIMEOUT;
            break;
        }
    }
    if (acquired != total) {
        for (unsigned i = 0; i < acquired; ++i)
            if (FAILED(outputs[i]->key->ReleaseSync(0)))
                reset_needed = true;
        if (reset_needed)
            invalidate(s);
        ++shared.dropped_frames;
        return;
    }
    for (unsigned i = 0; i < total; ++i) {
        auto &o = *outputs[i];
        const auto &source = *sources[i];
        // The consumer reads shared images without modifying them. Reused overlay
        // snapshots only need ownership transfer; base-view copying is unchanged.
        if (i < count || o.copied_swapchain != source.identity ||
            o.copied_revision != source.snapshot_revision || o.copied_slice != slices[i]) {
            record_copy_queued();
            s.context->CopySubresourceRegion(o.texture.p, 0, 0, 0, 0, source.snapshot.p, slices[i], nullptr);
        }
        auto &view = i < count ? frame.views[i] : frame.overlays[i - count].image;
        view.texture_handle = reinterpret_cast<std::uint64_t>(o.handle);
    }
    s.context->Flush();
    bool released = true;
    for (unsigned i = 0; i < total; ++i)
        if (FAILED(outputs[i]->key->ReleaseSync(1)))
            released = false;
    if (!released) {
        invalidate(s);
        return;
    }
    // Commit cache state only after every copy was submitted and every key released.
    for (unsigned i = count; i < total; ++i) {
        outputs[i]->copied_swapchain = sources[i]->identity;
        outputs[i]->copied_revision = sources[i]->snapshot_revision;
        outputs[i]->copied_slice = slices[i];
    }
    frame.generation = s.generation;
    // Only query gaze for a successfully captured frame. Disabled capture has
    // no gaze runtime calls; synchronization belongs to the game's input cycle.
    frame.gaze = s.gaze.sample(shared.gaze_requested != 0, end.displayTime, projection->space,
                               projection->views[eye].pose.orientation, s.epoch);
    shared.gaze_capability = static_cast<std::uint32_t>(s.gaze.capability);
    frame.sequence = ++s.sequence;
    frame.published_ms = GetTickCount64();
    shared.frame = frame;
    shared.ready = 1;
    ++shared.published_frames;
    s.limiter.published(monotonic_ns(), shared.fps);
}
#define STATE_LOCK std::lock_guard<std::recursive_mutex> guard(state_mutex)
bool context_configuration(XrViewConfigurationType type) {
    return type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ||
           type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
}
bool expanded_eye(const Instance &instance, XrViewConfigurationType type, uint32_t eye) {
    return instance.render.enabled && context_configuration(type) && eye < 2 &&
           (instance.render.eyes[eye].left > 0 || instance.render.eyes[eye].right > 0);
}
XrResult XRAPI_CALL layer_locate_views(XrSession handle, const XrViewLocateInfo *info, XrViewState *state,
                                       uint32_t capacity, uint32_t *count, XrView *views) {
    STATE_LOCK;
    auto it = sessions.find(handle);
    if (it == sessions.end())
        return XR_ERROR_HANDLE_INVALID;
    const auto &instance = *it->second->instance;
    if (!instance.LocateViews)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = instance.LocateViews(handle, info, state, capacity, count, views);
    if (XR_SUCCEEDED(result) && info && count && capacity && views && *count <= capacity)
        for (uint32_t eye = 0; eye < std::min(*count, 2u); ++eye) {
            if (!expanded_eye(instance, info->viewConfigurationType, eye))
                continue;
            auto &fov = views[eye].fov;
            const double left = std::tan(double(fov.angleLeft)), right = std::tan(double(fov.angleRight));
            const double width = right - left;
            if (!std::isfinite(width) || width <= 0)
                continue;
            const auto &expansion = instance.render.eyes[eye];
            if (expansion.left > 0)
                fov.angleLeft = float(std::atan(left - width * expansion.left / 100.0));
            if (expansion.right > 0)
                fov.angleRight = float(std::atan(right + width * expansion.right / 100.0));
        }
    return result;
}
XrResult XRAPI_CALL layer_view_sizes(XrInstance handle, XrSystemId system, XrViewConfigurationType type,
                                     uint32_t capacity, uint32_t *count, XrViewConfigurationView *views) {
    STATE_LOCK;
    auto it = instances.find(handle);
    if (it == instances.end())
        return XR_ERROR_HANDLE_INVALID;
    const auto &instance = *it->second;
    if (!instance.EnumerateViewConfigurationViews)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result =
        instance.EnumerateViewConfigurationViews(handle, system, type, capacity, count, views);
    if (XR_SUCCEEDED(result) && count && capacity && views && *count <= capacity)
        for (uint32_t eye = 0; eye < std::min(*count, 2u); ++eye) {
            if (!expanded_eye(instance, type, eye))
                continue;
            auto &view = views[eye];
            const auto &expansion = instance.render.eyes[eye];
            const double width =
                double(view.recommendedImageRectWidth) * (1.0 + (expansion.left + expansion.right) / 100.0);
            // Packing is unknown here. Limit additional pixels for possible side-by-side D3D11
            // textures, while retaining larger native recommendations used by arrays/separate images.
            const double requested =
                std::max(double(view.recommendedImageRectWidth), std::min(width, 8192.0));
            view.recommendedImageRectWidth =
                uint32_t(std::round(std::min(requested, double(view.maxImageRectWidth))));
        }
    return result;
}
XrResult XRAPI_CALL layer_visibility_mask(XrSession handle, XrViewConfigurationType type, uint32_t eye,
                                          XrVisibilityMaskTypeKHR kind, XrVisibilityMaskKHR *mask) {
    STATE_LOCK;
    auto it = sessions.find(handle);
    if (it == sessions.end())
        return XR_ERROR_HANDLE_INVALID;
    const auto &instance = *it->second->instance;
    if (!instance.GetVisibilityMaskKHR)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    // Preserve the runtime's validation, two-call behavior, and exact error result.
    const auto result = instance.GetVisibilityMaskKHR(handle, type, eye, kind, mask);
    if (XR_SUCCEEDED(result) && mask && expanded_eye(instance, type, eye)) {
        // KHR_visibility_mask specifies zero counts for an unavailable mask, for every mask type.
        mask->vertexCountOutput = 0;
        mask->indexCountOutput = 0;
    }
    return result;
}
XrResult XRAPI_CALL layer_destroy_instance(XrInstance handle) {
    STATE_LOCK;
    auto it = instances.find(handle);
    if (it == instances.end())
        return XR_ERROR_HANDLE_INVALID;
    for (auto &entry : sessions)
        if (entry.second->instance == it->second.get()) entry.second->gaze.release();
    it->second->gaze.release();
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
            s->gaze.initialize(it->second->gaze, *out, info ? info->systemId : XR_NULL_SYSTEM_ID);
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
    it->second->gaze.release();
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
            sc->identity = next_swapchain_identity++;
            sc->info = *info;
            sc->info.next = nullptr;
            if (s.device && supported_format(info->format) &&
                !(info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) {
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
        sc.released_once = true;
        if (!sc.acquired.empty())
            sc.acquired.pop_front();
        sc.snapshot_valid = captured;
        if (captured) {
            std::swap(sc.snapshot, sc.scratch);
            ++sc.snapshot_revision;
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
XrResult XRAPI_CALL layer_attach_actions(XrSession handle, const XrSessionActionSetsAttachInfo *info) {
    STATE_LOCK;
    auto it=sessions.find(handle);
    return it==sessions.end() ? XR_ERROR_HANDLE_INVALID : it->second->gaze.attach(info);
}
XrResult XRAPI_CALL layer_sync_actions(XrSession handle, const XrActionsSyncInfo *info) {
    STATE_LOCK;
    auto it=sessions.find(handle);
    if (it==sessions.end()) return XR_ERROR_HANDLE_INVALID;
    return it->second->gaze.sync(info,it->second->gaze_requested);
}
XrResult XRAPI_CALL layer_suggest_bindings(XrInstance handle, const XrInteractionProfileSuggestedBinding *info) {
    STATE_LOCK;
    auto it=instances.find(handle);
    return it==instances.end() ? XR_ERROR_HANDLE_INVALID : it->second->gaze.suggest(info);
}
XrResult XRAPI_CALL layer_gipa(XrInstance handle, const char *name, PFN_xrVoidFunction *out) {
    if (!name || !out)
        return XR_ERROR_VALIDATION_FAILURE;
    STATE_LOCK;
    // Never advertise optional extension functions that the next runtime does not expose.
    if (!std::strcmp(name, "xrLocateViews") || !std::strcmp(name, "xrEnumerateViewConfigurationViews") ||
        !std::strcmp(name, "xrGetVisibilityMaskKHR") || !std::strcmp(name,"xrAttachSessionActionSets") ||
        !std::strcmp(name,"xrSyncActions") || !std::strcmp(name,"xrSuggestInteractionProfileBindings")) {
        auto it = instances.find(handle);
        auto next = it == instances.end() ? global_next : it->second->gipa;
        *out = nullptr;
        const auto result = next ? next(handle, name, out) : XR_ERROR_FUNCTION_UNSUPPORTED;
        if (XR_SUCCEEDED(result) && *out && it != instances.end()) {
            if (!std::strcmp(name, "xrLocateViews"))
                *out = reinterpret_cast<PFN_xrVoidFunction>(layer_locate_views);
            else if (!std::strcmp(name, "xrEnumerateViewConfigurationViews"))
                *out = reinterpret_cast<PFN_xrVoidFunction>(layer_view_sizes);
            else if (!std::strcmp(name,"xrAttachSessionActionSets"))
                *out = reinterpret_cast<PFN_xrVoidFunction>(layer_attach_actions);
            else if (!std::strcmp(name,"xrSyncActions"))
                *out = reinterpret_cast<PFN_xrVoidFunction>(layer_sync_actions);
            else if (!std::strcmp(name,"xrSuggestInteractionProfileBindings"))
                *out = reinterpret_cast<PFN_xrVoidFunction>(layer_suggest_bindings);
            else
                *out = reinterpret_cast<PFN_xrVoidFunction>(layer_visibility_mask);
        }
        return result;
    }
#define HOOK(n, f)                                                                                           \
    if (std::strcmp(name, n) == 0) {                                                                         \
        *out = reinterpret_cast<PFN_xrVoidFunction>(f);                                                      \
        return XR_SUCCESS;                                                                                   \
    }
    HOOK("xrGetInstanceProcAddr", layer_gipa)
    HOOK("xrDestroyInstance", layer_destroy_instance)
    HOOK("xrCreateSession", layer_create_session)
    HOOK("xrDestroySession", layer_destroy_session)
    HOOK("xrCreateSwapchain", layer_create_swapchain)
    HOOK("xrDestroySwapchain", layer_destroy_swapchain)
    HOOK("xrAcquireSwapchainImage", layer_acquire)
    HOOK("xrWaitSwapchainImage", layer_wait_image)
    HOOK("xrReleaseSwapchainImage", layer_release)
    HOOK("xrWaitFrame", layer_wait_frame)
    HOOK("xrEndFrame", layer_end_frame)
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
    bool gaze_enabled=false, added=false;
    XrInstanceCreateInfo requested=*info;
    std::vector<const char*> extensions;
    try {
        for (uint32_t i=0; info->enabledExtensionNames && i<info->enabledExtensionCount; ++i)
            if (!std::strcmp(info->enabledExtensionNames[i],XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) gaze_enabled=true;
        if (!gaze_enabled && !build_expired() && gaze_extension_available(next->nextGetInstanceProcAddr)) {
            if (info->enabledExtensionCount && info->enabledExtensionNames)
                extensions.assign(info->enabledExtensionNames,info->enabledExtensionNames+info->enabledExtensionCount);
            extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
            requested.enabledExtensionCount=uint32_t(extensions.size()); requested.enabledExtensionNames=extensions.data();
            gaze_enabled=added=true;
        }
    } catch (...) { requested=*info; gaze_enabled=added=false; }
    auto result = next->nextCreateApiLayerInstance(added ? &requested : info, &forwarded, out);
    if (XR_FAILED(result) && added) {
        gaze_enabled=false;
        result=next->nextCreateApiLayerInstance(info,&forwarded,out);
    }
    if (XR_FAILED(result))
        return result;
    STATE_LOCK;
    try {
        auto instance = std::make_unique<Instance>();
        instance->handle = *out;
        instance->gipa = next->nextGetInstanceProcAddr;
        instance->gaze.initialize(*out,instance->gipa,gaze_enabled && !build_expired());
        std::wstring settings_path;
        const DWORD length = GetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_SETTINGS", nullptr, 0);
        if (length && length <= 32768) {
            settings_path.resize(length);
            const DWORD copied =
                GetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_SETTINGS", settings_path.data(), length);
            if (copied && copied < length)
                settings_path.resize(copied);
            else
                settings_path.clear();
        } else {
            HMODULE module{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(layer_name), &module)) {
                std::wstring path(32768, L'\0');
                const DWORD copied = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
                if (copied && copied < path.size()) {
                    path.resize(copied);
                    const auto separator = path.find_last_of(L"\\/");
                    if (separator != std::wstring::npos)
                        settings_path = capture_settings_path(path.substr(0, separator));
                }
            }
        }
        instance->render = read_render_settings(settings_path);
        // Preserve each active instance's FOV snapshot. Only newly created instances
        // disable expansion at expiry, so the running game's viewport never jumps.
        if (build_expired())
            instance->render.enabled = false;
#define LOAD(n) instance->gipa(*out, "xr" #n, reinterpret_cast<PFN_xrVoidFunction *>(&instance->n));
        LOAD(DestroyInstance)
        LOAD(CreateSession)
        LOAD(DestroySession)
        LOAD(LocateViews)
        LOAD(LocateSpace)
        LOAD(EnumerateViewConfigurationViews)
        LOAD(GetVisibilityMaskKHR)
        LOAD(CreateSwapchain)
        LOAD(DestroySwapchain)
        LOAD(EnumerateSwapchainImages)
        LOAD(AcquireSwapchainImage)
        LOAD(WaitSwapchainImage)
        LOAD(ReleaseSwapchainImage)
        LOAD(WaitFrame)
        LOAD(EndFrame)
        LOAD(PollEvent)
#undef LOAD
        MultiByteToWideChar(CP_UTF8, 0, info->applicationInfo.applicationName, -1, instance->application,
                            128);
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
