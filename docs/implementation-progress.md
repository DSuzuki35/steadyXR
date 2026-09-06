# VR Stabilizer progress

The initial sections record the v0.1.0 delivery. The v0.2.0 additions are recorded at the end.

Design: docs/superpowers/specs/2026-09-05-vr-stabilizer-design.md
Plan: docs/superpowers/plans/2026-09-05-vr-stabilizer.md

- Workspace initially empty and not a Git repository; use this dedicated folder and a new feature branch.
- Prior conversation approves the architecture and the user now explicitly requests implementation. Continue autonomously.
- User confirmed Windows 11 and latest OBS. Target Windows x64 OBS 32.2.2.
- Core, plugin and sender share exact headers to avoid interface drift. Root handles core/integration; one implementation worker at a time may handle a separate component while root does useful independent work.
- No live Windows/Pimax/Elgato environment available locally. Native macOS OBS is installed; LLVM-MinGW was downloaded for Windows cross-compilation.

## Implemented

- Portable quaternion/projection/smoothing core, bounded pose history, 90/60 frame selection, UDP protocol validation and clock synchronization.
- SteamVR background sender with observed frame poses, live fallback, selected-eye rotation/projection, reconnect and synthetic demo mode.
- OBS effect filter with all requested controls, input trim, final 16:9 framing, correction limiting, tracking-loss bypass and diagnostics.
- Windows x64 DLL/EXE, installation and launch scripts, complete source package, reproducible build scripts, dependency notices and CI workflow.
- README covers two-PC installation, calibration, mirror/FOV uncertainty, single-eye source alternatives and hardware limitations.

## Verification

- Core tests cover projection/rotation, temporal smoothing, 90/60 selection, malformed packets, clock offsets, tracking gaps and exact 16:9 dimensions after input trim.
- Sender math tests cover matrix-to-quaternion conversion, eye composition, tracking validity and frame/live transitions.
- Real local UDP tests exercise clock synchronization between simulated different PC clocks, pose delivery, stale data, reconnect, sender CLI, subscriber limits and expiry.
- Six actual OBS OpenGL shader cases pass with zero pixel mismatches: identity, trim, yaw, final framing, correction limiting and asymmetric FOV.
- Native OBS module smoke testing verifies registration, all 19 controls, callbacks, output dimensions, three complete filter renders, enable/disable and destruction. The installed OBS is 32.1.2, so that full module test used a temporary build against OBS 32.0 headers. The release package is compiled against OBS 32.2.2; OBS correctly rejects it on the older local installation.
- Actual Windows x64 binaries are linked using LLVM-MinGW. PE imports are inspected; OpenVR is packaged beside the sender, and no separately distributed C++ runtime is needed.
- Independent core/OBS and delivery reviews were performed. Review fixes include exact 16:9 output dimensions, selected-pose age wording, OpenVR DLL staging for Windows CTest, platform-correct integration-test cleanup and asymmetric OpenVR projection conversion.

## Practical limits

- Hosted CI subsequently built with Visual Studio and executed all four test suites successfully on Windows Server 2022, including the actual sender UDP integration. Linux CI also passed. Live Windows 11 and OBS plugin execution on Windows remain unverified. The Windows integration test uses bounded force termination for cleanup; graceful Windows console shutdown still requires a console check.
- Live SBoys3/Dream Air/Elgato/iRacing/LMU validation and empirical delay/FOV calibration require the user's two PCs. No claim of exact HDMI frame/pose identity is made.
- Stabilization corrects rotation only. Unknown game mirror cropping, missing pixels and 90-to-60 video cadence remain constraints.

## Final delivery checks — 2026-09-05

- All four CTest suites pass on the final source tree: 22 core cases, 18 sender math cases, sender UDP/CLI integration and receiver network integration.
- A fresh build from the packaged source also passes all four suites.
- Final Windows cross-build succeeds with the OpenVR runtime staged beside the executable. Both project binaries are AMD64 and their packaged bytes match the build outputs.
- ZIP CRC and all 66 SHA256 manifest entries pass verification.
- The delivery reviewer closed all three P2 findings after focused re-review. No unresolved review blocker remains.
- Initial local artifact: `dist/VR-Stabilizer-0.1.0-Windows-x64.zip`, built on `feat/vr-stabilizer` before publication.

## GitHub delivery

The user subsequently requested a new private GitHub repository. **HorizonLock VR** was created at `https://github.com/DSuzuki35/horizonlock-vr`, verified private, and the source pushed to `main`. Both jobs in CI run `33990785936` passed. The Windows package is delivered as an experimental release. Live hardware calibration remains outside the available environment.

## v0.2.0 — image microjitter and horizon leveling

The user authorized both additions within a one-hour window starting about 21:08 UTC on 2026-09-05. Development uses the isolated `feat/image-microjitter` worktree. Design and plan: `docs/superpowers/specs/2026-09-05-image-microjitter-design.md` and `docs/superpowers/plans/2026-09-05-image-microjitter.md`.

- Image mode estimates subpixel translation/roll from selected image features without tracking, FOV or clock calibration. Its two-slot GPU pipeline applies each correction to the exact analyzed frame, adding one OBS video frame. All crop/framing controls remain available.
- Pose mode adds independent horizon strength and maximum correction. It removes sustained headset roll, preserves look direction and fades near vertical gaze. The existing crop limiter still applies. Game-car roll is not inferred from the headset.
- 23 image checks and 28 core checks pass, including deliberate-pan following, jitter reduction, ROI isolation, brightness changes, scene cuts and leveling. ASan/UBSan checks pass. Independent review also ran 400,000 randomized horizon/warp checks and verified combined translation/roll sequences.
- Review found and fixed an ROI-boundary sampling leak. A dedicated regression proves moving content outside a flat selected region cannot inject correction. Patch brightness offset is removed during optical flow to prevent lighting changes from becoming motion.
- Eight new image GPU pixel cases have zero mismatches. Native OBS module tests verify exact one-frame pairing, one capture per tick, repeated renders, mode/size/trim/ROI/reset/visibility handling, full 3840×2160 output with at most 480×270 analysis, and synthetic jitter suppression. Local module checks use OBS 32.0-compatible headers for the installed 32.1.2 app; Windows release headers remain 32.2.2.
- Synthetic end-to-end pixel temporal energy fell to about 0.1% of the smoothing-off test (approximately 96.8% RMS reduction). This is a controlled generated sequence, not a measured result from Dream Air/racing footage. Native estimator diagnostics were under 3 ms in tested cases.
- Windows cross-compilation succeeds. Both independent reviews are clear. Final review also caught and fixed transparent-source overbrightness; a real OBS regression confirms matching RGB/alpha in pose, image warm-up and image delayed output. Local five-suite CTest, all 14 GPU pixel cases and the complete native module suite pass on the reviewed source.
- Windows and Linux jobs in final release CI run [33993937329](https://github.com/DSuzuki35/horizonlock-vr/actions/runs/33993937329) passed. Its native Visual Studio package passes ZIP CRC and all 78 manifest checksums; all 56 packaged source files match release commit `69efee6` after line-ending normalization. Both project binaries are AMD64 and need no separate C++ runtime.
- [v0.2.0](https://github.com/DSuzuki35/horizonlock-vr/releases/tag/v0.2.0) was published as an experimental release at 21:47 UTC, within the requested hour. The repository remains private. The release tag targets `69efee6`; GitHub's uploaded ZIP digest matches the locally verified SHA256 `1dce877776dd8b75f8005387de1e549dc2cde53b5ef5032cce011fb082712a5a`. The download includes binaries, source, notices and installation/calibration instructions, with a separate checksum asset.

## v0.3.0 — streamer tools

The user accepted guided calibration, hybrid stabilization, per-axis motion, matched A/B/presets and automatic crop, excluding the proposed visual tracking mask. Work uses the existing isolated worktree on `feat/streamer-tools`, starting from `af5829e`. Design and plan are under `docs/superpowers/` dated 2026-09-06.

- Advanced math adds independent axes, signed sustained-turn following, and bounded crop growth/relaxation; all defaults preserve v0.2 behavior. Its 36 checks plus existing 28 core/23 image checks pass, including sanitizers.
- Calibration fits positive effective projection gains against 0–1000 ms pose alignment and refuses unreliable/ambiguous data. Independent review exposed fine/coarse search bias in periodic motion; the fix evaluates all candidates at the same 1 ms resolution. All 24 solver cases and sanitizers pass; formerly accepted aliases are rejected. Near-cap input solves in about 186 ms on the development Mac, not a Windows performance guarantee.
- The collection session uses local capture timestamps rather than OBS's potentially different frame epoch or analysis time. Eighteen session checks include complete 83 ms recovery across distinct clock epochs, deduplication, duration bounds, source invalidation and stale-result protection. Real UDP tests verify remote-to-local pose timestamp conversion.
- Hybrid stores each frame's pose warp, measures residual on the pose-corrected picture and composes final sampling. Eighteen actual GPU shader cases and the buffered pipeline suite pass, including matched A/B, horizon, 4K, alpha and automatic crop. A mutation removing pose correction from analysis fails the intended residual-motion test.
- OBS controls, named preset callbacks/serialization and invalid-calibration guards pass native module tests. A regression where hidden pose settings reset image-only buffering was caught and fixed with separate processing/calibration signatures. All legacy image module checks pass afterward.
- Integration review found and fixed three stale-calibration paths: invalidation now discards observations so Finish cannot revive them; source identity/size is checked again before Apply; detected scene cuts invalidate both active collections and pending suggestions. Pose-only mode cannot detect later content-only view changes and documents the need to recalibrate. Actual OBS testing also exposed private-filter hotkey registration rejection. UUID-scoped frontend hotkeys now register correctly, save/load their bindings with each filter, trigger the real callbacks and unregister on destruction.
- Independent math, GPU, integration and whole-branch reviews are clear. The final Windows cross-build and all eight local CTest suites pass; the 18-case session suite additionally passes ASan/UBSan. Native Windows/Linux CI [34012655841](https://github.com/DSuzuki35/horizonlock-vr/actions/runs/34012655841) passes on release commit `aa771ce`. An MSVC-only lambda-capture issue in the test fixture was corrected before the successful build.
- [v0.3.0](https://github.com/DSuzuki35/horizonlock-vr/releases/tag/v0.3.0) was published at 04:57 UTC on 2026-09-06. The repository remains private. The native Visual Studio ZIP passes CRC, all 94 manifest checksums and comparison of all 72 corresponding source files after line-ending normalization. Both binaries are AMD64; required imports are supplied by OBS, Windows and the packaged OpenVR runtime. GitHub's uploaded digest matches local SHA256 `3e5c3d2831bab38821406fff540cd1d2f50dce31f9d3480f00ae23b0e9a38df4`.
- Live Windows D3D, Dream Air/SBoys3, Elgato and racing-footage quality remain unverified. No unresolved implementation/review blocker remains; hardware calibration is a documented limitation.

## v0.3.1 — image tracking continuity and calibration cadence

- Investigated a user report of hard jumps in image/hybrid modes with stable pose-only output and rejected delay calibration. New regressions reproduced abrupt correction loss, exposure-related match rejection, and calibration failure on repeated video frames. These fixtures identify software failure modes; they do not establish the cause of every jump in the user's live capture.
- Weak image matches now release the previous correction over 150 ms; accepted tracking ramps back over 100 ms. Raw motion remains separate for calibration. Exposure offsets are removed from patch validation. Motion-tolerant scene comparison avoids false cuts on combined movement/contrast loss, while unrelated scenes still reset immediately. Automatic crop reserves space for fading corrections.
- Calibration compares sliding approximately 60 ms motion windows, retaining original pose intervals and downweighting overlapping support. The full 0–1000 ms search and rejection thresholds remain. Synthetic 30/60 repeated-frame and 90/60 jitter/noise recordings recover known delay within 3 ms and FOV within 5 degrees. Fitted results also report axis correlation, fit error and collected observation rates.
- The local LLVM-MinGW Windows build against OBS 32.2.2 passes all eight CTest suites, including 32 image, 37 advanced, 28 calibration and 19 collection-session checks plus real UDP sender/receiver checks. An independent review's 48-case motion/contrast sweep reports no false scene cuts after the review fix.
- Actual Windows OBS 32.2.2 / D3D11 module tests passed twice on the gaming PC with synthetic sources. The image suite reports zero failures; the streamer suite passes all 25 checks. Alternating-texture jitter energy ratio is 0.0010. Buffering, jitter and alpha checks use 60 fps; full-pixel 4K verification uses 30 fps because twice-per-frame harness readback exceeded 16.7 ms on some runs. This verifies rendering behavior, not live 4K60 performance or racing quality. macOS test initialization is preserved but was not run on this Windows host.
- The experimental local package includes complete corresponding source and an `obs-install` drop-in folder for existing manual installations. The gaming-PC sender protocol remains compatible. Live iRacing comparison and a new parked-car calibration remain the next hardware checks; no v0.3.1 release has been published by this task.
