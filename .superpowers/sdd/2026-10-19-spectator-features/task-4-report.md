# Task 4: Shared GPU visual effects core

Status: DONE_WITH_CONCERNS (core implemented and hardware-tested; actual VR/OBS load qualification belongs to integration).

## Delivered

- `preview/visual_settings.hpp`: independently disabled exposure/adaptation/bloom/color, bypass, explicit SDR transfer/alpha conventions, finite bounded controls, user metering region and transient host content region.
- `preview/visual_effects.hpp/.cpp`: shared D3D11 FL11 immediate-context compute implementation. All disabled/bypassed returns exact input identity, zero GPU allocation/dispatch/query work. Disabling bloom/adaptation releases their optional textures. Resources allocate lazily and reuse at unchanged dimensions/device; reset clears resources and temporal state.
- Enabled output is borrowed linear RGBA16F SDR, clamped [0,1]. RGBA/BGRA8 UNORM, SRGB, TYPELESS and explicit linear RGBA16F supported; non-SDR/unknown formats, missing SRV bind, mip chains, arrays, MSAA and incompatible explicit transfers return input with unsupported_input diagnostic. No range conversion or HDR assumptions.
- Exact piecewise sRGB conversion; typed SRGB views account for hardware decode. Premultiplied encoded input is unpremultiplied before decode; output premultiplies linear light. Alpha remains unchanged; alpha0 is protected.
- Metering: fixed 4096 stratified GPU samples over intersection of user/content regions, highlight-clamped log luminance, small group reduction. GPU ping-pong 1x1 gain/transition state; no CPU statistics transfer or pixel readback in production. Bounded gain, separate exponential recovery rates, dt cap .25s, frame/time rewind and >2s gap resets; same frame/time pair does not advance twice. First/reset frame starts unity.
- Bloom: quarter-width/quarter-height extraction, threshold/soft-knee, two 9-tap reduced-resolution blur passes and linear combination. Exposure/color combine in one full-resolution pass; no full-resolution blur.
- Diagnostic cumulative allocation/dispatch/reset counts, per-frame dispatches, meter samples and texture bytes. Opt-in GPU timestamp ring has four slots, uses DONOTFLUSH, polls each pending query at most once per process, skips busy/unavailable samples, and never spins/stalls. Timing is off by default.

## Caller boundary (required for preview and OBS integration)

`process` uses the host's serialized immediate context. Input must belong to the supplied device and be unbound from all output slots. Previous returned effects texture MUST be unbound from all shader stages before reuse (it becomes a UAV). Core preserves touched compute shader/class instances, SRV slots0..2, UAV0 (counter retained), CB0; graphics state is untouched within this boundary. Host should unbind its composition RTV before process, sample returned RGBA16F via a linear SRV, then encode exactly once into its SDR output. Native application applies before independent overlays. OBS supplies its actual premultiplied convention and explicit SDR transfer and places this before scene graphics. Diagnostics output_transfer distinguishes passthrough from processed linear output. No renderer/main/plugin integration included in this task.

## RED / GREEN evidence

Skill used: test-driven-development + verification-before-completion. No additional agents/reviewers spawned as directed. Inline code review checked format/transfer semantics, state ownership and optional resource lifetime.

1. Added real hardware tests and an identity placeholder first. `cmake --build build-msvc --config Release --target vrst-visual-effects-tests` succeeded after correcting Windows `near` macro collision in test helper. `ctest --test-dir build-msvc -C Release -R '^visual-effects$' --output-on-failure` failed with 9 behavior failures: missing exposure, transfer, premultiplied processing, fallback diagnostics, adaptation, color, bloom, linear contract and benchmark processing. Disabled identity passed. This established missing-behavior RED; the crop and state cases independently passed placeholder because no mutation happened.
2. Implemented compute path. Build exposed a Windows `small` macro collision; renamed constant member to reduced. Real GPU tests then passed.
3. Follow-up regression tests exposed two actual defects: transition impulse fed into basic gain across frames, and full disable retained optional resources. RED output reported exactly those two failures. Stored base adaptation independently from transient output and released optional textures on full disable. GREEN: 14 cases passed.
4. Final source formatted with installed VS clang-format. Final command `cmake --build build-msvc --config Release --target vrst-visual-effects-tests`: exit0, no warnings. Final `ctest --test-dir build-msvc -C Release -R '^visual-effects$' -V`: 1/1 CTest, 14 internal cases, zero failures, 7.16s.

Tests cover disabled/bypass identity and zero work, neutral linear/alpha/resize, one-stop manual exposure and finite bounds, UNORM/SRGB/TYPELESS/BGRA and transfer toe accuracy, premultiplied alpha including alpha0, unsupported input fallback, bounded adaptation/rates/repeat/rewind, crop/bright HUD resistance, independent transition recovery/explicit reset, optional release, color controls, bloom threshold/spread/alpha/skipped passes, compute CB and graphics viewport preservation, and hardware timestamps.

Whole-suite verification: root owns final whole-branch suite due simultaneous Task2 integration. An all-tests run had already started before that direction arrived; it was interrupted at test15 with Ctrl+C after first14 tests passed, zero observed failures. It is NOT a completed whole-suite result.

## 4K GPU benchmark

Hardware: NVIDIA GeForce RTX 5090, real D3D11 hardware device. 3840x2160 linear RGBA16F input; 70 calls/mode, 10-frame warm-up, 59 available timing samples/mode. Test harness Flush + 1ms sleep permits asynchronous query collection; production core does neither. Reported timings measure dispatch workload only (shader compilation/lazy allocation/state preservation CPU cost excluded).

| Mode | Mean GPU ms | Dispatches/frame | Effects texture bytes |
| --- | ---: | ---: | ---: |
| Disabled baseline | no query / zero work | 0 | 0 |
| Manual color | 0.0745383 | 1 | 66,355,200 |
| Manual exposure | 0.0754207 | 1 | 66,355,200 |
| Bloom only | 0.158857 | 4 | 74,649,600 |
| All (adaptation + manual exposure + bloom + color) | 0.169476 | 5 | 74,649,616 |

Texture bytes exclude tiny constants, driver shader/query memory and caller input ownership. Meter/bloom disabled skips their respective dispatches; fully disabled also skips timestamp allocation/polling. Earlier synthetic runs varied with GPU clock/load (~.167-.188ms all effects), so these are measurements rather than guaranteed budgets.

## Concerns / limits

- No RTX4090 or actual VR-game-plus-capture/OBS-encoding sustained-load qualification performed here. No end-to-end latency/cadence claim. Root's Task5 must verify integration, overlay order, actual SDR conventions and state boundary; production allocation/first-use shader compilation can cause a one-time warm-up hitch when enabling.
- Fixed-grid robust log metering limits small HUD influence but cannot perfectly separate baked-in HUD or periodic sample aliasing. Region controls are explicit; no inferred overlay separation. The 0.6 linear luminance ceiling intentionally restrains highlight dominance.
- Test GPU readbacks occur only in test fixtures; production contains no CPU pixel or statistics readback. GPU query unavailability skips measurements.
- Supported contract intentionally excludes HDR/YUV/limited-range conversion, arrays/mips/MSAA and deferred contexts. Fallback diagnostic must be surfaced by hosts, not silently treated as processed linear output.
- Bypass retains the reusable full-resolution output and shader/CB resources for quick re-enable; optional bloom/meter textures are released. Explicit reset releases everything. The no-allocation/no-pass disabled requirement is satisfied.
- Same exact frame/time pair is treated as repeat; host must pass stable source IDs/timestamps (or call reset on source replacement) to avoid advancing adaptation for duplicate displayed frames.
Review fix: GPU surface allocation now constructs texture/SRV/UAV in a temporary and commits only on total success. Dedicated test-only fault injection verified failure at SRV and UAV boundaries: RED recovery pixel failed, GREEN visual-effects15cases passed7.85s. Shader production build contains no injection symbol. Stable source frame/time identity and source-reset requirements added to API contract. State test breadth minor deferred to final integration checks.
