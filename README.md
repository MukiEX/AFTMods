# AFTMods
Various mods for Hatsune Miku: Project DIVA Mega Mix+

This is MukiEX's fork of korenkonder's AFTMods. [FORK_CHANGES.md](FORK_CHANGES.md) maps what it adds and every change to the original code.

## DivaGL
Ports graphics of PDAFT 7.10 from legacy OpenGL to modern one. Works on Nvidia GPUs as well as on AMD and Intel GPUs (last statement needs testing).

In this fork, DivaGL's renderer ships inside **DivaLinux.dva** (below), on Windows as well as under Wine. The plain-OpenGL `DivaGL.dva` is retired: it isn't built by default or released, though its sources are the renderer DivaLinux compiles.

The use of DivaGL and/or its code and/or its shaders and the author of this plugin must be acknowledged in content created using anything above.

### Stereoscopic 3D (Side-by-Side)
Optional SBS stereo output, configured in the `[stereo]` section of `DivaLinux.ini` (`DivaGL.ini` for the retired plain-GL build), or the PD Loader config UI:

```ini
[stereo]
stereo_mode=1           ; 0 = off, 1 = half SBS (3D TVs, XR glasses), 2 = full SBS (letterboxed, cross-eye/VR players)
stereo_separation=65    ; eye separation in mm
stereo_convergence=0    ; zero-parallax distance in cm, 0 = auto (the character the shot frames)
stereo_swap_eyes=0      ; right eye on the left (cross-eyed viewing)
stereo_hud_parallax=0   ; HUD depth in 0.1% of image width, + = behind screen
stereo_bg_infinity=1    ; draw 2D backgrounds / PV movies at infinite depth
stereo_shadow_once=1    ; render the shadow maps for the first eye only (they're seen from the light)
stereo_max_parallax=20  ; most the eyes' pictures may differ, in 0.1% of the picture width (0 = no limit)
```

The whole pipeline (SSS, reflections, 3D, post-process, sprites) is rendered once per eye using parallel off-axis cameras, so GPU cost roughly doubles. Shadow maps are the exception: they're drawn from the light's point of view, so the second eye reuses the first eye's (`stereo_shadow_once=0` draws them twice). TAA is disabled while stereo is on because its history would mix the two eyes.

With automatic convergence, the zero-parallax plane follows the character nearest the middle of the shot, so the singer stays on the screen plane even when a PV camera aims past her. Eye separation is reduced in zoomed-in shots, so the background (or a character close to the camera) never splits more than `stereo_max_parallax` allows. Raise that for stronger 3D; lower it if anything still doubles.

## DivaLinux
The single renderer plugin for PDAFT 7.10, on Windows and under Wine or Proton (Linux, Steam Deck, and WinlatorXR on Quest). It replaces both `DivaGL.dva` and DivaMovie's `DivaMovie.dva`, so only `DivaLinux.dva` goes in `plugins/`. It began as the Wine build (hence the name) and runs the same on Windows with any Vulkan driver. It's built from:

- **DivaGL, unchanged in what it draws.** The same renderer sources, including SBS stereo, are compiled with `DIVAVK` and `DIVALINUX` defined. Every GL and WGL call goes to DivaVK (`src/DivaVK`), an OpenGL 4.3 subset implemented on Vulkan, so the prefix doesn't need a working OpenGL driver.
- **DivaMovie's FFmpeg movie player** (the `divamovie-ffmpeg` branch of MukiEX/PD-Loader). It replaces the game's own player, which is built on Media Foundation, DXVA2 and Direct3D 9 shared through `WGL_NV_DX_interop`. None of that works under Wine, so the game can't play its movies there.

How the movie player is swapped in:

1. At startup, DivaLinux checks that the game's movie player code matches diva.exe 7.10. Then it hooks the player factory, `CreatePlayer` (`0x1404255C0`), so the game gets the FFmpeg player instead of the Media Foundation one. The FFmpeg player implements the same interface: open, play, pause, seek, state queries and the movie texture.
2. FFmpeg (`avformat`/`avcodec`/`swscale`/`swresample`) is loaded at run time from `plugins/`. If the DLLs are missing, the hook steps aside and the game's own player is used.
3. Frames are decoded on the CPU. The YUV planes are uploaded as they are, and a small shader converts them to RGB straight into the game's movie texture. PV songs draw no 3D models, so the GPU has plenty of spare time for this, and the CPU skips the conversion entirely. It all goes through DivaVK's own GL functions, with the GL state saved and restored around it, and there's no D3D surface or interop handle anywhere. (`[movie] gpu_convert=0` converts to BGRA on the CPU instead.) The texture is created as soon as the video size is known, so the game sees one on its first query.
4. DivaGL's movie task normally switches to a Direct3D 11 copy path when a player's texture isn't ready yet. DivaLinux recognises its own players and keeps them on the GL path.
5. Movie audio is optional (`[movie] movie_audio=1`). It's resampled with `swresample` and played through `waveOut`, and its playback position drives the video timing. It never plays during songs, where the video follows the song's own audio.

DivaLinux also replaces the mouse cursor with a pointer drawn into the picture, a leek or a crosshair (`[cursor]` in `DivaLinux.ini`). It shows where the game sees the mouse, so in side-by-side 3D it's in the right place in both eyes and clicks land under it, and it fades out when the mouse isn't in use.

DivaLinux also speeds up loading under Wine. It answers the game's many file lookups from folder listings it makes once, instead of asking Wine every time. It can also read `rom.zip` and `mdata\<pack>.zip` (uncompressed, made with `tools/divalinux_pack.py`) in place of those folders, so a copy of the game is a few dozen files instead of tens of thousands.

Settings live in `plugins/DivaLinux.ini`: DivaGL's sections plus `[movie]` and `[vr]`. On first run, the file is created from an existing `DivaVK.ini` or `DivaGL.ini`, with DivaMovie's player settings carried over. See [src/DivaLinux/README.md](src/DivaLinux/README.md) for install steps, the FFmpeg DLL versions, settings, OpenXR output and building.

## LauncherFS (Launcher.dva)
A fullscreen, controller-friendly replacement for PD Loader's launcher (`src/LauncherFS`). It has Xbox- or PlayStation-style button prompts (which can also be clicked or tapped), and plugin options you can edit with a controller. It draws with Direct3D 11, OpenGL or the CPU, picking the CPU automatically under WinlatorXR, where the GPU paths leave X11 resources behind that the game's window then trips over.

With DivaLinux, it can also run as an in-game menu instead (Options > In-game menu): the game starts straight away, F10 or View + Start opens the launcher over it, and Restart applies changed settings.

It's built separately from DivaGL and DivaLinux, with llvm-mingw: `src/LauncherFS/build-mingw.sh`, or its Docker image (see `src/LauncherFS/README.md`).

## DivaOnline
Aime card and ALL.Net support for PDAFT 7.10, so the game can log in with a card and play on an ARTEMiS server without segatools' `inject.exe` (`src/DivaOnline`). It's segatools' divahook as a PD Loader plugin: the keychip, ALL.Net DNS and network environment, AMEX board and the Aime reader on COM10, minus the input and display hooks PD Loader already covers. The card is a line in `plugins/DivaOnline.ini`:

```ini
[aime]
; hold Insert on the card screen to tap the card
accessCode=5012 3456 7890 1234 5678
[dns]
default=your.artemis.server
```

It's built with llvm-mingw (`src/DivaOnline/build-mingw.sh`). See [src/DivaOnline/README.md](src/DivaOnline/README.md).

## OPD Play Gen
Generates OPD files on demand if not present. Also allows files to be cached. It only generates cache for missing motions or modules. If FARC with OPD is present, and OPD file inside that FARC is present too, it won't do anything and will load existing data.

## Rob Tex Anim
Allows usage of Auth3D Object's Texture Transform and Texture Pattern in Rob

## Sprite Draw Limit
Breaks limit for Sprite Draw. (Prevents displaying of mess on screen if too many sprites are shown)
