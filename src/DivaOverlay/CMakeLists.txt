# DivaOverlay is built into DivaLinux.dva (the top-level CMakeLists.txt adds these sources).
# This file builds only the Linux preview tool, which draws the menu to PNGs:
#   cmake -S src/DivaOverlay -B build-ovpreview && cmake --build build-ovpreview
#   build-ovpreview/ov_preview out/
cmake_minimum_required(VERSION 3.20)
project(DivaOverlayPreview CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LFS ${CMAKE_CURRENT_SOURCE_DIR}/../LauncherFS)
set(IMGUI ${LFS}/third_party/imgui)
add_executable(ov_preview
    tools/preview.cpp ov_ui.cpp ov_model.cpp env_file.cpp res/ov_fonts.cpp
    ${LFS}/src/model.cpp ${LFS}/src/ini.cpp ${LFS}/src/soft_render.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_widgets.cpp ${IMGUI}/imgui_tables.cpp)
target_include_directories(ov_preview PRIVATE . ${LFS}/src ${IMGUI} ${LFS}/third_party/simpleini ${LFS}/tools)
