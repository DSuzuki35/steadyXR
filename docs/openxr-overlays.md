# SteadyXR 0.9: additional OpenXR overlays

## Install and test with Racelab

1. Close the previous SteadyXR app and start Gaming-PC/Start-Preview.cmd from the 0.9 package.
2. Use Upgrade connection (or Install connection) to activate the new capture DLL. Restart iRacing in OpenXR mode. 0.9's overlay-capable capture format cannot use an older DLL.
3. Enable the desired Racelab VR overlays in its OpenXR mode. The actual runtime call order needs to be game → overlay injector → SteadyXR → runtime. SteadyXR must see the extra layers and the injector's swapchain calls. Use the existing OpenXR layer editor to adjust registrations if needed, then restart the game; delivery itself changes no registrations. There is no universal up/down instruction for every loader configuration.
4. For the standalone app open Overlay layers… . For OBS, close the standalone output, install the new obs-plugins/data folders with OBS closed, then open the SteadyXR OpenXR Capture source's OpenXR overlay layers group.
5. Refresh the list. Start with one supported Quad layer. Check the preview, then select additional layers as needed. One overlay program can submit several layers. Labels report type, dimensions and slot; OpenXR does not provide a reliable vendor name per composition layer.

Racelab and OpenKneeboard are candidates because their OpenXR integrations submit composition layers. Compatibility of the particular runtime, injector order and installed versions needs a live test; a visible headset overlay is not proof that it reaches SteadyXR's interceptor. Pure SteamVR compositor overlays and overlays injected downstream of SteadyXR are not available through this capture path.

## Selection behavior

- None is the default and publishes no extra overlay textures to the viewer.
- All supported includes newly appearing supported layers, within capture limits.
- Individual checkboxes apply to this producer/session. After restarting the game, reselect the desired layers. Stored slot numbers are never applied to a different session.
- Selection preferences are included in settings/presets; the detected list itself is not saved.
- Refresh explicitly after changing third-party overlays. OBS's properties do not rebuild themselves during slider editing. Only the active viewer/source can change capture selection.

Overlays are composited in submission order with their own position, rotation, eye visibility and alpha. They follow the game stabilization. Original, 50/50 split and side-by-side A/B views use the corresponding original or corrected rays for overlays too; the comparison remains 20 seconds or until pressed again. Fixed screen-space overlay placement is not a separate mode in this release.

## Formats and limits

| Format | Capture |
| --- | --- |
| Quad | Flat surfaces, submitted size/pose and alpha |
| Cylinder | Curved surfaces, including the infinite-radius form |
| Additional color Projection | Selected eye and submitted FOV/rotation; quad-view projections use the context view |
| Layers behind the primary game projection | Listed as unsupported; not moved in front of the game |
| Ambiguous layers sharing the same swapchain slice and rectangle | Listed as unsupported; not guessed by submission order |
| Other composition types or unsupported extension chains | Listed as unsupported |

The primary image is the first supported stereo/quad projection. At most 16 layer identities are tracked per session, with up to 8 selected overlay images in a frame. Capacity limits are reported. Destroyed/replaced resources cannot silently reuse a selected identity. Depth-aware composition is not implemented: extra projection views use their color image and ignore per-view depth. Unsupported layer-level extension chains, including optional color-scale handling, are not captured. This remains the existing SDR output path.

Captured overlay images stay on the GPU. The producer reuses an already-published overlay texture when its successful release revision and array slice are unchanged, avoiding a redundant GPU copy. Updated images and rebuilt connections still receive a fresh copy. The consumer retains its own synchronized image; this optimization does not skip required synchronization or pose updates. Additional copying and draw passes cost GPU time/bandwidth in proportion to image size and number. Swapchain snapshots are needed to safely retain released images; static overlays may reuse their last released snapshot. There is no CPU video readback or additional video encoder in the live path.

## References

- [Racelab OpenXR setup](https://garage.racelab.app/docs/vr/settings/)
- [OpenKneeboard composition-layer integration](https://openkneeboard.com/faq/third-party-developers/)
- [OpenXR composition layers](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameEndInfo.html)
- [OpenXR cylinder geometry](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerCylinderKHR.html)
