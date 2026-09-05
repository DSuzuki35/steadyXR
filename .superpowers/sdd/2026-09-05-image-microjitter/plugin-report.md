# OBS image integration report

Implemented in `feat/image-microjitter`, isolated `vr-stabilizer-image` worktree. Owned files: plugin image pipeline/header/effect, obs_filter.cpp, locale, plugin CMake; dedicated native module test.

## Behavior

- `mode=0` remains SteamVR pose by default; `mode=1` is Image-based microjitter. Image mode disconnects the receiver while retaining the saved host.
- Shared smoothing, input trim, crop, framing and aspect settings. Image-only limits and four ROI margins; opposing margins keep at least 6% to satisfy core validation. Pose-only network/FOV/delay and horizon controls are hidden in image mode.
- Horizon strength 0–100% (default0), limit0–60° (default45), calls `horizon_correction(source_orientation, target_orientation, ...)` before existing crop-aware pose warp.
- Two full-resolution GPU RGBA captures, two small render targets and matching small stages. Analyze preceding stage and render the same preceding full texture. First frame identity; repeated renders reuse the exact slot/warp. Filtered GPU downsample uses16 bilinear samples, bounded at480×270 and respecting trimmed aspect ratio. The plugin never reads full-resolution video back to CPU.
- Mode/size/trim/ROI/reset/enabled changes invalidate image history and buffers. Enable signal catches disable+enable between ticks. Missed rendering ticks invalidate captured-slot flags and history so resume cannot display video older than one frame; GPU allocations remain reusable. Null/zero-sized targets request reset.
- Weak or failed analysis uses identity correction on its matched video. Capture/allocation exceptions clear image state; shader-based fallback preserves image-mode framing. UI callbacks only publish config or atomic reset flags; graphics lifecycle remains in graphics context.

## Native build and actual OBS checks

Installed OBS32.1.2, Apple M5 Max, OpenGL4.1 Metal. Native module built against compatible OBS32.0 headers; the normal module API gate remained enabled. Both shaders compiled through actual OBS module creation.

Configure:

```sh
cmake -S . -B build-plugin-native -DCMAKE_BUILD_TYPE=Release -DVRST_BUILD_PLUGIN=ON -DVRST_OBS_SOURCE_DIR=/tmp/vrst-obs-plugin-check/obs-studio-32.0.0 -DVRST_OBS_GENERATED_DIR=/tmp/vrst-obs-plugin-check/include -DVRST_SIMDE_DIR=/tmp/vrst-obs-plugin-check/simde-0.8.2 -DVRST_OBS_LIBRARY=/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs
cmake --build build-plugin-native --parallel 4
clang++ -std=c++17 -Wno-deprecated-declarations -Iinclude -I/tmp/vrst-obs-plugin-check/obs-studio-32.0.0/libobs -I/tmp/vrst-obs-plugin-check/include -I/tmp/vrst-obs-plugin-check/simde-0.8.2 tests/image_module_tests.mm /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks -o build-plugin-native/vrst-image-module-tests
build-plugin-native/vrst-image-module-tests build-plugin-native/plugin/vr-stabilizer.plugin/Contents/MacOS/vr-stabilizer plugin/data
```

Verified against core commit `b585cca`:

- Dedicated real module suite: **0 failures**. Logs in ignored `build-plugin-native/image-module.log`.
- Exact source frame color pairing across warm-up and subsequent frames, repeated renders identical and source capture at most once per tick.
- Every output pixel opaque and fully covered, including 4K output, asymmetric trim and source resize.
- Mode property visibility; diagnostics explicitly show one-frame delay; FOV and tracking-delay inputs do not influence image output.
- Reset, ROI/trim/mode/size changes, immediate disable+enable, and skipped render ticks all restart with the current frame.
- End-to-end GPU capture/downsample/stage/core/warp test using known alternating ±2px textured source: temporal pixel energy33,778,590 with smoothing0 vs34,357 with smoothing100; ratio0.0010, approximately96.8% RMS reduction. Final tracking97/97 inliers,97% confidence,0.9ms estimator time at360×270.
- 3840×2160 video retained full output dimensions with480×270 analysis,100/100 inliers and0.6ms estimator time. Flat input after texture loss showed correct uncorrected matching video.
- Original `/tmp/vrst-obs-plugin-check/module_smoke` verifies prior pose path, controls/defaults, trim/framing, disabling and destruction.

The synthetic test initially omitted OBS's mandatory source destroy callback, causing teardown to call a null function; adding the callback resolved it. Later, strict skipped-tick handling exposed that a test-side explicit wait plus queued graphics task skipped a video tick; removing that redundant wait makes capture occur on successive OBS ticks and all assertions pass.

## Boundaries for root review/delivery

- Native tests exercise SDR RGBA source/filter behavior. HDR handling, live racing footage and real Windows D3D rendering remain unverified by this worker.
- Memory/staging allocation failures have identity fallback paths but were not force-injected on the real driver.
- Root owns affine shader GPU tests, CI/package integration and Windows release validation. This report does not claim racing footage quality from synthetic measurements.
