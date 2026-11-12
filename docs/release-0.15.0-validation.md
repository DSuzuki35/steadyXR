# SteadyXR 0.15.0 - native Capture Workshop

## Delivered behavior

The standalone app has English native navigation for Preview, Camera, Workshop and Settings, with advanced controls retained. Natural Head Movement is disabled for existing standalone profiles and removed from the controls; normal smoothing remains. Optional Pitch steadiness corrects up/down viewing angle and defaults to off.

Workshop records the complete submitted source rectangle of the selected OpenXR eye, including a separate focus view when supplied, before spectator cropping, stabilization and effects. Hardware H.264 proxies preserve source FOV and pair each accepted image with its pose, FOV, gaze and selected-game speed reading. Recording is opt-in, normally 30 seconds at 30 fps, with assignable keyboard/wheel input and no continuous recording buffer.

Editing pauses live output. A draggable frame with eight resize handles and synchronized controls maintains the configured output aspect ratio, normally 16:9. Drafts replay through the existing camera renderer, support Original comparison and apply explicitly to the active profile. See [archived Workshop guide](archive/capture-workshop-0.16.md) for operation and limits.

## Verification

- Windows x64 Release app and explicit test targets build without building the capture DLL target.
- Final post-review host CTest passed **42/42 in 42.23 seconds** on 2026-10-28. The complete log is `build-release014-verify/015-final-tests.log`. The 40% crop slider/save/reload regression also passes.
- Native UI tests cover navigation, settings, preset round trips, 125/150/200% scaling and keyboard focus on a 1280x720 work area. A regression for applying a dragged crop failed before the repair and passed afterward, including legacy trims and per-eye reserves.
- Crop geometry tests cover source/output aspect combinations, positions, boundaries, all eight resize handles and option/rectangle round trips. Camera tests cover optional pitch correction, yaw independence, recalibration and ignoring legacy Natural flags.
- Real RTX 5090 hardware recording tests encode and decode context/focus images, verify sequence/pose/gaze/speed association and corner pixels of a 4:3 source, and exercise scaling from 2560x1920 to 2048x1536, source gaps, bounded-queue overload, duration limits, recentering, cancellation, mono clips, sRGB formats and checksum rejection. These are synthetic GPU fixtures, not a measured game benchmark.
- Native Workshop interior dragging was exercised against a recorded fixture and changed the numeric X/Y controls while preserving crop size. The final additional manual UI pass was stopped by physical Escape; computer-use interaction stopped at that point. Subsequent validation uses automated tests and code review.
- Whole-branch review found four P2 issues: Apply omitted framing fields, recording resources outlived completion, Original could show stale state, and recording ignored the fixed telemetry game selection. The first, third and fourth fixes received targeted read-only review with no remaining P1/P2 finding. Resource cleanup is covered by nine hardware lifetime regressions (success/cancel/failure across UNORM and both sRGB formats), which return actual texture/device COM reference counts to baseline. Targeted read-only review also accepted the cleanup and 40% crop consistency fixes with no remaining P1/P2 finding.
- An incorrect capture DLL hash is rejected by the packaging script before any delivery output is created.

Initial sandbox execution denied temporary-file, registry and local-network operations. The host test run used an isolated temporary directory under the build folder; environment failures were not hidden by changing product behavior.

## Preserved capture component

Application version: **0.15.0**. Capture component version: **0.14.0**, copied from the existing signed release. Its SHA-256 is:

`74a6943f014d85b81f301da852b61d5a8cb2251a199130978bf4cbdba097d91d`

The source DLL has valid Authenticode verification. Capture/IPC source files are unchanged. The standalone package records the component versions and digest in `Components.json`; corresponding source also includes the original 0.14.0 capture source archive. Existing versioned deliveries are preserved.

## Remaining runtime limits

No live iRacing/LMU headset session, physical wheel-button test or game frame-time benchmark was performed for this version. Recording conversion, encoding and storage have a nonzero cost. There is no silent software-encoder fallback. HDR and separate overlay recording are rejected explicitly; learned chat-exclusion calibration remains a live-session check. Optional automatic clip analysis and simultaneous split comparisons were not added. OBS binaries are unchanged.

The new app is kept on the local `codex/0.15-ui-cropping-wizard` branch for the requested local build. The already-published 0.14 release remains on GitHub.
