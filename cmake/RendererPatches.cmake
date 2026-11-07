# The native renderer's libraries, as submodules pinned to upstream commits
# plus this project's small changes in patches/ (applied at configure time the
# same way as patches/rexglue.patch, see RexgluePatch.cmake):
#   thirdparty/plume        Plume (MIT), the D3D12/Vulkan rendering layer;
#                           patches/plume.patch
#   thirdparty/XenosRecomp  XenosRecomp (MIT), the Xenos shader recompiler, and
#                           its bundled DirectX Shader Compiler binaries;
#                           patches/xenosrecomp.patch
#
# To return a submodule to pristine upstream:
#   git -C thirdparty/<name> checkout -- .  &&  git -C thirdparty/<name> clean -fd

find_package(Git REQUIRED)

function(_ge_sub_git dir out_result out_output)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${dir}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out_result} ${_result} PARENT_SCOPE)
    set(${out_output} "${_output}" PARENT_SCOPE)
endfunction()

# Checks that `dir` is the submodule at `commit` and applies `patch` once
# (a copy of the applied patch in the submodule's git directory records it;
# a changed patch replaces the old one).
function(ge_apply_submodule_patch name dir commit patch probe)
    if(NOT EXISTS "${dir}/${probe}")
        message(FATAL_ERROR
            "${name} not found at '${dir}'. Fetch the submodules with:\n"
            "  git -c core.longpaths=true submodule update --init --recursive")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    _ge_sub_git("${dir}" _r _head rev-parse HEAD)
    if(NOT _r EQUAL 0 OR NOT _head STREQUAL commit)
        message(FATAL_ERROR
            "${dir} is at '${_head}', but GoldenEye is built against ${name} ${commit}. Run:\n"
            "  git -c core.longpaths=true submodule update --init --recursive")
    endif()
    _ge_sub_git("${dir}" _r _git_dir rev-parse --absolute-git-dir)
    set(_applied "${_git_dir}/goldeneye-applied.patch")
    file(SHA256 "${patch}" _want)
    if(EXISTS "${_applied}")
        file(SHA256 "${_applied}" _have)
        _ge_sub_git("${dir}" _r _out apply --check --reverse "${_applied}")
        if(_r EQUAL 0 AND _have STREQUAL _want)
            return()  # applied and unchanged
        endif()
        if(_r EQUAL 0)
            _ge_sub_git("${dir}" _r _out apply --reverse "${_applied}")
            message(STATUS "Removed the previous GoldenEye ${name} patch")
        endif()
        file(REMOVE "${_applied}")
    endif()
    _ge_sub_git("${dir}" _r _out apply --check --reverse "${patch}")
    if(NOT _r EQUAL 0)
        _ge_sub_git("${dir}" _r _out apply --whitespace=nowarn "${patch}")
        if(NOT _r EQUAL 0)
            message(FATAL_ERROR
                "The GoldenEye ${name} patch does not apply to ${dir}:\n${_out}\n"
                "Reset it to upstream and configure again:\n"
                "  git -C ${dir} checkout -- .  &&  git -C ${dir} clean -fd")
        endif()
        message(STATUS "Applied the GoldenEye ${name} patch to ${dir}")
    endif()
    file(COPY_FILE "${patch}" "${_applied}")
endfunction()

set(GE_PLUME_DIR "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/plume")
set(GE_XENOSRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/XenosRecomp")
cmake_path(NORMAL_PATH GE_PLUME_DIR)
cmake_path(NORMAL_PATH GE_XENOSRECOMP_DIR)
ge_apply_submodule_patch(Plume "${GE_PLUME_DIR}"
    e0c8871b930dc1a6544bf457f2348c456b9d7e13
    "${CMAKE_CURRENT_LIST_DIR}/../patches/plume.patch"
    contrib/D3D12MemoryAllocator/include/D3D12MemAlloc.h)
ge_apply_submodule_patch(XenosRecomp "${GE_XENOSRECOMP_DIR}"
    990d03b28a27b50277ee5d8d942e1c5f873869d1
    "${CMAKE_CURRENT_LIST_DIR}/../patches/xenosrecomp.patch"
    thirdparty/dxc-bin/bin/x64/dxcompiler.dll)
