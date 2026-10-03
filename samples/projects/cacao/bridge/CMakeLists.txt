cmake_minimum_required(VERSION 3.20)
project(CacaoDciBridge LANGUAGES CXX)

set(CACAO_ROOT "E:/Dev/C++/Cacao" CACHE PATH "Cacao source/build root")
set(CACAO_LIBRARY "${CACAO_ROOT}/cmake-build-debug/Cacao.lib" CACHE FILEPATH "Cacao import library")

add_library(CacaoDciBridge SHARED cacao_dci_bridge.cpp)
target_compile_features(CacaoDciBridge PRIVATE cxx_std_20)
target_include_directories(CacaoDciBridge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../.adapter_include" "${CACAO_ROOT}/include" "${CACAO_ROOT}/cmake-build-debug/Debug/include")
target_link_libraries(CacaoDciBridge PRIVATE "${CACAO_LIBRARY}")
target_compile_definitions(CacaoDciBridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

set_target_properties(CacaoDciBridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
