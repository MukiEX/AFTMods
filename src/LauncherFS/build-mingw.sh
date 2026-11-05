#!/bin/sh
# Cross-builds Launcher.dva with llvm-mingw (https://github.com/mstorsjo/llvm-mingw).
# Usage: LLVM_MINGW=/path/to/llvm-mingw ./build-mingw.sh   -> build/win/Launcher.dva
set -e
: "${LLVM_MINGW:?set LLVM_MINGW to the llvm-mingw directory}"
cat > /tmp/llvm-mingw-x64.cmake <<TC
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   $LLVM_MINGW/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER $LLVM_MINGW/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  $LLVM_MINGW/bin/x86_64-w64-mingw32-windres)
TC
cmake -S "$(dirname "$0")" -B build/win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/tmp/llvm-mingw-x64.cmake
cmake --build build/win
