# SteadyXR TODO

- [x] Add a direct OBS source for single-PC streaming without a visible preview window: **SteadyXR OpenXR Capture**. Includes existing stabilization/framing controls and read-only settings import. See [setup guide](docs/obs-openxr-source.md).

## Requested features — design in progress

All new features must be optional and off by default. Existing settings and presets retain their current behavior; new settings must round-trip through presets. Disabled features should skip their additional processing.

- [ ] Gaze-driven spectator framing for Pimax Dream Air, prioritizing SteamVR + SBoys3 and also supporting native Pimax OpenXR when valid gaze is available. Include configurable forbidden areas for chat fixed in the virtual cockpit/world. Add a **Learn chat position** button/hotkey with a countdown and stable gaze sampling to learn the center direction, plus adjustable angular width/height and a test indicator. Do not infer panel size or depth from a single gaze sample. Keep the learned direction anchored independently of head turns and preview reframing; require re-learning when chat moves or the tracking reference changes. Looking at chat suppresses the gaze contribution while normal stabilized head following continues. Fall back smoothly when gaze is unavailable, stale or untracked.
- [ ] An optional advanced stabilization mode that suppresses micro-jitter while preserving deliberate, natural head turns. Coordinate head and gaze framing within the available captured image; retain the existing stabilization mode.
- [ ] Independently optional exposure adaptation, bloom and color adjustments for the spectator output. Design for the existing preview/direct OBS source and a stream-PC OBS effects filter, so visual processing can run on the RTX 4090. Benchmark overhead and avoid applying effects twice.

## Ideas — deferred

- [ ] Investigate DLSS 5 neural enhancement for the spectator capture. Evaluate capture-only processing with estimated motion vectors versus game-provided vectors; confirm SDK access and supported hardware. Benchmark image stability, GPU cost and latency at the spectator output resolution/frame rate before committing to integration. The current setup is an RTX 5090 gaming PC and RTX 4090 stream PC; stream-PC support must be verified. This is an experiment, not part of the gaze framing, stabilization and visual-effects work.
