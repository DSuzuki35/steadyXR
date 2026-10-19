# SteadyXR Preview 0.10.0

Open **Spectator camera / effects…** for gaze framing, chat exclusion, natural head movement and local exposure/bloom/color. All new features start **off**. The master bypass preserves tuning. Camera and Visual pages have independent switches and explicit opt-in presets.

Enable gaze framing and chat exclusion, then press **Learn chat position** or **Ctrl+Alt+C** and look at chat: three seconds of countdown, then one second of stable samples. Adjust angular width/height. Head following continues while gaze toward chat is excluded. Relearn after recentering, changing eye/source/session, moving chat or loading a preset. Unsupported/stale gaze falls back to head stabilization. Actual Dream Air runtime compatibility still needs live validation.

Image effects run before separately captured overlays. **Compare image effects** uses the twenty-second A/B with identical framing. For minimum gaming-PC load, leave local image effects off and apply **SteadyXR Visual Effects** once to the capture-card source on the stream PC. The included **Spectator-Guide.md** explains the controls, SDR limits and measured costs. Install/upgrade the new connection and restart the OpenXR game for IPC 4.

Version **0.8.0** adds **Reset horizon**, global hotkeys, a temporary **20-second A/B comparison**, and compact capture/FPS feedback in the controls. The OBS drop-in folders now sit directly beside the standalone app in the delivered version folder. Existing settings and presets remain supported.

Horizon Lock VR became **SteadyXR** in 0.7.0, which introduced the on-demand **OpenXR layers…** editor with staged changes, backups and restore. The filenames are SteadyXR.exe, SteadyXR.ini, SteadyXR-OpenXR.dll, SteadyXR.ico and SteadyXR.png.

0.6.1 fixes disappearing sliders and makes Y offset use hidden stabilization reserves as well as aspect-ratio crop. It adds three tuning presets and named complete presets. Slider adjustments remain live; automatic settings writes are deferred until dragging settles.

0.5.0 adds a dark native control panel and an embedded application icon. The PNG and Windows ICO are also included beside the EXE. Existing settings migrate when upgrading the ready-to-use package.

When no new frame arrives for two seconds, the output becomes a plain black screen with “Waiting for OpenXR capture…” (or “Waiting for window capture…” in image mode). The next received frame restores the live preview automatically. Deliberate pause retains the last image. The waiting screen uses the GPU presentation path and does not recenter or alter the headset.

Standalone gaming-PC preview with OpenXR pose stabilization or existing-window capture. Preview stabilization affects only the stream output. Optional render extension asks the game to render a wider eye projection; the runtime receives that actual projection for correct headset composition. No OBS or video encoder is required on the gaming PC.

## Upgrade to 0.10.0

Close the previous app and start **Gaming-PC/Start-Preview.cmd**. The ready-to-use app lives in **Gaming-PC/SteadyXR-0.10.0** and inherits your previous SteadyXR.ini unchanged when its own INI is missing.

**The OpenXR connection must be upgraded for 0.10.0.** Its overlay-capable capture format is incompatible with older capture DLLs. Use **Upgrade connection** (or **Install connection**) in the new app, then restart the OpenXR game. Delivery itself never changes registrations or restarts anything. Existing-window capture needs neither connection setup nor a game restart. Keep older directories until their layer is unregistered.

For OBS, close OBS and copy **obs-plugins** and **data** from Gaming-PC/SteadyXR-0.10.0 directly into **C:\Program Files\obs-studio**. The same folders remain under Stream-PC and beside the app under preview in the ZIP. OBS uses the upgraded OpenXR layer without a running standalone app. Keep one active consumer: close/hide the standalone output while using OBS.

## Additional OpenXR overlays

Open **Overlay layers…** to inspect layers detected from the current game. None is the default. All supported includes future supported layers; individual checkboxes capture only the selected slots in this specific game session. The choices are included in complete presets, but a restarted game requires individual layers to be selected again. This avoids silently substituting a different overlay. Refresh the list after opening or closing an overlay. Layer labels describe type and dimensions, not an inferred program name.

Selected overlays use their own submitted geometry, positions and transparency and follow the stabilized game view, including 20-second A/B comparisons. No separate CPU video capture or encoder is used. Extra texture copying and drawing has a cost proportional to selected image sizes. See the packaged **docs/openxr-overlays.md** in the complete ZIP or the repository guide for setup, call order and format limits.

## Build validity

Each newly compiled SteadyXR build is valid for **30 days from its UTC build timestamp**. The app, bundled capture DLL and direct OBS source share the release's build timestamp and deadline, shown in the control-window title and included in SteadyXR-build.json for reference. Editing the INI or JSON, copying the files or restarting the app does not extend that deadline. This uses the PC's system UTC clock.

After expiry, the clean preview shows a black **Build expired** screen and stops capturing, including when the app was already running. Install a newer build to resume; settings remain available. The layer editor remains accessible so registrations can still be managed.

An expired capture DLL stops exporting frames and starts new OpenXR instances without extra render expansion. Existing game instances retain their configured projection until closed, so expiry cannot change the headset's field of view mid-session. The game is still forwarded to the runtime normally. A previously registered older DLL keeps its original behavior; the new preview enforces its own deadline regardless of the capture source. The separate legacy OBS filter and tracking sender are unchanged.

## OpenXR layer editor

Open **OpenXR layers…** in SteadyXR, or run **Gaming-PC/Start-Layer-Editor.cmd**. The editor runs only when opened, uses the Windows PowerShell/.NET Framework included with Windows, and performs no GPU capture. It does not load the listed third-party DLLs.

The default **64-bit (iRacing)** view shows system-wide and current-user implicit registrations followed by explicit registrations. Switch to 32-bit for older applications. Select a row for its technical name, full manifest/DLL paths and environment flags. Status describes the configuration visible to the editor; it cannot prove which layers an already-running game loaded. Elevated games ignore current-user registrations. Environment flags in the game process may differ from those visible to the editor.

- **Enable / Disable** changes the selected registration's flag.
- **Remove registration** unregisters the selected entry; its files stay on disk.
- **Move up / Move down** changes implicit discovery order within the same system/user group. System registrations precede user registrations and cannot be interleaved. Order can affect how layers interact, for example when one changes poses and another captures them. Explicit layers are requested and ordered by the game or `XR_ENABLE_API_LAYERS`, so their move buttons are disabled.
- **Register SteadyXR** stages the bundled layer for the current user if no SteadyXR/Horizon Lock registration is listed. Existing disabled entries can be enabled instead. The files are now XR_APILAYER_STEADYXR_preview.json and SteadyXR-OpenXR.dll. The internal `XR_APILAYER_HORIZONLOCK_preview` identifier is retained so old and new registrations are recognized as the same layer. Both filename generations are recognized by the editor and connection controls.
- **Apply changes** checks for external registry changes and write access, saves a verified backup, writes the requested data/order, and reads it back. On a write failure it attempts restoration and reports whether that restoration was verified. **Discard changes** leaves the registry alone.
- **Undo last apply** restores the previous configuration if it has not changed externally. **Restore backup…** stages a saved configuration for review and a new Apply. Select the backup's matching 32-bit/64-bit view first.
- **Administrator editor…** opens only the editor with Windows' elevation prompt for system-wide edits. The preview does not need administrator privileges. Apply or discard any pending edits before switching.

Backups live in **Gaming-PC/Layer-Backups**, outside versioned app folders. Keep this folder for recovery. Registry changes are verified but are not an operating-system transaction: avoid applying while an OpenXR game or another layer installer is starting. The editor refuses to replace keys with unexpected child keys when reordering. No runtime selection is changed. Restart the OpenXR game after applying layer changes.

Opening the editor, upgrading SteadyXR or unpacking files does not register or enable anything. A missing registration is shown as missing; the current registry alone does not reveal why it was removed.

Loader behavior: [Khronos OpenXR loader documentation](https://registry.khronos.org/OpenXR/specs/1.1/loader.html); [OpenXR API layer ordering guidance](https://fredemmott.com/blog/2024/11/25/best-practices-for-openxr-api-layers.html).

## Extra OpenXR scenery, asymmetric reserves and Y offset

Open **Render / edges…** in the main window. Choose the eye profile you want to edit; this does not switch the captured eye. Select the actual stream eye in the main window.

In window capture, the same main-window selector is named **Profile**. It selects which reserve settings apply to the existing window; it cannot change which eye the game shows in that window.

- **Use separate stabilization reserves:** four live sliders, 0–35% per edge in 0.1% steps. For a right-eye stream with more space kept on its inner side, try Left 2%, Right 20%; for the left eye, reverse these. The profile replaces the main uniform Crop reserve slider while enabled. Uniform reserve 10% equals 5% on each edge. Existing Crop left/right/top/bottom controls remain additional framing trims.
- **Y offset:** shift the displayed rectangle upward (negative) or downward (positive), at the same zoom. It uses the full captured image, including pixels hidden by stabilization reserves and framing crops. 0 restores automatic placement; ±100 reaches the available upper/lower image limit. Shifting uses up stabilization room on that side. With no hidden vertical pixels at all, there is no room to shift. This shared setting works in both capture modes.
- **Enable extra rendered area:** per-eye Extra left/right sliders, 0–30% each. Percentages refer to the original horizontal projection width, not degrees. Right-eye Extra left adds scenery toward the nose; left-eye Extra right is the corresponding inner side. **Both eyes inward: 10%** sets both inner sides to 10% and outer sides to zero.
- **Save for next game launch:** commits both render profiles. Restart the OpenXR game afterward, because view sizes and projections are configured at startup. Render drafts survive closing/reopening the panel within this app session. They are committed by this button or by loading a complete preset. Saving a complete preset includes the current render draft without activating it. Crop/reserve/Y settings are live and save automatically.

Cropping changes only the displayed output rectangle. It does not delete the original image behind it: the full captured eye remains available for stabilization. Mirroring supplies only pixels beyond the actual rendered boundaries. Aspect fitting can require further crop; separate reserves bias that extra crop toward the side with more reserve.

The layer expands `xrLocateViews` FOV tangents and recommends proportionally wider eye textures. It preserves poses, vertical FOV and submitted projection metadata, and makes visibility masks unavailable for expanded context views so hidden-area culling does not remove the new scenery. Stereo and quad context views are supported; quad focus views retain their original FOV and resolution. No extra per-frame GPU copy or full-screen shader pass was added for these controls.

The implementation uses standard OpenXR calls, with no Pimax- or SteamVR-specific switch. It is intended for native OpenXR and SteamVR OpenXR with SBoys3 alike; the game must actually run through OpenXR and honor the returned FOV. Window capture and a game's native OpenVR path cannot request extra rendering through this feature. Contract tests cover regular and asymmetric/canted stereo/quad configurations, but **physical validation in both runtime/driver combinations is still required**. Other FOV/visibility/foveation layers can affect the result.

Extra pixels and disabled visibility masks increase game GPU work; the cost depends on the scene and runtime. Recommendations add width up to a conservative 8192 pixels per eye for possible side-by-side D3D11 packing, retain larger native recommendations, and respect the runtime maximum. If a limit is reached, or the game ignores the larger resolution recommendation, added FOV has lower pixel density. Output resolution (for example 3840 × 2160) is independent of the game's eye texture size. Disable extra rendering, save and restart the game to return to the runtime's normal projection/masks.


## Controls

- **Crop left / right / top / bottom:** independently trim 0–45% per side, in 0.1% steps. Arrow keys adjust one step.
- **Crop reserve:** additional stabilization margin, separate from framing crop. Aspect fitting may trim the remaining image further.
- **Smoothing:** movement smoothing with 0.1% precision, separate from horizon leveling.
- **Horizon lock (OpenXR):** 0% preserves normal smoothed head roll; 100% levels roll using the upright Y-axis hemisphere of the submitted OpenXR layer space. This also supports iRacing poses whose upright direction is -Y. Hold the headset approximately upright and look normally ahead when starting the preview or pressing **Reset horizon**; the upright hemisphere is retained until reset. With **Adaptive curve** enabled (default), small tilts are suppressed more strongly while larger tilts retain some natural movement. At 50% strength, a 5° head tilt leaves about 0.5° and a 20° tilt leaves about 6.9° (away from vertical viewing). The response is continuous with no hard dead zone. Disable Adaptive curve to restore the previous linear response. Both modes preserve viewing direction. It fades near straight up/down, where a level horizon is undefined. Default: 0%. Inactive in image mode or when stabilization is disabled.
- **Mirror edges:** fill exposed borders with reflected source content.
- **Mirror limit:** maximum extension per source axis, as a percentage of source width/height. Default 10%; range 0–50%. Beyond the limit: black. Increase crop reserve or mirror limit if exposed edges appear. Mirroring does not reconstruct scenery.
- **Disable stabilization:** keep stabilization bypassed while capture continues. This is a saved setting.
- **Reset horizon:** hold the headset approximately upright, then reset the preview's upright reference while retaining smoothing without recentering the headset or changing tuning.
- **A/B comparison:** choose Original, 50/50 split (left stabilized, right original with identical framing), or Side by side (both complete views fitted without stretching) in **Hotkeys / A/B…**. The compare button uses that view for twenty seconds, retaining crop, reserves, aspect and output framing. The chosen view is saved; active comparison is temporary. Press again to return immediately. It returns automatically after twenty seconds, including if capture stops, and cancels on pause, reset, source or preset changes. The comparison never changes the saved bypass setting or becomes part of a preset.
- **Pause / GPU comparison:** stop capture and continuous rendering. Hiding/minimizing the output also pauses it. Compare VR frame times in the same scene.

Capture state, measured FPS, requested FPS and temporary comparison feedback appear in the control panel only. No status overlay is added to the live stream output.

## Hotkeys

These global shortcuts work while SteadyXR is running, including when the game has focus:

| Default shortcut | Action |
|---|---|
| Ctrl+Alt+R | Reset horizon; hold the headset approximately upright |
| Ctrl+Alt+P | Pause or resume capture |
| Ctrl+Alt+N | Load the next preset |
| Ctrl+Alt+B | Show the selected A/B view for twenty seconds; press again to return |

Open **Hotkeys / A/B…** to choose **Ctrl+Alt**, the alternate **Ctrl+Alt+Shift** combination, or **Off**. If another application owns a shortcut, the control panel reports the registration conflict; select the alternate combination or use the buttons.

Next preset cycles through **Natural**, **Stream Comfort**, **Strong Leveling**, then your saved presets alphabetically. Built-in presets change tuning while keeping framing; complete saved presets restore the settings described below. The direct OBS source uses OBS's own configurable **Settings → Hotkeys** entries instead of these standalone bindings.

## Output

Default internal render resolution: **3840 × 2160**. Enter width/height and click **Apply aspect / resolution**. Limits: 320–7680 × 180–4320. 4K costs more GPU work than 1080p; actual savings versus OBS have not been measured.

Select an aspect preset (16:9, 21:9, 32:9, 4:3, 1:1), or type a ratio such as 239:100, then apply. The image is fitted without stretching; letterboxing can appear in the output canvas or window.

This controls internal rendering. The HDMI capture-card signal still follows Windows display settings; the app does not change them.

**Start preview in fullscreen** defaults to enabled. Choose **Output monitor**; the choice is remembered. F11 or double-click toggles fullscreen; Escape exits it. Alt+Tab returns to the control panel.

## Capture and troubleshooting

**OpenXR pose capture** uses submitted D3D11 images and matching poses, including stereo and context/foveal views. First-time setup uses **Install connection**, then one OpenXR game restart. The 0.6 layer is required; no tracking sender or network-delay adjustment is needed.

**Capture existing window** uses image analysis with no setup or game restart. Select the simulator window and leave it open and unminimized. It is independent of the VR runtime (OpenVR, Pimax OpenXR, SteamVR/SBoys3). It has no absolute headset-based horizon. Scene changes or missing cockpit detail may limit tracking. Window capture is SDR.

Use **Clean Preview** on the capture-card display and disable the old OBS stabilization filter for that source to avoid stabilizing twice.

If **Windows capture shutdown stalled** appears after quickly closing a source, restart only SteadyXR Preview. iRacing can remain running. Cleanup remains bounded and does not block the UI.

Keep each preview directory intact. Settings are in SteadyXR.ini beside SteadyXR.exe. The new capture DLL reads SteadyXR.ini, with read-only fallback to preview.ini. An older registered DLL continues reading its own preview.ini, and explicit render saves still target that actual file. The --demo option shows synthetic content. Automated tests cover GPU output, resolution, crop/aspect, horizon, mirror limits, settings, fullscreen, capture lifecycle and OpenXR ownership. Driving quality and GPU cost still depend on the live setup.

OpenXR performance: the viewer polls for GPU-ready frames independently of the selected capture rate and copies only the selected eye subrect. This avoids a second frame-rate gate and reduces GPU copy bandwidth for side-by-side stereo textures. The selected FPS remains a target; actual output depends on game frame rate and available GPU time.

## Stream Comfort and softer mirrored borders

**Stream Comfort** applies 60% smoothing, 50% horizon lock with Adaptive curve, 12% crop reserve, a 12% mirror limit and 70% mirror softness. Stabilization and mirroring are enabled. It keeps your source, eye, FPS, custom edge crops, per-eye reserves, Y offset, render-extension settings, aspect, output resolution and monitor. An enabled per-eye reserve profile continues to override the uniform reserve value. In image mode, the horizon setting is saved for OpenXR but has no effect on image stabilization.

Fine-tune any slider after applying a tuning preset. **Undo tuning** restores stabilization tuning from before the first built-in preset click in this app session, including repeated clicks. Subsequent framing/output changes are kept. Loading a complete preset clears this temporary undo snapshot.

**Mirror softness** ranges from 0–100% (default 60%). Zero restores the sharp legacy reflection. Higher values progressively soften and slightly darken the reflection outside the source image, with a smooth transition at the original image edge and a fade to black at the mirror limit. Original image pixels stay untouched. Both OpenXR and image capture use the effect. The extra four texture samples apply only to mirrored regions; live GPU cost depends on the visible border area.

**Edge stretch** is under **Render / edges…** (and in the OBS source properties). It ranges from 0–100%, default 0 to preserve existing tuning. Toward the outer border, higher values blend mirrored scenery into stretched edge colors, reducing recognizable duplicate shapes. Start around 50% with Mirror softness enabled and compare. Actual image pixels stay unchanged and Mirror limit still controls the maximum extension; large missing areas can still look stretched. This reuses the existing texture samples and draw pass. Settings and presets store the value as MirrorStretchTenths.


## Presets

Open **Presets…** in the main window. **Natural**, **Stream Comfort** and **Strong Leveling** change smoothing, horizon/adaptive curve, uniform crop reserve and mirror tuning. They retain the source, eye, FPS, edge profiles, framing trims, Y offset, render expansion, output resolution/aspect and monitor. Natural uses 25% smoothing / 25% horizon; Strong Leveling uses 70% / 85%.

For your own preset, enter a name and click **Save new**. This saves a complete INI snapshot: all tuning, stabilization bypass, mode, selected source window identity, eye, FPS, both edge profiles, Y offset, both render-extension profiles (including current drafts), resolution, aspect, fullscreen startup preference and monitor. Valid edits in the aspect/resolution fields are included. Unknown future INI keys are preserved too. Pause, temporary original comparison, current fullscreen state and temporary smoothing/recenter history are session actions, not configuration; the fullscreen startup preference applies at the next preview launch.

Select a saved preset and click **Load** to restore it, **Overwrite selected** to replace it with current settings, or **Delete** to remove it. Live preview settings apply immediately; changed OpenXR render expansion applies at the next game launch. No automatic game restart occurs. A saved window source is matched by executable and title; if unavailable, select it when it is running.

Custom presets live in **Gaming-PC/Presets**, shared across versioned preview folders. Keep this folder when moving the app to another PC. Each preset is an INI file; saving a new name never overwrites an existing preset. Failed writes are reported rather than treated as successful saves.
