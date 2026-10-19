add_library(vr-stabilizer MODULE obs_filter.cpp image_filter.cpp timing_overlay.cpp presets.cpp calibration_session.cpp)
target_compile_features(vr-stabilizer PRIVATE cxx_std_17)
target_link_libraries(vr-stabilizer PRIVATE vrst_receiver vrst_core OBS::libobs)
if(WIN32 AND TARGET vrst-preview-renderer)
  target_sources(vr-stabilizer PRIVATE openxr_source.cpp openxr_source_settings.cpp spectator_effects_filter.cpp)
  target_compile_definitions(vr-stabilizer PRIVATE VRST_WITH_OPENXR_SOURCE=1)
  target_link_libraries(vr-stabilizer PRIVATE vrst-preview-renderer vrst-preview-build-expiry)
  if(VRST_BUILD_TESTS)
    add_executable(vrst-openxr-source-settings-tests openxr_source_settings_tests.cpp openxr_source_settings.cpp)
    target_link_libraries(vrst-openxr-source-settings-tests PRIVATE OBS::libobs vrst-preview-renderer)
    add_executable(vrst-openxr-module-tests ../tests/openxr_module_tests.cpp)
    target_link_libraries(vrst-openxr-module-tests PRIVATE OBS::libobs vrst-preview-transport d3d11 dxgi)
    set(VRST_OBS_RUNTIME_DIR "$ENV{ProgramFiles}/obs-studio" CACHE PATH "OBS installation used by native module tests")
    if(EXISTS "${VRST_OBS_RUNTIME_DIR}/bin/64bit/obs.dll")
      file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/obs-openxr-test-config")
      add_test(NAME obs-openxr-settings COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${VRST_OBS_RUNTIME_DIR}/bin/64bit;$ENV{PATH}" "$<TARGET_FILE:vrst-openxr-source-settings-tests>")
      add_test(NAME obs-openxr-module COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${VRST_OBS_RUNTIME_DIR}/bin/64bit;$ENV{PATH}" "$<TARGET_FILE:vrst-openxr-module-tests>"
        "$<TARGET_FILE:vr-stabilizer>" "${CMAKE_CURRENT_SOURCE_DIR}/data" "${VRST_OBS_RUNTIME_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/obs-openxr-test-config")
      set_tests_properties(obs-openxr-settings obs-openxr-module PROPERTIES TIMEOUT 60)
    endif()
  endif()
endif()
set_target_properties(vr-stabilizer PROPERTIES PREFIX "" OUTPUT_NAME "vr-stabilizer")
if(MSVC)
  target_compile_options(vr-stabilizer PRIVATE /W4 /utf-8)
else()
  target_compile_options(vr-stabilizer PRIVATE -Wall -Wextra)
endif()

if(APPLE)
  set_target_properties(vr-stabilizer PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_GUI_IDENTIFIER "local.vrst.vr-stabilizer")
  add_custom_command(TARGET vr-stabilizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/data"
      "$<TARGET_BUNDLE_DIR:vr-stabilizer>/Contents/Resources")
  install(TARGETS vr-stabilizer
    BUNDLE DESTINATION obs-plugins
    LIBRARY DESTINATION obs-plugins)
else()
  install(TARGETS vr-stabilizer
    RUNTIME DESTINATION obs-plugins/64bit
    LIBRARY DESTINATION obs-plugins/64bit)
  install(DIRECTORY data/ DESTINATION data/obs-plugins/vr-stabilizer)
endif()
