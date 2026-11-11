# What this fork changes, for korenkonder

A map of MukiEX's fork of AFTMods for someone who knows the original code: what was added,
what was changed in your files and why, and the rules the changes depend on.

## Where it starts

The fork is your `master` at `e3555bb` ("How did I forget this", 2026-07-05), which is still
upstream's latest commit. Its git history was restarted, so it shares no commits with yours
and `git log` won't show the changes. Comparing the trees works:

```
git remote add fork git@github.com:MukiEX/AFTMods.git && git fetch fork
git diff e3555bb fork/async-pv -- src/DivaGL          # everything changed in DivaGL
git diff --stat e3555bb fork/async-pv                 # everything, by file
```

Untouched: `src/AFTModsShared`, `src/KKdLib`, `src/OPDPlayGen`, `src/RobTexAnim`,
`src/SpriteDrawLimit`, `extern`. Two shaders changed (`res/ft_shaders/dof_downsample.frag`,
`dof_upsample.frag`: their `pow()` inputs are clamped, because Mesa's Turnip driver on Adreno
produced NaN there), and `res/ft_shaders.farc` was rebuilt with them (`tools/pack_ft_shaders.py`). `res/LakeHP_OpenRooms.dll` is new: open room filters for the headphone surround.

## New folders (yours to skip)

None of these are needed to work on DivaGL. Each has its own README.

| Folder | What it is |
|---|---|
| `src/DivaVK` | An OpenGL 4.3 subset on Vulkan, so DivaGL runs without an OpenGL driver (Wine, Android). Has its own test suite (`src/DivaVK/tests`). |
| `src/DivaLinux` | The plugin that ships, `DivaLinux.dva`: DivaGL compiled on DivaVK, plus an FFmpeg movie player (from DivaMovie), a file layer for Wine and zip archives, the drawn mouse pointer, OpenXR output, headphone surround (`audio/`, a hook on the game's mixer), and diagnostics. `main.cpp` is where it starts. |
| `src/DivaOverlay` | An in-game settings menu drawn over the game (F10). |
| `src/LauncherFS` | A fullscreen launcher, `Launcher.dva`. |
| `docker/`, `release.sh`, `CMakeLists.txt` | Building with MSVC under Wine in Docker, and packaging. The `.sln`/`.vcxproj` files still build too. |

`DivaLinux.dva` is DivaGL compiled with `DIVAVK` and `DIVALINUX` defined. Built without them
(`BUILD_DIVAGL=1 docker/build.sh`, the `DivaGL` CMake target, or the solution), DivaGL is plain
OpenGL as before, with 3D stereo and PV frame skip; threaded PV needs DivaVK's shared contexts
and is compiled out there (`pv_render.cpp` has stubs). The fork releases only DivaLinux.dva, so
that build is checked less often.

## Changes to your DivaGL files

Two new files hold most of it: `stereo.cpp/.hpp` (3D stereo and VR) and `pv_render.cpp/.hpp`
(PV frame skip, the VR HUD layer, threaded PV). The rest are edits in your files, each marked
with a comment naming what it's for, so `grep -rn "// threaded PV" src/DivaGL` finds all of
one feature's edits. The tags:

### `// 3D stereo`: side-by-side 3D and VR

The whole frame is drawn once per eye with off-axis cameras, then the two pictures are put
side by side.
- `render_manager.cpp`: your `render_all` body is now `render_all_passes`, which
  `render_all_stereo` runs once per eye (`render_all` runs it once when stereo is off). The
  shadow maps are drawn for the first eye only (`stereo_reuse_shadow_maps`). Once-per-frame
  counters (exposure history, queries) are kept from advancing twice.
- `camera.cpp`: no TAA jitter in stereo (TAA is off there); the eye cameras are made in
  `ctrl_camera`; the screen-AABB check widens its side planes so objects one eye sees aren't
  culled.
- `mdl/disp_manager.cpp`: the same wider culling for objects.
- `sprite.cpp`: per-list horizontal parallax for sprites, and the VR HUD layer's
  premultiplied-alpha blending.
- `render.cpp`: no copy to the window inside an eye pass.
- `dllmain.cpp`, `static_var.*`: the `[stereo]` settings, `SetOption`/`SetOptionInt` for
  them (the settings menu changes them live), and their launcher options.

### `// PV frame skip`: the HUD keeps its rate when the PV is heavy

`[performance] pv_frame_skip`. After post-process the finished PV picture is kept; on a
skipped frame the PV passes don't run, the kept picture is put back and only the HUD is drawn
on top. The game's logic and judging run every frame regardless. Code: `pv_render.cpp`; in
`render_all_passes` the skip branch and the save after `RND_PASSID_POST_PROCESS`.

### `// threaded PV`: the PV on a thread of its own

Only in DivaLinux.dva, and only under the DivaRecomp runtime (the Android port), which asks
for it through a shared struct (`DIVARECOMP_PV_SYNC`); on Windows and Wine it never starts. The game thread draws the
HUD every frame over the newest finished PV picture, and a second thread with its own GL
context draws the PV pictures. The design and its reasoning are in
`src/DivaLinux/THREADED_PV.md`. The edits in your files make the render globals per-thread:

- `rctx`, `disp_manager`, `gl_state`, `sprite_manager` and the `gl_rend_state` sets are now
  the calling thread's: the game thread's as before, or the PV thread's own copies
  (`render_context.hpp`, `mdl/disp_manager.hpp`, `gl_state.hpp`, `sprite.cpp`,
  `gl_rend_state.*`). On the game thread they behave exactly like the old globals.
- The camera getters (`camera.cpp`), the shadow state (`render_shadow()` in place of
  `shadow_ptr`), and the Glitter scene lists (`Glitter/particle_manager.cpp`) answer the PV
  thread with copies taken at the handoff.
- `vertex_array_cache` in `mdl/disp_manager.cpp` is under a shared mutex (the PV thread looks
  up, the game thread adds and removes).
- `hook.cpp`: `rndpass_pre_proc` and the PV's post-process are skipped on the game thread
  while the PV thread does them.
- `object.cpp`, `texture.cpp`, `effect.cpp`, `light_param.cpp`, `Glitter/particle_manager.cpp`:
  frees wait for the PV thread's picture (`pv_thread_wait_idle`, rule 2 below).

The rules it depends on, which an edit to these areas must keep:
1. Handoff happens on the game thread, only while the PV thread is idle: the draw lists, the
   PV's sprite lists (1 and 2), the Glitter scene pointers, the camera and the shadow state
   move or are copied to the PV thread then (`disp_manager_handoff`,
   `sprite_manager_handoff_pv_lists`, `SnapshotScenesForPv`, `pv_camera_snapshot_take`).
2. The game tasks that make and free what the PV thread draws (Glitter, the stage effects)
   update only while the PV thread is idle; the runtime schedules them (`pv_throttle.c` in
   DivaDroid). The characters' tasks update every frame (their motion needs it); they change
   matrices but don't free memory. Anything freed outside those tasks (a stage change in the
   song script unloads effects, object sets and textures right away) waits for the PV thread
   to finish its picture first: `pv_thread_wait_idle` in DivaGL's free hooks and in the game's
   Glitter scene destructor.
3. Anything the PV thread reads that the game thread changes every frame has to come from a
   handoff copy. A new read of live game state on the PV's draw path breaks this rule.
4. The game thread never waits for the PV thread during a song; it only checks whether a
   picture is done on the GPU.

### `// DivaLinux` and `// DivaVK`: the plugin around DivaGL

- `dllmain.cpp`: `DivaLinux.ini` instead of `DivaGL.ini` (migrated on first run), the bridge
  to DivaVK installed before GLUT makes its context, `divalinux_install` (everything in
  `src/DivaLinux`), OpenXR settings, the launcher options for movies, the pointer,
  performance and VR.
- `wrap.cpp`: the game's GL function tables pointed at DivaVK.
- `task_movie.cpp`: DivaLinux's FFmpeg player kept off the Direct3D 11 copy path.
- `renderer/dof.cpp`: `DIVA_DOF=0` turns depth of field off.
- Frame-time logging in `render_all` (`[performance] perf_log`).

Not marked, because they're removals: several files lost their local
`extern render_context* rctx;` lines, since `rctx` is now declared in `render_context.hpp`
as the per-thread reference.

## Switches outside the .ini

Environment variables read by this repository's code: `DIVA_DOF=0` (depth of field off),
`DIVAVK_PRESENT_MODE`, `DIVAVK_WINDOW_PRESENT=0` (no window present while OpenXR shows the
frames), `DIVAVK_HUD_DEPTH_CM` (the VR HUD layer's distance), `DIVAVK_DUMP_DRAWS=1` (frame
dumps after every draw), `DIVAVK_VALIDATION=1` (Vulkan validation layers),
`DIVALINUX_DUMP_AFTER=<seconds>` (one frame dump, no input needed). `DIVARECOMP_PV_SYNC`
and `DIVARECOMP_OVERLAY` are addresses of structs shared with the DivaRecomp runtime, which
sets them. The other switches listed in the settings menu belong to that runtime.
