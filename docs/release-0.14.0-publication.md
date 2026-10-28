# SteadyXR 0.14.0 publication provenance

The 0.14.0 production sources were restored from the previously delivered source archive on 2026-10-28. The original prebuilt packages are published without rebuilding or modifying their binaries.

Original source archive SHA-256:
`b90e760bea66ae3c61f3bfdec46b73a580fd5d846b9dce2f1dabbc654642b0e2`

Signed `SteadyXR-OpenXR.dll` SHA-256:
`74a6943f014d85b81f301da852b61d5a8cb2251a199130978bf4cbdba097d91d`

Windows Authenticode verification returned **Valid** on 2026-10-28, with signer **Gomez Sim Industries LLC**. The same DLL hash was verified inside both the standalone and combined release ZIPs. All four original ZIPs passed CRC verification and matched their delivered SHA-256 sidecars.

The source tag additionally corrects three test assertions comparing independently constructed `XrView` structures. Diagnostic output confirmed that every named field matched while offsets 4 and 5 (ABI padding) differed. Tests now compare type, next, pose and FOV explicitly. Production layer code is unchanged from the archived source. The original source ZIP remains available as the exact original build input; GitHub's tag archive includes the test-only portability correction and this publication note.

Fresh verification builds target test executables, the sender and the standalone EXE, not the capture DLL. All 39 standalone/sender/capture checks passed on 2026-10-28 (37.92 seconds). The original October 22 release report covers 41 checks including OBS; the two OBS checks are not part of this publication rerun.

Live LMU/VectorXR compatibility and game performance remain hardware acceptance items. A valid digital signature alone is not evidence of anti-cheat compatibility.

For the forthcoming 0.15.0 work, the signed DLL above must be reused byte-for-byte. A newer UI version does not imply a newer capture DLL; preserve that component distinction in packaging and connection diagnostics.
