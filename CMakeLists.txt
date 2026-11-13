
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
    src/ge_lan_adapters.cpp
    src/ge_lan_diagnostics.cpp
    src/ge_portraits.cpp
    src/ge_gpu_detect.cpp
    src/ge_native_probe.cpp
    src/ge_native_gpu.cpp
    src/ge_native_shaders.cpp
    src/ge_hit_fixes.cpp
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

# The player names the launcher picks one from at first start; the player card's
# arrows cycle through the same list (ge_native_menu.cpp includes it).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/player-names.txt" _ge_player_names)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/player-names.txt")
set(_ge_names_c "")
foreach(_name IN LISTS _ge_player_names)
    string(STRIP "${_name}" _name)
    string(LENGTH "${_name}" _len)
    if(_len GREATER 0 AND _len LESS 16 AND NOT _name MATCHES "[\"\\\\]")
        string(APPEND _ge_names_c "    \"${_name}\",\n")
    endif()
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_include/ge_player_names.h"
    CONTENT "// Generated from packaging/player-names.txt by CMakeLists.txt.\nstatic const char* const kGePlayerNames[] = {\n${_ge_names_c}};\n"
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

# x86-64-v2 (SSE4.2) so pre-2013 CPUs without AVX2 run it too (issue #22);
# v3 was ~7-12% less CPU time. -ffp-contract=off keeps
# float results identical to the guest (no fused multiply-add).
if(NOT REX_X86_MARCH)  # set by the SDK's CMakeLists; an installed SDK doesn't
  set(REX_X86_MARCH "x86-64-v2" CACHE STRING "x86-64 CPU level for the game code")
endif()
target_compile_options(ge PRIVATE -march=${REX_X86_MARCH} -ffp-contract=off)

# Character pictures are read with WIC (ge_portraits.cpp).
if(WIN32)
    target_link_libraries(ge PRIVATE windowscodecs ole32)
    # Check for Updates (src/ge_update.h): WinHTTP and CNG's SHA-256.
    target_link_libraries(ge PRIVATE winhttp bcrypt)
    # Online Settings > LAN Address (src/ge_lan_adapters.cpp): the network names.
    target_link_libraries(ge PRIVATE iphlpapi ws2_32)
    # [lan-check] log lines (src/ge_lan_diagnostics.cpp): firewall rules, network profiles.
    target_link_libraries(ge PRIVATE oleaut32)
endif()

# ge_version_rc.h for GoldenEye.rc (see the VERSION block above).
target_include_directories(ge PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_include")

# Native renderer (gpu_native, src/ge_native_gpu.cpp): Plume (MIT), D3D12 and Vulkan
# backends (gpu_native_api). Plume and XenosRecomp are submodules in thirdparty/,
# patched by cmake/RendererPatches.cmake (which sets GE_PLUME_DIR and GE_XENOSRECOMP_DIR).
if(WIN32)
    include(cmake/RendererPatches.cmake)
    add_library(ge_plume STATIC
        "${GE_PLUME_DIR}/plume_d3d12.cpp"
        "${GE_PLUME_DIR}/plume_vulkan.cpp"
        "${GE_PLUME_DIR}/contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
    target_include_directories(ge_plume PUBLIC
        "${GE_PLUME_DIR}"
        "${GE_PLUME_DIR}/contrib/D3D12MemoryAllocator/include")
    # Plume's Vulkan headers (volk, its Vulkan-Headers) only where Plume's Vulkan
    # types are used: the SDK's Vulkan code (ge_native_vk_out.cpp) keeps its own.
    set(_ge_plume_vk_includes
        "${GE_PLUME_DIR}/contrib/volk"
        "${GE_PLUME_DIR}/contrib/Vulkan-Headers/include"
        "${GE_PLUME_DIR}/contrib/VulkanMemoryAllocator/include")
    target_include_directories(ge_plume PRIVATE ${_ge_plume_vk_includes})
    set_source_files_properties(src/ge_native_gpu.cpp PROPERTIES INCLUDE_DIRECTORIES "${_ge_plume_vk_includes}")
    # The renderer and the probe read guest memory inside __try blocks that must
    # catch a bad page: without asynchronous exceptions clang only catches faults
    # raised in a called function, so a fault in the block itself (a texture
    # naming unmapped memory) ended the game.
    set_property(SOURCE src/ge_native_gpu.cpp src/ge_native_probe.cpp APPEND PROPERTY COMPILE_OPTIONS -fasync-exceptions)
    target_compile_definitions(ge_plume PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ge_plume PRIVATE -w)
    target_link_libraries(ge_plume PUBLIC d3d12 dxgi dxguid d3dcompiler)
    target_link_libraries(ge PRIVATE ge_plume)

    # Shader translation on the player's PC (src/ge_native_shaders.cpp): XenosRecomp's
    # recompiler (MIT) turns the game's Xenos shaders into HLSL; Microsoft's DXC
    # (dxcompiler.dll + dxil.dll, redistributable) compiles it. Nothing translated ships.
    set(_ge_xr "${GE_XENOSRECOMP_DIR}/XenosRecomp")
    set(_ge_dxc "${GE_XENOSRECOMP_DIR}/thirdparty/dxc-bin")
    add_library(ge_xenosrecomp STATIC "${_ge_xr}/shader_recompiler.cpp")
    target_include_directories(ge_xenosrecomp PUBLIC
        "${_ge_xr}"
        "${_ge_dxc}/inc"
        "${GE_XENOSRECOMP_DIR}/thirdparty/smol-v/source"
        "${GE_XENOSRECOMP_DIR}/thirdparty/zstd/lib"
        "${GE_XENOSRECOMP_DIR}/thirdparty/xxHash")
    target_precompile_headers(ge_xenosrecomp PRIVATE "${_ge_xr}/pch.h")
    target_compile_definitions(ge_xenosrecomp PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(ge_xenosrecomp PRIVATE -w -fms-extensions)
    target_link_libraries(ge_xenosrecomp PUBLIC fmt::fmt)
    target_link_libraries(ge PRIVATE ge_xenosrecomp)
    # The HLSL side of XenosRecomp's shared definitions, prepended to every
    # translated shader: embedded so the game needs no loose file for it.
    file(READ "${_ge_xr}/shader_common.h" _ge_shader_common)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated_include/ge_shader_common.inc"
        CONTENT "// Generated from XenosRecomp/shader_common.h by CMakeLists.txt.\nR\"GESC(@_ge_shader_common@)GESC\"\n"
        @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ge_xr}/shader_common.h")
    add_custom_command(TARGET ge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ge_dxc}/bin/x64/dxcompiler.dll" "${_ge_dxc}/bin/x64/dxil.dll" "$<TARGET_FILE_DIR:ge>")
    # The Vulkan presenter hand-off (src/ge_native_vk_out.cpp) uses the SDK's Vulkan
    # headers: built on its own, with the SDK's include paths only (the project root on
    # the game's path holds VERSION, which <version> would find on Windows).
    add_library(ge_vk_out OBJECT src/ge_native_vk_out.cpp)
    target_compile_features(ge_vk_out PRIVATE cxx_std_23)
    target_link_libraries(ge_vk_out PRIVATE rex::runtime)  # the SDK's headers and definitions
    # renderdoc_app.h (included by the SDK's Vulkan headers): the installed SDK's
    # include/renderdoc, or the SDK source tree's thirdparty/renderdoc.
    # The game uses it too: a test pipe word captures one frame when RenderDoc is attached.
    if(rexglue_DIR AND EXISTS "${rexglue_DIR}/../../../include/renderdoc/renderdoc_app.h")
        target_include_directories(ge_vk_out SYSTEM PRIVATE "${rexglue_DIR}/../../../include/renderdoc")
        target_include_directories(ge SYSTEM PRIVATE "${rexglue_DIR}/../../../include/renderdoc")
    elseif(REXSDK_DIR AND EXISTS "${REXSDK_DIR}/thirdparty/renderdoc/renderdoc_app.h")
        target_include_directories(ge_vk_out SYSTEM PRIVATE "${REXSDK_DIR}/thirdparty/renderdoc")
        target_include_directories(ge SYSTEM PRIVATE "${REXSDK_DIR}/thirdparty/renderdoc")
    endif()
    target_link_libraries(ge PRIVATE ge_vk_out)
endif()
