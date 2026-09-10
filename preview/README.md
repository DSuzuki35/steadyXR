# Horizon Lock VR Preview 0.5.0

0.5.0 adds a dark native control panel and an embedded application icon. The PNG and Windows ICO are also included beside the EXE. Existing settings migrate when upgrading the ready-to-use package.

When no new frame arrives for two seconds, the output becomes a plain black screen with “Waiting for OpenXR capture…” (or “Waiting for window capture…” in image mode). The next received frame restores the live preview automatically. Deliberate pause retains the last image. The waiting screen uses the GPU presentation path and does not recenter or alter the headset.

Standalone gaming-PC preview with OpenXR pose stabilization or existing-window capture. The headset image is unchanged. No OBS or video encoder is required on the gaming PC.

## Upgrade from a working OpenXR preview

Close only the previous Horizon Lock preview app, then run **Gaming-PC/Start-Preview.cmd**. Existing preview.ini settings are copied to the new version when it has no settings yet. OpenXR mode and previous tuning are retained; new settings use their defaults.

The new viewer works with the installed 0.4.0 layer. **No connection reinstall or iRacing restart is needed for this viewer update.** Keep Preview-0.4.0 while its layer is registered. Delivery does not modify layer registration, the active runtime, the game or OBS configuration.

## Controls

- **Crop left / right / top / bottom:** independently trim 0–45% per side, in 0.1% steps. Arrow keys adjust one step.
- **Crop reserve:** additional stabilization margin, separate from framing crop. Aspect fitting may trim the remaining image further.
- **Smoothing:** movement smoothing with 0.1% precision, separate from horizon leveling.
- **Horizon lock (OpenXR):** 0% preserves normal smoothed head roll; 100% levels roll using the upright Y-axis hemisphere of the submitted OpenXR layer space. This also supports iRacing poses whose upright direction is -Y. Look normally ahead when starting the preview or pressing Reset preview; the upright hemisphere is retained until reset. Intermediate values remove the corresponding fraction of head roll while preserving viewing direction. It fades near straight up/down, where a level horizon is undefined. Default: 0%. Inactive in image mode or when stabilization is disabled.
- **Mirror edges:** fill exposed borders with reflected source content.
- **Mirror limit:** maximum extension per source axis, as a percentage of source width/height. Default 10%; range 0–50%. Beyond the limit: black. Increase crop reserve or mirror limit if exposed edges appear. Mirroring does not reconstruct scenery.
- **Disable stabilization:** compare the captured image while capture continues.
- **Reset preview:** clear smoothing without recentering the headset.
- **Pause / GPU comparison:** stop capture and continuous rendering. Hiding/minimizing the output also pauses it. Compare VR frame times in the same scene.

## Output

Default internal render resolution: **3840 × 2160**. Enter width/height and click **Apply aspect / resolution**. Limits: 320–7680 × 180–4320. 4K costs more GPU work than 1080p; actual savings versus OBS have not been measured.

Select an aspect preset (16:9, 21:9, 32:9, 4:3, 1:1), or type a ratio such as 239:100, then apply. The image is fitted without stretching; letterboxing can appear in the output canvas or window.

This controls internal rendering. The HDMI capture-card signal still follows Windows display settings; the app does not change them.

**Start preview in fullscreen** defaults to enabled. Choose **Output monitor**; the choice is remembered. F11 or double-click toggles fullscreen; Escape exits it. Alt+Tab returns to the control panel.

## Capture and troubleshooting

**OpenXR pose capture** uses submitted D3D11 images and matching poses, including stereo and context/foveal views. First-time setup uses **Install connection**, then one OpenXR game restart. An existing Horizon Lock layer can be reused; no tracking sender or network-delay adjustment is needed.

**Capture existing window** uses image analysis with no setup or game restart. Select the simulator window and leave it open and unminimized. It is independent of the VR runtime (OpenVR, Pimax OpenXR, SteamVR/SBoys3). It has no absolute headset-based horizon. Scene changes or missing cockpit detail may limit tracking. Window capture is SDR.

Use **Clean Preview** on the capture-card display and disable the old OBS stabilization filter for that source to avoid stabilizing twice.

If **Windows capture shutdown stalled** appears after quickly closing a source, restart only Horizon Lock Preview. iRacing can remain running. Cleanup remains bounded and does not block the UI.

Keep each preview directory intact. Settings are in preview.ini beside the EXE. The --demo option shows synthetic content. Automated tests cover GPU output, resolution, crop/aspect, horizon, mirror limits, settings, fullscreen, capture lifecycle and OpenXR ownership. Driving quality and GPU cost still depend on the live setup.

OpenXR performance: the viewer polls for GPU-ready frames independently of the selected capture rate and copies only the selected eye subrect. This avoids a second frame-rate gate and reduces GPU copy bandwidth for side-by-side stereo textures. The selected FPS remains a target; actual output depends on game frame rate and available GPU time.
