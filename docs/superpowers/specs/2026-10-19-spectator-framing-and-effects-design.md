# Optional spectator framing, stabilization and visual effects

Status: draft for user review. Product implementation has not started.

## Intent and agreed requirements

Make SteadyXR's spectator video more comfortable, intentional and visually attractive while preserving natural head movement and protecting VR frame time. The user's gaming PC has an RTX 5090, the stream PC an RTX 4090, and the headset is a Pimax Dream Air. Prioritize SteamVR + SBoys3; also support native Pimax OpenXR when its runtime supplies usable gaze.

The user requested gaze-driven framing, a forbidden area for chat, more advanced stabilization, and exposure, bloom and color adjustments. Chat is fixed in the virtual cockpit/world. When looking at chat, ignore the gaze contribution but continue normally stabilized head following. Do not hold the spectator camera facing forward independently of the head.

Every new feature is optional and off by default. Existing settings and presets keep their current output. Provide separate feature switches and a master bypass for the new processing. Save new tuning in presets; keep transient calibration and comparison activity out of presets. DLSS 5 remains a deferred research idea in TODO.md.

## Delivery sequence and alternatives

Implement three independently testable stages:

1. Gaze acquisition, framing, and chat exclusion/calibration in the existing OpenXR layer, standalone preview and direct OBS source.
2. An optional natural-motion controller coordinated with gaze and the available image margin.
3. Shared GPU visual effects exposed in the preview/direct source and a dedicated OBS filter for capture-card video on the stream PC.

Start each stage from a verified baseline. Reuse the renderer and settings architecture, but keep acquisition, camera control and image effects in separate modules with explicit inputs and reset rules.

The recommended two-PC configuration keeps capture, stabilization and gaze framing on the gaming PC, sends that output through the existing capture card, then applies visual effects on the stream PC before OBS graphics and encoding. This moves the added visual-effects workload to the 4090. It does not remove the game's VR rendering or existing capture/stabilization cost.

An all-local configuration uses the same effects in the preview/direct OBS source. Users explicitly choose one processing location; the stream filter cannot reliably detect effects already baked into incoming video. Moving the pose/gaze pipeline itself to the stream PC would require a separate synchronized transport project and is outside this design.

## Gaze acquisition and capability reporting

Use the standard XR_EXT_eye_gaze_interaction path where available. The capture layer must obtain a gaze ray, its validity/tracking state, available sample time, and the transforms needed to express it relative to the captured eye and a stable runtime reference space. Pair this metadata with the submitted frame and its session/reference epoch. Do not infer gaze from the center of a foveated view.

Report distinct states: disabled, capture setup/restart required, unsupported runtime/device, available but untracked, stale sample, valid gaze, calibration, and gaze excluded by chat. Missing or invalid gaze smoothly removes the gaze contribution; ordinary capture and head stabilization continue.

Enabling the required instance extension can require upgrading the capture layer and restarting the game. Setup must report that requirement. Merely enabling a viewer checkbox cannot add an extension to an already-created OpenXR instance. Installation must retain the existing explicit connection-upgrade flow.

Any layer-owned actions must coexist with the game's actions. Preserve the game's bindings, attachment order, synchronization, focus behavior and results. OpenXR permits action-set attachment only once per session; do not issue a second attachment or run independent synchronization that changes the game's input state. The implementation plan must resolve and test action attachment/synchronization through the existing layer chain, including games that do not use gaze and other eye-tracking layers. If a safe acquisition path is unavailable, report it and disable gaze framing rather than interfere with gameplay.

Use the reported gaze sample time when available. Preserve the distinction between a known sample time and an unknown timestamp; do not describe a query-time estimate as an exact measurement. Reject nonfinite rays, invalid tracking, mismatched sessions/reference epochs and unusably old data. Bound all buffers and work. No full-frame CPU image readback is needed for gaze.

Extend the capture transport with a new version so older producers cannot be misread. Keep single-consumer ownership and nonblocking texture access. Gaze data must be associated with the same frame and eye as its image, including context/foveal composition and repeated renders.

## Gaze framing behavior

Gaze subtly biases the spectator composition toward a sustained point of interest. It does not replace normal head following or change the headset image.

- An adjustable central neutral region avoids reacting to small eye movements.
- A short fixation dwell filters out brief glances and saccades. An initial candidate is approximately 200 ms; tune against recordings rather than treating this as a physiological constant.
- Separate horizontal and vertical travel limits constrain the contribution. Default vertical influence should be modest to avoid repeatedly pulling the view down to instruments.
- Ease changes in the target and bound acceleration. Brief invalid samples/blinks retain continuity; longer loss returns smoothly to neutral. Reacquisition requires a new stable fixation.
- Compute the gaze target in a common reference frame with the stabilized head target. Do not independently add a head turn and an equivalent eye turn twice.
- Respect existing crop, asymmetric reserves, output aspect, Y offset and mirror limits. Allocate available image margin to stabilization first and reduce gaze travel smoothly near its limit. Gaze must not silently enable more game rendering or additional crop.

Expose enable, strength, horizontal/vertical travel, response/dwell, neutral-region size, and a control-panel diagnostic. Diagnostics and calibration markers do not appear in the clean stream output. Provide a conservative preset within the new feature; loading existing presets does not enable it.

## Learn chat position and forbidden areas

Provide **Learn chat position** in the controls and a bindable hotkey. Use the same calibration state machine in standalone and OBS.

1. Start a three-second countdown so the user can return attention to the headset.
2. Ask the user to look at the center of chat. Collect approximately one second of valid, stable gaze plus the corresponding reference transforms.
3. Average accepted directions robustly. If tracking is missing, samples are scattered, or the reference changes, explain the failure and preserve the previous zone.
4. Store the center direction in the stable reference frame. Adjustable angular width and height define the forbidden area around it; one center sample does not reveal the panel's size or depth.
5. A control-panel **Inside chat area / Outside chat area** indicator lets the user look around the panel and adjust coverage. Relearn and Clear are explicit actions.

The first implementation learns an angular region from the seated position. It remains fixed through head rotation and preview reframing, unlike a screen-space rectangle. It is not a measured 3D panel: large position changes, moving chat, runtime changes or recentering require relearning. A panel that moves independently in game-world coordinates cannot be tracked accurately from one calibration alone. Automatic attachment to a detected OpenXR overlay's geometry is a possible later extension, not assumed for SteamVR compositor overlays.

Classify gaze against the zone before it becomes a framing target. Inside it, ease the gaze offset back toward neutral while leaving head stabilization active. Use a small boundary hysteresis and reentry dwell to prevent flicker when reading near an edge. Keep the zone's enable switch independent of gaze framing.

Save zone size and calibration preferences. Do not silently reuse a world-direction calibration in a different runtime, session or reference origin; mark it as requiring relearning unless the implementation can prove the anchor is still valid. Presets must not transplant a live reference-space anchor into another session. A gaze-to-zone indicator should make this state visible without relying on the streamed image.

## Optional natural-motion stabilization

Retain the current algorithm as the default and comparison baseline. Add a new mode built around adaptive orientation filtering and a smoothly damped camera target.

Evaluate a speed-adaptive filter, inspired by the 1 Euro approach, against the existing per-axis and signed-turn-follow machinery. Use filtered directional velocity and coherence to distinguish sustained turns from alternating vibration; simply reducing smoothing whenever instantaneous speed rises would also admit fast jitter.

The proposed controller has three responsibilities:

- Suppress small, rapid angular disturbances, with independent yaw/pitch/roll tuning.
- Follow a deliberate, sustained turn promptly, then settle without overshoot or an abrupt catch-up at its end.
- Combine the head target, fixation-derived gaze bias and horizon correction once, then limit the resulting view to available image margin.

Keep quaternion calculations normalized and use elapsed time rather than render-call count. Avoid Euler wrap jumps. Bound lag, correction angle, speed and acceleration; at a crop limit, smoothly follow the source instead of freezing. Retain the existing horizon reset behavior. Do not add artificial head bob or random motion. Without game telemetry/depth, this is an angular camera controller, not a model that independently knows car motion or reconstructs head translation.

Expose a compact set of meaningful controls: micro-jitter suppression, deliberate-turn responsiveness, settling, per-axis amounts, and maximum correction. Suggested tuning presets are Natural and Cinematic, both explicit opt-ins. Keep advanced mathematical parameters out of the main panel.

Gaze and stabilization must be independently switchable. Gaze works with either the existing or new stabilization mode; new stabilization works without gaze. Gaps, game restart, reference reset, eye changes and source changes have explicit reset/reacquisition behavior. Rendering a frame twice must not advance either controller twice.

## Optional exposure, bloom and color

Use a shared effects implementation with separate enable flags. A separate OBS video filter allows the 4090 to apply these effects to the already-stabilized capture-card source. Existing stabilization/filter behavior remains available without the new effects.

### Exposure

Support manual exposure compensation and optional automatic adaptation with separate brightening/darkening rates and bounded gain. Meter a reduced-resolution representation of genuine image content, excluding letterboxing, mirrored borders and separately captured overlays. Prefer robust luminance statistics to a simple average that a small bright HUD can dominate.

Offer an optional, restrained transition effect for changes such as leaving a tunnel, with adjustable intensity and recovery. Keep it separate from basic correction so users can choose accurate exposure or a stylized response. Respect game-provided exposure changes and limit feedback/pumping. A stream-PC input can already contain baked-in HUD/overlays; provide a metering region rather than claim those elements can be perfectly separated.

The existing output is SDR. Processing cannot recover clipped highlights or turn the capture into native HDR. HDR support would require a separate explicit design; unsupported input color spaces must be identified rather than silently processed with SDR assumptions.

### Bloom

Use threshold/soft-knee extraction and reduced-resolution blur with adjustable strength/radius. Combine in linear light with controlled highlights. Avoid a full-resolution multi-tap blur when a downsampled pyramid suffices. Disable the passes and release unneeded resources when bloom is off.

### Color

Start with exposure compensation, temperature/tint, contrast, saturation and shadow/highlight shaping. Keep a neutral default and a restrained cinematic preset. Work in a defined linear intermediate and perform input/output transfer conversions exactly once. Avoid silently changing OBS color-range settings. A user LUT workflow can be added later if needed; it is not required for the first delivery.

### Composition and performance

For native capture, apply image effects to the game image before separately captured OpenXR overlays, so chat and HUD need not bloom or drive exposure. For the stream-PC filter, apply before OBS scene graphics; baked-in elements remain part of the incoming video.

An all-disabled path should avoid extra passes, analysis, texture allocations and synchronization. Metering and bloom run on GPU resources; any small statistics transfer must be asynchronous and bounded, with no full-resolution CPU readback or blocking query in the render loop. Reuse existing output scheduling and stop unnecessary processing when paused/hidden.

Measure latency, GPU time, memory and sustained output cadence with effects separately and together. The target is the existing 3840x2160 at 60 FPS, not an unverified performance promise. Test the stream-PC load alongside OBS encoding and the gaming-PC load alongside the actual VR game.

## Settings, controls and comparison

Give Gaze framing, Chat exclusion, Natural stabilization, Exposure, Bloom and Color their own controls. Preserve English UI and the existing dark controls. Put calibration and tracking diagnostics in controls, never over clean video. Maintain standalone hotkeys and native OBS hotkey integration.

Store all new optional settings in complete presets and direct-source imports. Missing keys mean disabled new features; preserve unknown settings and earlier tuning. A master bypass affects new processing without erasing configured values. Loading presets must not activate temporary calibration or A/B state.

Extend the current 20-second A/B flow with clear comparison scope: image-effects comparisons use identical geometry; camera comparisons use the same source frame and defined baseline/enhanced transforms. Estimation continues during comparison, but each captured frame advances state only once. Keep settings and active comparison state separate.

## Verification and completion criteria

- Runtime/layer tests: advertised and absent eye-gaze support, inactive/invalid/unknown-time samples, action-set coexistence, session lifecycle, reference changes, hook ordering, and preservation of game submissions and input results.
- Transport tests: matching frame/eye/gaze metadata, ownership, protocol mismatch, stale data, and reconnect. Old captures must remain usable without gaze when supported by the explicit compatibility path.
- Calibration tests: stable samples, blink/outlier rejection, countdown/cancel, head rotation with a fixed zone, zone boundaries, failed calibration retaining the old zone, and anchor invalidation.
- Camera tests: fixation versus saccades, chat exclusion with continued head following, reacquisition without snapping, no double head/gaze rotation, variable cadence, jitter attenuation, deliberate-turn lag, settling/overshoot, margin limits and recentering. Compare measured behavior with the existing algorithm on identical sequences.
- GPU tests: neutral/bypass output, transfer-function correctness, exposure response and bounds, bloom threshold/radius, overlay order, alpha, cropped/letterboxed content, source resets, A/B and 4K output. Test the actual D3D11 renderer and OBS filter.
- Settings/UI tests: feature independence, legacy imports/presets, complete round trips, invalid values, hotkeys, and visible calibration/capability status.
- Live validation: Dream Air with SteamVR + SBoys3 first, native Pimax OpenXR second; verify real gaze availability, chat behavior and racing comfort. Automated fake-runtime tests do not establish hardware compatibility.
- Packaging: build and deliver preview, capture DLL and OBS module plus corresponding source/checksums to the established OneDrive directory. Preserve previous packages and user settings; keep the existing 30-day build policy. Runtime/OBS configuration changes and game restarts remain explicit user setup actions.

## Evidence and open implementation questions

The repository already shares Renderer/FrameStabilizer between preview and the direct OBS source, supports context/foveal image composition and additional OpenXR layers, and has native D3D11/OBS test harnesses. The current transport has no gaze metadata, and the layer does not yet manage gaze actions.

SBoys3's release notes list Dream Air eye tracking. This supports investigating the standard runtime path but does not verify the user's installed driver/runtime or permissions. The first implementation milestone must establish that path before building the full framing UI.

Remaining engineering questions are provider coexistence in the live layer chain, safe behavior for games with no action synchronization, reliable reference-anchor lifetime, and measured visual quality/performance. Resolve these through focused tests and document any capability limits; do not disguise missing gaze as centered valid data.

References:

- [SBoys3 CustomHeadsetOpenVR releases](https://github.com/sboys3/CustomHeadsetOpenVR/releases)
- [OpenXR eye-gaze interaction](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_eye_gaze_interaction.html)
- [OpenXR gaze sample timestamps](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEyeGazeSampleTimeEXT.html)
- [OpenXR action-set attachment rules](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrAttachSessionActionSets.html)
- [1 Euro filter and original research](https://gery.casiez.net/1euro/)
- [NVIDIA DLSS 5 technical overview](https://developer.nvidia.com/blog/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities)
