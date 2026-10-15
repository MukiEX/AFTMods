#!/usr/bin/env bash
# Runs inside the container with the AFTMods checkout mounted at /src.
# Usage: build-aftmods [Release|Debug]   -> DVAs land in /src/out/
set -euo pipefail
CONFIG=${1:-Release}
SDK_TAG=vulkan-sdk-1.4.363.0
DEPS=/src/.deps
cd /src

# A persistent wineserver makes the many cl.exe/link.exe invocations much faster.
wineserver -p 2>/dev/null || true
$(command -v wine64 || command -v wine) wineboot >/dev/null 2>&1 || true

echo "== Fetching DivaVK dependencies ($SDK_TAG)"
mkdir -p "$DEPS"
[ -d "$DEPS/glslang" ] || git clone -q --depth 1 -b $SDK_TAG https://github.com/KhronosGroup/glslang.git "$DEPS/glslang"
[ -d src/DivaVK/external/Vulkan-Headers ] || \
    git clone -q --depth 1 -b $SDK_TAG https://github.com/KhronosGroup/Vulkan-Headers.git src/DivaVK/external/Vulkan-Headers

echo "== Building divavk_core + glslang (cl.exe via CMake/Ninja)"
CC=cl CXX=cl cmake -S src/DivaVK -B "$DEPS/divavk-build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_BUILD_TYPE=Release \
    -DDIVAVK_BUILD_TESTS=OFF \
    -DDIVAVK_VULKAN_HEADERS_DIR=/src/src/DivaVK/external/Vulkan-Headers \
    -DDIVAVK_GLSLANG_DIR="$DEPS/glslang"
cmake --build "$DEPS/divavk-build"   # stages libs into src/DivaVK/lib/Release

# The .vcxproj files ask for VS2019's v142 toolset; use whatever msvc-wine installed.
TOOLSET=$(ls /opt/msvc/MSBuild/Microsoft/VC/*/Platforms/x64/PlatformToolsets | grep -E '^v[0-9]+$' | sort -V | tail -1)
echo "== Building AFTMods.sln ($CONFIG, toolset $TOOLSET)"
msbuild AFTMods.sln /m /v:minimal /nologo \
    /p:Configuration=$CONFIG /p:Platform=x64 /p:PlatformToolset=$TOOLSET \
    /t:DivaGL /t:DivaVK

mkdir -p out
cp -v bin/DivaGL.dva bin/DivaVK.dva out/
echo "== Done: out/DivaGL.dva (DivaGL + SBS stereo), out/DivaVK.dva (DivaGL on Vulkan)"
