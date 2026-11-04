# SteadyXR 0.16.0 validation

Local standalone release on `codex/0.16-quad-view-workshop`, based on the local 0.15 branch. It has not been pushed or published to GitHub.

## Changes

- Both views are composed directly at output resolution. Workshop source framing now includes the high-resolution focus image with the submitted relative orientation and FOV, rather than displaying only the peripheral image. Existing live/processed composition is retained.
- Live source status distinguishes peripheral + focus from a single received view, shows separate source dimensions, and avoids labeling retained data as fresh while paused, stale, or disconnected. Workshop shows original source dimensions separately from proxy size.
- Playback accepts bounded MP4 timestamp rounding while retaining unique image/pose association, exact frame counts, checksums and source metadata checks.
- Decoding permits bounded H.264 surface padding only with an exact validated display aperture; pixels outside the recorded image are removed without rescaling its FOV.

## Evidence

Both reported failures were reproduced before their fixes. The real recordings used a 30,000 Hz MP4 clock and differed from original metadata times by at most 33.2 microseconds. Their 2048x964 video decoded to a 2048x976 RGB surface with a 2048x964 display aperture. Their original files were not modified.

- Both real clips decode and render all 77 and 567 frames, with midpoint seeks passing.
- GPU fixture with a 64x64 peripheral image and patterned 256x256 focus image rendered at 512x512 verifies retained focus detail, full peripheral coverage, displaced focus FOV/orientation, both eye identifiers, Original/processed/source modes and transition to one view. Workshop source assertion failed before the fix.
- Native UI assertions cover fresh quad/single view, paused, stale, demo and window sources. The new status assertion failed before integration.
- Real RTX 5090 hardware encode/decode fixtures cover irregular timestamps with two views, full-FOV downscaling, 2048x964 padded output at 60 fps, source image/pose/gaze/speed association, seeking, rejection of a 10 ms metadata mismatch and existing checksum/resource-lifetime cases.
- Final Windows x64 Release CTest passed **42/42 in 54.01 seconds** on 2026-11-04. The app resource version is 0.16.0. Full results are delivered in `automated-tests.log`; separate hardware and real-clip logs accompany the package.
- Independent whole-branch review found no actionable Critical, Important or Minor issues.

## Component provenance

Application 0.16.0. Capture/IPC source remains unchanged from the 0.15 branch, which reused the signed 0.14.0 DLL. The packaged capture hash must be:

`74a6943f014d85b81f301da852b61d5a8cb2251a199130978bf4cbdba097d91d`

The packaging script checks its Authenticode signature and digest before creating delivery output. The source archive includes the corresponding original 0.14.0 capture source archive. Previous packages and clips are preserved. OBS binaries are unchanged.

## Remaining validation

No live mbucchia Quad-Views-Foveated/Sboys3 SteamVR session on Dream Air or Dream Air SE has been performed for this version. Synthetic OpenXR D3D11/D3D12 tests do not prove compatibility with a particular game or active layer chain. A single captured image may be normal stereo or already composed upstream. No game frame-time benchmark or physical wheel-button test was performed. Existing SDR-only recording and separate-overlay recording limits remain.

The native UI self-test passed. An additional interactive Workshop check was not completed because the computer-use launch helper reported `accessibility window-opened handler did not become ready`. The existing user-run 0.15 application was left open; decode/render/seek validation used the real recorded clips in an isolated automated harness.
