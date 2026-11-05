# Building the DVAs in Docker (Linux host)

Uses Microsoft's MSVC (`cl.exe`) under Wine via [msvc-wine](https://github.com/mstorsjo/msvc-wine),
driven by the root `CMakeLists.txt` (MSBuild 18 doesn't run under Wine Mono).
MinGW isn't an option: the plugins share `std::` containers (`prj::vector`, `prj::list`, `prj::string`) with the game, so they need MSVC's STL layout.

```sh
# 1. Build the toolchain image once (~5 GB download, 10-20 min).
#    This accepts the Visual Studio license: https://go.microsoft.com/fwlink/?LinkId=2086102
docker build -t aftmods-msvc docker/

# 2. Build, from the repo root. Output: out/DivaLinux.dva and out/Launcher.dva
docker run --rm -v "$PWD:/src" aftmods-msvc            # or: ... aftmods-msvc Debug
```

What `build.sh` does: fetches glslang and Vulkan-Headers (`vulkan-sdk-1.4.363.0`) and the
FFmpeg headers DivaLinux's movie player is built against, builds everything with `cl.exe` through
CMake/Ninja, copies `DivaLinux.dva` to `out/`, and hands the files it created back to the owner of
the source folder. It then builds `Launcher.dva` (`src/LauncherFS`) with llvm-mingw, which it
downloads into `.deps` on first use. Set `BUILD_LAUNCHER=0` (`docker run -e BUILD_LAUNCHER=0 ...`)
to skip it.

- `DivaLinux.dva` is the renderer plugin for Windows and for Wine/Proton: DivaGL on Vulkan plus
  DivaMovie's FFmpeg movie player. It replaces `DivaGL.dva`, `DivaVK.dva` and `DivaMovie.dva`;
  install it instead of all three. See `src/DivaLinux/README.md`.
- `Launcher.dva` is the fullscreen, controller-friendly launcher. See `src/LauncherFS/README.md`.
- The plain-OpenGL `DivaGL.dva` is retired and no longer built by default. `BUILD_DIVAGL=1`
  (`docker run -e BUILD_DIVAGL=1 ...`) still builds it.

On a Windows host, open `AFTMods.sln` in Visual Studio instead (see `src/DivaVK/README.md`).
Microsoft's Windows containers can't run on a Linux host.

## Releases

`./release.sh v0.1.0-alpha.1` (from the repo root, after a build, or with `--build`) packages
`out/` into `release/<version>/`:

- `AFTMods-<version>.zip`: DivaLinux, the launcher and the pack tool, for Windows and Wine alike,
  with install notes and third-party licences
- `SHA256SUMS`, and `NOTES.md` (`RELEASE_NOTES.md` plus the commits since the last release)

`FFMPEG_DIR=<folder>` adds FFmpeg's DLLs to the zip. `--publish` tags the commit, pushes the branch and tag to
`git@github.com:MukiEX/AFTMods.git` (never upstream), and creates a GitHub pre-release with
`gh`.
