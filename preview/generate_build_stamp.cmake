cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED OUTPUT_DIR OR NOT DEFINED PREVIEW_VERSION)
  message(FATAL_ERROR "OUTPUT_DIR and PREVIEW_VERSION are required")
endif()

# This runs at build time. A reproducible-build environment must not substitute
# a source/commit date for the actual build's UTC deadline.
unset(ENV{SOURCE_DATE_EPOCH})
string(TIMESTAMP build_unix_seconds "%s" UTC)
math(EXPR expires_unix_seconds "${build_unix_seconds} + 30 * 86400")
set(ENV{SOURCE_DATE_EPOCH} "${build_unix_seconds}")
string(TIMESTAMP built_at_utc "%Y-%m-%dT%H:%M:%SZ" UTC)
set(ENV{SOURCE_DATE_EPOCH} "${expires_unix_seconds}")
string(TIMESTAMP expires_at_utc "%Y-%m-%dT%H:%M:%SZ" UTC)
string(TIMESTAMP expires_display "%Y-%m-%d %H:%M:%S UTC" UTC)
file(MAKE_DIRECTORY "${OUTPUT_DIR}/generated")
file(CONFIGURE OUTPUT "${OUTPUT_DIR}/generated/steadyxr_build_stamp.hpp" CONTENT
"#pragma once
#include <cstdint>
namespace vrst::preview {
inline constexpr std::uint64_t build_unix_seconds = @build_unix_seconds@ull;
inline constexpr std::uint64_t expires_unix_seconds = @expires_unix_seconds@ull;
inline constexpr wchar_t build_expiry_utc[] = L\"@expires_display@\";
}
" @ONLY)
file(CONFIGURE OUTPUT "${OUTPUT_DIR}/steadyxr-build.json" CONTENT
"{
  \"product\": \"SteadyXR\",
  \"version\": \"@PREVIEW_VERSION@\",
  \"preview_days\": 30,
  \"build_unix_seconds\": @build_unix_seconds@,
  \"expires_unix_seconds\": @expires_unix_seconds@,
  \"built_at_utc\": \"@built_at_utc@\",
  \"expires_at_utc\": \"@expires_at_utc@\"
}
" @ONLY)
