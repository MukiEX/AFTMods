# Building the DVAs in Docker (Linux host)

Uses Microsoft's MSVC (`cl.exe`) under Wine via [msvc-wine](https://github.com/mstorsjo/msvc-wine),
driven by the root `CMakeLists.txt` (MSBuild 18 doesn't run under Wine Mono).
MinGW isn't an option: the plugins share `std::` containers (`prj::vector`, `prj::list`, `prj::string`) with the game, so they need MSVC's STL layout.

```sh
# 1. Build the toolchain image once (~5 GB download, 10-20 min).
#    This accepts the Visual Studio license: https://go.microsoft.com/fwlink/?LinkId=2086102
docker build -t aftmods-msvc docker/

# 2. Build, from the repo root. Output: out/DivaGL.dva and out/DivaLinux.dva
docker run --rm -v "$PWD:/src" aftmods-msvc            # or: ... aftmods-msvc Debug
```

What `build.sh` does: fetches glslang and Vulkan-Headers (`vulkan-sdk-1.4.363.0`) and the
FFmpeg headers DivaLinux's movie player is built against, builds everything with `cl.exe` through
CMake/Ninja, copies the two DVAs to `out/`, and hands the files it created back to the owner of
the source folder.

- `DivaGL.dva` is for Windows.
- `DivaLinux.dva` is for Wine/Proton: DivaGL on Vulkan plus DivaMovie's FFmpeg movie player. It
  replaces `DivaGL.dva`, `DivaVK.dva` and `DivaMovie.dva`; install it instead of all three. See
  `src/DivaLinux/README.md`.

On a Windows host, open `AFTMods.sln` in Visual Studio instead (see `src/DivaVK/README.md`).
Microsoft's Windows containers can't run on a Linux host.
