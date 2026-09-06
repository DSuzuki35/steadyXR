#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${VRST_SIMDE_DIR:?Set VRST_SIMDE_DIR to extracted SIMDe 0.8.2 headers}"
vrst_obs_source="${VRST_NATIVE_OBS_SOURCE_DIR:-$PWD/.deps/obs-studio-32.2.2-sources}"
vrst_generated="${VRST_OBS_GENERATED_DIR:-$PWD/.deps/obs-sdk/include}"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
clang++ -std=c++17 -Iinclude -I"$vrst_obs_source/libobs" \
  -I"$vrst_generated" -I"$VRST_SIMDE_DIR" tests/gpu_tests.mm build/libvrst_core.a \
  /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
  -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
  -o build/vrst-gpu-tests
build/vrst-gpu-tests plugin/data/stabilize.effect
clang++ -std=c++17 -Iinclude -I"$vrst_obs_source/libobs" \
  -I"$vrst_generated" -I"$VRST_SIMDE_DIR" tests/image_gpu_tests.mm build/libvrst_core.a \
  /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
  -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
  -o build/vrst-image-gpu-tests
build/vrst-image-gpu-tests plugin/data/image-stabilize.effect

# The pipeline harness loads a real native OBS module to locate its effect.
# Use headers matching the installed OBS app for that module's API version.
if [ -n "${VRST_NATIVE_OBS_SOURCE_DIR:-}" ]; then
  cmake -S . -B build-plugin-native -DCMAKE_BUILD_TYPE=Release -DVRST_BUILD_PLUGIN=ON \
    -DVRST_OBS_SOURCE_DIR="$vrst_obs_source" -DVRST_OBS_GENERATED_DIR="$vrst_generated" \
    -DVRST_SIMDE_DIR="$VRST_SIMDE_DIR" \
    -DVRST_OBS_LIBRARY=/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs
  cmake --build build-plugin-native --target vr-stabilizer --parallel
  clang++ -std=c++17 -Wno-deprecated-declarations -Iinclude -Iplugin \
    -I"$vrst_obs_source/libobs" -I"$vrst_generated" -I"$VRST_SIMDE_DIR" \
    tests/streamer_gpu_tests.mm plugin/image_filter.cpp build-plugin-native/libvrst_core.a \
    /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
    -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
    -o build-plugin-native/vrst-streamer-gpu-tests
  build-plugin-native/vrst-streamer-gpu-tests \
    build-plugin-native/plugin/vr-stabilizer.plugin/Contents/MacOS/vr-stabilizer plugin/data
else
  echo "Set VRST_NATIVE_OBS_SOURCE_DIR to also run the buffered streamer GPU pipeline tests."
fi
