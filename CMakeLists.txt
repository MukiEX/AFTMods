# CMake build for DivaGL.dva and DivaLinux.dva, equivalent to the Release|x64 configuration of
# AFTMods.sln. It exists for building with MSVC's cl.exe without MSBuild (for example under
# Wine with msvc-wine, see docker/). On Windows, AFTMods.sln remains the normal way to build.
#
# Must be built with MSVC (cl.exe): the plugins share STL containers with the game.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DDIVAVK_VULKAN_HEADERS_DIR=... -DDIVAVK_GLSLANG_DIR=... -DDIVALINUX_FFMPEG_INCLUDE_DIR=...
#   cmake --build build --target DivaLinux   -> bin/DivaLinux.dva
#
# DivaLinux.dva is the plugin to ship, on Windows and under Wine/Proton: DivaGL's renderer on
# Vulkan (DivaVK) plus DivaMovie's FFmpeg movie player. The DivaGL target (plain OpenGL) is
# retired: it still builds, since DivaLinux compiles the same sources, but isn't released.

cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(AFTMods LANGUAGES C CXX RC ASM_MASM)

if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "AFTMods plugins must be built with MSVC's cl.exe (they share MSVC STL containers with the game).")
endif()

option(AFTMODS_BUILD_DIVALINUX "Build DivaLinux.dva (needs DIVAVK_VULKAN_HEADERS_DIR, DIVAVK_GLSLANG_DIR and DIVALINUX_FFMPEG_INCLUDE_DIR)" ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")                  # /MT, as in the .vcxproj files
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")

# The .vcxproj Release settings: /O2 /Ob1 /Oi /Ot /Oy- /W3 /permissive- /Zi, no NDEBUG.
set(CMAKE_C_FLAGS_RELEASE "/O2 /Ob1 /Oi /Ot /Oy-")
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob1 /Oi /Ot /Oy-")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO")

set(AFT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(AFT_BIN ${AFT_ROOT}/bin)

function(aft_settings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/TP /W3 /wd26812 /permissive- /EHsc /GR>)
    target_compile_definitions(${target} PRIVATE WIN32 _UNICODE UNICODE)
    target_include_directories(${target} PRIVATE ${AFT_ROOT}/extern/src)
endfunction()

function(aft_plugin target)
    aft_settings(${target})
    target_compile_definitions(${target} PRIVATE _WINDLL)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${target} PREFIX "" SUFFIX ".dva"
        RUNTIME_OUTPUT_DIRECTORY ${AFT_BIN}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${AFT_BIN}
        LIBRARY_OUTPUT_DIRECTORY ${AFT_BIN}
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implib)
    target_link_options(${target} PRIVATE
        /SUBSYSTEM:WINDOWS /MACHINE:X64 /DYNAMICBASE:NO /ignore:4098 /ignore:4099 /ignore:4286)
    target_link_directories(${target} PRIVATE ${AFT_ROOT}/extern/lib)
    target_link_libraries(${target} PRIVATE AFTModsShared KKdLib
        opengl32 detours syelog libdeflatestatic)
endfunction()

# ---------------------------------------------------------------- KKdLib
set(KKD ${AFT_ROOT}/src/KKdLib)
add_library(KKdLib STATIC
    ${KKD}/database/sprite.cpp ${KKD}/default.cpp ${KKD}/deflate.cpp ${KKD}/divafile.cpp
    ${KKD}/f2/enrs.cpp ${KKD}/f2/header.cpp ${KKD}/f2/pof.cpp ${KKD}/f2/struct.cpp
    ${KKD}/farc.cpp ${KKD}/half_t.cpp ${KKD}/hash.cpp ${KKD}/image.cpp ${KKD}/interpolation.cpp
    ${KKD}/io/file_stream.cpp ${KKD}/io/memory_stream.cpp ${KKD}/io/path.cpp ${KKD}/io/stream.cpp
    ${KKD}/key_val.cpp ${KKD}/kf.cpp ${KKD}/mat.cpp
    ${KKD}/prj/rijndael.cpp ${KKD}/prj/stack_allocator.cpp ${KKD}/prj/time.cpp
    ${KKD}/quat.cpp ${KKD}/sort.cpp ${KKD}/spr.cpp ${KKD}/str_utils.cpp ${KKD}/time.cpp
    ${KKD}/txp.cpp ${KKD}/vec.cpp)
aft_settings(KKdLib)

# ---------------------------------------------------------------- AFTModsShared
set(SH ${AFT_ROOT}/src/AFTModsShared)
add_library(AFTModsShared STATIC
    ${SH}/auth_2d.cpp ${SH}/auth_3d.cpp ${SH}/bone_database.cpp ${SH}/camera.cpp
    ${SH}/canonical_properties.cpp ${SH}/color.cpp ${SH}/dw.cpp ${SH}/file_handler.cpp
    ${SH}/frame_rate_control.cpp ${SH}/light_param/fog.cpp ${SH}/light_param/light.cpp
    ${SH}/mdl/disp_manager.cpp ${SH}/object.cpp ${SH}/ogl_buffer_object.cpp ${SH}/print_work.cpp
    ${SH}/resolution_mode.cpp ${SH}/rob/rob.cpp ${SH}/sprite.cpp ${SH}/task.cpp ${SH}/texture.cpp
    ${SH}/uniform.cpp)
aft_settings(AFTModsShared)

# ---------------------------------------------------------------- DivaGL
set(GL ${AFT_ROOT}/src/DivaGL)
set(DIVAGL_SOURCES
    camera_render.cpp GL/array_buffer.cpp GL/element_array_buffer.cpp GL/shader_storage_buffer.cpp
    GL/uniform_buffer.cpp
    Glitter/animation.cpp Glitter/counter.cpp Glitter/curve.cpp Glitter/effect.cpp
    Glitter/effect_group.cpp Glitter/effect_inst.cpp Glitter/emitter.cpp Glitter/emitter_inst.cpp
    Glitter/file_reader.cpp Glitter/glitter.cpp Glitter/item_base.cpp Glitter/locus_history.cpp
    Glitter/node.cpp Glitter/particle.cpp Glitter/particle_inst.cpp Glitter/particle_manager.cpp
    Glitter/random.cpp Glitter/render_element.cpp Glitter/render_group.cpp Glitter/render_scene.cpp
    Glitter/scene.cpp
    data_test/rob_osage_test.cpp graphics/color_change_dw.cpp mdl/disp_manager.cpp
    mdl/draw_object.cpp movie_play_lib/movie_play_lib.cpp ogl_buffer_object.cpp
    pv_game/dsc.cpp pv_game/firstread.cpp pv_game/pvpp.cpp pv_game/pvsr.cpp pv_game/pv_game.cpp
    reflect_full.cpp renderer/dof.cpp renderer/fbo.cpp renderer/transparency.cpp rob/rob.cpp
    clear_color.cpp auth_3d.cpp camera.cpp dllmain.cpp effect.cpp gl_state.cpp gl_rend_state.cpp
    hook.cpp light_param.cpp object.cpp print.cpp render.cpp render_context.cpp
    render_manager.cpp render_texture.cpp screen_shot.cpp shader.cpp shader_ft.cpp shadow.cpp
    shared.cpp sprite.cpp sss.cpp stage.cpp stage_param.cpp static_var.cpp stereo.cpp
    task_movie.cpp texture.cpp wrap.cpp
    DivaGL.rc common.asm)
list(TRANSFORM DIVAGL_SOURCES PREPEND ${GL}/)
# DivaGL.rc embeds res/ft_shaders.farc (the shaders DivaGL loads). Ninja's resource-compiler
# dependency scan only follows #include, so without this an edited farc isn't rebuilt into
# the DVA and the old shaders keep running.
set_source_files_properties(${GL}/DivaGL.rc PROPERTIES OBJECT_DEPENDS ${AFT_ROOT}/res/ft_shaders.farc)

add_library(DivaGL SHARED ${DIVAGL_SOURCES})
aft_plugin(DivaGL)

# ---------------------------------------------------------------- DivaLinux
# DivaGL compiled with DIVAVK (Vulkan backend + WGL bridge) and the FFmpeg movie player.
if(AFTMODS_BUILD_DIVALINUX)
    if(NOT EXISTS "${DIVALINUX_FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
        message(FATAL_ERROR "DIVALINUX_FFMPEG_INCLUDE_DIR must point at FFmpeg's headers (see docker/build.sh for the version).")
    endif()
    set(DIVAVK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(src/DivaVK)   # divavk_core + glslang

    # DivaOverlay: the in-game settings menu (src/DivaOverlay), with LauncherFS's settings model,
    # INI access and CPU renderer, and Dear ImGui.
    set(LFS ${AFT_ROOT}/src/LauncherFS)
    set(OV ${AFT_ROOT}/src/DivaOverlay)
    add_library(DivaOverlay STATIC
        ${OV}/ov_host.cpp ${OV}/ov_ui.cpp ${OV}/ov_model.cpp ${OV}/env_file.cpp ${OV}/res/ov_fonts.cpp
        ${LFS}/src/model.cpp ${LFS}/src/ini.cpp ${LFS}/src/soft_render.cpp
        ${LFS}/third_party/imgui/imgui.cpp ${LFS}/third_party/imgui/imgui_draw.cpp
        ${LFS}/third_party/imgui/imgui_widgets.cpp ${LFS}/third_party/imgui/imgui_tables.cpp)
    aft_settings(DivaOverlay)
    target_compile_definitions(DivaOverlay PRIVATE NOMINMAX
        IMGUI_DISABLE_WIN32_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
    target_compile_options(DivaOverlay PRIVATE /utf-8)
    target_include_directories(DivaOverlay PRIVATE ${OV} ${LFS}/src ${LFS}/third_party/imgui ${LFS}/third_party/simpleini
        ${AFT_ROOT}/src/DivaVK/core/include)

    set(MV ${AFT_ROOT}/src/DivaLinux/movie)
    set(DIVALINUX_MOVIE_SOURCES ${MV}/movie.cpp ${MV}/ff_loader.cpp ${MV}/ff_movie.cpp ${MV}/ff_player.cpp
        ${MV}/movie_gl.cpp ${MV}/movie_yuv.cpp)
    add_library(DivaLinux SHARED
        ${AFT_ROOT}/src/DivaLinux/gl_bridge/vk_bridge.cpp
        ${DIVALINUX_MOVIE_SOURCES}
        ${AFT_ROOT}/src/DivaLinux/debug/io_trace.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/vfs.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/paths.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/wildcard.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/zip_index.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/archives.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/dir_cache.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/vhandles.cpp
        ${AFT_ROOT}/src/DivaLinux/vfs/hooks.cpp
        ${AFT_ROOT}/src/DivaLinux/overlay/compositor.cpp
        ${AFT_ROOT}/src/DivaLinux/overlay/cursor.cpp
        ${AFT_ROOT}/src/DivaLinux/overlay/launcher_api.cpp
        ${AFT_ROOT}/src/DivaLinux/overlay/layout.cpp
        ${AFT_ROOT}/src/DivaLinux/overlay/overlay_gl.cpp
        ${AFT_ROOT}/src/DivaLinux/game/stages.cpp
        ${AFT_ROOT}/src/DivaLinux/common/iat_patch.cpp
        ${AFT_ROOT}/src/DivaLinux/common/util.cpp
        ${AFT_ROOT}/src/DivaLinux/common/config.cpp
        ${AFT_ROOT}/src/DivaLinux/main.cpp
        ${DIVAGL_SOURCES})
    aft_plugin(DivaLinux)
    target_compile_definitions(DivaLinux PRIVATE DIVAVK DIVALINUX)
    target_include_directories(DivaLinux PRIVATE
        ${AFT_ROOT}/src/DivaVK/core/include
        ${AFT_ROOT}/src/DivaVK/external/volk
        ${DIVAVK_VULKAN_HEADERS_DIR}/include)
    # FFmpeg's headers only for the movie player (it loads the DLLs at run time, no import libs).
    set_source_files_properties(${DIVALINUX_MOVIE_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${DIVALINUX_FFMPEG_INCLUDE_DIR}")
    target_link_libraries(DivaLinux PRIVATE DivaOverlay divavk_core glslang glslang-default-resource-limits winmm)
endif()
