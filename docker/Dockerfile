# Builds DivaGL.dva and DivaVK.dva with Microsoft's own MSVC + MSBuild running under Wine
# (https://github.com/mstorsjo/msvc-wine). Real cl.exe matters here: the plugins share
# std:: containers with the game, so they must use MSVC's STL layout (MinGW can't).
#
# Building this image downloads Visual Studio Build Tools from Microsoft and accepts the
# Visual Studio license on your behalf: https://go.microsoft.com/fwlink/?LinkId=2086102
# The resulting image contains Microsoft software and is not redistributable.

FROM ubuntu:24.04

ARG MSVC_WINE_COMMIT=514f8ea34842cd6d831804d0e9658d3a32870ae1

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        wine wine64 python3 msitools ca-certificates winbind git cmake ninja-build procps curl xz-utils && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# MSBuild.exe is a .NET program; under Wine it needs Wine Mono, which Ubuntu doesn't package.
# Wine 9.0 (Ubuntu 24.04) looks for exactly 8.1.0 in /usr/share/wine/mono and installs it
# into the prefix during wineboot below.
ARG WINE_MONO_VERSION=8.1.0
RUN mkdir -p /usr/share/wine/mono && \
    curl -fsSL https://github.com/wine-mono/wine-mono/releases/download/wine-mono-${WINE_MONO_VERSION}/wine-mono-${WINE_MONO_VERSION}-x86.tar.xz \
      | tar -xJ -C /usr/share/wine/mono

# /usr/bin/wine comes from the "wine" package (only a Recommends of wine64), so it's listed
# explicitly above. Fail early with a clear message if it's still missing.
ENV WINEDEBUG=-all
RUN WINE=$(command -v wine64 || command -v wine) || { echo "wine not found on PATH"; exit 1; } && \
    "$WINE" wineboot --init && \
    while pgrep wineserver > /dev/null; do sleep 1; done && \
    { [ -d "$HOME/.wine/drive_c/windows/mono" ] || { echo "Wine Mono didn't install; check WINE_MONO_VERSION matches '$(wine --version)'"; exit 1; }; }

RUN git clone https://github.com/mstorsjo/msvc-wine.git /tmp/msvc-wine && \
    git -C /tmp/msvc-wine checkout ${MSVC_WINE_COMMIT} && \
    cd /tmp/msvc-wine && \
    PYTHONUNBUFFERED=1 ./vsdownload.py --accept-license --architecture x64 --dest /opt/msvc && \
    ./install.sh /opt/msvc && \
    rm -rf /tmp/msvc-wine

ENV PATH=/opt/msvc/bin/x64:$PATH
# Fail the image build now, not the first project build, if MSBuild can't start.
RUN msbuild -version -nologo && while pgrep wineserver > /dev/null; do sleep 1; done
COPY build.sh /usr/local/bin/build-aftmods
RUN chmod +x /usr/local/bin/build-aftmods
WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-aftmods"]
