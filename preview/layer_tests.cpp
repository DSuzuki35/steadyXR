#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "build_expiry_test_clock.hpp"
#include "render_settings.hpp"
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
std::uint64_t gpu_copies{};
extern "C" void vrst_preview_test_copy_queued() {
    ++gpu_copies;
}
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
const XrCompositionLayerProjectionView *expected_submitted_views{};
uint32_t expected_submitted_count{};
const XrSwapchainImageReleaseInfo *release_arg{};
XrResult release_result = XR_SUCCESS, end_result = XR_SUCCESS;
bool check_runtime_progress{};
PFN_xrGetInstanceProcAddr layer_lookup{};
int release_calls{}, end_calls{};
int locate_space_calls{};
bool gaze_extension{}, reject_gaze_extension{};
bool saw_gaze_extension{};
const XrSessionActionSetsAttachInfo *attach_arg{};
const XrActionsSyncInfo *sync_arg{};
const XrInteractionProfileSuggestedBinding *suggest_arg{};
XrResult XRAPI_CALL fake_extensions(const char*, uint32_t capacity, uint32_t *count, XrExtensionProperties *out) {
    *count=gaze_extension?1:0;
    if(capacity && gaze_extension) std::strcpy(out[0].extensionName,XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_attach(XrSession,const XrSessionActionSetsAttachInfo *info) {
    CHECK(info==attach_arg); return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
}
XrResult XRAPI_CALL fake_sync(XrSession,const XrActionsSyncInfo *info) {
    CHECK(info==sync_arg); return XR_SESSION_NOT_FOCUSED;
}
XrResult XRAPI_CALL fake_suggest(XrInstance,const XrInteractionProfileSuggestedBinding *info) {
    CHECK(info==suggest_arg); return XR_ERROR_PATH_UNSUPPORTED;
}
bool valid_space = true;
XrResult XRAPI_CALL fake_locate_space(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *out) {
    CHECK(space == reinterpret_cast<XrSpace>(91) && base == XR_NULL_HANDLE && time == 123456);
    ++locate_space_calls;
    out->locationFlags =
        valid_space ? XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT : 0;
    out->pose = {{0, 0, 0, 1}, {2, 0, 0}};
    return XR_SUCCESS;
}
XrSwapchainCreateInfo swap_info{};
bool reference_event{};
XrResult view_result = XR_SUCCESS, mask_result = XR_SUCCESS;
bool asymmetric_runtime{}, mask_available = true;
uint32_t recommended_width = 2000, maximum_width = 12000;
const XrViewLocateInfo *locate_arg{};
XrViewState *state_arg{};
constexpr auto quad_views = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
XrView runtime_view(unsigned i) {
    XrView v{XR_TYPE_VIEW};
    v.pose.orientation =
        asymmetric_runtime ? XrQuaternionf{0, .1f, 0, .994987437f} : XrQuaternionf{0, 0, 0, 1};
    v.pose.position = {float(i) * .032f, 1.7f, -.3f};
    v.fov = asymmetric_runtime ? XrFovf{std::atan(-.6f), std::atan(1.4f), .8f, -.7f}
                               : XrFovf{std::atan(-1.f), std::atan(1.f), .8f, -.7f};
    return v;
}
XrResult XRAPI_CALL fake_locate(XrSession, const XrViewLocateInfo *info, XrViewState *state,
                                uint32_t capacity, uint32_t *count, XrView *views) {
    CHECK(info == locate_arg && state == state_arg);
    if (XR_FAILED(view_result))
        return view_result;
    *count = info->viewConfigurationType == quad_views ? 4 : 2;
    if (!capacity)
        return XR_SUCCESS;
    if (capacity < *count)
        return XR_ERROR_SIZE_INSUFFICIENT;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    for (unsigned i = 0; i < *count; ++i) {
        const void *next = views[i].next;
        views[i] = runtime_view(i);
        views[i].next = const_cast<void *>(next);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_view_sizes(XrInstance, XrSystemId system, XrViewConfigurationType type,
                                    uint32_t capacity, uint32_t *count, XrViewConfigurationView *views) {
    CHECK(system == 77);
    if (XR_FAILED(view_result))
        return view_result;
    *count = type == quad_views ? 4 : 2;
    if (!capacity)
        return XR_SUCCESS;
    if (capacity < *count)
        return XR_ERROR_SIZE_INSUFFICIENT;
    for (unsigned i = 0; i < *count; ++i) {
        views[i].recommendedImageRectWidth = recommended_width;
        views[i].maxImageRectWidth = maximum_width;
        views[i].recommendedImageRectHeight = 2100;
        views[i].maxImageRectHeight = 12000;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_mask(XrSession, XrViewConfigurationType type, uint32_t index,
                              XrVisibilityMaskTypeKHR kind, XrVisibilityMaskKHR *mask) {
    if (XR_FAILED(mask_result))
        return mask_result;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && type != quad_views)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (index >= (type == quad_views ? 4u : 2u))
        return XR_ERROR_VALIDATION_FAILURE;
    if (kind < XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR ||
        kind > XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR || !mask || mask->type != XR_TYPE_VISIBILITY_MASK_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
    mask->vertexCountOutput = mask->indexCountOutput = 3;
    // Per KHR_visibility_mask either zero capacity makes BOTH buffers inactive.
    if (!mask->vertexCapacityInput || !mask->indexCapacityInput)
        return XR_SUCCESS;
    if (!mask->vertices || !mask->indices)
        return XR_ERROR_VALIDATION_FAILURE;
    if (mask->vertexCapacityInput < 3 || mask->indexCapacityInput < 3)
        return XR_ERROR_SIZE_INSUFFICIENT;
    for (unsigned i = 0; i < 3; ++i) {
        mask->vertices[i] = {float(i), 1};
        mask->indices[i] = i;
    }
    return XR_SUCCESS;
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
XrResult XRAPI_CALL fake_create(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *chain,
                                XrInstance *out) {
    CHECK(chain->nextInfo == nullptr);
    saw_gaze_extension=false;
    for(unsigned i=0;i<info->enabledExtensionCount;++i)
        saw_gaze_extension |= !std::strcmp(info->enabledExtensionNames[i],XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    if(saw_gaze_extension && reject_gaze_extension) return XR_ERROR_EXTENSION_NOT_PRESENT;
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
    if (expected_submitted_views) {
        CHECK(p->layerCount == 1);
        const auto *projection = reinterpret_cast<const XrCompositionLayerProjection *>(p->layers[0]);
        CHECK(projection->viewCount == expected_submitted_count);
        CHECK(!std::memcmp(projection->views, expected_submitted_views,
                           sizeof(*expected_submitted_views) * expected_submitted_count));
    }
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
    if (!std::strcmp(name, "xrGetVisibilityMaskKHR") && !mask_available) {
        *out = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
#define ENTRY(n, f)                                                                                          \
    if (!std::strcmp(name, n)) {                                                                             \
        *out = reinterpret_cast<PFN_xrVoidFunction>(f);                                                      \
        return XR_SUCCESS;                                                                                   \
    }
    ENTRY("xrDestroyInstance", fake_destroy_instance)
    ENTRY("xrEnumerateInstanceExtensionProperties", fake_extensions)
    ENTRY("xrAttachSessionActionSets", fake_attach)
    ENTRY("xrSyncActions", fake_sync)
    ENTRY("xrSuggestInteractionProfileBindings", fake_suggest)
    ENTRY("xrLocateViews", fake_locate)
    ENTRY("xrLocateSpace", fake_locate_space)
    ENTRY("xrEnumerateViewConfigurationViews", fake_view_sizes)
    ENTRY("xrGetVisibilityMaskKHR", fake_mask)
    ENTRY("xrCreateSession", fake_create_session)
    ENTRY("xrDestroySession", fake_destroy_session)
    ENTRY("xrCreateSwapchain", fake_create_swapchain)
    ENTRY("xrDestroySwapchain", fake_destroy_swapchain)
    ENTRY("xrEnumerateSwapchainImages", fake_enumerate)
    ENTRY("xrAcquireSwapchainImage", fake_acquire)
    ENTRY("xrWaitSwapchainImage", fake_wait_image)
    ENTRY("xrReleaseSwapchainImage", fake_release)
    ENTRY("xrWaitFrame", fake_wait_frame)
    ENTRY("xrEndFrame", fake_end) ENTRY("xrPollEvent", fake_poll) *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
template <class T> T fn(PFN_xrGetInstanceProcAddr g, const char *name) {
    PFN_xrVoidFunction f{};
    CHECK(g(instance, name, &f) == XR_SUCCESS);
    CHECK(f);
    return reinterpret_cast<T>(f);
}
void check_gaze_hooks(const XrNegotiateApiLayerRequest &request, const XrInstanceCreateInfo &ci,
                      const XrApiLayerCreateInfo &chain) {
    for(bool reject : {false,true}) {
        gaze_extension=true; reject_gaze_extension=reject;
        XrInstance actual{};
        CHECK(request.createApiLayerInstance(&ci,&chain,&actual)==XR_SUCCESS);
        CHECK(saw_gaze_extension==!reject);
        const auto g=request.getInstanceProcAddr;
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        CHECK(fn<PFN_xrCreateSession>(g,"xrCreateSession")(actual,&info,&session)==XR_SUCCESS);
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; attach_arg=&attach;
        CHECK(fn<PFN_xrAttachSessionActionSets>(g,"xrAttachSessionActionSets")(session,&attach)==XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync_arg=&sync;
        CHECK(fn<PFN_xrSyncActions>(g,"xrSyncActions")(session,&sync)==XR_SESSION_NOT_FOCUSED);
        XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING}; suggest_arg=&suggest;
        CHECK(fn<PFN_xrSuggestInteractionProfileBindings>(g,"xrSuggestInteractionProfileBindings")(actual,&suggest)==XR_ERROR_PATH_UNSUPPORTED);
        CHECK(fn<PFN_xrDestroySession>(g,"xrDestroySession")(session)==XR_SUCCESS);
        CHECK(fn<PFN_xrDestroyInstance>(g,"xrDestroyInstance")(actual)==XR_SUCCESS);
    }
    gaze_extension=reject_gaze_extension=false;
}
void check_render_hooks(const XrNegotiateApiLayerRequest &request, const XrInstanceCreateInfo &ci,
                        const XrApiLayerCreateInfo &chain, const std::wstring &ini) {
    auto set = [&](const wchar_t *key, const wchar_t *value) {
        CHECK(WritePrivateProfileStringW(L"Render", key, value, ini.c_str()));
    };
    set(L"Enabled", L"1");
    set(L"LeftEyeLeftTenths", L"0");
    set(L"LeftEyeRightTenths", L"100");
    set(L"RightEyeLeftTenths", L"100");
    set(L"RightEyeRightTenths", L"0");
    for (bool canted : {false, true}) {
        asymmetric_runtime = canted;
        XrInstance actual{};
        CHECK(request.createApiLayerInstance(&ci, &chain, &actual) == XR_SUCCESS);
        const auto g = request.getInstanceProcAddr;
        XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
        CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
        auto locate = fn<PFN_xrLocateViews>(g, "xrLocateViews");
        auto sizes = fn<PFN_xrEnumerateViewConfigurationViews>(g, "xrEnumerateViewConfigurationViews");
        auto mask = fn<PFN_xrGetVisibilityMaskKHR>(g, "xrGetVisibilityMaskKHR");
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = canted ? quad_views : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = 123456;
        info.space = reinterpret_cast<XrSpace>(9);
        XrViewState state{XR_TYPE_VIEW_STATE};
        locate_arg = &info;
        state_arg = &state;
        XrView views[5]{};
        for (auto &v : views)
            v = runtime_view(9);
        const XrView sentinel = views[4];
        uint32_t count{};
        CHECK(locate(session, &info, &state, 5, &count, views) == XR_SUCCESS);
        CHECK(count == (canted ? 4u : 2u));
        const float base_left = canted ? -.6f : -1.f, base_right = canted ? 1.4f : 1.f;
        CHECK(std::abs(std::tan(views[0].fov.angleLeft) - base_left) < 1e-6f);
        CHECK(std::abs(std::tan(views[0].fov.angleRight) - (canted ? 1.6f : 1.2f)) < 1e-6f);
        CHECK(std::abs(std::tan(views[1].fov.angleLeft) - (canted ? -.8f : -1.2f)) < 1e-6f);
        CHECK(std::abs(std::tan(views[1].fov.angleRight) - base_right) < 1e-6f);
        for (unsigned i = 0; i < count; ++i) {
            auto original = runtime_view(i);
            CHECK(!std::memcmp(&views[i].pose, &original.pose, sizeof(original.pose)));
            CHECK(views[i].fov.angleUp == original.fov.angleUp);
            CHECK(views[i].fov.angleDown == original.fov.angleDown);
            if (i >= 2)
                CHECK(!std::memcmp(&views[i], &original, sizeof(original)));
        }
        CHECK(!std::memcmp(&views[4], &sentinel, sizeof(sentinel)));
        XrView saved[5];
        std::memcpy(saved, views, sizeof(views));
        CHECK(locate(session, &info, &state, 0, &count, views) == XR_SUCCESS);
        CHECK(!std::memcmp(saved, views, sizeof(views)));
        CHECK(locate(session, &info, &state, 0, &count, nullptr) == XR_SUCCESS);
        CHECK(locate(session, &info, &state, 1, &count, views) == XR_ERROR_SIZE_INSUFFICIENT);
        CHECK(!std::memcmp(saved, views, sizeof(views)));
        view_result = XR_ERROR_RUNTIME_FAILURE;
        CHECK(locate(session, &info, &state, 5, &count, views) == view_result);
        CHECK(!std::memcmp(saved, views, sizeof(views)));
        view_result = XR_SUCCESS;
        XrViewConfigurationView recommendations[5]{};
        for (auto &v : recommendations)
            v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 2200);
        CHECK(recommendations[1].recommendedImageRectWidth == 2200);
        for (unsigned i = 0; i < count; ++i) {
            CHECK(recommendations[i].maxImageRectWidth == maximum_width);
            CHECK(recommendations[i].recommendedImageRectHeight == 2100);
            CHECK(recommendations[i].maxImageRectHeight == 12000);
            CHECK(recommendations[i].maxSwapchainSampleCount == 4);
            if (i >= 2)
                CHECK(recommendations[i].recommendedImageRectWidth == 2000);
        }
        XrViewConfigurationView saved_sizes[5];
        std::memcpy(saved_sizes, recommendations, sizeof(recommendations));
        CHECK(sizes(instance, 77, info.viewConfigurationType, 0, &count, recommendations) == XR_SUCCESS);
        CHECK(!std::memcmp(saved_sizes, recommendations, sizeof(recommendations)));
        CHECK(sizes(instance, 77, info.viewConfigurationType, 0, &count, nullptr) == XR_SUCCESS);
        CHECK(sizes(instance, 77, info.viewConfigurationType, 1, &count, recommendations) ==
              XR_ERROR_SIZE_INSUFFICIENT);
        CHECK(!std::memcmp(saved_sizes, recommendations, sizeof(recommendations)));
        view_result = XR_ERROR_RUNTIME_FAILURE;
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == view_result);
        CHECK(!std::memcmp(saved_sizes, recommendations, sizeof(recommendations)));
        view_result = XR_SUCCESS;
        maximum_width = 2150;
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 2150);
        recommended_width = 8000;
        maximum_width = 16384;
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 8192);
        // View enumeration cannot reveal whether a game uses side-by-side, array, or separate textures.
        // A conservative allowance for additional pixels must not shrink the native recommendation.
        recommended_width = 10000;
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 10000);
        CHECK(recommendations[1].recommendedImageRectWidth == 10000);
        maximum_width = 10000;
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 10000);
        recommended_width = 2000;
        maximum_width = 12000;
        for (auto kind :
             {XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR,
              XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR}) {
            XrVector2f vertices[3]{{99, 98}, {97, 96}, {95, 94}};
            uint32_t indices[3]{99, 98, 97};
            XrVisibilityMaskKHR m{XR_TYPE_VISIBILITY_MASK_KHR};
            m.vertices = vertices;
            m.indices = indices;
            CHECK(mask(session, info.viewConfigurationType, 0, kind, &m) == XR_SUCCESS);
            CHECK(m.vertexCountOutput == 0 && m.indexCountOutput == 0);
            m.vertexCapacityInput = 3;
            CHECK(mask(session, info.viewConfigurationType, 0, kind, &m) == XR_SUCCESS);
            CHECK(m.vertexCountOutput == 0 && m.indexCountOutput == 0);
            CHECK(vertices[0].x == 99 && indices[0] == 99);
            m.vertexCapacityInput = 0;
            m.indexCapacityInput = 3;
            CHECK(mask(session, info.viewConfigurationType, 1, kind, &m) == XR_SUCCESS);
            CHECK(m.vertexCountOutput == 0 && m.indexCountOutput == 0);
            CHECK(vertices[0].x == 99 && indices[0] == 99);
            m.vertexCapacityInput = 3;
            CHECK(mask(session, info.viewConfigurationType, 0, kind, &m) == XR_SUCCESS);
            CHECK(m.vertexCountOutput == 0 && m.indexCountOutput == 0);
            if (canted) {
                CHECK(mask(session, quad_views, 2, kind, &m) == XR_SUCCESS);
                CHECK(m.vertexCountOutput == 3 && m.indexCountOutput == 3);
                CHECK(vertices[0].x == 0 && indices[0] == 0);
            }
            m.vertexCapacityInput = 1;
            CHECK(mask(session, info.viewConfigurationType, 0, kind, &m) == XR_ERROR_SIZE_INSUFFICIENT);
            CHECK(m.vertexCountOutput == 3 && m.indexCountOutput == 3);
            mask_result = XR_ERROR_RUNTIME_FAILURE;
            m.vertexCountOutput = 77;
            m.indexCountOutput = 88;
            CHECK(mask(session, info.viewConfigurationType, 0, kind, &m) == mask_result);
            CHECK(m.vertexCountOutput == 77 && m.indexCountOutput == 88);
            mask_result = XR_SUCCESS;
            CHECK(mask(session, info.viewConfigurationType, 9, kind, &m) == XR_ERROR_VALIDATION_FAILURE);
            CHECK(mask(session, info.viewConfigurationType, 0, static_cast<XrVisibilityMaskTypeKHR>(99),
                       &m) == XR_ERROR_VALIDATION_FAILURE);
            CHECK(mask(session, info.viewConfigurationType, 0, kind, nullptr) == XR_ERROR_VALIDATION_FAILURE);
            CHECK(mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO, 0, kind, &m) ==
                  XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
        }
        // Settings are an instance snapshot: editing the file cannot change a running game's FOV.
        set(L"Enabled", L"0");
        test_clock::unix_seconds = UINT64_MAX;
        CHECK(locate(session, &info, &state, 5, &count, views) == XR_SUCCESS);
        CHECK(std::abs(std::tan(views[0].fov.angleRight) - (canted ? 1.6f : 1.2f)) < 1e-6f);
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 2200);
        CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
        CHECK(fn<PFN_xrDestroyInstance>(g, "xrDestroyInstance")(instance) == XR_SUCCESS);
        test_clock::unix_seconds = 1;
        CHECK(request.createApiLayerInstance(&ci, &chain, &actual) == XR_SUCCESS);
        CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
        CHECK(locate(session, &info, &state, 5, &count, views) == XR_SUCCESS);
        for (unsigned i = 0; i < count; ++i) {
            auto original = runtime_view(i);
            CHECK(!std::memcmp(&views[i], &original, sizeof(original)));
        }
        CHECK(sizes(instance, 77, info.viewConfigurationType, 5, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 2000);
        XrVisibilityMaskKHR m{XR_TYPE_VISIBILITY_MASK_KHR};
        CHECK(mask(session, info.viewConfigurationType, 0, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &m) ==
              XR_SUCCESS);
        CHECK(m.vertexCountOutput == 3 && m.indexCountOutput == 3);
        CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
        CHECK(fn<PFN_xrDestroyInstance>(g, "xrDestroyInstance")(instance) == XR_SUCCESS);
        set(L"Enabled", L"1");
    }
    // Enabling only one eye must leave the other eye's view, recommendation, and mask identical.
    set(L"LeftEyeRightTenths", L"0");
    set(L"RightEyeLeftTenths", L"0");
    set(L"RightEyeRightTenths", L"900"); // Bound a hand-edited config to 30%.
    XrInstance single{};
    CHECK(request.createApiLayerInstance(&ci, &chain, &single) == XR_SUCCESS);
    const auto g = request.getInstanceProcAddr;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = quad_views;
    XrViewState state{XR_TYPE_VIEW_STATE};
    locate_arg = &info;
    state_arg = &state;
    XrView views[4]{};
    uint32_t count{};
    CHECK(fn<PFN_xrLocateViews>(g, "xrLocateViews")(session, &info, &state, 4, &count, views) == XR_SUCCESS);
    const auto original = runtime_view(0);
    CHECK(!std::memcmp(&original, &views[0], sizeof(original)));
    CHECK(std::abs(std::tan(views[1].fov.angleRight) - 2.0f) < 1e-6f);
    XrViewConfigurationView recommendations[4]{};
    CHECK(fn<PFN_xrEnumerateViewConfigurationViews>(g, "xrEnumerateViewConfigurationViews")(
              instance, 77, quad_views, 4, &count, recommendations) == XR_SUCCESS);
    CHECK(recommendations[0].recommendedImageRectWidth == 2000);
    CHECK(recommendations[1].recommendedImageRectWidth == 2600);
    XrVisibilityMaskKHR unchanged_mask{XR_TYPE_VISIBILITY_MASK_KHR};
    CHECK(fn<PFN_xrGetVisibilityMaskKHR>(g, "xrGetVisibilityMaskKHR")(
              session, quad_views, 0, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &unchanged_mask) ==
          XR_SUCCESS);
    CHECK(unchanged_mask.vertexCountOutput == 3 && unchanged_mask.indexCountOutput == 3);
    // Runtime must receive the expanded projection actually rendered by the game, without any rewrite.
    XrCompositionLayerProjectionView submitted_views[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        submitted_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        submitted_views[i].pose = views[i].pose;
        submitted_views[i].fov = views[i].fov;
    }
    XrCompositionLayerProjectionView expected[4];
    std::memcpy(expected, submitted_views, sizeof(expected));
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 4;
    projection.views = submitted_views;
    const auto *projection_header = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = 123456;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = 1;
    end.layers = &projection_header;
    const auto previous_calls = end_calls;
    submitted = &end;
    expected_submitted_views = expected;
    expected_submitted_count = 4;
    CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_SUCCESS);
    CHECK(!std::memcmp(submitted_views, expected, sizeof(expected)));
    end_calls = previous_calls;
    submitted = nullptr;
    expected_submitted_views = nullptr;
    expected_submitted_count = 0;
    CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
    CHECK(fn<PFN_xrDestroyInstance>(g, "xrDestroyInstance")(instance) == XR_SUCCESS);
    set(L"LeftEyeRightTenths", L"100");
    set(L"RightEyeLeftTenths", L"100");
    set(L"RightEyeRightTenths", L"0");
    mask_available = false;
    XrInstance actual{};
    CHECK(request.createApiLayerInstance(&ci, &chain, &actual) == XR_SUCCESS);
    PFN_xrVoidFunction missing = reinterpret_cast<PFN_xrVoidFunction>(1);
    CHECK(request.getInstanceProcAddr(instance, "xrGetVisibilityMaskKHR", &missing) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(!missing);
    CHECK(fn<PFN_xrDestroyInstance>(request.getInstanceProcAddr, "xrDestroyInstance")(instance) ==
          XR_SUCCESS);
    mask_available = true;
    asymmetric_runtime = false;
}
void check_expired_render(const XrNegotiateApiLayerRequest &request, const XrInstanceCreateInfo &ci,
                          const XrApiLayerCreateInfo &chain, const std::wstring &ini) {
    CHECK(GetPrivateProfileIntW(L"Render", L"Enabled", 0, ini.c_str()) == 1);
    test_clock::unix_seconds = UINT64_MAX;
    const auto g = request.getInstanceProcAddr;
    for (bool canted : {false, true}) {
        asymmetric_runtime = canted;
        XrInstance actual{};
        CHECK(request.createApiLayerInstance(&ci, &chain, &actual) == XR_SUCCESS);
        CHECK(actual == instance);
        XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
        CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = canted ? quad_views : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XrViewState state{XR_TYPE_VIEW_STATE};
        locate_arg = &info;
        state_arg = &state;
        XrView views[4]{};
        uint32_t count{};
        auto locate = fn<PFN_xrLocateViews>(g, "xrLocateViews");
        CHECK(locate(session, &info, &state, 4, &count, views) == XR_SUCCESS);
        CHECK(count == (canted ? 4u : 2u));
        for (uint32_t i = 0; i < count; ++i) {
            const auto expected = runtime_view(i);
            CHECK(views[i].type == expected.type && views[i].next == expected.next);
            CHECK(!std::memcmp(&views[i].pose, &expected.pose, sizeof(expected.pose)));
            CHECK(!std::memcmp(&views[i].fov, &expected.fov, sizeof(expected.fov)));
        }
        view_result = XR_ERROR_RUNTIME_FAILURE;
        CHECK(locate(session, &info, &state, 4, &count, views) == XR_ERROR_RUNTIME_FAILURE);
        view_result = XR_SUCCESS;
        XrViewConfigurationView recommendations[4]{};
        CHECK(fn<PFN_xrEnumerateViewConfigurationViews>(g, "xrEnumerateViewConfigurationViews")(
                  instance, 77, info.viewConfigurationType, 4, &count, recommendations) == XR_SUCCESS);
        CHECK(recommendations[0].recommendedImageRectWidth == 2000);
        CHECK(recommendations[1].recommendedImageRectWidth == 2000);
        XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
        CHECK(fn<PFN_xrGetVisibilityMaskKHR>(g, "xrGetVisibilityMaskKHR")(
                  session, info.viewConfigurationType, 0, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &mask) ==
              XR_SUCCESS);
        CHECK(mask.vertexCountOutput == 3 && mask.indexCountOutput == 3);
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        CHECK(fn<PFN_xrWaitFrame>(g, "xrWaitFrame")(session, &wait, &frame_state) == XR_SUCCESS);
        CHECK(frame_state.predictedDisplayTime == 123456);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        submitted = &end;
        const auto previous_calls = end_calls;
        end_result = XR_ERROR_RUNTIME_FAILURE;
        CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_ERROR_RUNTIME_FAILURE);
        CHECK(end_calls == previous_calls + 1);
        end_calls = previous_calls;
        end_result = XR_SUCCESS;
        submitted = nullptr;
        CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
        CHECK(fn<PFN_xrDestroyInstance>(g, "xrDestroyInstance")(instance) == XR_SUCCESS);
    }
    asymmetric_runtime = false;
    test_clock::unix_seconds = 1;
}
void check_render_settings(const std::wstring &ini) {
    const auto defaults = read_render_settings(ini);
    CHECK(!defaults.enabled && defaults.eyes[0].left == 0 && defaults.eyes[1].right == 0);
    CHECK(WritePrivateProfileStringW(L"Preview", L"Eye", L"1", ini.c_str()));
    RenderSettings settings{true, {{2.34, 12.36}, {-10, 100}}};
    CHECK(write_render_settings(ini, settings));
    auto read = read_render_settings(ini);
    CHECK(read.enabled);
    CHECK(read.eyes[0].left == 2.3 && read.eyes[0].right == 12.4);
    CHECK(read.eyes[1].left == 0 && read.eyes[1].right == 30);
    CHECK(GetPrivateProfileIntW(L"Preview", L"Eye", 0, ini.c_str()) == 1);
    settings.eyes[0] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
    CHECK(write_render_settings(ini, settings));
    read = read_render_settings(ini);
    CHECK(read.eyes[0].left == 0 && read.eyes[0].right == 0);
    CHECK(WritePrivateProfileStringW(L"Render", L"LeftEyeLeftTenths", L"-100", ini.c_str()));
    CHECK(WritePrivateProfileStringW(L"Render", L"LeftEyeRightTenths", L"900", ini.c_str()));
    CHECK(WritePrivateProfileStringW(L"Render", L"RightEyeLeftTenths", L"NaN", ini.c_str()));
    CHECK(WritePrivateProfileStringW(L"Render", L"RightEyeRightTenths", L"100 invalid", ini.c_str()));
    read = read_render_settings(ini);
    CHECK(read.eyes[0].left == 0 && read.eyes[0].right == 30);
    CHECK(read.eyes[1].left == 0 && read.eyes[1].right == 0);
    CHECK(!write_render_settings(ini + L"\\missing\\preview.ini", settings));
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
    wchar_t temp_dir[MAX_PATH]{}, temp_file[MAX_PATH]{};
    CHECK(GetTempPathW(MAX_PATH, temp_dir));
    CHECK(GetTempFileNameW(temp_dir, L"hlr", 0, temp_file));
    CHECK(SetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_SETTINGS", temp_file));
    check_render_settings(temp_file);
    check_render_hooks(request, ci, chain, temp_file);
    check_expired_render(request, ci, chain, temp_file);
    check_gaze_hooks(request, ci, chain);
    XrInstance actual{};
    CHECK(request.createApiLayerInstance(&ci, &chain, &actual) == XR_SUCCESS);
    CHECK(actual == instance);
    auto g = request.getInstanceProcAddr;
    layer_lookup = g;
    D3D_FEATURE_LEVEL level{};
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &device, &level, &context)));
    make_texture();
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
    unsigned extra_reuses = 0;
    bool paint_green = false;
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
        paint(paint_green ? 0.f : 1.f, paint_green ? 1.f : 0.f, 0);
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
    CHECK(first.eye == 0);
    CHECK(viewer.data()->render_enabled == 1);
    CHECK(viewer.data()->render_left[0] == 0 && viewer.data()->render_right[0] == 10);
    CHECK(viewer.data()->render_left[1] == 10 && viewer.data()->render_right[1] == 0);
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
    CHECK(viewer.data()->frame.eye == 1);
    auto invalid_eye = viewer.data()->frame;
    invalid_eye.eye = 2;
    CHECK(!valid_frame(invalid_eye));
    check_pixels(viewer.data()->frame.views[1], true);
    viewer.data()->ready = 0;
    viewer.unlock();
    // Additional composition layers are discoverable while None is selected, and retain
    // slot identities across visibility changes. The same runtime image is safe to reuse.
    XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
    overlay.subImage = views[1].subImage;
    overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    overlay.pose = {{0, 0, 0, 1}, {0, 0, -2}};
    overlay.size = {1, .5f};
    overlay.layerFlags =
        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    cylinder.subImage = views[1].subImage;
    cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    cylinder.pose.orientation.w = 1;
    cylinder.radius = 2;
    cylinder.centralAngle = 1;
    cylinder.aspectRatio = 2;
    XrCompositionLayerProjection extra = projection;
    XrCompositionLayerBaseHeader unsupported{};
    unsupported.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR;
    const XrCompositionLayerBaseHeader *overlay_layers[]{
        layers[0], reinterpret_cast<XrCompositionLayerBaseHeader *>(&overlay),
        reinterpret_cast<XrCompositionLayerBaseHeader *>(&cylinder),
        reinterpret_cast<XrCompositionLayerBaseHeader *>(&extra), &unsupported};
    end.layers = overlay_layers;
    end.layerCount = 5;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->overlay_catalog.count == 4);
    CHECK(viewer.data()->overlay_catalog.layers[3].supported == 0);
    CHECK(viewer.data()->frame.overlay_count == 0);
    viewer.data()->ready = 0;
    viewer.data()->overlay_selection.mode = 1;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    auto with_overlays = viewer.data()->frame;
    CHECK(with_overlays.overlay_count == 3);
    CHECK(with_overlays.overlays[0].type == 1 && with_overlays.overlays[0].flags == 3);
    CHECK(with_overlays.overlays[1].type == 2 && with_overlays.overlays[1].radius == 2);
    CHECK(with_overlays.overlays[2].type == 3 &&
          with_overlays.overlays[2].image.fov[1] == with_overlays.views[0].fov[1]);
    CHECK(with_overlays.overlays[0].position[2] == -2);
    CHECK(locate_space_calls == 0);
    for (unsigned i = 0; i < with_overlays.overlay_count; ++i)
        check_pixels(with_overlays.overlays[i].image, true);
    const auto stable_generation = with_overlays.generation;
    viewer.data()->ready = 0;
    viewer.unlock();
    // A busy overlay must roll back already-acquired base keys without partial publication.
    ID3D11Texture2D *busy_overlay{};
    CHECK(SUCCEEDED(reader_device->OpenSharedResource(
        reinterpret_cast<HANDLE>(with_overlays.overlays[0].image.texture_handle), __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(&busy_overlay))));
    IDXGIKeyedMutex *busy_key{};
    CHECK(SUCCEEDED(
        busy_overlay->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&busy_key))));
    CHECK(busy_key->AcquireSync(0, 0) == S_OK);
    frame(false);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.sequence == with_overlays.sequence);
    viewer.unlock();
    CHECK(SUCCEEDED(busy_key->ReleaseSync(0)));
    busy_key->Release();
    busy_overlay->Release();
    overlay.space = reinterpret_cast<XrSpace>(91);
    overlay.eyeVisibility = XR_EYE_VISIBILITY_LEFT; // viewer is right eye
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 2);
    CHECK(viewer.data()->frame.overlays[0].slot == 1);
    CHECK(viewer.data()->frame.generation == stable_generation);
    viewer.data()->ready = 0;
    viewer.unlock();
    overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 3);
    CHECK(viewer.data()->frame.overlays[0].slot == 0);
    CHECK(viewer.data()->frame.overlays[0].position[0] == 2);
    CHECK(locate_space_calls > 0);
    viewer.data()->ready = 0;
    viewer.unlock();
    valid_space = false;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 2);
    CHECK(std::wcsstr(viewer.data()->overlay_catalog.layers[0].label, L"space pose unavailable"));
    viewer.data()->ready = 0;
    viewer.unlock();
    valid_space = true;
    // Eye inverse includes both translation and rotation, including a canted context eye.
    views[1].pose = {{0, 0, 1, 0}, {1, 0, 0}};
    frame(true);
    CHECK(viewer.lock());
    CHECK(std::abs(viewer.data()->frame.overlays[0].position[0] + 1) < 1e-6f);
    CHECK(std::abs(viewer.data()->frame.overlays[0].orientation[2] + 1) < 1e-6f);
    viewer.data()->ready = 0;
    viewer.unlock();
    views[1].pose = {{0, 0, 0, 1}, {0, 0, 0}};
    CHECK(viewer.lock());
    viewer.data()->overlay_selection = {2, 2, GetCurrentProcessId(), with_overlays.session};
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 1 && viewer.data()->frame.overlays[0].slot == 1);
    viewer.data()->ready = 0;
    viewer.data()->overlay_selection = {};
    viewer.unlock();
    // Atlas rectangles/slices remain distinct when an earlier quad disappears.
    XrCompositionLayerQuad atlas_second = overlay;
    atlas_second.subImage.imageRect = {{0, 0}, {1, 1}};
    const XrCompositionLayerBaseHeader *atlas_layers[]{
        layers[0], overlay_layers[1], reinterpret_cast<XrCompositionLayerBaseHeader *>(&atlas_second)};
    end.layers = atlas_layers;
    end.layerCount = 3;
    CHECK(viewer.lock());
    viewer.data()->overlay_selection.mode = 1;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 2);
    auto atlas_selected = viewer.data()->frame.overlays[0].slot;
    auto atlas_other = viewer.data()->frame.overlays[1].slot;
    CHECK(atlas_selected != atlas_other);
    viewer.data()->ready = 0;
    viewer.data()->overlay_selection = {2, 1u << atlas_selected, GetCurrentProcessId(),
                                        with_overlays.session};
    viewer.unlock();
    atlas_layers[1] = atlas_layers[2];
    end.layerCount = 2;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 0);
    CHECK(viewer.data()->overlay_catalog.layers[0].slot == atlas_other);
    viewer.data()->ready = 0;
    viewer.data()->overlay_selection.mode = 1;
    viewer.unlock();
    atlas_layers[1] = overlay_layers[1];
    end.layerCount = 3;
    atlas_second.subImage.imageRect = overlay.subImage.imageRect;
    atlas_second.subImage.imageArrayIndex = 0;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 2); // Same rectangle, different array slice.
    viewer.data()->ready = 0;
    viewer.unlock();
    atlas_second.subImage.imageArrayIndex = overlay.subImage.imageArrayIndex;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 0); // Indistinguishable quads fail closed.
    CHECK(viewer.data()->overlay_catalog.count == 1 &&
          viewer.data()->overlay_catalog.layers[0].supported == 0);
    CHECK(std::wcsstr(viewer.data()->overlay_catalog.layers[0].label, L"ambiguous shared image"));
    viewer.data()->ready = 0;
    viewer.unlock();
    atlas_layers[1] = atlas_layers[2];
    end.layerCount = 2;
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 0 && viewer.data()->overlay_catalog.layers[0].supported == 0);
    viewer.data()->ready = 0;
    viewer.unlock();
    // A new unambiguous subimage has a new selection identity.
    overlay.subImage.imageRect.offset.x = 2;
    end.layers = overlay_layers;
    end.layerCount = 5;
    frame(true);
    frame(true); // Releases a fresh update while the previous frame still owns ready=1.
    CHECK(viewer.lock());
    viewer.data()->ready = 0;
    viewer.unlock();
    Sleep(20);
    CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_SUCCESS);
    ++extra_reuses;
    CHECK(viewer.lock());
    CHECK(viewer.data()->ready == 1 && viewer.data()->frame.overlay_count == 3);
    check_pixels(viewer.data()->frame.overlays[0].image, true);
    viewer.data()->ready = 0;
    viewer.unlock();
    // Unchanged overlays need no repeated producer shared-texture copy. Base copies remain.
    auto overlay_copy_start = gpu_copies;
    Sleep(20);
    CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_SUCCESS);
    ++extra_reuses;
    if (gpu_copies - overlay_copy_start != 2)
        std::fprintf(stderr, "Repeated frame copies: expected 2 base, observed %llu total\n",
                     static_cast<unsigned long long>(gpu_copies - overlay_copy_start));
    CHECK(gpu_copies - overlay_copy_start == 2);
    CHECK(viewer.lock());
    CHECK(viewer.data()->ready == 1 && viewer.data()->frame.overlay_count == 3);
    for (unsigned i = 0; i < 3; ++i)
        check_pixels(viewer.data()->frame.overlays[i].image, true);
    viewer.data()->ready = 0;
    viewer.unlock();
    // A successful new release changes pixels and requires all three overlay copies.
    paint_green = true;
    overlay_copy_start = gpu_copies;
    frame(true);
    CHECK(gpu_copies - overlay_copy_start == 7); // 2 snapshot slices + 2 base + 3 overlay.
    CHECK(viewer.lock());
    for (unsigned i = 0; i < 3; ++i)
        check_pixels(viewer.data()->frame.overlays[i].image, false);
    viewer.data()->ready = 2;
    viewer.unlock();
    // Consumer recovery recreates outputs and must copy even cached content.
    overlay_copy_start = gpu_copies;
    Sleep(20);
    CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_SUCCESS);
    ++extra_reuses;
    CHECK(gpu_copies - overlay_copy_start == 5);
    CHECK(viewer.lock());
    for (unsigned i = 0; i < 3; ++i)
        check_pixels(viewer.data()->frame.overlays[i].image, false);
    viewer.data()->ready = 0;
    viewer.unlock();
    paint_green = false;
    const XrCompositionLayerBaseHeader *underlay_layers[]{overlay_layers[1], layers[0], overlay_layers[2],
                                                          overlay_layers[3], overlay_layers[4]};
    end.layers = underlay_layers;
    CHECK(viewer.lock());
    viewer.data()->overlay_selection.mode = 1;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 2);
    CHECK(viewer.data()->overlay_catalog.layers[0].supported == 0);
    CHECK(std::wcsstr(viewer.data()->overlay_catalog.layers[0].label, L"behind game projection"));
    viewer.data()->ready = 0;
    viewer.unlock();
    // Overflow is visible and bounded; previously assigned identities are never reassigned.
    XrCompositionLayerQuad many_quads[20];
    const XrCompositionLayerBaseHeader *many_layers[21]{layers[0]};
    for (unsigned i = 0; i < 20; ++i) {
        many_quads[i] = overlay;
        many_quads[i].subImage.imageRect = {{int(i % 8), int(i / 8)}, {1, 1}};
        many_layers[i + 1] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&many_quads[i]);
    }
    end.layers = many_layers;
    end.layerCount = 21;
    CHECK(viewer.lock());
    viewer.data()->overlay_selection.mode = 1;
    viewer.unlock();
    frame(true);
    CHECK(viewer.lock());
    CHECK(viewer.data()->frame.overlay_count == 8);
    CHECK(viewer.data()->overlay_catalog.count <= 16);
    CHECK(std::wcsstr(viewer.data()->status, L"overlay limit"));
    viewer.data()->ready = 0;
    viewer.data()->overlay_selection = {};
    viewer.unlock();
    end.layers = overlay_layers;
    end.layerCount = 5;
    views[1].subImage.imageRect.offset.x = -1;
    frame(false);
    CHECK(viewer.lock());
    CHECK(viewer.data()->overlay_catalog.count == 4); // Inventory refreshes despite unusable base subrect.
    viewer.unlock();
    views[1].subImage.imageRect.offset.x = 1;
    end.layerCount = 0;
    frame(false);
    CHECK(viewer.lock());
    CHECK(viewer.data()->overlay_catalog.count == 0);
    viewer.unlock();
    end.layers = layers;
    end.layerCount = 1;
    // Subsequent legacy zero-copy tests use a base-only swapchain again.
    CHECK(fn<PFN_xrDestroySwapchain>(g, "xrDestroySwapchain")(swapchain) == XR_SUCCESS);
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
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
    // Expiry clears a pending preview and suppresses GPU work while all runtime calls still progress.
    frame(true);
    test_clock::unix_seconds = UINT64_MAX;
    before = gpu_copies;
    frame(false);
    CHECK(gpu_copies == before);
    CHECK(viewer.lock());
    CHECK(viewer.data()->status_code == 6);
    CHECK(viewer.data()->render_enabled == 1); // The running instance retains its render snapshot.
    viewer.unlock();
    test_clock::unix_seconds = 1;
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
    // An as-yet undiscovered sparse overlay's first release also survives backpressure.
    frame(true);
    CHECK(fn<PFN_xrDestroySwapchain>(g, "xrDestroySwapchain")(swapchain) == XR_SUCCESS);
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
    frame(true);
    CHECK(viewer.lock());
    viewer.data()->ready = 0;
    viewer.data()->overlay_selection.mode = 1;
    viewer.unlock();
    end.layers = overlay_layers;
    end.layerCount = 5;
    Sleep(20);
    CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_SUCCESS);
    ++extra_reuses;
    CHECK(viewer.lock());
    CHECK(viewer.data()->ready == 1 && viewer.data()->frame.overlay_count == 3);
    check_pixels(viewer.data()->frame.overlays[0].image, true);
    viewer.data()->ready = 0;
    viewer.unlock();
    end.layers = layers;
    end.layerCount = 1;
    CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
    // Static images released while paused remain available after claim and texture reset,
    // without reacquiring or rereading the blue image now owned by the fake runtime.
    CHECK(fn<PFN_xrCreateSession>(g, "xrCreateSession")(instance, &si, &session) == XR_SUCCESS);
    sc.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    CHECK(fn<PFN_xrCreateSwapchain>(g, "xrCreateSwapchain")(session, &sc, &swapchain) == XR_SUCCESS);
    CHECK(viewer.lock());
    viewer.data()->paused = 1;
    viewer.unlock();
    frame(false);
    for (unsigned reuse = 0; reuse < 3; ++reuse) {
        CHECK(viewer.lock());
        viewer.data()->paused = 0;
        viewer.data()->ready = reuse == 2 ? 2 : 0;
        viewer.data()->heartbeat_ms = GetTickCount64();
        viewer.data()->overlay_selection.mode = 1;
        viewer.unlock();
        end.layers = overlay_layers;
        end.layerCount = 5;
        Sleep(20);
        const auto static_copy_start = gpu_copies;
        CHECK(fn<PFN_xrEndFrame>(g, "xrEndFrame")(session, &end) == XR_SUCCESS);
        CHECK(gpu_copies - static_copy_start == (reuse == 1 ? 1u : 4u));
        ++extra_reuses;
        CHECK(viewer.lock());
        CHECK(viewer.data()->ready == 1 && viewer.data()->frame.overlay_count == 3);
        check_pixels(viewer.data()->frame.views[0], true);
        for (unsigned i = 0; i < 3; ++i)
            check_pixels(viewer.data()->frame.overlays[i].image, true);
        viewer.data()->ready = 0;
        viewer.unlock();
    }
    CHECK(fn<PFN_xrDestroySession>(g, "xrDestroySession")(session) == XR_SUCCESS);
    sc.createFlags = 0;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    end.layers = layers;
    end.layerCount = 1;
    CHECK(viewer.lock());
    viewer.data()->overlay_selection = {};
    viewer.unlock();
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
    CHECK(release_calls + 1 + int(extra_reuses) == end_calls);

    runtime_texture->Release();
    reader_context->Release();
    reader_device->Release();
    context->Release();
    device->Release();
    CHECK(DeleteFileW(temp_file));
    SetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_SETTINGS", nullptr);
    std::puts(
        "PASS: render settings round-trip/clamping; stereo/canted quad render expansion; resolution caps; "
        "count-only/failed calls; masks/types/runtime validation; startup snapshot; active settings/eye IPC; "
        "expired instance forwarding/native FOV; active FOV retention; zero-copy expiry; "
        "transport validation; before-release pixels; quad/array/rect; unchanged calls/results; "
        "pause; backpressure; failed release; invalid bounds; reference epoch; MSAA stereo; producer "
        "exclusivity; zero-copy inactive/paused/backpressure/rate-limit; stale reconnect; "
        "discarded-key recovery; lock contention; reused submission; recreated/unsupported session; "
        "overlay None/All/Selected catalog; quad/cylinder/projection; eye visibility; relative spaces; "
        "overlay busy rollback; stable generations; capacity; static inactive release/reconnect");
    return 0;
}
