# Included by the new FM4 Vulkan build, reusing its patched SDK targets.
get_filename_component(FM2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(FM2_VULKAN_OUTPUT "${FM2_ROOT}/out/build/win-amd64-vulkan")
file(GLOB FM2_HOST_SOURCES CONFIGURE_DEPENDS "${FM2_ROOT}/src/*.cpp")
include("${FM2_ROOT}/generated/default/sources.cmake")
add_library(forza2_vulkan_guest OBJECT ${GENERATED_SOURCES})
target_link_libraries(forza2_vulkan_guest PRIVATE rex::runtime)
target_include_directories(forza2_vulkan_guest PRIVATE "${FM2_ROOT}" "${FM2_ROOT}/generated/default")
target_compile_options(forza2_vulkan_guest PRIVATE -fasync-exceptions -gline-tables-only)
target_precompile_headers(forza2_vulkan_guest PRIVATE "${FM2_ROOT}/generated/default/forza2_pch.h")
add_executable(forza2-vulkan WIN32 ${FM2_HOST_SOURCES})
target_include_directories(forza2-vulkan PRIVATE "${FM2_ROOT}" "${FM2_ROOT}/src" "${FM2_ROOT}/generated/default" "${FM4_ROOT}/work/vulkan-trial/shiftglue-sdk/thirdparty/imgui")
target_link_libraries(forza2-vulkan PRIVATE forza2_vulkan_guest rex::runtime dbghelp ws2_32 bcrypt)
target_compile_options(forza2-vulkan PRIVATE -fasync-exceptions -gline-tables-only)
set_target_properties(forza2-vulkan PROPERTIES OUTPUT_NAME forza2 RUNTIME_OUTPUT_DIRECTORY "${FM2_VULKAN_OUTPUT}")
rexglue_configure_target(forza2-vulkan GPU_PLUGINS fh1)
set(FM2_MINIZ "${FM2_ROOT}/out/build/win-amd64-release/_deps/miniz-src")
set(FM2_IMGUI "${FM2_ROOT}/out/build/win-amd64-release/_deps/imgui-src")
add_executable(Forza2LauncherVulkan WIN32
 "${FM2_ROOT}/launcher/src/main.cpp" "${FM2_ROOT}/launcher/src/settings.cpp"
 "${FM2_ROOT}/launcher/src/installer.cpp" "${FM2_ROOT}/launcher/src/sha256.cpp"
 "${FM2_ROOT}/launcher/src/forza_plus.cpp" "${FM2_ROOT}/launcher/src/online_status.cpp" "${FM2_MINIZ}/miniz.c"
 "${FM2_IMGUI}/imgui.cpp" "${FM2_IMGUI}/imgui_draw.cpp" "${FM2_IMGUI}/imgui_tables.cpp" "${FM2_IMGUI}/imgui_widgets.cpp"
 "${FM2_IMGUI}/backends/imgui_impl_sdl3.cpp" "${FM2_IMGUI}/backends/imgui_impl_sdlgpu3.cpp")
target_include_directories(Forza2LauncherVulkan BEFORE PRIVATE "${FM2_IMGUI}" "${FM2_IMGUI}/backends" "${FM2_MINIZ}" "${FM4_ROOT}/work/vulkan-trial/shiftglue-sdk/thirdparty/tomlplusplus/include")
target_link_libraries(Forza2LauncherVulkan PRIVATE SDL3::SDL3-static ws2_32)
# Online server (host:port) baked into release launchers; kept out of the sources.
set(FM2_ONLINE_SERVER "" CACHE STRING "Default FM2 relay server for the launcher")
if(FM2_ONLINE_SERVER)
  target_compile_definitions(Forza2LauncherVulkan PRIVATE FM2_ONLINE_SERVER="${FM2_ONLINE_SERVER}")
endif()
set_target_properties(Forza2LauncherVulkan PROPERTIES CXX_STANDARD 17 OUTPUT_NAME Forza2Launcher RUNTIME_OUTPUT_DIRECTORY "${FM2_VULKAN_OUTPUT}")
foreach(t forza2-vulkan Forza2LauncherVulkan)
 add_custom_command(TARGET ${t} POST_BUILD
 COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FM2_ROOT}/config/forza2.toml" "${FM2_VULKAN_OUTPUT}/forza2.toml"
 COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FM2_ROOT}/launcher/data/game_files.sha256" "${FM2_VULKAN_OUTPUT}/game_files.sha256")
endforeach()



# LAN transport tests use the actual port implementation and two loopback endpoints.
add_executable(fm2-lan-tests "${FM2_ROOT}/tests/lan_tests.cpp")
target_link_libraries(fm2-lan-tests PRIVATE rex::runtime ws2_32 bcrypt)
target_compile_options(fm2-lan-tests PRIVATE -UNDEBUG)
set_target_properties(fm2-lan-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FM2_VULKAN_OUTPUT}")
