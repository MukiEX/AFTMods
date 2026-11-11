// PV frame skip, the VR HUD layer and threaded PV: the parts of DivaLinux's PV rendering that
// render_manager.cpp's render_all / render_all_passes / render_all_stereo call into
// (pv_render.cpp). Not part of upstream DivaGL; the call sites there are marked.
#pragma once

#include "render_manager.hpp"
#include "render_context.hpp"
#include "../KKdLib/default.hpp"
#include <windows.h>

class Shadow;

// Slots: in threaded mode one shown under the HUD, one whose picture the GPU is drawing, one
// the PV thread is recording into, so the next picture never waits for the last one's GPU work.
#define PV_SLOTS 3

/* A host that also throttles the PV's game-side work (the DivaRecomp runtime: PV tasks'
   updates and draw lists split over two frames) decides the schedule, so draw lists and
   the picture agree: its shared struct's address is in DIVARECOMP_PV_SYNC. */
struct pv_sync_struct {
    volatile uint32_t magic;        // 'PVSY'
    volatile uint32_t version;      // 1
    volatile uint32_t skip;         // host: this frame, reuse the kept PV picture
    volatile uint32_t cache_ready;  // us: a kept picture exists for every eye
    volatile uint32_t frames_seen;  // us: frames we followed the host's schedule
    // version 2: async PV (see pv_async_frame)
    volatile uint32_t async;        // host: async PV schedule this frame
    volatile uint32_t disp_seq;     // host: bumped on frames where the PV's draw lists were rebuilt
    volatile uint32_t pv_busy;      // us: a PV picture is being drawn over several frames
    volatile uint32_t pv_done;      // us: PV pictures finished
    volatile uint32_t async_ok;     // us: async PV supported
    volatile uint32_t period_us;    // host: the display's frame time
    volatile uint32_t pv_drawing;   // us: the PV thread is drawing right now (CPU side)
};
extern pv_sync_struct* pv_sync;

// PV frame skip: the kept PV pictures, [slot][eye] (eye 0 mono or left, 1 right)
extern GLuint pv_cache_fb[PV_SLOTS][2];
extern bool pv_cache_valid[PV_SLOTS][2];
extern int32_t pv_front;        // slot put back under the HUD
extern int32_t pv_save_slot;    // slot a finished PV picture goes to
extern bool pv_skip;            // this frame reuses the kept PV picture
extern bool pv_threaded_was;    // threaded PV was on last frame

// frame timing, for auto mode and the perf log
struct pv_stat_t { int32_t frames, full; double_t sum_ms, worst_ms, render_ms, pv_ms, hud_ms; LARGE_INTEGER start; };
extern pv_stat_t pv_stat;
extern LARGE_INTEGER pv_qpf;
extern double_t pv_frame_ms;
double_t pv_ms_since(const LARGE_INTEGER& t0);

void pv_frame_begin();                                                // once per frame, before drawing
void pv_cache_save(render_data_context& rend_data_ctx, int32_t eye);   // the finished PV picture
void pv_cache_restore(render_data_context& rend_data_ctx, int32_t eye);

// 3D stereo: VR HUD layer ([vr] hud_layer)
extern bool hud_layer_active;   // this frame
extern bool hud_layer_was;
bool hud_layer_wanted();
void draw_hud_layer();

// threaded PV
bool pv_threaded_on();          // the host's schedule asks for it this frame
bool pv_threaded_active();      // and the PV thread is running (hook.cpp)
void pv_threaded_frame();       // game thread: show a finished picture, hand off the next
void pv_threaded_leave();       // game thread: the first frame after a song
void pv_thread_stop();
// Game thread, before freeing anything a PV picture may read (scenes, object sets, textures):
// waits for the PV thread to finish recording the picture it's on. `what` names it in the log.
void pv_thread_wait_idle(const char* what);
Shadow* render_shadow();        // the shadow state the calling thread draws with
