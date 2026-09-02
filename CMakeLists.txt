cmake_minimum_required(VERSION 3.24)
project(snowrunner_vr LANGUAGES CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SnowRunner is x64 — configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Dynamic CRT (/MD) to match the FetchContent'd OpenXR loader; the VC++ 2015+
# runtime (MSVCP140) is present on all supported Windows 11 targets.

include(FetchContent)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)

# Static loader so we don't have to ship openxr_loader.dll next to the game exe.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.36
)

# In-game settings overlay. Ships no CMakeLists.txt of its own -- its sources
# are added directly to dxgi_proxy below instead of linked as a target.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
)

FetchContent_MakeAvailable(minhook openxr imgui)

add_library(dxgi_proxy SHARED
    src/proxy/dllmain.cpp
    src/proxy/dxgi_proxy.cpp
    src/proxy/dxgi_stubs.asm
    src/proxy/dxgi.def
    src/common/log.cpp
    src/common/crash_log.cpp
    src/common/config.cpp
    src/hooks/pattern_scan.cpp
    src/hooks/swapchain_hook.cpp
    src/hooks/resolution_hook.cpp
    src/hooks/input_block.cpp
    src/hooks/camera_hook.cpp
    src/hooks/cbuffer_hook.cpp
    src/hooks/viewbuild_hook.cpp
    src/hooks/depth_probe.cpp
    src/hooks/ui_hook.cpp
    src/hooks/shader_cull.cpp
    src/hooks/camera_fov.cpp
    src/hooks/menu_hook.cpp
    src/render/dibr.cpp
    src/render/reproject.cpp
    src/render/mirror_mask.cpp
    src/render/rigid_mask.cpp
    src/render/ui_layer.cpp
    src/render/cursor_overlay.cpp
    src/render/smudge_layer.cpp
    src/render/winch_layer.cpp
    src/render/frame_dump.cpp
    src/xr/xr_mirror.cpp
    # Dear ImGui (no CMakeLists.txt upstream; compiled directly into this target)
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

set_target_properties(dxgi_proxy PROPERTIES OUTPUT_NAME "dxgi" PREFIX "")
# A LINKER MAP, because the crash handler reports module+offset (see
# common/crash_log.h) and this is what turns an offset from somebody else's log
# back into a function. Keep the map for any build that is given out.
target_link_options(dxgi_proxy PRIVATE /MAP)
target_include_directories(dxgi_proxy PRIVATE src ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(dxgi_proxy PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    XR_USE_GRAPHICS_API_D3D11 XR_USE_PLATFORM_WIN32
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD   # we poll XInput ourselves once/frame (menu_hook.cpp);
                                       # this avoids a second, redundant XInputGetState call.
)
# Deliberately NOT linking dxgi.lib — the real dxgi.dll is resolved at runtime
# from System32 so the proxy never binds to itself.
# dxguid supplies GUID_SysKeyboard/GUID_SysMouse, which input_block.cpp uses to
# name the game's DirectInput devices in its log. Still no dinput8 link — every
# DirectInput entry point is reached through a hook, never called directly.
target_link_libraries(dxgi_proxy PRIVATE minhook openxr_loader d3d11 d3dcompiler
                                        windowscodecs ole32 xinput psapi dxguid)

# --- tests -------------------------------------------------------------------
# Host-side only: this links nothing but a header, so it builds and runs on the
# dev machine with no game, no headset and no D3D device. That is the whole
# reason src/common/dibr_policy.h exists as a separate, dependency-free header
# -- see its file comment. Run with `ctest` from the build directory.
#
# GUARDED, because tests/ is not part of the published tree: a source-only
# checkout has to configure and build the DLL without it rather than fail here.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/dibr_policy_tests.cpp")
    enable_testing()
    add_executable(dibr_policy_tests tests/dibr_policy_tests.cpp)
    target_include_directories(dibr_policy_tests PRIVATE src)
    target_compile_definitions(dibr_policy_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME dibr_policy COMMAND dibr_policy_tests)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cursor_policy_tests.cpp")
    enable_testing()
    add_executable(cursor_policy_tests tests/cursor_policy_tests.cpp)
    target_include_directories(cursor_policy_tests PRIVATE src)
    add_test(NAME cursor_policy COMMAND cursor_policy_tests)
endif()

