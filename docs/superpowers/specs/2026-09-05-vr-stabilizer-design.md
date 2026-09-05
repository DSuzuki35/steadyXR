# VR Stabilizer design

Build request: a Windows gaming-PC SteamVR pose sender and a Windows OBS filter on a second PC receiving an existing 4K60 capture-card feed. The user has approved the timestamp-and-adjustable-delay approach in the conversation and requested implementation. No further design approval is needed.

## Capture approach

SBoys3 is a SteamVR driver. The sender uses Valve's OpenVR API as a background application; it never submits headset frames or changes the runtime. It sends timestamped head orientation with eye-to-head rotation applied, raw per-eye projection tangents, tracking validity, and sequence/session identifiers over UDP. Prefer compositor frame-history poses when available, with a clearly identified live-pose fallback. Frame timing is useful evidence, not an exact identification of a game's desktop preview. The delay slider remains an empirical alignment control.

Keep the user's capture-card feed. Recommend a single-eye SteamVR mirror or a full-eye OBS OpenVR capture projected from the gaming PC when the game's companion view is hard to calibrate. Do not claim the unknown iRacing mirror has a particular crop. No frame stamping or image/pose atomicity is claimed in version 0.1.

## Components

- Portable C++17 core: quaternion math, time-based smoothing, bounded pose history, clock alignment, robust network packet encoding, projection and crop planning. No dependency on OBS, OpenVR, or Windows.
- UDP receiver: owns a worker thread and socket; sends periodic four-timestamp clock probes to the configured gaming-PC IPv4 address. Receives only from that peer. Receivers subscribe by sending probes; sender expires inactive subscribers. Both use monotonic nanoseconds, never wall clock.
- Gaming-PC sender: console executable with helpful startup text, IPv4 addresses, `--port`, `--eye left|right`, `--mode frame|live`, `--demo`, `--help`. Reconnects to SteamVR and handles headset tracking loss. No headset changes. One instance supports several OBS filters/clients.
- OBS video effect filter: samples pose history at local monotonic render time minus the user delay. This convention includes capture and OBS buffering; it does not add video latency. A quaternion low-pass target is generated on the timestamped pose timeline, then inverse rotational reprojection runs as one GPU pass. On missing/stale tracking, show a normally cropped image rather than a stale rotation. The processing remains independent of the tracking backend.

## Controls and framing

Gaming-PC IPv4 and UDP port; manual horizontal/vertical FOV; optional automatic full-eye FOV; tracking delay 0–1000 ms, default 80; smoothing 0–100, default 40; stabilization crop 0–40%, default 10; framing zoom 0–40%, default 0; output shape original or 16:9, default 16:9; framing horizontal/vertical offsets; input crop edges to remove bars/window borders; diagnostics and reset smoothing.

Smoothing zero disables motion correction exactly. Crop zero still permits correction if final aspect/framing provides enough room; otherwise correction is reduced. Smoothing is causal on the source timeline (roughly 20–500 ms time constant). Use quaternions and shortest-arc interpolation, not independent Euler smoothing. Detect recenter/jumps and tracking gaps, and reset the target. Preserve output framing during tracking loss.

Input crop removes unwanted pixels and defines the image region whose manual FOV is entered. Stabilization crop reserves motion margin. Aspect fill and final zoom produce the output framing, applied together in the shader to avoid multiple resampling passes. These cannot recover missing source pixels. Clamp correction toward identity until all output corner rays remain in the source. Identity must remain valid for every supported crop/framing configuration.

## Protocol contract

Explicit little-endian encoding; magic VRST, version 1, message type, payload length. Bounds-check every message and reject nonfinite numeric values, unreasonable quaternion norms, and invalid projections. No casting datagrams to native structs. Sync request carries local send time and nonce. Reply echoes those plus sender receive/send times and session ID. Pose packets carry sender monotonic pose time, frame sequence, orientation, projection, flags and session. Clock offset is remote minus local, selected from low-RTT recent probes; detect sender restart and reset histories. Never block OBS rendering on network I/O.

## Verification and delivery

Meaningful portable tests for projection sign, identity, wrap-around, delay lookup at 90/60 cadence, crop bounds, recenter, stale data, malformed packets, reordered poses, clock offsets and restart. Test UDP through loopback with the demo sender. Compile actual Windows sender and OBS DLL with pinned upstream dependencies. Include DLL, EXE, OpenVR runtime library, shader/locale, installation scripts, and setup/calibration guide in a ZIP. Also provide reproducible CMake and Windows build commands and CI configuration. Verify the OBS shader against a local OBS runtime if practical. Clearly distinguish successful cross-compilation and simulated tests from live Pimax/Elgato validation, which requires the user's hardware.
