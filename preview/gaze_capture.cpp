#include "gaze_capture.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
namespace vrst::preview {
bool gaze_extension_available(PFN_xrGetInstanceProcAddr gipa) {
    PFN_xrEnumerateInstanceExtensionProperties enumerate{};
    if (!gipa || XR_FAILED(gipa(XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties",
                               reinterpret_cast<PFN_xrVoidFunction*>(&enumerate))) || !enumerate) return false;
    uint32_t count{};
    if (XR_FAILED(enumerate(nullptr,0,&count,nullptr)) || !count || count>4096) return false;
    std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(enumerate(nullptr,count,&count,extensions.data())) || count>extensions.size()) return false;
    return std::any_of(extensions.begin(),extensions.begin()+count,[](const auto &e) {
        return !std::strcmp(e.extensionName,XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    });
}
void GazeRuntime::initialize(XrInstance value, PFN_xrGetInstanceProcAddr gipa, bool enabled) {
    instance=value;
#define LOAD(n) gipa(instance,"xr" #n,reinterpret_cast<PFN_xrVoidFunction*>(&n));
    LOAD(AttachSessionActionSets) LOAD(SyncActions) LOAD(SuggestInteractionProfileBindings)
    if (!enabled) return;
    LOAD(CreateActionSet) LOAD(DestroyActionSet) LOAD(CreateAction) LOAD(DestroyAction)
    LOAD(StringToPath) LOAD(GetActionStatePose) LOAD(CreateActionSpace) LOAD(CreateReferenceSpace)
    LOAD(DestroySpace) LOAD(LocateSpace) LOAD(GetSystemProperties)
#undef LOAD
    if (!CreateActionSet || !DestroyActionSet || !CreateAction || !DestroyAction || !StringToPath ||
        !SuggestInteractionProfileBindings || !AttachSessionActionSets || !SyncActions || !GetActionStatePose ||
        !CreateActionSpace || !CreateReferenceSpace || !DestroySpace || !LocateSpace || !GetSystemProperties) return;
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName,"steadyxr_spectator_gaze");
    std::strcpy(si.localizedActionSetName,"SteadyXR spectator gaze");
    if (XR_FAILED(CreateActionSet(instance,&si,&set))) { set={}; return; }
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType=XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(ai.actionName,"steadyxr_gaze_pose"); std::strcpy(ai.localizedActionName,"Spectator gaze pose");
    if (XR_FAILED(CreateAction(set,&ai,&action)) ||
        XR_FAILED(StringToPath(instance,"/interaction_profiles/ext/eye_gaze_interaction",&profile)) ||
        XR_FAILED(StringToPath(instance,"/user/eyes_ext/input/gaze_ext/pose",&binding))) { release(); return; }
    XrActionSuggestedBinding own{action,binding};
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.interactionProfile=profile; info.countSuggestedBindings=1; info.suggestedBindings=&own;
    if (XR_FAILED(SuggestInteractionProfileBindings(instance,&info))) release();
}
void GazeRuntime::release() {
    if (action && DestroyAction) DestroyAction(action);
    if (set && DestroyActionSet) DestroyActionSet(set);
    action={}; set={};
}
XrResult GazeRuntime::suggest(const XrInteractionProfileSuggestedBinding *info) {
    if (!SuggestInteractionProfileBindings) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!action || !info || info->interactionProfile!=profile ||
        (info->countSuggestedBindings && !info->suggestedBindings))
        return SuggestInteractionProfileBindings(instance,info);
    try {
        std::vector<XrActionSuggestedBinding> bindings;
        if(info->countSuggestedBindings) bindings.assign(info->suggestedBindings,info->suggestedBindings+info->countSuggestedBindings);
        bindings.push_back({action,binding});
        auto merged=*info; merged.countSuggestedBindings=uint32_t(bindings.size()); merged.suggestedBindings=bindings.data();
        const auto result=SuggestInteractionProfileBindings(instance,&merged);
        if (XR_SUCCEEDED(result)) return result;
    } catch (...) {}
    return SuggestInteractionProfileBindings(instance,info);
}
void GazeCapture::initialize(GazeRuntime &r, XrSession value, XrSystemId system) {
    runtime=&r; session=value;
    if (!r.action) return;
    XrSystemEyeGazeInteractionPropertiesEXT eyes{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES}; properties.next=&eyes;
    if (XR_FAILED(r.GetSystemProperties(r.instance,system,&properties)) || !eyes.supportsEyeGazeInteraction) return;
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    reference.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; reference.poseInReferenceSpace.orientation.w=1;
    XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO}; space.action=r.action; space.poseInActionSpace.orientation.w=1;
    if (XR_FAILED(r.CreateReferenceSpace(session,&reference,&local)) || XR_FAILED(r.CreateActionSpace(session,&space,&gaze))) {
        release(); return;
    }
    capability=GazeCapability::waiting_actions;
}
void GazeCapture::release() {
    if (runtime && runtime->DestroySpace) {
        if (gaze) runtime->DestroySpace(gaze);
        if (local) runtime->DestroySpace(local);
    }
    gaze={}; local={}; attached=synchronized=false; capability=GazeCapability::unsupported;
}
XrResult GazeCapture::attach(const XrSessionActionSetsAttachInfo *info) {
    if (!runtime || !runtime->AttachSessionActionSets) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (gaze && !attached && info && info->countActionSets && info->actionSets) try {
        std::vector<XrActionSet> sets(info->actionSets,info->actionSets+info->countActionSets);
        sets.push_back(runtime->set);
        auto merged=*info; merged.countActionSets=uint32_t(sets.size()); merged.actionSets=sets.data();
        auto result=runtime->AttachSessionActionSets(session,&merged);
        if (XR_SUCCEEDED(result)) { attached=true; return result; }
    } catch (...) {}
    return runtime->AttachSessionActionSets(session,info);
}
XrResult GazeCapture::sync(const XrActionsSyncInfo *info, bool requested) {
    synchronized=false;
    if (!runtime || !runtime->SyncActions) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (requested && attached && info && info->countActiveActionSets < 64 &&
        (!info->countActiveActionSets || info->activeActionSets)) {
        std::array<XrActiveActionSet,64> sets{};
        if (info->countActiveActionSets) std::copy_n(info->activeActionSets,info->countActiveActionSets,sets.begin());
        sets[info->countActiveActionSets]={runtime->set,XR_NULL_PATH};
        auto merged=*info; merged.countActiveActionSets=info->countActiveActionSets+1; merged.activeActionSets=sets.data();
        auto result=runtime->SyncActions(session,&merged);
        if (XR_SUCCEEDED(result)) { synchronized=result==XR_SUCCESS; return result; }
    }
    return runtime->SyncActions(session,info);
}
GazeSample GazeCapture::sample(bool requested, XrTime time, XrSpace submitted_space,
                              XrQuaternionf eye, std::uint64_t epoch) {
    GazeSample out; out.reference_epoch=epoch;
    capability=!requested ? GazeCapability::disabled : !gaze ? GazeCapability::unsupported :
               !attached ? GazeCapability::waiting_actions : GazeCapability::inactive;
    if (!requested || !gaze || !attached || !synchronized) return out;
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO}; info.action=runtime->action;
    XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
    if (XR_FAILED(runtime->GetActionStatePose(session,&info,&state)) || !state.isActive) return out;
    XrEyeGazeSampleTimeEXT timestamp{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION}, reference{XR_TYPE_SPACE_LOCATION}; location.next=&timestamp;
    if (XR_FAILED(runtime->LocateSpace(gaze,local,time,&location)) ||
        XR_FAILED(runtime->LocateSpace(submitted_space,local,time,&reference))) return out;
    constexpr auto valid_bit=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    constexpr auto tracked_bit=XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (!(location.locationFlags&valid_bit) || !(reference.locationFlags&valid_bit)) return out;
    const auto q=[](XrQuaternionf v) { return Quat{v.x,v.y,v.z,v.w}; };
    if (!valid(q(location.pose.orientation)) || !valid(q(reference.pose.orientation)) || !valid(q(eye))) return out;
    out.reference_from_eye=normalized(multiply(q(reference.pose.orientation),q(eye)));
    out.direction=rotate(normalized(q(location.pose.orientation)),{0,0,-1});
    out.flags=GazeValid;
    if ((location.locationFlags&tracked_bit) && (reference.locationFlags&tracked_bit)) out.flags|=GazeTracked;
    out.sample_time_ns=timestamp.time;
    if (timestamp.time>0) out.flags|=GazeTimeKnown;
    if (out.flags&GazeTracked) capability=timestamp.time>0 ? GazeCapability::tracked : GazeCapability::unknown_time;
    return out;
}
}
