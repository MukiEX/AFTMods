/*
    by korenkonder
    GitHub/GitLab: korenkonder
*/

#include "static_var.hpp"

int32_t sv_max_texture_size = 2048;
int32_t sv_max_texture_max_anisotropy = 1;
int32_t sv_max_uniform_buffer_size = 0x10000;
int32_t sv_max_storage_buffer_size = 0x8000000;
int32_t sv_min_uniform_buffer_alignment = 0x100;
int32_t sv_min_storage_buffer_alignment = 0x100;

bool sv_reflect_full = true;
int32_t sv_reflect_res_scale = 100;
int32_t sv_litproj_res = 2;
int32_t sv_litproj_shadow_scale = 1;

bool sv_shared_storage_uniform_buffer = false;

bool sv_task_movie_player_no_interop = false;

bool sv_texture_skinning_buffer = false;

// 3D stereo (and the VR HUD layer below)
int32_t sv_stereo_mode = 0;          // 0 = off, 1 = half SBS, 2 = full SBS
int32_t sv_stereo_separation = 65;   // Eye separation, mm (world units are metres)
int32_t sv_stereo_convergence = 0;   // Zero-parallax distance, cm (0 = auto, camera interest)
bool sv_stereo_swap_eyes = false;    // Right eye on the left (cross-eyed viewing)
int32_t sv_stereo_hud_parallax = 0;  // HUD parallax, 0.1% of image width (+ = behind screen)
bool sv_stereo_bg_infinity = true;  // Push pre-3D sprites (2D backgrounds, PV movies) to infinity
bool sv_stereo_shadow_once = true;

// PV frame skip, logging and the VR HUD layer (DivaLinux.ini [performance], [vr])
int32_t sv_pv_frame_skip = 1;      // [performance] PV (3D) drawn less often than the HUD: 0 off, 1 auto (only when a frame runs late), 2-4 = every Nth frame
bool sv_perf_log = true;           // [performance] a frame-time line in the log every 10 seconds
bool sv_vr_hud_layer = false;      // [vr] hud_layer: in a headset, the HUD as its own layer in front of the PV (off: see README)
int32_t sv_stereo_max_parallax = 20; // most the eyes' pictures may differ, 0.1% of the picture width (0 = no limit)
int32_t sv_stereo_content_aspect = 1778; // Game picture aspect x1000 (16:9); 0 = use the whole screen
