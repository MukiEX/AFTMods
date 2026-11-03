cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
project(DivaVK LANGUAGES C CXX)

# DivaGL links the static CRT (/MT) in every configuration; match it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# External sources that aren't vendored (too large): point these at checkouts.
set(DIVAVK_VULKAN_HEADERS_DIR "" CACHE PATH "Path to a KhronosGroup/Vulkan-Headers checkout")
set(DIVAVK_GLSLANG_DIR "" CACHE PATH "Path to a KhronosGroup/glslang checkout")
option(DIVAVK_BUILD_TESTS "Build the headless test runner" ON)

if(NOT DIVAVK_VULKAN_HEADERS_DIR OR NOT EXISTS "${DIVAVK_VULKAN_HEADERS_DIR}/include/vulkan/vulkan.h")
    message(FATAL_ERROR "Set DIVAVK_VULKAN_HEADERS_DIR to a Vulkan-Headers checkout")
endif()
if(NOT DIVAVK_GLSLANG_DIR OR NOT EXISTS "${DIVAVK_GLSLANG_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set DIVAVK_GLSLANG_DIR to a glslang checkout")
endif()

# glslang (front end + SPIR-V generator only)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(${DIVAVK_GLSLANG_DIR} glslang EXCLUDE_FROM_ALL)

add_library(divavk_core STATIC
    core/src/buffer.cpp
    core/src/device.cpp
    core/src/divavk.cpp
    core/src/dump.cpp
    core/src/draw.cpp
    core/src/formats.cpp
    core/src/framebuffer.cpp
    core/src/gl_entry.cpp
    core/src/program.cpp
    core/src/query.cpp
    core/src/spirv_reflect.cpp
    core/src/texture.cpp
    core/src/xr.cpp
    external/volk/volk.c
)
target_include_directories(divavk_core
    PUBLIC core/include
    PRIVATE core/src external external/volk external/vma external/openxr/include ${DIVAVK_VULKAN_HEADERS_DIR}/include
            ${DIVAVK_GLSLANG_DIR})
target_compile_definitions(divavk_core PRIVATE VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(divavk_core PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(ANDROID)
    target_compile_definitions(divavk_core PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
endif()
find_package(Threads REQUIRED)
target_link_libraries(divavk_core PRIVATE glslang glslang-default-resource-limits ${CMAKE_DL_LIBS} Threads::Threads)
if(MSVC)
    target_compile_options(divavk_core PRIVATE /W3 /bigobj)
else()
    target_compile_options(divavk_core PRIVATE -Wall -Wno-unused-function -Wno-missing-field-initializers)
endif()

# Windows: stage the static libs where plugin/DivaVK.vcxproj looks for them.
if(WIN32)
    add_custom_target(divavk_stage_libs ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/lib/Release
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:divavk_core> $<TARGET_FILE:glslang>
                $<TARGET_FILE:glslang-default-resource-limits> ${CMAKE_CURRENT_SOURCE_DIR}/lib/Release
        DEPENDS divavk_core glslang glslang-default-resource-limits)
endif()

if(DIVAVK_BUILD_TESTS AND NOT WIN32)
    add_executable(divavk_tests tests/test_main.cpp)
    target_include_directories(divavk_tests PRIVATE core/include external ${DIVAVK_VULKAN_HEADERS_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../DivaLinux/movie)
    target_link_libraries(divavk_tests PRIVATE divavk_core)
    target_compile_definitions(divavk_tests PRIVATE
        DIVAVK_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../res/ft_shaders")

    # OpenXR output, against a mock runtime (no headset or real runtime needed).
    add_library(divavk_mock_xr SHARED tests/mock_xr_runtime.cpp)
    target_include_directories(divavk_mock_xr PRIVATE external/openxr/include ${DIVAVK_VULKAN_HEADERS_DIR}/include)
    target_compile_definitions(divavk_mock_xr PRIVATE VK_NO_PROTOTYPES)
    add_executable(divavk_xr_tests tests/test_xr.cpp)
    target_include_directories(divavk_xr_tests PRIVATE core/include external external/openxr/include ${DIVAVK_VULKAN_HEADERS_DIR}/include)
    target_link_libraries(divavk_xr_tests PRIVATE divavk_core ${CMAKE_DL_LIBS})
    target_compile_definitions(divavk_xr_tests PRIVATE
        DIVAVK_MOCK_XR="$<TARGET_FILE:divavk_mock_xr>")
    add_dependencies(divavk_xr_tests divavk_mock_xr)
endif()
