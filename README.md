# AFTMods
Various mods for Hatsune Miku: Project DIVA Mega Mix+

## DivaGL
Ports graphics of PDAFT 7.10 from legacy OpenGL to modern one. Works on Nvidia GPUs as well as on AMD and Intel GPUs (last statement needs testing).

The use of DivaGL and/or its code and/or its shaders and the author of this plugin must be acknowledged in content created using anything above.

### Stereoscopic 3D (Side-by-Side)
Optional SBS stereo output, configured in the `[stereo]` section of `DivaGL.ini` (or the PD Loader config UI):

```ini
[stereo]
stereo_mode=1           ; 0 = off, 1 = half SBS (3D TVs, XR glasses), 2 = full SBS (letterboxed, cross-eye/VR players)
stereo_separation=65    ; eye separation in mm
stereo_convergence=0    ; zero-parallax distance in cm, 0 = auto (camera interest point, usually the singer)
stereo_swap_eyes=0      ; right eye on the left (cross-eyed viewing)
stereo_hud_parallax=0   ; HUD depth in 0.1% of image width, + = behind screen
stereo_bg_infinity=1    ; draw 2D backgrounds / PV movies at infinite depth
```

The whole pipeline (shadows, SSS, reflections, 3D, post-process, sprites) is rendered once per eye using parallel off-axis cameras, so GPU cost roughly doubles. TAA is disabled while stereo is on because its history would mix the two eyes.

## DivaLinux
A single plugin for running PDAFT 7.10 under Wine or Proton (Linux, Steam Deck, and WinlatorXR on Quest). It replaces both `DivaGL.dva` and DivaMovie's `DivaMovie.dva`, so only `DivaLinux.dva` goes in `plugins/`. It's built from:

- **DivaGL, unchanged in what it draws.** The same renderer sources, including SBS stereo, are compiled with `DIVAVK` and `DIVALINUX` defined. Every GL and WGL call goes to DivaVK (`src/DivaVK`), an OpenGL 4.3 subset implemented on Vulkan, so the prefix doesn't need a working OpenGL driver.
- **DivaMovie's FFmpeg movie player** (the `divamovie-ffmpeg` branch of MukiEX/PD-Loader). It replaces the game's own player, which is built on Media Foundation, DXVA2 and Direct3D 9 shared through `WGL_NV_DX_interop`. None of that works under Wine, so the game can't play its movies there.

How the movie player is swapped in:

1. At startup, DivaLinux checks that the game's movie player code matches diva.exe 7.10. Then it hooks the player factory, `CreatePlayer` (`0x1404255C0`), so the game gets the FFmpeg player instead of the Media Foundation one. The FFmpeg player implements the same interface: open, play, pause, seek, state queries and the movie texture.
2. FFmpeg (`avformat`/`avcodec`/`swscale`/`swresample`) is loaded at run time from `plugins/`. If the DLLs are missing, the hook steps aside and the game's own player is used.
3. Frames are decoded on the CPU. The YUV planes are uploaded as they are, and a small shader converts them to RGB straight into the game's movie texture. PV songs draw no 3D models, so the GPU has plenty of spare time for this, and the CPU skips the conversion entirely. It all goes through DivaVK's own GL functions, with the GL state saved and restored around it, and there's no D3D surface or interop handle anywhere. (`[movie] gpu_convert=0` converts to BGRA on the CPU instead.) The texture is created as soon as the video size is known, so the game sees one on its first query.
4. DivaGL's movie task normally switches to a Direct3D 11 copy path when a player's texture isn't ready yet. DivaLinux recognises its own players and keeps them on the GL path.
5. Movie audio is optional (`[movie] movie_audio=1`). It's resampled with `swresample` and played through `waveOut`, and its playback position drives the video timing. It never plays during songs, where the video follows the song's own audio.

Settings live in `plugins/DivaLinux.ini`: DivaGL's sections plus `[movie]` and `[vr]`. On first run, the file is created from an existing `DivaVK.ini` or `DivaGL.ini`, with DivaMovie's player settings carried over. On Windows, keep using `DivaGL.dva`. See [src/DivaLinux/README.md](src/DivaLinux/README.md) for install steps, the FFmpeg DLL versions, settings, OpenXR output and building.

## OPD Play Gen
Generates OPD files on demand if not present. Also allows files to be cached. It only generates cache for missing motions or modules. If FARC with OPD is present, and OPD file inside that FARC is present too, it won't do anything and will load existing data.

## Rob Tex Anim
Allows usage of Auth3D Object's Texture Transform and Texture Pattern in Rob

## Sprite Draw Limit
Breaks limit for Sprite Draw. (Prevents displaying of mess on screen if too many sprites are shown)
