/*
    by korenkonder
    GitHub/GitLab: korenkonder
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

struct light_data_color {
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
};

RenderTexture* litproj_shadow = (RenderTexture*)0x0000000141194DA0;
RenderTexture* litproj_texture = (RenderTexture*)0x0000000141194E00;
rndr::RenderManager& render_manager = *(rndr::RenderManager*)0x00000001411AD320;

void apply_esm_filter(render_data_context& rend_data_ctx,
    RenderTexture* dst, RenderTexture* buf, RenderTexture* src,
    float_t sigma, float_t far_texel_offset, bool enable_lit_proj);
void apply_esm_min_filter(render_data_context& rend_data_ctx,
    RenderTexture* dst, RenderTexture* buf, RenderTexture* src);
static bool litproj_clear(render_data_context& rend_data_ctx, cam_data& cam);
static bool litproj_set(render_data_context& rend_data_ctx, cam_data& cam);
static bool litproj_set_mat(render_data_context& rend_data_ctx, cam_data& cam, bool set_mat);
static int32_t draw_pass_3d_get_translucent_count();
extern void draw_pass_3d_translucent(render_data_context& rend_data_ctx,
    mdl::ObjType opaque, mdl::ObjType transparent, mdl::ObjType translucent, cam_data& cam);
extern void draw_pass_3d_translucent(render_data_context& rend_data_ctx,
    mdl::ObjTypeScreen opaque, mdl::ObjTypeScreen transparent, mdl::ObjTypeScreen translucent, cam_data& cam);
static int32_t draw_pass_3d_translucent_count_layers(int32_t* alpha_array,
    mdl::ObjType opaque, mdl::ObjType transparent, mdl::ObjType translucent, cam_data& cam);
static int32_t draw_pass_3d_translucent_count_layers(int32_t* alpha_array,
    mdl::ObjTypeScreen opaque, mdl::ObjTypeScreen transparent, mdl::ObjTypeScreen translucent, cam_data& cam);
static void draw_pass_3d_translucent_has_objects(bool* arr, mdl::ObjType type, cam_data& cam);
static void draw_pass_3d_translucent_has_objects(bool* arr, mdl::ObjTypeScreen type, cam_data& cam);

static void draw_pass_reflect_full(render_data_context& rend_data_ctx, rndr::RenderManager& render_manager);

static bool draw_pass_reflect_get_obj_reflect_surface();
static bool draw_pass_reflect_get_obj_reflect_surface(mdl::ObjType type);

void apply_blur_filter_sub(render_data_context& rend_data_ctx, RenderTexture* dst, RenderTexture* src,
    ImgfBoxSampl filter, const vec2 res_scale, const vec4 scale, const vec4 offset);

static void fog_set(render_data_context& rend_data_ctx, fog_id id);

static void light_get_direction_from_position(vec4* pos_dir, const light_data* light, bool force = false);
static void light_get_light_color(const light_data* light, light_data_color& value, light_id id);

static void lighting_set(render_data_context& rend_data_ctx, light_set_id set_id);

static void render_manager_free_render_textures();
static void render_manager_init_render_textures(int32_t multisample);

static void set_reflect_mat(render_data_context& rend_data_ctx, cam_data& cam);

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
static RenderTexture* pv_cache[2][2] = {};    // [slot][eye]: eye 0 mono or left, 1 right
static bool pv_cache_valid[2][2] = {};
static int32_t pv_front = 0;                  // slot put back under the HUD
static int32_t pv_save_slot = 0;              // slot a finished PV picture goes to (the other one in async mode)
static int32_t pv_cache_w[2] = {};           // per slot (written by whoever draws into it)
static int32_t pv_cache_h[2] = {};
static bool pv_skip = false;                  // this frame reuses the kept PV picture
static bool pv_threaded_was = false;          // threaded PV (pv_threaded_frame) was on last frame
static int32_t pv_since_full = 0;             // frames since the PV was last drawn

/* frame timing, for auto mode and the perf log */
static LARGE_INTEGER pv_qpf = {};
static LARGE_INTEGER pv_last = {};
static double_t pv_frame_ms = 0.0;            // last frame's duration (start to start)
static double_t pv_vsync_ms = 0.0;            // shortest recent frame: about the display's frame time
static bool pv_last_was_full = false;
static struct { int32_t frames, full; double_t sum_ms, worst_ms, render_ms, pv_ms, hud_ms; LARGE_INTEGER start; } pv_stat = {};

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
static pv_sync_struct* pv_sync = 0;
static bool pv_sync_checked = false;

static double_t pv_ms_since(const LARGE_INTEGER& t0) {
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
static void pv_frame_begin() {
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
                if (s->version >= 2)
                    s->async_ok = 1;
                OutputDebugStringA(s->version >= 2
                    ? "[DivaLinux] PV frame skip: following the host's PV schedule (async PV supported)\n"
                    : "[DivaLinux] PV frame skip: following the host's PV schedule\n");
            }
        }
    }

    const bool have = pv_cache_valid[pv_front][0] && (!stereo_enabled() || pv_cache_valid[pv_front][1]);
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

static void pv_cache_save(render_data_context& rend_data_ctx, int32_t eye) {
    if ((!sv_pv_frame_skip && !pv_sync) || !rctx->screen_buffer.m_txhd_color)
        return;
    const int32_t w = rctx->screen_buffer.get_width();
    const int32_t h = rctx->screen_buffer.get_height();
    if (w <= 0 || h <= 0)
        return;
    const int32_t slot = pv_save_slot;
    if (w != pv_cache_w[slot] || h != pv_cache_h[slot]) {
        for (RenderTexture* i : pv_cache[slot])
            if (i)
                i->destroy();
        pv_cache_valid[slot][0] = pv_cache_valid[slot][1] = false;
        pv_cache_w[slot] = w;
        pv_cache_h[slot] = h;
    }
    RenderTexture*& rt = pv_cache[slot][eye];
    if (!rt)
        rt = new RenderTexture;
    if (!rt->m_txhd_color)
        rt->create_texture(w, h, 0, GL_RGBA8, 0);
    fbo_blit(rend_data_ctx.state, rctx->screen_buffer.get_fb(), rt->get_fb(),
        0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    pv_cache_valid[slot][eye] = true;
}

/* HUD layer ([vr] hud_layer): in a headset, the HUD (sprite list 0) isn't drawn into the
   picture but once per frame into a transparent target of its own, which DivaVK shows as a
   separate layer just in front of the PV. The PV picture (both eyes with SBS stereo) goes to
   the window/headset without it, so the HUD stands out from the 3D, and it can later update
   on its own. */
static RenderTexture* hud_layer_rt = 0;
static bool hud_layer_active = false;   // this frame
static bool hud_layer_was = false;
extern bool sprite_hud_layer_blend;

static bool hud_layer_wanted() {
#ifdef DIVAVK
    return sv_vr_hud_layer && dvk_xr_active();
#else
    return false;
#endif
}

static void draw_hud_layer() {
#ifdef DIVAVK
    // The HUD layer is shown at the picture's shape: with SBS stereo the window can be 32:9
    // (two 16:9 eyes), but the HUD is one 16:9 picture over both.
    const int32_t h = rctx->screen_height;
    int32_t w = rctx->screen_width;
    if (stereo_enabled() && sv_stereo_content_aspect > 0 && h > 0)
        w = (int32_t)(h * (sv_stereo_content_aspect / 1000.0) + 0.5);
    if (w <= 0 || h <= 0)
        return;
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
        rend_data_ctx.state.enable_cull_face();
        rend_data_ctx.state.disable_blend();
        rend_data_ctx.state.enable_depth_test();
        shader::unbind(rend_data_ctx.state);
    }
    hud_layer_rt->end_render(rend_data_ctx.state);
    rend_data_ctx.state.bind_framebuffer(0);
    rend_data_ctx.state.end_event();
    rend_data_ctx.state.finish();
    dvk_xr_set_hud_texture(hud_layer_rt->get_texture_glid());
#endif
}

static void pv_cache_restore(render_data_context& rend_data_ctx, int32_t eye) {
    const int32_t w = pv_cache_w[pv_front], h = pv_cache_h[pv_front];
    const int32_t sw = rctx->screen_width, sh = rctx->screen_height;
    fbo_blit(rend_data_ctx.state, pv_cache[pv_front][eye]->get_fb(), rctx->screen_buffer.get_fb(),
        0, 0, w, h, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, w == sw && h == sh ? GL_NEAREST : GL_LINEAR);
}

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
   the slot once the ticket is done on the GPU; the next picture is handed off after that.
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
        std::atomic<uint64_t> ticket{ 0 };      // the picture's ticket once submitted
        std::atomic<uint32_t> recorded{ 0 };    // number of the picture last submitted
        std::atomic<double> cpu_ms{ 0.0 };      // its thread time for the last picture
    };
    pv_thread_t* pvt = nullptr;
    std::atomic<uint32_t> pv_song_first{ 1 };   // the song's first picture number, for the log
    bool pv_job_out = false;                    // game thread: handed off, not shown yet
    uint32_t pv_shown_number = 0;
    uint32_t pv_frame_no = 0;
    int32_t pv_trace_left = 0;
    LARGE_INTEGER pv_handoff_time = {};
    struct {
        int32_t frames, late, pictures;
        double_t worst_ms, sum_ms, pic_ms_sum, pic_ms_max, pv_cpu_ms, handoff_ms;
        LARGE_INTEGER start;
    } pv_ts = {};
}

void pv_camera_snapshot_take();   // camera.cpp

static bool pv_threaded_on() {
    return pv_sync && pv_sync->version >= 2 && pv_sync->async;
}

static bool pv_front_valid() {
    return pv_cache_valid[pv_front][0] && (!stereo_enabled() || pv_cache_valid[pv_front][1]);
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
        dvk_flush();   // submitted in pieces, so the GPU can fit the HUD's frames in between
        {
            render_data_context rend_data_ctx(GL_REND_STATE_3D);
            rend_data_ctx.state.get();
            rend_data_ctx.state.begin_event("PV thread: 3d");
            for (int32_t i = rndr::RND_PASSID_PRE_SPRITE; i <= rndr::RND_PASSID_3D; i++)
                render_manager.render_single_pass(rend_data_ctx, (rndr::RenderPassID)i);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
        }
        dvk_flush();
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
        render_data_context rend_data_ctx(GL_REND_STATE_POST_2D);
        rend_data_ctx.state.finish();
    }

    dvk_dump_end();
    const uint64_t ticket = dvk_flush();
    t.cpu_ms.store(pv_ms_since(t0));
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
                "%d eye(s), %.1f ms\n", t.number, objs, obj_opaque, obj_transp, obj_transl, obj_screen, spr1, spr2,
                (unsigned long long)(st1.draws - st0.draws), (unsigned long long)(st1.render_passes - st0.render_passes),
                t.eyes, t.cpu_ms.load());
            OutputDebugStringA(buf);
        }
    }
    t.ticket.store(ticket);
    t.recorded.store(t.number);
}

/* DIVA_PV_THREAD_INLINE=1 (a test): the game thread draws each PV picture itself, right at
   its handoff, as if it were the PV thread (its context, render context, lists and camera).
   Everything is the same except the drawing happens on the game thread, in the handoff frame:
   if the picture is right this way and wrong on the thread, the cause is something the game
   thread changes while the PV thread draws. */
static bool pv_inline_mode() {
    static int v = -1;
    if (v < 0) {
        char buf[8];
        const DWORD n = GetEnvironmentVariableA("DIVA_PV_THREAD_INLINE", buf, sizeof(buf));
        v = n && n < sizeof(buf) && buf[0] == '1';
        if (v)
            OutputDebugStringA("[DivaLinux] PV thread inline test: the game thread draws each PV picture at its handoff\n");
    }
    return v > 0;
}

static gl_state_struct pv_gl_state_shared = {};

static void pv_thread_bind(bool on) {
    pv_render_thread_flag = on;
    gl_state_thread = on ? &pv_gl_state_shared : 0;
    gl_rend_state_thread_base = on ? GL_REND_STATE_COUNT : 0;
    rctx_thread = on ? pvt->rctx.load() : 0;
    disp_manager_thread = on ? pvt->dm : 0;
    sprite_manager_thread_bind(on);
    Glitter::UsePvScenes(on);
    dvk_make_current(on ? pvt->ctx : nullptr);
}

static void pv_thread_main() {
    pv_thread_t& t = *pvt;
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
    return false;   // ready on a later frame
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
    char buf[640];
    snprintf(buf, sizeof(buf),
        "[DivaLinux] threaded PV: HUD %.1f fps (avg %.1f ms, worst %.1f ms, %d frames over the display time); "
        "the game thread waited for the PV 0 times (handoffs %.2f ms each). PV %.1f pictures/s, %.1f ms each "
        "from handoff to shown (longest %.1f), PV thread %.1f ms per picture. GPU: HUD queue %.1f ms per frame, "
        "PV queue %.1f ms per picture\n",
        pv_ts.frames / elapsed_s, pv_ts.sum_ms / pv_ts.frames, pv_ts.worst_ms, pv_ts.late,
        pv_ts.pictures ? pv_ts.handoff_ms / pv_ts.pictures : 0.0,
        pv_ts.pictures / elapsed_s, pv_ts.pictures ? pv_ts.pic_ms_sum / pv_ts.pictures : 0.0, pv_ts.pic_ms_max,
        pv_ts.pictures ? pv_ts.pv_cpu_ms / pv_ts.pictures : 0.0,
        hud_gpu_frames ? hud_gpu_ms / hud_gpu_frames : 0.0, gpu_frames ? gpu_ms / gpu_frames : 0.0);
    OutputDebugStringA(buf);
}

/* Game thread, once per frame in threaded mode, before the HUD is drawn: show a finished
   picture, hand off the next one. Never waits. */
static void pv_threaded_frame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    pv_frame_no++;
    if (!pv_threaded_was) {
        pv_threaded_was = true;
        pv_trace_left = 15;
        // nothing from before this song: black until the first picture is done
        memset(pv_cache_valid, 0, sizeof(pv_cache_valid));
        pv_song_first = pvt ? pvt->number + 1 : 1;
        OutputDebugStringA("[DivaLinux] threaded PV on: the HUD every frame over the newest finished PV picture "
            "(slots A and B), the PV on its own thread at whatever rate it manages\n");
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

    // a picture finished on the GPU: show it from this frame
    if (ready && pv_job_out && pvt->recorded.load() == pvt->number
        && dvk_ticket_done(pvt->ctx, pvt->ticket.load())) {
        dvk_ticket_use(pvt->ctx, pvt->ticket.load());   // (finished: no GPU wait)
        pv_front = pvt->slot;
        pv_job_out = false;
        pv_shown_number = pvt->number;
        const double_t ms = pv_ms_since(pv_handoff_time);
        pv_ts.pictures++;
        pv_ts.pic_ms_sum += ms;
        pv_ts.pic_ms_max = max_def(pv_ts.pic_ms_max, ms);
        pv_ts.pv_cpu_ms += pvt->cpu_ms.load();
        pv_stat.full++;
        pv_sync->pv_busy = 0;
        pv_sync->pv_done++;
        snprintf(trace, sizeof(trace), "picture %u finished (%.1f ms after its handoff, PV thread %.1f ms) -> %c shown from now; ",
            pv_shown_number, ms, pvt->cpu_ms.load(), pv_front ? 'B' : 'A');
    }

    // the PV's tasks rebuilt their lists this frame: hand them to the PV thread
    static uint32_t seq_seen = 0;
    if (ready && !pv_job_out && pv_sync->disp_seq != seq_seen) {
        seq_seen = pv_sync->disp_seq;
        LARGE_INTEGER h0;
        QueryPerformanceCounter(&h0);
        pv_thread_t& t = *pvt;
        t.number++;
        t.slot = pv_front_valid() ? pv_front ^ 1 : pv_front;
        pv_cache_valid[t.slot][0] = pv_cache_valid[t.slot][1] = false;
        pv_save_slot = t.slot;
        t.eyes = stereo_enabled() ? 2 : 1;
        t.cam = rctx_main->render_manager_cam;
        pv_camera_snapshot_take();   // what the game's camera getters tell the PV thread
        // Everything the game thread has recorded so far goes to the GPU now, and the
        // picture's GPU work goes after it: this frame's uploads (character vertex data,
        // textures) included, not just the previous frames'. (A submit, not a wait.)
        t.after_main = dvk_flush();
        disp_manager_handoff(t.dm, t.rctx.load());
        sprite_manager_handoff_pv_lists();
        Glitter::SnapshotScenesForPv();
        if (pv_inline_mode()) {
            // the test mode: draw it here and now, as the PV thread would
            pv_thread_bind(true);
            pv_thread_draw();
            pv_thread_bind(false);
        }
        else {
            {
                std::lock_guard<std::mutex> lock(t.m);
                t.job = true;
            }
            t.cv.notify_one();
        }
        pv_job_out = true;
        pv_sync->pv_busy = 1;
        QueryPerformanceCounter(&pv_handoff_time);
        pv_ts.handoff_ms += pv_ms_since(h0);
        snprintf(trace + strlen(trace), sizeof(trace) - strlen(trace), "picture %u handed to the PV thread (into %c); ",
            t.number, t.slot ? 'B' : 'A');
    }

    if (pv_trace_left > 0) {
        pv_trace_left--;
        char buf[512];
        snprintf(buf, sizeof(buf), "[DivaLinux] threaded PV frame %u (%.1f ms): HUD drawn over %c (picture %u), "
            "waited for the PV 0 ms; %s%s\n", pv_frame_no, pv_frame_ms, pv_front ? 'B' : 'A', pv_shown_number, trace,
            pv_job_out ? "PV thread drawing" : (ready ? "PV thread idle" : "PV thread starting"));
        OutputDebugStringA(buf);
    }
}

/* The game thread's share of the per-frame render pre/post-process: in threaded mode the PV
   thread does the PV's part (hook.cpp). */
bool pv_threaded_active() {
    return pvt && pvt->ready.load() && pv_threaded_on();
}

namespace rndr {
    struct RenderTextureData {
        GLenum type;
        int32_t width;
        int32_t height;
        int32_t max_level;
        GLenum color_format;
        GLenum depth_format;
    };

    struct RenderTextureIndex {
        int32_t arr[3];
    };

    static const RenderTextureData* render_manager_render_texture_data_array
        = (const RenderTextureData*)0x0000000140A24420;

    static const RenderTextureIndex* render_manager_render_texture_index_array
        = (const RenderTextureIndex*)0x0000000140A244E0;

    void RenderManager::add_pre_process(int32_t type, draw_pre_process_func func, void* data) {
        pre_process.push_back({ type, func, data });
    }

    void RenderManager::clear_pre_process(int32_t type) {
        for (auto i = pre_process.begin(); i != pre_process.end(); i++)
            if (i->type == type) {
                pre_process.erase(i);
                break;
            }
    }

    RenderTexture& RenderManager::get_render_texture(int32_t index) {
        return render_textures[render_manager_render_texture_index_array[index].arr[tex_index[index]]];
    }

    void RenderManager::reset() {
        for (int32_t i = 0; i < RND_PASSID_NUM; i++) {
            pass_sw[i] = true;
            cpu_time[i] = 0.0;
            gpu_time[i] = 0.0;
        }

        set_pass_sw(RND_PASSID_2, 0);
        set_pass_sw(RND_PASSID_REFLECT, 0);
        set_pass_sw(RND_PASSID_REFRACT, 0);
        set_pass_sw(RND_PASSID_PRE_PROCESS, 0);
        set_pass_sw(RND_PASSID_SHOW_VECTOR, 0);

        shadow_ptr = 0;
        sync_gpu = false;
        time.get_timestamp();

        shadow = true;
        opaque_z_sort = true;
        alpha_z_sort = true;
        silhouette = false;
        silhouette_high = false;

        reflect_blur_num = 1;
        reflect_blur_filter = IMGF_BOX_SAMPL_4;
        show_ref_map = false;

        reflect_type = STAGE_DATA_REFLECT_DISABLE;
        check_state = false;
        show_vector_flags = 0;
        show_vector_length = 0.05f;
        show_vector_z_offset = 0.0f;
        show_stage_shadow = false;
        effect_texture = 0;

        reflect = false;
        refract = false;
        npr_param = 0;
        npr_mask = false;
        reflect_texture_mask = false;
        reflect_tone_curve = false;
        field_31F = false;
        light_stage_ambient = false;

        for (bool& i : draw_pass_3d)
            i = true;

        field_11E = false;
        clear = false;

        width = 0;
        height = 0;
        multisample_framebuffer = 0;
        multisample_renderbuffer = 0;
        multisample = true;

        npr = false;
    }

    void RenderManager::resize(int32_t width, int32_t height) {
        if (this->width == width && this->height == height)
            return;

        this->width = width;
        this->height = height;

        if (!multisample_framebuffer)
            glGenFramebuffers(1, &multisample_framebuffer);

        if (!multisample_renderbuffer)
            glGenRenderbuffers(1, &multisample_renderbuffer);

        glBindFramebuffer(GL_FRAMEBUFFER, multisample_framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, multisample_renderbuffer);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 8, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, multisample_renderbuffer);
        glDrawBufferDLL(GL_COLOR_ATTACHMENT0);
        glReadBufferDLL(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    void RenderManager::set_clear(bool value) {
        clear = value;
    }

    void RenderManager::set_effect_texture(texture* value) {
        effect_texture = value;
    }

    void RenderManager::set_multisample(bool value) {
        multisample = value;
    }

    void RenderManager::set_npr_param(int32_t value) {
        npr_param = value;
    }

    void RenderManager::set_pass_sw(RenderPassID id, bool value) {
        pass_sw[id] = value;
    }

    void RenderManager::set_reflect(bool value) {
        reflect = value;
    }

    void RenderManager::set_reflect_blur(int32_t reflect_blur_num, ImgfBoxSampl reflect_blur_filter) {
        this->reflect_blur_num = reflect_blur_num;
        this->reflect_blur_filter = reflect_blur_filter;
    }

    void RenderManager::set_reflect_resolution_mode(reflect_refract_resolution_mode mode) {
        tex_index[0] = mode;
    }

    void RenderManager::set_reflect_type(stage_data_reflect_type type) {
        reflect_type = type;
    }

    void RenderManager::set_refract(bool value) {
        refract = value;
    }

    void RenderManager::set_refract_resolution_mode(reflect_refract_resolution_mode mode) {
        tex_index[1] = mode;
    }

    void RenderManager::set_shadow_false() {
        shadow = false;
    }

    void RenderManager::set_shadow_true() {
        shadow = true;
    }

    void RenderManager::render_all() {
        if (!rctx)
            return;

        gl_state.get();
        pv_frame_begin();
        hud_layer_active = hud_layer_wanted();
        LARGE_INTEGER render_start;
        QueryPerformanceCounter(&render_start);
        sprite_manager_pre_draw();
        const bool pv_on_thread = pv_threaded_active();   // then the PV thread does the PV's part
        if (!pv_on_thread)
            mdl::etc_obj_manager->pre_draw();

        {
            render_data_context rend_data_ctx(GL_REND_STATE_PRE_3D);
            rend_data_ctx.state.bind_framebuffer(0);
            static const vec4 color_clear = 0.0f;
            rend_data_ctx.state.clear_buffer(GL_COLOR, 0, (float_t*)&color_clear);
            rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            rend_data_ctx.state.enable_depth_test();
            rend_data_ctx.state.set_depth_func(GL_LEQUAL);
            rend_data_ctx.state.set_depth_mask(GL_TRUE);
        }

        // Threaded PV: show a finished picture, hand off the next; the HUD over the newest
        const bool threaded = pv_threaded_on();
        if (threaded) {
            pv_threaded_frame();
            pv_skip = true;   // never the PV here: the HUD over the newest picture (or black)
        }
        else if (pv_threaded_was) {
            // Leaving a song: the PV thread may still be drawing the last picture with the
            // shared post-process state and objects; the game thread draws the PV itself from
            // now on, so let that finish first (not during play: this is the song's end).
            if (pvt && pv_job_out)
                while (pvt->recorded.load() != pvt->number)
                    Sleep(0);
            pv_threaded_was = false;
            pv_job_out = false;
            pv_save_slot = pv_front;
            if (pv_sync)
                pv_sync->pv_busy = 0;
        }

        if (stereo_enabled())
            render_all_stereo();
        else
            render_all_passes();

        // The HUD once, outside the eye passes, into its own layer
        if (hud_layer_active)
            draw_hud_layer();
#ifdef DIVAVK
        else if (hud_layer_was)
            dvk_xr_set_hud_texture(0);
#endif
        hud_layer_was = hud_layer_active;

        if (!pv_on_thread)
            mdl::etc_obj_manager->post_draw();
        sprite_manager_post_draw();
        {
            LARGE_INTEGER render_end;
            QueryPerformanceCounter(&render_end);
            pv_stat.render_ms += (double_t)(render_end.QuadPart - render_start.QuadPart) * 1000.0 / (double_t)pv_qpf.QuadPart;
        }

        render_data_context rend_data_ctx(GL_REND_STATE_POST_2D);
        rend_data_ctx.state.get();
        rend_data_ctx.state.bind_vertex_array(0);
        rend_data_ctx.state.disable_primitive_restart();
        rend_data_ctx.state.bind_uniform_buffer(0);
        rend_data_ctx.state.bind_uniform_buffer_base(0, 0);
        rend_data_ctx.state.bind_uniform_buffer_base(1, 0);
        rend_data_ctx.state.bind_uniform_buffer_base(2, 0);
        rend_data_ctx.state.bind_uniform_buffer_base(3, 0);
        rend_data_ctx.state.bind_uniform_buffer_base(4, 0);
        rend_data_ctx.state.bind_shader_storage_buffer(0);
        rend_data_ctx.state.bind_shader_storage_buffer_base(0, 0);
    }

    void RenderManager::render_all_passes() {
        const int32_t eye = stereo_eye_index();
        LARGE_INTEGER pass_start;
        QueryPerformanceCounter(&pass_start);
        const bool have_picture = pv_cache[pv_front][eye] && pv_cache_valid[pv_front][eye];
        if (pv_skip && (have_picture || pv_threaded_was)) {
            // PV frame skip / threaded PV: the kept PV picture (black before the first one),
            // then a fresh HUD on top
            render_data_context rend_data_ctx(GL_REND_STATE_2D);
            rend_data_ctx.state.get();
            rend_data_ctx.state.begin_event("rndpass_render_all_pass::Caller::execute_2d (PV reused)");
            if (have_picture)
                pv_cache_restore(rend_data_ctx, eye);
            else {
                rctx->screen_buffer.begin_render(rend_data_ctx.state);
                rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 1.0f);
                rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
                rctx->screen_buffer.end_render(rend_data_ctx.state);
            }
            for (int32_t i = RND_PASSID_SHOW_VECTOR; i <= RND_PASSID_12; i++)
                if (i != RND_PASSID_POST_PROCESS)
                    RenderManager::render_single_pass(rend_data_ctx, (RenderPassID)i);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
            pv_stat.hud_ms += pv_ms_since(pass_start);
            return;
        }

        {
            render_data_context rend_data_ctx(GL_REND_STATE_PRE_3D);
            rend_data_ctx.state.begin_event("rndpass_render_all_pass::Caller::execute_pre3d");
            for (int32_t i = RND_PASSID_SHADOW; i <= RND_PASSID_CLEAR; i++)
                RenderManager::render_single_pass(rend_data_ctx, (RenderPassID)i);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
        }

        {
            render_data_context rend_data_ctx(GL_REND_STATE_3D);
            rend_data_ctx.state.get();
            rend_data_ctx.state.begin_event("rndpass_render_all_pass::Caller::execute_3d");
            for (int32_t i = RND_PASSID_PRE_SPRITE; i <= RND_PASSID_3D; i++)
                RenderManager::render_single_pass(rend_data_ctx, (RenderPassID)i);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
        }

        {
            render_data_context rend_data_ctx(GL_REND_STATE_2D);
            rend_data_ctx.state.get();
            rend_data_ctx.state.begin_event("rndpass_render_all_pass::Caller::execute_2d");
            LARGE_INTEGER hud_start = {};
            for (int32_t i = RND_PASSID_SHOW_VECTOR; i <= RND_PASSID_12; i++) {
                RenderManager::render_single_pass(rend_data_ctx, (RenderPassID)i);
                if (i == RND_PASSID_POST_PROCESS) {
                    pv_cache_save(rend_data_ctx, eye);   // the finished PV picture, before the HUD
                    pv_stat.pv_ms += pv_ms_since(pass_start);
                    QueryPerformanceCounter(&hud_start);
                }
            }
            if (hud_start.QuadPart)
                pv_stat.hud_ms += pv_ms_since(hud_start);
            rend_data_ctx.state.end_event();
            rend_data_ctx.state.finish();
        }
    }

    // Renders the full pipeline once per eye and composites the results side by side.
    void RenderManager::render_all_stereo() {
        rndr::Render* rend = render;
        cam_data center_cam = rctx->render_manager_cam;

        // TAA history would blend the two eyes together, so it's off for stereo frames.
        // The camera's TAA jitter is already suppressed in init_projection_matrix.
        // (Threaded PV: the HUD only; the post-process state is the PV thread's, left alone.)
        const bool touch_rend = !pv_threaded_was;
        const int32_t taa = rend->taa;
        const float_t taa_blend = rend->taa_blend;
        if (touch_rend) {
            rend->taa = 0;
            rend->taa_blend = 1.0f;
        }

        // Per-frame ring buffers that must advance once per frame, not once per eye.
        const int32_t exposure_history_counter = rend->exposure_history_counter;
        const int32_t exposure_query_index = rend->exposure_query_index;
        const int32_t lens_flare_query_index = rend->lens_flare_query_index;

        for (int32_t i = 0; i < 2; i++) {
            const int32_t eye_sign = i ? 1 : -1;
            if (i && touch_rend) {
                rend->exposure_history_counter = exposure_history_counter;
                rend->exposure_query_index = exposure_query_index;
                rend->lens_flare_query_index = lens_flare_query_index;
            }

            // The pre-3D state cache is only synced with GL once per frame (rndpass_pre_proc).
            // By the second eye the 3D/2D passes have rebound the uniform buffers, textures and
            // so on, and a stale cache skips "redundant" binds: the shadow and SSS passes then
            // read the other contexts' buffers (red hair from the main pass's sss_param, wrong
            // skin lighting, missing objects in the main pass). Resync it for every later eye.
            if (i) {
                render_data_context rend_data_ctx(GL_REND_STATE_PRE_3D);
                rend_data_ctx.state.get();
            }

            stereo_make_eye_cam(center_cam, rctx->render_manager_cam, eye_sign);
            stereo_eye_begin(eye_sign);
            render_all_passes();
            stereo_eye_end(eye_sign);
        }

        stereo_present();

        rctx->render_manager_cam = center_cam;
        if (touch_rend) {
            rend->taa = taa;
            rend->taa_blend = taa_blend;
        }
    }

    void RenderManager::render_single_pass(render_data_context& rend_data_ctx, RenderPassID id) {
        cpu_time[id] = 0.0;
        gpu_time[id] = 0.0;
        if (!pass_sw[id]) {
            gl_get_error_print();
            return;
        }

        render_pass_begin();
        switch (id) {
        case RND_PASSID_SHADOW:
            pass_shadow(rend_data_ctx);
            break;
        case RND_PASSID_SS_SSS:
            pass_ss_sss(rend_data_ctx);
            break;
        case RND_PASSID_REFLECT:
            pass_reflect(rend_data_ctx);
            break;
        case RND_PASSID_REFRACT:
            pass_refract(rend_data_ctx);
            break;
        case RND_PASSID_PRE_PROCESS:
            pass_pre_process(rend_data_ctx);
            break;
        case RND_PASSID_CLEAR:
            pass_clear(rend_data_ctx);
            break;
        case RND_PASSID_PRE_SPRITE:
            pass_pre_sprite(rend_data_ctx);
            break;
        case RND_PASSID_3D:
            pass_3d(rend_data_ctx);
            break;
        case RND_PASSID_SHOW_VECTOR:
            pass_show_vector(rend_data_ctx);
            break;
        case RND_PASSID_POST_PROCESS:
            pass_post_process(rend_data_ctx);
            break;
        case RND_PASSID_SPRITE:
            pass_sprite(rend_data_ctx);
            break;
        }
        render_pass_end(id);
        gl_get_error_print();
    }

    void RenderManager::render_pass_begin() {
        if (sync_gpu)
            glFinishDLL();
        time.get_timestamp();
    }

    void RenderManager::render_pass_end(RenderPassID id) {
        cpu_time[id] = time.calc_time();
        if (sync_gpu) {
            time_struct t;
            glFinishDLL();
            gpu_time[id] = t.calc_time();
        }
        else
            gpu_time[id] = 0;
    }

    void RenderManager::pass_shadow(render_data_context& rend_data_ctx) {
        cam_data& cam = rctx->render_manager_cam;

        rend_data_ctx.state.begin_event("pass_shadow");
        rend_data_ctx.state.begin_event("texproj");
        cam_data texproj_cam;
        if (litproj_set(rend_data_ctx, texproj_cam)) {
            rend_data_ctx.set_batch_scene_camera(texproj_cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_OPAQUE, texproj_cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_TRANSPARENT, texproj_cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_TRANSLUCENT, texproj_cam);

            rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;
            rend_data_ctx.shader_flags.arr[U_DEPTH] = 0;
            apply_esm_filter(rend_data_ctx, &litproj_shadow[0],
                &litproj_shadow[1], 0, 1.5f, 0.01f, true);

            if (litproj_clear(rend_data_ctx, texproj_cam)) {
                for (int32_t i = LIGHT_SET_MAIN; i < LIGHT_SET_MAX; i++)
                    lighting_set(rend_data_ctx, (light_set_id)i);

                rend_data_ctx.set_batch_scene_camera(cam);
                disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_OPAQUE, cam);
                disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_TRANSPARENT, cam);
                disp_manager->obj_sort(rend_data_ctx, mdl::OBJ_TYPE_TRANSLUCENT, 1, cam);
                rend_data_ctx.state.set_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                rend_data_ctx.state.enable_blend();
                disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_TRANSLUCENT, cam);
                rend_data_ctx.state.disable_blend();
                rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;
                litproj_shadow[0].end_render(rend_data_ctx.state);
            }
        }
        rend_data_ctx.state.end_event();

        bool make_shadowmap = false;
        int32_t obj_shadow_count[2];
        for (int32_t i = 0; i < 2; i++) {
            obj_shadow_count[i] = disp_manager->get_obj_count((mdl::ObjType)(mdl::OBJ_TYPE_SHADOW_CHARA + i));
            if (obj_shadow_count[i])
                make_shadowmap = true;
        }

        Shadow* shad = shadow_ptr;
        if (stereo_reuse_shadow_maps()) {
            // Second stereo eye: the shadow maps are rendered from the light's camera, so the
            // first eye's are exactly what this eye would draw. (The texproj part above uses
            // the eye's camera and is still drawn per eye.)
        }
        else if (shadow && make_shadowmap) {
            int32_t group[2];
            shad->begin_make_shadow_textures(group);

            cam_data cam;
            for (int32_t i = 0, index = 0; i < 2; i++) {
                if (!obj_shadow_count[i])
                    continue;

                shad->begin_make_silhouette_map(rend_data_ctx, cam, group[i], index);
                rend_data_ctx.set_batch_scene_camera(cam);
                disp_manager->draw(rend_data_ctx, (mdl::ObjType)(mdl::OBJ_TYPE_SHADOW_CHARA + group[i]), cam);
                if (disp_manager->get_obj_count(
                    (mdl::ObjType)(mdl::OBJ_TYPE_SHADOW_OBJECT_CHARA + group[i])) > 0) {
                    rend_data_ctx.state.set_color_mask(show_stage_shadow
                        ? GL_TRUE : GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    disp_manager->draw(rend_data_ctx,
                        (mdl::ObjType)(mdl::OBJ_TYPE_SHADOW_OBJECT_CHARA + i), cam);
                    rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                }
                shad->end_make_silhouette_map(rend_data_ctx, group[i], index);
                index++;
            }
        }
        else {
            shad->begin_make_shadow_textures(0);
            shad->clear(rend_data_ctx.state);
        }
        shad->end_make_shadow_textures(rend_data_ctx.state);
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_ss_sss(render_data_context& rend_data_ctx) {
        ::sss_data* sss = sss_data_get();
        if (!sss->init_data || !sss->enable) {
            sss_param = 0.0f;
            sss_param_reflect = 0.0f;
            return;
        }

        cam_data& cam = rctx->render_manager_cam;
        rend_data_ctx.state.begin_event("pass_ss_sss");
        rndr::Render* rend = render;
        extern bool reflect_full;
        if (reflect_full && pass_sw[rndr::RND_PASSID_REFLECT]
            && draw_pass_reflect_get_obj_reflect_surface()
            && disp_manager->get_obj_count(mdl::OBJ_TYPE_SSS) && reflect_full_ptr) {
            rend_data_ctx.state.begin_event("reflect");
            reflect_draw = true;

            RenderTexture& refl_tex = reflect_full_ptr->reflect_texture;
            if (sss->set(rend_data_ctx, refl_tex)) {
                rndr::Render* rend = render;

                for (int32_t i = LIGHT_SET_MAIN; i < LIGHT_SET_MAX; i++)
                    lighting_set(rend_data_ctx, (light_set_id)i);

                if (shadow)
                    shadow_ptr->bind_shadow(rend_data_ctx);
                else
                    render_manager.shadow_ptr->unbind_shadow(rend_data_ctx);

                cam_data reflect_cam = cam;
                set_reflect_mat(rend_data_ctx, reflect_cam);
                rend_data_ctx.set_batch_scene_camera(reflect_cam);

                rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_SSS_SKIN;
                rend_data_ctx.state.enable_depth_test();
                rend_data_ctx.state.set_depth_func(GL_LEQUAL);
                rend_data_ctx.state.set_depth_mask(GL_TRUE);
                disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SSS, reflect_cam);
                rend_data_ctx.state.disable_depth_test();
                rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;
                render_manager.shadow_ptr->unbind_shadow(rend_data_ctx);
                rend_data_ctx.shader_flags.arr[U_NPR] = 0;

                if (npr_param == 1) {
                    if (sss->enable && sss->downsample) {
                        rend_data_ctx.shader_flags.arr[U_NPR] = 1;
                        rend->draw_sss_contour(rend_data_ctx, reflect_cam);
                    }
                    else if (npr) {
                        refl_tex.begin_render(rend_data_ctx.state);
                        refl_tex.set_viewport(rend_data_ctx.state);
                        rend_data_ctx.state.clear(GL_DEPTH_BUFFER_BIT);
                        rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_SSS_SKIN;
                        rend_data_ctx.state.enable_depth_test();
                        rend_data_ctx.state.set_depth_mask(GL_TRUE);
                        disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SSS, reflect_cam);
                        rend_data_ctx.state.disable_depth_test();
                        rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;
                        refl_tex.end_render(rend_data_ctx.state);
                        rend_data_ctx.shader_flags.arr[U_NPR] = 1;
                        rend->draw_sss_contour(rend_data_ctx, reflect_cam);
                    }
                }

                sss->apply_filter(rend_data_ctx);
                sss->reset(rend_data_ctx);
            }

            rend_data_ctx.set_npr(this);
            reflect_draw = false;
            rend_data_ctx.state.end_event();
        }

        if (sss->set(rend_data_ctx)) {
            for (int32_t i = LIGHT_SET_MAIN; i < LIGHT_SET_MAX; i++)
                lighting_set(rend_data_ctx, (light_set_id)i);

            if (shadow)
                shadow_ptr->bind_shadow(rend_data_ctx);
            else
                shadow_ptr->unbind_shadow(rend_data_ctx);

            rend_data_ctx.set_batch_scene_camera(cam);

            rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_SSS_SKIN;
            rend_data_ctx.state.enable_depth_test();
            rend_data_ctx.state.set_depth_func(GL_LEQUAL);
            rend_data_ctx.state.set_depth_mask(GL_TRUE);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SSS, cam);
            rend_data_ctx.state.disable_depth_test();
            rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;
            shadow_ptr->unbind_shadow(rend_data_ctx);
            rend_data_ctx.shader_flags.arr[U_NPR] = 0;

            if (npr_param == 1) {
                if (sss->enable && sss->downsample) {
                    rend_data_ctx.shader_flags.arr[U_NPR] = 1;
                    rend->draw_sss_contour(rend_data_ctx, cam);
                }
                else if (npr) {
                    rend->rend_texture[0].begin_render(rend_data_ctx.state);
                    rend->rend_texture[0].set_viewport(rend_data_ctx.state);
                    rend_data_ctx.state.clear(GL_DEPTH_BUFFER_BIT);
                    rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_SSS_SKIN;
                    rend_data_ctx.state.enable_depth_test();
                    rend_data_ctx.state.set_depth_mask(GL_TRUE);
                    disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SSS, cam);
                    rend_data_ctx.state.disable_depth_test();
                    rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;
                    rend->rend_texture[0].end_render(rend_data_ctx.state);
                    rend_data_ctx.shader_flags.arr[U_NPR] = 1;
                    rend->draw_sss_contour(rend_data_ctx, cam);
                }
            }

            sss->apply_filter(rend_data_ctx);
            sss->reset(rend_data_ctx);
        }

        rend_data_ctx.set_npr(this);
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_reflect(render_data_context& rend_data_ctx) {
        extern bool reflect_full;
        if (reflect_full) {
            draw_pass_reflect_full(rend_data_ctx, *this);
            return;
        }

        cam_data& cam = rctx->render_manager_cam;
        rend_data_ctx.state.begin_event("pass_reflect");
        RenderTexture& refl_tex = get_render_texture(0);
        RenderTexture& refl_buf_tex = rctx->reflect_buffer;
        refl_tex.begin_render(rend_data_ctx.state);
        if (disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_OPAQUE)
            || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_TRANSPARENT)
            || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_TRANSLUCENT)
            || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_CHARA_OPAQUE)
            || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_CHARA_TRANSPARENT)
            || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_CHARA_TRANSLUCENT)) {
            refl_tex.set_viewport(rend_data_ctx.state);
            rend_data_ctx.set_batch_scene_camera(cam);

            for (int32_t i = LIGHT_SET_MAIN; i < LIGHT_SET_MAX; i++)
                lighting_set(rend_data_ctx, (light_set_id)i);
            for (int32_t i = FOG_DEPTH; i < FOG_BUMP; i++)
                fog_set(rend_data_ctx, (fog_id)i);

            rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_S_REFL;

            light_set* set = &light_set_data[LIGHT_SET_MAIN];
            light_clip_plane clip_plane;
            set->lights[LIGHT_SPOT].get_clip_plane(clip_plane);
            rend_data_ctx.shader_flags.arr[U_REFLECT] = 2;
            rend_data_ctx.shader_flags.arr[U_CLIP_PLANE] = clip_plane.data[1];

            rend_data_ctx.state.enable_depth_test();
            rend_data_ctx.state.set_depth_func(GL_LEQUAL);
            rend_data_ctx.state.set_depth_mask(GL_TRUE);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_OPAQUE, cam, 0, reflect_texture_mask);
            if (reflect_type == STAGE_DATA_REFLECT_REFLECT_MAP) {
                rend_data_ctx.state.enable_blend();
                rend_data_ctx.state.set_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                rend_data_ctx.state.set_blend_equation(GL_FUNC_ADD);
                disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_TRANSLUCENT, cam);
                rend_data_ctx.state.disable_blend();
            }

            if (reflect_type == STAGE_DATA_REFLECT_REFLECT_MAP) {
                rend_data_ctx.shader_flags.arr[U_REFLECT] = reflect_tone_curve ? 1 : 0;
                rend_data_ctx.shader_flags.arr[U_CLIP_PLANE] = clip_plane.data[0];
                disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_CHARA_OPAQUE, cam);
            }
            rend_data_ctx.state.disable_depth_test();
            rend_data_ctx.shader_flags.arr[U_REFLECT] = 0;
            rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;

            for (int32_t i = reflect_blur_num, j = 0; i > 0; i--, j++) {
                apply_blur_filter_sub(rend_data_ctx, &refl_buf_tex, &refl_tex,
                    reflect_blur_filter, 1.0f, 1.0f, 0.0f);
                image_filter_scale(rend_data_ctx, &refl_tex, refl_buf_tex.get_texture());
            }
        }
        else {
            vec4 clear_color;
            rend_data_ctx.state.get_clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
            rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
            rend_data_ctx.state.clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        }
        shader::unbind(rend_data_ctx.state);
        refl_tex.end_render(rend_data_ctx.state);
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_refract(render_data_context& rend_data_ctx) {
        cam_data& cam = rctx->render_manager_cam;
        rend_data_ctx.state.begin_event("pass_refract");
        RenderTexture& reflect_texture = get_render_texture(1);
        if (disp_manager->get_obj_count(mdl::OBJ_TYPE_REFRACT_OPAQUE)
            || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFRACT_TRANSPARENT)
            || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFRACT_TRANSLUCENT)) {
            reflect_texture.begin_render(rend_data_ctx.state);
            reflect_texture.set_viewport(rend_data_ctx.state);
            rend_data_ctx.set_batch_scene_camera(cam);

            for (int32_t i = LIGHT_SET_MAIN; i < LIGHT_SET_MAX; i++)
                lighting_set(rend_data_ctx, (light_set_id)i);
            for (int32_t i = FOG_DEPTH; i < FOG_BUMP; i++)
                fog_set(rend_data_ctx, (fog_id)i);

            vec4 clear_color;
            rend_data_ctx.state.get_clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
            rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            rend_data_ctx.state.clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);

            rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_S_REFR;
            rend_data_ctx.state.enable_depth_test();
            rend_data_ctx.state.set_depth_func(GL_LEQUAL);
            rend_data_ctx.state.set_depth_mask(GL_TRUE);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFRACT_OPAQUE, cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFRACT_TRANSPARENT, cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFRACT_TRANSLUCENT, cam);
            rend_data_ctx.state.disable_depth_test();

            rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;
            shader::unbind(rend_data_ctx.state);
            reflect_texture.end_render(rend_data_ctx.state);
        }
        else {
            reflect_texture.begin_render(rend_data_ctx.state);
            vec4 clear_color;
            rend_data_ctx.state.get_clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
            rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
            rend_data_ctx.state.clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
            reflect_texture.end_render(rend_data_ctx.state);
        }
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_pre_process(render_data_context& rend_data_ctx) {
        rend_data_ctx.state.begin_event("pass_pre_process");
        for (draw_pre_process& i : pre_process)
            if (i.func)
                i.func(rend_data_ctx, i.data, rctx->render_manager_cam);
        shader::unbind(rend_data_ctx.state);
        rend_data_ctx.state.bind_framebuffer(0);
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_clear(render_data_context& rend_data_ctx) {
        rend_data_ctx.state.begin_event("pass_clear");
        if (clear) {
            rctx->screen_buffer.begin_render(rend_data_ctx.state);
            clear_color_set_gl(rend_data_ctx.state);
            //rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
            rctx->screen_buffer.end_render(rend_data_ctx.state);
        }

        rndr::Render* rend = render;
        if (!sss_data_get()->enable || !sss_data_get()->downsample) {
            rend->begin_render(rend_data_ctx.state);
            if (sprite_manager_reqlist_count(2)) {
                rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
                rend_data_ctx.state.clear_depth(1.0f);
                rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }
            else {
                clear_color_set_gl(rend_data_ctx.state);
                rend_data_ctx.state.clear_depth(1.0f);
                if (clear)
                    rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                else
                    rend_data_ctx.state.clear(GL_DEPTH_BUFFER_BIT);
            }
            rend->end_render(rend_data_ctx.state);
        }
        else {
            if (sprite_manager_reqlist_count(2)) {
                rend->begin_render(rend_data_ctx.state);
                rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
                rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
                rend->end_render(rend_data_ctx.state);
            }
            else if (clear) {
                rend->begin_render(rend_data_ctx.state);
                clear_color_set_gl(rend_data_ctx.state);
                rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
                rend->end_render(rend_data_ctx.state);
            }
        }

        gl_get_error_print();
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_pre_sprite(render_data_context& rend_data_ctx) {
        if (!sprite_manager_reqlist_count(2))
            return;

        rend_data_ctx.state.begin_event("pass_pre_sprite");
        rndr::Render* rend = render;
        rend->begin_render(rend_data_ctx.state, true);
        rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
        rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
        rend_data_ctx.state.set_depth_mask(GL_FALSE);
        rend_data_ctx.state.disable_depth_test();
        rend_data_ctx.state.enable_blend();
        rend_data_ctx.state.disable_cull_face();
        sprite_manager_draw(rend_data_ctx, 2, true,
            rend->temp_buffer.get_texture(), set_projection_matrix_2d(true));
        rend_data_ctx.state.enable_cull_face();
        rend_data_ctx.state.disable_blend();
        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        shader::unbind(rend_data_ctx.state);
        rend->end_render(rend_data_ctx.state);
        gl_get_error_print();
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_3d(render_data_context& rend_data_ctx) {
        cam_data& cam = rctx->render_manager_cam;
        render->begin_render(rend_data_ctx.state);
        if (!sss_data_get()->enable || !sss_data_get()->downsample
            || draw_pass_3d_get_translucent_count()) {
            rend_data_ctx.state.set_depth_mask(GL_TRUE);
            rend_data_ctx.state.clear(GL_DEPTH_BUFFER_BIT);
        }

        rend_data_ctx.state.set_depth_func(GL_LEQUAL);

        rend_data_ctx.set_batch_scene_camera(cam);
        for (int32_t i = LIGHT_SET_MAIN; i < LIGHT_SET_MAX; i++)
            lighting_set(rend_data_ctx, (light_set_id)i);
        for (int32_t i = FOG_DEPTH; i < FOG_MAX; i++)
            fog_set(rend_data_ctx, (fog_id)i);

        if (shadow)
            shadow_ptr->bind_shadow(rend_data_ctx);
        else
            shadow_ptr->unbind_shadow(rend_data_ctx);

        if (effect_texture)
            rend_data_ctx.state.active_bind_texture_2d(14, effect_texture->glid);
        else
            rend_data_ctx.state.active_bind_texture_2d(14, rctx->empty_texture_2d->glid);

        if (pass_sw[RND_PASSID_REFLECT] && reflect) {
            extern bool reflect_full;
            RenderTexture& refl_tex = reflect_full && reflect_full_ptr
                ? reflect_full_ptr->reflect_texture : get_render_texture(0);
            rend_data_ctx.state.active_bind_texture_2d(15, refl_tex.get_texture_glid());
            rend_data_ctx.shader_flags.arr[U_TEX_REFLECTMAP] = 1;
        }
        else {
            rend_data_ctx.state.active_bind_texture_2d(15, rctx->empty_texture_2d->glid);
            rend_data_ctx.shader_flags.arr[U_TEX_REFLECTMAP] = 0;
        }

        rend_data_ctx.state.bind_sampler(14, rctx->render_samplers[0]);
        rend_data_ctx.state.bind_sampler(15, rctx->render_samplers[0]);
        rend_data_ctx.state.bind_sampler(16, rctx->render_samplers[0]);

        rend_data_ctx.shader_flags.arr[U_STAGE_AMBIENT] = light_stage_ambient ? 1 : 0;

        sss_data_get()->set_texture(rend_data_ctx.state, 1);

        rend_data_ctx.set_npr(this);
        rend_data_ctx.set_batch_sss_param(sss_param);

        if (alpha_z_sort) {
            disp_manager->obj_sort(rend_data_ctx,
                mdl::OBJ_TYPE_TRANSLUCENT, 1, cam, field_31F);
            disp_manager->obj_sort(rend_data_ctx,
                mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_GLITTER, 1, cam, field_31F);
            disp_manager->obj_sort(rend_data_ctx,
                mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_TRANSLUCENT, 1, cam, field_31F);
            disp_manager->obj_sort(rend_data_ctx,
                mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_OPAQUE, 1, cam, field_31F);
        }

        if (alpha_z_sort)
            disp_manager->obj_sort(rend_data_ctx, mdl::OBJ_TYPE_TRANSLUCENT_SORT_BY_RADIUS, 2, cam);

        if (opaque_z_sort)
            disp_manager->obj_sort(rend_data_ctx, mdl::OBJ_TYPE_OPAQUE, 0, cam);

        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        if (silhouette)
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SILHOUETTE, cam);

        if (draw_pass_3d[DRAW_PASS_3D_OPAQUE])
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_OPAQUE, cam);
        rend_data_ctx.state.disable_depth_test();

        Glitter::glt_particle_manager->DispScenes(rend_data_ctx, Glitter::DISP_OPAQUE, cam);
        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_OPAQUE, cam);

        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        if (draw_pass_3d[DRAW_PASS_3D_TRANSPARENT])
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_TRANSPARENT, cam);

        if (silhouette_high)
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SILHOUETTE_HIGH, cam);

        render->calc_exposure_chara_data(rend_data_ctx, cam);

        if (npr_param == 1)
            pass_3d_contour(rend_data_ctx);

        render->draw_lens_flare(rend_data_ctx, cam);
        star_catalog_draw(rend_data_ctx, cam);

        draw_pass_3d_translucent(rend_data_ctx,
            mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_OPAQUE,
            mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_OPAQUE,
            mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_OPAQUE, cam);

        snow_particle_draw(rend_data_ctx, cam);
        rain_particle_draw(rend_data_ctx, cam);
        leaf_particle_draw(rend_data_ctx, cam);
        particle_draw(rend_data_ctx, cam);
        rend_data_ctx.state.disable_depth_test();

        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        Glitter::glt_particle_manager->DispScenes(rend_data_ctx, Glitter::DISP_PRE_TRANSLUCENT, cam);
        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_PRE_TRANSLUCENT, cam);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        /*if (npr_param == 1) {
            rend_data_ctx.state.set_color_mask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
            rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }*/

        rend_data_ctx.state.enable_depth_test();

        if (draw_pass_3d[DRAW_PASS_3D_TRANSLUCENT]) {
            rend_data_ctx.state.enable_blend();
            rend_data_ctx.state.set_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            rend_data_ctx.state.set_depth_mask(GL_FALSE);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_TRANSLUCENT_SORT_BY_RADIUS, cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_TRANSLUCENT, cam);
            rend_data_ctx.state.disable_blend();
        }

        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // X
        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_POST_TRANSLUCENT, cam);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        draw_pass_3d_translucent(rend_data_ctx,
            mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_TRANSLUCENT,
            mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_TRANSLUCENT,
            mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_TRANSLUCENT, cam);
        rend_data_ctx.state.disable_depth_test();

        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        Glitter::glt_particle_manager->DispScenes(rend_data_ctx, Glitter::DISP_NORMAL, cam);
        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_NORMAL, cam);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        draw_pass_3d_translucent(rend_data_ctx,
            mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_GLITTER,
            mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_GLITTER,
            mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_GLITTER, cam);

        if (Glitter::glt_particle_manager_x->CheckHasScreenEffect()) { // X
            rend_data_ctx.state.begin_event("screen");

            cam_data screen_cam = cam;
            screen_cam.set_fov(32.2673416137695f * DEG_TO_RAD_FLOAT);
            screen_cam.calc_persp_proj_mat_offset(1.0f, render_get()->get_taa_offset()
                + stereo_get_proj_offset(screen_cam.get_fov(), screen_cam.get_aspect()));
            screen_cam.calc_view_proj_mat();
            rend_data_ctx.set_batch_scene_camera(screen_cam);

            if (alpha_z_sort) {
                disp_manager->obj_sort(rend_data_ctx,
                    mdl::OBJ_TYPE_SCREEN_TRANSLUCENT, 1, screen_cam);
                disp_manager->obj_sort(rend_data_ctx,
                    mdl::OBJ_TYPE_SCREEN_TRANSLUCENT_ALPHA_ORDER_POST_TRANSLUCENT, 1, screen_cam);
            }

            if (opaque_z_sort)
                disp_manager->obj_sort(rend_data_ctx, mdl::OBJ_TYPE_SCREEN_OPAQUE, 0, screen_cam);

            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SCREEN_OPAQUE, screen_cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SCREEN_TRANSPARENT, screen_cam);
            rend_data_ctx.state.enable_blend();
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_SCREEN_TRANSLUCENT, screen_cam);
            rend_data_ctx.state.disable_blend();

            draw_pass_3d_translucent(rend_data_ctx,
                mdl::OBJ_TYPE_SCREEN_OPAQUE_ALPHA_ORDER_POST_TRANSLUCENT,
                mdl::OBJ_TYPE_SCREEN_TRANSPARENT_ALPHA_ORDER_POST_TRANSLUCENT,
                mdl::OBJ_TYPE_SCREEN_TRANSLUCENT_ALPHA_ORDER_POST_TRANSLUCENT, screen_cam);

            rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_SCREEN, screen_cam);
            rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            rend_data_ctx.state.end_event();

            rend_data_ctx.set_batch_scene_camera(cam);
        }

        rend_data_ctx.state.active_bind_texture_2d(14, 0);
        rend_data_ctx.state.active_bind_texture_2d(15, 0);

        rend_data_ctx.state.disable_depth_test();

        if (shadow)
            shadow_ptr->unbind_shadow(rend_data_ctx);
        pass_sprite_surf(rend_data_ctx);
        shader::unbind(rend_data_ctx.state);
        render->end_render(rend_data_ctx.state);
    }

    void RenderManager::pass_show_vector(render_data_context& rend_data_ctx) {
        if (show_vector_flags) {
            rend_data_ctx.state.begin_event("pass_show_vector");
            rend_data_ctx.state.end_event();
        }
    }

    void RenderManager::pass_post_process(render_data_context& rend_data_ctx) {
        rend_data_ctx.state.begin_event("pass_post_process");

        rend_data_ctx.set_npr(this);

        static texture* (FASTCALL * litproj_texture_get)() = (texture * (FASTCALL*)())0x0000000140350600;
        render->apply_post_process(rend_data_ctx,
            rctx->render_manager_cam, litproj_texture_get(), npr_param);
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_sprite(render_data_context& rend_data_ctx) {
        rend_data_ctx.state.begin_event("pass_sprite");
        // With the HUD layer on, the HUD goes to its own target after the passes (draw_hud_layer)
        if (!hud_layer_active && sprite_manager_reqlist_count(0)) {
            rend_data_ctx.state.set_viewport(0, 0, render->screen_width, render->screen_height);

            if (multisample && multisample_framebuffer) {
                rend_data_ctx.state.bind_framebuffer(multisample_framebuffer);
                rend_data_ctx.state.enable_multisample();
                rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
                rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
            }
            else
                rctx->screen_buffer.begin_render(rend_data_ctx.state);

            rend_data_ctx.state.disable_depth_test();
            rend_data_ctx.state.enable_blend();
            rend_data_ctx.state.disable_cull_face();
            sprite_manager_set_res((double_t)rctx->screen_width / (double_t)rctx->screen_height,
                rctx->screen_width, rctx->screen_height);
            sprite_manager_draw(rend_data_ctx, 0, true,
                rctx->screen_overlay_buffer.get_texture(), set_projection_matrix_2d(false));
            rend_data_ctx.state.enable_cull_face();
            rend_data_ctx.state.disable_blend();
            rend_data_ctx.state.enable_depth_test();

            if (multisample && multisample_framebuffer) {
                rend_data_ctx.state.bind_framebuffer(0);
                rend_data_ctx.state.disable_multisample();
                fbo_blit(rend_data_ctx.state, multisample_framebuffer, rctx->screen_buffer.get_fb(),
                    0, 0, rctx->screen_width, rctx->screen_height,
                    0, 0, rctx->screen_width, rctx->screen_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            else
                rctx->screen_buffer.end_render(rend_data_ctx.state);
            shader::unbind(rend_data_ctx.state);

            gl_get_error_print();
        }

        if (!stereo_in_eye_pass())
            fbo_blit(rend_data_ctx.state, rctx->screen_buffer.get_fb(), 0,
                0, 0, rctx->screen_width, rctx->screen_height,
                0, 0, rctx->screen_width, rctx->screen_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

        rend_data_ctx.state.bind_framebuffer(0);
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_3d_contour(render_data_context& rend_data_ctx) {
        cam_data& cam = rctx->render_manager_cam;

        RenderTexture* rt;
        RenderTexture* contour_rt;
        if (reflect_draw) {
            rt = &reflect_full_ptr->reflect_texture;
            contour_rt = &reflect_full_ptr->reflect_buffer_texture;
        }
        else {
            rt = &render->rend_texture[0];
            contour_rt = render->sss_contour_texture;
        }

        rend_data_ctx.state.begin_event("`anonymous-namespace'::draw_npr_frame");
        rend_data_ctx.state.enable_blend();
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        rend_data_ctx.state.set_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        rend_data_ctx.state.set_blend_equation(GL_FUNC_ADD);
        rend_data_ctx.state.disable_depth_test();

        quad_shader_data quad_data = {};
        quad_data.g_texcoord_modifier = { 0.5f, 0.5f, 0.5f, 0.5f };
        rend_data_ctx.state.write_uniform_buffer(rctx->quad_ubo, quad_data);

        contour_params_shader_data contour_params_data = {};
        const double_t max_distance = cam.get_max_distance();
        const double_t min_distance = cam.get_min_distance();
        contour_params_data.g_near_far = {
            (float_t)(max_distance * (1.0 / (max_distance - min_distance))),
            (float_t)(-(max_distance * min_distance) * (1.0 / (max_distance - min_distance))),
            (float_t)min_distance, (float_t)max_distance
        };
        rend_data_ctx.state.write_uniform_buffer(rctx->contour_params_ubo, contour_params_data);

        shaders_ft.set(rend_data_ctx.state, rend_data_ctx.shader_flags, SHADER_FT_CONTOUR_NPR);
        rend_data_ctx.state.active_bind_texture_2d(14, rt->get_depth_texture_glid());
        rend_data_ctx.state.active_bind_texture_2d(16, contour_rt->get_texture_glid());
        rend_data_ctx.state.active_bind_texture_2d(17, contour_rt->get_depth_texture_glid());
        rend_data_ctx.state.bind_sampler(14, rctx->render_samplers[1]);
        rend_data_ctx.state.bind_sampler(16, rctx->render_samplers[1]);
        rend_data_ctx.state.bind_sampler(17, rctx->render_samplers[1]);
        rend_data_ctx.state.bind_uniform_buffer_base(0, rctx->quad_ubo);
        rend_data_ctx.state.bind_uniform_buffer_base(2, rctx->contour_params_ubo);
        rend_data_ctx.state.bind_vertex_array(rctx->common_vao);
        rend_data_ctx.state.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);

        if (effect_texture)
            rend_data_ctx.state.active_bind_texture_2d(14, effect_texture->glid);
        else
            rend_data_ctx.state.active_bind_texture_2d(14, rctx->empty_texture_2d->glid);
        rend_data_ctx.state.bind_sampler(14, rctx->render_samplers[0]);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.disable_blend();
        rend_data_ctx.state.end_event();
    }

    void RenderManager::pass_sprite_surf(render_data_context& rend_data_ctx) {
        if (!sprite_manager_reqlist_count(1))
            return;

        rndr::Render* rend = render;

        rend_data_ctx.state.set_depth_mask(GL_FALSE);
        rend_data_ctx.state.disable_depth_test();
        rend_data_ctx.state.disable_depth_test();
        rend_data_ctx.state.enable_blend();
        rend_data_ctx.state.disable_cull_face();
        sprite_manager_draw(rend_data_ctx, 1, true,
            rend->temp_buffer.get_texture(), set_projection_matrix_2d(false));
    }
}

void image_filter_scale(render_data_context& rend_data_ctx,
    RenderTexture* dst, texture* src, const vec4& scale) {
    if (!dst || !dst->m_txhd_color->glid || !dst->get_texture() || !src || !src->glid)
        return;

    rend_data_ctx.state.begin_event("`anonymous-namespace'::Impl::apply_no_filter_sub");

    dst->begin_render(rend_data_ctx.state);
    dst->set_viewport(rend_data_ctx.state);

    image_filter_scene_shader_data filter_scene = {};
    filter_scene.g_transform = { 1.0f, 1.0f, 0.0f, 0.0f };
    filter_scene.g_texcoord = { 1.0f, 1.0f, 0.0f, 0.0f };
    rend_data_ctx.state.write_uniform_buffer(rctx->image_filter_scene_ubo, filter_scene);

    image_filter_batch_shader_data image_filter_batch = {};
    image_filter_batch.g_color_scale = scale;
    image_filter_batch.g_color_offset = 0.0f;
    image_filter_batch.g_texture_lod = 0.0f;
    rend_data_ctx.state.write_uniform_buffer(rctx->image_filter_batch_ubo, image_filter_batch);

    rend_data_ctx.shader_flags.arr[U_IMAGE_FILTER] = 5;
    shaders_ft.set(rend_data_ctx.state, rend_data_ctx.shader_flags, SHADER_FT_IMGFILT);
    rend_data_ctx.state.bind_uniform_buffer_base(0, rctx->image_filter_scene_ubo);
    rend_data_ctx.state.bind_uniform_buffer_base(1, rctx->image_filter_batch_ubo);
    rend_data_ctx.state.active_bind_texture_2d(0, src->glid);
    rend_data_ctx.state.bind_sampler(0, rctx->render_samplers[0]);
    rend_data_ctx.state.bind_vertex_array(rctx->common_vao);
    rend_data_ctx.state.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);
    rend_data_ctx.state.end_event();
}

HOOK(void, FASTCALL, render_manager_init_render_textures, 0x0000000140502560, int32_t multisample) {
    render_manager_init_render_textures(multisample);
}

HOOK(void, FASTCALL, render_manager_free_data, 0x0000000140502770) {
    stereo_free();
    if (reflect_full_ptr)
        reflect_full_ptr->free();
    render_manager_free_render_textures();
    render_manager.render->free();

    finish_shadow();

    sss_data_get()->free();
}

HOOK(void, FASTCALL, render_manager_free_render_textures, 0x00000001405027A0) {
    render_manager_free_render_textures();
}

HOOK(void, FASTCALL, render_manager_init_data, 0x0000000140502A2A, int32_t ssaa, int32_t hd_res, int32_t ss_alpha_mask, bool npr) {
    render_manager.reset();
    render_manager.npr = npr;
    RenderTexture::init();

    resolution_struct* res_wind = res_window_get();
    resolution_struct* res_wind_int = res_window_internal_get();
    render_manager.render = render_get();
    render_manager.render->init_render_buffers(res_wind_int->width, res_wind_int->height, ssaa, hd_res, ss_alpha_mask);
    render_manager.render->set_screen_res(res_wind_int->x_offset,
        res_wind_int->y_offset, res_wind_int->width, res_wind_int->height);
    render_manager.width = res_wind->width;
    render_manager.height = res_wind->height;
    render_manager_init_render_textures(1);
    render_manager.render->init_post_process_buffers();

    init_shadow();
    render_manager.shadow_ptr = get_shadow();
    render_manager.shadow_ptr->create();

    sss_data_get()->init();

    void(FASTCALL * sub_1403B6CC0)() = (void(FASTCALL*)())0x00000001403B6CC0;
    sub_1403B6CC0();

    if (reflect_full_ptr && sv_reflect_full)
        reflect_full_ptr->init();

    glGetErrorDLL();
}

void reflect_full_init() {
    if (!reflect_full_ptr)
        reflect_full_ptr = new reflect_full_struct;
}

void reflect_full_free() {
    if (reflect_full_ptr) {
        delete reflect_full_ptr;
        reflect_full_ptr = 0;
    }
}

void render_manager_patch() {
    WRITE_NOP_6(0x0000000140502A24);
    WRITE_MEMORY(0x0000000140502A3C, uint8_t, 0xC3);

    INSTALL_HOOK(render_manager_free_data);
    INSTALL_HOOK(render_manager_init_render_textures);
    INSTALL_HOOK(render_manager_free_render_textures);
    INSTALL_HOOK(render_manager_init_data);
}

void get_reflect_mat() {
    vec4 reflect_clip_plane;
    const light_set* set = &light_set_data[LIGHT_SET_MAIN];
    const light_data* light_reflect = &set->lights[LIGHT_REFLECT];
    if (light_reflect->type == LIGHT_SPOT) {
        const vec3 spot_direction = -light_reflect->spot_direction;
        const vec3 position = *(vec3*)&light_reflect->position;

        float_t length = vec3::length(spot_direction);
        if (length > 0.000001f)
            *(vec3*)&reflect_clip_plane = spot_direction * (1.0f / length);
        else
            *(vec3*)&reflect_clip_plane = { 0.0f, 1.0f, 0.0f };

        reflect_clip_plane.w = -vec3::dot(position, *(vec3*)&reflect_clip_plane);
    }
    else if (light_reflect->type == LIGHT_POINT)
        reflect_clip_plane = { 0.0f, -1.0f, 0.0f, 9999.0f };
    else
        reflect_clip_plane = { 0.0f, -1.0f, 0.0f, 0.0f };

    const vec4 temp = 2.0f * reflect_clip_plane;

    reflect_mat = mat4_identity;
    *(vec3*)&reflect_mat.row0 -= temp.x * *(vec3*)&reflect_clip_plane;
    *(vec3*)&reflect_mat.row1 -= temp.y * *(vec3*)&reflect_clip_plane;
    *(vec3*)&reflect_mat.row2 -= temp.z * *(vec3*)&reflect_clip_plane;
    *(vec3*)&reflect_mat.row3 -= temp.w * *(vec3*)&reflect_clip_plane;
    reflect_mat.row0.w = 0.0f;
    reflect_mat.row1.w = 0.0f;
    reflect_mat.row2.w = 0.0f;
    reflect_mat.row3.w = 1.0f;
}

// 0x1403B6560
void apply_esm_filter(render_data_context& rend_data_ctx,
    RenderTexture* dst, RenderTexture* buf, RenderTexture* src,
    float_t sigma, float_t far_texel_offset, bool enable_lit_proj) {
    texture* dst_tex = dst->get_texture();
    texture* buf_tex = buf->get_texture();
    texture* src_tex = dst->get_texture();
    if (src)
        src_tex = src->get_depth_texture();

    if (!dst_tex || !buf_tex || !src_tex || dst_tex->width != buf_tex->width || dst_tex->height != buf_tex->height)
        return;

    rend_data_ctx.state.begin_event("`anonymous-namespace'::Impl::apply_esm_filter");
    esm_filter_scene_shader_data filter_scene = {};
    filter_scene.g_transform = 0.0f;
    filter_scene.g_texcoord = { 1.0f, 1.0f, 0.0f, 0.0f };
    rend_data_ctx.state.write_uniform_buffer(rctx->esm_filter_scene_ubo, filter_scene);

    esm_filter_batch_shader_data esm_filter_batch = {};
    double_t v6 = 1.0 / (sqrt(M_PI * 2.0) * sigma);
    double_t v8 = -1.0 / (2.0 * sigma * sigma);
    for (int32_t i = 0; i < 8; i++)
        ((float_t*)esm_filter_batch.g_gauss)[i] = (float_t)(exp((double_t)((ssize_t)i * i) * v8) * v6);

    rend_data_ctx.shader_flags.arr[U_LIGHT_PROJ] = enable_lit_proj ? 1 : 0;
    shaders_ft.set(rend_data_ctx.state, rend_data_ctx.shader_flags, SHADER_FT_ESMGAUSS);
    rend_data_ctx.state.bind_uniform_buffer_base(0, rctx->esm_filter_scene_ubo);
    rend_data_ctx.state.bind_uniform_buffer_base(1, rctx->esm_filter_batch_ubo);

    rend_data_ctx.state.set_viewport(0, 0, dst_tex->width, dst_tex->height);

    buf->begin_render(rend_data_ctx.state);
    esm_filter_batch.g_params = { 1.0f / (float_t)dst_tex->width, 0.0f, far_texel_offset, far_texel_offset };
    rend_data_ctx.state.write_uniform_buffer(rctx->esm_filter_batch_ubo, esm_filter_batch);

    rend_data_ctx.state.active_bind_texture_2d(0, src_tex->glid);
    rend_data_ctx.state.bind_sampler(0, rctx->render_samplers[0]);
    rend_data_ctx.state.bind_vertex_array(rctx->common_vao);
    rend_data_ctx.state.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);

    dst->begin_render(rend_data_ctx.state);
    esm_filter_batch.g_params = { 0.0f, 1.0f / (float_t)dst_tex->height, far_texel_offset, far_texel_offset };
    rend_data_ctx.state.write_uniform_buffer(rctx->esm_filter_batch_ubo, esm_filter_batch);

    rend_data_ctx.state.active_bind_texture_2d(0, buf_tex->glid);
    rend_data_ctx.state.bind_sampler(0, rctx->render_samplers[0]);
    rend_data_ctx.state.bind_vertex_array(rctx->common_vao);
    rend_data_ctx.state.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);
    shader::unbind(rend_data_ctx.state);
    rend_data_ctx.state.end_event();
}

// 0x1403B6950
void apply_esm_min_filter(render_data_context& rend_data_ctx,
    RenderTexture* dst, RenderTexture* buf, RenderTexture* src) {
    texture* dst_tex = dst->get_texture();
    texture* buf_tex = buf->get_texture();
    texture* src_tex = src->get_texture();
    rend_data_ctx.state.begin_event("`anonymous-namespace'::Impl::apply_esm_min_filter");
    if (!dst_tex || !dst_tex->glid || !buf_tex || !buf_tex->glid || !src_tex || !src_tex->glid) {
        rend_data_ctx.state.end_event();
        return;
    }

    rend_data_ctx.state.begin_event("minimize");
    esm_filter_scene_shader_data filter_scene = {};
    filter_scene.g_transform = 0.0f;
    filter_scene.g_texcoord = { 1.0f, 1.0f, 0.0f, 0.0f };
    rend_data_ctx.state.write_uniform_buffer(rctx->esm_filter_scene_ubo, filter_scene);

    esm_filter_batch_shader_data esm_filter_batch = {};
    esm_filter_batch.g_gauss[0] = 0.0f;
    esm_filter_batch.g_gauss[1] = 0.0f;

    rend_data_ctx.state.bind_uniform_buffer_base(0, rctx->esm_filter_scene_ubo);
    rend_data_ctx.state.bind_uniform_buffer_base(1, rctx->esm_filter_batch_ubo);

    rend_data_ctx.state.set_viewport(0, 0, dst_tex->width, dst_tex->height);

    buf->begin_render(rend_data_ctx.state);
    esm_filter_batch.g_params = { 1.0f / (float_t)src_tex->width,
        1.0f / (float_t)src_tex->height, 0.0f, 0.0f };
    rend_data_ctx.state.write_uniform_buffer(rctx->esm_filter_batch_ubo, esm_filter_batch);

    rend_data_ctx.shader_flags.arr[U_ESM_FILTER] = 0;
    shaders_ft.set(rend_data_ctx.state, rend_data_ctx.shader_flags, SHADER_FT_ESMFILT);
    rend_data_ctx.state.active_bind_texture_2d(0, src_tex->glid);
    rend_data_ctx.state.bind_sampler(0, rctx->render_samplers[0]);
    rend_data_ctx.state.bind_vertex_array(rctx->common_vao);
    rend_data_ctx.state.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);
    rend_data_ctx.state.end_event();

    rend_data_ctx.state.begin_event("erosion");
    dst->begin_render(rend_data_ctx.state);
    esm_filter_batch.g_params = { 0.75f / (float_t)buf_tex->width,
        0.75f / (float_t)buf_tex->height, 0.0f, 0.0f };
    rend_data_ctx.state.write_uniform_buffer(rctx->esm_filter_batch_ubo, esm_filter_batch);

    rend_data_ctx.shader_flags.arr[U_ESM_FILTER] = 1;
    shaders_ft.set(rend_data_ctx.state, rend_data_ctx.shader_flags, SHADER_FT_ESMFILT);
    rend_data_ctx.state.active_bind_texture_2d(0, buf_tex->glid);
    rend_data_ctx.state.bind_sampler(0, rctx->render_samplers[0]);
    rend_data_ctx.state.bind_vertex_array(rctx->common_vao);
    rend_data_ctx.state.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);
    rend_data_ctx.state.end_event();
    rend_data_ctx.state.end_event();
}

static bool litproj_clear(render_data_context& rend_data_ctx, cam_data& cam) {
    if (!stage_param_data_litproj_current)
        return false;

    texture* tex = texture_manager_get_texture(stage_param_data_litproj_current->tex_id);
    if (!tex)
        return false;

    litproj_texture->begin_render(rend_data_ctx.state);
    litproj_texture->set_viewport(rend_data_ctx.state);
    rend_data_ctx.state.enable_depth_test();
    rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    rend_data_ctx.state.clear_depth(1.0f);
    rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!litproj_set_mat(rend_data_ctx, cam, true)) {
        litproj_texture->end_render(rend_data_ctx.state);
        return false;
    }

    rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_LITPROJ;
    rend_data_ctx.state.active_bind_texture_2d(17, tex->glid);
    rend_data_ctx.state.bind_sampler(17, rctx->render_samplers[2]);
    rend_data_ctx.state.active_bind_texture_2d(18, litproj_shadow[0].get_texture_glid());
    rend_data_ctx.state.bind_sampler(18, rctx->render_samplers[2]);
    rend_data_ctx.state.active_texture(0);
    return true;
}

static bool litproj_set(render_data_context& rend_data_ctx, cam_data& cam) {
    if (!stage_param_data_litproj_current)
        return 0;

    static const vec4 color_clear = 0.0f;
    static const GLfloat depth_clear = 1.0f;

    litproj_shadow[0].begin_render(rend_data_ctx.state);
    rend_data_ctx.state.set_viewport(0, 0, 2048, 512);
    rend_data_ctx.state.enable_depth_test();
    rend_data_ctx.state.set_depth_mask(GL_TRUE);
    rend_data_ctx.state.clear_buffer(GL_COLOR, 0, (float_t*)&color_clear);
    rend_data_ctx.state.clear_buffer(GL_DEPTH, 0, &depth_clear);

    if (litproj_set_mat(rend_data_ctx, cam, false)) {
        rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = SHADER_FT_SIL;
        rend_data_ctx.shader_flags.arr[U_DEPTH] = 1;
        return true;
    }
    else {
        litproj_texture->begin_render(rend_data_ctx.state);
        litproj_texture->set_viewport(rend_data_ctx.state);
        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        rend_data_ctx.state.clear_buffer(GL_COLOR, 0, (float_t*)&color_clear);
        rend_data_ctx.state.clear_buffer(GL_DEPTH, 0, &depth_clear);
    }
    return false;
}

static void light_proj_get_view_proj_mat(cam_data& cam, const vec3& view_point, const vec3& interest,
    const float_t fov, const float_t aspect, const float_t min_distance, const float_t max_distance, mat4* mat) {
    cam.set_view_point(view_point);
    cam.set_interest(interest);
    cam.set_up({ 0.0f, 1.0f, 0.0f });
    cam.calc_view_mat();
    cam.set_fov(fov * DEG_TO_RAD_FLOAT);
    cam.set_aspect(aspect);
    cam.set_min_distance(min_distance);
    cam.set_max_distance(max_distance);
    if (mat) {
        cam.calc_persp_proj_mat_offset(0.5f, 0.5f);
        cam.calc_view_proj_mat();
        *mat = cam.get_view_proj_mat();
    }
    else {
        cam.calc_persp_proj_mat();
        cam.calc_view_proj_mat();
    }
}

static bool litproj_set_mat(render_data_context& rend_data_ctx, cam_data& cam, bool set_mat) {
    light_set* set = &light_set_data[LIGHT_SET_MAIN];
    light_data* data = &set->lights[LIGHT_PROJECTION];
    if (data->get_type() != LIGHT_SPOT)
        return false;

    vec3 position;
    vec3 spot_direction;
    data->get_position(position);
    data->get_spot_direction(spot_direction);

    if (vec3::length_squared(spot_direction) <= 0.000001f)
        return false;

    float_t spot_cutoff = data->get_spot_cutoff();
    float_t fov = atanf(tanf(spot_cutoff * DEG_TO_RAD_FLOAT) * 0.25f) * 2.0f;

    vec3 interest = position + spot_direction;
    if (set_mat) {
        mat4 mat;
        light_proj_get_view_proj_mat(cam, position, interest,
            fov * RAD_TO_DEG_FLOAT, 4.0f, 0.1f, 10.0f, &mat);

        mat4 mat_depth = mat;
        rend_data_ctx.set_scene_light_projection(mat, mat_depth);
    }
    else
        light_proj_get_view_proj_mat(cam, position, interest,
            fov * RAD_TO_DEG_FLOAT, 4.0f, 0.1f, 10.0f, 0);
    return true;
}

static int32_t draw_pass_3d_get_translucent_count() {
    int32_t count = 0;
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_GLITTER);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_GLITTER);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_GLITTER);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_TRANSLUCENT);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_TRANSLUCENT);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_TRANSLUCENT);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_OPAQUE);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_OPAQUE);
    count += disp_manager->get_obj_count(mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_OPAQUE);
    return count;
}

static void draw_pass_3d_translucent(render_data_context& rend_data_ctx,
    mdl::ObjType opaque, mdl::ObjType transparent, mdl::ObjType translucent, cam_data& cam) {
    if (disp_manager->get_obj_count(opaque) < 1
        && disp_manager->get_obj_count(transparent) < 1
        && disp_manager->get_obj_count(translucent) < 1)
        return;

    rndr::Render* rend = render_get();

    RenderTexture& rt = reflect_draw && reflect_full_ptr
        ? reflect_full_ptr->reflect_texture : rend->rend_texture[0];

    int32_t alpha_array[256];
    int32_t count = draw_pass_3d_translucent_count_layers(
        alpha_array, opaque, transparent, translucent, cam);
    for (int32_t i = 0; i < count; i++) {
        int32_t alpha = alpha_array[i];
        rend->begin_render_transparency(rend_data_ctx, &rt);
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_OPAQUE] && disp_manager->get_obj_count(opaque))
            disp_manager->draw(rend_data_ctx, opaque, cam, 0, true, alpha);
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_TRANSPARENT] && disp_manager->get_obj_count(transparent))
            disp_manager->draw(rend_data_ctx, transparent, cam, 0, true, alpha);
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_TRANSLUCENT] && disp_manager->get_obj_count(translucent)) {
            rend_data_ctx.state.enable_blend();
            disp_manager->draw(rend_data_ctx, translucent, cam, 0, true, alpha);
            rend_data_ctx.state.disable_blend();
        }
        rend->end_render_transparency(rend_data_ctx, &rt, (float_t)alpha * (float_t)(1.0 / 255.0));
    }
}

static void draw_pass_3d_translucent(render_data_context& rend_data_ctx,
    mdl::ObjTypeScreen opaque, mdl::ObjTypeScreen transparent, mdl::ObjTypeScreen translucent, cam_data& cam) {
    if (disp_manager->get_obj_count(opaque) < 1
        && disp_manager->get_obj_count(transparent) < 1
        && disp_manager->get_obj_count(translucent) < 1)
        return;

    rndr::Render* rend = render_get();

    RenderTexture& rt = reflect_draw && reflect_full_ptr
        ? reflect_full_ptr->reflect_texture : rend->rend_texture[0];

    int32_t alpha_array[256];
    int32_t count = draw_pass_3d_translucent_count_layers(
        alpha_array, opaque, transparent, translucent, cam);
    for (int32_t i = 0; i < count; i++) {
        int32_t alpha = alpha_array[i];
        rend->begin_render_transparency(rend_data_ctx, &rt);
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_OPAQUE] && disp_manager->get_obj_count(opaque))
            disp_manager->draw(rend_data_ctx, opaque, cam, 0, true, alpha);
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_TRANSPARENT] && disp_manager->get_obj_count(transparent))
            disp_manager->draw(rend_data_ctx, transparent, cam, 0, true, alpha);
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_TRANSLUCENT] && disp_manager->get_obj_count(translucent)) {
            rend_data_ctx.state.enable_blend();
            disp_manager->draw(rend_data_ctx, translucent, cam, 0, true, alpha);
            rend_data_ctx.state.disable_blend();
        }
        rend->end_render_transparency(rend_data_ctx, &rt, (float_t)alpha * (float_t)(1.0 / 255.0));
    }
}

static int32_t draw_pass_3d_translucent_count_layers(int32_t* alpha_array,
    mdl::ObjType opaque, mdl::ObjType transparent, mdl::ObjType translucent, cam_data& cam) {
    bool arr[0x100] = { false };

    draw_pass_3d_translucent_has_objects(arr, opaque, cam);
    draw_pass_3d_translucent_has_objects(arr, transparent, cam);
    draw_pass_3d_translucent_has_objects(arr, translucent, cam);

    int32_t count = 0;
    for (int32_t i = 0xFF; i >= 0; i--)
        if (arr[i]) {
            count++;
            *alpha_array++ = i;
        }
    return count;
}

static int32_t draw_pass_3d_translucent_count_layers(int32_t* alpha_array,
    mdl::ObjTypeScreen opaque, mdl::ObjTypeScreen transparent, mdl::ObjTypeScreen translucent, cam_data& cam) {
    bool arr[0x100] = { false };

    draw_pass_3d_translucent_has_objects(arr, opaque, cam);
    draw_pass_3d_translucent_has_objects(arr, transparent, cam);
    draw_pass_3d_translucent_has_objects(arr, translucent, cam);

    int32_t count = 0;
    for (int32_t i = 0xFF; i >= 0; i--)
        if (arr[i]) {
            count++;
            *alpha_array++ = i;
        }
    return count;
}

static void draw_pass_3d_translucent_has_objects(bool* arr, mdl::ObjType type, cam_data& cam) {
    disp_manager->calc_obj_radius(cam, type);
    for (mdl::ObjData*& i : disp_manager->get_obj_list(type))
        switch (i->kind) {
        case mdl::OBJ_KIND_NORMAL: {
            int32_t alpha = (int32_t)(i->args.sub_mesh.blend_color.w * 255.0f);
            alpha = clamp_def(alpha, 0, 255);
            arr[alpha] = true;
        } break;
        case mdl::OBJ_KIND_TRANSLUCENT: {
            for (int32_t j = 0; j < i->args.translucent.count; j++) {
                int32_t alpha = (int32_t)(i->args.translucent.sub_mesh[j]->blend_color.w * 255.0f);
                alpha = clamp_def(alpha, 0, 255);
                arr[alpha] = true;
            }
        } break;
        }
}

static void draw_pass_3d_translucent_has_objects(bool* arr, mdl::ObjTypeScreen type, cam_data& cam) {
    disp_manager->calc_obj_radius(cam, type);
    for (mdl::ObjData*& i : disp_manager->get_obj_list(type))
        switch (i->kind) {
        case mdl::OBJ_KIND_NORMAL: {
            int32_t alpha = (int32_t)(i->args.sub_mesh.blend_color.w * 255.0f);
            alpha = clamp_def(alpha, 0, 255);
            arr[alpha] = true;
        } break;
        case mdl::OBJ_KIND_TRANSLUCENT: {
            for (int32_t j = 0; j < i->args.translucent.count; j++) {
                int32_t alpha = (int32_t)(i->args.translucent.sub_mesh[j]->blend_color.w * 255.0f);
                alpha = clamp_def(alpha, 0, 255);
                arr[alpha] = true;
            }
        } break;
        }
}

static void draw_pass_reflect_full(render_data_context& rend_data_ctx, rndr::RenderManager& render_manager) {
    rend_data_ctx.state.begin_event("pass_reflect");
    RenderTexture& refl_tex = reflect_full_ptr->reflect_texture;
    RenderTexture& refl_buf_tex = reflect_full_ptr->reflect_buffer_texture;
    if (draw_pass_reflect_get_obj_reflect_surface()
        && (disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_OPAQUE)
        || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_TRANSPARENT)
        || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_TRANSLUCENT_SORT_BY_RADIUS)
        || disp_manager->get_obj_count(mdl::OBJ_TYPE_REFLECT_TRANSLUCENT))) {
        refl_tex.begin_render(rend_data_ctx.state);
        refl_tex.set_viewport(rend_data_ctx.state);

        if (!sss_data_get()->enable || !sss_data_get()->downsample
            || draw_pass_3d_get_translucent_count()) {
            rend_data_ctx.state.set_depth_mask(GL_TRUE);
            rend_data_ctx.state.clear(GL_DEPTH_BUFFER_BIT);
        }

        rend_data_ctx.state.set_depth_func(GL_LEQUAL);

        for (int32_t i = LIGHT_SET_MAIN; i < LIGHT_SET_MAX; i++)
            lighting_set(rend_data_ctx, (light_set_id)i);
        for (int32_t i = FOG_DEPTH; i < FOG_MAX; i++)
            fog_set(rend_data_ctx, (fog_id)i);

        rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
        if (disp_manager->get_obj_count(mdl::OBJ_TYPE_SSS))
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
        else
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_func(GL_LEQUAL);
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        rend_data_ctx.state.set_cull_face_mode(GL_FRONT);

        reflect_draw = true;
        rctx->draw_state_rend_data[rend_data_ctx.index].shader_index = -1;

        if (render_manager.shadow)
            render_manager.shadow_ptr->bind_shadow(rend_data_ctx);
        else
            render_manager.shadow_ptr->unbind_shadow(rend_data_ctx);

        if (render_manager.effect_texture)
            rend_data_ctx.state.active_bind_texture_2d(14, render_manager.effect_texture->glid);
        else
            rend_data_ctx.state.active_bind_texture_2d(14, rctx->empty_texture_2d->glid);

        rend_data_ctx.shader_flags.arr[U_TEX_REFLECTMAP] = 0;

        rend_data_ctx.state.bind_sampler(14, rctx->render_samplers[0]);
        rend_data_ctx.state.bind_sampler(15, rctx->render_samplers[0]);
        rend_data_ctx.state.bind_sampler(16, rctx->render_samplers[0]);

        rend_data_ctx.shader_flags.arr[U_STAGE_AMBIENT] = render_manager.light_stage_ambient ? 1 : 0;

        sss_data_get()->set_texture(rend_data_ctx.state, 3);

        rend_data_ctx.set_npr(&render_manager);
        rend_data_ctx.set_batch_sss_param(sss_param_reflect);

        cam_data reflect_cam = rctx->render_manager_cam;
        set_reflect_mat(rend_data_ctx, reflect_cam);
        rend_data_ctx.set_batch_scene_camera(reflect_cam);

        if (render_manager.alpha_z_sort) {
            disp_manager->obj_sort(rend_data_ctx,
                mdl::OBJ_TYPE_REFLECT_TRANSLUCENT, 1, reflect_cam, render_manager.field_31F);
            disp_manager->obj_sort(rend_data_ctx,
                mdl::OBJ_TYPE_REFLECT_TRANSLUCENT_SORT_BY_RADIUS, 2, reflect_cam);
        }

        if (render_manager.opaque_z_sort)
            disp_manager->obj_sort(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_OPAQUE, 0, reflect_cam);

        rend_data_ctx.shader_flags.arr[U_REFLECT] = 1;
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_OPAQUE]) {
            rend_data_ctx.state.enable_depth_test();
            rend_data_ctx.state.set_depth_mask(GL_TRUE);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_OPAQUE, reflect_cam);
            rend_data_ctx.state.disable_depth_test();
        }

        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_OPAQUE, reflect_cam);

        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        if (render_manager.draw_pass_3d[DRAW_PASS_3D_TRANSPARENT])
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_TRANSPARENT, reflect_cam);

        if (render_manager.npr_param == 1)
            render_manager.pass_3d_contour(rend_data_ctx);

        draw_pass_3d_translucent(rend_data_ctx,
            mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_OPAQUE,
            mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_OPAQUE,
            mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_OPAQUE, reflect_cam);

        rend_data_ctx.state.disable_depth_test();

        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_PRE_TRANSLUCENT, reflect_cam);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        /*if (npr_param == 1) {
            rend_data_ctx.state.set_color_mask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
            rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
            rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }*/

        rend_data_ctx.state.enable_depth_test();

        if (render_manager.draw_pass_3d[DRAW_PASS_3D_TRANSLUCENT]) {
            rend_data_ctx.state.enable_blend();
            rend_data_ctx.state.set_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            rend_data_ctx.state.set_depth_mask(GL_FALSE);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_TRANSLUCENT_SORT_BY_RADIUS, reflect_cam);
            disp_manager->draw(rend_data_ctx, mdl::OBJ_TYPE_REFLECT_TRANSLUCENT, reflect_cam);
            rend_data_ctx.state.disable_blend();
        }

        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // X
        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_POST_TRANSLUCENT, reflect_cam);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        draw_pass_3d_translucent(rend_data_ctx,
            mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_TRANSLUCENT,
            mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_TRANSLUCENT,
            mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_TRANSLUCENT, reflect_cam);
        rend_data_ctx.state.disable_depth_test();

        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        Glitter::glt_particle_manager_x->DispScenes(rend_data_ctx, Glitter::DISP_NORMAL, reflect_cam);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        rend_data_ctx.state.enable_depth_test();
        rend_data_ctx.state.set_depth_mask(GL_TRUE);
        draw_pass_3d_translucent(rend_data_ctx,
            mdl::OBJ_TYPE_OPAQUE_ALPHA_ORDER_POST_GLITTER,
            mdl::OBJ_TYPE_TRANSPARENT_ALPHA_ORDER_POST_GLITTER,
            mdl::OBJ_TYPE_TRANSLUCENT_ALPHA_ORDER_POST_GLITTER, reflect_cam);

        rend_data_ctx.state.active_bind_texture_2d(14, 0);
        rend_data_ctx.state.active_bind_texture_2d(15, 0);

        if (render_manager.shadow)
            render_manager.shadow_ptr->unbind_shadow(rend_data_ctx);

        rend_data_ctx.state.set_cull_face_mode(GL_BACK);
        rend_data_ctx.state.disable_depth_test();

        reflect_full_ptr->dof->apply(rend_data_ctx, &refl_tex, &refl_buf_tex);
        reflect_draw = false;

        rend_data_ctx.shader_flags.arr[U_REFLECT] = 0;

        for (int32_t i = render_manager.reflect_blur_num, j = 0; i > 0; i--, j++) {
            apply_blur_filter_sub(rend_data_ctx, &refl_buf_tex, &refl_tex,
                render_manager.reflect_blur_filter, 1.0f, 1.0f, 0.0f);
            image_filter_scale(rend_data_ctx, &refl_tex, refl_buf_tex.get_texture());
        }

        shader::unbind(rend_data_ctx.state);
        refl_tex.end_render(rend_data_ctx.state);
    }
    else {
        refl_tex.begin_render(rend_data_ctx.state);
        vec4 clear_color;
        rend_data_ctx.state.get_clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 0.0f);
        rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
        rend_data_ctx.state.clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        refl_tex.end_render(rend_data_ctx.state);
    }
    rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    rend_data_ctx.state.enable_depth_test();
    rend_data_ctx.state.set_depth_func(GL_LEQUAL);
    rend_data_ctx.state.set_depth_mask(GL_TRUE);
    rend_data_ctx.state.end_event();
}

static bool draw_pass_reflect_get_obj_reflect_surface() {
    return draw_pass_reflect_get_obj_reflect_surface(mdl::OBJ_TYPE_OPAQUE)
        || draw_pass_reflect_get_obj_reflect_surface(mdl::OBJ_TYPE_TRANSLUCENT)
        || draw_pass_reflect_get_obj_reflect_surface(mdl::OBJ_TYPE_TRANSLUCENT_SORT_BY_RADIUS)
        || draw_pass_reflect_get_obj_reflect_surface(mdl::OBJ_TYPE_TRANSPARENT);
}

static bool draw_pass_reflect_get_obj_reflect_surface(mdl::ObjType type) {
    mdl::ObjList& list = disp_manager->get_obj_list(type);

    for (mdl::ObjData*& i : disp_manager->get_obj_list(type))
        switch (i->kind) {
        case mdl::OBJ_KIND_NORMAL: {
            switch (i->args.sub_mesh.material->material.shader.index) {
            case SHADER_FT_WATER01: // _WATER01
            case SHADER_FT_FLOOR:   // _FLOOR
            case SHADER_FT_PUDDLE:  // _PUDDLE
                return true;
            }
        } break;
        case mdl::OBJ_KIND_TRANSLUCENT: {
            for (int32_t j = 0; j < i->args.translucent.count; j++)
                switch (i->args.translucent.sub_mesh[j]->material->material.shader.index) {
                case SHADER_FT_WATER01: // _WATER01
                case SHADER_FT_FLOOR:   // _FLOOR
                case SHADER_FT_PUDDLE:  // _PUDDLE
                    return true;
                }
        } break;
        }
    return false;
}

void apply_blur_filter_sub(render_data_context& rend_data_ctx, RenderTexture* dst, RenderTexture* src,
    ImgfBoxSampl filter, const vec2 res_scale, const vec4 scale, const vec4 offset) {
    if (!dst || !src)
        return;

    rend_data_ctx.state.begin_event("`anonymous-namespace'::Impl::apply_blur_filter_sub");

    dst->begin_render(rend_data_ctx.state);
    dst->set_viewport(rend_data_ctx.state);

    image_filter_scene_shader_data filter_scene = {};
    float_t w = res_scale.x / (float_t)src->get_width();
    float_t h = res_scale.y / (float_t)src->get_height();
    filter_scene.g_transform = { w, h, 0.0f, 0.0f };
    filter_scene.g_texcoord = { 1.0f, 1.0f, 0.0f, 0.0f };
    rend_data_ctx.state.write_uniform_buffer(rctx->image_filter_scene_ubo, filter_scene);

    image_filter_batch_shader_data image_filter_batch = {};
    if (filter == IMGF_BOX_SAMPL_32)
        image_filter_batch.g_color_scale = scale * (float_t)(1.0 / 8.0);
    else
        image_filter_batch.g_color_scale = scale * (float_t)(1.0 / 4.0);
    image_filter_batch.g_color_offset = offset;
    image_filter_batch.g_texture_lod = 0.0f;
    rend_data_ctx.state.write_uniform_buffer(rctx->image_filter_batch_ubo, image_filter_batch);

    rend_data_ctx.shader_flags.arr[U_IMAGE_FILTER] = filter == IMGF_BOX_SAMPL_32 ? 1 : 0;
    rend_data_ctx.state.bind_vertex_array(rctx->box_vao);
    shaders_ft.set(rend_data_ctx.state, rend_data_ctx.shader_flags, SHADER_FT_IMGFILT);
    rend_data_ctx.state.bind_uniform_buffer_base(0, rctx->image_filter_scene_ubo);
    rend_data_ctx.state.bind_uniform_buffer_base(1, rctx->image_filter_batch_ubo);
    rend_data_ctx.state.active_bind_texture_2d(0, src->get_texture_glid());
    rend_data_ctx.state.bind_sampler(0, rctx->render_samplers[0]);
    rend_data_ctx.state.draw_arrays(GL_TRIANGLE_STRIP, (GLint)(filter * 4LL), 4);
    rend_data_ctx.state.end_event();
}

static void fog_set(render_data_context& rend_data_ctx, fog_id id) {
    fog* f = &fog_data[id];
    fog_type type = f->get_type();
    if (type == FOG_NONE)
        return;

    float_t density = f->get_density();
    float_t start = f->get_start();
    float_t end = f->get_end();
    if (start >= end)
        start = end - 0.01f;

    vec4 params;
    params.x = density;
    params.y = start;
    params.z = end;
    params.w = 1.0f / (end - start);

    switch (id) {
    case FOG_DEPTH: {
        float_t density = f->get_density();
        float_t start = f->get_start();
        float_t end = f->get_end();
        if (start >= end)
            start = end - 0.01f;
        vec4 color;
        f->get_color(color);

        render_data_context::fog_params params = {};
        rend_data_ctx.get_scene_fog_params(params);
        params.density = density;
        params.start = start;
        params.end = end;
        params.depth_color = color;
        rend_data_ctx.set_scene_fog_params(params);
    } break;
    case FOG_HEIGHT: {
        float_t density = f->get_density();
        float_t start = f->get_start();
        float_t end = f->get_end();
        if (start >= end)
            start = end - 0.01f;
        vec4 color;
        f->get_color(color);

        render_data_context::fog_params params = {};
        rend_data_ctx.get_scene_fog_params(params);
        params.height_params.x = density;
        params.height_params.y = start;
        params.height_params.z = end;
        params.height_params.w = 1.0f / (end - start);
        params.height_color = color;
        rend_data_ctx.set_scene_fog_params(params);
    } break;
    case FOG_BUMP: {
        float_t density = f->get_density();
        float_t start = f->get_start();
        float_t end = f->get_end();
        if (start >= end)
            start = end - 0.01f;

        render_data_context::fog_params params = {};
        rend_data_ctx.get_scene_fog_params(params);
        params.bump_params.x = density;
        params.bump_params.y = start;
        params.bump_params.z = end;
        params.bump_params.w = 1.0f / (end - start);
        rend_data_ctx.set_scene_fog_params(params);
    } break;
    }
}

static void light_get_direction_from_position(vec4* pos_dir, const light_data* light, bool force) {
    if (force || light->get_type() == LIGHT_PARALLEL) {
        float_t length = vec3::length(*(vec3*)pos_dir);
        if (length <= 0.000001)
            *(vec3*)pos_dir = vec3(0.0f, 1.0f, 0.0f);
        else
            *(vec3*)pos_dir *= 1.0f / length;
        pos_dir->w = 0.0f;
    }
}

static void light_get_light_color(const light_data* light, light_data_color& value, light_id id) {
    static bool& light_chara_ambient = *(bool*)0x00000001411A0080;
    if (id || light_chara_ambient)
        value.ambient = light->ambient;
    else
        value.ambient = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (!id && false)
        value.diffuse = light->diffuse * 4.0f;
    else
        value.diffuse = light->diffuse;

    value.specular = light->specular;
}

static void lighting_set(render_data_context& rend_data_ctx, light_set_id set_id) {
    static const float_t spec_coef = (float_t)(1.0 / (1.0 - cos(18.0 * DEG_TO_RAD)));
    static const float_t luce_coef = (float_t)(1.0 / (1.0 - cos(45.0 * DEG_TO_RAD)));

    const light_set* set = &light_set_data[set_id];

    light_data_color light_color[LIGHT_MAX];
    for (int32_t i = 0; i < LIGHT_MAX; i++)
        light_get_light_color(&set->lights[i], light_color[i], (light_id)i);

    vec4 light_env_stage_diffuse = light_color[LIGHT_STAGE].diffuse;
    vec4 light_env_stage_specular = light_color[LIGHT_STAGE].specular;

    vec4 light_env_chara_diffuse = light_color[LIGHT_CHARA].diffuse;
    vec4 light_env_chara_ambient = light_color[LIGHT_CHARA].ambient;
    vec4 light_env_chara_specular = light_color[LIGHT_CHARA].specular;

    vec4 light_env_reflect_diffuse;
    vec4 light_env_reflect_ambient;
    vec4 light_env_reflect_specular;
    set->lights[LIGHT_REFLECT].get_diffuse(light_env_reflect_diffuse);
    set->lights[LIGHT_REFLECT].get_ambient(light_env_reflect_ambient);
    set->lights[LIGHT_REFLECT].get_specular(light_env_reflect_specular);

    vec4 light_env_proj_diffuse;
    vec4 light_env_proj_specular;
    vec4 light_env_proj_position;
    set->lights[LIGHT_PROJECTION].get_diffuse(light_env_proj_diffuse);
    set->lights[LIGHT_PROJECTION].get_specular(light_env_proj_specular);
    set->lights[LIGHT_PROJECTION].get_position(light_env_proj_position);
    if (set->lights[LIGHT_PROJECTION].get_type() == LIGHT_PARALLEL)
        light_get_direction_from_position(&light_env_proj_position, 0, true);

    vec4 light_chara_dir;
    vec4 light_chara_ibl_color0;
    vec4 light_chara_ibl_color1;
    vec4 light_chara_specular;
    set->lights[LIGHT_CHARA].get_position(light_chara_dir);
    light_get_direction_from_position(&light_chara_dir, &set->lights[LIGHT_CHARA]);
    set->lights[LIGHT_CHARA].get_ibl_color0(light_chara_ibl_color0);
    set->lights[LIGHT_CHARA].get_ibl_color1(light_chara_ibl_color1);
    set->lights[LIGHT_CHARA].get_specular(light_chara_specular);

    vec4 light_chara_spec = light_chara_specular * light_chara_ibl_color0 * spec_coef;
    vec4 light_chara_luce = light_chara_ibl_color0 * luce_coef;
    vec4 light_chara_back = light_chara_specular * light_chara_ibl_color1 * spec_coef;

    vec4 light_stage_dir;
    vec4 light_stage_ibl_color;
    vec4 light_stage_diffuse;
    vec4 light_stage_specular;
    set->lights[LIGHT_STAGE].get_position(light_stage_dir);
    light_get_direction_from_position(&light_stage_dir, &set->lights[LIGHT_STAGE]);
    set->lights[LIGHT_STAGE].get_ibl_color0(light_stage_ibl_color);
    set->lights[LIGHT_STAGE].get_diffuse(light_stage_diffuse);
    set->lights[LIGHT_STAGE].get_specular(light_stage_specular);

    vec4 light_stage_diff = light_stage_diffuse * light_stage_ibl_color;
    vec4 light_stage_spec = light_stage_specular * light_stage_ibl_color * spec_coef;

    mat4 irradiance_r_transforms;
    mat4 irradiance_g_transforms;
    mat4 irradiance_b_transforms;
    set->get_irradiance(irradiance_r_transforms, irradiance_g_transforms, irradiance_b_transforms);

    float_t* (FASTCALL * face_data_get)() = (float_t * (FASTCALL*)())0x00000001403D8310;
    float_t v27 = face_data_get()[0];
    float_t v28 = (1.0f - expf(v27 * -0.45f)) * 2.0f;
    v27 = max_def(v27, 0.0f);
    vec4 light_face_diff = { v28 * 0.1f, v27 * 0.06f, 1.0f, 1.0f };

    vec4 chara_color_rim;
    vec4 chara_color0;
    vec4 chara_color1;
    if (set->lights[LIGHT_CHARA_COLOR].get_type() == LIGHT_PARALLEL) {
        set->lights[LIGHT_CHARA_COLOR].get_ambient(chara_color_rim);
        set->lights[LIGHT_CHARA_COLOR].get_diffuse(chara_color0);
        set->lights[LIGHT_CHARA_COLOR].get_specular(chara_color1);
        if (chara_color1.w > 1.0f)
            chara_color1.w = 1.0f;
    }
    else {
        chara_color_rim = 0.0f;
        chara_color0 = 0.0f;
        chara_color1 = 0.0f;
    }

    vec4 chara_f_dir;
    vec4 chara_f_ambient;
    vec4 chara_f_diffuse;
    vec4 chara_tc_param;
    if (set->lights[LIGHT_TONE_CURVE].get_type() == LIGHT_PARALLEL) {
        rend_data_ctx.shader_flags.arr[U_TONE_CURVE] = 1;
        set->lights[LIGHT_TONE_CURVE].get_position(chara_f_dir);
        light_get_direction_from_position(&chara_f_dir, &set->lights[LIGHT_TONE_CURVE]);
        set->lights[LIGHT_TONE_CURVE].get_ambient(chara_f_ambient);
        set->lights[LIGHT_TONE_CURVE].get_diffuse(chara_f_diffuse);

        light_tone_curve tone_curve;
        set->lights[LIGHT_TONE_CURVE].get_tone_curve(tone_curve);
        chara_tc_param.x = tone_curve.start_point;
        float_t end_point = min_def(tone_curve.end_point, 1.0f) - tone_curve.start_point;
        if (end_point > 0.0f)
            chara_tc_param.y = 1.0f / end_point;
        else
            chara_tc_param.y = 0.0f;
        chara_tc_param.z = tone_curve.coefficient;
        chara_tc_param.w = 0.0f;
    }
    else {
        rend_data_ctx.shader_flags.arr[U_TONE_CURVE] = 0;
        chara_f_dir = { 0.0f, 1.0f, 0.0f, 0.0f };
        chara_f_ambient = 0.0f;
        chara_f_diffuse = 0.0f;
        chara_tc_param = 0.0f;
    }

    vec4 light_reflect_dir;
    set->lights[LIGHT_CHARA].get_position(light_reflect_dir);
    if (fabsf(light_reflect_dir.x) <= 0.000001f && fabsf(light_reflect_dir.y) <= 0.000001f
        && fabsf(light_reflect_dir.z) <= 0.000001f)
        *(vec3*)&light_reflect_dir = vec3(0.0f, 1.0f, 0.0f);
    else {
        float_t length = vec3::length(*(vec3*)&light_reflect_dir);
        if (length != 0.0f)
            *(vec3*)&light_reflect_dir *= 1.0f / length;
    }
    light_reflect_dir.w = 1.0f;

    vec4 clip_plane;
    if (set->lights[LIGHT_REFLECT].get_type() == LIGHT_SPOT) {
        vec3 spot_direction;
        vec4 position;
        set->lights[LIGHT_REFLECT].get_spot_direction(spot_direction);
        set->lights[LIGHT_REFLECT].get_position(position);
        *(vec3*)&clip_plane = -spot_direction;
        light_get_direction_from_position(&clip_plane, &set->lights[LIGHT_REFLECT], true);
        clip_plane.w = -vec3::dot(*(vec3*)&position, *(vec3*)&clip_plane);
    }
    else if (set->lights[LIGHT_REFLECT].get_type() == LIGHT_POINT)
        clip_plane = { 0.0f, -1.0f, 0.0f, 9999.0f };
    else
        clip_plane = { 0.0f, -1.0f, 0.0f, 0.0f };

    vec4 light_chara_ibl_direction;
    vec4 light_chara_position;
    set->lights[LIGHT_CHARA].get_ibl_direction(light_chara_ibl_direction);
    set->lights[LIGHT_CHARA].get_position(light_chara_position);

    mat4 normal_tangent_transforms = mat4_identity;

    float_t length = vec3::length(*(vec3*)&light_chara_ibl_direction);
    if (length >= 0.000001f) {
        vec3 ibl_direction = *(vec3*)&light_chara_ibl_direction * (1.0f / length);

        length = vec3::length(*(vec3*)&light_chara_position);
        if (length >= 0.000001f) {
            vec3 position = *(vec3*)&light_chara_position * (1.0f / length);

            vec3 axis = vec3::cross(ibl_direction, position);
            length = vec3::length(axis);

            float_t v52 = vec3::dot(ibl_direction, position);
            float_t angle = fabsf(atan2f(length, v52));
            if (angle >= 0.01f && angle <= 3.131592653589793f) {
                if (length != 0.0f)
                    axis *= 1.0f / length;

                mat4_set(&axis, -angle, &normal_tangent_transforms);
            }
        }
    }

    static vec4& npr_cloth_spec_color = *(vec4*)0x0000000140C9B2A0;

    rend_data_ctx.set_scene_light(irradiance_r_transforms, irradiance_g_transforms, irradiance_b_transforms,
        light_env_stage_diffuse, light_env_stage_specular,
        light_env_chara_diffuse, light_env_chara_ambient, light_env_chara_specular,
        light_env_reflect_diffuse, light_env_reflect_ambient, light_env_reflect_specular,
        light_env_proj_diffuse, light_env_proj_specular, light_env_proj_position,
        light_stage_dir, light_stage_diff, light_stage_spec,
        light_chara_dir, light_chara_spec, light_chara_luce, light_chara_back,
        light_face_diff, chara_color0, chara_color1, chara_f_dir, chara_f_ambient,
        chara_f_diffuse, chara_tc_param, normal_tangent_transforms,
        light_reflect_dir, clip_plane, npr_cloth_spec_color);

    extern const GLuint* light_param_data_storage_data_get_ibl_textures();
    const GLuint* textures = light_param_data_storage_data_get_ibl_textures();
    static const int32_t ibl_texture_index[] = {
        9, 10, 11, 12, 13
    };

    for (int32_t i = 0; i < 5; i++)
        rend_data_ctx.state.active_bind_texture_cube_map(ibl_texture_index[i], textures[i]);
}

static void render_manager_free_render_textures() {
    if (render_manager.multisample_renderbuffer) {
        glDeleteRenderbuffers(1, &render_manager.multisample_renderbuffer);
        render_manager.multisample_renderbuffer = 0;
    }

    if (render_manager.multisample_framebuffer) {
        glDeleteFramebuffers(1, &render_manager.multisample_framebuffer);
        render_manager.multisample_framebuffer = 0;
    }

    for (RenderTexture& i : render_manager.render_textures)
        i.destroy();
}

static void render_manager_init_render_textures(int32_t multisample) {
    if (multisample) {
        glGenFramebuffers(1, &render_manager.multisample_framebuffer);
        glGenRenderbuffers(1, &render_manager.multisample_renderbuffer);

        gl_state.bind_framebuffer(render_manager.multisample_framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, render_manager.multisample_renderbuffer);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 8,
            GL_RGBA8, render_manager.width, render_manager.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_RENDERBUFFER, render_manager.multisample_renderbuffer);
        glDrawBufferDLL(GL_COLOR_ATTACHMENT0);
        glDrawBufferDLL(GL_COLOR_ATTACHMENT0);
        gl_state.bind_framebuffer(0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        if (glGetErrorDLL()) {
            glDeleteRenderbuffers(1, &render_manager.multisample_renderbuffer);
            render_manager.multisample_renderbuffer = 0;
            glDeleteFramebuffers(1, &render_manager.multisample_framebuffer);
            render_manager.multisample_framebuffer = 0;
        }
    }

    for (int32_t i = 0; i < 9; i++) {
        const rndr::RenderTextureData* tex_data = &rndr::render_manager_render_texture_data_array[i];
        if (tex_data->type != GL_TEXTURE_2D)
            continue;

        RenderTexture& rt = render_manager.render_textures[i];
        rt.create_texture(tex_data->width, tex_data->height, tex_data->max_level,
            tex_data->color_format, tex_data->depth_format);
        rt.begin_render(gl_state);
        glClearColorDLL(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDLL(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    gl_state.bind_framebuffer(0);
}

static void set_reflect_mat(render_data_context& rend_data_ctx, cam_data& cam) {
    const vec3 clip_plane = *(vec3*)&rend_data_ctx.data.buffer_scene_data.g_clip_plane;
    const vec4 temp = 2.0f * rend_data_ctx.data.buffer_scene_data.g_clip_plane;

    reflect_mat = mat4_identity;
    *(vec3*)&reflect_mat.row0 -= temp.x * clip_plane;
    *(vec3*)&reflect_mat.row1 -= temp.y * clip_plane;
    *(vec3*)&reflect_mat.row2 -= temp.z * clip_plane;
    *(vec3*)&reflect_mat.row3 -= temp.w * clip_plane;
    reflect_mat.row0.w = 0.0f;
    reflect_mat.row1.w = 0.0f;
    reflect_mat.row2.w = 0.0f;
    reflect_mat.row3.w = 1.0f;

    mat4_mul(&reflect_mat, &cam.view_mat, &cam.view_mat);
    cam.calc_view_proj_mat();

    vec3 view_point = cam.get_view_point();
    mat4_transform_point(&reflect_mat, &view_point, &view_point);
    cam.set_view_point(view_point);
}
