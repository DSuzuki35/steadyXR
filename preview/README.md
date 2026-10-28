# SteadyXR Preview 0.14.0

This community test build is free and has **no expiry**. Future paid releases are planned separately. The notice stays in the controls and never appears in the clean captured output.

Open **Spectator & effects** for gaze framing, chat exclusion, natural head movement and local exposure/bloom/color. All new features start **off**. The master bypass preserves tuning. Gaze & chat, Motion, Exposure, Bloom, Color and Director each have their own page. Camera and visual bypass switches preserve tuning; presets are explicit opt-ins.

Enable gaze framing and chat exclusion, then press **Look at chat now / Learn** or **Ctrl+Alt+C** and look at chat: three seconds of countdown, then one second of stable samples. Adjust angular width/height. Head following continues while gaze toward chat is excluded. Relearn after recentering, changing eye/source/session, moving chat or loading a preset. Unsupported/stale gaze falls back to head stabilization. Actual Dream Air runtime compatibility still needs live validation.

Image effects run before separately captured overlays. **A/B compares visual effects only** selects the twenty-second comparison with identical framing; start it with **Compare effects** in the main window. For minimum gaming-PC load, leave local image effects off and apply **SteadyXR Visual Effects** once to the capture-card source on the stream PC. The included **Spectator-Guide.md** explains the controls, SDR limits and measured costs. Pre-0.10 connections require an upgrade and game restart for IPC 4.

Version **0.8.0** adds **Reset horizon**, global hotkeys, a temporary **20-second A/B comparison**, and compact capture/FPS feedback in the controls. The OBS drop-in folders now sit directly beside the standalone app in the delivered version folder. Existing settings and presets remain supported.

Horizon Lock VR became **SteadyXR** in 0.7.0, which introduced the on-demand **OpenXR layers…** editor with staged changes, backups and restore. The filenames are SteadyXR.exe, SteadyXR.ini, SteadyXR-OpenXR.dll, SteadyXR.ico and SteadyXR.png.

0.6.1 fixes disappearing sliders and makes Y offset use hidden stabilization reserves as well as aspect-ratio crop. It adds three tuning presets and named complete presets. Slider adjustments remain live; automatic settings writes are deferred until dragging settles.

0.5.0 adds a dark native control panel and an embedded application icon. The PNG and Windows ICO are also included beside the EXE. Existing settings migrate when upgrading the ready-to-use package.

When no new frame arrives for two seconds, the output becomes a plain black screen with “Waiting for OpenXR capture…” (or “Waiting for window capture…” in image mode). The next received frame restores the live preview automatically. Deliberate pause retains the last image. The waiting screen uses the GPU presentation path and does not recenter or alter the headset.

Standalone gaming-PC preview with OpenXR pose stabilization or existing-window capture. Preview stabilization affects only the stream output. Optional render extension asks the game to render a wider eye projection; the runtime receives that actual projection for correct headset composition. No OBS or video encoder is required on the gaming PC.

## New in 0.14.0

D3D12 OpenXR sessions are detected automatically alongside D3D11. The native GPU capture preserves the submitted image and pose and uses the same framing, stabilization, overlay and OBS output. No extra setting is needed. Capture skips a frame if its bounded buffers are still busy. The game queue does not wait for the preview viewer. See [validation and signing](../docs/release-0.14.0-validation.md).

## From 0.13.1

The app, OBS source and newly compiled capture DLL have no expiry. **Install/Upgrade connection** registers SteadyXR system-wide (HKLM), requests Windows administrator approval when needed, and disables old per-user SteadyXR entries. Other layer enable states are preserved. With VectorXR present, SteadyXR is placed before it to capture the game's D3D11 images before Frame Generation creates a D3D12 session.

A startup fix skips automatic eye-gaze extension probing before VectorXR has initialized. Application-requested eye-gaze extensions remain unchanged. Without an application request, automatic gaze features may be unavailable in this combination; normal head-pose stabilization remains available. Preview captures real game frames, not VectorXR-generated intermediate frames.

The new DLL needs a fresh signature before the LMU/EAC test. Signing and hardware validation remain pending; see [release validation](../docs/release-0.14.0-validation.md). The old signed DLL is never patched in place.

## From 0.13.0

**Symmetric (both eyes)** in **Framing → Render & edge settings** mirrors render expansion between eyes: left-eye extra right follows right-eye extra left, and the two outer edges follow each other. Enabling it copies the currently selected eye to the other eye. Both eyes remain editable. Turning it off keeps the current values and restores independent editing. **Both eyes inward: 10%** also enables the lock. Click **Save for next game launch**, then restart the game. The lock is stored in settings and complete presets; old settings retain their independent values until you enable it.

**LMU requires equal recommended image dimensions for both eyes.** Keep Symmetric enabled when using extra render area in LMU. Cropping and stabilization reserves remain independent.

The earlier 0.13.0 package preserved the signed 0.12.0 DLL. It remains untouched as a fallback; its old embedded expiry is unaffected by the new version.

## From 0.12.0

**Director** adds optional lookahead stabilization, automatic crop and speed framing for iRacing and Le Mans Ultimate. Lookahead retains at most four delayed frames within a 256 MiB texture budget and displays actual video delay for audio sync. Automatic crop and speed framing also work with existing-window capture. All three default off. See [Viewer camera controls](../docs/viewer-director.md) for costs, limits and two-PC setup. Live driving validation is still pending.

## Find your controls

- **Capture:** game window or OpenXR mode, eye/profile, frame rate, connection setup and layer tools.
- **Camera:** smoothing, horizon lock, reset and the Stream Comfort starting point.
- **Framing:** crop reserve, four trims, mirrored edges and **Render & edge settings…**.
- **Output:** aspect ratio, resolution, monitor and fullscreen startup.
- **Spectator & effects:** gaze/chat learning, natural motion, exposure, bloom, color and Director camera controls. This button stays visible on every main page.

Open preview and pause stay at the top; comparison, presets and hotkeys stay at the bottom. Tab/Shift+Tab navigate native controls; Space activates the focused button or checkbox. Numeric effects settings apply when you leave the field. Switching pages preserves tuning and does not activate effects.

## Upgrade to 0.14.0

Close the previous app and start **Gaming-PC/Start-Preview.cmd**. The ready-to-use app lives in **Gaming-PC/SteadyXR-0.14.0** and inherits your previous SteadyXR.ini unchanged when its own INI is missing.

**0.14.0 uses IPC 4 but requires the new capture DLL for D3D12 support.** After signing, click **Capture → Upgrade connection**, accept the Windows administrator prompt, and restart the OpenXR game. The preview itself runs without elevation. Existing-window capture needs no connection or game restart. Keep older directories until their layer is disabled or unregistered. Packaging never changes live registrations.

For OBS, close OBS and copy **obs-plugins** and **data** from the separate **SteadyXR-0.14.0-OBS-Windows-x64.zip** (or Stream-PC) directly into **C:\Program Files\obs-studio**. The standalone community ZIP excludes OBS binaries. OBS uses the upgraded OpenXR layer without a running standalone app. Keep one active consumer: close/hide the standalone output while using OBS.

## Additional OpenXR overlays

Open **Capture → Overlay layers…** to inspect layers detected from the current game. None is the default. All supported includes future supported layers; individual checkboxes capture only the selected slots in this specific game session. The choices are included in complete presets, but a restarted game requires individual layers to be selected again. This avoids silently substituting a different overlay. Refresh the list after opening or closing an overlay. Layer labels describe type and dimensions, not an inferred program name.

Selected overlays use their own submitted geometry, positions and transparency and follow the stabilized game view, including 20-second A/B comparisons. No separate CPU video capture or encoder is used. Extra texture copying and drawing has a cost proportional to selected image sizes. See the packaged **docs/openxr-overlays.md** in the complete ZIP or the repository guide for setup, call order and format limits.

## Build validity

The app, new capture DLL and direct OBS source have **no expiry**. `SteadyXR-build.json` records the build timestamp and explicitly marks expiration disabled. Old signed DLLs retain their embedded deadlines until replaced by the newly signed capture DLL. No clock changes or settings edits are needed.

## OpenXR layer editor

Open **Capture → Layer order…** in SteadyXR, or run **Gaming-PC/Start-Layer-Editor.cmd**. The editor runs only when opened, uses the Windows PowerShell/.NET Framework included with Windows, and performs no GPU capture. It does not load the listed third-party DLLs.

The default **64-bit (iRacing)** view shows system-wide and current-user implicit registrations followed by explicit registrations. Switch to 32-bit for older applications. Select a row for its technical name, full manifest/DLL paths and environment flags. Status describes the configuration visible to the editor; it cannot prove which layers an already-running game loaded. Elevated games ignore current-user registrations. Environment flags in the game process may differ from those visible to the editor.

- **Enable / Disable** changes the selected registration's flag.
- **Remove registration** unregisters the selected entry; its files stay on disk.
- **Move up / Move down** changes implicit discovery order within the same system/user group. System registrations precede user registrations and cannot be interleaved. Order can affect how layers interact, for example when one changes poses and another captures them. Explicit layers are requested and ordered by the game or `XR_ENABLE_API_LAYERS`, so their move buttons are disabled.
- **Register SteadyXR** uses the app's system-wide installer and requests Windows administrator approval when required. Legacy per-user SteadyXR registrations must be migrated through this installer rather than enabled again. The files are now XR_APILAYER_STEADYXR_preview.json and SteadyXR-OpenXR.dll. The internal `XR_APILAYER_HORIZONLOCK_preview` identifier is retained so old and new registrations are recognized as the same layer. Both filename generations are recognized by the editor and connection controls.
- **Apply changes** checks for external registry changes and write access, saves a verified backup, writes the requested data/order, and reads it back. On a write failure it attempts restoration and reports whether that restoration was verified. **Discard changes** leaves the registry alone.
- **Undo last apply** restores the previous configuration if it has not changed externally. **Restore backup…** stages a saved configuration for review and a new Apply. Select the backup's matching 32-bit/64-bit view first.
- **Administrator editor…** opens only the editor with Windows' elevation prompt for system-wide edits. The preview does not need administrator privileges. Apply or discard any pending edits before switching.

Backups live in **Gaming-PC/Layer-Backups**, outside versioned app folders. Keep this folder for recovery. Registry changes are verified but are not an operating-system transaction: avoid applying while an OpenXR game or another layer installer is starting. The editor refuses to replace keys with unexpected child keys when reordering. No runtime selection is changed. Restart the OpenXR game after applying layer changes.

Opening the editor, upgrading SteadyXR or unpacking files does not register or enable anything. A missing registration is shown as missing; the current registry alone does not reveal why it was removed.

Loader behavior: [Khronos OpenXR loader documentation](https://registry.khronos.org/OpenXR/specs/1.1/loader.html); [OpenXR API layer ordering guidance](https://fredemmott.com/blog/2024/11/25/best-practices-for-openxr-api-layers.html).

## Extra OpenXR scenery, asymmetric reserves and Y offset

Open **Framing → Render & edge settings…**. Choose the eye profile you want to edit; this does not switch the captured eye. Select the actual stream eye in the main window.

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
