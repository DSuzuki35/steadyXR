# Photo Timing Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Provide a phone-readable gaming-PC overlay and correctly paired OBS received/used pose diagnostics.

**Architecture:** Extend the existing sender and receiver without changing UDP wire format. Store selected pose metadata with buffered video. Draw one shared portable bitmap panel in a Win32 sender window and after OBS stabilization.

**Tech Stack:** C++17, Win32/GDI, OBS 32.2.2, CMake, existing Python packaging and Windows test tools.

**Spec:** `docs/superpowers/specs/2026-09-07-photo-timing-design.md`

## Global Constraints

- Off by default; preserve v0.3.1 processing and protocol compatibility.
- Original gaming-clock timestamps, explicit session/staleness and interpolation.
- Never claim the observed pose is a verified arbitrary game-mirror frame tag.
- Post-transform OBS panel is excluded from analysis; image-only and comparison modes must not claim an applied pose.
- No external publication or user OBS/SteamVR configuration changes.

## Task 1: Timing metadata

Files: `include/vrst/core.hpp`, `src/core.cpp`, `include/vrst/receiver.hpp`, `src/receiver.cpp`, `tests/core_tests.cpp`, `tests/network_tests.cpp`.

Interfaces: append `session`, `sample_time_ns`, `interpolated` to `PoseResult`. Append `Pose latest_pose`, `bool latest_received`, and `uint64_t local_time_ns, remote_now_ns, latest_receive_time_ns` to `TrackingSnapshot`. Existing `pose.time_ns` remains its earlier packet stamp; `sample_time_ns` is interpolation target only when interpolation is actually used, otherwise the packet stamp. `latest_pose` is the last accepted packet independent of requested delay. The new local/remote now and receive times are explicitly named clock domains.

- [x] Add and observe failing tests for source packet vs effective interpolation time and independent latest vs delayed pose, including 5-second clock offset.
- [x] Implement metadata and reset/session/stale handling without changing smoothing or packet acceptance.
- [x] Run core/network suites and review results.

## Task 2: Shared photo panel and gaming window

Files: new `include/vrst/timing_overlay.hpp`, `src/timing_overlay.cpp`, `tests/timing_overlay_tests.cpp`, `sender/overlay.hpp`, `sender/overlay.cpp`, `Start-Delay-Check.cmd`; modify sender main/help/integration tests, root CMake and packaging.

Interfaces: shared `TimingOverlayData` holds source kind, availability/mode flags, sender-clock now/session, latest `Pose`, selected `PoseResult`, receive age/buffer age and requested delay. `timing_overlay_lines` returns bounded ASCII lines; `render_timing_overlay(data, scale)` returns an RGBA `OverlayBitmap`. The sender wrapper receives each actual published packet after assigning session/sequence and refreshes the desktop panel without stealing focus or requiring a subscriber. No header/source overlap between sender and shared panel work.

- [x] Write failing formatting/status/raster-bound tests, then implement the shared panel.
- [x] Add opt-in sender flag and movable non-activating Windows overlay; closing it preserves sender operation.
- [x] Verify help, invalid CLI cases, headless default behavior, demo packet compatibility and native overlay lifecycle.

## Task 3: OBS display pairing

Files: `plugin/obs_filter.cpp`, `plugin/image_filter.hpp/.cpp`, new `plugin/timing_overlay.hpp/.cpp`, plugin CMake/locale, native module tests.

Interfaces: retain displayed slot's full selected `PoseResult` and expose it with capture time. Plugin timing renderer uploads the portable panel bitmap and draws at a bounded output-relative size after final source sampling. Track whether a pose was actually applied independently of availability.

- [x] Add native tests for default-off output, enabled overlay pixels and control visibility.
- [x] Add checkbox, sender connection when diagnostics need it, graphics lifecycle and overlay drawing on all valid output paths.
- [x] Verify buffered hybrid pairing, repeated render reuse, interpolation labels and explicit no-applied-pose states.

## Task 4: Delivery

- [x] Run all portable/sender/network suites and actual Windows OBS module checks.
- [x] Request independent code review; fix actionable findings and rerun affected checks.
- [x] Document phone-photo workflow and diagnostic limits, package v0.3.2 with the new launcher and manual `obs-install` layout.
- [x] Verify ZIP/checksums/corresponding source and give the user exact gaming/stream-PC steps.
