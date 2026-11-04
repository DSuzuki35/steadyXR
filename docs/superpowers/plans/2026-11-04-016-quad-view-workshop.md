# SteadyXR 0.16 Quad-view Workshop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Deliver the approved 0.16 combined preview and source status, including the verified Workshop playback repairs.

**Architecture:** Preserve separate source images through capture and replay; composite once in the viewer. Share source-description formatting between live status and Workshop. Preserve the signed capture DLL and existing clip format.

**Tech Stack:** C++17, Windows, D3D11, OpenXR, Media Foundation, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-11-04-016-quad-view-workshop-design.md` (approved 2026-11-04).

## Global Constraints

- Retain the native viewer and D3D11/D3D12 capture contracts; no provider dependency.
- No automatic layer/runtime/game configuration changes or GitHub publication.
- Preserve separate sources and clip format v1; no profile migration.
- Deliver versioned packages to `C:\Users\danie\OneDrive\Simracing\Utilities\Horizon Lock VR` and preserve existing packages.
- Initial physical targets: mbucchia QVFR/Sboys3 SteamVR, Dream Air and Dream Air SE; live testing remains unverified unless performed.

## Review Focus

- Unequal source resolutions: focus detail must survive final output sampling (Task 1).
- Moving/asymmetric focus: use its FOV and orientation in source and processed modes (Task 1).
- Single-view transition: do not sample a retained focus texture (Task 1).
- Stale/paused capture: never present retained two-view data as fresh input (Task 2).
- Padded 60 fps replay: original framing and pose/image association survive decode (already RED/GREEN; Task 3 retains verification).

## Task 1: Combined Workshop source and composition regressions

**Files:** `preview/renderer.cpp`, `preview/renderer_tests.cpp`.
**Interfaces:** Existing `Renderer::consume_replay`, `Renderer::draw`, `Renderer::readback`, `Options::workshop_source`.

- [x] Add GPU fixture with low-resolution solid peripheral and higher-resolution patterned focus; assert outside pixels retain peripheral, inside retains detail, and source mode includes focus.
- [x] Exercise displaced focus FOV/orientation, Original versus processed, both eye metadata values, subrectangle sampling, and dual-to-single transition. Reuse existing coverage where exact behavior is already asserted.
- [x] Run `vrst-preview-renderer-tests.exe`; observe source-mode regression fail before production edits.
- [x] In Workshop source mode, retain focus enabled for two views and map unstabilized context rays into focus using `inverse(focus) * context`. Keep effect bypass and full context FOV.
- [x] Build/run renderer and composition tests; commit the verified change.

## Task 2: Honest source status in live preview and Workshop

**Files:** New `preview/source_status.hpp`; modify `preview/main.cpp`, `preview/workshop.cpp`, `preview/renderer_tests.cpp` and existing UI self-test in `main.cpp`.
**Interfaces:** Inline `std::wstring source_view_summary(const Frame&, bool details = false)`; uses submitted rectangles for each view. Existing `Renderer::source_snapshot` provides accepted frame metadata without new IPC.

- [x] Add tests for distinct source descriptions/dimensions and missing frame; UI self-test asserts fresh quad versus paused/stale/window/demo status.
- [x] Observe failure before implementing source summaries and UI integration.
- [x] Render “Peripheral + focus” for two accepted views and “Single view received” for one. Details show source dimensions; single-view guidance explains upstream composition/layer chain without declaring failure.
- [x] Show source kind only for fresh active OpenXR input in compact status; details may label retained data explicitly. Workshop labels the recorded source using original metadata dimensions, not proxy dimensions.
- [x] Run renderer tests and `ctest -R preview-ui`; commit.

## Task 3: Validate and package 0.16

**Files:** `CMakeLists.txt`, new `scripts/package-016-standalone.ps1`, `preview/README.md`, `docs/workshop.md`, new `docs/release-0.16.0-validation.md`.

- [x] Set app version 0.16.0, document combined source/status behavior, existing playback repairs and runtime limitations.
- [x] Adapt the existing versioned standalone packager: preserve signed 0.14 DLL hash, include corresponding capture source, refuse existing delivery targets, include current source and test logs.
- [x] Build app and changed test targets without rebuilding the capture DLL. Run full CTest plus Workshop `--hardware`, `--hardware-large`, `--hardware-padded` and actual saved clip decode/render/seek probe.
- [x] Request fresh whole-branch review against c7ef9ee, resolve material findings with regression tests, and commit.
- [ ] Package source and binary ZIPs/checksums; verify executable version, capture signature/hash, archive contents and clean Git state. Keep branch local.

## Completed prerequisite

- [x] Workshop timestamp/padding repair: c7600da. Baseline 42/42 and post-fix 42/42; three hardware variants and 77+567 real frames/seek pass. Independent review found no actionable issues.
