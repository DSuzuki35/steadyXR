# SteadyXR licensing and third-party notices

The public SteadyXR 1.0.0 standalone application and OpenXR capture layer use PolyForm Noncommercial 1.0.0 for their original code and assets. Earlier GPL grants remain in force for the code distributed under them. [SteadyXR license](LICENSE) · [GNU GPL version 2](licenses/GPL-2.0.txt)

Required Notice: Copyright (c) 2026 Dan Suzuki (https://github.com/DSuzuki35/steadyXR)

## OpenXR SDK headers

Copyright 2017–2024, The Khronos Group Inc. SteadyXR uses the OpenXR SDK 1.1.43 headers at commit `781f2eab3698d653c804ecbd11e0aed47eaad1c6`. Their SPDX license expression is `Apache-2.0 OR MIT`. The source contains the headers and upstream notices in `third_party/openxr/`. The portable package carries both license texts. The game loads the capture layer through its OpenXR loader. [Upstream headers](https://github.com/KhronosGroup/OpenXR-SDK/tree/781f2eab3698d653c804ecbd11e0aed47eaad1c6) · [Apache License 2.0](licenses/OpenXR-Apache-2.0.txt) · [MIT license](licenses/OpenXR-MIT.txt)

## Alternative toolchains

**LLVM-MinGW.** Alternative cross-builds can include LLVM compiler/runtime support under Apache-2.0 WITH LLVM-exception and MinGW-w64 or winpthreads runtime support. The repository retains the corresponding notices. The public standalone Windows build uses Visual Studio with the static Microsoft C++ runtime. Windows supplies its system DLLs. [LLVM-MinGW toolchain](https://github.com/mstorsjo/llvm-mingw) · [LLVM license and exceptions](licenses/LLVM-MinGW-LICENSE.txt) · [MinGW-w64 notices](licenses/COPYING.MinGW-w64-COPYING.MinGW-w64.txt) · [Winpthreads notice](licenses/Winpthreads-COPYING.txt)
