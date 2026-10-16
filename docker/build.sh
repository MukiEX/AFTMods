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

echo "== Building KKdLib, AFTModsShared, DivaGL and DivaVK (cl.exe via CMake/Ninja, no MSBuild)"
# MSBuild 18 can't run under Wine Mono (it loads Visual Studio telemetry assemblies that
# Build Tools doesn't ship), so CMakeLists.txt mirrors the solution's Release|x64 settings.
CC=cl CXX=cl cmake -S . -B "$DEPS/build-$CONFIG" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_BUILD_TYPE=$CONFIG \
    -DDIVAVK_VULKAN_HEADERS_DIR=/src/src/DivaVK/external/Vulkan-Headers \
    -DDIVAVK_GLSLANG_DIR="$DEPS/glslang"
# cl.exe under Wine occasionally fails to read a header when many compilers run at once
# (C1001 "internal compiler error" in p0io.cpp). It's transient, and Ninja resumes where it
# stopped, so retry a few times. Set JOBS to use fewer parallel compiles if it keeps happening.
JOBS=${JOBS:-$(nproc)}
for attempt in 1 2 3 4; do
    if cmake --build "$DEPS/build-$CONFIG" --target DivaGL DivaVK -- -j "$JOBS"; then
        break
    fi
    if [ "$attempt" = 4 ]; then
        echo "== Build failed 4 times. If it's the same file and error each time, it's a real error; otherwise try JOBS=4." >&2
        exit 1
    fi
    echo "== Build stopped (attempt $attempt); retrying from where it left off"
    JOBS=$(( JOBS > 2 ? JOBS / 2 : 1 ))
done

mkdir -p out
cp -v bin/DivaGL.dva bin/DivaVK.dva out/
echo "== Done: out/DivaGL.dva (DivaGL + SBS stereo), out/DivaVK.dva (DivaGL on Vulkan)"
