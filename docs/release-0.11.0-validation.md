# SteadyXR 0.11.0 validation — 2026-10-19

The main preview window now states that this 30-day test version is free and displays its fixed UTC expiry. A second line describes planned paid access for channel subscribers/supporters or a one-time license purchase. The notice stays visible across the four main tabs and is not drawn into the clean spectator output. Both lines are native static controls created once, with no new timer, network activity or GPU pass. The window is 80 pixels taller to accommodate them without covering existing controls.

App resources, the preview title, project metadata, packaging defaults and the OBS build label now use version 0.11.0. Capture algorithms, IPC 4, settings, existing license notices and build-expiry behavior are unchanged.

- Native Windows x64 Release build: passed.
- Complete CTest suite: **34/34 passed**, 58.17 seconds.
- Existing UI navigation checks cover all main pages and verify that visible controls fit inside the client area.
- Capture and Output screenshots were inspected: both notice lines fit without clipping or overlapping existing controls.
- Windows executable FileVersion and ProductVersion both report **0.11.0**.

Build timestamp: **2026-10-19 13:45:01 UTC**. Expiry: **2026-11-18 13:45:01 UTC**. The validity period runs from compilation, not first launch. No new live headset compatibility or performance measurements were made for this static UI change.

The standalone runtime package excludes the OBS plugin, tracking sender and project build/source tree. Required layer-editor runtime helpers and license notices remain included. Matching source is supplied as a separate download to accompany the runtime ZIP.
