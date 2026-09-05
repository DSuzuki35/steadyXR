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
- Windows cross-compilation succeeds. Final independent delivery review, hosted CI and package verification are completed before publishing v0.2.0.
