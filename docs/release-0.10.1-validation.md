# SteadyXR 0.10.1 UI validation — 2026-10-19

This update reorganizes the existing native preview controls. The main window has Capture, Camera, Framing and Output pages; Spectator & effects has Gaze & chat, Motion, Exposure, Bloom and Color pages. Capture/rendering algorithms, IPC 4, settings keys and feature defaults are unchanged. New visual treatments use native GDI painting on demand; hover has no animation timer. Spectator status text is repainted only when its content changes.

- Native Windows x64 Release ALL build succeeds with MSVC 19.44 and Windows SDK 10.0.26100.
- Complete CTest suite: **34/34 passed**, 58.45 seconds, including native UI, GPU rendering, isolated OBS, fake OpenXR, settings/presets and package fixtures.
- After the final accessibility/scrollbar fixes: **6/6 affected suites passed**, 29.56 seconds (preview UI, theme, presets, build expiry, layer and isolated OBS module).
- Navigation regressions failed before implementation and pass afterward. They check page visibility, unchanged settings, native radio checked state, all-page control bounds, Tab-based numeric commits and moving focus away from a hidden page.
- All nine main/spectator page screenshots were visually inspected, along with existing preset/advanced dialogs. The screenshot fixture now flushes GDI drawing before CPU bitmap reads and shows native edit controls before capture.
- Independent review closed both findings: selected navigation state now uses native radio-button semantics, and long connection details retain a visible scrollbar. No remaining review findings. Screen-reader runtime and additional DPI configurations were not exercised.

The app, capture layer and OBS source share build timestamp **2026-10-19 13:16:38 UTC** and expiry **2026-11-18 13:16:38 UTC**. An unexpired 0.10.0 layer remains IPC-compatible. Delivery preserves previous packages and viewer settings; it does not alter an installed OBS configuration or register an OpenXR layer.

Live Dream Air gaze compatibility and sustained VR/stream-PC performance remain the hardware acceptance checks documented for 0.10.0; this UI update does not claim new hardware validation.
