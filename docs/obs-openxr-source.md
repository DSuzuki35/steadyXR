# SteadyXR OpenXR Capture for OBS

The direct source provides the stabilized OpenXR image in OBS on the same PC as the game. The standalone app can stay closed. The original VR Stabilizer filter remains available for a capture-card source on a separate PC.

## Install

1. Close OBS. From the delivered `Stream-PC` folder, copy **obs-plugins** and **data** into `C:\Program Files\obs-studio` on the gaming PC. In the complete ZIP those folders are under `obs-install`. If you installed the previous module with `Install-OBS.cmd`, use that installer again instead so only one module copy is installed.
2. Start OBS and add **Sources → + → SteadyXR OpenXR Capture**.
3. Close the standalone SteadyXR/Horizon Lock preview. The source uses the same OpenXR connection and works without the app running. Reuse this source in other scenes using **Add Existing**.
4. Keep the working SteadyXR OpenXR layer enabled. If the game already loaded that compatible layer, no game restart is needed. For first setup, use the app's **Install connection**, then restart the game in OpenXR/D3D11 once.

OBS and the game must run on the same graphics adapter. Stereo and context/foveal quad views use the existing capture path, including the OpenXR path used with the Pimax SteamVR driver. This is a video source; capture game audio separately in OBS as usual.

## Bring over your settings

In the source properties select your `Gaming-PC\SteadyXR-0.7.0\SteadyXR.ini` (or the legacy `preview.ini` beside the preview you last used), then click **Import settings**. A complete INI preset can also be imported. This copies stabilization and framing into OBS without changing the file.

OBS stores subsequent edits with this source in the scene collection. Render-area extension stays in the settings beside the registered OpenXR layer; the source does not overwrite it. Use the standalone app to change extra rendered area for the next game launch, then close it before returning to the OBS source.

## Controls and behavior

The default output is 3840×2160, 16:9 and 60 FPS. Eye, output resolution, aspect, smoothing, horizon lock, adaptive leveling, mirror allowance/softness, crop, per-eye edge reserves and Y offset are adjustable. Stream Comfort applies the same tuning as the standalone preview. Crop margins remain available as stabilization reserve behind the visible frame.

The source pauses capture when hidden or explicitly paused. If no frames arrive for two seconds, it replaces the previous image with a black waiting screen and reconnects when frames resume. A busy message means another source or preview owns the feed. A GPU mismatch requires moving OBS and the game to the same adapter. Do not add the original VR Stabilizer filter to this already stabilized output.

This build expires 30 days after its UTC build timestamp. The source then displays an expiration notice; settings remain saved and the game continues running. Install a fresh build to resume capture.
