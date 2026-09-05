# HorizonLock VR

An OBS effect filter that stabilizes a capture-card picture on your stream PC. Choose **SteamVR pose** with optional horizon leveling, or **Image-based microjitter** using the picture alone. The pose mode includes a lightweight tracking sender for the gaming PC.

The filter appears as **VR Stabilizer** in OBS. Windows downloads include both components and complete source code.

Downloads: [Windows releases](https://github.com/DSuzuki35/horizonlock-vr/releases). The repository and its releases are private; sign in to GitHub with an account that has access.

Version **0.2.0 — experimental**, targeting **Windows 11 x64 and OBS 32.2.2**. Pose mode is designed for Dream Air + SBoys3/SteamVR, including games using either SteamVR's OpenXR path or OpenVR. The sender never changes the active runtime, recenters the headset, or alters the headset image. Image mode requires no headset connection, FOV calibration or tracking sender.

For pose mode, SteamVR must be supplying headset tracking. Native Pimax OpenXR without SteamVR needs a future tracking provider; it is not included in this release. Image mode works independently of the runtime because it analyzes the captured video.

The release DLL and EXE are cross-built and their dependencies inspected. Five automated suites cover core math, image motion, sender math and real UDP integration; GitHub builds on Windows and Linux. Actual OBS GPU/module tests run on macOS, including full 4K image output and bounded 480×270 analysis. Live Windows 11 / Dream Air / Elgato / iRacing / LMU validation still needs your hardware. Synthetic results do not guarantee racing-footage quality. See [build and test runs](https://github.com/DSuzuki35/horizonlock-vr/actions).

## Install the downloaded ZIP

Extract the **entire ZIP** first. You can copy the same extracted folder to both PCs.

**Stream PC**

1. Close OBS. Double-click `Install-OBS.cmd` and accept the Windows elevation prompt. The installer copies only this plugin into `%PROGRAMDATA%\obs-studio\plugins\vr-stabilizer`.
2. Open OBS and select your Elgato/capture-card source. Open **Filters → Effect Filters → + → VR Stabilizer**.
3. Choose **Stabilization algorithm**. For **SteamVR pose**, enter the gaming PC's IPv4 address and port **42424**. For **Image-based microjitter**, skip the gaming-PC sender setup below.
4. Start with the defaults below. Click **Refresh diagnostics** to inspect pose alignment or image-feature confidence and the correction limit.
5. Set the OBS canvas/output to your intended 16:9 resolution. With the source selected, use **Transform → Fit to screen** after changing input trim or output shape.

**Gaming PC — pose mode only**

1. Start SBoys3 / SteamVR and the game as usual.
2. Double-click `Start-Tracking.cmd`. It starts `sender/vrst-sender.exe` and prints the PC's IPv4 addresses. Keep the console open while streaming.
3. If Windows Firewall prompts for the sender, allow it on your private home network. The stream PC must be able to reach UDP 42424 on the gaming PC. Do not set up internet port forwarding.
4. Continue sending your normal desktop mirror through the capture card. The sender does not capture or encode video.

Pose mode reads tracking independently of video transport. The Elgato model is not required for installation, but capture settings affect the tracking delay you need. Close the sender console or press Ctrl+C to stop it. Updating from v0.1.0 keeps your saved settings; the default algorithm remains pose mode and horizon leveling starts off.

For portable OBS, copy `obs-plugin/bin/64bit/vr-stabilizer.dll` to its `obs-plugins/64bit` directory and `obs-plugin/data/*` to `data/obs-plugins/vr-stabilizer/` instead of using the installer. `Uninstall-OBS.cmd` removes the normal installation while OBS is closed.

## Controls

| Control | Meaning | Starting value |
|---|---|---|
| Stabilization algorithm | SteamVR pose or Image-based microjitter | SteamVR pose |
| Horizontal / vertical FOV | Angular view of the incoming image after input trim | Enter/calibrate your captured view |
| Use automatic full-eye FOV | Use the selected eye projection reported by SteamVR; only correct for a complete eye image | Off |
| Tracking delay | Look back this many milliseconds from filter render time; includes capture-card and OBS buffering | 80 ms |
| Smoothing | Motion smoothing for the selected algorithm; zero disables smoothing, while pose horizon leveling remains independent | 40 |
| Horizon leveling (pose) | Cancel sustained head roll while preserving look direction | 0% |
| Maximum horizon correction (pose) | Limit how much roll leveling may remove | 45° |
| Maximum image shift (image) | Limit correction per axis, relative to trimmed input | 1% |
| Maximum image roll (image) | Limit small roll corrections | 0.5° |
| Correct image roll (image) | Estimate rotation as well as translation | On |
| Tracking region, four margins (image) | Select the stationary cockpit area used for analysis | Left/right 10%, top 50%, bottom 10% |
| Stabilization crop | Reserve image margin to absorb head movement | 10% |
| Framing zoom | Additional final crop/zoom | 0% |
| Fill 16:9 | Center-crop the output to 16:9 without stretching | On |
| Framing horizontal/vertical | Shift the final framing within available image margin | 0% |
| Input trim, four edges | Remove black bars or window borders before interpreting the image's FOV | 0% |
| Reset smoothing | Clear motion history without changing any controls | As needed |

The **Tracking delay** slider in pose mode does not buffer video or audio. It changes which older tracking pose is used for the currently rendered image. Upstream video delays and capture buffering changes require recalibration. Image mode adds its own fixed one-OBS-frame video delay, described below. Put VR Stabilizer first among image-processing filters where practical.

Crop values reduce the total span of each axis. For example, a 10% stabilization crop keeps 90% of the width and height before aspect framing, approximately 5% off each edge. Crops combine multiplicatively. Input trimming, stabilization crop, final zoom and aspect fill are combined into one GPU reprojection sample. More framing zoom also leaves more room for correction. Moving framing toward an edge reduces the available motion margin on that side.

At a large/fast head turn the filter gradually limits correction to what fits inside the source. It follows the head when there is insufficient margin. It cannot reveal pixels the game did not render. On tracking loss it keeps the usual framing and bypasses rotation correction. Recenter jumps reset the smoother. Intentional slow turns remain visible.

Diagnostics are refreshed when the properties open or when **Refresh diagnostics** is clicked. In pose mode, “Selected pose age” includes tracking delay and RTT is the network round trip. Image mode reports feature confidence, inliers, analysis dimensions and processing time. A correction limit below 100% means the crop margin constrained the requested correction.

## Image-based microjitter

1. Choose **Image-based microjitter**. FOV, network, tracking-delay and horizon controls are hidden because this mode does not use them.
2. Remove borders using **Input trim**. Choose a **Tracking region** containing fixed cockpit structure, such as dashboard trim or a windshield pillar. The four margins refer to the trimmed image. Defaults select the lower part: 10–90% horizontally and 50–90% vertically.
3. Exclude animated steering wheels, mirrors, overlays, other cars and moving track scenery. Leave a reasonably large, textured area. Very narrow or flat regions may have too few safe feature patches; diagnostics then show weak tracking and correction is bypassed.
4. Start with smoothing 40, maximum shift 1%, maximum roll 0.5° and stabilization crop 10%. Compare against smoothing 0, then increase smoothing gradually. Increase correction limits only if visible jitter needs it.

The algorithm tracks subpixel image features, rejects inconsistent matches and estimates small shifts/roll. It smooths that measured motion, caps the correction, and limits it again to available crop. Weak matches, scene changes and unavailable analysis produce normally framed video without an old correction. ROI, size, mode, trim and reset changes restart history.

Full-resolution video remains on the GPU. A filtered grayscale copy, at most 480×270, is used for analysis. The measured correction is applied to that exact captured frame. This adds **one OBS video frame after warm-up: 16.7 ms at 60 fps**. Audio is not delayed automatically; account for this when aligning audio or other cameras. Missing/hidden frames restart the buffer. No gaming-PC clock or capture-card delay calibration is needed for image mode.

This is a small 2D correction, not full perspective/depth reconstruction. Turning off game neck movement helps, but image analysis still cannot distinguish all head motion from car motion, lighting changes or parallax. It cannot recover missing frames, undo motion blur, or remove 90-to-60 cadence judder. Test with the car parked, then on a lap.

## Horizon leveling for head tilt

Choose **SteamVR pose**, connect the sender and calibrate FOV/delay as below. Increase **Horizon leveling** to remove the sustained roll caused by tilting your head into a corner. At 100%, the target view is upright relative to SteamVR tracking up, subject to the maximum-angle and crop limits. Yaw/pitch viewing direction is preserved; existing motion smoothing can still smooth those directions.

Set smoothing 0 if you want only horizon leveling. Larger tilts require more crop; the filter reduces correction when it would expose missing pixels. Leveling fades near straight-up/down gaze where roll is ambiguous. It cancels headset roll, not unknown roll from the game's car camera or banking. The image mode is a separate algorithm; a coordinated hybrid is not included in v0.2.0.

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

Pose mode smooths headset yaw, pitch and roll, with optional sustained roll leveling. Image mode smooths estimated 2D translation and roll in the selected region. Neither reconstructs depth or corrects positional parallax across the whole cockpit. Baked-in HUDs, motion blur, missing edge pixels and foveated rendering artifacts cannot be undone. 90-to-60 motion cadence can remain visible independently of head stabilization.

The sender prefers SteamVR compositor frame-history orientation when available and composes the selected eye's rotation. These packets use the local **time the helper first observed the frame**, not an assumed common epoch for SteamVR's internal timestamp. In live fallback it sends current headset orientation. Neither mode proves which pose produced an arbitrary HDMI frame. The plugin's clock sync aligns the two PCs; manual delay handles the remaining empirical video alignment.

## Sender commands

```bat
Start-Tracking.cmd --help
Start-Tracking.cmd --eye right
Start-Tracking.cmd --mode live
Start-Tracking.cmd --port 42425
Start-Tracking.cmd --demo
```

Demo mode emits synthetic tracking for connection tests; it does not follow a headset or provide matching game footage. Do not use it to stabilize gameplay. Keep `openvr_api.dll` beside `vrst-sender.exe`.

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

The supplied binaries were cross-built with LLVM-MinGW 20260826. To reproduce from macOS, extract the toolchain from its [upstream release](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260826), then:

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

`scripts/test-gpu-macos.sh` validates the pose and image shaders against CPU coordinates on macOS. `scripts/test-module-macos.sh` exercises the complete OBS filter, including frame pairing, repeated renders, resets, 4K output and a known synthetic jitter sequence. Both require an installed OBS app and SIMDe headers. The module test also requires `VRST_NATIVE_OBS_SOURCE_DIR` pointing to headers compatible with the installed OBS version; it does not bypass OBS's version gate. Synthetic tests establish implementation behavior, not live racing quality.

License: GPL-2.0-or-later. Third-party runtime notices and licenses are included. These development binaries are unsigned.
