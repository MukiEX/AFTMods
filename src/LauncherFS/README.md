# Fullscreen launcher for PD Loader

A replacement for PD Loader's `Launcher.dva` that opens borderless fullscreen and works with a
controller, so the game can be started from Steam (including SteamOS / Steam Deck) without the
small WinForms window.

- Native Win32 + Direct3D 11 + Dear ImGui. No .NET, so nothing extra to install in the Proton prefix.
- Reads XInput pads 1–4. Steam Input presents every controller as XInput under Proton, so the
  Deck's built-in controls, DualSense, Switch Pro and Xbox pads all work.
- Same settings, files, keys and defaults as the original launcher: `plugins\config.ini`,
  `components.ini`, `playerdata.ini`, `keyconfig.ini`, plugin options from `GetPluginOptions`,
  and custom patches from the `patches` folder.
- Starts the game the same way: `diva.exe --launch` plus your command line, or
  `divahook.bat` / `start.bat` when that option is on.

## Install

1. Back up `plugins\Launcher.dva`.
2. Copy the new `Launcher.dva` over it. Keep the name: PD Loader enables it through
   `Launcher.dva=1` in the `[plugins]` section of `plugins\config.ini`.
3. Start the game as usual.

Only one launcher can be installed at a time; both hook the game's entry point.

## Steam / SteamOS

1. Add `diva.exe` to Steam as a non-Steam game and force a Proton version under
   Properties > Compatibility.
2. Start it. The launcher fills the screen and is ready for the controller straight away.
3. Before the first run, check Display > Display and Resolution. Set Resolution to
   Match screen so the game uses the display's size (1280 × 800 on Steam Deck).
4. To go straight into the game in future, turn on Options > Skip launcher. To get the launcher
   back, set `skip=0` in the `[launcher]` section of `plugins\config.ini`.

## Controls

| Action | Controller | Keyboard | Mouse |
| --- | --- | --- | --- |
| Move | D-pad or left stick | Arrow keys | Hover, wheel |
| Select / switch | A (✕) | Enter, Space | Click |
| Change value | D-pad left/right (hold to speed up) | Left/Right | Click the arrows |
| Back / cancel | B (○) | Esc, Backspace | Click outside |
| Previous / next tab | LB / RB | Q / E, Page Up / Page Down | Click the tab |
| Plugin options | Y (△) | Tab | |
| Play | Start | F5 | Play button |
| Close | View | Alt+F4 | |
| Windowed / fullscreen | | Alt+Enter, F11 | |

Text and numbers are entered with an on-screen keyboard (A types, X deletes, Y toggles shift,
Start finishes). A physical keyboard can type into it too.

Before starting, the launcher checks for combinations that stop the game working: DivaGL and
DivaVK enabled together, or Quick Start used with divahook.bat. It also warns when DivaGL or
DivaVK is enabled alongside Novidia or ShaderPatch.

## Build

The easiest route is llvm-mingw, from Linux or Docker:

```sh
docker build -t launcherfs docker/
docker run --rm -v "$PWD:/src" launcherfs      # -> build/win/Launcher.dva
```

or with llvm-mingw unpacked locally:

```sh
LLVM_MINGW=/path/to/llvm-mingw ./build-mingw.sh
```

MSVC also works (Visual Studio 2019 or later, x64):

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Plugin options are read through `src/plugin_abi.hpp`, a mirror of `PluginConfigApi.h` that
replaces `std::vector` with its three-pointer layout. That's what lets a MinGW build read
options from plugins compiled with MSVC.

### Preview on Linux

`launcher_preview` runs the UI against sample config files, drives it with simulated controller
input and writes PNG screenshots, which is handy for checking layout changes:

```sh
cmake -S . -B build/preview -G Ninja && cmake --build build/preview
build/preview/launcher_preview res/MPLUSRounded1c-Medium.ttf res/MPLUSRounded1c-ExtraBold.ttf shots 1280 800
```

## Third-party code

- [Dear ImGui](https://github.com/ocornut/imgui) — MIT, `third_party/imgui/LICENSE.txt`
- [SimpleIni](https://github.com/brofield/simpleini) — MIT, `third_party/simpleini/LICENCE.txt`
- [MinHook](https://github.com/TsudaKageyu/minhook) — BSD-2-Clause, `third_party/minhook/LICENSE.txt`
- [M PLUS Rounded 1c](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) — SIL OFL 1.1, `res/OFL.txt`
- stb_image_write (preview tool only) — public domain / MIT
