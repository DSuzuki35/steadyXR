# Streamer tools design

The user approved suggestions 1, 2, 3, 5 and 6 and excluded the visual tracking mask. This is an architectural extension of v0.2.0. Keep the existing four-margin image region and all saved scene defaults; no native OpenXR provider or game telemetry is added. Target Windows 11 x64 / OBS 32.2.2, Dream Air through SBoys3/SteamVR for pose, any captured SDR video for image mode.

## Processing

Add mode 2 (hybrid): capture raw video once, retain the associated pose correction with that slot, analyze the pose-corrected picture, then apply only residual image correction. Final rendering composes image and pose sampling into the raw texture. Allocate the requested stabilization crop between the stages multiplicatively (each keeps the square root of the total keep fraction). Pose stage applies input trim; final image stage applies framing and output aspect. Analysis stays <=480x270; GPU full-resolution storage remains bounded. Hybrid, like image mode, has one video frame latency. Missing tracking yields image-only residual processing and fresh history at tracking transitions.

A/B comparison always runs the estimator and crop controller. Only final display uses identity corrections, with identical crop, frame and dimensions. It does not change the algorithm, latency or settings. Expose a checkbox, OBS hotkey and reset hotkey. Named presets persist per filter in OBS scene data, including game/car tuning; they do not overwrite the gaming PC address or port. Save/load/delete operate on explicit names; next-preset hotkey cycles saved names.

## Motion and crop

An optional separate-axis setting overrides the common amount: yaw/horizontal, pitch/vertical, roll. A bounded signed-velocity follower accelerates intentional sustained turns; its amount defaults to zero and all advanced controls default off so v0.2 behavior is preserved. Quaternion processing must preserve normalization and handle resets, gaps and recentering.

Automatic crop starts at the configured stabilization crop, can add at most a configured extra amount, never exceeds the existing 40% total crop limit, and changes at a configured percentage-point rate with slower relaxation. Safety still comes from the existing correction limiter, so sudden movement never forces an instant zoom. It follows actual requested correction even in A/B mode. Reset/size/trim/mode changes clear its state. Both processing algorithms and hybrid support it.

## Guided calibration

A start/finish/apply flow collects up to 30 seconds of raw image motion and timestamped headset poses while the car is parked. During collection, display a normally framed raw buffered view and instruct varied small left/right followed by up/down movements. The receiver remains connected even if the saved mode is image. Export raw pose observations with clock-corrected local timestamps; video observations use their captured slot's local monotonic time, never analysis completion time.

The portable solver searches delay 0..1000 ms, interpolates orientation at both ends of each image-motion interval, fits positive horizontal/vertical scale and converts those gains into effective FOV. Reject weak texture, insufficient movement on either axis, tracking gaps, poor correlation, wrong signs and ambiguous delay peaks. Results are suggestions with confidence, supporting sample count and explicit failure reason. Apply only a valid result on user action; disable automatic full-eye FOV when applying. Never silently change settings on completion. FOV is an approximation for the captured mirror, not a measurement of headset optics. Scene/settings/source changes cancel collection or invalidate stale suggestions. Bound all storage and computation.

## Verification and delivery

Use synthetic motion tests before implementation; preserve all v0.2 suites. Add tests for per-axis isolation, deliberate turns, crop rate/bounds, calibration delay/FOV recovery and rejection. Actual OBS module tests must verify hybrid frame/pose pairing, matched A/B pixels, presets and callbacks, repeated renders and resets. Test shader coordinates on actual macOS OBS GPU, cross-build Windows, run native Windows/Linux CI. Publish source and experimental v0.3.0 in the existing private repository after passing checks. Document live Dream Air/Elgato/Windows D3D calibration as unverified until hardware testing.
