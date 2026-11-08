/*
    DivaVK - public API.

    DivaVK implements the subset of OpenGL 4.3 used by DivaGL and PD Loader's game on top of
    Vulkan. The core library is platform independent; a platform layer (the PD Loader plugin
    on Windows/Wine, or a test harness) creates the context, supplies a VkSurfaceKHR if it
    wants on-screen output, routes GL entry points through dvk_get_proc_address and calls
    dvk_swap_buffers once per frame.
*/

#ifndef DIVAVK_H
#define DIVAVK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkSurfaceKHR_T* dvk_surface;
typedef struct dvk_context_t* dvk_context;   /* a shared context (dvk_context_create_shared) */   /* VkSurfaceKHR */
typedef struct VkInstance_T* dvk_instance;     /* VkInstance */

typedef void (*dvk_log_func)(const char* message, void* user);

/* Called after the instance is created so the platform layer can create its surface. */
typedef dvk_surface (*dvk_create_surface_func)(dvk_instance instance, void* user);

typedef struct dvk_create_info {
    const char* app_name;
    int32_t width;                       /* default framebuffer size */
    int32_t height;
    bool enable_validation;              /* VK_LAYER_KHRONOS_validation if present */
    bool vsync;
    const char* const* instance_extensions; /* extra instance extensions (surface, ...) */
    uint32_t instance_extension_count;
    dvk_create_surface_func create_surface; /* optional; NULL = offscreen only */
    void* user;
    dvk_log_func log;
    const char* pipeline_cache_path;     /* optional, persisted on destroy */
} dvk_create_info;

typedef struct dvk_stats {
    uint64_t frames;
    uint64_t programs_linked;
    uint64_t draws;
    uint64_t render_passes;
    uint64_t render_pass_breaks;
    uint64_t pipelines_created;
    uint64_t hoisted_uploads;
    uint64_t inline_uploads;
} dvk_stats;

/* Creates the (single) context and makes it current. Returns false on failure. */
bool dvk_create(const dvk_create_info* info);
void dvk_destroy(void);

/* Returns the DivaVK implementation of a GL function, or a logging stub for
   functions that aren't implemented yet. Never returns NULL for "gl*" names. */
void* dvk_get_proc_address(const char* name);
// Like dvk_get_proc_address, but nullptr for anything DivaVK doesn't implement.
void* dvk_get_implemented_proc(const char* name);

/* Ends the frame: submits work and presents the default framebuffer if a surface exists. */
void dvk_swap_buffers(void);

/* Resizes the default framebuffer (and swapchain). */
void dvk_resize(int32_t width, int32_t height);
/* Size of the default framebuffer (what dvk_resize last set); 0 x 0 before dvk_create. */
void dvk_get_framebuffer_size(int32_t* width, int32_t* height);

void dvk_set_vsync(bool vsync);

/* Writes the pipeline cache to info->pipeline_cache_path (also done periodically). */
void dvk_save_pipeline_cache(void);

// Debugging: writes every render target of the next frame to dir (PNG + index.txt).
void dvk_request_frame_dump(const char* dir);
/* On a shared context's thread, around one picture: dumps it too (as pv_*) when a frame dump
   was requested. */
void dvk_dump_begin(void);
void dvk_dump_end(void);
void dvk_get_stats(dvk_stats* out);
/* One context's counters (NULL = the first context). */
void dvk_context_get_stats(dvk_context ctx, dvk_stats* out);
/* What DivaVK is doing right now (e.g. waiting for the window, compiling a pipeline), for a
   watchdog that reports where a frame is stuck. Safe to call from any thread. */
const char* dvk_debug_stage(void);

/* Saves / restores every piece of GL state an application can see (bindings, raster state,
   pixel store, clear values, the error flag), so a plugin can draw with its own objects
   without changing what the game's renderer, or its cached copy of the GL state, expects.
   Calls nest. */
void dvk_push_gl_state(void);
void dvk_pop_gl_state(void);

/* ---------------------------------------------------------------- a second drawing thread
   A shared context has its own GL state and command buffers and shares every GL object
   (textures, buffers, programs, framebuffers, vertex arrays) with the first one. When the
   device has a second queue in the family, it submits there, at a lower priority than the
   game's queue. Two threads can draw at the same time, one per context, as long as they
   don't write the same objects. Its work is handed over by ticket: dvk_flush on its thread
   submits and returns a ticket; dvk_ticket_done (any thread, never waits) says when the GPU
   has finished it; dvk_ticket_use on the other thread, once it's done, makes that thread's
   next submit see its results (the GPU doesn't wait: the work is finished). */
/* NULL when not possible (needs timeline semaphores) */
dvk_context dvk_context_create_shared(void);
void dvk_context_destroy(dvk_context ctx);
/* The calling thread draws with ctx from now on; NULL = the first context again. */
void dvk_make_current(dvk_context ctx);
/* Submits what the current context has recorded. Returns its ticket (0 = nothing yet). */
uint64_t dvk_flush(void);
bool dvk_ticket_done(dvk_context ctx, uint64_t ticket);
void dvk_ticket_use(dvk_context ctx, uint64_t ticket);
/* The first (game thread's) context, and the ticket of a context's latest submit (NULL = the
   current context): another context can order its work after it with dvk_ticket_use. */
dvk_context dvk_main_context(void);
uint64_t dvk_last_ticket(dvk_context ctx);
/* GPU time a context's frames took since the last call (NULL = the first context). */
double dvk_context_take_gpu_ms(dvk_context ctx, uint32_t* frames);
/* Time a context's thread spent waiting for its own earlier GPU work since the last call. */
double dvk_context_take_wait_ms(dvk_context ctx);

/* Name of the Vulkan device in use (for logs/overlays). */
const char* dvk_device_name(void);

/* ---------------------------------------------------------------- OpenXR output (xr.cpp)

   Shows the default framebuffer in a headset as a virtual screen floating in front of the
   player, one quad layer per eye. With a side-by-side frame, the left half goes to the left
   eye and the right half to the right eye, so DivaGL's SBS stereo becomes real 3D.

   Call dvk_xr_setup before dvk_create: the OpenXR runtime has to choose the Vulkan device.
   Frames go to the headset from a thread of their own, paced by the runtime, so the game
   keeps its own frame rate and the headset keeps the screen steady between game frames. */

enum {
    DVK_XR_LAYOUT_MONO = 0,      /* the whole frame to both eyes */
    DVK_XR_LAYOUT_SBS_HALF = 1,  /* each half squeezed: shown stretched to content_aspect */
    DVK_XR_LAYOUT_SBS_FULL = 2,  /* each half holds the picture at content_aspect, letterboxed */
};

typedef struct dvk_xr_info {
    /* The runtime's xrGetInstanceProcAddr, or NULL to load the active runtime: runtime_json,
       else XR_RUNTIME_JSON, else the system's active runtime (registry on Windows, XDG
       config on Linux). */
    void* get_instance_proc_addr;
    const char* runtime_json;
    const char* app_name;
    int32_t layout;              /* DVK_XR_LAYOUT_* */
    bool swap_eyes;              /* the frame's left half is the right eye */
    float content_aspect;        /* width / height of the picture; 0 = take it from the frame */
    float screen_width;          /* metres */
    float screen_distance;       /* metres in front of the player */
    float screen_height;         /* metres above (or below, negative) eye level */
    dvk_log_func log;            /* for messages before dvk_create; dvk_create's log after */
    void* user;
} dvk_xr_info;

/* Loads the runtime and finds a headset. False (with the reason logged through
   dvk_create_info.log, or stderr before dvk_create) means no XR: carry on without it. */
bool dvk_xr_setup(const dvk_xr_info* info);
/* True while frames are going to the headset. */
bool dvk_xr_active(void);
/* Puts the screen straight ahead of where the player is looking now. */
void dvk_xr_recenter(void);
/* Shows a GL texture (an RGBA colour target, premultiplied alpha, rendered like the default
   framebuffer: row 0 at the bottom) as a layer of its own over the picture, a little nearer
   the player (DIVAVK_HUD_DEPTH_CM, default 10). Each presented frame copies it, so the
   game draws its HUD there instead of into the frame. 0 turns it off. */
void dvk_xr_set_hud_texture(uint32_t texture);

#ifdef __cplusplus
}
#endif

#endif
