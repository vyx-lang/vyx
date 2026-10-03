cmake_minimum_required(VERSION 3.20)
project(VyxLan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match LLVM's MSVC runtime linkage (static)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Find LLVM: use -DLLVM_DIR=<path> to override, otherwise derive it from LLVM_ROOT.
if(NOT DEFINED LLVM_DIR)
    if(DEFINED ENV{LLVM_ROOT} AND NOT "$ENV{LLVM_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{LLVM_ROOT}" VYX_LLVM_ROOT)
        set(LLVM_DIR "${VYX_LLVM_ROOT}/lib/cmake/llvm")
    else()
        message(FATAL_ERROR "LLVM_ROOT is not set. Set LLVM_ROOT to the LLVM SDK root or pass -DLLVM_DIR=<path>.")
    endif()
endif()
find_package(LLVM 22.1 REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# Map LLVM components to library names
llvm_map_components_to_libnames(LLVM_LIBS
    core
    support
    irreader
    passes
    ${LLVM_TARGETS_TO_BUILD}
    target
    mc
    transformutils
    scalaropts
    instcombine
    analysis
    codegen
    selectiondag
    asmprinter
    asmparser
    globalisel
    ipo
    orcjit
    executionengine
    runtimedyld
    object
    jitlink
)

# Core compiler library (shared by all targets)
file(GLOB VYXC_CODEGEN_SOURCES CONFIGURE_DEPENDS
    src/CodeGen/*.cpp
)

set(VYXC_CORE_SOURCES
    src/Common/Diagnostics.cpp
    src/Common/Package.cpp
    src/Lexer/Lexer.cpp
    src/Parser/Parser.cpp
    src/Parser/ParserDecl.cpp
    src/Parser/ParserType.cpp
    src/Sema/Sema.cpp
    src/Sema/SemaDecl.cpp
    src/Sema/SemaStmt.cpp
    src/Sema/SemaResolve.cpp
    src/Sema/SemaComptime.cpp
    src/Sema/SemaCloneHelpers.cpp
    src/Parser/ParserExpr.cpp
    src/Parser/ParserExprPrimary.cpp
    src/Parser/ParserStmt.cpp
    src/Sema/SemaExpr.cpp
    src/Sema/SemaExprOps.cpp
    src/Sema/SemaGeneric.cpp
    src/Sema/TemplateResolver.cpp
    src/Sema/LangItemRegistry.cpp
    src/Mono/Monomorphize.cpp
    ${VYXC_CODEGEN_SOURCES}
)

add_library(vyxc_core STATIC ${VYXC_CORE_SOURCES})
target_include_directories(vyxc_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vyxc_core PUBLIC ${LLVM_LIBS})
if(MSVC)
    target_compile_options(vyxc_core PRIVATE /GR- /EHsc /W3)
    if(ENABLE_ASAN)
        target_compile_options(vyxc_core PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(vyxc_core PRIVATE -fno-rtti -fno-exceptions -Wall)
    if(ENABLE_ASAN)
        target_compile_options(vyxc_core PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(vyxc_core PRIVATE -fsanitize=address)
    endif()
endif()

# Shared driver sources (used by both vyxc and vyx)
set(VYXC_DRIVER_SOURCES
    src/Compiler.cpp
    src/ProjectManager.cpp
    src/ImportResolver.cpp
    src/Linker.cpp
)

# --- vyxc: compiler CLI ---
add_executable(vyxc src/main.cpp ${VYXC_DRIVER_SOURCES})

# --- vyx: project manager CLI ---
add_executable(vyx src/vyx_main.cpp ${VYXC_DRIVER_SOURCES})
target_link_libraries(vyxc PRIVATE vyxc_core)
target_link_libraries(vyx PRIVATE vyxc_core)

# Optional AddressSanitizer support: cmake -DENABLE_ASAN=ON ..
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

# Self-host LLVM backend DLL (loaded by Vyx-min via std/dll.vyx).
# Toggle with -DBUILD_BOOTSTRAP=OFF to skip if you only want vyxc/vyx.
option(BUILD_BOOTSTRAP "Build the private Vyx compiler backend" ON)
if(BUILD_BOOTSTRAP)
    add_subdirectory(vyx_codegen)
endif()

# Shared compile/link options for all executables
foreach(target vyxc vyx)
    if(MSVC)
        target_compile_options(${target} PRIVATE /GR- /EHsc /W3)
        if(ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        endif()
    else()
        target_compile_options(${target} PRIVATE -fno-rtti -fno-exceptions -Wall)
        if(ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
    if(WIN32)
        target_link_options(${target} PRIVATE "LINKER:/STACK:268435456")
    else()
        target_link_options(${target} PRIVATE -Wl,-z,stacksize=16777216)
    endif()
endforeach()
