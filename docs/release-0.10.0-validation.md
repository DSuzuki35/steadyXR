# SteadyXR 0.10.0 validation — 2026-10-19

Implementation through `1a5bc22` on `codex/spectator-framing-design`. Windows x64 Release, MSVC 19.44, Windows SDK 10.0.26100, native D3D11 on RTX 5090 and isolated libobs 32.2.2. The final source review approved the branch after the HDR negotiation correction. No installed OBS scene/settings, runtime selection or OpenXR registration was changed.

## Automated verification

- Complete native suite: **34/34 passed**, 57.84 seconds. Includes portable math/camera, sender/network, fake OpenXR provider/layer, IPC ownership, real GPU effects/rendering, preview controls/settings/presets, registry-editor fixtures, delivery/package fixtures and real libobs module/settings tests.
- After broadening GPU state checks and setting the preview's UTF-8 compiler mode: **5/5 affected suites passed**, 34.90 seconds (visual effects, package layout, expiry, preview UI and OBS module).
- HDR regression: a fixture that implements real libobs async color-space preference fallback reproduced the issue first. The fix probes both HDR spaces before deciding SDR compatibility. The focused OBS module then passed, covering both HDR spaces, one upstream render, explicit passthrough and no effects resources.
- Final aligned ALL build after the correction: successful. App, layer and OBS module share the generated build timestamp. Final expiry/UI/layer/OBS checks: **4/4 passed**, 28.30 seconds.
- Native renderer regressions verify disabled zero allocations/dispatches, resource release, exposure before overlays, neutral transfer/alpha, same-frame A/B, pillarboxed overlay alignment, finite mirror bounds and head-first correction allocation. The camera tests also verify calibration before the first frame and clearing transient diagnostics when acquisition stops.
- Shared GPU tests verify CS shader, all three touched SRVs, UAV and constant-buffer restoration plus untouched graphics viewport. No CPU image/statistics readback occurs in the live effects path; readbacks belong to test fixtures only.

The build retains the existing renderer warning for converting double image-warp coordinates to float shader constants. It has no new build errors. Synthetic fixtures do not establish live headset compatibility.

## Performance evidence

At 3840×2160, the shared effects core measured **0.168–0.187 ms** for all effects across repeated local runs, using GPU timestamp queries only in the test harness. Manual exposure/color use one compute dispatch; all effects use five. Texture usage is about 63.3 MiB for the output and 71.2 MiB with bloom/adaptation. Fully disabled hosts create no effects textures and dispatch no effects work. Host composition, OBS render/copy costs, encoding, warm-up and actual VR load are excluded.

The final MSVC portable-controller benchmark measured 156.5 ns/update disabled and 638.7 ns/update with gaze/natural enabled, including deterministic fixture generation. The synthetic 45°/s turn lag was 1.03° with natural mode versus 9.39° with the existing smoother. Alternating ±1° jitter residual was 0.028° versus 0.026° respectively; natural mode is not universally better on every jitter sequence.

For minimum gaming-PC impact, leave local image effects off and process the already stabilized capture on the stream PC using **SteadyXR Visual Effects**. Gaze polling is gated by the owning active consumer's request for framing or calibration. All new feature switches default off.

## Delivery and remaining live checks

The 0.10.0 build timestamp is **2026-10-19 12:28:45 UTC**, with the existing preview/capture validity ending **2026-11-18 12:28:45 UTC**. The standalone OBS effects filter follows the existing OBS image-filter validity behavior. Settings are preserved by the versioned delivery process. Upgrade the OpenXR connection from the new app and restart the game to use IPC 4; delivery itself does not perform either action.

Still requires the user's hardware: Dream Air eye tracking through SteamVR + SBoys3 and native Pimax OpenXR; actual chat placement and driving comfort; sustained 5090 VR frame timing; 4090 capture/encoding cadence; real HDR source/canvas passthrough. DLSS 5 remains deferred research. These are live acceptance checks, not claims of completed hardware validation.
