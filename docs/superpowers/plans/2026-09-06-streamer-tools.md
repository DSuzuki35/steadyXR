# Streamer Tools Implementation Plan

> For agentic workers: use superpowers:subagent-driven-development for isolated implementation and review. User already approved these features; execute continuously.

**Goal:** Deliver guided calibration, hybrid stabilization, per-axis motion, matched comparison/presets and automatic crop in private v0.3.0.
**Architecture:** Portable math and calibration modules support a slot-paired OBS pipeline. Root owns OBS controls and integration, workers own disjoint math, solver and GPU files.
**Tech Stack:** C++17, OBS 32.2.2 C API/effect shaders, CMake, Windows x64, macOS GPU verification.
**Spec:** ../specs/2026-09-06-streamer-tools-design.md

## Global Constraints

Preserve v0.2 scenes and defaults. No visual tracking mask. SDR pipeline, <=480x270 analysis, one buffered frame for image/hybrid/calibration. Native OpenXR and telemetry remain outside scope. No changes to the UDP wire format. Existing private GitHub delivery remains authorized.

## Task 1: Motion and crop core

Own include/vrst/core.hpp, src/core.cpp, include/vrst/image.hpp, src/image.cpp, new include/vrst/advanced.hpp and src/advanced.cpp, tests/advanced_tests.cpp. Root owns CMake changes.

Interfaces:
```cpp
struct MotionSettings { double amount{40}; bool separate_axes{}; double yaw{40}, pitch{40}, roll{40}; double turn_follow{}, turn_speed{90}; };
// PoseHistory overload preserves set_smoothing(double).
void PoseHistory::set_smoothing(const MotionSettings &);
struct CropSettings { bool enabled{}; double max_extra{10}, speed{3}; };
class CropController { public: void reset(); double update(double required, double base, const CropSettings &, std::uint64_t time_ns); };
double required_pose_crop(unsigned, unsigned, const Settings &, Quat, std::optional<Projection>, double max_extra);
double required_image_crop(unsigned, unsigned, const Settings &, const ImageResult &, double max_extra);
```
Add ImageSettings.motion; its amount comes from existing ImageSettings.smoothing. Add ImageResult.motion (measured previous-to-current image transform), time_ns and previous_time_ns; populate measured motion only on tracking status.

- [x] Write failing independent-axis, turn-follow, crop rate/clamp/idle/invalid/reset tests; run against stubs.
- [x] Implement quaternion local-axis smoothing with exact legacy path when advanced controls off. Expose measured image motion before smoothing and implement image-axis/follow behavior.
- [x] Implement crop requirement search and rate controller; prove zero-extra/disabled identity and all corners safe after limiting.
- [x] Run advanced, core and image tests and sanitizers; commit owned files and report.

## Task 2: Calibration solver

Own include/vrst/calibration.hpp, src/calibration.cpp, tests/calibration_tests.cpp. Root owns CMake and UI.

Interfaces:
```cpp
struct CalibrationPose { std::uint64_t time_ns{}; Quat orientation; };
struct CalibrationMotion { std::uint64_t previous_time_ns{}, time_ns{}; double x{}, y{}, confidence{}; }; // shifts / analysis width and height
struct CalibrationResult { bool valid{}; double delay_ms{}, horizontal_fov{}, vertical_fov{}, confidence{}; unsigned samples{}; std::string reason; };
CalibrationResult calibrate(const std::vector<CalibrationPose>&, const std::vector<CalibrationMotion>&);
```
- [x] Create irregular yaw/pitch synthetic trajectories and normalized projected shifts delayed by 83 ms; assert recovery within 3 ms and 5 degrees.
- [x] Add rejection tests for static, one-axis, noise, wrong direction, gaps, ambiguous periodic motion and invalid samples; run failing stubs.
- [x] Implement bounded full-resolution 1 ms delay search with quaternion interpolation, positive gain fit, support/quality/excitation gates and clear reasons.
- [x] Run focused tests/sanitizers and record accuracy/runtime, then commit owned files.

## Task 3: GPU pipeline

Own plugin/image_filter.hpp/.cpp, plugin/data/image-stabilize.effect, tests/streamer_gpu_tests.mm or focused additions to image GPU/module tests. Do not edit plugin/obs_filter.cpp.

Extend ImageFilter with:
```cpp
struct ImageRenderOptions { bool hybrid{}, comparison{}; vrst::PoseResult pose; std::optional<vrst::Projection> automatic; vrst::CropSettings crop; };
// Existing render signature gains const ImageRenderOptions &options = {}.
std::uint64_t ImageFilter::capture_time_ns() const; // currently displayed slot's local monotonic timestamp
// result() exposes measured motion and image timestamps through Task 1 additions.
```
Keep existing callers valid. Save each slot's pose warp at capture; estimate residual on safe pose-warped pixels and compose final GPU transforms. Split stabilization crop multiplicatively. Comparison zeroes both corrections at draw only; output crop follows stabilized control path. Expose effective crop diagnostics if useful. Preserve legacy pure-image path.

- [x] Extend actual OBS/GPU tests with known pose/image movements, two differently colored frames, matched comparison and a nonidentity hybrid transform; watch failure.
- [x] Implement paired hybrid analysis/draw, comparison, bounded automatic crop and transition resets.
- [x] Verify repeated renders, alpha, 4K output, no pose double correction and exact one-frame pairing; commit owned files.

## Task 4: Controls, receiver, presets and calibration integration (root)

Own plugin/obs_filter.cpp, plugin/presets.hpp/.cpp, plugin/data/locale/en-US.ini, include/vrst/receiver.hpp, src/receiver.cpp, CMake files, tests/streamer_module_tests.mm.

- [x] Add Receiver snapshot overload accepting MotionSettings; expose mapped local_pose_time_ns in TrackingSnapshot and use existing raw orientation/time for calibration observations.
- [x] Add mode 2; pass matched pose and auto-crop settings into ImageFilter. Add axis/follow controls and matched comparison checkbox/hotkeys without estimator reset.
- [x] Implement named preset save/load/delete plus cycle hotkey, persist only explicit tuning keys and avoid copying nested preset storage or network address.
- [x] Build start/finish/apply calibration UI with bounded collection, raw buffered video, deduplicated pose and image times, solver result/error display and stale-result invalidation.
- [x] Exercise real OBS callbacks, preset round trip, A/B timing and calibration apply safeguards with native module tests; run full portable and GPU suites.

## Task 5: Review and release

- [x] Independent focused reviews of motion/solver and complete GPU/OBS integration; fix important findings and re-review.
- [x] Update README, version, calibration guide and release notes to describe final behavior and limits.
- [x] Cross-build and native Windows/Linux CI, verify ZIP CRC/SHA256/source and binary imports.
- [x] Publish private v0.3.0 release with source/binaries, verify uploaded digest and record completion.
