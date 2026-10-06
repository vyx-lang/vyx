# vyx_codegen — private Vyx compiler backend, exposed as one shared library.
#
# Inputs:
#   include/vyx_codegen.h      — current C ABI surface (112 VYX_API exports)
#   src/vyx_codegen.cpp        — LLVM bridge implementation
#   src/vyx_bootstrap_rt.cpp   — bootstrap runtime string/time helpers
#   src/vyx_service_memory.cpp — compiler service allocation scopes
#   src/vyx_dap_launcher.cpp   — SDK debugger process launcher
#
# Outputs:
#   <build>/vyx_compiler_backend.dll         (Windows)
#   <build>/libvyx_compiler_backend.so       (Linux)
#   <build>/libvyx_compiler_backend.dylib    (macOS)
#
# This CMakeLists is meant to be `add_subdirectory(vyx_codegen)`-ed from
# the project root; LLVM is therefore already located via find_package
# in the parent scope and ${LLVM_LIBS} is populated.
cmake_minimum_required(VERSION 3.20)

# When this folder is configured stand-alone (without the parent), still
# locate LLVM the same way the parent does. Otherwise reuse parent state.
if(NOT TARGET vyxc_core)
    project(vyx_compiler_backend LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(NOT DEFINED LLVM_DIR)
        if(DEFINED ENV{LLVM_ROOT} AND NOT "$ENV{LLVM_ROOT}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{LLVM_ROOT}" VYX_LLVM_ROOT)
            set(LLVM_DIR "${VYX_LLVM_ROOT}/lib/cmake/llvm")
        else()
            message(FATAL_ERROR "LLVM_ROOT is not set. Set LLVM_ROOT to the LLVM SDK root or pass -DLLVM_DIR=<path>.")
        endif()
    endif()
    find_package(LLVM 22.1 REQUIRED CONFIG)
    add_definitions(${LLVM_DEFINITIONS})
    include_directories(${LLVM_INCLUDE_DIRS})
    llvm_map_components_to_libnames(LLVM_LIBS
        core support target mc analysis transformutils passes scalaropts instcombine ipo irreader
        bitwriter bitreader
        ${LLVM_TARGETS_TO_BUILD}
        codegen selectiondag asmprinter asmparser globalisel
        orcjit executionengine runtimedyld object jitlink
    )
endif()

add_library(vyx_compiler_backend SHARED
    include/vyx_codegen.h
    src/vyx_codegen.cpp
    src/vyx_bootstrap_rt.cpp
    src/vyx_service_memory.cpp
    src/vyx_lsp_bindings.cpp
    src/vyx_dap_launcher.cpp
)

target_compile_definitions(vyx_compiler_backend PRIVATE VYX_CODEGEN_BUILD)

target_include_directories(vyx_compiler_backend
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(TARGET LLVM)
    # Linux/macOS LLVM packages commonly expose one versioned shared library.
    # It is the only stable way to satisfy InitializeAll* for every backend
    # compiled into that SDK without hard-coding its Targets.def contents.
    target_link_libraries(vyx_compiler_backend PRIVATE LLVM)
else()
    # The bundled Windows SDK ships static component libraries only.
    target_link_libraries(vyx_compiler_backend PRIVATE ${LLVM_LIBS})
endif()

set_target_properties(vyx_compiler_backend PROPERTIES
    OUTPUT_NAME              "vyx_compiler_backend"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET     hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# The bootstrap executable imports printf through the compiler backend. Clang's CRT
# linkage used to expose that transitively, while MSVC requires an explicit
# DLL export. The definition file aliases that ABI name to a local bridge.
if(WIN32)
    target_link_options(vyx_compiler_backend PRIVATE
        "LINKER:/DEF:${CMAKE_CURRENT_SOURCE_DIR}/vyx_rt_exports.def"
    )
endif()

if(MSVC)
    target_compile_options(vyx_compiler_backend PRIVATE /GR- /EHsc /W3)
else()
    target_compile_options(vyx_compiler_backend PRIVATE -fno-rtti -fno-exceptions -Wall)
    if(UNIX AND NOT APPLE)
        target_link_options(vyx_compiler_backend PRIVATE -Wl,-z,defs)
    endif()
endif()

# Smoke test executable: links the freshly built DLL and exercises the core
# module/type/function/builder/emit path; produces out.ll containing `ret i32 42`.
add_executable(vyx_codegen_smoke test/smoke.cpp)
target_link_libraries(vyx_codegen_smoke PRIVATE vyx_compiler_backend)
target_include_directories(vyx_codegen_smoke
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_options(vyx_codegen_smoke PRIVATE /EHsc /W3)
endif()
