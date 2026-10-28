# SteadyXR 0.13.0 verification

## Change

The render-extension dialog has a **Symmetric (both eyes)** checkbox. Enabling
it mirrors the selected eye into the other eye; subsequent edits link the
two inward and the two outward margins. Disabling the lock retains both
eyes' values. The 10% inward preset enables the lock. Save/reopen and complete
INI presets preserve it. Existing INIs keep independent values by default.
Crop and stabilization reserves remain independent.

LMU's 2026-10-21 startup trace rejected unequal recommended eye dimensions.
The existing layer independently scales each eye's recommended width. The
checkbox keeps extension totals equal without changing the capture DLL.
This assumes the runtime already recommends equal native dimensions.

## Preserved capture binary

The supplied signed `SteadyXR-OpenXR.dll` from 0.12.0 is copied byte for byte:

`d4dd5d550676c7efca22657472d896557cfc2260cd1ea750317eaebed841137a`

Windows verifies its Authenticode signature from Gomez Sim Industries LLC.
The embedded expiry comparison was checked in the binary: Unix 1795071189,
**2026-11-19 06:53:09 UTC**. A 0.13.0 app build does not extend this deadline.
Changing it would require rebuilding and signing again.

Packaging accepts a pinned capture binary, original build metadata and original
corresponding source archive. It rejects a wrong SHA-256 before changing a
package. The source ZIP retains the original capture source ZIP under
`prebuilt-capture`; separate capture metadata records its unchanged deadline.

## Verification

- New hidden native-control regression test failed before implementation
  because the checkbox was absent, then passed. Covers both edit directions,
  copying the selected profile on enable, boundaries, unlock, save/reopen,
  preset reload and all controls staying inside the dialog.
- Packaging regression failed before the preservation option was implemented,
  then passed. Checks exact binary bytes in the ready folder and ZIP, original
  metadata/source retention, and refusal of a changed DLL without replacing
  the existing delivery.
- Full native build with LLVM-MinGW 20260826, static executable, shared-library
  and module C++ runtime linkage; OBS 32.2.2 SDK and installed runtime.
- All 38 CTest cases passed across the full run and the corrected OBS-module
  rerun. The initial OBS-module load failed because the local module link
  configuration omitted static C++ runtime linkage. Correcting
  `CMAKE_MODULE_LINKER_FLAGS=-static` removed the extra runtime DLL dependency;
  the native OBS module test then passed (25.23 seconds).
- Python packaging and delivery fixtures passed (3 and 2 tests).
- The captured advanced-dialog test image was inspected: both checkboxes,
  sliders, save button and explanatory text are visible without overlap.
- The source of the OpenXR capture layer was not changed. Fresh build outputs
  of that target are test intermediates only, not the delivered capture DLL.

The previous investigation verified real LMU OpenXR capture with the signed
DLL and 60 FPS. This release's linked sliders were verified in native tests;
no additional in-game render-extension restart was performed for delivery.
OBS/OpenXR registrations and the user's running game are not changed by delivery.
