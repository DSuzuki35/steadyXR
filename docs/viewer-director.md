# Viewer camera controls

The **Director** page in **Settings → Gaze & effects** adds three independent controls.
All start off and are included in complete presets. Master spectator bypass disables them.

## Lookahead

Lookahead retains captured images and uses later captured head poses to smooth
the camera for the delayed image. Each image keeps its own projection, gaze,
overlay textures and speed sample. This only changes the viewer's image.

Choose one to four frames. Three frames are approximately 50 ms at 60 captured
frames per second, or 33 ms at 90. The status shows the **actual timestamp delay**
and GPU memory use, which depend on capture cadence and warmup. Add that delay
to the audio sync offset in OBS for audio arriving without the same video delay.
SteadyXR itself does not capture or delay audio.

The buffer has a **256 MiB texture budget**, counting context, focus and captured
overlays in every retained slot. It reduces the requested delay if necessary;
if even one delayed frame will not fit, it bypasses buffering. Four delayed
frames need five texture slots. For example, a single 3840 x 2160 RGBA8 image
uses about 31.6 MiB per slot; three delayed frames need about 126.6 MiB.
Higher-resolution eyes, HDR formats, quad views and overlays increase that cost.
These numbers describe texture payload, not driver allocation overhead.

Lookahead needs OpenXR pose capture and stabilization enabled. Existing-window
capture does not have the original XR poses. It still supports the two controls
below. During warmup the first retained image is held briefly. Changes of source,
eye, reference, image layout or delay flush the buffer.

## Automatic crop

The output zooms in only as much as necessary to keep the corrected image within
captured pixels, up to the selected maximum. It reacts immediately to a newly
exposed edge and returns more slowly to the wider framing. **Release** controls
that return time. Existing aspect ratio, trims and vertical offset remain part
of the calculation.

If the maximum crop is insufficient, camera correction is reduced to fit.
Automatic crop does not invent scenery and cannot recover details that the game
never rendered. Speed crop also provides reserve before additional automatic
crop is calculated.

## Speed framing: iRacing and Le Mans Ultimate

**Speed telemetry** selects Auto, iRacing or Le Mans Ultimate. Keep the reader on
the same PC as the game. At low speed the output adds up to the selected crop.
As speed increases it smoothly returns to the normal framing, reaching it at
**Normal framing speed**. It never expands the game's rendered field of view.
**Response** controls how quickly it follows speed changes.

Auto prefers valid iRacing data and otherwise uses LMU. No data, leaving the
driving session or a stalled feed makes speed framing ease back to the normal
view. The reader polls at most 20 times per second while enabled and retries
missing connections at most once per second. It closes mappings when disabled.
The verified native layouts and live-validation limits are documented in
[Telemetry](telemetry.md).

## Resource use and two-PC setups

Automatic crop and speed framing change the existing draw's coordinates; they
add no image-processing pass. Lookahead adds GPU copies and bounded texture
storage on the capture consumer. It uses no CPU pixel readback and introduces
no waits on the game capture thread. Disabled lookahead allocates no history;
disabled speed framing performs no telemetry polling.

For a two-PC HDMI/capture-card setup, run these camera controls in the gaming
PC's standalone preview, then capture that result. The stream PC receives the
already framed video without the original eye poses or hidden image reserve.

Start with automatic crop alone, then try a short lookahead delay and subtle
speed crop. Actual GPU timing and iRacing/LMU headset behavior need a live
driving test; synthetic tests do not establish a game FPS or latency guarantee.
