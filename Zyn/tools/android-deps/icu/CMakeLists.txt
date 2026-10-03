cmake_minimum_required(VERSION 3.22)
project(zyn_icu_android VERSION 78.3 LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI MATCHES "^(arm64-v8a|x86_64)$")
  message(FATAL_ERROR "Use the NDK toolchain with arm64-v8a or x86_64")
endif()
if(NOT DEFINED ANDROID_PLATFORM_LEVEL OR ANDROID_PLATFORM_LEVEL LESS 29)
  message(FATAL_ERROR "This ICU package requires Android API 29 or later")
endif()
set(ICU_SOURCE_DIR "" CACHE PATH "Unmodified ICU 78.3 source directory containing common and i18n")
set(ICU_DATA_FILE "" CACHE FILEPATH "Official little-endian icudt78l.dat")
option(ICU_BUILD_SMOKE "Build the on-device data verification executable" ON)
foreach(required common/sources.txt i18n/sources.txt common/unicode/uvernum.h)
  if(NOT EXISTS "${ICU_SOURCE_DIR}/${required}")
    message(FATAL_ERROR "Missing ICU input: ${ICU_SOURCE_DIR}/${required}")
  endif()
endforeach()
file(READ "${ICU_SOURCE_DIR}/common/unicode/uvernum.h" version_header)
if(NOT version_header MATCHES "#define U_ICU_VERSION \"78\\.3\"")
  message(FATAL_ERROR "This wrapper is pinned to ICU 78.3")
endif()
file(SHA256 "${ICU_DATA_FILE}" data_sha)
if(NOT data_sha STREQUAL "d5cf2a40dccbe471781ec7af85693bff542ff12f0b670c9630c4e72d60714b8b")
  message(FATAL_ERROR "ICU data must be the verified full release-78.3 little-endian package")
endif()

function(icu_upstream_sources library result)
  # The upstream Makefile.in reads this same authoritative list. Do not glob
  # platform-specific files or maintain a second hand-written source list.
  file(STRINGS "${ICU_SOURCE_DIR}/${library}/sources.txt" names)
  set(files "")
  foreach(name IN LISTS names)
    string(STRIP "${name}" name)
    if(NOT name MATCHES "^[A-Za-z0-9_-]+\\.cpp$" OR NOT EXISTS "${ICU_SOURCE_DIR}/${library}/${name}")
      message(FATAL_ERROR "Unexpected upstream ${library} source: ${name}")
    endif()
    list(APPEND files "${ICU_SOURCE_DIR}/${library}/${name}")
  endforeach()
  list(LENGTH files count)
  message(STATUS "ICU ${library}: ${count} upstream sources")
  set(${result} "${files}" PARENT_SCOPE)
endfunction()

# Upstream pkg_genc.cpp uses the same global ELF object and 16-byte alignment.
# incbin preserves the official data package byte-for-byte without host ICU
# generators, and never links source/stubdata/stubdata.cpp.
file(TO_CMAKE_PATH "${ICU_DATA_FILE}" ICU_DATA_ASM_PATH)
configure_file(icudata.S.in "${CMAKE_CURRENT_BINARY_DIR}/icudata.S" @ONLY)
add_library(icudata SHARED "${CMAKE_CURRENT_BINARY_DIR}/icudata.S")
set_target_properties(icudata PROPERTIES LINKER_LANGUAGE C)
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/icudata.S" PROPERTY OBJECT_DEPENDS "${ICU_DATA_FILE}")

icu_upstream_sources(common common_sources)
icu_upstream_sources(i18n i18n_sources)
add_library(icuuc SHARED ${common_sources})
target_compile_features(icuuc PRIVATE cxx_std_17)
target_compile_definitions(icuuc PRIVATE U_COMMON_IMPLEMENTATION)
target_include_directories(icuuc PUBLIC "${ICU_SOURCE_DIR}/common")
target_link_libraries(icuuc PRIVATE icudata dl m)

add_library(icui18n SHARED ${i18n_sources})
target_compile_features(icui18n PRIVATE cxx_std_17)
target_compile_definitions(icui18n PRIVATE U_I18N_IMPLEMENTATION)
target_include_directories(icui18n PUBLIC "${ICU_SOURCE_DIR}/i18n" "${ICU_SOURCE_DIR}/common")
target_link_libraries(icui18n PRIVATE icuuc m)

foreach(library icudata icuuc icui18n)
  # Android ships its own libicu*.so in another linker namespace. Give the
  # bundled libraries private SONAMEs so libZyn always resolves ICU 78 here.
  # Public ICU entry points keep the upstream _78 suffix.
  set_target_properties(${library} PROPERTIES
    OUTPUT_NAME "zyn${library}"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
  target_link_options(${library} PRIVATE "-Wl,--no-undefined")
endforeach()

if(ICU_BUILD_SMOKE)
  add_executable(icu_data_smoke data_smoke.cpp)
  target_compile_features(icu_data_smoke PRIVATE cxx_std_17)
  target_link_libraries(icu_data_smoke PRIVATE icui18n icuuc icudata)
endif()
