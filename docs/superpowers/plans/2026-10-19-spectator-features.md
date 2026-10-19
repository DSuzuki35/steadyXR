# Optional Spectator Features Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Deliver optional gaze framing/chat calibration, natural head stabilization and efficient visual effects in the local preview and OBS, preserving the disabled baseline.

**Architecture:** Portable camera control consumes timestamped poses/gaze; the OpenXR capture layer supplies gaze only when requested. A shared D3D11 image-effects processor serves preview/direct-source output and a stream-PC OBS filter. Settings and controls stay separate from algorithms.

**Tech Stack:** C++17, Windows x64, OpenXR 1.1.43 headers, D3D11/HLSL, Win32, OBS 32.2.2, CMake/CTest, Python packaging.

**Spec:** ../specs/2026-10-19-spectator-framing-and-effects-design.md

## Global Constraints

- All new features are independently optional and off by default; existing settings/presets retain output.
- Gaming-PC overhead is the highest priority: no full-frame CPU readback, no new video buffering, no GPU effects allocations/passes when disabled, no gaze polling when unused.
- Preserve game rendering/input, the active runtime, OpenXR registrations and OBS configuration.
- Primary hardware: RTX 5090 gaming PC, RTX 4090 stream PC, Dream Air with SteamVR + SBoys3; native Pimax OpenXR is secondary.
- Chat exclusion suppresses only gaze framing; normal head following continues. Learn a stable angular region with a countdown/hotkey; no inferred panel depth.
- Preserve current 30-day build policy and deliver a new versioned package/source/checksums to the AGENTS.md OneDrive location without removing older packages.
- User approved the spec and explicitly requested implementation; continue through implementation and verification without another methodology approval.

## Review Focus

1. Disabled/bypassed features preserve pixels and avoid hidden allocations or runtime queries (tasks 1, 2, 4, 5).
2. Gaze does not steal game action bindings, attach twice or resynchronize game input independently (task 2).
3. World-direction chat anchors cannot survive recenter/runtime/session changes silently (tasks 1, 3).
4. Repeated frames, invalid samples and long pauses cannot advance temporal filters incorrectly (tasks 1, 4, 5).
5. Effects preserve alpha/color space and do not meter/bloom separate overlays or count borders as scenery (tasks 4, 5).

## Task 1: Portable spectator camera controller

**Files:** create include/vrst/spectator.hpp, src/spectator.cpp, tests/spectator_tests.cpp; update root CMakeLists.txt.

**Interfaces:** namespace vrst; `GazeSample` is a fixed-layout value with `Quat reference_from_eye`, `Vec3 direction` (unit ray in the stable reference frame), `int64_t sample_time_ns`, `uint64_t reference_epoch`, and `uint32_t flags` (valid/tracked/time-known bits). `SpectatorSettings` holds bypass, gaze enable/strength/dwell/neutral/travel/response, chat enable/width/height and natural-motion enable/suppression/follow/settling/max-correction. All features default false. `SpectatorInput` supplies source orientation, base target orientation, gaze, frame timestamp, session, sequence, reference epoch. `SpectatorController::update(const SpectatorInput&, const SpectatorSettings&) -> Quat` returns a target orientation; `begin_chat_calibration(uint64_t now_ns)`, `cancel_chat_calibration()`, `clear_chat_zone()`, `reset()`, `status() -> SpectatorStatus` expose calibration and diagnostics. Status includes calibration phase/countdown, progress, zone validity/in-zone and gaze state. `sanitize_spectator_settings` clamps all inputs. No Windows dependency, heap allocation per frame or unbounded history.

- [ ] Add failing tests for disabled identity/base-target preservation, valid sustained fixation versus saccades, stale/invalid gaze recovery, head/gaze coordinate composition, angular travel bounds, repeated timestamps/sequences, chat countdown/stable acquisition/outlier rejection/head rotation/neutral return, failed recalibration preserving prior zone, and session/reference invalidation.
- [ ] Run the targeted new test before implementing, then implement the controller with bounded time-based quaternion filtering. Use a 3 s countdown and 1 s acquisition; initially require at least 20 valid samples with bounded angular dispersion. Default fixation dwell 200 ms, maximum gaze age 150 ms, zero/unknown timestamps handled explicitly.
- [ ] Add natural-motion fixtures that compare alternating micro-jitter, sustained turns and settling with the existing smoother; reject large overshoot and timestep dependence. Use directional coherence plus filtered angular velocity; preserve source orientation on resets and avoid Euler wrap discontinuities.
- [ ] Add an opt-in deterministic CPU timing benchmark, reporting enabled/disabled per-frame cost without making hardware-general claims.
- [ ] Build `vrst-spectator-tests`, run CTest `spectator`, run existing portable suites (network requires local network access here), review and commit only owned files.

## Task 2: Capture-layer gaze provider and versioned transport

**Files:** preview/gaze_capture.hpp/.cpp, preview/layer.cpp, preview/layer_tests.cpp, preview/transport.hpp/.cpp, preview/consumer_tests.cpp, preview/CMakeLists.txt.

**Interfaces:** embed `vrst::GazeSample gaze` in `Frame`; increment IPC version/name to v4; Shared has `gaze_requested` plus a small capability status. `Channel::request_gaze(bool)` respects active consumer ownership. Producer passes valid gaze relative to stable LOCAL and selected submitted eye; same session/reference epoch as its frame. Task 1 is the portable interface authority.

- [ ] Add fake-runtime tests for absent extension, valid gaze, invalid/stale samples, zero sample timestamp, reference resets, owner-only requests and no gaze locate/sync calls while disabled.
- [ ] Implement extension capability discovery and layer-owned pose action safely. Prefer augmenting the game's single attach and active sync arrays, retaining all bindings and return behavior. Do not issue a second attachment; do not independently synchronize an attached game action set. Cover the no-game-actions path explicitly and report unsupported when a safe lifecycle is unavailable.
- [ ] Sample once for each captured frame, query reference transforms at matching display time, propagate sample-time flags, and release owned spaces/actions correctly. Never create a second rendering session. Do not alter game projections or action results.
- [ ] Preserve consumer ownership, overlay behavior and frame validation in v4; update compatibility diagnostics/tests. Capture metadata can be initialized at session creation, but disabled per-frame processing must remain skipped.
- [ ] Run native layer/consumer tests and review API lifetime/coexistence before committing.

## Task 3: Camera integration, settings and controls

**Files:** preview/renderer.hpp/.cpp, preview/composition.cpp, preview/composition_tests.cpp, preview/spectator_settings.hpp, preview/spectator_controls.hpp/.cpp, preview/main.cpp, plugin/openxr_source_settings.cpp/.hpp, plugin/openxr_source.cpp, corresponding settings/UI/native-module tests and CMake.

**Interfaces:** Options contains `SpectatorSettings spectator`; FrameStabilizer owns SpectatorController and forwards calibration/status commands through Renderer. Camera target applies exactly once before the final crop limiter and context/foveal correction. Shared metadata/requests are task 2's contract.

- [ ] Add meaningful regression tests for default identical geometry, gaze with old/new stabilization, paired A/B frames, crop limits, calibration and resets.
- [ ] Implement flat INI read/write helpers and OBS conversion for every setting, missing keys off. Keep runtime zone anchors transient and invalidated across sessions/recenter; save zone size and tuning only.
- [ ] Add a modeless Spectator controls window with independent toggles, numeric tuning, Learn/Cancel/Clear buttons, countdown/status and an assignable learning hotkey. Keep clean output free of diagnostics. Add equivalent OBS groups, calibration buttons, refresh status and native hotkey.
- [ ] Wire gaze requests only while visible/active and gaze or calibration needs them; master bypass releases requests. Calibration must still work while framing is off when explicitly requested.
- [ ] Run targeted composition/settings/UI/native-module tests, review, commit.

## Task 4: Shared efficient GPU visual effects

**Files:** preview/visual_effects.hpp/.cpp, preview/visual_effects_tests.cpp, preview/visual_settings.hpp, CMake.

**Interfaces:** `VisualSettings` carries independently disabled exposure, adaptation, bloom, color and bypass; finite bounded controls. `VisualEffects::process(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, const VisualSettings&, uint64_t frame_id, double timestamp_seconds) -> ID3D11Texture2D*`; disabled returns the original input without allocations/draws. `reset()` releases temporal state/resources. Expose diagnostic counters/timings for tests/benchmark. Preserve caller-owned D3D state through a documented integration boundary.

- [ ] Write GPU tests for disabled texture identity/zero pass count, neutral pixel accuracy, manual exposure, temporal adaptation bounds/rates, bloom threshold/radius, color controls, alpha, repeated frame/time resets, resize and input format/color handling.
- [ ] Implement lazy resources, linear-light RGBA16F processing and a small GPU reduction for metering. Avoid synchronous GPU readback: adaptation stays on GPU where practical. Metering and bloom operate at reduced resolution; disabled passes are skipped individually.
- [ ] Include metering region, bounded gain, separate light/dark recovery, restrained optional transition response, bloom soft knee and low-resolution blur. Color includes temperature/tint, contrast/saturation and shadow/highlight adjustments with exact transfer conversion.
- [ ] Add GPU-timestamp benchmark for 4K baseline/manual color/exposure/bloom/all; report timings and skipped passes. Bound query polling and skip measurements when unavailable instead of stalling.
- [ ] Build/run real D3D11 tests, review and commit.

## Task 5: Preview/direct-source effects and stream-PC OBS filter

**Files:** preview/renderer.hpp/.cpp, preview/main.cpp or visual-controls module, plugin/spectator_effects_filter.cpp/.hpp, plugin/obs_filter.cpp, plugin/openxr_source_settings.cpp, plugin/openxr_source.cpp, native module tests, CMake.

- [ ] Add tests proving all hosts expose/save/import controls and preserve legacy output when all new flags are off.
- [ ] Insert effects after game stabilization/framing and before separately captured overlays in preview/direct source. Keep A/B effects-only comparison geometry identical; do not advance exposure twice for comparison.
- [ ] Register a separate OBS filter named **SteadyXR Visual Effects**. Render the input once, process through the shared GPU implementation on the OBS adapter, preserve premultiplied alpha and source dimensions. Unsupported color spaces get a clear fallback, not silent SDR conversion.
- [ ] Expose the same optional controls in local preview, direct OBS source and stream filter, with neutral defaults and complete presets. Explain selecting one effects location to avoid double application.
- [ ] Verify source hide/pause/reconnect/expiry cleanup, alpha, 4K output, off-path counters and actual libobs rendering; review and commit.

## Task 6: End-to-end review, performance and delivery

- [ ] Run required portable, native layer/renderer/UI/OBS/settings/packaging suites on final code. Run real loopback tests with network access. Record hardware-only limitations distinctly.
- [ ] Review all changes against the spec with a fresh reviewer, fix substantive findings, and recheck the affected tests.
- [ ] Run baseline versus enabled CPU/GPU benchmarks at 4K60. Record build/toolchain/GPU and timing method; no claim of live VR headroom without a game measurement.
- [ ] Bump to experimental 0.10.0; document controls, calibration/relearning, runtime fallback, overhead findings, optional paths and 4090 workflow.
- [ ] Produce current-stamp Windows binaries, corresponding source and checksums; verify ZIP CRC, manifest and packaged hashes. Deliver versioned files to AGENTS.md location, preserving existing configurations/packages. Do not install into OBS or register a layer automatically.
- [ ] Report tested behavior, measured costs, delivery paths and remaining live headset validation. Keep implementation local unless user requests publication.
