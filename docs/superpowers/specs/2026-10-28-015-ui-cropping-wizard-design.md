# SteadyXR 0.15.0: native UI and capture workshop

Status: proposed design for user review, not approved for product implementation.

## Intent and agreed constraints

Dan wants a simpler modern standalone UI, inspired by the VectorXR navigation and using https://dansuzuki.com/ui-kit/ colors. The primary goal remains minimal impact on the gaming PC. He chose modernizing the existing native UI over a WebView/Tauri UI, and asked to see a clickable mockup before implementation.

He wants to drive, press an assignable keyboard shortcut or joystick/wheel button, record about 30 seconds, then replay that capture while changing stabilization and crop settings. He clarified that vertical stabilization primarily means looking up/down (pitch), not physical vertical head displacement.

The signed 0.14.0 capture DLL MUST remain byte-identical:

`74a6943f014d85b81f301da852b61d5a8cb2251a199130978bf4cbdba097d91d`

No capture DLL rebuild, replacement with unsigned output, signing, injection change or IPC layout change is part of 0.15.0. Existing game/runtime configuration is preserved. All new features are optional. 0.14.0 remains an independently usable release.

## Approaches

1. **Modern native UI + existing C++ renderer (selected by Dan).** Reuse the rendering pipeline and update layout, theme and native controls. Avoid a browser runtime in the shipped app. More custom control/layout work, but best fit for the resource constraint.
2. **Web frontend similar to VectorXR.** Easier rich UI layout and component reuse, but an additional runtime, bridge and deployment surface. Not selected.

For recording, propose compressed clips plus synchronized metadata, rather than keeping 30 seconds of full-resolution images in RAM/VRAM. Use Windows Media Foundation hardware encoding when available. This is an implementation recommendation, not a measured performance claim. Never silently fall back to a heavy software encoder.

## UI proposal

Keep the app native C++/Win32 with existing D3D11 output. No wholesale frontend framework migration. Navigation follows VectorXR's persistent sidebar and grouped controls; visual assets/code are not copied from VectorXR.

Four primary destinations: **Preview**, **Camera**, **Workshop**, **Settings**. Default view emphasizes the preview, connection status and a few essential controls. Rarely used settings remain under expandable Advanced groups. Current controls must remain reachable, including source mode, eye, output monitor, resolution/FPS, horizon, effects, gaze/chat calibration, director, registration and troubleshooting. Keyboard navigation, focus indication, scalable layout and DPI handling are required.

Use actual UI-kit dark tokens: background #0f0f12, surface #1a1b1f, raised surface #25262c, border #32343c, text #f4f4f6, muted #9da0aa, accent #7059dd, accent text #a79bf0. Use the existing/system font stack unless redistribution of kit fonts is deliberately added with its license. Native controls should visually match the reference, not embed the reference website.

The clickable HTML mockup is a disposable design aid. Its synthetic driving illustration, animation and controls are clearly marked as demo; it records no game and is not a performance or stabilization prototype.

## Recording flow

1. Choose an existing OpenXR source and eye, duration (default 30 seconds) and capture quality. Keep full available source FOV; do not record the already-cropped or effect-processed output.
2. Assign a keyboard chord or press a button on a selected joystick/wheel. The app shows connection/readiness and any shortcut conflict. A manual Start button is always available.
3. Trigger a one-shot recording. Show elapsed/remaining time and allow cancellation. No always-on rolling video buffer.
4. Stop at the duration limit, flush bounded pending work, and show the saved clip in Workshop. Disconnect/recenter/size-format changes must produce a clearly identified partial clip or stop reason, never silently pair incompatible frames.

Suggested default quality: 30 fps, at most 2048 pixels on the longer edge, retaining aspect ratio and full FOV. Offer 60 fps / higher resolution as an explicit higher-cost option only within device/encoder capabilities. The recording is for framing and motion tuning; a reduced-resolution or 30-fps recording cannot validate final sharpness or 60-fps cadence. Report actual frame rate, sample gaps and scaling.

Images and metadata must be taken from the same consumed Frame sequence. Store source dimensions, subrect, pixel/color format, FOV, orientation quaternion, source eye, display time, sequence, session and reference epoch; also preserve available gaze and telemetry needed for reproducible optional framing. Shared texture handles are not persistent video data and must not be serialized as replay resources. Head position and depth are NOT in the v4 View payload.

The standalone is the existing owning consumer; recording branches from its own captured GPU textures, after shared-reader access has ended and before stabilization/crop/effects. It does not register a second consumer or ask the game layer to wait. Preserve the existing ownership rule: OBS's OpenXR source and standalone cannot simultaneously own this channel; explain conflicts instead of stealing ownership.

Use a bounded texture/encoder queue and separate file I/O. When overloaded, skip recording work and retain the exact timestamp gap; prioritize game/preview progress. Record drops and show them in the clip. No unbounded RAM/VRAM queue, synchronous disk writes in capture, or automatic render expansion. Recording overhead is transient and nonzero: GPU copy/conversion, hardware encoding and storage still cost resources even without a DLL change.

Implementation may choose the exact Media Foundation codec/container and sidecar schema after a hardware-encoder feasibility check. Version the clip metadata, write atomically, validate lengths and timestamps, bound decoder allocations, and reject malformed clips safely. Initial scope is SDR capture with explicit rejection/explanation of unsupported HDR recording; existing live HDR behavior remains unchanged. HDR recording is a separate followup, not an implicit tonemapping change.

Keyboard uses registered global hotkeys with collision feedback. Joystick binding must support actual racing wheels/HID buttons (DirectInput or appropriate Raw Input), not only XInput gamepads. Persist stable device identity plus button, trigger on press edge, debounce, survive reconnect and avoid repeated starts while held. No input injection, exclusive access or force-feedback writes. Implementer chooses exact Windows input API.

## Workshop playback and crop helper

Dan chose to pause live output while editing a clip in Workshop, to avoid parallel live/replay pipelines. Show an explicit Live paused status. Replay stays inside Workshop; external output displays a paused slate rather than silently broadcasting the recorded clip. Resume live explicitly on returning to Preview, reset stale filter state, and use the original profile unless Apply was chosen. Recording itself still takes place during live capture; only editing pauses live work.

Dan approved the visual direction and requested direct mouse editing: drag the crop interior to reposition it, and drag any edge or corner to resize it. Keep the selected output aspect ratio fixed. Synchronize the crop/zoom and horizontal/vertical position controls continuously in both directions; render from the same canonical crop rectangle. Opposite edges/corners anchor resizing; edge handles preserve the transverse center. Clamp to the recorded source bounds and supported crop limits. Use pointer capture, visible handles, appropriate cursors, cancel on Escape, keyboard fine movement, and a Center action. Convert normalized source coordinates to the existing profile parameters explicitly; demo percentages are not an unverified mapping to renderer settings. Mouse editing changes only draft settings until Apply.

Provide play/pause, looping, scrub timeline, before/after toggle and optional split comparison. Reuse the same native stabilization/composition functions as the live preview. Changes to crop, smoothing, natural movement, roll leveling and pitch stabilization update the replay. Stateful filters must reset and warm up deterministically after a seek or settings change; process from the clip start or a valid state checkpoint so scrubbing does not invent a different camera path.

Draw safe-frame/source-boundary guides and mark frames exposing unavailable pixels. Optional Analyze clip computes a crop recommendation for the currently selected settings from the entire recorded sample, with small configurable margin and explicit coverage limits. Changing stabilization invalidates the previous recommendation. Report sample gaps; a clip cannot prove safe framing for unrecorded movements.

Missing margins cannot be recreated: more correction needs more crop. A full-source recording preserves existing render expansion if already enabled, but replay cannot retroactively increase the game's rendered FOV. Changing render expansion requires a new game instance/recording under the existing semantics; never make it appear to be an offline slider effect.

Use a draft settings copy during experimentation. **Apply profile** explicitly copies supported viewer settings to the selected live profile. Cancel/back leaves live settings alone. Offer naming/saving presets. Color effects can use the existing pipeline; light-adaptation replay must reset/warm up consistently. Do not claim exact HDR/color fidelity for SDR proxy clips.

## Vertical stabilization

Existing horizon leveling compensates roll (tilting sideways); it does not hold vertical viewing direction. Add optional pitch stabilization in the standalone's camera composition, independently of yaw and roll, with strength and an explicit **Set forward view** action. Default off.

Recommend softly anchoring pitch near the calibrated direction while letting sustained intentional large looks follow smoothly. Give the user a bounded strength/response control; avoid snap-back, hard thresholds and double filtering with Natural motion. Exact response curve is left to implementation, verified with recorded and synthetic start/stop/reversal scenarios. Report insufficient image margins instead of silently claiming stabilization succeeded.

Projection orientations are in application layer space, not guaranteed absolute gravity/world coordinates. Calibration and reference-epoch/session resets must be respected. Name this feature **Vertical steadiness / Pitch stabilization**, not a claim of a physically perfect world horizon. It cannot remove true positional head bob, parallax or camera translations without position/depth data. Do not modify the DLL to obtain them.

## Packaging and validation

App/UI reports 0.15.0 while capture component reports the pinned signed 0.14.0. Packaging takes that prebuilt DLL explicitly, verifies its hash and Authenticode status, and fails closed on any mismatch. Do not run the ordinary combined preview-package target if it would rebuild the layer. Freeze DLL-facing IPC structures and manifest negotiation; regression tests must verify v4 compatibility. Existing registration/upgrade must use the pinned DLL and must not substitute a build-directory DLL.

Validate recorded image/orientation association, time gaps, cancellation/partial files, resource limits, ownership conflicts and replay seeking. Compare live and replay composition results on the same metadata, including roll/pitch, gaze, lookahead and effects where supported. Test shortcut conflicts, button holding/reconnect, keyboard-only navigation, DPI and sizing. Measure baseline vs active recording on target hardware, including game frame time, GPU/CPU/VRAM and disk activity; report measurements, not an invented negligible-overhead guarantee. Recheck the signed DLL hash in every delivered ZIP.

## Scope / remaining approval

This is a substantial standalone UI, recording/playback and camera feature release. OBS's existing binary is reused; a new OBS plugin DLL is outside the no-DLL-change scope. The design and clickable layout need Dan's approval before production implementation. Specific codec details and the calibrated pitch response are engineering decisions within the constraints above, to be validated before final delivery.
