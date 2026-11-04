# SteadyXR 0.16: quad-view preview and reliable Workshop playback

## Intent and agreed scope

Combine the selected eye's wide, lower-resolution peripheral image and narrower,
higher-resolution focus image into the stream preview. Preserve the user's 0.15
work. Initial compatibility target: mbucchia Quad-Views-Foveated with the Sboys3
SteamVR setup and Pimax Dream Air / Dream Air SE. The exact game is not specified;
do not claim game or headset validation from synthetic tests.

The user selected direct composition in the final preview, with capture status.
The implementation remains native C++/D3D11 on the viewer side and retains the
existing OpenXR D3D11/D3D12 capture paths. No new framework or provider installation.

## What exists and what needs work

The capture layer already selects views `eye` and `eye + 2` from a four-view
projection. IPC transports both images with each view's FOV and orientation.
The renderer projects output rays into each image and blends focus over context
in its final draw. Workshop records both raw sources separately when available.
However, Workshop's source-framing mode explicitly disables focus composition,
and the main status does not say whether one or two source views were received.

Local OpenGrid XR reference: `native/src/lmu_stream.cpp` maps outer rays into the
focus camera, clamps sampling to the submitted subrectangle, and blends the edge.
Its LMU path precomposes at sufficient resolution rather than reducing focus to
the peripheral texture size. This is reference material; retain SteadyXR's own
implementation and do not copy provider code or add OpenGrid as a dependency.

Two reproducible Workshop defects are already fixed on the 0.16 branch:

- MP4 timestamp quantization exceeded the previous 100 ns comparison. Accept up
  to 33.4 microseconds, still requiring a unique nearest metadata frame, exact
  frame counts, valid source metadata and matching video checksums.
- The decoder's padded RGB surface (2048x976 for a 2048x964 clip) was rejected.
  Permit bounded macroblock padding only with an exact validated display aperture;
  copy the active image without rescaling the padding into the source FOV.

Both user recordings now decode/render completely (77 and 567 frames) and seek.
Irregular-timestamp and padded dual-view hardware fixtures pass. Full baseline
and post-fix CTest runs pass 42/42. Physical headset testing remains outstanding.

## Approaches

1. **Selected: compose directly in the final preview.** Keep context and focus
   separate through transport and replay, sample both at output resolution, and
   apply the same stabilized output geometry to both. This preserves detail and
   uses the existing rendering pass.
2. **Considered: precompose before stabilization, as OpenGrid's LMU stream path
   does.** A single intermediate image simplifies subsequent consumers, but adds
   an image/pass and can discard focus detail if its resolution is too low.

## Resulting behavior

- A valid dual-view frame automatically produces a combined preview. No additional
  enable switch is necessary. Use each submitted view's FOV, orientation and
  subrectangle; do not assume centered or stationary focus. Keep the existing
  narrow edge feather unless tests reveal an alignment defect.
- Compose at the configured output resolution, never at the peripheral source's
  lower resolution. Preserve behavior under crop, horizon/pitch correction, gaze
  framing, visual effects and Original comparison.
- Workshop source-framing mode shows the combined, unstabilized full source view;
  processed mode uses the same combined renderer as live preview. Raw recording
  files remain separate and clip format v1 remains readable. No changes to saved
  clips or user profiles are needed for the playback repair.
- Main capture status and Workshop identify **Peripheral + focus** versus
  **Single view received** and show per-view source dimensions in details. Demo,
  window capture, paused, stale and disconnected states remain clearly distinct.
  Keep status out of the clean stream image.
- A single received view continues to render normally. Do not label it a failed
  quad view: it may be ordinary stereo or an image already combined upstream.
  Missing/invalid new dual-view data must never mix fresh context with stale focus;
  preserve the existing atomic-frame/waiting behavior.
- Explain that raw focus capture depends on the active layer chain. If only one
  image arrives, show that fact and troubleshooting guidance. Do not automatically
  reorder layers, register another runtime, or change headset/game settings.

## Validation and delivery

Add targeted tests for differently sized peripheral/focus images, displaced focus
FOV and orientation, subrect edges, both eyes, transitions back to single view,
Workshop source mode and Original/processed comparisons. Verify sharp focus pixels
survive output composition and peripheral pixels remain visible outside focus.
Extend status/UI tests for live, missing and stale data. Retain existing D3D11 and
D3D12 capture contract tests and the full test suite.

Keep the three hardware Workshop regressions (regular dual-view, downscaled source,
and non-macroblock-aligned 60 fps source); verify rejected mismatched metadata.
Recheck the two user clips read-only through decode, rendering and seek. Live
validation should check a supported quad-view game, moving gaze, both eyes and
record/replay under the user's mbucchia/Sboys3 headset setup; list it as unverified
until actually performed.

Build 0.16 after design approval. If capture source/IPC remains unchanged, preserve
the signed 0.14 capture DLL; if investigation requires a capture change, document
that scope before packaging and do not claim the old signature covers it. Deliver
versioned app, source ZIP, checksums and validation notes to the repository's
AGENTS.md destination, preserving previous packages. OBS binaries are outside this
change. No GitHub push or release publication is requested.

## Builder decisions and non-goals

Builder chooses exact status wording/layout, test fixture design and small helper
APIs. No separate focus sharpening, binocular panorama, synthesized peripheral
detail, quad-view provider implementation, depth/parallax reconstruction, automatic
layer reordering, or claim of zero performance cost is included.
