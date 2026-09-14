# SteadyXR 0.8.0 OpenXR Capture for OBS

The direct source provides the stabilized OpenXR image in OBS on the same PC as the game. The standalone app can stay closed. The original VR Stabilizer filter remains available for a capture-card source on a separate PC.

## Install

1. Close OBS. Open `Gaming-PC\SteadyXR-0.8.0` and copy **obs-plugins** and **data** directly into `C:\Program Files\obs-studio` on the gaming PC. These folders sit beside SteadyXR.exe, its layer and INI; no extra nesting is needed. The identical folders remain available under `Stream-PC`. In the complete ZIP they are beside the app under `preview`, as well as under `obs-install`. If you installed the previous module with `Install-OBS.cmd`, use that installer again instead so only one module copy is installed.
2. Start OBS and add **Sources → + → SteadyXR OpenXR Capture**.
3. Close the standalone SteadyXR/Horizon Lock preview. The source uses the same OpenXR connection and works without the app running. Reuse this source in other scenes using **Add Existing**.
4. Keep the working SteadyXR OpenXR layer enabled. If the game already loaded that compatible layer, no game restart is needed. For first setup, use the app's **Install connection**, then restart the game in OpenXR/D3D11 once.

OBS and the game must run on the same graphics adapter. Stereo and context/foveal quad views use the existing capture path, including the OpenXR path used with the Pimax SteamVR driver. This is a video source; capture game audio separately in OBS as usual.

## Bring over your settings

In the source properties select your `Gaming-PC\SteadyXR-0.8.0\SteadyXR.ini` (or the INI beside the preview you last used), then click **Import settings**. The ready-to-use 0.8.0 folder receives the previous viewer's INI unchanged if it has no settings yet. A complete INI preset or legacy `preview.ini` can also be imported. This copies stabilization and framing into OBS without changing the file.

OBS stores subsequent edits with this source in the scene collection. Render-area extension stays in the settings beside the registered OpenXR layer; the source does not overwrite it. Use the standalone app to change extra rendered area for the next game launch, then close it before returning to the OBS source.

## Controls and behavior

The default output is 3840×2160, 16:9 and 60 FPS. Eye, output resolution, aspect, smoothing, horizon lock, adaptive leveling, mirror allowance/softness, crop, per-eye edge reserves and Y offset are adjustable. Natural, Stream Comfort and Strong leveling apply the same built-in tuning as the standalone preview while keeping framing. Crop margins remain available as stabilization reserve behind the visible frame.

- **Reset stabilization** clears motion history without changing tuning.
- **Reset horizon** selects the upright reference again while retaining motion history and tuning. Hold the headset approximately upright and look normally ahead before using it. It does not recenter the headset.
- **Compare A/B (20 seconds)** uses the selected **A/B comparison view**: Original, 50/50 split (left stabilized, right original), or Side by side (two complete views fitted without stretching). All three retain the crop, reserve, aspect and framing. Press it again to return immediately. It returns automatically after twenty seconds and never changes the saved bypass setting or preset contents.
- **Saved preset 1/2/3** accept paths to complete INI presets. Use the corresponding **Load saved preset** button or hotkey to apply one. The source saves the slot paths in its OBS settings and reads the selected preset without modifying the file. Importing a preset keeps the current capture pause state and leaves the game's render-extension settings alone.

The compact **Status snapshot** in source properties reports the connection, measured captured FPS versus requested FPS, pause and comparison state. Open the properties or press **Refresh status** for an updated reading. Measurements continue internally; the panel is not rebuilt automatically while you adjust sliders. These messages add no pixels over the live game image. The chosen comparison view is saved, but an active comparison is temporary.

## Hotkeys

Open **OBS → Settings → Hotkeys** and find this SteadyXR OpenXR Capture source. Assign your own shortcuts to **Reset stabilization**, **Reset horizon**, **Pause / resume capture**, **Compare A/B (20 seconds)**, **Natural**, **Stream Comfort**, **Strong leveling**, and **Load saved preset 1/2/3**. OBS stores and handles these bindings; the standalone app's Ctrl+Alt shortcuts apply only while that app is running.

The source pauses capture when hidden or explicitly paused. If no frames arrive for two seconds, it replaces the previous image with a black waiting screen and reconnects when frames resume. A busy message means another source or preview owns the feed. A GPU mismatch requires moving OBS and the game to the same adapter. Do not add the original VR Stabilizer filter to this already stabilized output.

This build expires 30 days after its UTC build timestamp, shared with the release's standalone app and bundled capture DLL. The source then displays an expiration notice; settings remain saved and the game continues running. Install a fresh build to resume capture.
