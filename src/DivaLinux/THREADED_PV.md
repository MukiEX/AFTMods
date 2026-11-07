# Threaded PV rendering: audit and design

Goal: notes and HUD at the display's rate, whatever the PV costs. The PV (characters, stage,
effects, camera, and the 3D render) may lag a frame or drop frames.

Status: audit and design only. Nothing here is implemented yet, apart from the pieces called
out as existing.

## Where the time goes today (Quest 3, heavy PV, DivaRecomp runtime)

| | per frame |
|---|---|
| game tasks: update + draw lists (the runtime's `[pv]` task profile) | ~7 ms, of which PV ~5 ms |
| PV render, on frames that draw it (`[DivaLinux] perf:`) | 16-19 ms |
| HUD render | ~1.4 ms |

With the PV split over two frames (runtime `pv_throttle.c` plus `pv-frame-skip`), one frame
updates the PV and the next draws it. The draw frame alone is 16-19 ms, so the HUD's rate is
bounded by the PV render.

The game thread also presented to the Android window with vsync (FIFO). On light frames the log
showed `present took 11 ms`, and a 17 ms frame was rounded up to the next slot (27.8 ms at
72 Hz). In the headset that window isn't what you see: DivaVK's OpenXR frame thread (xr.cpp)
shows the newest finished staging image at the headset's rate. The runtime now presents in
mailbox mode and paces frames itself (no rounding to slots). That's a separate, smaller fix and
already done.

## How a frame flows (single thread)

1. Engine update (input), then `TaskWork` runs every task's `ctrl()` and then `disp()`.
   PV tasks' `disp()` fill DivaGL's draw lists. The HUD and note game fill sprite request lists.
2. `RenderManager::render_all()` on the same thread: shadow, SSS, reflect/refract, pre-process,
   clear, pre-3D sprites (list 2), 3D (including surface sprites, list 1, and Glitter), post-process,
   then HUD sprites (list 0).
3. `Context::present()`: submit, copy to an XR staging image, and present to the window.

## What the PV render reads

### 1. Draw lists: self-contained, can be double-buffered

`mdl::DispManager` lives at the game's address `0x1411A27F0` (0x9D0 bytes, DivaGL's layout).
Each entry is allocated from a per-frame arena (`buff`/`buff_offset`). `init_sub_mesh` stores the
object matrix by value. `alloc_mat4_array` copies the transposed bone matrices, so entries don't
point into characters' live bone data. The remaining pointers are static: mesh, material,
texture name lists, vertex/index/morph buffers (GL names).

Game code that touches the manager's memory directly (found by scanning the recompiled game for
absolute references into `0x1411A27F0..+0x9D0`) is confined to its own method block
`0x1404356E0-0x14043A7D0`:
- setters for the *current entry state*: obj flags, shadow group, texture pattern, texture
  colour/transform, morph, wet param (offsets 0x0-0xC, 0x230-0x364);
- the hooked `entry_obj` / `refresh` / `init` (lists at 0x10-0x1F0, arena at 0x23C-0x248);
- four of the game's original list-draw routines that DivaGL replaces.

So swapping the 30 object lists and the arena at handoff is invisible to the game.

**Exception: user entries.** `OBJ_KIND_USER` entries keep a function pointer and a data pointer
to a live object: fog ring (`effect.cpp:896`), ripple (`:1194`), water particles (`:1462`). Those
draw from live effect state.

### 2. Sprite request lists 1 and 2

Surface sprites (list 1, drawn inside the 3D pass) and pre-3D sprites (list 2: 2D stage
backgrounds and movie-PV frames) belong to the PV picture. List 0 is the HUD. The sprite manager
is at `0x14CC611C0`. Lists 1 and 2 must be handed off with the draw lists; list 0 stays on the
main thread.

### 3. Render parameters read live, to snapshot at handoff

| state | where | written by |
|---|---|---|
| camera (`rctx->render_manager_cam`) | DivaGL `render_context` | DivaGL's `ctrl_camera` hook (0x1401F8970), every frame |
| light sets | `light_set_data` 0x1411A00A0 | game: stage/PV light params, song-script light commands |
| fog | `fog_data` 0x1411974D0 | same |
| light param storage, IBL textures | 0x1411A06A0 | stage changes (hooked gen/delete textures) |
| post-process (exposure, tone map, bloom, DOF, lens flare, chara alpha) | `rndr::Render` via `render_get()` 0x1404AE8A0 | `pv_param_task` PostProcessCtrl* (a PV task) |
| render manager flags (`pass_sw`, shadow, reflect, NPR) | `render_manager` 0x1411AD320 | game and DivaGL |
| shadow parameters | `g_shadow` 0x14CC587B8 | game, per frame |
| SSS | `_sss_data` 0x14CC924E0 | game |
| draw state | `draw_state` 0x1411A32B0 | game |
| light projection (litproj) | 0x141194D80 / 0x141194DA0 / 0x141194E00 | stage params |
| misc | face data (0x1403D8310), `light_chara_ambient`, `chara_reflect`, `npr_cloth_spec_color` | game |

These are plain structs. Copying them at handoff into a render-side snapshot (and pointing the
render passes at the copy) makes the render independent of later writes.

### 4. Live objects drawn during the render passes

- **Glitter:** particle vertex data is built in `CalcDisp()`, called from the
  `rndpass_pre_proc` hook (hook.cpp:200) on the game thread before `render_all`. The render
  passes call `DispScenes()` (render_manager.cpp 1114-1222), which only walks the render groups
  and draws buffers that are already filled. So `pre_proc` (with `CalcDisp`) belongs to the
  handoff, and the render thread only needs the render groups to stay put.
- **Effects:** the user entries above (fog ring, ripple, water), and any effect drawing from its
  task's live state.

The particles and effect state change in their tasks' `ctrl()` (PV tasks), and their structure
changes when effects are created or destroyed (song-script commands run by `TaskPvGame`).

## Design

### Schedule: update the PV only while the renderer is idle

The runtime already hooks every task's `ctrl`/`disp` and knows which tasks are PV
(`pv_throttle.c`). Extend its schedule:

1. **Render thread idle:** the main thread runs the PV tasks' `ctrl` (frame speed scaled by
   the frames since their last update, as now) and `disp`, then **hands off**: swap draw lists
   and sprite lists 1 and 2, snapshot the render parameters, and start the render thread.
2. **Render thread busy:** PV tasks skip `ctrl` and `disp` (the throttle already does this).
   The main thread runs the note game, the HUD and everything else, draws the HUD over the newest
   *finished* PV picture, and presents.
3. **Render thread done:** its PV picture becomes the newest; the next frame does step 1.

`rndpass_pre_proc` (render pre-process, Glitter `CalcDisp`, `rctx->pre_proc`) runs as part of
step 1 on the main thread, right before the handoff.

This solves most of the data-race problem by construction: PV objects (characters, particles,
effect state) never change while the render thread reads them, because their tasks only update
while it's idle. What remains are changes made by non-PV code during a busy frame:

- **Song-script commands** in `TaskPvGame::ctrl` (camera cuts, lights, effects, stage changes).
  Lights, fog, camera and post-process are covered by the snapshot. Character motion changes
  are safe because the draw lists hold copied bone matrices.
- **Effect and Glitter creation/destruction** changes structures the render thread iterates.
  DivaGL owns its Glitter manager (`Glitter/particle_manager.cpp`) and effect code, so requests
  arriving while the render thread is busy are queued and applied at the next handoff.
- **Loading and unloading** (song start and end, stage changes): drain the render thread first.
  Simplest: run threaded only during `GS_GAME`, and drain on any game-state change.

### DivaVK: a second context

Today there's one global context (`g_ctx`, `#define CTX Context& c = *g_ctx`) that holds GL state,
name tables (buffers, textures, samplers, framebuffers, VAOs, shaders, programs, queries),
pipeline, render-pass and framebuffer caches, and per-frame command buffers, descriptor pools and
arenas (`FRAMES_IN_FLIGHT`). Things that help:
- `queue_mutex` already serialises submission (the XR frame thread submits too);
- objects carry `last_used_serial` and `defer_destroy()`;
- images return to a `rest_layout` between uses.

Changes:
- **Split shared from per-thread state.** Shared: device, queue, name tables, pipeline and
  render-pass caches (each behind a mutex), deferred destruction. Per thread: GL state
  (bindings, raster state, current program), `Frame` ring (command buffers, arena, descriptor
  pools, queries), pending clears, the current render pass.
- **Current context per thread:** `g_ctx` becomes thread-local. The render thread gets its own
  per-thread state sharing the same objects.
- **Destruction across threads:** destroy an object only once every context's frames that used
  it have completed: a global timeline instead of one serial, or per-context completed serials.
- **Images used by both threads** (the PV pictures, movie textures, shared render targets): the
  rest-layout convention plus submission order on one queue keeps them consistent. Only the
  layout bookkeeping needs locking.
- **Present and XR staging copy:** main thread only.

### DivaGL

- **Draw lists:** a second `ObjList` set and arena. At handoff, the game-facing struct at
  `0x1411A27F0` gets the empty set; the render thread owns the filled one. `refresh()` clears only
  the game-facing set.
- **Sprites:** the same for request lists 1 and 2.
- **Render snapshot:** one struct with the section 3 parameters. The render passes read it
  through `rctx` / `render_manager` pointers set to the copy while the render thread runs.
- **PV pictures:** two per eye (being drawn, finished); the existing `pv_cache` becomes the
  finished one. The main thread's HUD pass composites over the finished picture, as on a skipped
  frame now.
- **Glitter and effect requests** queued during busy frames, applied at handoff.
- **`render_all` split** into a PV part (render thread) and a HUD part (main thread). The pass
  structure already separates them: everything up to post-process, then sprites list 0.

### The runtime

- **Schedule:** extend `pv_throttle.c` from "split over two frames" to "update when the renderer
  is idle". The sync struct gains `render_busy` / `picture_serial`.
- **Fallbacks:** the current two-frame split and the render-only skip stay as fallbacks
  (`pv_frame_skip` values).

## Expected result

On the heavy PV, each main-thread frame costs the note game, HUD tasks and HUD render (about
3 ms), plus the PV's update and draw lists on handoff frames (about 5 ms): within a 13.9 ms frame.
The PV runs at whatever the render thread manages, one frame or more behind. The Quest 3's XR2
Gen 2 has cores to spare.

## Phases

1. **Done in the runtime:** mailbox present and frame pacing; the per-task profile.
2. **DivaVK:** shared/per-thread state split, thread-local context, cross-context destruction.
   Testable on its own with a second thread drawing into an offscreen target.
3. **DivaGL:** double-buffered draw lists and sprite lists 1/2, render snapshot, Glitter/effect
   request queue.
4. **DivaGL:** `render_all` split, PV render on a thread, HUD composite over the finished
   picture, drain on game-state changes.
5. **Runtime:** "update when idle" schedule; fallbacks.

## Open questions

- **Song-script commands:** do any (stage object swaps, auth3d changes) alter state the draw lists
  point at, beyond what's listed? Test by logging script opcodes while the render thread is busy.
- **Shadow pass:** does it read character positions live (for shadow placement) or only from
  the draw lists? `pass_shadow` needs a closer read.
- **Movie frames:** decoded on the main thread into a texture the PV picture samples (list 2).
  This is fine on one queue, but the GPU conversion pass and the render thread's reads need
  ordering (DivaVK layout bookkeeping).
