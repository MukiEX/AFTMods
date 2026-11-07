/*
    Side-by-side stereoscopic rendering for DivaGL
*/

#pragma once

#include "../KKdLib/default.hpp"
#include "../KKdLib/vec.hpp"

enum stereo_mode {
    STEREO_MODE_OFF      = 0, // Regular mono rendering
    STEREO_MODE_SBS_HALF = 1, // Each eye squeezed into half the width (3D TVs, XR glasses)
    STEREO_MODE_SBS_FULL = 2, // Each eye keeps its aspect ratio, letterboxed (cross/parallel viewing, VR players)
    STEREO_MODE_MAX,
};

struct cam_data;

// Per-sprite-list horizontal offset in NDC, applied by SpriteManager::Draw.
// Indices match sprite_manager_draw's index (0 = HUD, 1 = surface, 2 = pre-3D/background).
extern thread_local float_t sprite_stereo_offset[4];

// True when the user has stereo enabled (regardless of whether a frame is being rendered).
extern bool stereo_enabled();

// Back to no eye and no sprite offsets.
extern void stereo_eye_reset();

// True while one of the two eye passes is being rendered.
extern bool stereo_in_eye_pass();

// 0 outside stereo and for the left eye, 1 for the right eye.
extern int32_t stereo_eye_index();

// True during the second eye when its shadow maps can be the first eye's (stereo_shadow_once).
extern bool stereo_reuse_shadow_maps();

// Called once per frame after the game camera has been computed.
// Updates convergence and the culling margin.
extern void stereo_update_camera(const cam_data& center);

// Margin (world units) to widen the side planes of the center-camera culling frustum by
// for an object at the given view depth, so both eye frustums are covered.
extern float_t stereo_cull_margin(float_t depth);

// Projection offset (NDC) for the eye currently being rendered, for a camera with the given fov.
// Returns 0 outside of eye passes.
extern vec2 stereo_get_proj_offset(float_t fov_y, float_t aspect);

// Builds the camera for one eye (eye_sign: -1 left, +1 right) from the center camera.
extern void stereo_make_eye_cam(const cam_data& center, cam_data& eye, int32_t eye_sign);

// Frame orchestration, called from RenderManager::render_all.
extern void stereo_eye_begin(int32_t eye_sign);
extern void stereo_eye_end(int32_t eye_sign);
extern void stereo_present();

extern void stereo_free();
