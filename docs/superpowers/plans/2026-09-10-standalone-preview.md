# Standalone preview implementation plan

**Goal:** Deliver an OBS-free stabilized OpenXR preview for the gaming PC.

**Architecture:** D3D11 OpenXR implicit capture layer, versioned single-outstanding-frame GPU IPC, native Win32 viewer using existing rotational stabilization. Images are copied before release and paired with submitted context/inset metadata at EndFrame. The game never waits for the viewer.

**Tech Stack:** C++17, Win32, D3D11/DXGI, OpenXR 1.1 headers, CMake, existing vrst_core.

**Spec:** docs/superpowers/specs/2026-09-10-standalone-preview.md

## Global constraints

- Do not alter live OBS, SteamVR, Pimax or iRacing configuration for delivery/testing.
- Keep finished artifacts in OneDrive/Simracing/Utilities/Horizon Lock VR; preserve earlier packages.
- GPU work is disabled without an active unpaused viewer heartbeat. No blocking game-side mutex/GPU waits.
- Transport and capture changes must preserve all runtime API arguments/results and headset submissions.
- No automatic claim that synthetic verification proves real-game behavior or GPU overhead.

## Task 1: Capture layer and shared transport

Owner: delegated implementation, bounded to preview/transport.*, preview/layer.*, preview/layer_tests.cpp and preview/openxr-layer.json.in. Shared interface is preview/transport.hpp.

Implement a named mapping protected by a named Win32 mutex with zero-timeout locking. Viewer owns controls and heartbeat; producer publishes at most one pending frame. Packet has producer PID/adapter LUID/generation/sequence/session/reference epoch/display time and at most two views. Each view contains shared texture handle, width/height/format, subimage bounds, orientation and tangent FOV. Keyed texture mutex transitions 0 (producer) -> 1 (consumer) -> 0. Consumer copies to private textures before returning key 0. Backpressure drops new frames. Generation changes only replace idle resources, and reconnect discards old-generation state.

Negotiate loader API layer interface, chain instance/session/swapchain operations, acquire/wait/release, WaitFrame, EndFrame, PollEvent. Snapshot valid color images before successful release; never access runtime-owned images afterwards. EndFrame uses exact latest released image for referenced subimages and selected stereo eye; quad views pair index eye with eye+2. Handle arrays, subrects, MSAA, reference resets, no-consumer/paused/rate-limited paths and unsupported graphics APIs. Use a fake runtime test to assert pass-through and correct image/metadata lifetime. First write failing tests for validation and ownership behavior, then implementation, then native tests. Do not register the layer globally in tests.

## Task 2: Native renderer and viewer

Owner: controller, independent files preview/renderer.*, preview/main.cpp, preview/preview_tests.cpp. Consume Task 1's fixed IPC schema. Render a fullscreen triangle with existing core warp math, focus inset projection, optional mirrored edges. Use producer adapter and local consumer texture copies. Native settings/clean output windows, F11 fullscreen, pause/A-B, smoothing/crop/mirror/eye/FPS controls, explicit setup/unregister buttons, settings persistence. Provide --demo and --self-test paths without registry mutation. Add CPU projection/reset checks and native GPU readback tests; test pause and resize behavior.

## Task 3: Build, review and delivery

Owner: controller. Add optional VRST_BUILD_PREVIEW, pinned OpenXR headers dependency, native tests and package scripts. Compile existing OBS/sender plus preview, run appropriate suites and native demo. Request an independent code review and resolve material findings. Version package 0.4.0 with complete source and checksums; keep legacy Stream-PC folder structure. Put app, layer and manifest in Gaming-PC/Preview-0.4.0 with a stable Start-Preview.cmd entry point. Document per-user setup, restart, clean capture and avoiding double stabilization. Do not restart the running game or change its runtime. Report live-game validation as pending if not observed.

## Task 4: Default existing-window capture

User clarified that the existing iRacing preview is the required input and restarting the game must not be necessary. Retain optional OpenXR path, add default Win32 Windows Graphics Capture on the renderer device. A delegated bounded component owns WGC resource lifetime/client cropping/resize/minimize and native owned-window tests. Controller integrates selected HWND+PID identity, default window UI, small asynchronous image-analysis readback with matching retained full frame, SDR colors and pause. Verify no unrelated HWND can be recaptured after source closure, accurate image/result ownership, native crop/resize/minimize, and complete package docs. Live iRacing diagnostics may run without a restart when the simulator is available.
