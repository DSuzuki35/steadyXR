# HorizonLock VR

An OBS effect filter that stabilizes a capture-card picture on your stream PC. Choose **SteamVR pose** with optional horizon leveling, **Image-based microjitter** using the picture alone, or **Hybrid** combining pose with residual image cleanup. The pose mode includes a lightweight tracking sender for the gaming PC.

The filter appears as **VR Stabilizer** in OBS. Windows downloads include both components and complete source code.

Downloads: [Windows releases](https://github.com/DSuzuki35/horizonlock-vr/releases). The repository and its releases are private; sign in to GitHub with an account that has access.

Version **0.5.1 — experimental**, targeting **Windows 11 x64 and OBS 32.2.2**. Pose mode is designed for Dream Air + SBoys3/SteamVR, including games using either SteamVR's OpenXR path or OpenVR. The sender never changes the active runtime, recenters the headset, or alters the headset image. Image mode requires no headset connection, FOV calibration or tracking sender for stabilization; the optional photo timing display can connect a sender for diagnostics.

For pose mode, SteamVR must be supplying headset tracking. Native Pimax OpenXR without SteamVR needs a future tracking provider; it is not included in this release. Image mode works independently of the runtime because it analyzes the captured video.

The local v0.3.4 Windows test package uses LLVM-MinGW with static runtime libraries. Twelve automated suites cover core math, image motion, per-axis smoothing/crop, calibration and collection, timing displays, the native sender window, sender math, launcher failures and real UDP integration. Version 0.3.3 fixes a SteamVR startup crash in the MinGW sender by retrieving the eye transform through Valve's C function table. Native OBS GPU/module tests exercise SDR sources, full 4K output and bounded 480×270 analysis. Synthetic results do not guarantee racing-footage quality; the live Dream Air / Elgato / iRacing / LMU setup still needs validation. The published v0.3.0 package uses Visual Studio binaries from [GitHub's build and test runs](https://github.com/DSuzuki35/horizonlock-vr/actions).

## Standalone gaming-PC preview (new in 0.4.0)

`Start-Preview.cmd` opens an OBS-free native app. The default **Vorschaufenster aufnehmen (ohne Neustart)** captures the existing iRacing preview with Windows Graphics Capture, stabilizes it with image analysis and displays a clean output window. No game restart or OpenXR setup is required. Select the preview window, move **Clean Preview** to the capture-card display and press F11. Keep the original window open and not minimized. This path is independent of the headset runtime, including OpenVR, Pimax OpenXR and SteamVR/SBoys3.

Output defaults to an exact **3840×2160** render canvas, with configurable resolution, aspect ratio, four crop controls, OpenXR horizon leveling, mirror limits and automatic fullscreen on a saved monitor. Controls and messages are English. Small asynchronous GPU readbacks feed window-mode image analysis; results remain paired with retained frames. **Pause / GPU comparison** stops capture and continuous rendering. GPU savings versus OBS have not been measured. Disable the OBS stabilizer on an already stabilized source. Window capture targets SDR. [Setup and controls](preview/README.md).

The optional **OpenXR-Pose direkt** mode captures submitted eye images with their corresponding poses. Only that mode requires clicking **Verbindung einrichten** and restarting iRacing in **OpenXR / D3D11**. It supports stereo/context-foveal quad views and is runtime-independent at the OpenXR layer level, including the intended SteamVR OpenXR + SBoys3 path. It does not change headset submissions. Native tests use a fake OpenXR runtime; real Pimax/SBoys3 compatibility and overhead still need live validation.

To build only the preview: run `python scripts/prepare-preview-deps.py`, configure CMake with `-DVRST_BUILD_PREVIEW=ON -DVRST_OPENXR_DIR=.deps/openxr-sdk` and a Windows x64 toolchain, then build. The source ZIP includes pinned headers under `third_party/openxr`, which CMake detects automatically. Tests need no global layer registration and do not switch runtimes.

## Install the downloaded ZIP

Extract the **entire ZIP** first. You can copy the same extracted folder to both PCs.

**Updating a manual installation:** close OBS, open the package's `obs-install` folder and copy its `obs-plugins` and `data` folders into your existing OBS installation (normally `C:\Program Files\obs-studio`), replacing the old VR Stabilizer files. Use the same installation location as before so OBS loads one copy. For installations made with `Install-OBS.cmd`, run that installer again instead. Existing filter settings remain in your OBS scene collection.

**Stream PC**

1. Close OBS. Double-click `Install-OBS.cmd` and accept the Windows elevation prompt. The installer copies only this plugin into `%PROGRAMDATA%\obs-studio\plugins\vr-stabilizer`.
2. Open OBS and select your Elgato/capture-card source. Open **Filters → Effect Filters → + → VR Stabilizer**.
3. Choose **Stabilization algorithm**. For **SteamVR pose** or **Hybrid**, enter the gaming PC's IPv4 address and port **42424**. For **Image-based microjitter**, skip the gaming-PC sender setup below.
4. Start with the defaults below. Click **Refresh diagnostics** to inspect pose alignment or image-feature confidence and the correction limit.
5. Set the OBS canvas/output to your intended 16:9 resolution. With the source selected, use **Transform → Fit to screen** after changing input trim or output shape.

**Gaming PC — pose and hybrid modes**

1. Start SBoys3 / SteamVR and the game as usual.
2. Double-click `Start-Tracking.cmd`. It starts `sender/vrst-sender.exe` and prints the PC's IPv4 addresses. Keep the console open while streaming.
3. If Windows Firewall prompts for the sender, allow it on your private home network. The stream PC must be able to reach UDP 42424 on the gaming PC. Do not set up internet port forwarding.
4. Continue sending your normal desktop mirror through the capture card. The sender does not capture or encode video.

Pose mode reads tracking independently of video transport. The Elgato model is not required for installation, but capture settings affect the tracking delay you need. Close the sender console or press Ctrl+C to stop it. Updating from v0.1.0 or v0.2.0 keeps your saved settings. The default remains pose mode. Separate axes, faster turn following, automatic crop and A/B comparison start off; existing smoothing behavior is preserved.

For portable OBS, copy `obs-plugin/bin/64bit/vr-stabilizer.dll` to its `obs-plugins/64bit` directory and `obs-plugin/data/*` to `data/obs-plugins/vr-stabilizer/` instead of using the installer. `Uninstall-OBS.cmd` removes the normal installation while OBS is closed.

## Controls

| Control | Meaning | Starting value |
|---|---|---|
| Stabilization algorithm | SteamVR pose, Image-based microjitter, or Hybrid | SteamVR pose |
| A/B: show unstabilized picture | Same frame, framing and latency with correction hidden; estimation continues | Off |
| Horizontal / vertical FOV | Angular view of the incoming image after input trim | Enter/calibrate your captured view |
| Use automatic full-eye FOV | Use the selected eye projection reported by SteamVR; only correct for a complete eye image | Off |
| Tracking delay | Look back this many milliseconds from filter render time; includes capture-card and OBS buffering | 80 ms |
| Smoothing | Common motion smoothing; zero disables smoothing, while horizon leveling remains independent | 40 |
| Use separate smoothing for each axis | Override common smoothing with yaw/horizontal, pitch/vertical and roll values | Off; each axis 40 |
| Follow deliberate turns faster | Reduce lag during sustained turns using signed motion averaging | 0% |
| Turn-follow reference speed | Speed where the follow amount reaches full effect | 90°/s |
| Horizon leveling (pose) | Cancel sustained head roll while preserving look direction | 0% |
| Maximum horizon correction (pose) | Limit how much roll leveling may remove | 45° |
| Maximum image shift (image) | Limit correction per axis, relative to trimmed input | 1% |
| Maximum image roll (image) | Limit small roll corrections | 0.5° |
| Correct image roll (image) | Estimate rotation as well as translation | On |
| Tracking region, four margins (image) | Select the stationary cockpit area used for analysis | Left/right 10%, top 50%, bottom 10% |
| Stabilization crop | Reserve image margin to absorb head movement; auto crop uses this as its minimum | 10% |
| Automatically add crop | Add crop when correction needs more margin, then return slowly | Off |
| Maximum additional crop | Extra percentage points, with a 40% total crop ceiling | 10% |
| Crop increase speed | Percentage points per second; relaxation runs at half this rate | 3%/s |
| Framing zoom | Additional final crop/zoom | 0% |
| Fill 16:9 | Center-crop the output to 16:9 without stretching | On |
| Framing horizontal/vertical | Shift the final framing within available image margin | 0% |
| Input trim, four edges | Remove black bars or window borders before interpreting the image's FOV | 0% |
| Reset smoothing | Clear motion history without changing any controls | As needed |

The **Tracking delay** slider in pose/hybrid modes does not buffer video or audio. It changes which older tracking pose is used for the currently rendered image. Upstream video delays and capture buffering changes require recalibration. Image and hybrid modes add their own fixed one-OBS-frame video delay, described below. Put VR Stabilizer first among image-processing filters where practical.

Crop values reduce the total span of each axis. For example, a 10% stabilization crop keeps 90% of the width and height before aspect framing, approximately 5% off each edge. Crops combine multiplicatively. Input trimming, stabilization crop, final zoom and aspect fill are combined into one GPU reprojection sample. More framing zoom also leaves more room for correction. Moving framing toward an edge reduces the available motion margin on that side.

At a large/fast head turn the filter gradually limits correction to what fits inside the source. It follows the head when there is insufficient margin. It cannot reveal pixels the game did not render. On tracking loss it keeps the usual framing and bypasses rotation correction. Recenter jumps reset the smoother. Intentional slow turns remain visible.

Diagnostics are refreshed when the properties open or when **Refresh diagnostics** is clicked. In pose mode, “Selected pose age” includes tracking delay and RTT is the network round trip. Image mode reports feature confidence, inliers, analysis dimensions and processing time. A correction limit below 100% means the crop margin constrained the requested correction.

## Mirrored edges when crop runs out

Enable **Mirror image edges** beside the crop controls to keep correction active beyond the available crop in pose, image and hybrid modes. Missing pixels are reflected from the trimmed source after the final transform. Input trim stays excluded, including at bilinear sample boundaries. The option defaults off and is saved in presets. Automatic crop still seeks genuine image margin and reduces the amount of reflection needed.

This can preserve smoother movement when crop would otherwise limit correction. It does not reconstruct unseen scenery, correct a wrong motion estimate, or hide a game view reset. Large movements expose recognizable reflections. Image shift/roll limits remain in effect, and extreme pose correction remains bounded before the projection becomes singular. Hybrid analysis uses only genuine source pixels; reflected or clamped areas cannot supply tracked features or trigger a false scene cut solely because they leave the frame.

Version 0.3.4 adds CPU motion/mask tests and native OBS pixel readback checks for all rendering paths, trim boundaries, analysis validity, presets and fallback behavior. In a synthetic alternating-jitter test with zero crop, the applied output motion RMS falls from 1.4000 to 0.0331 analysis pixels with mirroring. This measures removal of the crop constraint, not a guaranteed improvement on racing footage. Install both the new DLL and shader/data files together.

## Image-based microjitter

1. Choose **Image-based microjitter**. FOV, network, tracking-delay and horizon controls are hidden because this mode does not use them.
2. Remove borders using **Input trim**. Choose a **Tracking region** containing fixed cockpit structure, such as dashboard trim or a windshield pillar. The four margins refer to the trimmed image. Defaults select the lower part: 10–90% horizontally and 50–90% vertically.
3. Exclude animated steering wheels, mirrors, overlays, other cars and moving track scenery. Leave a reasonably large, textured area. Very narrow or flat regions may have too few safe feature patches; diagnostics then show weak tracking and the correction gently releases.
4. Start with smoothing 40, maximum shift 1%, maximum roll 0.5° and stabilization crop 10%. Compare against smoothing 0, then increase smoothing gradually. Increase correction limits only if visible jitter needs it.

The algorithm tracks subpixel image features, rejects inconsistent matches and estimates small shifts/roll. It smooths that measured motion, caps the correction, and limits it again to available crop when mirrored edges are disabled. Uniform exposure changes are removed when checking feature matches. Brief weak matches release the existing correction over 150 ms instead of snapping to zero; accepted tracking ramps back in over 100 ms. This transition does not invent motion for calibration. Automatic crop accounts for the fading correction. Detected scene changes, unavailable analysis and ROI, size, mode, trim or reset changes clear history immediately.

Full-resolution video remains on the GPU. A filtered grayscale copy, at most 480×270, is used for analysis. The measured correction is applied to that exact captured frame. This adds **one OBS video frame after warm-up: 16.7 ms at 60 fps**. Audio is not delayed automatically; account for this when aligning audio or other cameras. Missing/hidden frames restart the buffer. No gaming-PC clock or capture-card delay calibration is needed for image mode.

This is a small 2D correction, not full perspective/depth reconstruction. Turning off game neck movement helps, but image analysis still cannot distinguish all head motion from car motion, lighting changes or parallax. It cannot recover missing frames, undo motion blur, or remove 90-to-60 cadence judder. Test with the car parked, then on a lap.

## Horizon leveling for head tilt

Choose **SteamVR pose**, connect the sender and calibrate FOV/delay as below. Increase **Horizon leveling** to remove the sustained roll caused by tilting your head into a corner. At 100%, the target view is upright relative to SteamVR tracking up, subject to the maximum-angle and crop limits. Yaw/pitch viewing direction is preserved; existing motion smoothing can still smooth those directions.

Set smoothing 0 if you want only horizon leveling. Larger tilts require more crop; the filter reduces correction when it would expose missing pixels. Leveling fades near straight-up/down gaze where roll is ambiguous. It cancels headset roll, not unknown roll from the game's car camera or banking. Hybrid mode also supports horizon leveling, followed by residual image cleanup.

## Hybrid: pose plus residual image cleanup

Choose **Hybrid: pose + image cleanup**, start the gaming-PC sender and calibrate the incoming mirror's FOV/delay. The filter associates a pose correction with each captured frame, analyzes that pose-corrected image, then removes small remaining image motion. It composes both transforms into the raw texture for the final GPU sample. It does not simply stack two independent corrections of the raw video.

The requested stabilization crop is shared between the two stages: each stage keeps the square root of the requested total keep fraction. A 10% total crop therefore uses about 5.13% per stage; combined, it still keeps 90%. Input trim applies first; final zoom, positioning and 16:9 framing apply last. Each stage retains its own safety limiter. This allocation can limit large pose movements sooner than pose-only mode at the same crop.

Hybrid adds one OBS video frame, like image mode. The pose transform travels with that exact buffered frame. Tracking validity changes reset residual history. With no valid pose, image tracking can still operate, but horizon leveling requires pose tracking. Keep the existing image-region margins on fixed cockpit structure. A visual tracking mask is not part of this release.

## Guided delay and FOV calibration

1. Start the tracking sender and enter the gaming-PC address. Keep the car parked with a clear, textured cockpit region. The same four region margins used by image mode select its observations; there is no new mask editor.
2. Click **Start parked-car calibration**. The saved algorithm stays unchanged. During collection, the filter temporarily displays a normally framed raw picture with one buffered video frame and no stabilization. Calibration also connects the sender when your saved algorithm is image mode.
3. Make varied, small left/right head movements, then varied up/down movements for about 15–25 seconds. Avoid perfectly repetitive oscillations and do not move the car. Both axes need enough motion. Click **Refresh diagnostics** to see elapsed time and collected observations; collection stops automatically after 30 seconds.
4. Click **Finish and analyze**. A valid result shows suggested delay, effective horizontal/vertical FOV and confidence. Weak texture, missing tracking, insufficient movement or ambiguous timing produce an explanation instead. Your current settings are preserved.
5. Click **Apply suggested delay and FOV** only after reviewing the result. This updates delay/HFOV/VFOV and switches off automatic full-eye FOV. Compare the result using A/B, then refine manually if needed.

The solver aligns raw observed image motion with clock-corrected pose times, searching the entire 0–1000 ms range in 1 ms steps. It compares motion accumulated over sliding windows of approximately 60 ms, so repeated capture frames and timing noise do not dominate individual-frame matches. It uses each buffered frame's capture time, not the later analysis time. A fitted recording reports horizontal/vertical match, fit error and the rates of collected video observations and poses. These are collection rates, not hardware refresh rates; poor or ambiguous matches still produce no suggestion. Source, size or tuning changes invalidate stale observations/suggestions and discard their recorded data. Detected scene cuts also invalidate them while image analysis is running. Pose-only mode does not keep image analysis running after collection; changes to the game mirror or camera view that leave the OBS source/size unchanged require a fresh calibration even if an old suggestion is still shown. It estimates an effective small-motion projection of the captured mirror; this is not a measurement of Dream Air optics. Scene parallax, mirror processing and render/capture cadence can still bias results. Synthetic calibration tests establish implementation accuracy, not a live hardware accuracy guarantee.

## Photo timing check with both monitors

1. On the gaming PC, close the previous sender console with Ctrl+C and start **`Start-Delay-Check.cmd`** from the new package. It runs the normal sender with a movable yellow desktop panel. Drag its title bar to position it on the display sent through HDMI/Elgato. Keep the panel inside the retained picture and outside the **Image tracking region**, so its moving digits cannot influence stabilization/calibration. If exclusive fullscreen hides the desktop panel, use a borderless/windowed mirror.
2. Update the filter on the stream PC and enable **Photo timing overlay**. Enter the gaming PC address/port if needed. A cyan panel appears in the filter's video output, after stabilization. It also works in image-only mode, where it explicitly says no headset pose is applied.
3. Keep the car parked. Show the gaming display and the stream PC's OBS preview side by side and take several phone photos containing both monitors. The gaming panel should also be readable inside the captured picture on the stream monitor.
4. Compare the yellow panel directly on the gaming display with its delayed copy in the OBS picture to estimate visible image delay. Compare the captured yellow **POSE** with the cyan **USED** to investigate tracking alignment. These values use the same original gaming-clock epoch; check that **SESSION** agrees. For an interpolated live pose, use **BLEND AT** as the effective selected time and retain **USED** as the earlier packet's stamp. This is a manual diagnostic; it never changes Tracking delay automatically.
5. After measuring, close the yellow window and uncheck **Photo timing overlay**. Closing the panel leaves the tracking sender running. Ctrl+C in the sender console stops tracking. To show the gaming panel again, restart the sender with `Start-Delay-Check.cmd`.

| Label | Meaning |
| --- | --- |
| NOW | Current gaming-clock seconds; OBS maps its local clock to that same clock |
| POSE / LATEST | The sender's observed pose / the latest accepted pose packet at OBS |
| USED | Original packet timestamp/sequence selected for the displayed image, including hybrid's buffered frame |
| BLEND AT | Effective time when the used live pose was interpolated between packets |
| SESSION / OLD SESSION | Sender run identity; a previous buffered session is explicitly labeled |
| DELAY / VIDEO BUFFER | Requested tracking lookback / local age of the buffered image in this filter |
| RECEIVE AGE / RTT | Time since the last accepted packet / clock-sync round-trip time |
| NONE / STALE / WAITING | No pose correction applied, old tracking data, or unavailable clock/data |

The desktop panel timestamps a **pose observation**, not a verified pose-to-iRacing-mirror frame association. It is composited independently of the game and does not tag that game's render submission. A phone photo also includes display/preview latency, display scanout and camera exposure timing. At 60 fps a new picture arrives about every 16.7 ms; three decimal places do not imply 1 ms alignment accuracy. The filter's own image/hybrid buffer adds another video frame, so a photographed end-to-end delay is not automatically the correct Tracking delay setting. A/B comparison and calibration display **NONE** because their output applies no headset correction. No source frame-ID decoder or automatic delay measurement is included in this version.

## A/B comparison, presets and hotkeys

Use **A/B: show unstabilized picture** to compare fairly: crop, framing, output size and buffering stay the same, while only final correction is hidden. Motion analysis and automatic crop keep running, so changing A/B does not cold-start the estimator. This differs from disabling the entire OBS filter, which also removes its framing and buffer.

Under **Game / car preset**, type a name such as `iRacing / GT3` or `LMU / Porsche`, then **Save preset**. Choose a saved name and **Load preset** to restore its tuning, or **Delete preset** to remove it. Saving an existing name replaces it. Up to 32 presets persist with that filter in OBS scene data. They include mode, FOV, delay, image region, axis/follow settings and crop/framing; the gaming-PC connection and A/B state remain unchanged. Presets are your saved calibrations, not preconfigured claims about a game's mirror.

In **OBS Settings → Hotkeys**, bind **HorizonLock: toggle matched A/B comparison**, **Reset smoothing**, and **HorizonLock: load next saved preset**. Bindings are saved with the filter in your scene data and restored when it loads. Next-preset cycles names alphabetically. Loading a preset may change algorithm/buffering; use it between comparable shots and recheck audio alignment when switching between pose-only and buffered modes.

## Per-axis motion and automatic crop

Enable **Use separate smoothing for each axis** to control yaw/horizontal, pitch/vertical and roll independently. A zero value follows that axis directly. Horizon leveling remains separate. **Follow deliberate turns faster** reduces lag during sustained signed movement; it cannot perfectly classify intent. Start low and compare a quick mirror glance with alternating vibration. The reference speed uses headset degrees/second in pose processing; image processing approximates translation using a 90° field, so treat it as a sensitivity setting there.

Enable **Automatically add crop** to let the filter grow from your base stabilization crop when more correction space is needed. **Maximum additional crop** and the 40% total ceiling limit the zoom. Crop rises at the configured speed and relaxes at half that speed; sudden motion is still limited to currently available pixels while the crop catches up. This prevents instant zoom jumps. Diagnostics show the current effective crop. Keep it off if any framing movement is distracting.

## Calibrate pose mode with the car parked

1. **Choose the source and eye first.** The sender defaults to the left eye. If capturing the right eye, run `Start-Tracking.cmd --eye right`. Avoid a blended/stereo view; a single rotational camera model cannot accurately stabilize both eyes together.
2. **Remove borders.** Use Input trim to isolate the actual eye image. Keep the capture resolution and mirror window size fixed afterward.
3. **Set projection.** Keep automatic FOV off for an unknown game mirror. Use SBoys3's per-eye HFOV/VFOV as an initial estimate, then adjust for any crop already performed by the game. A headset's combined binocular HFOV is not the correct value for one eye.
4. **Check tracking.** Refresh diagnostics. Expect ready clock sync and observed render-frame poses, or the explicitly labeled live fallback. Let history fill for at least your delay after starting/resetting.
5. **Find delay.** Use smoothing around 50 and stabilization crop around 15%. Make small, repeated left/right head movements while parked. Sweep delay in roughly 10 ms increments, then refine in 1–2 ms increments where residual jitter is lowest. Repeat with small up/down movements. Do not calibrate while driving, because vehicle movement complicates visual judgment.
6. **Refine FOV.** Delay error tends to leave a mistimed or oscillating correction. A FOV mismatch tends to under/over-correct the amount of movement. Alternate small delay and FOV adjustments rather than assuming one sweep is conclusive.
7. **Set the look.** Reduce/increase smoothing and crop to taste. Enable Fill 16:9, then adjust framing zoom and position. Forcing the view to stay almost fixed requires more crop and limits deliberate turns.

If alignment changes when the game drops below its usual frame rate, try `Start-Tracking.cmd --mode live`, then recalibrate the delay. There is no universally correct offset for an independently captured desktop mirror. The 90 Hz headset refresh rate also does not guarantee that the game renders 90 new frames every second.

## iRacing, LMU and a more predictable SBoys3 source

This version deliberately does not assume how iRacing or LMU converts an eye texture into its 16:9 desktop window. A 16:9 window can contain a cropped, scaled, shifted or letterboxed eye image. Knowing the driver's full-eye FOV does not tell us which of those mappings the game chose.

**A single-eye SteamVR capture is the better calibration reference.** Try SteamVR's VR View configured for one eye and compare it with the game mirror. For a direct eye capture, an OBS OpenVR source on the gaming PC can be projected fullscreen onto the display output connected to the capture card. OBS does not have to record or encode there. The [OpenXR + OpenVR OBS Mirror project](https://github.com/elliotttate/OpenXR-Layer-OBSMirror) provides such an OpenVR source; its OpenVR fallback does not need an OpenXR API layer for capture. Our stream-PC filter supplies the stabilization.

Keep the full eye visible during calibration. A fullscreen projector may add bars to preserve the eye's aspect ratio; remove those with Input trim here. If the image truly contains the full selected eye, automatic full-eye FOV can then use the driver's asymmetric projection and eye rotation. A “fill canvas” crop on the gaming PC changes that assumption. Hidden-area masks can also leave unavailable corners; reserve enough crop to exclude them.

Using a known single-eye source solves projection uncertainty; it does **not** make independent video and tracking exactly synchronized. A future sender owning a matched image/pose capture could stamp frame identifiers into the outgoing video. That requires additional capture integration and is not claimed by this version.

## What is stabilized

Pose mode smooths headset yaw, pitch and roll, with optional sustained roll leveling. Image mode smooths estimated 2D translation and roll in the selected region. Hybrid measures that image motion after pose correction. Neither reconstructs depth or corrects positional parallax across the whole cockpit. Baked-in HUDs, motion blur, missing edge pixels and foveated rendering artifacts cannot be undone. 90-to-60 motion cadence can remain visible independently of head stabilization.

The sender prefers SteamVR compositor frame-history orientation when available and composes the selected eye's rotation. These packets use the local **time the helper first observed the frame**, not an assumed common epoch for SteamVR's internal timestamp. In live fallback it sends current headset orientation. Neither mode proves which pose produced an arbitrary HDMI frame. The plugin's clock sync aligns the two PCs; manual delay handles the remaining empirical video alignment.

## Sender commands

```bat
Start-Tracking.cmd --help
Start-Tracking.cmd --eye right
Start-Tracking.cmd --mode live
Start-Tracking.cmd --port 42425
Start-Tracking.cmd --demo
Start-Delay-Check.cmd
```

Demo mode emits synthetic tracking for connection tests; it does not follow a headset or provide matching game footage. Do not use it to stabilize gameplay. Keep `openvr_api.dll` beside `vrst-sender.exe`.

For an opt-in hardware check with SteamVR running, use `python sender/steamvr_smoke_test.py build-windows/vrst-sender.exe --require-tracking`. It tests both eyes, frame/live modes, actual UDP poses and photo-window visibility, without changing SteamVR settings. It is separate from CTest because it requires a live tracked headset.

Local delivery can be created with `scripts/package.py --openvr <SDK> --delivery-dir <folder>`. It preserves versioned ZIPs and adds `Gaming-PC` launchers plus `Stream-PC/obs-plugins` and `Stream-PC/data` ready to copy into OBS.

## Build and test from source

Source is included in the binary package's `source/` directory. CMake 3.24+, C++17 and Python 3.12+ are used. No OBS frontend or Qt SDK is needed.

Portable core/UDP/sender tests, without OBS or an HMD:

```sh
cmake -S . -B build -DVRST_BUILD_SENDER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows the sender target also requires `VRST_OPENVR_DIR`; use the complete Windows build script below. Without the OpenVR build definition, the portable sender supports only demo mode.

For Windows, install Visual Studio 2022's Desktop development with C++ workload, CMake and Python. Run from its **x64 Developer PowerShell**:

```powershell
./scripts/build-windows.ps1
```

It downloads OBS 32.2.2 headers and runtime exports plus OpenVR 2.15.6, prepares an OBS import library, builds the DLL/EXE, runs the tests, and creates the ZIP in `dist/`. Visual Studio builds use the static C++ runtime by default so the sender needs no separate VC++ redistributable. The GitHub Actions workflow performs the same build when the repository is pushed; its Windows and Linux jobs have passed.

An alternative cross-build is also verified with LLVM-MinGW 20260826. To build from macOS, extract the toolchain from its [upstream release](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260826), then:

```sh
export VRST_LLVM_MINGW=/absolute/path/to/llvm-mingw-20260826-ucrt-macos-universal
python3 scripts/prepare-deps.py --llvm-mingw "$VRST_LLVM_MINGW"
cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release -DVRST_BUILD_TESTS=OFF \
  -DVRST_BUILD_PLUGIN=ON -DVRST_BUILD_SENDER=ON \
  -DVRST_OBS_SOURCE_DIR="$PWD/.deps/obs-studio-32.2.2-sources" \
  -DVRST_OBS_GENERATED_DIR="$PWD/.deps/obs-sdk/include" \
  -DVRST_OBS_LIBRARY="$PWD/.deps/obs-sdk/lib/libobs.dll.a" \
  -DVRST_OPENVR_DIR="$PWD/.deps/openvr-2.15.6"
cmake --build build-windows --parallel
python3 scripts/package.py --openvr .deps/openvr-2.15.6
```

`scripts/test-gpu-macos.sh` validates the pose and image shaders against CPU coordinates on macOS. `scripts/test-module-macos.sh` exercises the complete OBS filter, including frame pairing, repeated renders, resets, 4K output and a known synthetic jitter sequence, plus streamer controls and preset serialization. The GPU script additionally exercises composed hybrid sampling, A/B and adaptive crop. Both require an installed OBS app and SIMDe headers. The module test also requires `VRST_NATIVE_OBS_SOURCE_DIR` pointing to headers compatible with the installed OBS version; it does not bypass OBS's version gate. Synthetic tests establish implementation behavior, not live racing quality.

License: GPL-2.0-or-later. Third-party runtime notices and licenses are included. These development binaries are unsigned.

On Windows with OBS installed, `scripts/test-module-windows.ps1` builds and runs image, streamer and deterministic buffered-pose pairing module tests in isolated OBS contexts. It accepts `-Compiler`, `-DependenciesDirectory`, `-ObsDirectory`, `-PluginPath` and `-CoreLibrary`; its defaults use the prepared `.deps` SDK and `build-windows` plugin/core library. Buffering and jitter checks run at 60 fps. The full-pixel 4K readback check runs at 30 fps because the harness reads the entire output twice per sample; it does not certify 4K60 performance. The photo overlay is verified in actual D3D11 output and is off by default.
