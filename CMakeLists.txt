# forza2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(forza2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the same patched source SDK as the FM4 Vulkan build; the release SDK
# is D3D12-only and cannot provide the ABI-2 fh1 plugin.
if(NOT REXSDK_DIR)
    get_filename_component(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../fm4-recomp/work/vulkan-trial/shiftglue-sdk" ABSOLUTE)
endif()
if(NOT EXISTS "${REXSDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set REXSDK_DIR to the patched Vulkan SDK source used by FM4")
endif()
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
include(generated/rexglue.cmake)

# The build runs `rexglue codegen` on assets/default.xex first. The game is not
# part of the repository, so without it codegen is swapped for a no-op and the
# committed generated/ sources are compiled as they are. Drop your default.xex
# into assets/ (the launcher's import puts it in game/) to regenerate.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex")
    if(WIN32)
        set(_forza2_codegen_stub "${CMAKE_CURRENT_SOURCE_DIR}/cmake/codegen_skipped.cmd")
    else()
        set(_forza2_codegen_stub "${CMAKE_CURRENT_SOURCE_DIR}/cmake/codegen_skipped.sh")
    endif()
    foreach(_config "" _DEBUG _RELEASE _RELWITHDEBINFO)
        set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION${_config} "${_forza2_codegen_stub}")
    endforeach()
    message(STATUS "forza2: no assets/default.xex - building the committed generated/ code")
endif()

# Sources
set(FORZA2_SOURCES
    src/main.cpp
    src/profile.cpp
    src/lan.cpp
    src/crash_report.cpp
    src/stream_guard.cpp
    src/diag_hooks.cpp
    src/log_filter.cpp
    src/frame_stats.cpp
    src/shader_warmup.cpp
    src/scripted_input.cpp
    src/dlc.cpp
)

if(WIN32)
    add_executable(forza2 WIN32 ${FORZA2_SOURCES})
else()
    add_executable(forza2 ${FORZA2_SOURCES})
endif()

rexglue_setup_target(forza2 GPU_PLUGINS fh1)
target_include_directories(forza2 PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
if(WIN32)
    target_link_libraries(forza2 PRIVATE dbghelp ws2_32 bcrypt)
endif()

# Launcher: profiles, settings, ISO import (launcher/).
add_subdirectory(launcher)

# Port settings ship next to the exe (the runtime reads <exe dir>/forza2.toml).
add_custom_command(TARGET forza2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/config/forza2.toml $<TARGET_FILE_DIR:forza2>/forza2.toml)
