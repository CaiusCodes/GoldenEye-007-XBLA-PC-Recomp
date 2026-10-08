
# ge - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared builds must not carry the developer's folder layout (and with it the
# Windows user name): source paths recorded by __FILE__ in log and assert
# messages are rewritten to short project-relative names. Set before the SDK
# is added so its targets inherit it too.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    cmake_path(CONVERT "${CMAKE_SOURCE_DIR}" TO_CMAKE_PATH_LIST _ge_source_root NORMALIZE)
    add_compile_options("-ffile-prefix-map=${_ge_source_root}=goldeneye")
    if(REXSDK_DIR)
        cmake_path(CONVERT "${REXSDK_DIR}" TO_CMAKE_PATH_LIST _ge_sdk_root NORMALIZE)
        add_compile_options("-ffile-prefix-map=${_ge_sdk_root}=rexglue-sdk")
    endif()
endif()

# With REXSDK_DIR (the presets point it at the rexglue-sdk submodule, upstream
# v0.10.0), apply this project's SDK changes before the SDK is added; see
# cmake/RexgluePatch.cmake. Without it, an installed, already patched SDK is
# found through find_package (rexglue_DIR).
if(REXSDK_DIR)
    include(cmake/RexgluePatch.cmake)
endif()

include(generated/rexglue.cmake)

# Sources
set(GE_SOURCES
    src/main.cpp
    src/ge_hooks.cpp
    src/ge_menu.cpp
    src/ge_postfx.cpp
    src/ge_username.cpp
    src/ge_ce_patches.cpp
    src/ge_native_menu.cpp
    src/ge_portraits.cpp
)

# The release version: VERSION is the single source of truth (Make-Release.ps1
# and Build-Stubs.ps1 read the same file). It goes into the exe's version
# resource through a generated header GoldenEye.rc includes.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GE_VERSION)
string(STRIP "${GE_VERSION}" GE_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
string(REGEX MATCHALL "[0-9]+" _ge_version_parts "${GE_VERSION}")
list(APPEND _ge_version_parts 0 0 0 0)
list(SUBLIST _ge_version_parts 0 4 _ge_version_parts)
list(JOIN _ge_version_parts "," GE_VERSION_COMMA)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_include/ge_version_rc.h"
    CONTENT "// Generated from VERSION by CMakeLists.txt.\n#define GE_VERSION_STR \"${GE_VERSION}\"\n#define GE_VERSION_COMMA ${GE_VERSION_COMMA}\n"
    @ONLY)

if(WIN32)
    # Embed the application icon (GoldenEye.rc -> "MAINICON"). Use LLVM's resource
    # compiler since the toolchain is clang/lld (MSVC rc.exe may not be on PATH).
    if(NOT CMAKE_RC_COMPILER)
        set(CMAKE_RC_COMPILER "llvm-rc")
    endif()
    enable_language(RC)
    add_executable(ge WIN32 ${GE_SOURCES} GoldenEye.rc)
else()
    add_executable(ge ${GE_SOURCES})
endif()

# Output "GoldenEye 007.exe" (the in-Explorer/process name), independent of the
# internal target/app name "ge" (which still drives ge.toml / the user data dir).
set_target_properties(ge PROPERTIES OUTPUT_NAME "GoldenEye 007")

rexglue_setup_target(ge GPU_PLUGINS xenos)

# Same CPU baseline as the PD/Hexic ports (AVX2 class). -ffp-contract=off keeps
# float results identical to the guest (no fused multiply-add).
target_compile_options(ge PRIVATE -march=x86-64-v3 -ffp-contract=off)

# Character pictures are read with WIC (ge_portraits.cpp).
if(WIN32)
    target_link_libraries(ge PRIVATE windowscodecs ole32)
endif()

# ge_version_rc.h for GoldenEye.rc (see the VERSION block above).
target_include_directories(ge PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_include")
