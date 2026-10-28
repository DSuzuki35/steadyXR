# Capture Workshop - SteadyXR 0.15.0

The Workshop records the **entire submitted OpenXR source rectangle of the selected eye**, before spectator crop, stabilization or visual effects. If OpenXR supplies a separate focus view, that view is recorded too. Source FOV, orientation, gaze, timestamps and available speed telemetry stay paired with their images. It never records the 16:9 spectator output as its source.

1. Start the live OpenXR preview. Open Workshop and choose 15, 30 or 60 seconds, at 30 or 60 fps. Default is 30 seconds at 30 fps; the proxy retains the source aspect ratio and full captured FOV, with a maximum 2048-pixel long edge.
2. Use **Recording controls...** to assign a keyboard shortcut or select your wheel and learn a button. Shortcuts are initially unassigned. Release the wheel buttons before learning. Conflicts are shown rather than silently replacing another shortcut. Recording starts on a fresh press; held buttons and reconnects do not start it.
3. Drive and trigger recording. No always-on buffer runs. The shortcut does not bring the Workshop to the foreground. After completion, open Workshop to edit the saved clip. If you are already working in that window, the completed clip opens there.
4. Live output pauses while editing, releases capture ownership, and displays a pause slate. The recording plays only in Workshop. Drag inside the crop frame to move it; drag an edge or corner to resize. The output aspect ratio stays fixed. Arrow keys move the focused frame; Shift makes larger steps. Escape cancels an active drag. Sliders and frame geometry update together.
5. Switch between **Edit source framing** and **Show processed output**. The **Original** checkbox compares the same framing with stabilization bypassed. Adjust normal Smoothing, optional Pitch steadiness, horizon leveling and framing. Seek/setting changes rebuild filter history from the clip start, in bounded batches. Large clips take longer to rebuild than short clips.
6. **Apply to active profile** saves the draft. **Reset draft** returns to the last applied settings. **Return to live** resumes the prior live pause state and resets stale motion history. Closing without Apply leaves your profile unchanged.

Clips are local `.sxclip` folders under the viewer settings directory's `Clips` folder. Keep their metadata and MP4 files together. **Open clip...** selects that folder. The command line also accepts `--clip "C:\path\Drive.sxclip"`. The metadata validates matching video hashes and timestamps before replay.

## Camera changes

Natural Head Movement has been removed from the standalone controls and disabled for old standalone profiles. Normal Smoothing remains in use, including with gaze framing. Existing OBS binaries are unchanged.

Pitch steadiness is optional and defaults to off. It softly anchors up/down viewing angle, preserves yaw, and lets large deliberate looks follow. Use **Set forward view** to recalibrate. Workshop calibration is for that clip; use the live Camera calibration action after applying when needed. This is rotational stabilization, not positional head-bob/parallax correction.

## Limits and costs

- Recording uses a verified hardware H.264 encoder. It does not silently fall back to software encoding. GPU conversion/copies, encoding and storage still have a cost; no claim of zero game frame-time impact is made.
- Source images are scaled, not cropped. The clip cannot recover unrendered areas or change render expansion after capture. A 30 fps/2048 proxy is for framing/motion tuning, not validation of full-resolution sharpness or 60/90 fps cadence.
- Queue overflow drops recording samples instead of blocking the game. The Workshop reports drop/gap/partial-clip information. Disconnects, recentering, eye or format changes stop the current clip with an explanation.
- Initial recording supports SDR formats. HDR and separately captured OpenXR overlays are rejected with a clear message; disable overlay capture before recording a Workshop clip. Live HDR/overlay support is unchanged.
- The saved gaze samples can drive replay; learned chat exclusion calibration is session state and must still be checked live. The Workshop does not promise to reproduce a previously learned live chat zone.
- The crop outline is the nominal source framing, before pose correction. A margin warning indicates correction that had to be limited. The recorded sample cannot prove safe framing for future head movements.
- Manual edits normalize framing to the supported uniform crop range (0-40%), clear separate trims and disable the selected eye's asymmetric reserve. Other-eye reserves and unrelated camera settings remain intact.

The application is 0.15.0; its capture component remains the byte-identical signed 0.14.0 DLL. No capture DLL rebuild or new signing is required.
