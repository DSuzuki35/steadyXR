# SteadyXR 0.12.0 verification

Verified 2026-10-20 on Windows x64 with native MSVC/D3D11.

## Build

- `cmake --build build-msvc --config Release --parallel 4`: successful.
- Version 0.12.0 in the app, resources, package default and shared build stamp.
- Built: `2026-10-20T06:53:09Z`.
- Fixed 30-day expiry: `2026-11-19T06:53:09Z`.
- Existing double-to-float shader parameter conversion warning remains; no build errors.

## Tests

`ctest --test-dir build-msvc -C Release --output-on-failure`:
**37/37 passed**, 59.54 seconds in the final full run.

Coverage includes:

- Native GPU delayed background/overlay association, slot reuse, repeated draw,
  recenter, bypass, disable, source wait/change, window lookahead bypass.
- Moving-pose/nonuniform-image A/B confirms the future target changes actual
  rendered geometry while retaining the same video delay.
- Warmup target remains fixed while the first image is held; next displayed
  frame uses future poses. Memory capacity includes all retained texture slots.
- Quaternion antipodes, final projection/affine auto-crop bounds, maximum crop,
  correction fallback, release timing, speed response, stale data and A/B.
- Combined Natural movement and lookahead: future motion influences Natural
  while actual captured pose continues to align gaze.
- iRacing dynamic variable parsing/torn reads, LMU player selection and native
  synchronization, 20 Hz polling, 1 Hz retry, 500 ms stale expiry, cleanup.
- Standalone settings/presets, OBS scene settings/import, UI navigation and input.
- Existing core, capture, renderer, visual effects, layer, build expiry,
  sender, network and native OBS module tests.

The first renderer regression failed before buffering was implemented.
Independent camera review found Natural movement discarded the future target;
its new regression failed before the fix and passed afterward. The first full
suite found the package fixture missing the two new guides; the fixture and
archive assertions were updated before the final passing run.

## Review and UI

Telemetry/UI review checked the LMU offsets and synchronization against the
installed SDK and found no actionable issue. Final broad review found no P1/P2
defects; the reported packaged guide-link mismatch was corrected.

The native UI self-test generated `preview-director-test.bmp`. Visual inspection
confirmed the six tabs, all controls, status, audio-sync guidance and footer fit
without clipping. Main UI self-tests preserve settings while changing pages.

## Packaging

Release artifacts:

- `SteadyXR-0.12.0-Standalone-Windows-x64.zip`
- `SteadyXR-0.12.0-OBS-Windows-x64.zip`
- `SteadyXR-0.12.0-Source.zip`
- SHA-256 sidecars and per-package checksums.

The standalone community package excludes OBS binaries, the sender and the
development/build tree; it retains required layer-editor runtime helpers and
license notices. Both runtime packages reference the matching source archive.
Versioned previous releases are preserved. Ready app settings are copied
unchanged; delivery does not edit OBS settings or register an OpenXR layer.

## Limits of this validation

No live iRacing/LMU driving session or Dream Air hardware test was performed.
LMU support is for the verified installed SDK layout; future ABI changes need
another check. Native GPU tests establish functionality, not game FPS or a
measured performance budget under gameplay load.

The 256 MiB history budget counts texture payload, not driver overhead.
Lookahead adds consumer GPU copies and video delay. Auto-crop and speed framing
use the existing draw. Remote HDMI video does not carry XR poses or telemetry.
