cmake_minimum_required(VERSION 3.21)
project(zyn_android_deps_verify C)
if(NOT ANDROID OR NOT ZYN_ROOT)
    message(FATAL_ERROR "Provide the Android NDK toolchain and ZYN_ROOT")
endif()
add_library(zyn_text_bridge_check SHARED "${ZYN_ROOT}/src/nut/text_bridge.c")
target_include_directories(zyn_text_bridge_check PRIVATE
    "${ZYN_ROOT}/vendor/FreeType/include"
    "${ZYN_ROOT}/vendor/HarfBuzz/include"
    "${ZYN_ROOT}/vendor/ICU/include")
target_link_options(zyn_text_bridge_check PRIVATE -Wl,--no-undefined)
target_link_libraries(zyn_text_bridge_check PRIVATE
    "${ZYN_ROOT}/vendor/FreeType/lib/android/${ANDROID_ABI}/libfreetype.so"
    "${ZYN_ROOT}/vendor/HarfBuzz/lib/android/${ANDROID_ABI}/libharfbuzz.so"
    "${ZYN_ROOT}/vendor/ICU/lib/android/${ANDROID_ABI}/libzynicui18n.so"
    "${ZYN_ROOT}/vendor/ICU/lib/android/${ANDROID_ABI}/libzynicuuc.so"
    "${ZYN_ROOT}/vendor/ICU/lib/android/${ANDROID_ABI}/libzynicudata.so")
add_executable(zyn_android_deps_verify smoke.c)
target_include_directories(zyn_android_deps_verify PRIVATE
    "${ZYN_ROOT}/vendor/SDL3/include"
    "${ZYN_ROOT}/vendor/FreeType/include"
    "${ZYN_ROOT}/vendor/HarfBuzz/include"
    "${ZYN_ROOT}/vendor/ICU/include")
target_link_options(zyn_android_deps_verify PRIVATE -Wl,--no-undefined)
target_link_libraries(zyn_android_deps_verify PRIVATE zyn_text_bridge_check
    "${ZYN_ROOT}/vendor/SDL3/lib/android/${ANDROID_ABI}/libSDL3.so"
    "${ZYN_ROOT}/vendor/FreeType/lib/android/${ANDROID_ABI}/libfreetype.so"
    "${ZYN_ROOT}/vendor/HarfBuzz/lib/android/${ANDROID_ABI}/libharfbuzz.so"
    "${ZYN_ROOT}/vendor/ICU/lib/android/${ANDROID_ABI}/libzynicui18n.so"
    "${ZYN_ROOT}/vendor/ICU/lib/android/${ANDROID_ABI}/libzynicuuc.so"
    "${ZYN_ROOT}/vendor/ICU/lib/android/${ANDROID_ABI}/libzynicudata.so")
