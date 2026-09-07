# Phone-photo timing diagnostics

The user approved implementing the discussed gaming-PC timestamp overlay and stream-PC received/used pose display, with implementation choices delegated to the assistant. This is a diagnostic extension to the existing sender and OBS filter. It does not replace the capture pipeline or promise game-frame metadata unavailable from the arbitrary HDMI mirror.

## Behavior

- `Start-Delay-Check.cmd` starts the normal Windows sender with `--overlay`. A movable, non-activating desktop window displays the gaming monotonic clock, latest published pose timestamp/sequence, sender session and tracking mode/status. Closing the overlay leaves tracking running. The window must be visible in the HDMI picture; the user places it outside the selected image tracking region and inside retained crop.
- OBS exposes an off-by-default photo timing overlay checkbox. The overlay is drawn after stabilization, and never enters image analysis. It displays latest received pose separately from the pose selected for the displayed picture, in original gaming-clock units, with actual interpolation metadata. In image-only, A/B comparison, calibration, missing tracking and fallback paths it explicitly states when no pose is applied.
- Hybrid diagnostics travel with the buffered image slot, so the selected pose is the one whose transform accompanies that image, even on repeated renders. Current reception data remains separately identified. Show the additional image buffer age, requested delay, session and stale/sync state. If a displayed buffered image briefly still uses an older session, show its actual used stamp with an explicit OLD SESSION label. No timestamp from a previous connection may appear current after reset or session change.
- Toggling the diagnostic display does not silently change delay, smoothing, FOV or saved presets. Image-only mode may connect the configured sender for diagnostics, while still applying no pose correction. Calibration's own sender requirement remains intact.
- A shared portable bitmap panel provides readable ASCII text without Qt, OBS text plugins or extra runtime dependencies. CPU/GPU work and storage are bounded. Formatting uses integer timestamps without floating point rounding into another millisecond. All overlays are explicitly diagnostic and appear in the video when enabled.

## Limits and validation

The marker records sender observation time, not a proven pose/game-mirror frame match. A phone photo measures visible relative timing including display/preview delay; it cannot automatically supply an exact tracking-delay setting. No automatic value is applied. Packet timestamps use the existing protocol and sender clock. Session identifiers distinguish restarts. Live interpolation reports both the earlier packet stamp and effective interpolation time.

Verify portable formatting and image bounds, independent received/selected timestamps under real UDP and clock offset, stale/restarted sessions, live interpolation metadata, default-off pixel identity, hybrid frame association and output-only rendering in actual OBS/D3D11. Verify sender help/CLI/network compatibility and native overlay lifecycle. Preserve all v0.3.1 regressions. Build an experimental v0.3.2 Windows package containing the new launcher and complete source; do not publish remotely or change the user's OBS/SteamVR configuration.
