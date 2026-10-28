# SteadyXR 0.11.0 spectator features

Standalone 0.15.0: open **Camera → Gaze & effects**. Natural Head Movement and its Motion page have been removed from the standalone app; normal Smoothing is used. The Motion descriptions below apply only to older OBS binaries. See [Capture Workshop](workshop.md) for the new recorder and optional pitch steadiness.

Version 0.12.0 adds the optional **Director** page: lookahead, automatic crop and iRacing/LMU speed framing. See [Viewer camera controls](viewer-director.md) for setup, video delay, memory budget and two-PC limits.

Every new feature is optional and off by default, including when loading old settings. Existing stabilization stays available. The master bypass disables the new camera and image processing without erasing tuning. DLSS 5 remains a research item in TODO.md.

## Lowest gaming-PC overhead

Use gaze framing and camera stabilization on the gaming PC. Leave its image effects off, then add **SteadyXR Visual Effects** to the capture-card source in stream-PC OBS. The stream PC needs no eye-tracking connection for image effects. Local preview and direct OpenXR OBS capture can also apply the same effects when wanted. Avoid applying them twice unless intentional.

Disabled image effects add no GPU passes or textures. The layer queries eye state only when its active consumer requests gaze framing or chat learning. Metering and bloom stay on GPU, with no image/statistics readback or blocking GPU query in live processing. Initial enable can cause a one-time shader compilation/allocation cost. No extra game rendering area is enabled by these features.

## Gaze and chat

Open **Spectator & effects → Gaze & chat** in the preview, or the spectator groups in the direct OBS source. Enable gaze framing and tune strength, fixation dwell, neutral zone, horizontal/vertical travel and response. Framing stays within the captured image margins. The **Motion** page controls natural stabilization independently; its suppression, turn response, settling and per-axis controls balance jitter suppression against head following.

Enable chat exclusion, then press **Look at chat now / Learn** (OBS: **Learn chat position**) (standalone default **Ctrl+Alt+C**, configurable native OBS hotkey). Look at chat during the three-second countdown and one-second acquisition. Stable samples learn a center direction; width/height are adjustable angular bounds. This does not infer a panel's physical size or depth. Looking there removes the gaze contribution smoothly while stabilized head following continues. Cancel and Clear are available.

The learned direction is valid for the current tracking reference only. Relearn after recentering, changing eye/source/game session, loading a preset or moving chat. Presets save tuning, never a live world anchor. The countdown advances with captured frames. Unsupported, untracked or stale gaze falls back to ordinary head stabilization; a runtime with unknown eye timestamps can frame but cannot reliably learn a chat anchor.

The layer uses `XR_EXT_eye_gaze_interaction` when advertised and coexists with the game's action attachment/sync lifecycle. Install the 0.10 connection and restart the OpenXR game for IPC 4. SteamVR + SBoys3 and native Pimax OpenXR require live Dream Air validation; fake-runtime tests do not establish hardware compatibility. A supported headset alone does not guarantee the active runtime exposes usable gaze.

## Exposure, bloom and color

The **Exposure**, **Bloom** and **Color** pages have separate optional switches. Manual exposure stays fixed; automatic adaptation responds to the metered brightness over time. Leaving a dark tunnel can briefly brighten the view before it settles; entering shade can initially darken it before gain rises. Bloom continuously follows bright highlights, and color grading is a fixed adjustment. Adaptation has bounded gain and separate brightening/darkening rates. Transition damping restrains abrupt brightness transitions; it does not generate an artificial white flash. Bloom uses threshold/soft knee and a reduced-resolution blur. Temperature, tint, contrast, saturation, shadows and highlights are adjustable. Filmic is an explicit opt-in preset.

Local/direct-source effects run after camera framing and before separately captured OpenXR overlays. Metering excludes known letterboxing and mirrored borders and intersects the user-selected metering region. On a capture-card input, baked-in HUD/chat cannot be separated; use the region controls to reduce their influence. These are SDR effects: they cannot recover clipped highlights. The OBS filter passes unsupported HDR color spaces through with a status explanation.

Choose image-effects comparison scope for the existing twenty-second A/B: original, split, or fitted side-by-side. Both sides use identical camera geometry. Camera comparisons retain their existing purpose. Comparison activity and calibration are transient; tuning persists in complete local presets and OBS settings/imports.

## Validation and measured cost

Native D3D11 tests cover neutral transfer/alpha, exposure/adaptation, bloom, resource release, overlay ordering, same-frame comparisons and GPU lifecycle. Fake OpenXR tests cover capability and action lifecycle; portable tests cover fixation, exclusion, calibration and motion. See the release validation report for the final complete-suite result.

Synthetic RTX 5090 measurements at 3840×2160 (effects core only, 59 GPU timestamp samples after warm-up):

| Mode | Mean GPU time | Compute dispatches | Effects textures |
| --- | ---: | ---: | ---: |
| Disabled | no GPU work | 0 | 0 |
| Manual exposure | 0.075 ms | 1 | 63.3 MiB |
| Color | 0.075 ms | 1 | 63.3 MiB |
| Bloom | 0.159 ms | 4 | 71.2 MiB |
| All effects | 0.169 ms | 5 | 71.2 MiB |

Repeated all-effects runs measured 0.168–0.187 ms as GPU clocks/load varied. These exclude host composition/copies, shader warm-up, OBS encoding and VR-game load. Effects comparison adds an original-image copy; normal preview effects use the existing output as input. No RTX 4090 or sustained live VR/encoding benchmark was performed. The numbers do not establish end-to-end latency or guaranteed GPU headroom.

The deterministic camera fixture measured about 0.64 microseconds per update with gaze/natural enabled, including fixture generation. At a sustained 45°/s head turn, natural-mode lag was 1.03° versus 9.39° for the existing smoother in that fixture. Alternating ±1° jitter residual was 0.028° versus 0.026°: both strongly suppressed it, with no universal claim that natural mode is smoother. Live comfort and actual runtime gaze behavior still need headset testing.
