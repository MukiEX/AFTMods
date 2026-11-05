/*
    Side-by-side stereoscopic rendering for DivaGL

    How it works:
    - RenderManager::render_all runs the whole render pipeline (shadow, SSS, reflections,
      3D, post-process, sprites) once per eye with an offset camera.
    - Eye cameras are parallel (no toe-in) with an asymmetric frustum, so objects at the
      convergence distance land on the screen plane with zero parallax.
    - After each eye, screen_buffer (final composited frame) is scaled into its half of
      an SBS buffer; the SBS buffer is then copied to screen_buffer and the backbuffer.

    All matrices here follow KKdLib's row-vector convention (v * View * Proj,
    translation in row3), which is what cam_data stores.
*/

#include "stereo.hpp"
#include "camera_render.hpp"
#include "render_context.hpp"
#include "render_texture.hpp"
#include "shared.hpp"
#include "static_var.hpp"
#include "../AFTModsShared/rob/rob.hpp"

extern render_context* rctx;

float_t sprite_stereo_offset[4] = {};

// Heap-allocated so no GL/texture release runs from a static destructor at process exit.
static RenderTexture* stereo_buffer = 0;
static int32_t stereo_buffer_width = 0;
static int32_t stereo_buffer_height = 0;

static int32_t stereo_current_eye = 0;

// Updated once per frame from the center camera
static float_t stereo_half_sep = 0.0f;     // half the eye separation in use, world units
static float_t stereo_conv_dist = 1.0f;    // convergence (zero parallax) distance, world units
static float_t stereo_conv_smoothed = 0.0f; // auto convergence, eased between frames (0 = none yet)
static vec3 stereo_last_view_point = 0.0f;
static vec3 stereo_last_interest = 0.0f;

bool stereo_enabled() {
    return sv_stereo_mode > STEREO_MODE_OFF && sv_stereo_mode < STEREO_MODE_MAX;
}

bool stereo_in_eye_pass() {
    return stereo_current_eye != 0;
}

// The right eye renders second (render_all_stereo: -1, then +1).
bool stereo_reuse_shadow_maps() {
    return sv_stereo_shadow_once && stereo_current_eye > 0;
}

// Depths (along the view axis) of the visible characters in front of the camera. Returns
// the one nearest the middle of the picture (the one the shot is framing) in *framed and the
// nearest one in *nearest; false if none is in view.
static bool stereo_chara_depths(const cam_data& center, const vec3& forward, const vec3& right,
    const vec3& up, float_t min_depth, float_t* framed, float_t* nearest) {
    const float_t tan_y = tanf(center.fov * 0.5f);
    const float_t tan_x = tan_y * center.aspect;
    size_t rob_chara_smth = get_rob_chara_smth();
    float_t best = 1e30f;
    bool found = false;
    *nearest = 1e30f;
    for (int32_t i = 0; i < ROB_CHARA_COUNT; i++) {
        if (!rob_chara_pv_data_array_check_chara_id(rob_chara_smth, i)
            || !rob_chara_array_check_visibility(rob_chara_smth, i))
            continue;
        const mat4* mat = rob_chara_bone_data_get_mats_mat(rob_chara_array_get_bone_data(rob_chara_smth, i), 0);
        if (!mat)
            continue;
        mat4 temp;
        mat4_transpose(mat, &temp);
        vec3 pos;
        mat4_get_translation(&temp, &pos);
        // The root bone is at the hips; the face and chest are what the eye goes to.
        pos = pos + up * 0.35f;
        const vec3 rel = pos - center.view_point;
        const float_t depth = vec3::dot(rel, forward);
        if (depth < min_depth)
            continue;
        // How far off the middle of the picture, as a fraction of the half-width/height
        const float_t off_x = fabsf(vec3::dot(rel, right)) / (depth * tan_x);
        const float_t off_y = fabsf(vec3::dot(rel, up)) / (depth * tan_y);
        if (off_x > 1.2f || off_y > 1.2f)
            continue; // out of the shot
        const float_t off = off_x * off_x + off_y * off_y;
        if (off < best) {
            best = off;
            *framed = depth;
        }
        *nearest = min_def(*nearest, depth);
        found = true;
    }
    return found;
}

void stereo_update_camera(const cam_data& center) {
    if (!stereo_enabled()) {
        stereo_half_sep = 0.0f;
        stereo_conv_smoothed = 0.0f;
        return;
    }

    const mat4& view = center.view_mat;
    const vec3 forward = -vec3(view.row0.z, view.row1.z, view.row2.z);
    const vec3 right(view.row0.x, view.row1.x, view.row2.x);
    const vec3 up(view.row0.y, view.row1.y, view.row2.y);

    // Keep the convergence plane sane: never closer than the near plane or 10 cm,
    // never so far that the stereo shift vanishes.
    const float_t min_conv = max_def(center.min_distance * 2.0f, 0.1f);

    float_t nearest_chara = 1e30f;
    float_t conv;
    if (sv_stereo_convergence > 0)
        conv = (float_t)sv_stereo_convergence * 0.01f;
    else {
        // Auto: converge on the character the shot is framing, which puts the singer on the
        // screen plane even when the camera aims past her (at the stage, a dancer behind, the
        // sky). Without a character in view, the camera's interest point.
        float_t framed = 0.0f;
        if (stereo_chara_depths(center, forward, right, up, min_conv, &framed, &nearest_chara))
            conv = framed;
        else
            conv = vec3::dot(center.interest - center.view_point, forward);
        conv = clamp_def(conv, min_conv, 1000.0f);

        // Ease between frames so a character walking towards the camera doesn't make the
        // depth twitch, but follow a camera cut (or a big jump) at once.
        const bool cut = vec3::distance(center.view_point, stereo_last_view_point) > 0.5f
            || vec3::distance(center.interest, stereo_last_interest) > 1.0f;
        if (stereo_conv_smoothed <= 0.0f || cut || fabsf(conv / stereo_conv_smoothed - 1.0f) > 0.5f)
            stereo_conv_smoothed = conv;
        else
            stereo_conv_smoothed += (conv - stereo_conv_smoothed) * 0.15f;
        conv = stereo_conv_smoothed;
        stereo_last_view_point = center.view_point;
        stereo_last_interest = center.interest;
    }
    stereo_conv_dist = clamp_def(conv, min_conv, 1000.0f);

    // Separation, limited so the two pictures never drift too far apart on screen. The
    // on-screen offset of a point at depth z, as a fraction of the picture's width, is
    // half_sep * p00 * |1/z - 1/conv|: zoomed-in shots (large p00) and a far convergence
    // plane make the background split; a character well in front of it splits the other way.
    float_t half_sep = (float_t)sv_stereo_separation * 0.001f * 0.5f;
    if (sv_stereo_max_parallax > 0) {
        const float_t p00 = 1.0f / (center.aspect * tanf(center.fov * 0.5f));
        const float_t limit = (float_t)sv_stereo_max_parallax * 0.001f;
        float_t worst = 1.0f / stereo_conv_dist;                       // the background (infinity)
        if (nearest_chara < stereo_conv_dist)
            worst = max_def(worst, 1.0f / nearest_chara - 1.0f / stereo_conv_dist);
        half_sep = min_def(half_sep, limit / (p00 * worst));
    }
    stereo_half_sep = half_sep;
}

float_t stereo_cull_margin(float_t depth) {
    if (!stereo_enabled() || stereo_half_sep <= 0.0f)
        return 0.0f;

    // An eye frustum differs from the center one by at most half_sep in front of the
    // convergence plane, and by half_sep * (depth / conv - 1) behind it.
    return stereo_half_sep * max_def(1.0f, depth / stereo_conv_dist);
}

static float_t stereo_proj_shift(int32_t eye_sign, float_t fov_y, float_t aspect) {
    if (!eye_sign)
        return 0.0f;

    // NDC x of a point at the convergence distance on the center axis is
    // -p00 * e / conv in the eye camera; shift it back to 0.
    const float_t p00 = 1.0f / (aspect * tanf(fov_y * 0.5f));
    const float_t e = stereo_half_sep * (float_t)eye_sign;
    return e * p00 / stereo_conv_dist;
}

vec2 stereo_get_proj_offset(float_t fov_y, float_t aspect) {
    return vec2(stereo_proj_shift(stereo_current_eye, fov_y, aspect), 0.0f);
}

void stereo_make_eye_cam(const cam_data& center, cam_data& eye, int32_t eye_sign) {
    eye = center;

    const float_t e = stereo_half_sep * (float_t)eye_sign;
    const mat4& view = center.view_mat;
    const vec3 right(view.row0.x, view.row1.x, view.row2.x);

    eye.view_point = center.view_point + right * e;
    eye.interest = center.interest + right * e;

    // Moving the camera by +e along its own X axis shifts everything in view space by -e.
    eye.view_mat.row3.x -= e;

    // Rebuild projection without TAA jitter, with the asymmetric (off-axis) shift.
    eye.calc_persp_proj_mat_offset(1.0f,
        vec2(stereo_proj_shift(eye_sign, eye.fov, eye.aspect), 0.0f));
    eye.calc_view_proj_mat();
}

static void stereo_buffer_check(int32_t width, int32_t height) {
    if (!stereo_buffer)
        stereo_buffer = new RenderTexture;

    if (stereo_buffer_width == width && stereo_buffer_height == height && stereo_buffer->m_txhd_color)
        return;

    stereo_buffer->create_texture(width, height, 0, GL_RGBA8, 0);
    stereo_buffer_width = width;
    stereo_buffer_height = height;
}

void stereo_eye_begin(int32_t eye_sign) {
    stereo_current_eye = eye_sign;

    const cam_data& cam = rctx->render_manager_cam;

    // HUD: user-set parallax (0 = on the screen plane).
    sprite_stereo_offset[0] = (float_t)eye_sign * (float_t)sv_stereo_hud_parallax * 0.001f;

    // Sprites drawn before the 3D (2D stage backgrounds, movie PV video) are placed at
    // infinity, which is where they belong behind a 3D scene.
    sprite_stereo_offset[2] = sv_stereo_bg_infinity
        ? stereo_proj_shift(eye_sign, cam.fov, cam.aspect) : 0.0f;

    sprite_stereo_offset[1] = 0.0f;
    sprite_stereo_offset[3] = sprite_stereo_offset[0];
}

void stereo_eye_end(int32_t eye_sign) {
    const int32_t width = rctx->screen_width;
    const int32_t height = rctx->screen_height;
    if (width <= 1 || height <= 0)
        return;

    stereo_buffer_check(width, height);

    render_data_context rend_data_ctx(GL_REND_STATE_POST_2D);
    rend_data_ctx.state.get();
    rend_data_ctx.state.begin_event("stereo_eye_composite");

    // First eye clears the target (letterbox bars in full SBS mode).
    if (eye_sign < 0) {
        stereo_buffer->begin_render(rend_data_ctx.state);
        vec4 clear_color;
        rend_data_ctx.state.get_clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        rend_data_ctx.state.set_color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        rend_data_ctx.state.clear_color(0.0f, 0.0f, 0.0f, 1.0f);
        rend_data_ctx.state.clear(GL_COLOR_BUFFER_BIT);
        rend_data_ctx.state.clear_color(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        stereo_buffer->end_render(rend_data_ctx.state);
    }

    const bool left_half = (eye_sign < 0) != sv_stereo_swap_eyes;
    const int32_t half_width = width / 2;
    const int32_t half_x = left_half ? 0 : width - half_width;

    // The game's picture is 16:9 and sits centred in the screen buffer, with black bars on
    // screens of any other shape (an ultrawide, say). Take only that picture, so the bars
    // aren't copied into each eye as well.
    int32_t src_x = 0, src_y = 0, src_w = width, src_h = height;
    if (sv_stereo_content_aspect > 0) {
        const double aspect = (double)sv_stereo_content_aspect / 1000.0;
        if ((double)width / height > aspect + 0.01) {
            src_w = (int32_t)(height * aspect + 0.5);
            src_x = (width - src_w) / 2;
        }
        else if ((double)width / height < aspect - 0.01) {
            src_h = (int32_t)(width / aspect + 0.5);
            src_y = (height - src_h) / 2;
        }
    }

    // Half SBS: the picture is squeezed into its half (the display stretches it back).
    // Full SBS: the picture keeps its shape and is fitted into its half, with bars only
    // where the half's shape differs from the picture's.
    int32_t dst_x = half_x, dst_y = 0, dst_w = half_width, dst_h = height;
    if (sv_stereo_mode == STEREO_MODE_SBS_FULL) {
        const double src_aspect = (double)src_w / src_h;
        if ((double)half_width / height > src_aspect) {
            dst_w = (int32_t)(height * src_aspect + 0.5);
            dst_x = half_x + (half_width - dst_w) / 2;
        }
        else {
            dst_h = (int32_t)(half_width / src_aspect + 0.5);
            dst_y = (height - dst_h) / 2;
        }
    }

    fbo_blit(rend_data_ctx.state, rctx->screen_buffer.get_fb(), stereo_buffer->get_fb(),
        src_x, src_y, src_w, src_h,
        dst_x, dst_y, dst_w, dst_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    rend_data_ctx.state.bind_framebuffer(0);
    rend_data_ctx.state.end_event();
    rend_data_ctx.state.finish();

    stereo_current_eye = 0;
}

void stereo_present() {
    for (float_t& i : sprite_stereo_offset)
        i = 0.0f;
    stereo_current_eye = 0;

    const int32_t width = rctx->screen_width;
    const int32_t height = rctx->screen_height;
    if (!stereo_buffer || !stereo_buffer->m_txhd_color || stereo_buffer_width != width || stereo_buffer_height != height)
        return;

    render_data_context rend_data_ctx(GL_REND_STATE_POST_2D);
    rend_data_ctx.state.get();
    rend_data_ctx.state.begin_event("stereo_present");

    // Copy back into screen_buffer so screenshots/captures get the SBS frame too.
    fbo_blit(rend_data_ctx.state, stereo_buffer->get_fb(), rctx->screen_buffer.get_fb(),
        0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    fbo_blit(rend_data_ctx.state, stereo_buffer->get_fb(), 0,
        0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    rend_data_ctx.state.bind_framebuffer(0);
    rend_data_ctx.state.end_event();
    rend_data_ctx.state.finish();
}

void stereo_free() {
    if (stereo_buffer) {
        delete stereo_buffer;
        stereo_buffer = 0;
    }
    stereo_buffer_width = 0;
    stereo_buffer_height = 0;
}
