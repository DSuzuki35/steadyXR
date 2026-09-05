#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${VRST_SIMDE_DIR:?Set VRST_SIMDE_DIR to extracted SIMDe 0.8.2 headers}"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
clang++ -std=c++17 -Iinclude -I.deps/obs-studio-32.2.2-sources/libobs \
  -I.deps/obs-sdk/include -I"$VRST_SIMDE_DIR" tests/gpu_tests.mm build/libvrst_core.a \
  /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
  -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
  -o build/vrst-gpu-tests
build/vrst-gpu-tests plugin/data/stabilize.effect
clang++ -std=c++17 -Iinclude -I.deps/obs-studio-32.2.2-sources/libobs \
  -I.deps/obs-sdk/include -I"$VRST_SIMDE_DIR" tests/image_gpu_tests.mm build/libvrst_core.a \
  /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
  -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
  -o build/vrst-image-gpu-tests
build/vrst-image-gpu-tests plugin/data/image-stabilize.effect
