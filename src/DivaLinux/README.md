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
| `gpu_convert` | 1 | Convert frames from YUV to RGB on the GPU: the decoded planes are uploaded as they are and a shader writes the movie texture. 0 converts on the CPU with swscale, as DivaMovie does. Falls back to the CPU by itself if the GPU path fails. |
| `fast_decode` | 0 | Skip the H.264/HEVC deblocking filter and use FFmpeg's "fast" shortcuts from the start. Slightly blockier, noticeably cheaper on slow CPUs (Quest/phones through FEX). Without it, the player only does this while it's behind. |
| `decode_threads` | 0 | Threads for video decoding; 0 lets FFmpeg use one per CPU. |

On the first run, if `DivaLinux.ini` doesn't exist yet, it is created from `DivaVK.ini` (or
`DivaGL.ini`), and `ffmpeg_player`/`movie_audio` are copied from `DivaMovie.ini`.

DivaMovie's other options (software/hybrid decoding, interop emulation, debug) are gone: they
controlled Media Foundation and D3D9 paths that DivaLinux doesn't use.

## Faster loading and fewer files: `[loading]`

DivaLinux can take over the game's file access in `rom\` and `mdata\` (`vfs.cpp`). This
helps with two problems on slow devices such as a Quest, or a phone running the game through
Wine and FEX.

### Lookup cache

While loading, the game asks every mdata pack for every file it might override: tens of
thousands of single-file lookups (`FindFirstFileExW`) per start-up or song load, most of
which find nothing. Under Wine, each lookup scans a whole folder, because Windows names ignore
case and Linux ones don't. Under FEX on a Quest that costs about 5 ms. A `[debug] io_trace`
log there showed a 160 s start-up spending 200 s of loading-thread time in lookups that
missed, and 1 s reading data.

With the cache, each folder is listed once, the first time something in it is looked for.
Later lookups are answered from memory: misses fail at once with the error Windows would
give, and hits return the file's details. Wildcard searches are answered from the same
listings. A file created through `CreateFileW` drops its folder's listing, so the next lookup
sees it.

### Archives

`rom.zip` (next to `diva.exe`) and `mdata\<pack>.zip` stand in for the `rom` folder and
the `mdata\<pack>` folders, so a copy of the game is a few dozen files instead of tens of
thousands. That matters over MTP, which copies files one by one. With an archive present,
DivaLinux reads it instead of the folder of the same name. Without it, the folder is used.
Archives and loose packs can be mixed freely.

- **Uncompressed only.** A member is then a plain range of the zip, read with one
  positional read, and nothing is decompressed on the emulated CPU. The game's data (FARC
  archives, textures, movies) is compressed already, so compressing again saves little
  space. A zip with compressed members is ignored, with a log line naming one, and the
  folder is used.
- **Laid out like the game folder.** `rom.zip` holds `rom/...` and `MZKO.zip` holds
  `mdata/MZKO/...`, so "extract here" in the game folder rebuilds the original folders.
  Zips of just the folder (`MZKO/...`) or of its contents are read as well.
- **Read-only.** Opening an archived file for writing fails with "access denied".
- **zip64** is supported, for archives and members over 4 GB.

To make the archives:

```
python tools/divalinux_pack.py H:\Games\PDAFT                 (writes H:\Games\PDAFT_packed)
python tools/divalinux_pack.py H:\Games\PDAFT --packs MZKO     (just one pack)
```

The output mirrors the game folder (`rom.zip`, `mdata\<pack>.zip`); copy it into the game
folder on the device. A zip is only rewritten when its folder changed. It needs Python 3.8 or
later and nothing else.

With 7-Zip, from the game folder: `7z a -tzip -mx0 rom.zip rom` and
`7z a -tzip -mx0 mdata\MZKO.zip mdata\MZKO`. `-mx0` (Store) matters. Windows' own "Send to
compressed folder" compresses, so those zips won't be read.

| `[loading]` key | Default | What it does |
| --- | --- | --- |
| `lookup_cache` | 1 | The lookup cache. 0 asks Wine for every lookup in loose folders. |
| `archives` | 1 | Read `rom.zip` and `mdata\<pack>.zip` in place of their folders. |

The log names each archive in use, with its file count, and every 10 s while busy prints a
`Files:` line with lookups answered, folders listed, archive files opened and MB read.

All of this works through the import tables of the game and its plugins, never Wine's own
DLLs. If another plugin already hooks a function, that plugin's version is left in place.
One limit: a plugin that reads `rom`/`mdata` through the C runtime DLL (ucrtbase's `fopen`)
bypasses the hooks and sees only real folders. The game itself reads through its own import
table.

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

Each movie that played ends with a `[Movie] stats` line: codec, pixel format and decoder
threads; decoded frames per second; CPU time per frame for decoding, YUV-to-BGRA conversion
and the texture upload; the worst lag behind the clock; and how long the player spent catching
up (reduced quality, keyframe jumps). A `Video decode:` line near the top says whether the GPU
driver offers Vulkan Video decoding.

Frame dump: press Scroll Lock or Pause, or create an empty file named `DivaLinux_dump_request`
in the game folder. The next frame's passes are written to `DivaLinux_dump_<time>/`.

File I/O trace: with `[debug] io_trace=1`, DivaLinux hooks the file functions of the game and
its plugins (import tables only; any plugin's own path redirection still runs) and logs a
summary after each burst of file activity (start-up, each song load), once the game has been
quiet for two seconds:

- calls, time and misses for each function (`CreateFileW`, `GetFileAttributesW`,
  `FindFirstFileW`, `ReadFile`, the C runtime's `fopen`/`_stat`/`fread`, ...)
- the busiest folders (`mdata\<pack>`, `rom`, ...) by time
- the most-missed file names, and the slowest single calls
- megabytes read, and how long that took

The hooks cost a little time themselves, so leave it off for normal play.

Set `DIVAVK_VALIDATION=1` in the environment to enable the Khronos validation layer.

## Source layout

| Path | What |
| --- | --- |
| `src/DivaGL/` | The renderer; compiled with `DIVAVK` and `DIVALINUX` defined for this plugin |
| `src/DivaVK/core/` | GL-on-Vulkan runtime (`divavk_core`) |
| `src/DivaVK/plugin/vk_bridge.cpp` | WGL emulation and GL import/table patching |
| `movie/FFLoader.h`, `FFMovie.h`, `FFPlayer.h` | DivaMovie's FFmpeg player, nearly unchanged |
| `movie/movie_gl.h` | The player's GL upload functions, taken from DivaVK instead of opengl32 |
| `movie/movie_yuv.h` | GPU YUV-to-RGB conversion into the movie texture, inside `dvk_push_gl_state`/`dvk_pop_gl_state`; tested in `src/DivaVK/tests` |
| `movie/movie.cpp` | Installs the player on the game's `CreatePlayer` factory |
| `io_trace.cpp` | The `[debug] io_trace` file I/O trace |
| `vfs.cpp` | `[loading]`: the lookup cache and archive reading |
| `../../tools/divalinux_pack.py` | Makes `rom.zip` and `mdata\<pack>.zip` |
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
