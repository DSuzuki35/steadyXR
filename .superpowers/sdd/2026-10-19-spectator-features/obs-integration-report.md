# OBS host integration report — 2026-10-19

Implemented the assigned Tasks 3/5 OBS slice. All feature toggles remain independently optional and false by default. CMake wiring and renderer work were owned by the root integrator.

## Delivered behavior

- Direct OpenXR source: descriptor-based spectator and visual controls; bounded native defaults/read/store, complete OBS scene JSON and shared INI import, persistent effects-only comparison scope; Learn/Cancel/Clear properties and native source hotkeys. Chat actions cross to the render thread through an atomically consumed command. Properties show capability, actual controller gaze state, countdown/progress/failure and learned-zone status. No new diagnostics are drawn into game output.
- Runtime gaze permission is requested only by the visible, unpaused, unexpired owning source, when spectator bypass is off and either gaze framing or explicit calibration is active. Cancel, bypass, pause and ownership release stop the request.
- Registered `steadyxr_visual_effects`, display name exactly **SteadyXR Visual Effects**. The separate filter uses shared visual controls, native persistence, shared INI import and explicit status refresh. Instructions explain applying effects in one location to avoid double processing.
- The enabled filter captures the source once into linear RGBA16F on the OBS graphics thread, runs the shared implementation on OBS's current D3D11 device/immediate context, copies the borrowed result into an OBS float texture, and presents linear premultiplied alpha with one SDR encoding. No live staging/readback, CPU pixels, extra device/context, video queue or source rerender. Compute output is never bound to a host shader SRV.
- OBS defers actual OM render-target rebinding after texrender ends. The host explicitly saves/unbinds/restores the actual RTV/DSV state around compute, avoiding an input/output binding conflict. Blend, framebuffer sRGB, texrender projection/viewport/matrix and touched native OM state are restored.
- Disabled/bypassed filter calls `obs_source_skip_video_filter`, creates no optional GPU resources/passes and frees previous resources. Inactivity, changed target/dimensions and unsupported HDR/color spaces clear optional resources. HDR fallback reports its reason and forwards upstream color-space negotiation. Existing standalone OBS image-filter expiry behavior remains unchanged by root instruction; direct capture retains its existing build-expiry gate.
- Updated direct-source version/help text to 0.10.0 / IPC 4.

## RED / GREEN evidence

1. Added all-field spectator/visual JSON/property/preset tests before settings integration. Native settings executable failed at `spectator toggle survives scene JSON and has a native property`, then passed after descriptor integration.
2. Added real-libobs standalone filter tests before registration. Native module executable failed at `standalone optional effects filter is registered under its exact display name` against the prior DLL.
3. Native render initially produced black effects output despite successful dispatches. Investigation of libobs texrender/D3D implementation established lazy OM binding as the cause. Explicit OM unbind/restore fixed it. Measured RGBA at +1EV: off `99 71 50 127`, enabled `137 99 71 127`; neutral enabled output returned to `99 71 50 127`.
4. Release build of `vr-stabilizer`, `vrst-openxr-module-tests`, `vrst-openxr-source-settings-tests` completed successfully with no compiler warnings after formatting.
5. Final command: `ctest --test-dir build-msvc -C Release -R '^obs-openxr-(settings|module)$' --output-on-failure`. Result: **2/2 passed**, settings 0.13 s, module 24.42 s, total 24.56 s. The scoped run follows the explicit instruction not to run whole CTest.
6. `git diff --check` passed for changed tracked OBS files.

## Coverage and limits

Settings tests cover every spectator/visual toggle and numeric descriptor, metering region, input transfer, alpha convention, comparison scope, JSON roundtrip, shared INI imports and legacy imports resetting formerly enabled new features to off. Existing reserve/overlay/import-read-only tests remain green.

Real libobs 32.2.2 module tests cover exact registration, all learning hotkeys, native buttons/status properties, default no-gaze polling, owner gaze enable, spectator bypass, explicit learning with framing disabled, cancel/clear/pause request release, single input render, disabled zero effects allocations/dispatches, RGB/alpha changes, neutral SDR equivalence, 3840×2160 output, HDR passthrough and negotiation, zero resources after off, native filter preset import and scene persistence. Existing native shared-frame RGB, source resize/eye/ownership/hide/reconnect/timeout/errors, twenty-second comparison and clean hotkey teardown tests continue passing.

Tests use a synthetic isolated producer and temporary libobs configuration. Installed OBS settings, registrations and user scenes were not modified. Test-process OBS startup reports host GPU scheduling/priority and optional `libobs-winrt` availability messages; these are environmental and do not fail the tests. Live Dream Air eye tracking, a real OBS HDR canvas and GPU timing under a game workload still require hardware acceptance testing. Shared compute temporal/graphics-state contracts are separately tested by the renderer/effects owner.

## Final-review HDR negotiation correction

Root review identified that the original HDR fixture ignored caller preferences. Local official libobs `obs-source.c`, `obs_source_get_color_space`'s `OBS_SOURCE_ASYNC` branch, returns the last preferred space if native HDR is absent. The original SDR-only probe could therefore select SDR conversion and conceal upstream HDR.

The fixture now implements the actual preference-selection loop and checks both `GS_CS_709_EXTENDED` and `GS_CS_709_SCRGB`. It demonstrates that SDR-only preferences select `GS_CS_SRGB_16F`, then verifies the filter performs one upstream render, reports unsupported HDR, retains zero effects resources and forwards native HDR negotiation. The new regression failed against the earlier filter at `preference-respecting native HDR input passes through once without effects resources` (RED, 1.31 s).

The filter now includes all four native SDR/HDR spaces in its detection probe and bypasses the two HDR results explicitly. Focused Release build (`vr-stabilizer`, `vrst-openxr-module-tests`) passed without warnings. `ctest --test-dir build-msvc -C Release -R '^obs-openxr-module$' --output-on-failure` passed **1/1 in 24.44 s**; `git diff --check` passed. Logs are `build-msvc/plugin/hdr-red.log` and `hdr-green.log`. This preference-respecting fixture replaces the earlier permissive HDR fixture; a real HDR canvas remains hardware acceptance scope.
