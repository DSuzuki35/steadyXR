# Direct OBS OpenXR Source Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Execute the independent renderer task alongside controller ownership/integration work; review the complete implementation before delivery.

**Goal:** Stream stabilized OpenXR images in OBS without a visible preview or separate application.

**Architecture:** Reuse the frame/pose channel and stabilization renderer in a new OBS source. An offscreen renderer shares its GPU output with OBS using a zero-timeout keyed mutex and OBS retains a private texture for repeated scene rendering.

**Tech Stack:** C++17, Windows x64, D3D11/DXGI, libobs, CMake, Python packaging.

**Spec:** docs/superpowers/specs/2026-09-14-obs-openxr-source-design.md

## Global Constraints

- Version 0.7.0 and 30 days from build remain in force.
- English UI, default 3840x2160 at 60 FPS; reuse current visual algorithms.
- No live registry, runtime, OBS configuration changes or user-app restarts during development/delivery.
- GPU-only video transfer. All cross-device mutex acquisitions use zero timeout.
- Delivery: C:\Users\danie\OneDrive\Simracing\Utilities\Horizon Lock VR, preserving user settings and previous packages.

### Task 1: Offscreen renderer and GPU output

**Files:** preview/renderer.hpp, preview/renderer.cpp, preview/renderer_tests.cpp.

**Interfaces:** Add `bool Renderer::attach_offscreen(const LUID *adapter = nullptr)`, `std::uint64_t Renderer::output_handle() const`, `std::uint64_t Renderer::output_generation() const`. Offscreen draw writes output only and publishes key 1; external reader releases key 0. A requested adapter stays fixed so mismatched game adapters fail clearly. Output generation changes on replacement/device reset and handles are valid only while their generation survives. Standalone HWND rendering retains existing behavior.

- [x] Add a test that an offscreen renderer draws the black waiting image and demo without creating a window, with correct output dimensions.
- [x] Check tests fail with absent offscreen capability. Implement the interface and an RAII writer mutex release; failed draws never publish a partial successful frame.
- [x] Test `AcquireSync(1, 0)` on a second D3D11 device, read back the copied pixels, `ReleaseSync(0)`, draw again. A draw while reader holds the key must return promptly. Verify resizing changes generation, preserves the held old texture, and new pixels are correct.
- [x] Re-run renderer/composition tests; report native GPU results and changed files.

### Task 2: Consumer ownership and standalone cooperation

**Files:** preview/transport.hpp/cpp, preview/consumer_tests.cpp, preview/main.cpp, preview/CMakeLists.txt.

**Interfaces:** Add `bool Channel::heartbeat(unsigned eye, unsigned fps)`, `void Channel::release_consumer()`, and `bool Channel::owns_consumer() const`. Heartbeat claims one consumer under the channel mutex, respecting live foreign PID and another local Channel owner. Release clears shared fields only for the current owner; close releases automatically. No shared ABI changes.

- [x] Test two Channels on an isolated name: first heartbeat succeeds, second cannot change eye/FPS or clear the owner, release permits handoff. A foreign live heartbeat refuses takeover; a stale heartbeat allows it.
- [x] Implement ownership and change standalone heartbeat/shutdown to use it. Gate standalone frame consumption on ownership and show a conflict message.
- [x] Verify expired/paused/minimized standalone states release ownership and existing UI/layer tests still pass.

### Task 3: Native OBS source, properties and import

**Files:** plugin/openxr_source.hpp/cpp, plugin/openxr_source_settings.hpp/cpp, plugin/openxr_source_settings_tests.cpp, plugin/obs_filter.cpp, tests/openxr_module_tests.cpp.

**Interfaces:** `void register_openxr_source()` registers id `steadyxr_openxr_source`, name `SteadyXR OpenXR Capture`, Windows only. Use Task 1 Renderer methods and Task 2 Channel methods. Root owns CMake integration.

- [x] Write settings tests for native defaults, finite/clamped slider inputs, selected-eye reserves, and literal legacy/new INI fixture imports. Import is read-only and does not touch render-area settings.
- [x] Add source properties for all render options plus reset, pause, import and Stream Comfort. OBS scene settings hold values, independent of app lifetime.
- [x] Create the renderer using the OBS D3D11 adapter. In video tick acquire ownership only when showing and not paused/expired, consume once, draw on new frames/settings/status changes, import shared output by handle/generation. Acquire key 1 without waiting, GPU-copy to private GS_RGBA texture, release key 0. Draw the private texture using correct sRGB handling.
- [x] Clear stale image on eye change, source inactivity/reconnect and two-second timeout; show readable black waiting/expired/conflict/error messages. Report adapter mismatch instead of attempting cross-GPU reads. Resource creation/destruction uses OBS graphics context; settings and graphics callbacks do not race.
- [x] Add a real libobs harness that creates source, publishes synthetic shared GPU frames with exact poses, stages output and checks pixels, new dimensions, waiting after timeout, ownership/hidden/reconnect behavior and clean teardown. Keep fixture data/channel/config isolated from live applications.

### Task 4: Integration, verification and delivery

**Files:** CMakeLists.txt, plugin/CMakeLists.txt, preview/CMakeLists.txt, scripts/test-module-windows.ps1, scripts/package.py, README.md, docs/local-delivery-de.txt, TODO.md.

- [x] Link Windows source to existing preview renderer/build expiry; preserve non-Windows/filter-only builds. Add meaningful tests to CTest and native module harness.
- [x] Run single ALL Windows build so app/layer/plugin receive the same deadline; run full CTest and real libobs harness. Review implementation and fix reproducible findings.
- [x] Document source installation, settings import, close standalone preview, reuse source across scenes, black waiting/expiry and same-GPU requirement. Mark requested TODO complete.
- [x] Package 0.7.0 with source/checksums and deliver drop-in folders; verify ZIP contents, hashes, settings preservation and expiration. Retain prior same-version packages.
