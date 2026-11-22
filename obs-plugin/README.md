# SteadyXR OBS source 1.1.0

Windows x64 plugin for OBS Studio 32.2 or newer. Tested against OBS 32.2.1.
Receives SteadyXR's final GPU canvas, including stabilization, framing, effects,
comparison and overlays. SteadyXR remains running; its direct OBS output mode
hides the separate preview window. Video only: configure audio in OBS normally.

## Install and use

1. Extract the complete SteadyXR package and close OBS.
2. Click **Install OBS integration** in SteadyXR, or run
   `obs-plugin/Install-OBS-Integration.ps1` with Windows PowerShell.
3. Pick the OBS installation folder. The standard `C:\Program Files\obs-studio`
   folder is suggested; portable installations are supported. Accept Windows'
   administrator prompt if that installation requires it.
4. Start OBS and add the **SteadyXR** source. In SteadyXR select
   **Direct OBS output plugin**. Both programs must use the same GPU and Windows
   login session. Windows Graphics settings can assign both to the same GPU.

The installer writes only `obs-plugins/64bit/steadyxr-obs.dll` and
`data/obs-plugins/steadyxr-obs/` inside the selected installation. It preserves a
previous DLL as a timestamped backup in that data folder. It does not change OBS
profiles, scenes, settings or other plugins, and refuses to install while OBS is
running. Close OBS before manually removing those two plugin locations.

Source properties include **Automatically start and close SteadyXR** (off by
default) and an application path picker. The installer supplies the original
package's `SteadyXR.exe` path. Update the path if you move the package. Enabling
the option launches SteadyXR with `--obs-output` when the source loads. All
enabled sources share one process. The plugin requests a normal close only for
the process it started, when its last enabled source is removed/disabled or OBS
exits. Merely hiding a source or changing scenes does not stop the app. An
independently started SteadyXR is never closed or forcibly terminated.

## Connection behavior

The source automatically reconnects after output mode changes, a producer
restart or output resolution changes. Temporary GPU contention repeats the last
completed frame. A disabled, stale (over three seconds) or dead producer becomes
transparent. Native output dimensions follow the first completed new frame;
before connection the source reports 1920×1080. Fit the source to your OBS canvas
after changing its aspect ratio. Multiple sources can read the same paused image.

The protocol in `../preview/obs_output_protocol.hpp` is local-session metadata
plus a legacy D3D11 shared texture. Named metadata mutex and GPU keyed mutex
acquisitions use zero timeout. OBS copies the shared image into a private GPU
texture, flushes the copy and releases key 1, enabling latest-frame broadcast.
The producer tries key 0, then key 1, before rendering a replacement. No CPU image
readback, encoder, network transport or user configuration is involved.

## Build and verification

Install Visual Studio 2022 C++ Build Tools, Windows SDK, CMake and OBS Studio.
Run `./obs-plugin/Build.ps1` from PowerShell. This downloads pinned official OBS
32.2.1 source headers with a SHA256 check, creates an import library from the
installed OBS DLL, builds, runs isolated tests and stages the distributable at
`build-obs/package/obs-plugin/`. Existing SDKs can be supplied via
`-ObsSourceDirectory` and `-ObsImportLibrary`. No OBS DLLs are redistributed.

Tests initialize libobs and D3D11 in their own process without loading the OBS
frontend or any user profile. Close SteadyXR and OBS before testing: the test
refuses to use an existing SteadyXR IPC session. Tests cover GPU pixels and sRGB,
contention, resizing, multiple sources, hide/resume and producer reconnection.
Lifecycle tests use a separate fixture executable, never the user's SteadyXR.

OBS API references: [graphics header](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs/graphics/graphics.h),
[D3D11 keyed mutex implementation](https://github.com/obsproject/obs-studio/blob/32.2.1/libobs-d3d11/d3d11-subsystem.cpp).

## License

This plugin directory is GPL-2.0-or-later; see `LICENSE`. The independent IPC
protocol header is MIT-licensed. The standalone SteadyXR application retains its
own license. The plugin links to libobs, which is GPL-2.0-or-later. These scope
exceptions apply to this new plugin and protocol, not the rest of SteadyXR.
