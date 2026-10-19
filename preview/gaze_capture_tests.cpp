#include "gaze_capture.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace vrst;
using namespace vrst::preview;
namespace {
template<class T> T handle(int n) { return reinterpret_cast<T>(std::uintptr_t(n)); }
int calls{}, attaches{}, syncs{}, spaces{}, destroyed{}, created{};
bool supported=true, active=true, tracked=true, extension=true, fail_attach=false;
XrTime sample_time=1000000000;
std::vector<XrActionSet> attached_sets, synced_sets;
std::vector<XrActionSuggestedBinding> bindings;
void check(bool v, const char *s) { if (!v) throw std::runtime_error(s); }
XrResult XRAPI_CALL enumerate(const char*, uint32_t capacity, uint32_t *n, XrExtensionProperties *p) {
    *n=extension?1:0;
    if (capacity && extension) std::strcpy(p[0].extensionName,XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL create_set(XrInstance,const XrActionSetCreateInfo*,XrActionSet *out) {
    ++created; *out=handle<XrActionSet>(10); return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_set(XrActionSet) { ++destroyed; return XR_SUCCESS; }
XrResult XRAPI_CALL create_action(XrActionSet,const XrActionCreateInfo*,XrAction *out) {
    ++created; *out=handle<XrAction>(11); return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_action(XrAction) { ++destroyed; return XR_SUCCESS; }
XrResult XRAPI_CALL path(XrInstance,const char *s,XrPath *out) { *out=std::strstr(s,"interaction_profiles")?42:43; return XR_SUCCESS; }
XrResult XRAPI_CALL suggest(XrInstance,const XrInteractionProfileSuggestedBinding *info) {
    bindings.assign(info->suggestedBindings,info->suggestedBindings+info->countSuggestedBindings); return XR_SUCCESS;
}
XrResult XRAPI_CALL attach(XrSession,const XrSessionActionSetsAttachInfo *info) {
    ++attaches; attached_sets.assign(info->actionSets,info->actionSets+info->countActionSets);
    return fail_attach && info->countActionSets>1 ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
XrResult XRAPI_CALL sync(XrSession,const XrActionsSyncInfo *info) {
    ++syncs; synced_sets.clear();
    for(unsigned i=0;i<info->countActiveActionSets;++i) synced_sets.push_back(info->activeActionSets[i].actionSet);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL state(XrSession,const XrActionStateGetInfo*,XrActionStatePose *out) { ++calls; out->isActive=active; return XR_SUCCESS; }
XrResult XRAPI_CALL create_reference(XrSession,const XrReferenceSpaceCreateInfo *info,XrSpace *out) {
    check(info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL,"stable LOCAL reference");
    *out=handle<XrSpace>(20); ++spaces; return XR_SUCCESS;
}
XrResult XRAPI_CALL create_space(XrSession,const XrActionSpaceCreateInfo*,XrSpace *out) {
    *out=handle<XrSpace>(21); ++spaces; return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_space(XrSpace) { --spaces; return XR_SUCCESS; }
XrResult XRAPI_CALL locate(XrSpace space,XrSpace reference,XrTime time,XrSpaceLocation *out) {
    ++calls; check(reference==handle<XrSpace>(20),"locate in LOCAL"); check(time==1000000000,"display time");
    out->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | (tracked?XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT:0);
    out->pose.orientation={0,0,0,1};
    if(space==handle<XrSpace>(21)) {
        check(out->next,"gaze sample timestamp chain");
        static_cast<XrEyeGazeSampleTimeEXT*>(out->next)->time=sample_time;
    } else out->pose.orientation={0,.70710678f,0,.70710678f};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL properties(XrInstance,XrSystemId,XrSystemProperties *p) {
    static_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(p->next)->supportsEyeGazeInteraction=supported; return XR_SUCCESS;
}
XrResult XRAPI_CALL gipa(XrInstance,const char *name,PFN_xrVoidFunction *out) {
#define F(n,f) if(!std::strcmp(name,"xr" #n)) { *out=reinterpret_cast<PFN_xrVoidFunction>(f); return XR_SUCCESS; }
    F(EnumerateInstanceExtensionProperties,enumerate) F(CreateActionSet,create_set) F(DestroyActionSet,destroy_set)
    F(CreateAction,create_action) F(DestroyAction,destroy_action) F(StringToPath,path)
    F(SuggestInteractionProfileBindings,suggest) F(AttachSessionActionSets,attach) F(SyncActions,sync)
    F(GetActionStatePose,state) F(CreateActionSpace,create_space) F(CreateReferenceSpace,create_reference)
    F(DestroySpace,destroy_space) F(LocateSpace,locate) F(GetSystemProperties,properties)
#undef F
    *out=nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
}
int main() { try {
    check(gaze_extension_available(gipa),"discover advertised extension");
    extension=false; check(!gaze_extension_available(gipa),"absent extension"); extension=true;
    GazeRuntime disabled; disabled.initialize(handle<XrInstance>(1),gipa,false);
    check(!disabled.set && created==0,"no action allocation without extension");
    GazeRuntime runtime; runtime.initialize(handle<XrInstance>(1),gipa,true);
    check(runtime.set && runtime.action && bindings.size()==1,"create and bind own pose action");
    XrActionSuggestedBinding app_binding{handle<XrAction>(12),43};
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile=42; suggested.countSuggestedBindings=1; suggested.suggestedBindings=&app_binding;
    check(runtime.suggest(&suggested)==XR_SUCCESS && bindings.size()==2 && bindings[0].action==app_binding.action,
          "preserve app gaze binding and retain own");
    GazeCapture capture; capture.initialize(runtime,handle<XrSession>(2),77);
    check(spaces==2 && capture.capability==GazeCapability::waiting_actions,"initialize only two spaces");
    check(capture.sample(true,1000000000,handle<XrSpace>(22),{0,0,0,1},7).flags==0 && calls==0 && syncs==0,
          "no unsafe independent sync or attach for games without actions");
    XrActionSet app_set=handle<XrActionSet>(13);
    XrSessionActionSetsAttachInfo attachment{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachment.countActionSets=1; attachment.actionSets=&app_set;
    check(capture.attach(&attachment)==XR_SUCCESS && attaches==1 && attached_sets.size()==2 && attached_sets[0]==app_set,
          "augment single application attachment");
    XrActiveActionSet app_active{app_set,0}; XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets=1; si.activeActionSets=&app_active;
    capture.sync(&si,false); check(syncs==1 && synced_sets.size()==1,"disabled sync preserves application array");
    capture.sample(false,1000000000,handle<XrSpace>(22),{0,0,0,1},7);
    check(calls==0,"disabled capture performs zero runtime queries");
    capture.sync(&si,true); check(syncs==2 && synced_sets.size()==2 && synced_sets[0]==app_set,"requested gaze joins app sync");
    auto gaze=capture.sample(true,1000000000,handle<XrSpace>(22),{0,0,0,1},7);
    check(gaze.flags==(GazeValid|GazeTracked|GazeTimeKnown) && gaze.reference_epoch==7 && gaze.sample_time_ns==sample_time,"timestamped tracked gaze");
    check(gaze.direction.z < -.99 && angle_between(gaze.reference_from_eye,axis_angle({0,1,0},pi/2))<.00001,"eye pose transformed from submitted space");
    sample_time=0; gaze=capture.sample(true,1000000000,handle<XrSpace>(22),{0,0,0,1},8);
    check(!(gaze.flags&GazeTimeKnown) && gaze.reference_epoch==8 && capture.capability==GazeCapability::unknown_time,"explicit unknown clock and epoch");
    active=false; check(!capture.sample(true,1000000000,handle<XrSpace>(22),{0,0,0,1},8).flags,"inactive action rejected"); active=true;
    tracked=false; check(!(capture.sample(true,1000000000,handle<XrSpace>(22),{0,0,0,1},8).flags&GazeTracked),"untracked rejected"); tracked=true;
    capture.release(); check(spaces==0,"release owned spaces");
    supported=false; capture.initialize(runtime,handle<XrSession>(3),77); check(spaces==0 && capture.capability==GazeCapability::unsupported,"headset without eye gaze"); supported=true;
    capture.initialize(runtime,handle<XrSession>(3),77); fail_attach=true;
    check(capture.attach(&attachment)==XR_SUCCESS && !capture.attached && attached_sets.size()==1,"failed optional attachment retries original safely");
    capture.release(); runtime.release(); check(destroyed==created,"release owned actions");
    std::puts("PASS gaze extension, action coexistence, zero disabled queries, timestamps, reference conversion, fallback and lifetime"); return 0;
} catch(const std::exception &e) { std::printf("FAIL %s\n",e.what()); return 1; } }
