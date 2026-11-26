# MukiEX's AFTMods fork: 0.1.6

Builds on korenkonder's AFTMods (DivaGL 0.11.0.0) for Hatsune Miku: Project DIVA Arcade
Future Tone 7.10 under PD Loader. It has been played on a handful of setups (Windows with AMD,
Steam Deck with Proton, Quest 3 with WinlatorXR, and Quest 3 and an Odin 3 under the DivaPocket
Android port) and nowhere else yet.

## New in 0.1.6

- **Online play: DivaOnline.dva** (new, in `AFTMods-DivaOnline-<version>.zip`). Aime card
  login and ALL.Net, so the game plays on an [ARTEMiS](https://gitea.tendokyu.moe/Hay1tsme/artemis)
  server under PD Loader, without segatools' `inject.exe`. It's segatools' `divahook` as a
  plugin: keychip, ALL.Net DNS and network environment, the AMEX board and the Aime reader
  on COM10, minus the input and display hooks TLAC and DivaLinux already cover. Its settings
  are segatools.ini's sections and keys, in `plugins\DivaOnline.ini`.
  - The card is a line in the .ini (`[aime] accessCode=`, 20 digits, or `felicaIdm=`),
    tapped with Insert, R3, or L2 + R2 on a gamepad or a DIVA controller on DIVA Bridge.
    `autoInsert=1` keeps it on the reader.
  - A real card on a PC/SC reader such as an ACR122U: `[aime] reader=auto`. A segatools
    aimeio DLL works too (`[aimeio] path=`); if it fails to start, the .ini's card is used.
  - PD Loader's `quick_start` skip of the card screen is undone, and its other patches are
    left as they are (the startup network check stays short).
  - While online, TLAC's `playerdata.ini` and `scores.ini` are switched off for the run, since
    your profile and scores come from the server. No file is edited; `enable=0` plays
    offline with them as before.
  - `plugins\DivaOnline.zip`, if you make one, holds known-good `DEVICE\`, `billing\` and
    `firm\` files that are restored at startup; `reset_device=1` (or the menu's Online >
    Reset device data) goes back to them.
  - The settings menu has **Game > Online**: online play on or off, the card reader, the
    card always on the reader, keeping TLAC's player data.
  - Its log (`[divaonline] log_file=DivaOnline.log`) includes diva.exe's own ALL.Net and
    card reader messages.
- **Headphone surround.** `[audio] headphones=1|2|3` in `DivaLinux.ini` plays the game's
  stereo as if from two speakers in a small, medium or large room, with LakeHP's engine and
  3 ms of added delay. The rooms come from `LakeHP_OpenRooms.dll` (open filters, CC BY-SA
  3.0, included), or from `DolbyHph.dll` from your own copy of the original engine. The
  settings menu has an Audio page that changes it live.
- **Light projection at full resolution.** The stage lights projected onto the scene (the
  spinning gears in Gaikotsu Gakudan to Riria, for one) were drawn at the game's 1280x720
  and stretched, which stepped their edges at high resolutions and on wide screens. They're
  now drawn at the scene's resolution (`[general] litproj_res`: 0 the game's, 1 half,
  2 full, the default), with a shadow map up to 4x the game's size
  (`litproj_shadow_scale`). Both are under Graphics > Light projection in the menu.
- **Stereo 3D.** The light projection's shadow map is drawn once per frame, not per eye.
  `[stereo] stereo_hud_once=1` (off by default) draws the HUD once and places it in each
  eye, so on ultrawide SBS (32:9) the HUD's depth works as at 16:9 and nothing is cut off at
  the edges. Multiply and overlay HUD effects are approximate that way, as on the VR HUD layer.
- **A DIVA controller drives the settings menu on Windows** with DivallerBridge.dva: Select +
  Start opens it, and the slider and face buttons move around it.
- **Fixes.** Depth of field's near blur read the wrong sample offsets on AMD, so near
  objects came out sharp and aliased. Threaded PV (the DivaRecomp port) waits for the PV
  thread before the song frees a stage's objects and textures.

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

- Headphone surround (`[audio]`), with open room filters included.

**DivaOnline.dva**: Aime card and ALL.Net, in its own zip
- segatools' divahook as a PD Loader plugin, for playing on an ARTEMiS server.
- The card from the .ini, a key or gamepad tap, or a real PC/SC card reader.
- TLAC's player data and scores off while online; on again with `enable=0`.
- Vendored segatools (public domain) and capnhook (MIT); built with llvm-mingw.

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
- DivaOnline has been played on Windows against one self-hosted ARTEMiS server; under Wine
  only its tests have run. It's for diva.exe 7.10 only. The startup check shows Location
  Server NG (the in-store file server, which ARTEMiS doesn't provide); the game plays online
  regardless.
