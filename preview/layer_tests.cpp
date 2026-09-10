#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "transport.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <limits>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
using namespace vrst::preview;
extern "C" XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *, const char *,
                                                                  XrNegotiateApiLayerRequest *);
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                        \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (0)
namespace {
ID3D11Device *device{};
ID3D11DeviceContext *context{};
ID3D11Texture2D *runtime_texture{};
ID3D11Device *reader_device{};
ID3D11DeviceContext *reader_context{};
XrInstance instance = reinterpret_cast<XrInstance>(1);
XrSession session = reinterpret_cast<XrSession>(2);
XrSwapchain swapchain = reinterpret_cast<XrSwapchain>(3);
const XrFrameEndInfo *submitted{};
const XrSwapchainImageReleaseInfo *release_arg{};
XrResult release_result = XR_SUCCESS, end_result = XR_SUCCESS;
bool check_runtime_progress{};
PFN_xrGetInstanceProcAddr layer_lookup{};
int release_calls{}, end_calls{};
XrSwapchainCreateInfo swap_info{};
bool reference_event{};
std::uint64_t gpu_copies{};
using CopyRegion = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                             ID3D11Resource *, UINT, const D3D11_BOX *);
using CopyWhole = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
using Resolve = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, ID3D11Resource *,
                                          UINT, DXGI_FORMAT);
CopyRegion original_region{};
CopyWhole original_whole{};
Resolve original_resolve{};
void STDMETHODCALLTYPE count_region(ID3D11DeviceContext *c, ID3D11Resource *d, UINT ds, UINT x, UINT y,
                                    UINT z, ID3D11Resource *s, UINT ss, const D3D11_BOX *b) {
    ++gpu_copies;
    original_region(c, d, ds, x, y, z, s, ss, b);
}
void STDMETHODCALLTYPE count_whole(ID3D11DeviceContext *c, ID3D11Resource *d, ID3D11Resource *s) {
    ++gpu_copies;
    original_whole(c, d, s);
}
void STDMETHODCALLTYPE count_resolve(ID3D11DeviceContext *c, ID3D11Resource *d, UINT ds, ID3D11Resource *s,
                                     UINT ss, DXGI_FORMAT f) {
    ++gpu_copies;
    original_resolve(c, d, ds, s, ss, f);
}
void *replace_slot(unsigned index, void *fn) {
    auto **table = *reinterpret_cast<void ***>(context);
    DWORD old{};
    CHECK(VirtualProtect(table + index, sizeof(void *), PAGE_READWRITE, &old));
    auto *previous = table[index];
    table[index] = fn;
    DWORD ignored{};
    CHECK(VirtualProtect(table + index, sizeof(void *), old, &ignored));
    return previous;
}
// Slots are the published ID3D11DeviceContext COM ABI, not private layer hooks.
void instrument() {
    original_region = reinterpret_cast<CopyRegion>(replace_slot(46, reinterpret_cast<void *>(count_region)));
    original_whole = reinterpret_cast<CopyWhole>(replace_slot(47, reinterpret_cast<void *>(count_whole)));
    original_resolve = reinterpret_cast<Resolve>(replace_slot(57, reinterpret_cast<void *>(count_resolve)));
}
void uninstrument() {
    replace_slot(46, reinterpret_cast<void *>(original_region));
    replace_slot(47, reinterpret_cast<void *>(original_whole));
    replace_slot(57, reinterpret_cast<void *>(original_resolve));
}
void paint(float r, float g, float b) {
    D3D11_TEXTURE2D_DESC d{};
    runtime_texture->GetDesc(&d);
    for (UINT slice = 0; slice < d.ArraySize; ++slice) {
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        vd.Format = d.Format;
        vd.ViewDimension = d.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY
                                                  : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        if (d.SampleDesc.Count > 1) {
            vd.Texture2DMSArray.FirstArraySlice = slice;
            vd.Texture2DMSArray.ArraySize = 1;
        } else {
            vd.Texture2DArray.FirstArraySlice = slice;
            vd.Texture2DArray.ArraySize = 1;
        }
        ID3D11RenderTargetView *v{};
        CHECK(SUCCEEDED(device->CreateRenderTargetView(runtime_texture, &vd, &v)));
        float color[]{r, g, b, 1};
        if (slice == 1) {
            color[0] = g;
            color[1] = r;
        }
        context->ClearRenderTargetView(v, color);
        v->Release();
    }
}
XrResult XRAPI_CALL fake_create(const XrInstanceCreateInfo *, const XrApiLayerCreateInfo *chain,
                                XrInstance *out) {
    CHECK(chain->nextInfo == nullptr);
    *out = instance;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_destroy_instance(XrInstance) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_create_session(XrInstance, const XrSessionCreateInfo *, XrSession *out) {
    *out = session;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_destroy_session(XrSession) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_create_swapchain(XrSession, const XrSwapchainCreateInfo *info, XrSwapchain *out) {
    swap_info = *info;
    *out = swapchain;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_destroy_swapchain(XrSwapchain) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_enumerate(XrSwapchain, uint32_t cap, uint32_t *count,
                                   XrSwapchainImageBaseHeader *images) {
    *count = 1;
    if (cap) {
        CHECK(images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR);
        reinterpret_cast<XrSwapchainImageD3D11KHR *>(images)->texture = runtime_texture;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_acquire(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *i) {
    *i = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_wait_image(XrSwapchain, const XrSwapchainImageWaitInfo *) {
    if (check_runtime_progress) {
        std::atomic<bool> finished{};
        std::thread other([&] {
            PFN_xrVoidFunction out{};
            CHECK(layer_lookup(instance, "xrGetInstanceProcAddr", &out) == XR_SUCCESS);
            finished = true;
        });
        auto until = GetTickCount64() + 250;
        while (!finished && GetTickCount64() < until)
            Sleep(1);
        CHECK(finished);
        other.join();
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_release(XrSwapchain, const XrSwapchainImageReleaseInfo *p) {
    ++release_calls;
    CHECK(p == release_arg);
    if (XR_SUCCEEDED(release_result))
        paint(0, 0, 1);
    return release_result;
}
XrResult XRAPI_CALL fake_wait_frame(XrSession, const XrFrameWaitInfo *, XrFrameState *state) {
    state->predictedDisplayTime = 123456;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_end(XrSession, const XrFrameEndInfo *p) {
    ++end_calls;
    CHECK(p == submitted);
    return end_result;
}
XrResult XRAPI_CALL fake_poll(XrInstance, XrEventDataBuffer *p) {
    if (!reference_event)
        return XR_EVENT_UNAVAILABLE;
    reference_event = false;
    auto *e = reinterpret_cast<XrEventDataReferenceSpaceChangePending *>(p);
    *e = {XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
    e->session = session;
    e->changeTime = 123456;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_gipa(XrInstance, const char *name, PFN_xrVoidFunction *out) {
#define ENTRY(n, f)                                                                                          \
    if (!std::strcmp(name, n)) {                                                                             \
        *out = reinterpret_cast<PFN_xrVoidFunction>(f);                                                      \
        return XR_SUCCESS;                                                                                   \
    }
    ENTRY("xrDestroyInstance", fake_destroy_instance)
    ENTRY("xrCreateSession", fake_create_session)
    ENTRY("xrDestroySession", fake_destroy_session)
    ENTRY("xrCreateSwapchain", fake_create_swapchain) ENTRY("xrDestroySwapchain", fake_destroy_swapchain)
        ENTRY("xrEnumerateSwapchainImages", fake_enumerate) ENTRY("xrAcquireSwapchainImage", fake_acquire)
            ENTRY("xrWaitSwapchainImage", fake_wait_image) ENTRY("xrReleaseSwapchainImage", fake_release)
                ENTRY("xrWaitFrame", fake_wait_frame) ENTRY("xrEndFrame", fake_end)
                    ENTRY("xrPollEvent", fake_poll) *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
template <class T> T fn(PFN_xrGetInstanceProcAddr g, const char *name) {
    PFN_xrVoidFunction f{};
    CHECK(g(instance, name, &f) == XR_SUCCESS);
    CHECK(f);
    return reinterpret_cast<T>(f);
}
void make_texture(UINT samples = 1) {
    if (runtime_texture)
        runtime_texture->Release();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 8;
    d.Height = 8;
    d.MipLevels = 1;
    d.ArraySize = 2;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CHECK(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &runtime_texture)));
}
void check_pixels(const View &view, bool green) {
    ID3D11Texture2D *shared{};
    CHECK(SUCCEEDED(reader_device->OpenSharedResource(reinterpret_cast<HANDLE>(view.texture_handle),
                                                      __uuidof(ID3D11Texture2D),
                                                      reinterpret_cast<void **>(&shared))));
    IDXGIKeyedMutex *key{};
    CHECK(SUCCEEDED(shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&key))));
    CHECK(key->AcquireSync(1, 1000) == S_OK);
    D3D11_TEXTURE2D_DESC d{};
    shared->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *read{};
    CHECK(SUCCEEDED(reader_device->CreateTexture2D(&d, nullptr, &read)));
    reader_context->CopyResource(read, shared);
    D3D11_MAPPED_SUBRESOURCE m{};
    CHECK(SUCCEEDED(reader_context->Map(read, 0, D3D11_MAP_READ, 0, &m)));
    auto *p = static_cast<unsigned char *>(m.pData);
    CHECK(p[0] == (green ? 0 : 255));
    CHECK(p[1] == (green ? 255 : 0));
    CHECK(p[2] == 0);
    reader_context->Unmap(read, 0);
    reader_context->Flush();
    CHECK(SUCCEEDED(key->ReleaseSync(0)));
    read->Release();
    key->Release();
    shared->Release();
}
} // namespace
int main(int argc, char **) {
    if (argc > 1) {
        Sleep(5000);
        return 0;
    }
    for (unsigned game_hz : {90u, 120u}) {
        RateLimiter limiter;
        unsigned delivered = 0;
        for (unsigned i = 0; i < game_hz / 10; ++i) {
            const std::uint64_t now = 1000000000ull + std::uint64_t(i) * 1000000000ull / game_hz;
            if (limiter.due(now, 60)) {
                ++delivered;
                limiter.published(now, 60);
            }
        }
        CHECK(delivered == 6);
        CHECK(limiter.due(9000000000ull, 60));
        limiter.published(9000000000ull, 60);
        CHECK(!limiter.due(9000000000ull, 60));
        limiter.reset();
        CHECK(limiter.due(9000000000ull, 60));
        limiter.published(9000000000ull, 60);
        CHECK(!limiter.due(9000000000ull, 60));
        CHECK(limiter.due(9000000000ull, 30));
        limiter.published(9000000000ull, 30);
        CHECK(!limiter.due(9010000000ull, 30));
    }
    Channel producer_only;
    auto isolated = L"Local\\HorizonLockProducerTest-" + std::to_wstring(GetCurrentProcessId());
    CHECK(producer_only.open(false, isolated));
    CHECK(producer_only.lock());
    CHECK(producer_only.data()->magic == ipc_magic);
    CHECK(producer_only.data()->paused == 1);
    producer_only.data()->published_frames = 42;
    producer_only.unlock();
    Channel second;
    CHECK(second.open(true, isolated));
    CHECK(second.lock());
    CHECK(second.data()->published_frames == 42);
    second.unlock();
    View v{};
    CHECK(!valid_view(v));
    v.texture_handle = 1;
    v.width = v.height = 8;
    v.rect_width = v.rect_height = 8;
    v.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK(valid_view(v));
    v.x = 1;
    CHECK(!valid_view(v));
    v.x = 0;
    v.orientation[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!valid_view(v));
    v.orientation[0] = 0;
    v.fov[0] = v.fov[1];
    CHECK(!valid_view(v));
    Shared s{};
    s.consumer_pid = GetCurrentProcessId();
    s.paused = 0;
    s.heartbeat_ms = 100;
    CHECK(viewer_active(s, 101));
    CHECK(!viewer_active(s, 5000));
    s.heartbeat_ms = 200;
    CHECK(!viewer_active(s, 100));
    std::wstring channel = L"Local\\HorizonLockCaptureTest-" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_CHANNEL", channel.c_str());
    Channel viewer;
    CHECK(viewer.open(true));
    CHECK(viewer.lock());
    viewer.data()->consumer_pid = GetCurrentProcessId();
    viewer.data()->paused = 0;
    viewer.data()->fps = 240;
    viewer.data()->heartbeat_ms = GetTickCount64();
    viewer.unlock();
    XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                 1,
                                 sizeof(loader),
                                 1,
                                 1,
                                 XR_MAKE_VERSION(1, 0, 0),
                                 XR_CURRENT_API_VERSION};
    XrNegotiateApiLayerRequest request{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, 1, sizeof(request)};
    CHECK(xrNegotiateLoaderApiLayerInterface(&loader, "XR_APILAYER_HORIZONLOCK_preview", &request) ==
          XR_SUCCESS);
    XrApiLayerNextInfo next{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, 1, sizeof(next)};
    std::strcpy(next.layerName, "XR_APILAYER_HORIZONLOCK_preview");
    next.nextGetInstanceProcAddr = fake_gipa;
    next.nextCreateApiLayerInstance = fake_create;
    XrApiLayerCreateInfo chain{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO, 1, sizeof(chain)};
    chain.nextInfo = &next;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "Capture regression test");
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    XrInstance actual{};
    CHECK(request.createApiLayerInstance(&ci, &chain, &actual) == XR_SUCCESS);
    CHECK(actual == instance);
    auto g = request.getInstanceProcAddr;
    layer_lookup = g;
    D3D_FEATURE_LEVEL level{};
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &device, &level, &context)));
    make_texture();
    instrument();
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &reader_device, &level, &reader_context)));
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &binding};
    CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.sampleCount = 1;
    sc.width = sc.height = 8;
    sc.faceCount = sc.mipCount = 1;
    sc.arraySize = 2;
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
    XrCompositionLayerProjectionView views[4]{};
    for (int i = 0; i < 4; ++i) {
        views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views[i].pose.orientation = {0, 0, 0, 1};
        views[i].fov = {-.5f, .6f, .7f, -.8f};
        views[i].subImage.swapchain = swapchain;
        views[i].subImage.imageRect = {{1, 2}, {4, 3}};
        views[i].subImage.imageArrayIndex = i % 2;
    }
    views[2].pose.orientation = {0, .1f, 0, .994987437f};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 4;
    projection.views = views;
    const XrCompositionLayerBaseHeader *layers[]{
        reinterpret_cast<XrCompositionLayerBaseHeader *>(&projection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = 123456;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = 1;
    end.layers = layers;
    submitted = &end;
    bool renew_heartbeat = true, mutex_busy = false;
    auto frame = [&](bool expected) {
        Sleep(20);
        if (!mutex_busy) {
            CHECK(viewer.lock());
            if (renew_heartbeat)
                viewer.data()->heartbeat_ms = GetTickCount64();
            viewer.unlock();
        }
        const auto saved_end = end;
        const auto saved_projection = projection;
        XrCompositionLayerProjectionView saved_views[4];
        std::memcpy(saved_views, views, sizeof(views));
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        CHECK(fn<PFN_xrWaitFrame>(g, "xrWaitFrame")(session, &wait, &state) == XR_SUCCESS);
        CHECK(state.predictedDisplayTime == 123456);
        paint(1, 0, 0);
        uint32_t index = 99;
        XrSwapchainImageAcquireInfo ac{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        CHECK(fn<PFN_xrAcquireSwapchainImage>(g, "xrAcquireSwapchainImage")(swapchain, &ac, &index) ==
              XR_SUCCESS);
        CHECK(index == 0);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        CHECK(fn<PFN_xrWaitSwapchainImage>(g, "xrWaitSwapchainImage")(swapchain, &wi) == XR_SUCCESS);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        release_arg = &ri;
        CHECK(fn<PFN_xrReleaseSwapchainImage>(g, "xrReleaseSwapchainImage")(swapchain, &ri) ==
              release_result);
        CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == end_result);
        CHECK(std::memcmp(&saved_end, &end, sizeof(end)) == 0);
        CHECK(std::memcmp(&saved_projection, &projection, sizeof(projection)) == 0);
        CHECK(std::memcmp(saved_views, views, sizeof(views)) == 0);
        if (mutex_busy)
            return;
        CHECK(viewer.lock());
        if (bool(viewer.data()->ready) != expected)
            std::fprintf(stderr, "frame %d expected %d ready %u status %u %ls\n", end_calls, expected,
                         viewer.data()->ready, viewer.data()->status_code, viewer.data()->status);
        CHECK(bool(viewer.data()->ready) == expected);
        viewer.unlock();
    };
    // A living producer in another process has exclusive channel ownership.
    wchar_t own_path[MAX_PATH]{};
    CHECK(GetModuleFileNameW(nullptr, own_path, MAX_PATH));
    std::wstring child_command = L"\"" + std::wstring(own_path) + L"\" --hold";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    CHECK(CreateProcessW(own_path, child_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &child));
    CHECK(viewer.lock());
    viewer.data()->producer_pid = child.dwProcessId;
    viewer.unlock();
    auto exclusive_copies = gpu_copies;
    frame(false);
    CHECK(gpu_copies == exclusive_copies);
    CHECK(viewer.lock());
    CHECK(viewer.data()->producer_pid == child.dwProcessId);
    viewer.data()->producer_pid = GetCurrentProcessId();
    viewer.unlock();
    CHECK(TerminateProcess(child.hProcess, 0));
    CHECK(WaitForSingleObject(child.hProcess, 1000) == WAIT_OBJECT_0);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    check_runtime_progress = true;
    frame(true);
    check_runtime_progress = false;
    CHECK(viewer.lock());
    Frame first = viewer.data()->frame;
    CHECK(valid_frame(first));
    CHECK(first.view_count == 2);
    CHECK(first.display_time == 123456);
    CHECK(first.views[0].x == 1 && first.views[0].y == 2);
    CHECK(first.views[0].rect_width == 4 && first.views[0].rect_height == 3);
    CHECK(std::abs(first.views[0].fov[0] - std::tan(-.5f)) < 1e-6f);
    CHECK(first.views[1].orientation[1] == .1f);
    viewer.unlock();
    auto before = gpu_copies;
    frame(true);
    CHECK(gpu_copies == before);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.sequence == first.sequence);
    check_pixels(first.views[0], false);
    check_pixels(first.views[1], false);
    viewer.data()->ready = 0;
    viewer.data()->paused = 1;
    viewer.unlock();
    before = gpu_copies;
    frame(false);
    CHECK(gpu_copies == before);
    CHECK(viewer.lock());
    viewer.data()->paused = 0;
    viewer.data()->eye = 1;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    check_pixels(viewer.data()->frame.views[0], true);
    check_pixels(viewer.data()->frame.views[1], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    release_result = XR_ERROR_RUNTIME_FAILURE;
    frame(false);
    release_result = XR_SUCCESS;
    views[1].subImage.imageRect.offset.x = -1;
    frame(false);
    views[1].subImage.imageRect.offset.x = 1;
    reference_event = true;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    CHECK(fn<PFN_xrPollEvent>(g, "xrPollEvent")(instance, &event) == XR_SUCCESS);
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.reference_epoch > first.reference_epoch);
    check_pixels(viewer.data()->frame.views[0], true);
    check_pixels(viewer.data()->frame.views[1], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    CHECK(fn<PFN_xrDestroySwapchain>(g, "xrDestroySwapchain")(swapchain) == XR_SUCCESS);
    make_texture(4);
    sc.sampleCount = 4;
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
    projection.viewCount = 2;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.view_count == 1);
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    // Stale heartbeat and absent consumer must suppress all D3D11 copies/resolves.
    CHECK(viewer.lock());
    viewer.data()->heartbeat_ms = GetTickCount64() - 5000;
    viewer.unlock();
    renew_heartbeat = false;
    before = gpu_copies;
    frame(false);
    CHECK(gpu_copies == before);
    renew_heartbeat = true;
    CHECK(viewer.lock());
    viewer.data()->consumer_pid = 0;
    viewer.unlock();
    before = gpu_copies;
    frame(false);
    CHECK(gpu_copies == before);
    CHECK(viewer.lock());
    viewer.data()->consumer_pid = GetCurrentProcessId();
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.generation != first.generation);
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.data()->fps = 1;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    before = gpu_copies;
    frame(false);
    CHECK(gpu_copies == before);
    CHECK(viewer.lock());
    viewer.data()->fps = 240;
    viewer.unlock();
    frame(true);
    // A viewer that discards a frame without opening its textures must not wedge key 1.
    CHECK(viewer.lock());
    viewer.data()->ready = 0;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    // GPU consumer failures request a new resource generation, not a stuck pending frame.
    frame(true);
    CHECK(viewer.lock());
    auto reset_generation = viewer.data()->frame.generation;
    viewer.data()->ready = 2;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.generation != reset_generation);
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    // Contention in the other process's header lock must drop immediately.
    std::atomic<bool> held{}, done{};
    std::thread busy([&] {
        Channel other;
        CHECK(other.open(true));
        CHECK(other.lock());
        held = true;
        auto until = GetTickCount64() + 1000;
        while (!done && GetTickCount64() < until)
            Sleep(1);
        other.unlock();
    });
    while (!held)
        Sleep(1);
    mutex_busy = true;
    before = gpu_copies;
    auto start = GetTickCount64();
    frame(false);
    CHECK(GetTickCount64() - start < 400);
    CHECK(gpu_copies == before);
    done = true;
    busy.join();
    mutex_busy = false;
    frame(true);
    CHECK(viewer.lock());
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    // A projection can reuse its latest released image in a later frame.
    Sleep(20);
    XrFrameWaitInfo reuse_wait{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState reuse_state{XR_TYPE_FRAME_STATE};
    CHECK(fn<PFN_xrWaitFrame>(g, "xrWaitFrame")(session, &reuse_wait, &reuse_state) == XR_SUCCESS);
    CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_SUCCESS);
    CHECK(viewer.lock());
    CHECK(viewer.data()->ready == 1);
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) ==
          XR_SUCCESS); // A recreated session uses a new identity and generation, even if runtime handles
                       // repeat.
    CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.session != first.session);
    CHECK(viewer.data()->frame.generation != first.generation);
    check_pixels(viewer.data()->frame.views[0], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
    si.next = &binding;
    CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
    sc.format = DXGI_FORMAT_D32_FLOAT;
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
    before = gpu_copies;
    frame(false);
    CHECK(gpu_copies == before);
    CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
    sc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    si.next = nullptr;
    CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
    before = gpu_copies;
    frame(false);
    CHECK(gpu_copies == before);
    CHECK(viewer.lock());
    CHECK(viewer.data()->status_code == 2);
    CHECK(viewer.data()->application[0] != 0);
    viewer.unlock();
    CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
    PFN_xrVoidFunction missing = reinterpret_cast<PFN_xrVoidFunction>(1);
    CHECK(g(instance, "xrNonexistentTestFunction", &missing) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(!missing);
    CHECK(fn<PFN_xrDestroyInstance>(g, "xrDestroyInstance")(instance) == XR_SUCCESS);
    CHECK(release_calls + 1 == end_calls);
    uninstrument();
    runtime_texture->Release();
    reader_context->Release();
    reader_device->Release();
    context->Release();
    device->Release();
    std::puts("PASS: transport validation; before-release pixels; quad/array/rect; unchanged calls/results; "
              "pause; backpressure; failed release; invalid bounds; reference epoch; MSAA stereo; producer "
              "exclusivity; zero-copy inactive/paused/backpressure/rate-limit; stale reconnect; "
              "discarded-key recovery; lock contention; reused submission; recreated/unsupported session");
    return 0;
}
