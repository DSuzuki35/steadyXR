# Horizon Lock VR Preview 0.6.0

0.5.0 adds a dark native control panel and an embedded application icon. The PNG and Windows ICO are also included beside the EXE. Existing settings migrate when upgrading the ready-to-use package.

When no new frame arrives for two seconds, the output becomes a plain black screen with “Waiting for OpenXR capture…” (or “Waiting for window capture…” in image mode). The next received frame restores the live preview automatically. Deliberate pause retains the last image. The waiting screen uses the GPU presentation path and does not recenter or alter the headset.

Standalone gaming-PC preview with OpenXR pose stabilization or existing-window capture. Preview stabilization affects only the stream output. Optional render extension asks the game to render a wider eye projection; the runtime receives that actual projection for correct headset composition. No OBS or video encoder is required on the gaming PC.

## Upgrade from a working OpenXR preview

Close only the previous Horizon Lock preview app, then run **Gaming-PC/Start-Preview.cmd**. Existing preview.ini settings are copied to the new version when it has no settings yet. OpenXR mode and previous tuning are retained; new settings use their defaults.

**0.6.0 requires the new OpenXR layer (IPC v2).** Select OpenXR pose capture and click **Upgrade connection** (or **Install connection**). This enables the current Horizon Lock layer and disables older Horizon Lock registrations for this Windows user; it leaves other API layers and the active runtime alone. Restart iRacing once. Both the viewer and layer must come from Preview-0.6.0. Delivery itself never changes registration or restarts anything. Window capture needs no connection or game restart.

## Extra OpenXR scenery, asymmetric reserves and Y offset

Open **Render / edges…** in the main window. Choose the eye profile you want to edit; this does not switch the captured eye. Select the actual stream eye in the main window.

In window capture, the same main-window selector is named **Profile**. It selects which reserve settings apply to the existing window; it cannot change which eye the game shows in that window.

- **Use separate stabilization reserves:** four live sliders, 0–35% per edge in 0.1% steps. For a right-eye stream with more space kept on its inner side, try Left 2%, Right 20%; for the left eye, reverse these. The profile replaces the main uniform Crop reserve slider while enabled. Uniform reserve 10% equals 5% on each edge. Existing Crop left/right/top/bottom controls remain additional framing trims.
- **Y offset:** move the aspect-ratio crop upward (negative) or downward (positive), without zooming or changing the edge trims. 0 restores the automatic placement; ±100 reaches the available upper/lower limit. If the chosen aspect leaves no vertical slack, it has no effect. This is a shared preview setting and works in both capture modes.
- **Enable extra rendered area:** per-eye Extra left/right sliders, 0–30% each. Percentages refer to the original horizontal projection width, not degrees. Right-eye Extra left adds scenery toward the nose; left-eye Extra right is the corresponding inner side. **Both eyes inward: 10%** sets both inner sides to 10% and outer sides to zero.
- **Save for next game launch:** commits both render profiles. Restart the OpenXR game afterward, because view sizes and projections are configured at startup. Render changes stay unsaved until this button is pressed; closing/reopening the panel discards unsaved render edits. Crop/reserve/Y settings are live and save automatically.

Cropping changes only the displayed output rectangle. It does not delete the original image behind it: the full captured eye remains available for stabilization. Mirroring supplies only pixels beyond the actual rendered boundaries. Aspect fitting can require further crop; separate reserves bias that extra crop toward the side with more reserve.

The layer expands `xrLocateViews` FOV tangents and recommends proportionally wider eye textures. It preserves poses, vertical FOV and submitted projection metadata, and makes visibility masks unavailable for expanded context views so hidden-area culling does not remove the new scenery. Stereo and quad context views are supported; quad focus views retain their original FOV and resolution. No extra per-frame GPU copy or full-screen shader pass was added for these controls.

The implementation uses standard OpenXR calls, with no Pimax- or SteamVR-specific switch. It is intended for native OpenXR and SteamVR OpenXR with SBoys3 alike; the game must actually run through OpenXR and honor the returned FOV. Window capture and a game's native OpenVR path cannot request extra rendering through this feature. Contract tests cover regular and asymmetric/canted stereo/quad configurations, but **physical validation in both runtime/driver combinations is still required**. Other FOV/visibility/foveation layers can affect the result.

Extra pixels and disabled visibility masks increase game GPU work; the cost depends on the scene and runtime. Recommendations add width up to a conservative 8192 pixels per eye for possible side-by-side D3D11 packing, retain larger native recommendations, and respect the runtime maximum. If a limit is reached, or the game ignores the larger resolution recommendation, added FOV has lower pixel density. Output resolution (for example 3840 × 2160) is independent of the game's eye texture size. Disable extra rendering, save and restart the game to return to the runtime's normal projection/masks.


## Controls

- **Crop left / right / top / bottom:** independently trim 0–45% per side, in 0.1% steps. Arrow keys adjust one step.
- **Crop reserve:** additional stabilization margin, separate from framing crop. Aspect fitting may trim the remaining image further.
- **Smoothing:** movement smoothing with 0.1% precision, separate from horizon leveling.
- **Horizon lock (OpenXR):** 0% preserves normal smoothed head roll; 100% levels roll using the upright Y-axis hemisphere of the submitted OpenXR layer space. This also supports iRacing poses whose upright direction is -Y. Look normally ahead when starting the preview or pressing Reset preview; the upright hemisphere is retained until reset. With **Adaptive curve** enabled (default), small tilts are suppressed more strongly while larger tilts retain some natural movement. At 50% strength, a 5° head tilt leaves about 0.5° and a 20° tilt leaves about 6.9° (away from vertical viewing). The response is continuous with no hard dead zone. Disable Adaptive curve to restore the previous linear response. Both modes preserve viewing direction. It fades near straight up/down, where a level horizon is undefined. Default: 0%. Inactive in image mode or when stabilization is disabled.
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

**OpenXR pose capture** uses submitted D3D11 images and matching poses, including stereo and context/foveal views. First-time setup uses **Install connection**, then one OpenXR game restart. The 0.6 layer is required; no tracking sender or network-delay adjustment is needed.

**Capture existing window** uses image analysis with no setup or game restart. Select the simulator window and leave it open and unminimized. It is independent of the VR runtime (OpenVR, Pimax OpenXR, SteamVR/SBoys3). It has no absolute headset-based horizon. Scene changes or missing cockpit detail may limit tracking. Window capture is SDR.

Use **Clean Preview** on the capture-card display and disable the old OBS stabilization filter for that source to avoid stabilizing twice.

If **Windows capture shutdown stalled** appears after quickly closing a source, restart only Horizon Lock Preview. iRacing can remain running. Cleanup remains bounded and does not block the UI.

Keep each preview directory intact. Settings are in preview.ini beside the EXE. The --demo option shows synthetic content. Automated tests cover GPU output, resolution, crop/aspect, horizon, mirror limits, settings, fullscreen, capture lifecycle and OpenXR ownership. Driving quality and GPU cost still depend on the live setup.

OpenXR performance: the viewer polls for GPU-ready frames independently of the selected capture rate and copies only the selected eye subrect. This avoids a second frame-rate gate and reduces GPU copy bandwidth for side-by-side stereo textures. The selected FPS remains a target; actual output depends on game frame rate and available GPU time.

## Stream Comfort and softer mirrored borders

**Stream Comfort** applies 60% smoothing, 50% horizon lock with Adaptive curve, 12% crop reserve, a 12% mirror limit and 70% mirror softness. Stabilization and mirroring are enabled. It keeps your source, eye, FPS, custom edge crops, per-eye reserves, Y offset, render-extension settings, aspect, output resolution and monitor. An enabled per-eye reserve profile continues to override the uniform reserve value. In image mode, the horizon setting is saved for OpenXR but has no effect on image stabilization.

Fine-tune any slider after applying the preset. **Undo preset** restores the stabilization tuning from before the first preset click in this app session, including if you apply it repeatedly. Subsequent framing/output changes are kept. The current tuning is saved between launches; the temporary undo snapshot is not.

**Mirror softness** ranges from 0–100% (default 60%). Zero restores the sharp legacy reflection. Higher values progressively soften and slightly darken the reflection outside the source image, with a smooth transition at the original image edge and a fade to black at the mirror limit. Original image pixels stay untouched. Both OpenXR and image capture use the effect. The extra four texture samples apply only to mirrored regions; live GPU cost depends on the visible border area.
