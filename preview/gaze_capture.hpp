#pragma once
#include <openxr/openxr.h>
#include <vrst/spectator.hpp>
#include <cstdint>
#include <vector>
#include <utility>

namespace vrst::preview {
enum class GazeCapability : std::uint32_t { disabled, unsupported, waiting_actions, inactive, tracked, unknown_time };
bool gaze_extension_available(PFN_xrGetInstanceProcAddr);
struct GazeRuntime {
    XrInstance instance{};
    XrActionSet set{};
    XrAction action{};
    bool ever_attached{};
    unsigned action_generation{};
    std::vector<std::pair<XrActionSet,XrAction>> retired; // Kept alive for earlier sessions.
    XrPath profile{}, binding{};
#define GAZE_PROC(n) PFN_xr##n n{};
    GAZE_PROC(CreateActionSet) GAZE_PROC(DestroyActionSet) GAZE_PROC(CreateAction)
    GAZE_PROC(DestroyAction) GAZE_PROC(StringToPath) GAZE_PROC(SuggestInteractionProfileBindings)
    GAZE_PROC(AttachSessionActionSets) GAZE_PROC(SyncActions) GAZE_PROC(GetActionStatePose)
    GAZE_PROC(CreateActionSpace) GAZE_PROC(CreateReferenceSpace) GAZE_PROC(DestroySpace)
    GAZE_PROC(LocateSpace) GAZE_PROC(GetSystemProperties)
#undef GAZE_PROC
    void initialize(XrInstance, PFN_xrGetInstanceProcAddr, bool extension_enabled);
    void release();
    XrResult suggest(const XrInteractionProfileSuggestedBinding*);
    bool create_actions(XrActionSet&, XrAction&);
};
struct GazeCapture {
    GazeRuntime *runtime{};
    XrSession session{};
    XrSpace local{}, gaze{};
    XrActionSet action_set{};
    XrAction action{};
    bool attached{}, synchronized{};
    GazeCapability capability{GazeCapability::unsupported};
    void initialize(GazeRuntime&, XrSession, XrSystemId);
    void release();
    XrResult attach(const XrSessionActionSetsAttachInfo*);
    XrResult sync(const XrActionsSyncInfo*, bool requested);
    GazeSample sample(bool requested, XrTime, XrSpace submitted_space,
                      XrQuaternionf submitted_eye, std::uint64_t epoch);
};
} // namespace vrst::preview
