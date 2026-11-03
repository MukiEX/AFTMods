cmake_minimum_required(VERSION 3.20)
project(LauncherFS C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI third_party/imgui)
set(IMGUI_SOURCES ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_widgets.cpp ${IMGUI}/imgui_tables.cpp)
set(UI_SOURCES src/app.cpp src/model.cpp src/ini.cpp src/soft_render.cpp)

if(WIN32)
    # The launcher plugin: build with MSVC (x64) or llvm-mingw / MinGW-w64 (x86_64).
    file(GLOB MINHOOK_SOURCES third_party/minhook/src/*.c third_party/minhook/src/hde/hde64.c)
    add_library(LauncherFS SHARED
        src/platform_win32.cpp src/sysdll.cpp ${UI_SOURCES} ${IMGUI_SOURCES}
        ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp
        ${MINHOOK_SOURCES}
        res/LauncherFS.rc)
    set_target_properties(LauncherFS PROPERTIES OUTPUT_NAME "Launcher" PREFIX "" SUFFIX ".dva")
    target_include_directories(LauncherFS PRIVATE src ${IMGUI} third_party/simpleini third_party/minhook/include)
    target_compile_definitions(LauncherFS PRIVATE UNICODE _UNICODE NOMINMAX IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    # d3d11 and d3dcompiler are loaded from System32 at run time (src/sysdll.cpp), not imported.
    target_link_libraries(LauncherFS PRIVATE user32 gdi32 dwmapi imm32 shell32)
    if(MSVC)
        target_compile_options(LauncherFS PRIVATE /utf-8 /W3)
        set_property(TARGET LauncherFS PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    else()
        target_link_options(LauncherFS PRIVATE -static -s -Wl,--exclude-all-symbols)
    endif()
else()
    # Linux preview: renders the UI to PNGs with simulated controller input.
    add_executable(launcher_preview tools/preview.cpp ${UI_SOURCES} ${IMGUI_SOURCES})
    target_include_directories(launcher_preview PRIVATE src ${IMGUI} third_party/simpleini tools)
endif()
