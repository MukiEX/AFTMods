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

# /usr/bin/wine and wineserver come from the "wine" package (only a Recommends of wine64),
# so it's listed explicitly above.
#
# MSBuild.exe is a .NET program, so it needs Wine Mono, which Ubuntu doesn't package.
# Install the exact version Wine 9.0 (Ubuntu 24.04) expects with msiexec, straight into the
# prefix. mscoree/mshtml are disabled during wineboot so it doesn't try to fetch add-ons itself.
# The "syswow64 ... c0000135" lines wineboot prints are expected: this image is 64-bit only
# (no wine32), and everything here (cl, link, MSBuild amd64) is 64-bit.
ARG WINE_MONO_VERSION=8.1.0
ENV WINEDEBUG=-all
RUN curl -fsSL -o /tmp/wine-mono.msi \
      https://github.com/wine-mono/wine-mono/releases/download/wine-mono-${WINE_MONO_VERSION}/wine-mono-${WINE_MONO_VERSION}-x86.msi && \
    WINEDLLOVERRIDES="mscoree,mshtml=" wine wineboot --init && wineserver -w && \
    { wine msiexec /i 'Z:\tmp\wine-mono.msi' /qn || echo "msiexec returned $? (checking the result below)"; } && wineserver -w && \
    rm /tmp/wine-mono.msi && \
    if [ -d "$HOME/.wine/drive_c/windows/mono" ]; then echo "Wine Mono ${WINE_MONO_VERSION} installed"; \
    else echo "Wine Mono didn't install into the prefix ($(wine --version))"; exit 1; fi

RUN git clone https://github.com/mstorsjo/msvc-wine.git /tmp/msvc-wine && \
    git -C /tmp/msvc-wine checkout ${MSVC_WINE_COMMIT} && \
    cd /tmp/msvc-wine && \
    PYTHONUNBUFFERED=1 ./vsdownload.py --accept-license --architecture x64 --dest /opt/msvc && \
    ./install.sh /opt/msvc && \
    rm -rf /tmp/msvc-wine

ENV PATH=/opt/msvc/bin/x64:$PATH
# Fail the image build now, not the first project build, if MSBuild can't start.
RUN msbuild -version -nologo && wineserver -w
COPY build.sh /usr/local/bin/build-aftmods
RUN chmod +x /usr/local/bin/build-aftmods
WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-aftmods"]
