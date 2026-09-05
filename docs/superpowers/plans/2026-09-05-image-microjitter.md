# Image-based microjitter implementation plan

> For agentic workers: use superpowers:subagent-driven-development for the isolated plugin task and review. The user has requested the feature; continue autonomously.

**Goal:** Add working image-only microjitter stabilization and publish a tested experimental Windows release.

**Architecture:** Portable sparse feature tracker and bounded 2D smoother feed an OBS GPU image pipeline with a one-frame matched video buffer. Pose mode remains intact.

**Tech Stack:** C++17, OBS libobs, CMake, GPU effect shaders, Windows x64, macOS GPU verification.

**Spec:** ../specs/2026-09-05-image-microjitter-design.md

## Global constraints

Preserve pose mode and saved scenes. Image mode requires no headset, FOV or network sync. Match each correction to its analyzed image. Bound memory/CPU/correction and fall back on invalid estimates. One OBS-frame image-mode latency is explicit. No hybrid or native OpenXR provider in this change.

### Task 1: Estimator, smoothing and image warp (root)

Files: include/vrst/image.hpp, src/image.cpp, tests/image_tests.cpp, CMakeLists.txt.

- [ ] Define exact interfaces for grayscale input, settings, results and inverse image-warp matrix.
- [ ] Write failing synthetic-motion tests, implement tracker/rigid fit/smoother, verify failures then passes.
- [ ] Test crop-aware warp and failure/reset behavior, benchmark and integrate CTest.

### Task 2: OBS image mode (implementation worker)

Files: plugin/image_filter.hpp/.cpp, plugin/data/image-stabilize.effect, plugin/obs_filter.cpp, plugin/data/locale/en-US.ini, plugin/CMakeLists.txt.

Consumes include/vrst/image.hpp. Image pipeline captures current source and renders preceding analyzed frame using its ImageWarp. Own GPU state on rendering thread; cache within video tick. Root owns core and tests.

- [ ] Add safe ring capture/downsample/readback and exact-frame rendering.
- [ ] Add mode controls, region, limits, diagnostics, resets and visibility; preserve existing defaults and pose path.
- [ ] Build against actual OBS, verify GPU/filter behavior and report findings.

### Task 3: Review and delivery (root + fresh review)

Files: tests/gpu_tests.mm or dedicated image GPU/module tests, README.md, version/build/package files, docs/implementation-progress.md.

- [ ] Verify actual GPU output and image mode frame pairing/lifecycle.
- [ ] Review estimator and plugin integration independently; resolve findings.
- [ ] Run portable suites, Windows builds and Windows/Linux CI; verify package and imports.
- [ ] Update guide and publish source and v0.2.0 in the existing private repository.
