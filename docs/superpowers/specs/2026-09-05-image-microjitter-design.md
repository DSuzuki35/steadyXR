# Image-based microjitter

The user requested image-only stabilization as an additional algorithm and clarified game neck movement is disabled. Implement a separate selectable mode in HorizonLock VR. Existing authorization includes completing the plugin and publishing it to the user's private GitHub repository. No further permission pause is needed for implementation or the next experimental release.

## Approach

Use sparse image features, pyramidal Lucas–Kanade tracking, and robust rigid 2D motion estimation. This supports small horizontal/vertical motion and roll, with no headset pose, runtime, network delay or FOV input. Pure phase correlation is simpler but translation-only; dense/learned flow adds unnecessary dependencies and cost for this bounded motion problem.

Track a user-selected rectangular region of the trimmed input, preferably stationary cockpit structure. Exclude animated steering wheels, mirrors and overlays through that region selection. Disabled neck motion helps, but car/world motion and parallax still exist and image analysis cannot identify physical causes with certainty.

## Boundaries

- Modes are existing SteamVR pose and new Image-based microjitter. Hybrid is future work.
- Image estimator is portable C++17 without external computer-vision dependencies. Track subpixel motion, reject inconsistent/weak matches and scene cuts; confidence and correction limits are mandatory.
- Analysis grayscale input is at most 480×270 with preserved aspect ratio, formed from a filtered GPU downsample of the trimmed input. Full-resolution video stays on GPU.
- Use two full-resolution texture slots and matching low-resolution staging surfaces. Capture current input once per OBS video tick, analyze the preceding slot and render that same preceding slot. This adds exactly one OBS frame after warm-up (16.7 ms at 60 fps). Repeat renders in a tick reuse the result. No stale correction applied to a newer frame.
- First frame renders framed identity. Mode/size/trim/region changes, tracking loss, scene cuts and reset invalidate temporal state. No image processing failure may stop normal video output.
- Image motion consists of 2D rigid transforms (translation and optional roll), not perspective/depth reconstruction. The smoother is causal and allows intentional motion to continue; correction is bounded per axis/roll and further limited to the available crop. Smoothing zero is identity.
- Input trim, stabilization crop, final framing zoom/position and 16:9 fill remain available. Image mode ignores FOV and tracking-delay settings; hide irrelevant controls and show mode-specific diagnostics.
- New controls: max translation percent, max roll degrees, roll enable, and four tracking-region margins. Smoothing is shared. Status reports confidence, tracked/inlier feature counts, processing time and one-frame video delay.
- Existing pose mode and shader behavior remain unchanged. Preserve saved scenes; default remains pose mode.

## Verification and release

## Added scope: pose-based horizon leveling

The user authorized both additions now with a one-hour working window, starting about 21:08 UTC. Add a 0–100% horizon-level strength and 0–60 degree maximum roll correction to pose mode (defaults 0%, 45 degrees). Level the smoothed target's roll relative to SteamVR tracking up while preserving its forward/look direction, then compute the usual source-to-target inverse ray correction. This must work even with motion smoothing at zero. Fade leveling near vertical gaze where roll is geometrically ambiguous. Existing crop-aware limiting applies. It removes head tilt, not unknown game-car roll or track banking. Image mode hides these pose-only controls.

## Verification and release

Synthetic textured sequences must demonstrate subpixel translation and roll estimates, jitter reduction, intentional-pan following, bounded corrections, flat/occluded/cut fallback, ROI isolation, configuration reset and irregular/repeated timestamps. Tests must use known rendered motion, not implementation self-comparisons. Benchmark estimator cost. Verify actual GPU shader pixels and full OBS filter behavior including frame pairing, repeat renders, lifecycle and size/mode changes. Build Windows binaries and run Windows/Linux CI. Publish v0.2.0 only after review, retaining the v0.1.0 release. Hardware/racing footage quality remains unverified without a real clip.
