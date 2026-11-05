# MukiEX's AFTMods fork: 0.1.1

Builds on korenkonder's AFTMods (DivaGL 0.11.0.0) for Hatsune Miku: Project DIVA Arcade
Future Tone 7.10 under PD Loader. It has been played on a handful of setups (Windows with AMD,
Steam Deck with Proton, Quest 3 with WinlatorXR) and nowhere else yet.

## New in 0.1.1

- **Movies play from zip archives.** With `rom.zip` or `mdata\<pack>.zip` in place of the
  folders, PV and attract movies inside them failed to open ("No such file or directory" in
  `DivaLinux.log`), because FFmpeg opened movie files itself and didn't see the archives.
  It now reads them through DivaLinux's file layer, so movies inside the zips play and can
  seek.
- **A movie that won't open no longer hangs the song.** The game waits for a movie it asked
  for to become ready; when one failed, the song froze on its loading screen forever. A movie
  that can't be opened is now logged and played as an empty one, and the song carries on.
- **One package for Windows and Wine.** DivaLinux.dva runs fine on Windows too (with DivaGL.dva
  and DivaMovie.dva removed), so the plain-OpenGL DivaGL.dva is no longer released, and the
  separate Windows and Linux zips are now a single `AFTMods-<version>.zip`. If you used the
  Windows package, replace `plugins\DivaGL.dva` with `plugins\DivaLinux.dva`; its settings go
  in `plugins\DivaLinux.ini`, created from your `DivaGL.ini` on first run.

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
