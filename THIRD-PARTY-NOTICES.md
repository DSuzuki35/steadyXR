# Third-party notices

VR Stabilizer source is licensed under GPL-2.0-or-later; see LICENSE. Corresponding project source and build scripts accompany the Windows package under `source/`.

- **OBS Studio 32.2.2**: GPL-2.0-or-later. The plugin uses libobs's C API and links to the OBS installation's `obs.dll`; that DLL is not redistributed here. https://github.com/obsproject/obs-studio/tree/32.2.2
- **Valve OpenVR SDK 2.15.6**: BSD-3-Clause, copyright Valve Corporation. `openvr_api.dll` is redistributed unchanged with the sender. The complete license is included at `licenses/OpenVR-LICENSE.txt`. https://github.com/ValveSoftware/openvr/tree/v2.15.6
- **LLVM-MinGW toolchain 20260826**: the supplied cross-built binaries statically include compiler/runtime support distributed under LLVM's Apache-2.0 with exceptions in `licenses/LLVM-MinGW-LICENSE.txt`, with MinGW and winpthreads notices in the other `licenses/` files. https://github.com/mstorsjo/llvm-mingw/releases/tag/20260826

Windows and UCRT system DLLs are supplied by Windows. The native macOS verification harness uses SIMDe 0.8.2 headers (MIT); they are not included in the Windows package.

The implementation does not copy code from OpenXR-Layer-OBSMirror; that project is referenced as a capture option and conceptual precedent.
