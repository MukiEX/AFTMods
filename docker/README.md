# Building the DVAs in Docker (Linux host)

Uses Microsoft's MSVC and MSBuild under Wine via [msvc-wine](https://github.com/mstorsjo/msvc-wine),
so the solution builds unmodified. MinGW isn't an option: the plugins share `std::`
containers (`prj::vector`, `prj::list`, `prj::string`) with the game, so they need MSVC's STL layout.

```sh
# 1. Build the toolchain image once (~5 GB download, 10-20 min).
#    This accepts the Visual Studio license: https://go.microsoft.com/fwlink/?LinkId=2086102
docker build -t aftmods-msvc docker/

# 2. Build, from the repo root. Output: out/DivaGL.dva and out/DivaVK.dva
docker run --rm -v "$PWD:/src" aftmods-msvc            # or: ... aftmods-msvc Debug
sudo chown -R "$(id -u):$(id -g)" out bin lib obj .deps  # files are created as root
```

What `build.sh` does: fetches glslang and Vulkan-Headers (`vulkan-sdk-1.4.363.0`), builds
`divavk_core` + glslang with `cl.exe` through CMake/Ninja (staging them into
`src/DivaVK/lib/Release`), then runs `msbuild AFTMods.sln /t:DivaGL /t:DivaVK` with the newest
installed platform toolset in place of the projects' VS2019 `v142`.

Install one of the two in PD Loader's `plugins/` folder, not both: `DivaVK.dva` replaces
`DivaGL.dva`. DivaVK logs to `plugins/DivaVK.log`.

On a Windows host, open `AFTMods.sln` in Visual Studio instead (see `src/DivaVK/README.md`).
Microsoft's Windows containers can't run on a Linux host.
