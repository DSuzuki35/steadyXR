# OpenXR expanded framing — 0.6.0

The user requests real additional rendered scenery on the inner side of either
eye, compatibility with native OpenXR and SteamVR OpenXR with SBoys3, and
independent stabilization margins (for example 2% left and 20% right).

## Rendering

Extend the existing implicit layer through standard OpenXR calls. At instance
creation, read a stable configuration from `preview.ini` beside the loaded DLL.
Expose per-eye left/right expansion, 0–30% of the original tangent-space width
per side, disabled by default. An inward preset sets left-eye right expansion
and right-eye left expansion to 10%. Expand context views 0/1 in stereo and
quad-view configurations; preserve focus views, poses and vertical FOV.

Intercept view resolution recommendations and scale width by 1 + left + right,
respecting runtime bounds, capping additional width at8192 while retaining larger native recommendations. The game owns allocation and rendering;
recommendations cannot force a game to honor either resolution or FOV. Keep the
expanded submitted projection unchanged on its way to the runtime, so the
compositor has the actual render projection. Never enlarge textures behind the
game's back. Disable the visibility mask for expanded context views (return
unavailable/zero counts after runtime validation), so hidden-area culling does
not remove the additional scenery. Pass other views and errors through.

Use IPC v2 with the captured eye in each Frame to prevent mixing per-eye crop
profiles when switching. Report the active instance's expansion settings in the
shared status. Older layers require the explicit connection upgrade button;
delivery alone never changes registration or the user's active runtime.

## Framing and UI

Add a dark modeless `Render & edge settings` window with separate left/right eye
profiles. Render changes are saved for the next game launch. Edge reserve
changes are immediate and independent of render changes. Each profile has a
checkbox enabling four edge reserves (0–35%, actual percentage per edge),
replacing the symmetric reserve while enabled. Existing settings and default
appearance remain unchanged. Aspect fitting must retain the asymmetric framing
bias rather than silently recentering it; document additional aspect crop.

The primary window keeps the existing uniform reserve slider, disabled when the
selected profile uses separate reserves. Existing framing trims remain live.
Window capture supports asymmetric reserves but cannot render extra scenery.
UI and documentation are English. No additional per-frame CPU readback or
full-screen shader pass is introduced.

## Verification and delivery

Test real layer hook chaining with fake runtime inputs (count-only, failures,
stereo/quad, unchanged nonselected eye, mask types, startup settings snapshot),
projection geometry, per-eye switching, settings persistence, UI and existing
GPU regressions. Test runtime-neutral contracts with distinct simulated runtime
profiles; clearly distinguish this from physical headset validation.

Build 0.6.0, run the full suite and an independent review, and deliver ready
Gaming-PC/Stream-PC files, binary ZIP, matching source ZIP and checksums to the
established OneDrive destination. Preserve previous releases and user settings.

Y offset was added at the user’s request: a live -100..100 slider moves the aspect-fitted rectangle inside the available vertical slack, retaining its size and edge trims. It applies to both capture modes.
