cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED OUTPUT_DIR OR NOT DEFINED PREVIEW_VERSION)
  message(FATAL_ERROR "OUTPUT_DIR and PREVIEW_VERSION are required")
endif()

# Record the actual build time for provenance; this build has no expiry deadline.
unset(ENV{SOURCE_DATE_EPOCH})
string(TIMESTAMP build_unix_seconds "%s" UTC)
set(ENV{SOURCE_DATE_EPOCH} "${build_unix_seconds}")
string(TIMESTAMP built_at_utc "%Y-%m-%dT%H:%M:%SZ" UTC)
file(MAKE_DIRECTORY "${OUTPUT_DIR}/generated")
file(CONFIGURE OUTPUT "${OUTPUT_DIR}/generated/steadyxr_build_stamp.hpp" CONTENT
"#pragma once
#include <cstdint>
namespace vrst::preview {
inline constexpr std::uint64_t build_unix_seconds = @build_unix_seconds@ull;
inline constexpr std::uint64_t expires_unix_seconds = 0ull;
inline constexpr wchar_t build_expiry_utc[] = L\"No expiry\";
}
" @ONLY)
file(CONFIGURE OUTPUT "${OUTPUT_DIR}/steadyxr-build.json" CONTENT
"{
  \"product\": \"SteadyXR\",
  \"version\": \"@PREVIEW_VERSION@\",
  \"expires\": false,
  \"preview_days\": 0,
  \"build_unix_seconds\": @build_unix_seconds@,
  \"expires_unix_seconds\": 0,
  \"built_at_utc\": \"@built_at_utc@\",
  \"expires_at_utc\": null
}
" @ONLY)
