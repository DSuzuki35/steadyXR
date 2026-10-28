# SteadyXR 0.13.1 — VectorXR compatibility and permanent builds

## Scope

- Preserve the 0.13.0 symmetric render-extension controls and presets.
- Install/upgrade the capture layer in the system-wide 64-bit OpenXR implicit layer registry. Disable legacy user entries after migration. Ask Windows for administrator approval through a separate helper; keep the preview unelevated.
- Place SteadyXR before VectorXR while preserving other layers' relative order and enabled states.
- Avoid a pre-instance optional eye-gaze query through VectorXR. Explicit application extension requests still pass through. Automatic eye-gaze support may be unavailable if the application does not request it; head-pose stabilization is unaffected.
- Remove the build deadline from the new app, capture DLL and direct OBS source. The build timestamp remains in metadata for provenance.

## Diagnosis

VectorXR 0.19.3 Frame Generation bridges the game's D3D11 images into a D3D12 runtime session. With SteadyXR after VectorXR, its D3D11 capture path sees the converted backend and reports `Unsupported graphics backend: D3D11 required`.

Reordering the previous capture DLL exposed a separate startup crash. The LMU minidump recorded exception `0xc0000005` at address zero, returning into `XR_APILAYER_DIENERTECH_VECTORXR.dll+0x69e6d`. Disassembly shows an indirect call through an unset function pointer. SteadyXR's optional eye-gaze extension discovery called VectorXR's `xrGetInstanceProcAddr` before VectorXR's instance creation initialized its downstream function pointer. The regression fixture rejects that same early call; it failed before the fix and passes afterward.

The fix captures **original game frames before VectorXR**. It does not capture Frame Generation's synthetic intermediate frames and does not add a D3D12 capture backend. Turning Frame Generation off before restarting the game is the temporary workaround to test with 0.13.0.

Primary implementation reference: [VectorXR OpenXR layer](https://github.com/DienerTech/vectorxr/blob/master/layer/src/openxr_layer.cpp), especially session creation and `GetInstanceProcAddr`.

## Signed binary boundary

The previously supplied signed DLL is unchanged:

`D4DD5D550676C7EFCA22657472D896557CFC2260CD1EA750317EAEBED841137A`

That older binary still has its embedded 2026-11-19 06:53:09 UTC deadline. Changing the expiry or startup behavior requires a rebuilt DLL and a fresh signature. The 0.13.1 candidate is therefore delivered under `Needs-signing`, without replacing the installed signed layer or the default launcher. Its new metadata explicitly sets `expires: false` and `expires_at_utc: null`.

Sign the new `SteadyXR-OpenXR.dll`, return the complete signed file, and verify/repackage it before installing. Signing changes the file hash, so checksums and archives must be regenerated from that exact signed file. Do not put the old signed DLL into the new package: it contains neither the startup fix nor permanent validity.

## Validation

Automated validation includes a downstream VectorXR startup fixture (direct and intervening layer, with and without application-requested gaze), permanent render expansion at `UINT64_MAX`, live GPU copies at that timestamp, no-expiry metadata generation, and private-registry migration/rollback tests. Full build and suite results are recorded with the delivered candidate.

Real LMU + VectorXR Frame Generation validation remains pending a signed 0.13.1 DLL. After installation, restart LMU and check the layer order/backend, preview frames and pose correction. Keep symmetric render extension enabled in LMU. The existing signed configuration was restored after the earlier crash; delivery does not alter it.
