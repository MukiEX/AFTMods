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
        wine wine64 python3 msitools ca-certificates winbind git cmake ninja-build procps && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# /usr/bin/wine comes from the "wine" package (only a Recommends of wine64), so it's listed
# explicitly above. Fail early with a clear message if it's still missing.
ENV WINEDEBUG=-all
RUN WINE=$(command -v wine64 || command -v wine) || { echo "wine not found on PATH"; exit 1; } && \
    "$WINE" wineboot --init && \
    while pgrep wineserver > /dev/null; do sleep 1; done

RUN git clone https://github.com/mstorsjo/msvc-wine.git /tmp/msvc-wine && \
    git -C /tmp/msvc-wine checkout ${MSVC_WINE_COMMIT} && \
    cd /tmp/msvc-wine && \
    PYTHONUNBUFFERED=1 ./vsdownload.py --accept-license --architecture x64 --dest /opt/msvc && \
    ./install.sh /opt/msvc && \
    rm -rf /tmp/msvc-wine

ENV PATH=/opt/msvc/bin/x64:$PATH
COPY build.sh /usr/local/bin/build-aftmods
RUN chmod +x /usr/local/bin/build-aftmods
WORKDIR /src
ENTRYPOINT ["/usr/local/bin/build-aftmods"]
