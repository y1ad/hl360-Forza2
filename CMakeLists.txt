# forza2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(forza2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android (arm64): the game is libmain.so, which SDL's activity loads and
# whose SDL_main it calls (android/, tools/package_android.py). The tree is
# cross compiled, so no generator can run: the committed generated/ code is
# compiled as it is.
if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(FORZA2_ANDROID_MARCH "armv8.2-a" CACHE STRING
        "-march of every Android target, the SDK and its libraries included")
    add_compile_options(-march=${FORZA2_ANDROID_MARCH})
    set(REXGLUE_ARM64_MARCH "${FORZA2_ANDROID_MARCH}")
    set(REXGLUE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rexglue-artifacts" CACHE PATH
        "ReXGlue artifacts of this build" FORCE)
    set(FORZA2_FROZEN_CODEGEN ON)
endif()

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

# The game is not part of the repository. Without assets/default.xex the code
# generator is not run at all and the committed generated/ sources are compiled
# as they are. Drop your default.xex into assets/ (the launcher's import puts
# it in game/) to regenerate them.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex")
    set(FORZA2_FROZEN_CODEGEN ON)
endif()
if(FORZA2_FROZEN_CODEGEN)
    message(STATUS "forza2: building the committed generated/ code (no code generator run)")
endif()
include(generated/rexglue.cmake)

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

if(ANDROID)
    # Automation only; its wrapper has no way to reach the runtime's own
    # XamInputGetState outside Windows and would leave the pad dead.
    list(REMOVE_ITEM FORZA2_SOURCES src/scripted_input.cpp)
    add_library(forza2 SHARED ${FORZA2_SOURCES})
    set_target_properties(forza2 PROPERTIES OUTPUT_NAME main)
    # Generated functions are weak, default-visibility symbols: bind calls to
    # their definitions in this library, where the port's replacements (src/)
    # are strong definitions and win.
    target_link_options(forza2 PRIVATE -Wl,-Bsymbolic-functions)
elseif(WIN32)
    add_executable(forza2 WIN32 ${FORZA2_SOURCES})
else()
    add_executable(forza2 ${FORZA2_SOURCES})
endif()

rexglue_setup_target(forza2 GPU_PLUGINS fh1)
target_include_directories(forza2 PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
if(ANDROID)
    # rex/version.h is configured into the SDK sub-build and is needed only
    # by the rex_app.cpp the SDK helper injects into this target.
    set_property(SOURCE "${REXGLUE_SHARE_DIR}/rex_app.cpp" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/rexglue-sdk/include")
    find_package(Threads REQUIRED)
    target_link_libraries(forza2 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(TARGET rex::gpu-fh1)
        target_link_libraries(forza2 PRIVATE rex::gpu-fh1)
    elseif(TARGET rexgpu-fh1)
        target_link_libraries(forza2 PRIVATE rexgpu-fh1)
    endif()
elseif(WIN32)
    target_link_libraries(forza2 PRIVATE dbghelp ws2_32 bcrypt)
endif()

# Launcher: profiles, settings, ISO import (launcher/). Android has none; the
# activity provides the settings files and the game folder.
if(NOT ANDROID)
    add_subdirectory(launcher)

    # Port settings ship next to the exe (the runtime reads <exe dir>/forza2.toml).
    add_custom_command(TARGET forza2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/forza2.toml $<TARGET_FILE_DIR:forza2>/forza2.toml)
endif()
