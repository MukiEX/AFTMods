#!/usr/bin/env bash
# Packages a release of this fork from the DVAs in out/ (made by docker/build.sh).
#
#   ./release.sh v0.1.0-alpha.1                 -> release/v0.1.0-alpha.1/*.zip, SHA256SUMS, NOTES.md
#   ./release.sh v0.1.0-alpha.1 --build         builds first (the Docker command from docker/README.md)
#   ./release.sh v0.1.0-alpha.1 --publish       also tags the commit, pushes the tag to the fork
#                                               and creates a GitHub pre-release with the files
#
# FFMPEG_DIR=<folder with avutil-61.dll ...> also ships FFmpeg's DLLs in the package: unzip the
# aftmods-ffmpeg-*.zip from a github.com/MukiEX/FFmpeg-Builds release (LGPL; its licence and
# BUILDINFO.txt go along). GPL or nonfree builds are refused.
# FORK_REMOTE overrides where --publish pushes (default git@github.com:MukiEX/AFTMods.git).
#
# One package for Windows and Wine/Proton alike, laid out like the game folder (unzip into the
# folder with diva.exe):
#   AFTMods-<version>.zip  plugins/DivaLinux.dva, plugins/Launcher.dva, tools/divalinux_pack.py,
#                          docs and third-party licences
# (The plain-OpenGL DivaGL.dva is retired and not released.)
set -euo pipefail
cd "$(dirname "$0")"

VERSION=""
BUILD=0
PUBLISH=0
for arg in "$@"; do
    case "$arg" in
        --build) BUILD=1 ;;
        --publish) PUBLISH=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        -*) echo "unknown option $arg" >&2; exit 2 ;;
        *) VERSION="$arg" ;;
    esac
done
if [ -z "$VERSION" ]; then
    echo "usage: ./release.sh <version, e.g. v0.1.0-alpha.1> [--build] [--publish]" >&2
    exit 2
fi
case "$VERSION" in
    DivaGL-*) echo "DivaGL-* tags are upstream's (korenkonder); pick another name, e.g. v0.1.0-alpha.1" >&2; exit 2 ;;
esac
FORK_REMOTE=${FORK_REMOTE:-git@github.com:MukiEX/AFTMods.git}
case "$FORK_REMOTE" in
    *korenkonder*) echo "FORK_REMOTE points at korenkonder's upstream; refusing to publish there" >&2; exit 2 ;;
esac

COMMIT=$(git rev-parse --short HEAD)
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "!! Uncommitted changes: the release will say $COMMIT but the DVAs may include them." >&2
    [ "$PUBLISH" = 1 ] && { echo "   Commit first when publishing." >&2; exit 1; }
fi

if [ "$BUILD" = 1 ]; then
    echo "== Building"
    docker run --rm --ulimit nofile=65536:65536 -v "$PWD:/src:z" --entrypoint /src/docker/build.sh aftmods-msvc
fi

for f in DivaLinux.dva Launcher.dva; do
    [ -f "out/$f" ] || { echo "out/$f is missing; build first (or use --build)" >&2; exit 1; }
done
# DVAs older than the last commit were probably built from older source
LAST_COMMIT_TIME=$(git log -1 --format=%ct)
for f in out/DivaLinux.dva out/Launcher.dva; do
    if [ "$(stat -c %Y "$f")" -lt "$LAST_COMMIT_TIME" ]; then
        echo "!! $f is older than the last commit ($COMMIT); rebuild (--build) unless that's intended." >&2
    fi
done

OUT="release/$VERSION"
rm -rf "$OUT"
mkdir -p "$OUT"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# ---- shared docs and licences
common() {
    local dir="$1"
    mkdir -p "$dir/docs" "$dir/licenses"
    cp README.md "$dir/docs/AFTMods_README.md"
    cp RELEASE_NOTES.md "$dir/docs/RELEASE_NOTES.md"
    cp src/LauncherFS/README.md "$dir/docs/Launcher_README.md"
    cp thirdparty.txt "$dir/licenses/thirdparty.txt"
    cp src/LauncherFS/third_party/imgui/LICENSE.txt "$dir/licenses/imgui.txt"
    cp src/LauncherFS/third_party/minhook/LICENSE.txt "$dir/licenses/minhook.txt"
    cp src/LauncherFS/res/OFL.txt "$dir/licenses/M_PLUS_Rounded_1c_fonts_OFL.txt"
}

# ---- the package (Windows, Wine, Proton, WinlatorXR)
LIN="$STAGE/AFTMods-$VERSION"
mkdir -p "$LIN/plugins" "$LIN/tools"
cp out/DivaLinux.dva out/Launcher.dva "$LIN/plugins/"
cp tools/divalinux_pack.py "$LIN/tools/"
common "$LIN"
cp src/DivaLinux/README.md "$LIN/docs/DivaLinux_README.md"
cp src/DivaOverlay/README.md "$LIN/docs/DivaOverlay_README.md"
for f in src/DivaVK/external/volk/LICENSE.md:volk.md src/DivaVK/external/vma/LICENSE.txt:VulkanMemoryAllocator.txt \
         src/DivaVK/external/openxr/LICENSE:OpenXR.txt .deps/glslang/LICENSE.txt:glslang.txt; do
    src=${f%%:*}; dst=${f##*:}
    if [ -f "$src" ]; then cp "$src" "$LIN/licenses/$dst"; else echo "!! licence file $src not found" >&2; fi
done
FFMPEG_NOTE="Movies need FFmpeg's DLLs in plugins\\ as well: avutil-61, swresample-7, swscale-10,
avcodec-63 and avformat-63, from the aftmods-ffmpeg-ba987fe24 release at
https://github.com/MukiEX/FFmpeg-Builds/releases (DivaMovie's DLLs work too). Without them the game's own
movie player is used, which doesn't work under Wine."
if [ -n "${FFMPEG_DIR:-}" ]; then
    for d in avutil-61 swresample-7 swscale-10 avcodec-63 avformat-63; do
        cp "$FFMPEG_DIR/$d.dll" "$LIN/plugins/"
    done
    if strings "$FFMPEG_DIR/avutil-61.dll" | grep -Eq '^(GPL version|nonfree and unredistributable)'; then
        echo "FFMPEG_DIR has a GPL or nonfree FFmpeg build; only an LGPL build can ship with AFTMods" >&2; exit 1
    fi
    for l in LICENSE LICENSE.md LICENSE.txt COPYING.LGPLv2.1 COPYING.LGPLv3 COPYING.GPLv3 BUILDINFO.txt; do
        [ -f "$FFMPEG_DIR/$l" ] && cp "$FFMPEG_DIR/$l" "$LIN/licenses/FFmpeg_$l"
    done
    ls "$LIN/licenses"/FFmpeg_* >/dev/null 2>&1 || echo "!! no FFmpeg licence file found in $FFMPEG_DIR; add one before publishing" >&2
    FFMPEG_NOTE="FFmpeg's DLLs for the movie player are included in plugins\\. They're FFmpeg
(https://ffmpeg.org) under the LGPL: licence, build details and where to get the source are in
licenses\\FFmpeg_*. Any FFmpeg build with the same library versions can replace them."
fi
cat > "$LIN/INSTALL.txt" <<EOF
AFTMods $VERSION (MukiEX fork, built from $COMMIT) - Windows, Wine, Proton, Steam Deck, WinlatorXR

Unzip into the game folder (the one with diva.exe), so the DVAs land in plugins\\.

  plugins\\DivaLinux.dva  DivaGL's renderer on Vulkan (any Vulkan driver, on Windows or under
                         Wine), with SBS stereo, plus an FFmpeg movie player, faster loading
                         and zip archive support. It replaces DivaGL.dva, DivaVK.dva and
                         DivaMovie.dva: remove or disable those. Settings:
                         plugins\\DivaLinux.ini (created on first run), or the
                         in-game settings menu: F10, or View + Start. Log:
                         plugins\\DivaLinux.log.
  plugins\\Launcher.dva   Fullscreen, controller-friendly launcher (draws on the CPU under
                         WinlatorXR).

$FFMPEG_NOTE

Fewer files to copy: tools\\divalinux_pack.py (Python 3.8+) packs rom and each mdata pack into
uncompressed zips that DivaLinux reads in place of the folders:
    python tools/divalinux_pack.py <game folder>      -> <game folder>_packed\\
Copy rom.zip and mdata\\*.zip into the game folder on the device.

Steam Deck / Proton: launch option  WINEDLLOVERRIDES="dinput8=n,b" %command%
Details: docs\\DivaLinux_README.md and docs\\RELEASE_NOTES.md. DivaGL and its shaders are
korenkonder's work; credit them in anything you make with it (see docs\\AFTMods_README.md).
EOF

# ---- zip, checksums, notes
python3 - "$STAGE" "$OUT" <<'PY'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
for name in sorted(os.listdir(stage)):
    root = os.path.join(stage, name)
    with zipfile.ZipFile(os.path.join(out, name + ".zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, dirs, files in os.walk(root):
            dirs.sort()
            for f in sorted(files):
                path = os.path.join(base, f)
                z.write(path, os.path.relpath(path, root))
PY
(cd "$OUT" && sha256sum ./*.zip | sed 's| \./| |' > SHA256SUMS)

# Changes since the previous release of this fork, or since the upstream commit it branched
# from. Upstream has v-tags of its own (v0.8.0.0, four numbers): fork releases are vX.Y.Z or
# vX.Y.Z-something, on this branch (the fork's history doesn't share commits with upstream's master).
UPSTREAM_BASE=e3555bbc391ab3afb497ec09b325d071c5eec0d1   # korenkonder/AFTMods master when forked
PREV=""
for t in $(git tag --list 'v*' --sort=-creatordate); do
    [ "$t" = "$VERSION" ] && continue
    echo "$t" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' || continue
    git merge-base --is-ancestor "$t" HEAD 2>/dev/null || continue   # this branch's own releases
    PREV=$t
    break
done
BASE=${PREV:-$UPSTREAM_BASE}
git merge-base --is-ancestor "$BASE" HEAD 2>/dev/null || BASE=$(git rev-list --max-parents=0 HEAD | tail -1)
{
    cat RELEASE_NOTES.md
    echo
    if [ -n "$PREV" ]; then echo "## Changes since $PREV"; else echo "## Changes in this fork"; fi
    echo
    git log --no-merges --format='- %s (%h)' "$BASE..HEAD"
    echo
    echo "Built from $COMMIT. SHA-256:"
    echo
    echo '```'
    cat "$OUT/SHA256SUMS"
    echo '```'
} > "$OUT/NOTES.md"

echo "== $OUT"
ls -la "$OUT"

if [ "$PUBLISH" = 1 ]; then
    command -v gh >/dev/null || { echo "--publish needs the GitHub CLI (gh), logged in" >&2; exit 1; }
    if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null; then
        [ "$(git rev-parse "$VERSION^{commit}")" = "$(git rev-parse HEAD)" ] || { echo "tag $VERSION exists on another commit" >&2; exit 1; }
    else
        git tag -a "$VERSION" -m "AFTMods $VERSION (MukiEX fork)"
    fi
    BRANCH=$(git rev-parse --abbrev-ref HEAD)
    git push "$FORK_REMOTE" "$BRANCH" "refs/tags/$VERSION"
    REPO=$(echo "$FORK_REMOTE" | sed -E 's#^(git@github\.com:|https://github\.com/)##; s#\.git$##')
    PRE=""
    case "$VERSION" in *alpha*|*beta*|*rc*) PRE="--prerelease" ;; esac
    gh release create "$VERSION" --repo "$REPO" $PRE --title "AFTMods $VERSION" \
        --notes-file "$OUT/NOTES.md" "$OUT"/*.zip "$OUT/SHA256SUMS"
    echo "== Published $VERSION to $REPO"
fi
