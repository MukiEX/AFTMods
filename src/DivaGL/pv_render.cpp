/*
    DivaLinux (MukiEX's AFTMods fork): PV frame skip, the VR HUD layer and threaded PV, moved
    out of render_manager.cpp, which calls into them (see pv_render.hpp). Built into
    DivaGL.dva and DivaLinux.dva alike; threaded PV runs only under a host that asks for it
    (the DivaRecomp runtime), and the design is in src/DivaLinux/THREADED_PV.md.
*/

#include "render_manager.hpp"
#include "../KKdLib/mat.hpp"
#include "../AFTModsShared/light_param/fog.hpp"
#include "../AFTModsShared/light_param/light.hpp"
#include "Glitter/glitter.hpp"
#include "mdl/disp_manager.hpp"
#include "mdl/draw_object.hpp"
#include "rob/rob.hpp"
#include "camera.hpp"
#include "clear_color.hpp"
#include "effect.hpp"
#include "gl_state.hpp"
#include "light_param.hpp"
#include "reflect_full.hpp"
#include "render.hpp"
#include "render_context.hpp"
#include "render_texture.hpp"
#include "shader_ft.hpp"
#include "sprite.hpp"
#include "sss.hpp"
#include "stage_param.hpp"
#include "static_var.hpp"
#include "stereo.hpp"
#include "texture.hpp"
#include <Helpers.h>
#ifdef DIVAVK
#include <divavk/divavk.h>
#endif
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstring>
#include "pv_render.hpp"

/* PV frame skip ([performance] pv_frame_skip in DivaLinux.ini).
   The game's logic, note timing and judging run every frame no matter what; this only
   decides how often the PV picture itself (shadows, reflections, 3D, post-process) is
   drawn. After post-process the finished PV picture is kept; on a skipped frame those
   passes don't run, the kept picture is put back and only the HUD is drawn fresh on top.
     0  off
     1  auto: when a frame with the PV drawn ran late, the next frame reuses it
        (never two in a row), so the HUD keeps the display's rate under load
     2-4  draw the PV every 2nd/3rd/4th frame
   Everything drawn into the PV picture freezes with it on skipped frames, including 2D
   backgrounds and movie PVs (cheap, so auto mode rarely skips those). */
// Slots: in threaded mode one shown under the HUD, one whose picture the GPU is drawing, one
// the PV thread is recording into, so the next picture never waits for the last one's GPU work.
GLuint pv_cache_fb[PV_SLOTS][2] = {};       // [slot][eye]: eye 0 mono or left, 1 right
static GLuint pv_cache_tex[PV_SLOTS][2] = {};      // their colour textures (plain GL ones, see pv_cache_save)
bool pv_cache_valid[PV_SLOTS][2] = {};
int32_t pv_front = 0;                  // slot put back under the HUD
int32_t pv_save_slot = 0;              // slot a finished PV picture goes to (the other one in async mode)
static int32_t pv_cache_w[PV_SLOTS] = {};    // per slot (written by whoever draws into it)
static int32_t pv_cache_h[PV_SLOTS] = {};
bool pv_skip = false;                  // this frame reuses the kept PV picture
bool pv_threaded_was = false;          // threaded PV (pv_threaded_frame) was on last frame
static int32_t pv_since_full = 0;             // frames since the PV was last drawn

/* frame timing, for auto mode and the perf log */
LARGE_INTEGER pv_qpf = {};
static LARGE_INTEGER pv_last = {};
double_t pv_frame_ms = 0.0;            // last frame's duration (start to start)
static double_t pv_vsync_ms = 0.0;            // shortest recent frame: about the display's frame time
static bool pv_last_was_full = false;
pv_stat_t pv_stat = {};

pv_sync_struct* pv_sync = 0;
static bool pv_sync_checked = false;

double_t pv_ms_since(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return pv_qpf.QuadPart ? (double_t)(t.QuadPart - t0.QuadPart) * 1000.0 / (double_t)pv_qpf.QuadPart : 0.0;
}

static void pv_log_stats(double_t elapsed_s) {
    if (!sv_perf_log || pv_stat.frames <= 0)
        return;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "[DivaLinux] perf: %.1f fps, frame avg %.1f ms worst %.1f ms (display %.1f ms), PV drawn %d%% of frames"
        " (%s), render CPU avg %.1f ms (PV %.1f per drawn frame, HUD %.1f)\n",
        pv_stat.frames / elapsed_s, pv_stat.sum_ms / pv_stat.frames, pv_stat.worst_ms, pv_vsync_ms,
        pv_stat.full * 100 / pv_stat.frames,
        pv_sync ? "following the host's schedule" : sv_pv_frame_skip == 0 ? "frame skip off" : sv_pv_frame_skip == 1 ? "auto" : "fixed",
        pv_stat.render_ms / pv_stat.frames, pv_stat.full ? pv_stat.pv_ms / pv_stat.full : 0.0,
        pv_stat.hud_ms / pv_stat.frames);
    OutputDebugStringA(buf);
    {
        // for comparing with the PV thread's pictures: what the game thread's lists hold now
        size_t objs = 0;
        for (int32_t i = 0; i < mdl::OBJ_TYPE_MAX; i++)
            objs += disp_manager_main->obj[i].size();
        snprintf(buf, sizeof(buf), "[DivaLinux] game thread's draw lists at this frame's render: %zu entries (opaque %zu)\n", objs, disp_manager_main->obj[mdl::OBJ_TYPE_OPAQUE].size());
        OutputDebugStringA(buf);
    }
}

/* once per frame, before anything is drawn */
void pv_frame_begin() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!pv_qpf.QuadPart)
        QueryPerformanceFrequency(&pv_qpf);
    pv_frame_ms = pv_last.QuadPart ? (double_t)(now.QuadPart - pv_last.QuadPart) * 1000.0 / (double_t)pv_qpf.QuadPart : 0.0;
    pv_last = now;
    if (pv_frame_ms > 0.5) {
        if (pv_vsync_ms <= 0.0 || pv_frame_ms < pv_vsync_ms)
            pv_vsync_ms = pv_frame_ms;
        else
            pv_vsync_ms += (pv_frame_ms - pv_vsync_ms) * 0.001;   // drift up slowly if the display rate drops
        pv_stat.frames++;
        pv_stat.sum_ms += pv_frame_ms;
        if (pv_frame_ms > pv_stat.worst_ms)
            pv_stat.worst_ms = pv_frame_ms;
    }
    if (!pv_stat.start.QuadPart)
        pv_stat.start = now;
    const double_t elapsed = (double_t)(now.QuadPart - pv_stat.start.QuadPart) / (double_t)pv_qpf.QuadPart;
    if (elapsed >= 10.0) {
        pv_log_stats(elapsed);
        pv_stat = {};
        pv_stat.start = now;
    }

    const bool sb = rctx->screen_buffer.m_txhd_color != 0;
    const int32_t w = sb ? rctx->screen_buffer.get_width() : 0;
    const int32_t h = sb ? rctx->screen_buffer.get_height() : 0;
    if (!pv_threaded_was && (w != pv_cache_w[pv_front] || h != pv_cache_h[pv_front]))
        memset(pv_cache_valid, 0, sizeof(pv_cache_valid));

    if (!pv_sync_checked) {
        pv_sync_checked = true;
        char buf[32];
        const DWORD n = GetEnvironmentVariableA("DIVARECOMP_PV_SYNC", buf, sizeof(buf));
        if (n && n < sizeof(buf)) {
            pv_sync_struct* s = (pv_sync_struct*)(uintptr_t)strtoull(buf, 0, 16);
            if (s && s->magic == 0x59535650u && s->version >= 1) {
                pv_sync = s;
#ifdef DIVAVK   // threaded PV needs DivaVK's shared contexts
                if (s->version >= 2)
                    s->async_ok = 1;
#endif
                OutputDebugStringA(s->version >= 2
                    ? "[DivaLinux] PV frame skip: following the host's PV schedule (async PV supported)\n"
                    : "[DivaLinux] PV frame skip: following the host's PV schedule\n");
            }
        }
    }

    const bool have = pv_cache_valid[pv_front][0] && (!stereo_enabled() || pv_cache_valid[pv_front][1]);   // 3D stereo: both eyes cached
    bool skip = false;
    if (pv_sync) {
        skip = have && pv_sync->skip;
        pv_sync->cache_ready = have ? 1 : 0;
        pv_sync->frames_seen++;
    }
    else if (sv_pv_frame_skip == 1)
        skip = have && pv_last_was_full && pv_vsync_ms > 0.0 && pv_frame_ms > pv_vsync_ms * 1.3;
    else if (sv_pv_frame_skip >= 2)
        skip = have && pv_since_full + 1 < sv_pv_frame_skip;
    pv_skip = skip;
    pv_last_was_full = !skip;
    pv_since_full = skip ? pv_since_full + 1 : 0;
    if (!skip && !(pv_sync && pv_sync->version >= 2 && pv_sync->async))
        pv_stat.full++;   // (async PV counts finished pictures instead)
}

/* A PV picture copy (save or copy back) with plain GL calls: bound right before the blit and
   the earlier bindings put back after, nothing through the render state's or gl_state's
   binding caches. A cache that is out of date with what's really bound sends a blit to or
   from the wrong framebuffer; on the Odin 3 with Turnip the PV now and then showed, for whole
   frames, a picture from the start of the song (as Qualcomm's driver had shown in one half). */
static void pv_blit(GLuint src_fb, GLuint dst_fb, int32_t sw, int32_t sh, int32_t dw, int32_t dh) {
    GLint prev_draw = 0, prev_read = 0;
    glGetIntegervDLL(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glGetIntegervDLL(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fb);
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, sw == dw && sh == dh ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
}

void pv_cache_save(render_data_context& rend_data_ctx, int32_t eye) {
    if ((!sv_pv_frame_skip && !pv_sync) || !rctx->screen_buffer.m_txhd_color)
        return;
    const int32_t w = rctx->screen_buffer.get_width();
    const int32_t h = rctx->screen_buffer.get_height();
    if (w <= 0 || h <= 0)
        return;
    const int32_t slot = pv_save_slot;
    if (w != pv_cache_w[slot] || h != pv_cache_h[slot]) {
        for (int32_t e = 0; e < 2; e++) {
            if (pv_cache_fb[slot][e]) {
                glDeleteFramebuffers(1, &pv_cache_fb[slot][e]);
                pv_cache_fb[slot][e] = 0;
            }
            if (pv_cache_tex[slot][e]) {
                glDeleteTexturesDLL(1, &pv_cache_tex[slot][e]);
                pv_cache_tex[slot][e] = 0;
            }
        }
        pv_cache_valid[slot][0] = pv_cache_valid[slot][1] = false;
        pv_cache_w[slot] = w;
        pv_cache_h[slot] = h;
    }
    if (!pv_cache_tex[slot][eye]) {
        /* A plain GL texture and framebuffer, made with GL calls that don't go through any of
           the binding caches, and the real bindings put back after:
           - not RenderTexture::create_texture: the game's texture manager is a map without a
             lock, filled from the game thread too, keyed by a counter that isn't unique;
           - not RenderTexture's framebuffer setup: it binds through gl_state's cache, which the
             render state's own binds (straight to GL) leave stale, so the bind could be
             skipped and the texture attached to whatever framebuffer was really bound. Frame
             dumps from an Odin 3 (SBS stereo) showed exactly that: both eyes saved into one
             texture, the post-process drawn into a saved picture, and the right eye shown
             with half of an old picture. */
        GLint prev_draw = 0, prev_read = 0, prev_tex = 0;
        glGetIntegervDLL(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
        glGetIntegervDLL(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
        glGetIntegervDLL(GL_TEXTURE_BINDING_2D, &prev_tex);

        GLuint tex = 0, fb = 0;
        glGenTexturesDLL(1, &tex);
        glBindTextureDLL(GL_TEXTURE_2D, tex);
        glTexParameteriDLL(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteriDLL(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteriDLL(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteriDLL(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteriDLL(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteriDLL(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2DDLL(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindTextureDLL(GL_TEXTURE_2D, (GLuint)prev_tex);

        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0);
        glDrawBufferDLL(GL_COLOR_ATTACHMENT0);
        glReadBufferDLL(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);

        pv_cache_tex[slot][eye] = tex;
        pv_cache_fb[slot][eye] = fb;
        char buf[160];
        snprintf(buf, sizeof(buf), "[DivaLinux] PV picture slot %d %s: texture %u, framebuffer %u (%dx%d)\n",
            slot, eye ? "right eye" : "left eye/mono", tex, fb, w, h);
        OutputDebugStringA(buf);
    }
    pv_blit(rctx->screen_buffer.get_fb(), pv_cache_fb[slot][eye], w, h, w, h);
    pv_cache_valid[slot][eye] = true;
}

// 3D stereo: VR HUD layer, to draw_hud_layer
/* HUD layer ([vr] hud_layer): in a headset, the HUD (sprite list 0) isn't drawn into the
   picture but once per frame into a transparent target of its own, which DivaVK shows as a
   separate layer just in front of the PV. The PV picture (both eyes with SBS stereo) goes to
   the window/headset without it, so the HUD stands out from the 3D, and it can later update
   on its own. */
static RenderTexture* hud_layer_rt = 0;
bool hud_layer_active = false;   // this frame
bool hud_layer_was = false;
bool stereo_hud_once_active = false;   // 3D stereo: [stereo] stereo_hud_once, this frame
extern bool sprite_hud_layer_blend;

bool hud_layer_wanted() {
#ifdef DIVAVK
    return sv_vr_hud_layer && dvk_xr_active();
#else
    return false;
#endif
}

// The HUD (sprite list 0) drawn once into the transparent layer target, premultiplied; its
// texture, or 0. For the VR layer (draw_hud_layer) and for SBS stereo's HUD drawn once
// (stereo_composite_hud).
GLuint render_hud_layer_texture() {
    // The HUD layer is shown at the picture's shape: with SBS stereo the window can be 32:9
    // (two 16:9 eyes), but the HUD is one 16:9 picture over both.
    const int32_t h = rctx->screen_height;
    int32_t w = rctx->screen_width;
    if (stereo_enabled() && sv_stereo_content_aspect > 0 && h > 0)
        w = (int32_t)(h * (sv_stereo_content_aspect / 1000.0) + 0.5);
    if (w <= 0 || h <= 0)
        return 0;
    if (!hud_layer_rt)
        hud_layer_rt = new RenderTexture;
    if (hud_layer_rt->m_txhd_color && (hud_layer_rt->get_width() != w || hud_layer_rt->get_height() != h))
        hud_layer_rt->destroy();
    if (!hud_layer_rt->m_txhd_color)
        hud_layer_rt->create_texture(w, h, 0, GL_RGBA8, 0);

    render_data_context rend_data_ctx(GL_REND_STATE_2D);
    rend_data_ctx.state.get();
    rend_data_ctx.state.begin_event("hud_layer");
    hud_layer_rt->begin_render(rend_data_ctx.state);
    rend_data_ctx.state.set_viewport(0, 0, w, h);
    // Whatever earlier passes left behind must not limit the clear: everything outside the
    // HUD's sprites has to be fully transparent, or the headset shows it over the PV.
    rend_data_ctx.state.disable_scissor_test();
    rend_data_ctx.state.disable_stencil_test();
    rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
    if (sprite_manager_reqlist_count(0)) {
        rend_data_ctx.state.disable_depth_test();
        rend_data_ctx.state.enable_blend();
        rend_data_ctx.state.disable_cull_face();
        sprite_manager_set_res((double_t)w / (double_t)h, w, h);
        sprite_hud_layer_blend = true;
        sprite_manager_draw(rend_data_ctx, 0, true,
            rctx->screen_overlay_buffer.get_texture(), set_projection_matrix_2d(false));
        sprite_hud_layer_blend = false;
        // Back to the screen's resolution for the next frame's other sprite lists, which
        // don't set it themselves (pass_sprite does, before list 0).
        sprite_manager_set_res((double_t)rctx->screen_width / (double_t)rctx->screen_height,
            rctx->screen_width, rctx->screen_height);
        rend_data_ctx.state.enable_cull_face();
        rend_data_ctx.state.disable_blend();
        rend_data_ctx.state.enable_depth_test();
        shader::unbind(rend_data_ctx.state);
    }
    hud_layer_rt->end_render(rend_data_ctx.state);
    rend_data_ctx.state.bind_framebuffer(0);
    rend_data_ctx.state.end_event();
    rend_data_ctx.state.finish();
    return hud_layer_rt->get_texture_glid();
}

void draw_hud_layer() {
#ifdef DIVAVK
    dvk_xr_set_hud_texture(render_hud_layer_texture());
#endif
}

void pv_cache_restore(render_data_context& rend_data_ctx, int32_t eye) {
    const int32_t w = pv_cache_w[pv_front], h = pv_cache_h[pv_front];
    const int32_t sw = rctx->screen_width, sh = rctx->screen_height;
    const GLuint screen = rctx->screen_buffer.get_fb();
    pv_blit(pv_cache_fb[pv_front][eye], screen, w, h, sw, sh);
    // the HUD is drawn into the screen buffer next: leave it bound, as the render state is told
    glBindFramebuffer(GL_FRAMEBUFFER, screen);
    rend_data_ctx.state.bind_framebuffer(screen);
}

#ifdef DIVAVK
// ---- threaded PV (needs DivaVK's shared contexts; the plain-OpenGL DivaGL.dva has stubs below)

/* Threaded PV (a host with schedule version 2: the DivaRecomp runtime on Quest).
   The PV picture is drawn by a thread of its own, with a GL context of its own (DivaVK's
   shared context, submitting to a second, lower-priority GPU queue when there is one). The
   game thread draws only the HUD, every frame, over the newest *finished* PV picture, and
   never waits for the PV thread or its GPU work:

     game thread   HUD over A | HUD over A | HUD over A | HUD over B | HUD over B ...
     PV thread     [ drawing picture into B .......... ]  [ drawing into A ...
                                          finished on the GPU ^ (checked, never waited for)

   Handoff (game thread, only while the PV thread is idle, on the frame the runtime updated
   the PV's tasks and rebuilt their draw lists): the draw lists and their arena, the screen
   and reflection lists, the PV's sprite lists (1 and 2) and the scene pointers of the
   particle managers move to the PV thread's own copies, and the camera is copied. The game's
   lists are then refreshed and filled as usual. The PV thread has its own render context
   (buffers, targets, cached GL state), draws shadows, reflections, 3D and post-process per
   eye, keeps the result in its slot, submits and publishes the ticket. The game thread shows
   the slot once the ticket is done on the GPU.
   Pipelined: the next picture is handed off as soon as the PV thread is free on the CPU (and a
   slot is free), not when the last one is shown, so recording picture N+1 overlaps the GPU
   drawing picture N. At most two pictures are out (one on the GPU, one being recorded); the
   runtime is told (pv_busy) only while neither a thread nor a slot is free.
   The PV thread's GPU work is ordered after the game frames that last showed the slot it
   overwrites (a GPU-side wait of the PV queue on the game's queue, never the reverse). */
namespace {
    struct pv_thread_t {
        std::thread thread;
        std::mutex m;
        std::condition_variable cv;
        bool job = false;                       // under m
        bool quit = false;                      // under m
        bool tried = false;                     // game thread: creation attempted
        dvk_context ctx = nullptr;
        mdl::DispManager* dm = nullptr;
        std::atomic<render_context*> rctx{ nullptr };
        std::atomic<bool> ready{ false };
        // the job, written by the game thread before it's posted
        int32_t slot = 0;
        int32_t eyes = 1;
        uint64_t after_main = 0;
        uint32_t number = 0;
        cam_data cam;
        // results, published by the PV thread
        std::atomic<uint32_t> recorded{ 0 };    // number of the picture last submitted
        std::atomic<uint64_t> cores{ 0 };       // CPU cores it ran pictures on (bits), for the log
        std::atomic<bool> cpu_busy{ false };    // a picture handed off, the PV thread still recording it
        std::atomic<int32_t> out{ 0 };          // pictures handed off and not shown yet (0..2)
        // per picture out, by number % PV_SLOTS
        struct pic_t {
            std::atomic<uint64_t> ticket{ 0 };  // PV thread: its ticket once submitted (0 before)
            std::atomic<double> cpu_ms{ 0.0 };  // PV thread: its thread time
            int32_t slot = 0;                   // game thread, at the handoff
            uint32_t number = 0;
            LARGE_INTEGER handoff = {};
        } pic[PV_SLOTS];
    };
    pv_thread_t* pvt = nullptr;
    std::atomic<uint32_t> pv_song_first{ 1 };   // the song's first picture number, for the log
    uint32_t pv_next_show = 1;                  // game thread: the oldest picture out
    uint32_t pv_shown_number = 0;
    uint32_t pv_frame_no = 0;
    int32_t pv_trace_left = 0;
    struct {
        int32_t frames, late, pictures;
        double_t worst_ms, sum_ms, pic_ms_sum, pic_ms_max, pv_cpu_ms, handoff_ms;
        LARGE_INTEGER start;
    } pv_ts = {};
}

void pv_camera_snapshot_take();   // camera.cpp

/* The shadow state for the PV picture being drawn. The game sets parts of it every frame (each
   character's shadow distance base as objects are entered), so on the PV thread the passes use
   a copy taken at the handoff: otherwise the right eye, drawn a game frame or two after the
   left, used a shadow state the left eye's shadow maps weren't made with. */
alignas(16) static uint8_t pv_shadow_copy[sizeof(Shadow)];
static bool pv_shadow_copied = false;

Shadow* render_shadow() {
    if (pv_render_thread_flag && pv_shadow_copied)
        return (Shadow*)pv_shadow_copy;
    return render_manager.shadow_ptr;
}

static void pv_shadow_snapshot_take() {
    if (render_manager.shadow_ptr) {
        memcpy(pv_shadow_copy, render_manager.shadow_ptr, sizeof(Shadow));
        pv_shadow_copied = true;
    }
}

bool pv_threaded_on() {
    return pv_sync && pv_sync->version >= 2 && pv_sync->async;
}

// What render_all does at the start of every frame (and, in stereo, what the HUD's sprite
// pass and the eye composite left between the eyes): DivaGL's cached GL state back in step
// with GL, and the baseline render state the passes expect (colour writes on, depth test
// LEQUAL with writes, cull face on, no blending). Without it a pass that left colour writes off
// carried into the next picture or eye: objects drew depth only, as black holes. (Not
// framebuffer 0's contents: that's the window's image, the game thread's.)
static void pv_thread_baseline_state() {
    gl_state.get();
    render_data_context rend_data_ctx(GL_REND_STATE_PRE_3D);
    rend_data_ctx.state.get();
    rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    rend_data_ctx.state.enable_depth_test();
    rend_data_ctx.state.set_depth_func(GL_LEQUAL);
    rend_data_ctx.state.set_depth_mask(GL_TRUE);
    rend_data_ctx.state.enable_cull_face();
    rend_data_ctx.state.disable_blend();
    rend_data_ctx.state.bind_framebuffer(0);
    rend_data_ctx.state.finish();
}

static void pv_thread_draw() {
    pv_thread_t& t = *pvt;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    render_context* r = rctx;   // this thread's
    rndr::Render* rend = render_manager.render;

    // its GPU work goes after the game frames that last showed the slot it overwrites
    dvk_ticket_use(dvk_main_context(), t.after_main);
    dvk_dump_begin();   // a frame dump asked for (DivaLinux_dump_request): this picture too

    // what this picture has to draw, for the log
    size_t objs = 0, obj_opaque = 0, obj_transp = 0, obj_transl = 0, obj_screen = 0;
    for (int32_t i = 0; i < mdl::OBJ_TYPE_MAX; i++)
        objs += disp_manager->obj[i].size();
    obj_opaque = disp_manager->obj[mdl::OBJ_TYPE_OPAQUE].size();
    obj_transp = disp_manager->obj[mdl::OBJ_TYPE_TRANSPARENT].size();
    obj_transl = disp_manager->obj[mdl::OBJ_TYPE_TRANSLUCENT].size();
    for (int32_t i = 0; i < mdl::OBJ_TYPE_SCREEN_MAX; i++)
        obj_screen += r->obj_screen[i].size();
    const size_t spr1 = sprite_manager_reqlist_count(1), spr2 = sprite_manager_reqlist_count(2);
    dvk_stats st0 = {};
    dvk_context_get_stats(t.ctx, &st0);
    r->ctrl();
    r->render_manager_cam = t.cam;

    pv_thread_baseline_state();

    {
        // what the game thread's rndpass_pre_proc did for the PV, now for this picture only
        render_data_context rend_data_ctx(GL_REND_STATE_PRE_3D);
        rend_data_ctx.state.get();
        rend_data_ctx.state.begin_event("PV thread: pre_proc");
        rend->pre_proc(rend_data_ctx);
        Glitter::glt_particle_manager->CalcDisp();
        if (Glitter::glt_particle_manager_x)
            Glitter::glt_particle_manager_x->CalcDisp();
        r->pre_proc();
        rend_data_ctx.state.end_event();
    }
    sprite_manager_pre_draw();
    mdl::etc_obj_manager->pre_draw();
    const double_t ms_pre = pv_ms_since(t0);
    double_t ms_eye[2] = {};

    // 3D stereo: the eye loop below
    const bool stereo = t.eyes == 2;
    const cam_data center_cam = r->render_manager_cam;
    const int32_t taa = rend->taa;
    const float_t taa_blend = rend->taa_blend;
    const int32_t exposure_history_counter = rend->exposure_history_counter;
    const int32_t exposure_query_index = rend->exposure_query_index;
    const int32_t lens_flare_query_index = rend->lens_flare_query_index;
    if (stereo) {
        rend->taa = 0;
        rend->taa_blend = 1.0f;
    }
    for (int32_t eye = 0; eye < t.eyes; eye++) {
        LARGE_INTEGER te;
        QueryPerformanceCounter(&te);
        if (eye)
            pv_thread_baseline_state();   // as the eye before it left things, the HUD passes didn't run here
        if (stereo) {
            const int32_t sign = eye ? 1 : -1;
            if (eye) {
                rend->exposure_history_counter = exposure_history_counter;
                rend->exposure_query_index = exposure_query_index;
                rend->lens_flare_query_index = lens_flare_query_index;
            }
            stereo_make_eye_cam(center_cam, r->render_manager_cam, sign);
            stereo_eye_begin(sign);
        }
        {
            render_data_context rend_data_ctx(GL_REND_STATE_PRE_3D);
            rend_data_ctx.state.get();
            rend_data_ctx.state.begin_event("PV thread: pre3d");
            for (int32_t i = rndr::RND_PASSID_SHADOW; i <= rndr::RND_PASSID_CLEAR; i++)
                render_manager.render_single_pass(rend_data_ctx, (rndr::RenderPassID)i);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
        }
        {
            render_data_context rend_data_ctx(GL_REND_STATE_3D);
            rend_data_ctx.state.get();
            rend_data_ctx.state.begin_event("PV thread: 3d");
            for (int32_t i = rndr::RND_PASSID_PRE_SPRITE; i <= rndr::RND_PASSID_3D; i++)
                render_manager.render_single_pass(rend_data_ctx, (rndr::RenderPassID)i);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
        }
        {
            render_data_context rend_data_ctx(GL_REND_STATE_2D);
            rend_data_ctx.state.get();
            rend_data_ctx.state.begin_event("PV thread: post-process");
            render_manager.render_single_pass(rend_data_ctx, rndr::RND_PASSID_POST_PROCESS);
            pv_cache_save(rend_data_ctx, eye);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
        }
        stereo_eye_reset();
        ms_eye[eye] = pv_ms_since(te);
    }
    r->render_manager_cam = center_cam;
    rend->taa = taa;
    rend->taa_blend = taa_blend;

    // what the game thread's rndpass_post_proc did for the PV
    rend->post_proc();
    r->post_proc();
    mdl::etc_obj_manager->post_draw();
    sprite_manager_post_draw();
    sprite_manager_thread_clear_lists();
    {
        // and what render_all does at its end
        render_data_context rend_data_ctx(GL_REND_STATE_POST_2D);
        rend_data_ctx.state.get();
        rend_data_ctx.state.bind_vertex_array(0);
        rend_data_ctx.state.disable_primitive_restart();
        rend_data_ctx.state.bind_uniform_buffer(0);
        for (int32_t i = 0; i < 5; i++)
            rend_data_ctx.state.bind_uniform_buffer_base(i, 0);
        rend_data_ctx.state.bind_shader_storage_buffer(0);
        rend_data_ctx.state.bind_shader_storage_buffer_base(0, 0);
        rend_data_ctx.state.finish();
    }

    dvk_dump_end();
    const uint64_t ticket = dvk_flush();
    pv_thread_t::pic_t& pic = t.pic[t.number % PV_SLOTS];
    pic.cpu_ms.store(pv_ms_since(t0));
    {
        const DWORD core = GetCurrentProcessorNumber();
        if (core < 64)
            t.cores.fetch_or(1ull << core);
    }
    {
        // the first pictures of each song, then one every 10 seconds
        static LARGE_INTEGER last_log = {};
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const bool first = t.number < pv_song_first.load() + 5;
        if (first || (double_t)(now.QuadPart - last_log.QuadPart) / (double_t)pv_qpf.QuadPart >= 10.0) {
            last_log = now;
            dvk_stats st1 = {};
            dvk_context_get_stats(t.ctx, &st1);
            char buf[512];
            snprintf(buf, sizeof(buf), "[DivaLinux] PV thread picture %u: %zu draw-list entries (opaque %zu, transparent %zu, "
                "translucent %zu, screen %zu), sprites: surface %zu, background %zu; %llu draws in %llu render passes, "
                "%d eye(s), %.1f ms (setup %.1f, eyes %.1f + %.1f, rest %.1f)\n", t.number, objs, obj_opaque, obj_transp, obj_transl,
                obj_screen, spr1, spr2,
                (unsigned long long)(st1.draws - st0.draws), (unsigned long long)(st1.render_passes - st0.render_passes),
                t.eyes, pic.cpu_ms.load(), ms_pre, ms_eye[0], ms_eye[1], pic.cpu_ms.load() - ms_pre - ms_eye[0] - ms_eye[1]);
            OutputDebugStringA(buf);
        }
    }
    pic.ticket.store(ticket);
    t.recorded.store(t.number);
}

static gl_state_struct pv_gl_state_shared = {};

static void pv_thread_main() {
    pv_thread_t& t = *pvt;
    // Ask for the fast cores: the DivaRecomp runtime keeps a thread raising itself to HIGHEST on
    // them, at a higher host priority (Android otherwise parks a thread that sleeps between
    // pictures on an efficiency core). Elsewhere it's just a priority.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    dvk_make_current(t.ctx);
    pv_render_thread_flag = true;
    gl_state_thread = &pv_gl_state_shared;
    gl_rend_state_thread_base = GL_REND_STATE_COUNT;
    gl_state.get();
    render_context* r = new render_context;    // its own buffers and vertex arrays, on its context
    rctx_thread = r;
    r->init_pv_thread(rctx_main);
    disp_manager_thread = t.dm;
    sprite_manager_thread_init();
    sprite_manager_thread_register();
    Glitter::UsePvScenes(true);
    dvk_flush();
    t.rctx.store(r);
    t.ready.store(true);
    OutputDebugStringA("[DivaLinux] PV thread ready: the PV is drawn on its own thread and GL context\n");

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(t.m);
            t.cv.wait(lock, [&] { return t.job || t.quit; });
            if (t.quit)
                break;
        }
        if (pv_sync)
            pv_sync->pv_drawing = 1;
        pv_thread_draw();
        {
            std::lock_guard<std::mutex> lock(t.m);
            t.job = false;
        }
        if (pv_sync)
            pv_sync->pv_drawing = 0;
        // free for the next picture now (its GPU work may still be running): the runtime
        // may update the PV again from its next frame if a slot is free too
        // (pv_busy first: the game thread can hand off, and set it, only once cpu_busy clears)
        if (pv_sync && t.out.load() < 2)
            pv_sync->pv_busy = 0;
        t.cpu_busy.store(false);
    }

    sprite_manager_thread_free();
    r->free_pv_thread();
    dvk_flush();
    rctx_thread = 0;
    gl_state_thread = 0;
    dvk_make_current(nullptr);
}

static bool pv_thread_start() {
    if (pvt)
        return pvt->ready.load();
    static bool tried = false;
    if (tried)
        return false;
    tried = true;
    dvk_context ctx = dvk_context_create_shared();
    if (!ctx) {
        OutputDebugStringA("[DivaLinux] PV thread: not possible here (no second GL context); the PV stays on the game thread\n");
        return false;
    }
    pvt = new pv_thread_t;
    pvt->ctx = ctx;
    pvt->dm = disp_manager_thread_create();
    // the PV thread's GL work may use what the game thread made so far
    dvk_flush();
    pvt->thread = std::thread(pv_thread_main);
    /* Wait for its setup (its render context's buffers, made through the game's texture
       manager, which has no lock): the game thread making textures at the same time could
       leave two of them sharing one. A few milliseconds, once. */
    for (int32_t i = 0; i < 5000 && !pvt->ready.load(); i++)
        Sleep(1);
    return false;   // used from the next frame
}

void pv_thread_stop() {
    if (!pvt)
        return;
    {
        std::lock_guard<std::mutex> lock(pvt->m);
        pvt->quit = true;
    }
    pvt->cv.notify_one();
    if (pvt->thread.joinable())
        pvt->thread.join();
    dvk_context_destroy(pvt->ctx);
    pvt = nullptr;   // (the thread's draw-list copy stays: its arena may be the game's by now)
}

static void pv_threaded_log(double_t elapsed_s) {
    if (pv_ts.frames <= 0)
        return;
    uint32_t gpu_frames = 0;
    const double_t gpu_ms = pvt ? dvk_context_take_gpu_ms(pvt->ctx, &gpu_frames) : 0.0;
    uint32_t hud_gpu_frames = 0;
    const double_t hud_gpu_ms = dvk_context_take_gpu_ms(nullptr, &hud_gpu_frames);
    char cores[96] = "-";
    if (pvt) {
        const uint64_t m = pvt->cores.exchange(0);
        size_t o = 0;
        for (int32_t i = 0; i < 64 && o + 4 < sizeof(cores); i++)
            if (m & (1ull << i))
                o += snprintf(cores + o, sizeof(cores) - o, "%s%d", o ? "," : "", i);
    }
    const double_t pv_wait_ms = pvt ? dvk_context_take_wait_ms(pvt->ctx) : 0.0;
    char buf[800];
    snprintf(buf, sizeof(buf),
        "[DivaLinux] threaded PV: HUD %.1f fps (avg %.1f ms, worst %.1f ms, %d frames over the display time); "
        "the game thread waited for the PV 0 times (handoffs %.2f ms each). PV %.1f pictures/s, %.1f ms each "
        "from handoff to shown (longest %.1f), PV thread %.1f ms per picture. GPU: HUD queue %.1f ms per frame, "
        "PV queue %.1f ms per picture; PV thread on cores %s, waiting for its own GPU work %.1f ms per picture\n",
        pv_ts.frames / elapsed_s, pv_ts.sum_ms / pv_ts.frames, pv_ts.worst_ms, pv_ts.late,
        pv_ts.pictures ? pv_ts.handoff_ms / pv_ts.pictures : 0.0,
        pv_ts.pictures / elapsed_s, pv_ts.pictures ? pv_ts.pic_ms_sum / pv_ts.pictures : 0.0, pv_ts.pic_ms_max,
        pv_ts.pictures ? pv_ts.pv_cpu_ms / pv_ts.pictures : 0.0,
        hud_gpu_frames ? hud_gpu_ms / hud_gpu_frames : 0.0, gpu_frames ? gpu_ms / gpu_frames : 0.0, cores,
        pv_ts.pictures ? pv_wait_ms / pv_ts.pictures : 0.0);
    OutputDebugStringA(buf);
}

/* Game thread, once per frame in threaded mode, before the HUD is drawn: show a finished
   picture, hand off the next one. Never waits. */
void pv_threaded_frame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    pv_frame_no++;
    if (!pv_threaded_was) {
        pv_threaded_was = true;
        pv_trace_left = 15;
        // nothing from before this song: black until the first picture is done
        memset(pv_cache_valid, 0, sizeof(pv_cache_valid));
        pv_song_first = pvt ? pvt->number + 1 : 1;
        pv_next_show = pvt ? pvt->number + 1 : 1;
        OutputDebugStringA("[DivaLinux] threaded PV on: the HUD every frame over the newest finished PV picture "
            "(slots A, B and C), the PV on its own thread at whatever rate it manages, pipelined (the next picture "
            "recorded while the GPU draws the last)\n");
    }
    const double_t period = pv_sync->period_us ? pv_sync->period_us / 1000.0 : 13.9;
    if (!pv_ts.start.QuadPart)
        pv_ts.start = now;
    if (pv_frame_ms > 0.0) {
        pv_ts.frames++;
        pv_ts.sum_ms += pv_frame_ms;
        pv_ts.worst_ms = max_def(pv_ts.worst_ms, pv_frame_ms);
        if (pv_frame_ms > period * 1.10)
            pv_ts.late++;
    }
    const double_t elapsed = (double_t)(now.QuadPart - pv_ts.start.QuadPart) / (double_t)pv_qpf.QuadPart;
    if (elapsed >= 10.0) {
        pv_threaded_log(elapsed);
        pv_ts = {};
        pv_ts.start = now;
        pv_trace_left = 10;
    }

    char trace[256] = "";
    const bool ready = pv_thread_start();

    // pictures finished on the GPU, oldest first: the newest of them shown from this frame
    while (ready && pvt->out.load() > 0) {
        pv_thread_t::pic_t& p = pvt->pic[pv_next_show % PV_SLOTS];
        const uint64_t ticket = p.ticket.load();
        if (!ticket || !dvk_ticket_done(pvt->ctx, ticket))
            break;
        dvk_ticket_use(pvt->ctx, ticket);   // (finished: no GPU wait)
        pv_front = p.slot;
        pv_shown_number = p.number;
        pv_next_show++;
        pvt->out.fetch_sub(1);
        const double_t ms = pv_ms_since(p.handoff);
        pv_ts.pictures++;
        pv_ts.pic_ms_sum += ms;
        pv_ts.pic_ms_max = max_def(pv_ts.pic_ms_max, ms);
        pv_ts.pv_cpu_ms += p.cpu_ms.load();
        pv_stat.full++;
        pv_sync->pv_done++;
        snprintf(trace + strlen(trace), sizeof(trace) - strlen(trace),
            "picture %u finished (%.1f ms after its handoff, PV thread %.1f ms) -> %c shown from now; ",
            pv_shown_number, ms, p.cpu_ms.load(), 'A' + pv_front);
    }
    // the thread and a slot free: the runtime may update the PV (again) from its next frame
    const bool can_hand_off = ready && !pvt->cpu_busy.load() && pvt->out.load() < 2;
    if (can_hand_off)
        pv_sync->pv_busy = 0;

    // the PV's tasks rebuilt their lists this frame: hand them to the PV thread
    static uint32_t seq_seen = 0;
    if (can_hand_off && pv_sync->disp_seq != seq_seen) {
        seq_seen = pv_sync->disp_seq;
        LARGE_INTEGER h0;
        QueryPerformanceCounter(&h0);
        pv_thread_t& t = *pvt;
        t.number++;
        // a slot neither shown (when there's a picture to show) nor used by a picture still out
        {
            // The shown slot is always off limits, whether or not both its eyes are marked
            // saved: with it left out when they weren't, the PV thread drew into the picture
            // being shown. Frame dumps from the Odin 3 caught the game thread copying slot A
            // while the PV thread drew into slot A: Qualcomm's driver showed it as the right
            // eye with one triangle black (the copy reading the picture half redrawn), Turnip
            // as whole frames of black PV now and then.
            bool used[PV_SLOTS] = {};
            used[pv_front] = true;
            for (uint32_t n = pv_next_show; n < t.number; n++)
                used[t.pic[n % PV_SLOTS].slot] = true;
            t.slot = 0;
            while (t.slot < PV_SLOTS && used[t.slot])
                t.slot++;
            if (t.slot == PV_SLOTS) {   // can't happen with at most one picture out at a handoff
                static bool said;
                if (!said) {
                    said = true;
                    OutputDebugStringA("[DivaLinux] threaded PV: no free picture slot at a handoff; reusing the oldest\n");
                }
                t.slot = t.pic[pv_next_show % PV_SLOTS].slot;
            }
        }
        pv_thread_t::pic_t& p = t.pic[t.number % PV_SLOTS];
        p.ticket.store(0);
        p.cpu_ms.store(0.0);
        p.slot = t.slot;
        p.number = t.number;
        QueryPerformanceCounter(&p.handoff);
        pv_cache_valid[t.slot][0] = pv_cache_valid[t.slot][1] = false;
        pv_save_slot = t.slot;
        t.eyes = stereo_enabled() ? 2 : 1;   // 3D stereo
        t.cam = rctx_main->render_manager_cam;
        pv_camera_snapshot_take();   // what the game's camera getters tell the PV thread
        pv_shadow_snapshot_take();   // and the shadow state it draws with
        stereo_snapshot_for_pv();    // and the eye separation
        // Everything the game thread has recorded so far goes to the GPU now, and the
        // picture's GPU work goes after it: this frame's uploads (character vertex data,
        // textures) included, not just the previous frames'. (A submit, not a wait.)
        t.after_main = dvk_flush();
        disp_manager_handoff(t.dm, t.rctx.load());
        sprite_manager_handoff_pv_lists();
        Glitter::SnapshotScenesForPv();
        t.cpu_busy.store(true);
        t.out.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(t.m);
            t.job = true;
        }
        t.cv.notify_one();
        // busy until the PV thread is done recording it (it clears this itself) and a slot is free
        pv_sync->pv_busy = 1;
        pv_ts.handoff_ms += pv_ms_since(h0);
        snprintf(trace + strlen(trace), sizeof(trace) - strlen(trace), "picture %u handed to the PV thread (into %c); ",
            t.number, 'A' + t.slot);
    }

    if (pv_trace_left > 0) {
        pv_trace_left--;
        char buf[512];
        snprintf(buf, sizeof(buf), "[DivaLinux] threaded PV frame %u (%.1f ms): HUD drawn over %c (picture %u), "
            "waited for the PV 0 ms; %s%s, %d picture(s) out\n", pv_frame_no, pv_frame_ms, 'A' + pv_front, pv_shown_number, trace,
            !ready ? "PV thread starting" : pvt->cpu_busy.load() ? "PV thread recording" : "PV thread free",
            ready ? pvt->out.load() : 0);
        OutputDebugStringA(buf);
    }
}

/* The game thread's share of the per-frame render pre/post-process: in threaded mode the PV
   thread does the PV's part (hook.cpp). */
bool pv_threaded_active() {
    return pvt && pvt->ready.load() && pv_threaded_on();
}

/* Game thread, before something a PV picture may read is freed. The runtime runs the tasks
   that make and free what the PV draws (Glitter, the stage effects) only while the PV thread
   is idle, but the song script (TaskPvGame, on the HUD side, every frame) changes stages
   too, and a stage change unloads the old stage's effect group (its Glitter scenes), object
   sets and textures right away. A picture being recorded would then read freed memory. So
   the frees wait for the PV thread to finish its picture first. They're rare (a stage
   change, an unload), and the wait is at most one picture's recording. */
void pv_thread_wait_idle(const char* what) {
    if (!pvt || pv_render_thread_flag || !pvt->cpu_busy.load())
        return;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    while (pvt->cpu_busy.load())
        Sleep(0);
    static int32_t said = 0;
    if (said < 20) {
        said++;
        char buf[200];
        snprintf(buf, sizeof(buf), "[DivaLinux] threaded PV: freeing %s waited %.1f ms for the PV thread's picture%s\n",
            what, pv_ms_since(t0), said == 20 ? " (not logged again)" : "");
        OutputDebugStringA(buf);
    }
}

/* Game thread, the first frame after a song with threaded PV: the PV thread may still be
   drawing the last picture with the shared post-process state and objects; the game thread
   draws the PV itself from now on, so let that finish first (not during play: this is the
   song's end). */
void pv_threaded_leave() {
    if (pvt)
        while (pvt->cpu_busy.load())
            Sleep(0);
    if (pvt) {
        pvt->out.store(0);
        pv_next_show = pvt->number + 1;
    }
    pv_threaded_was = false;
    pv_save_slot = pv_front;
    if (pv_sync)
        pv_sync->pv_busy = 0;
}

#else   // !DIVAVK: no threaded PV in the plain-OpenGL build

Shadow* render_shadow() {
    return render_manager.shadow_ptr;
}

bool pv_threaded_on() {
    return false;
}

bool pv_threaded_active() {
    return false;
}

void pv_threaded_frame() {
}

void pv_threaded_leave() {
    pv_threaded_was = false;
}

void pv_thread_stop() {
}

void pv_thread_wait_idle(const char* what) {
}

#endif
