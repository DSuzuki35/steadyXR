# Expanded framing implementation plan

> Use superpowers:subagent-driven-development for independent layer and framing
> tasks, with root implementing UI integration and final delivery.

**Goal:** Ship real per-eye OpenXR overscan and asymmetric stabilization reserves
as 0.6.0, without changing runtime configuration during delivery.

**Spec:** `docs/superpowers/specs/2026-09-11-expanded-framing-design.md`

## 1. Runtime-independent render expansion

- [x] Own `preview/layer.cpp`, `layer_tests.cpp`, `transport.hpp/cpp` and new
  header-only `render_settings.hpp`; no main/UI or composition edits.
- [x] Define the shared UI/layer configuration contract:
  ```cpp
  struct EyeExpansion { double left{}, right{}; };
  struct RenderSettings { bool enabled{}; EyeExpansion eyes[2]{}; };
  RenderSettings read_render_settings(const std::wstring& ini);
  bool write_render_settings(const std::wstring& ini, const RenderSettings&);
  ```
  Store `[Render] Enabled`, `LeftEyeLeftTenths`, `LeftEyeRightTenths`,
  `RightEyeLeftTenths`, `RightEyeRightTenths`. Clamp finite values to 0–30%.
- [x] Write failing hook tests: 10% inward gives tangent bounds -1/+1.2 for
  left and -1.2/+1 for right; original width 2000 recommends 2200. Count-only
  and failed calls never modify buffers, focus views stay identical.
- [x] Implement startup-snapshot settings, LocateViews, enumeration and mask
  hooks. Forward runtime-submitted projection without rewriting it.
- [x] Set `ipc_version=2`, channel suffix v2 and `Frame.eye`. Add
  `Shared.render_enabled` and `Shared.render_left[2]/render_right[2]` active
  percentages. Test config snapshot, masks and both runtime profiles.

## 2. Asymmetric reserves and composition

- [x] Own core Settings/warp and their tests, renderer.hpp, composition.cpp/tests,
  and necessary window-mode renderer.cpp/tests changes.
- [x] Define `EdgeReserve { bool enabled{}; double left{5}, right{5}, top{5},
  bottom{5}; };` and `Options.edge_reserve[2]`, `Options.eye`.
- [x] Write failing geometry tests for 2%/20%, swapped-eye profiles, legacy
  symmetric equivalence, finite clamping and aspect framing bias.
- [x] Apply reserves before correction limiting and aspect fitting. Use the
  captured `Frame.eye` for OpenXR, `Options.eye` for window capture. Keep
  symmetric behavior unchanged when the profile is disabled.

## 3. UI, migration and release

- [x] Root owns main.cpp, new advanced settings window, CMake integration, theme
  version/resources and docs/package scripts. Add a dark English dialog with
  per-eye controls, explicit render save/restart text, inward preset and live
  edge sliders. Persist edge settings in preview.ini.
- [x] Test profile save/load and eye changes through native UI self-tests.
- [x] Upgrade only Horizon Lock registrations when the user clicks connection;
  preserve other layers and avoid duplicate versions. Test against a private
  registry key. Show old-layer upgrade guidance before capture starts.
- [x] Build all targets and run ctest. Independently review the layer, geometry,
  UI and whole branch, address actionable findings, rerun affected checks.
- [ ] Update versions to 0.6.0, package binaries/source/checksums in OneDrive,
  verify hashes and archive contents, commit locally and fast-forward main.
- [ ] Report delivered files and the exact restart/activation steps, with
  hardware compatibility verification still clearly pending if untested.

## Added request: Y offset

- [x] Add Options.vertical_offset (-100..100, default0), live slider and persistence. Move only inside aspect-fit vertical slack; tests cover both capture modes, extrema and no-slack behavior.
