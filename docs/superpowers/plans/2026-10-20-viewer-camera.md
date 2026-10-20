# Viewer Camera Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the independent telemetry and controls tasks; renderer integration remains with the parent. Work in the existing checkout because the requested release builds on its uncommitted 0.11 work. Do not commit unrelated existing changes.

**Goal:** Add optional lookahead stabilization, automatic crop and speed-driven framing to standalone and direct OpenXR OBS capture, with iRacing and Le Mans Ultimate speed telemetry.

**Architecture:** Reuse the shared D3D11 renderer and composition controller. Preserve image/pose/overlay association in a bounded consumer-side GPU history, using future captured poses to smooth the delayed camera. Read only speed from supported local shared-memory telemetry, with stale/disconnected data returning smoothly to neutral framing. Extend current native controls and settings/presets.

**Tech Stack:** C++17, D3D11, Win32 shared memory, native controls, existing OBS API.

**Spec:** The user's selected ideas 1, 2 and 4 in this conversation, explicitly including standalone, iRacing and LMU. All features optional and off by default; minimal gaming-PC resource use remains the primary constraint.

## Global Constraints

- No frame buffering, telemetry polling or additional GPU passes when disabled.
- No game/runtime registry or plugin configuration changes as part of development/delivery.
- Lookahead is real delayed-frame smoothing, not extrapolation; delay and limits must be visible. Audio is not captured here, so document manual OBS audio sync.
- Buffer cap: four delayed frames and 256 MiB of additional GPU textures; reduce or bypass gracefully if insufficient. Never block the game on a GPU wait.
- Auto-crop uses existing captured pixels, bounded zoom, fast attack/slow release. Speed framing adds at most the configured crop at low speed and returns to normal framing at high speed; it never requests extra game rendering.
- iRacing and LMU integrations must use documented/shared interfaces, bounds-check metadata and reject stale, disconnected or invalid data.
- Source loss, session/reference/eye/format changes and disabling features release history. Settings and presets round-trip through standalone and OBS.
- Preserve existing public ZIPs; deliver a new version with standalone ZIP and matching source separately. License notices remain unchanged.
- Two-PC capture-card feeds do not currently transport original eye poses/textures. These camera features run at the capture consumer (standalone/direct OBS), not on an arbitrary already-cropped HDMI image. Document this limitation instead of claiming remote support.

## Tasks

- [x] Telemetry: implement a bounded read-only speed reader for iRacing and LMU, with parser fixtures and lifecycle tests.
- [x] Camera math: implement sanitized director settings, centered future-pose averaging, bounded automatic crop with hysteresis and speed crop; meaningful deterministic tests.
- [x] Controls: add an optional Director page and matching OBS controls; persist all settings, default off; extend round-trip tests.
- [x] Renderer: retain delayed GPU frames/poses/overlays in a fixed-size resource pool, apply camera math in shared rendering, expose status and reset correctly. Exercise real GPU delayed-content and disabled-resource tests.
- [ ] Integration: build, run affected tests, independent review, full suite, visual inspection, document limitations/costs and package/deliver.

## Review Focus

- A delayed image must always use its own poses, projection, gaze, overlays and speed snapshot.
- Repeated drawing of one frame must not advance smoothing, auto-crop or telemetry timestamps.
- A/B comparisons must not ingest a frame twice or alter stored settings.
- Recenter, source switches and metadata changes must not reuse stale history.
- Disabled/bypassed features must not allocate buffer textures or keep telemetry mappings open.

## Execution ledger

- Ruling: keep work in the current branch to preserve the user's existing UI/release edits; make no commits or pushes during implementation.
- Ruling: implement on the shared capture renderer for standalone and direct OBS. Remote capture-card postprocessing lacks the source pose/image reserve required by idea 1, so no claim of stream-PC camera processing is made.
- Ruling: telemetry failure is nonfatal and fades speed framing back to the baseline. Live game/headset validation remains a separately reported acceptance check.

- Task review: telemetry/UI reviewed independently against installed LMU SDK; no actionable findings.
- Camera review found Natural movement discarded the future target. Added an explicit optional natural motion input while retaining actual source pose for gaze mapping; regression observed failing before fix and passing afterward.
- Warmup camera target is now fixed per displayed frame; future poses cannot move a held frame without advancing the controller clock. Waiting resets new crop/speed state without clearing prearmed chat learning.
- Final broad review found no P1/P2 defects; packaged guide link (P3) corrected in the copied files.
- Native build succeeded. Full suite initially passed 36/37; package fixture lacked the two new guides. Fixture updated and packaging test passed. Final release verification and delivery follow below.

- Final native suite: 37/37 passed in 59.54 s. Director screenshot inspected; controls and guidance fit. Release validation recorded in docs/release-0.12.0-validation.md. Packaging/delivery scripts verify archive contents, checksums, preserved settings and prior versions before final handoff.
