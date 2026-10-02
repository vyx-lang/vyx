cmake_minimum_required(VERSION 3.25)
project(vyx_container_bench LANGUAGES CXX)

set(VYX_BENCH_PROFILE "matched-o2" CACHE STRING "Benchmark optimization profile")
set_property(CACHE VYX_BENCH_PROFILE PROPERTY STRINGS matched-o2 native-release)
if (NOT VYX_BENCH_PROFILE STREQUAL "matched-o2" AND
    NOT VYX_BENCH_PROFILE STREQUAL "native-release")
    message(FATAL_ERROR "VYX_BENCH_PROFILE must be matched-o2 or native-release")
endif()

if (MSVC)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

add_executable(containers_bench main.cpp)

if (MSVC)
    target_compile_options(containers_bench PRIVATE /std:c++latest /O2 /W4)
    target_compile_definitions(containers_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(containers_bench PRIVATE psapi)
else()
    if (VYX_BENCH_PROFILE STREQUAL "native-release")
        target_compile_options(containers_bench PRIVATE -O3 -Wall -Wextra -Wpedantic)
    else()
        target_compile_options(containers_bench PRIVATE -O2 -Wall -Wextra -Wpedantic)
    endif()
endif()

target_compile_definitions(containers_bench PRIVATE VYX_BENCH_PROFILE="${VYX_BENCH_PROFILE}")
