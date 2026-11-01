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

## Building

`docker/build.sh` (from the repo root) builds `out/DivaGL.dva` and `out/DivaLinux.dva`. It
fetches the FFmpeg headers for commit `ba987fe` into
`.deps/ffmpeg-ba987fe24d6713c4d4b3a15cebaeca18c41fbd31/include`; to use another FFmpeg, change
`FFMPEG_COMMIT` there and ship that build's DLLs.

With CMake directly, pass `-DDIVALINUX_FFMPEG_INCLUDE_DIR=<dir with libavcodec/...>` along with
the DivaVK options. In Visual Studio, the `DivaLinux` project reads the same folder through the
`DivaLinuxFFmpegInclude` property.

## Next steps

- Upload movie frames as YUV planes and convert them in a shader, instead of converting to BGRA
  on the CPU and swizzling again in DivaVK's upload.
