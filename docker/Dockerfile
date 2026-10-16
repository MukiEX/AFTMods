# Builds DivaGL.dva and DivaVK.dva with Microsoft's own MSVC (cl.exe) running under Wine
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
        wine wine64 python3 msitools ca-certificates winbind git cmake ninja-build procps && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# /usr/bin/wine and wineserver come from the "wine" package (only a Recommends of wine64),
# so it's listed explicitly above. The "syswow64 ... c0000135" lines wineboot prints are
# expected: this image is 64-bit only (no wine32), and cl/link/rc/ml64 are all 64-bit.
# No Wine Mono: the build uses CMake + Ninja driving cl.exe directly, not MSBuild.
ENV WINEDEBUG=-all
RUN WINEDLLOVERRIDES="mscoree,mshtml=" wine wineboot --init && wineserver -w

RUN git clone https://github.com/mstorsjo/msvc-wine.git /tmp/msvc-wine && \
    git -C /tmp/msvc-wine checkout ${MSVC_WINE_COMMIT} && \
    cd /tmp/msvc-wine && \
    PYTHONUNBUFFERED=1 ./vsdownload.py --accept-license --architecture x64 --dest /opt/msvc && \
    ./install.sh /opt/msvc && \
    rm -rf /tmp/msvc-wine

ENV PATH=/opt/msvc/bin/x64:$PATH
# Fail the image build now if the compiler can't run.
RUN cl 2>&1 | grep -q "Microsoft" && wineserver -w
COPY build.sh /usr/local/bin/build-aftmods
RUN chmod +x /usr/local/bin/build-aftmods
WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-aftmods"]
