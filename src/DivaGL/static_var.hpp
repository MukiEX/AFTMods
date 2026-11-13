/*
    by korenkonder
    GitHub/GitLab: korenkonder
*/

#pragma once

#include "../KKdLib/default.hpp"

extern int32_t sv_max_texture_size;
extern int32_t sv_max_texture_max_anisotropy;
extern int32_t sv_max_uniform_buffer_size;
extern int32_t sv_max_storage_buffer_size;
extern int32_t sv_min_uniform_buffer_alignment;
extern int32_t sv_min_storage_buffer_alignment;

extern bool sv_reflect_full;
extern int32_t sv_reflect_res_scale;
extern int32_t sv_litproj_res;         // DivaLinux: light projection target: 0 the game's 1280x720, 1 half the scene's, 2 the scene's (default)
extern int32_t sv_litproj_shadow_scale; // DivaLinux: light projection shadow map, x the game's 2048x512

extern bool sv_shared_storage_uniform_buffer;

extern bool sv_task_movie_player_no_interop;

extern bool sv_texture_skinning_buffer;

// 3D stereo
extern int32_t sv_stereo_mode;
extern int32_t sv_stereo_separation;
extern int32_t sv_stereo_convergence;
extern bool sv_stereo_swap_eyes;
extern int32_t sv_stereo_hud_parallax;
extern int32_t sv_stereo_content_aspect;
extern bool sv_stereo_bg_infinity;
extern bool sv_stereo_shadow_once;
extern bool sv_stereo_hud_once;   // DivaLinux: SBS stereo draws the HUD once, not per eye
extern int32_t sv_stereo_max_parallax;
extern int32_t sv_pv_frame_skip;   // PV frame skip
extern bool sv_vr_hud_layer;   // 3D stereo: VR HUD layer
extern bool sv_perf_log;   // DivaLinux: frame-time log
