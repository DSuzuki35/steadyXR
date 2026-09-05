add_library(vr-stabilizer MODULE obs_filter.cpp)
target_compile_features(vr-stabilizer PRIVATE cxx_std_17)
target_link_libraries(vr-stabilizer PRIVATE vrst_receiver vrst_core OBS::libobs)
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
