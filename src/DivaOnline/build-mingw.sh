#!/bin/sh
# Cross-builds DivaOnline.dva with llvm-mingw (https://github.com/mstorsjo/llvm-mingw).
# Usage: LLVM_MINGW=/path/to/llvm-mingw ./build-mingw.sh   -> build/win/DivaOnline.dva
# (x86_64-w64-mingw32-gcc works too: point CMAKE_C_COMPILER/CMAKE_RC_COMPILER at it.)
set -e
: "${LLVM_MINGW:?set LLVM_MINGW to the llvm-mingw directory}"
here="$(cd "$(dirname "$0")" && pwd)"
# The toolchain file stays next to the build: CMake's cache refers back to it on every
# later build, so it can't be a temporary file.
mkdir -p build/win
tc="$(pwd)/build/llvm-mingw-x64.cmake"
cat > "$tc" <<TC
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   $LLVM_MINGW/bin/x86_64-w64-mingw32-clang)
set(CMAKE_RC_COMPILER  $LLVM_MINGW/bin/x86_64-w64-mingw32-windres)
TC
cmake -S "$here" -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$tc"
cmake --build build/win
