# VR Stabilizer Implementation Plan

> For agentic workers: use superpowers:subagent-driven-development for isolated implementation/review tasks where appropriate. The user already requested the complete implementation; continue without another approval pause.

**Goal:** Deliver a usable two-PC SteamVR tracking sender and OBS rotational stabilization filter, with all requested controls and an installable Windows package.

**Architecture:** C++17 platform-independent core and UDP receiver, Windows OpenVR sender, and an OBS GPU filter. The HDMI video remains unchanged on the gaming PC; timing alignment is adjustable on the stream PC.

**Tech Stack:** C++17, CMake, Valve OpenVR 2.15.6, OBS libobs C API, OBS effect shader, Winsock/POSIX sockets, Windows x64 and macOS verification.

**Spec:** ../specs/2026-09-05-vr-stabilizer-design.md

## Global Constraints

- Do not modify headset rendering, tracking origin, or the active runtime.
- Manual delay is tracking lookback at filter render time; it adds no video buffer.
- Never claim capture-card frame identity from independent SteamVR timing alone.
- Validate UDP messages before using numeric data. Keep network I/O off the rendering thread.
- Smoothing off and tracking loss both preserve normal output framing.
- Support the requested FOV, delay, smoothing, stabilization crop, and final 16:9 framing controls.

### Task 1: Core and transport

Files: `include/vrst/core.hpp`, `src/core.cpp`, `include/vrst/protocol.hpp`, `src/protocol.cpp`, `include/vrst/receiver.hpp`, `src/receiver.cpp`, `include/vrst/net.hpp`, `tests/core_tests.cpp`, `tests/network_tests.cpp`, `CMakeLists.txt`.

Interfaces: public C++ structs and signatures in the headers; receiver supplies a `TrackingSnapshot` containing correction, projection and diagnostics without exposing sockets to OBS.

- [x] Write tests with literal rotation/projection expectations, 90/60 frame selection, timestamp alignment, malformed packets and tracking gaps; demonstrate missing behavior before implementation.
- [x] Implement and run portable core tests using `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
- [x] Implement a bounded UDP receiver and exercise real loopback sync/pose delivery.

### Task 2: OBS filter

Files: `plugin/obs_filter.cpp`, `plugin/data/stabilize.effect`, `plugin/data/locale/en-US.ini`, `plugin/CMakeLists.txt`.

Consumes: `vrst::Receiver`, `vrst::Settings`, `vrst::make_warp`, `vrst::Warp` from the shared headers. Produces the `vr-stabilizer` OBS filter.

- [x] Read the headers and implement all controls with stable names and clear descriptions.
- [x] Render source once via OBS's filter API and use the core's warp rays and quaternion in the GPU shader. Report output dimensions through filter callbacks.
- [x] Include connection/clock/tracking diagnostics and reset. Release sockets, threads and GPU resources on destruction.
- [x] Compile against real libobs and inspect shader/runtime behavior with the available local OBS installation.

### Task 3: Sender and delivery

Files: `sender/main.cpp`, `scripts/build-windows.ps1`, `scripts/install-obs.ps1`, `scripts/uninstall-obs.ps1`, `scripts/package.py`, `.github/workflows/build.yml`, `README.md`.

Consumes: core math, wire protocol and network wrapper. Produces `vrst-sender.exe` and installable plugin bundle.

- [x] Implement SteamVR background pose gathering and projection conversion with reconnect and validity handling. Expose live/frame mode and synthetic demo mode.
- [x] Cross-compile actual Windows binaries, inspect imports, package only needed files with dependency license notices and checksums.
- [x] Write setup, calibration, iRacing/SteamVR source guidance, and clear limitations. Document Windows builds and add CI.
- [x] Run final integration tests, independent code review, and package verification. Resolve important findings before delivery.
