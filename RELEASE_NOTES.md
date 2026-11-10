# MukiEX's AFTMods fork: 0.1.5

Builds on korenkonder's AFTMods (DivaGL 0.11.0.0) for Hatsune Miku: Project DIVA Arcade
Future Tone 7.10 under PD Loader. It has been played on a handful of setups (Windows with AMD,
Steam Deck with Proton, Quest 3 with WinlatorXR, and Quest 3 and an Odin 3 under the DivaPocket
Android port) and nowhere else yet.

## New in 0.1.5

- **An in-game settings menu.** Press F10, or View + Start on a controller, and a menu in the
  style of the game's Key Config screen opens over the running game. It edits
  `DivaLinux.ini` and PD Loader's `config.ini`, `components.ini`, `playerdata.ini` and
  `keyconfig.ini` in one place, with the launcher's own pages and help. Stereo 3D,
  reflections and the pointer change straight away; anything else is saved and applied with
  **Restart game** from the menu. B selects and A goes back, as in the game. It's drawn on the
  GPU, so the game keeps its frame rate while it's open. `[overlay]` in `DivaLinux.ini` turns
  it off or changes the key.
- **More songs per credit.** `Normal_Stages` and `Protected_Stages` in `config.ini`'s
  `[Patches]` (1-10; 0 keeps the game's own) set how many songs a credit gives in Normal and
  in Protected (ASSURE NEXT STAGE) mode. They're in the settings menu too. They stay off when
  `Enhanced_Stage_Manager` is on.
- **The HUD keeps its frame rate when the PV is heavy.** With `pv_frame_skip=1` (the
  default), a frame whose PV ran late is followed by one that reuses the finished PV picture
  and draws only a fresh HUD, so notes stay smooth. `2`-`4` draw the PV every 2nd-4th frame;
  `0` turns it off.
- **A pointer drawn into the picture.** DivaLinux draws the mouse pointer itself, where the
  game sees the mouse: a leek by default, or a crosshair (`[cursor]`). It's in the right
  place in both eyes in side-by-side 3D, where the Windows cursor never is.
- **The launcher's in-game menu** can be drawn over the game by DivaLinux.
- **VR: the HUD as its own layer** (`[vr] hud_layer=1`, experimental and off by default):
  notes and gauges in front of the 3D picture. Screen effects that blend with the picture
  underneath lose their effect on that layer.
- **Fixes.** Depth of field could produce broken (NaN) pixels on Mesa's Turnip driver (Adreno);
  sprites shifted for 3D could stop short of the screen edges; ini files saved by the
  launcher started with a byte order mark, which made the first section unreadable to
  PD Loader.
- **FFmpeg for movies** now comes from the `aftmods-ffmpeg-ba987fe24` release of
  github.com/MukiEX/FFmpeg-Builds (LGPL).

## What's in it

**DivaLinux.dva (Windows, Wine, Proton, Steam Deck, WinlatorXR)**: replaces DivaGL, DivaVK
and DivaMovie
- DivaGL's renderer on Vulkan, on Windows (any Vulkan driver) and under Wine, where no OpenGL
  driver is needed in the prefix. The plain-OpenGL DivaGL.dva is no longer released.
- Side-by-side stereoscopic 3D: half SBS for 3D TVs and XR glasses, full SBS for VR players.
  Shadow maps are drawn once per frame and shared by both eyes. `[stereo]` in DivaLinux.ini.
- FFmpeg movie player (from DivaMovie), with colour conversion done on the GPU. Movies play
  smoothly on a Quest 3.
- OpenXR output: the game on a virtual screen in a headset, in 3D with full SBS
  (experimental).
- Faster loading under Wine. File lookups in rom and mdata are answered from folder listings
  made once; this was most of the load time under Wine.
- `rom.zip` and `mdata\<pack>.zip` (uncompressed) read in place of their folders, so a copy
  of the game is a few hundred files instead of tens of thousands. Make them with
  `tools/divalinux_pack.py`. Movies play from inside them too.
- An in-game settings menu (F10 / View + Start) for DivaLinux's and PD Loader's settings.
- Songs per credit for Normal and Protected mode.
- A pointer drawn into the picture (leek or crosshair), right in both eyes in 3D.
- Diagnostics: frame dump (Scroll Lock), a watchdog that reports stalls, movie playback
  statistics, and a file I/O trace (`[debug] io_trace=1`).

**Launcher.dva**
- A fullscreen, controller-friendly launcher. It has Xbox- or PlayStation-style button
  prompts (which can also be clicked or tapped), and plugin options you can edit with a
  controller. Under WinlatorXR it draws on the CPU, which avoids an X11 error the game would
  otherwise hit.

## Known limits

- Movies need FFmpeg's DLLs in `plugins` (avutil-61, swresample-7, swscale-10, avcodec-63,
  avformat-63). Without them, the game's own player is used, which doesn't work under Wine.
- OpenXR output crashed under WinlatorXR in testing; SBS on a flat screen works.
- A plugin that reads rom/mdata through the C runtime DLL doesn't see archived files (the
  game itself does).
- Archives must be stored zips (7-Zip: `-mx0`). Compressed ones are ignored, with a line in
  `DivaLinux.log` saying so.
