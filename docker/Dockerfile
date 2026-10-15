# Builds Launcher.dva with llvm-mingw. No Windows SDK or MSVC needed.
#   docker build -t launcherfs docker/
#   docker run --rm -v "$PWD:/src" launcherfs        -> build/win/Launcher.dva
FROM ubuntu:24.04
ARG LLVM_MINGW=llvm-mingw-20251104-ucrt-ubuntu-22.04-x86_64
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build curl xz-utils ca-certificates \
 && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://github.com/mstorsjo/llvm-mingw/releases/download/20251104/$LLVM_MINGW.tar.xz | tar -xJ -C /opt \
 && mv /opt/$LLVM_MINGW /opt/llvm-mingw
ENV LLVM_MINGW=/opt/llvm-mingw
WORKDIR /src
CMD ["sh", "-c", "rm -rf build/win && ./build-mingw.sh"]
