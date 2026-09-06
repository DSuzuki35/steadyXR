#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${VRST_SIMDE_DIR:?Set VRST_SIMDE_DIR to extracted SIMDe headers}"
: "${VRST_NATIVE_OBS_SOURCE_DIR:?Set VRST_NATIVE_OBS_SOURCE_DIR to OBS headers compatible with the installed app}"
vrst_generated="${VRST_OBS_GENERATED_DIR:-$PWD/.deps/obs-sdk/include}"
cmake -S . -B build-plugin-native -DCMAKE_BUILD_TYPE=Release -DVRST_BUILD_PLUGIN=ON \
  -DVRST_OBS_SOURCE_DIR="$VRST_NATIVE_OBS_SOURCE_DIR" -DVRST_OBS_GENERATED_DIR="$vrst_generated" \
  -DVRST_SIMDE_DIR="$VRST_SIMDE_DIR" \
  -DVRST_OBS_LIBRARY=/Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs
cmake --build build-plugin-native --parallel
clang++ -std=c++17 -Wno-deprecated-declarations -Iinclude \
  -I"$VRST_NATIVE_OBS_SOURCE_DIR/libobs" -I"$vrst_generated" -I"$VRST_SIMDE_DIR" \
  tests/image_module_tests.mm \
  /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
  -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
  -o build-plugin-native/vrst-image-module-tests
build-plugin-native/vrst-image-module-tests \
  build-plugin-native/plugin/vr-stabilizer.plugin/Contents/MacOS/vr-stabilizer plugin/data

clang++ -std=c++17 -Wno-deprecated-declarations -Iinclude \
  -I"$VRST_NATIVE_OBS_SOURCE_DIR/libobs" -I"$vrst_generated" -I"$VRST_SIMDE_DIR" \
  tests/streamer_module_tests.mm \
  /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
  -framework AppKit -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks \
  -o build-plugin-native/vrst-streamer-module-tests
build-plugin-native/vrst-streamer-module-tests \
  build-plugin-native/plugin/vr-stabilizer.plugin/Contents/MacOS/vr-stabilizer plugin/data
clang++ -std=c++17 -Iplugin \
  -I"$VRST_NATIVE_OBS_SOURCE_DIR/libobs" -I"$vrst_generated" -I"$VRST_SIMDE_DIR" \
  tests/presets_tests.cpp plugin/presets.cpp \
  /Applications/OBS.app/Contents/Frameworks/libobs.framework/Versions/A/libobs \
  -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks -o build-plugin-native/vrst-presets-tests
build-plugin-native/vrst-presets-tests
