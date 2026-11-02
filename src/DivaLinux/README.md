# DivaLinux

One PD Loader plugin for running Project DIVA Arcade Future Tone under Wine or Proton (Linux,
Steam Deck, and later Android/ARM through FEX). It combines:

- **DivaGL on Vulkan (DivaVK).** DivaGL's renderer, including SBS stereo, with every GL/WGL call
  routed to a GL-on-Vulkan runtime (`src/DivaVK`). No real OpenGL context is created.
- **The FFmpeg movie player from DivaMovie** (`divamovie-ffmpeg` branch of MukiEX/PD-Loader).
  Song and attract-mode movies are decoded by FFmpeg and uploaded straight into the game's movie
  texture, so Media Foundation, DXVA2, Direct3D and `WGL_NV_DX_interop` are never needed.

On Windows, use `DivaGL.dva` instead; it handles movies itself.

## Install

1. Put `DivaLinux.dva` in `plugins/`.
2. Remove or disable `DivaGL.dva`, `DivaVK.dva` and `DivaMovie.dva`. DivaLinux contains all
   three, and it shows a warning if any of them is loaded too.
3. For movies, put the FFmpeg DLLs in `plugins/`: `avutil-61.dll`, `swresample-7.dll`,
   `swscale-10.dll`, `avcodec-63.dll`, `avformat-63.dll`. These are the same files DivaMovie
   uses: FFmpeg master builds from October 2026 (DivaLinux is built against master `ba987fe`,
   2026-10-10). Their major versions must match exactly. Without them, the game's own movie
   player is used.

## Settings: `plugins/DivaLinux.ini`

Same sections and keys as `DivaGL.ini` (`[general]`, `[experimentals]`, `[stereo]`), plus:

| `[movie]` key | Default | What it does |
| --- | --- | --- |
| `ffmpeg_player` | 1 | Play movies with FFmpeg. 0 uses the game's movie player. |
| `movie_audio` | 0 | Play movies' own audio tracks. Never during songs, where the video follows the song. |

On the first run, if `DivaLinux.ini` doesn't exist yet, it is created from `DivaVK.ini` (or
`DivaGL.ini`), and `ffmpeg_player`/`movie_audio` are copied from `DivaMovie.ini`.

DivaMovie's other options (software/hybrid decoding, interop emulation, debug) are gone: they
controlled Media Foundation and D3D9 paths that DivaLinux doesn't use.

## VR headsets (OpenXR)

With `[vr] enabled=1`, DivaLinux also shows the game on a virtual screen in a headset, through
whatever OpenXR runtime the Wine prefix has: Proton's `wineopenxr` (which hands over to
SteamVR, Monado or WiVRn on the Linux side) or, on a Quest, WinlatorXR's `openxr_wxr.dll`.
The monitor/window keeps showing the game too.

| `[vr]` key | Default | What it does |
| --- | --- | --- |
| `enabled` | 0 | Turn VR output on. |
| `screen_width_cm` | 300 | Width of the virtual screen. |
| `screen_distance_cm` | 250 | How far in front of you it floats. |
| `screen_height_cm` | 0 | Raise (or lower, negative) the screen from eye level. |
| `runtime_json` | (empty) | A runtime manifest to use instead of the prefix's active runtime. |

3D comes from DivaGL's SBS stereo: set `[stereo] stereo_mode` to 2 (full SBS) and the left half
of each frame goes to your left eye, the right half to your right eye. Full SBS on a 32:9
resolution (e.g. 3840x1080) gives each eye a full 16:9 1920x1080 picture; with
`stereo_content_aspect` set, the letterbox bars are left out. Half SBS (mode 1) works too, at
half the horizontal resolution per eye. `stereo_mode` 0 gives a flat screen.

The screen is placed straight ahead of where you look when the game starts. Press **Home** to
bring it back in front of you; recentring from the headset's own menu does the same.

Frames go to the headset from their own thread at the headset's refresh rate, so the screen
stays steady in the room even though the game renders at 60 fps.

The log shows each step (`OpenXR: runtime ...`, `headset ...`, `session focused`,
`first frame shown in the headset`). "no runtime is installed or active" means the prefix has
no OpenXR runtime registered: set `runtime_json`, or start the game through Proton with your
OpenXR runtime running.

## Log and debugging

Everything goes to `plugins/DivaLinux.log`: Vulkan device, unimplemented GL calls, and movie
lines tagged `[Movie]` (FFmpeg loaded or why not, each movie opened and finished).

Frame dump: press Scroll Lock or Pause, or create an empty file named `DivaLinux_dump_request`
in the game folder. The next frame's passes are written to `DivaLinux_dump_<time>/`.

Set `DIVAVK_VALIDATION=1` in the environment to enable the Khronos validation layer.

## Source layout

| Path | What |
| --- | --- |
| `src/DivaGL/` | The renderer; compiled with `DIVAVK` and `DIVALINUX` defined for this plugin |
| `src/DivaVK/core/` | GL-on-Vulkan runtime (`divavk_core`) |
| `src/DivaVK/plugin/vk_bridge.cpp` | WGL emulation and GL import/table patching |
| `movie/FFLoader.h`, `FFMovie.h`, `FFPlayer.h` | DivaMovie's FFmpeg player, nearly unchanged |
| `movie/movie_gl.h` | The player's GL upload functions, taken from DivaVK instead of opengl32 |
| `movie/movie.cpp` | Installs the player on the game's `CreatePlayer` factory |
| `src/DivaVK/core/src/xr.cpp` | OpenXR output (virtual screen), tested against `src/DivaVK/tests/mock_xr_runtime.cpp` |

## Building

`docker/build.sh` (from the repo root) builds `out/DivaGL.dva` and `out/DivaLinux.dva`. It
fetches the FFmpeg headers for commit `ba987fe` into
`.deps/ffmpeg-ba987fe24d6713c4d4b3a15cebaeca18c41fbd31/include`; to use another FFmpeg, change
`FFMPEG_COMMIT` there and ship that build's DLLs.

With CMake directly, pass `-DDIVALINUX_FFMPEG_INCLUDE_DIR=<dir with libavcodec/...>` along with
the DivaVK options. In Visual Studio, the `DivaLinux` project reads the same folder through the
`DivaLinuxFFmpegInclude` property.

## Next steps

- Head-tracked VR: render each eye from the headset's pose (a projection layer) instead of
  showing the SBS frame on a virtual screen.
- Upload movie frames as YUV planes and convert them in a shader, instead of converting to BGRA
  on the CPU and swizzling again in DivaVK's upload.
