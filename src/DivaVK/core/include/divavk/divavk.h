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

typedef struct VkSurfaceKHR_T* dvk_surface;   /* VkSurfaceKHR */
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

/* Ends the frame: submits work and presents the default framebuffer if a surface exists. */
void dvk_swap_buffers(void);

/* Resizes the default framebuffer (and swapchain). */
void dvk_resize(int32_t width, int32_t height);

void dvk_set_vsync(bool vsync);

/* Writes the pipeline cache to info->pipeline_cache_path (also done periodically). */
void dvk_save_pipeline_cache(void);

// Debugging: writes every render target of the next frame to dir (PNG + index.txt).
void dvk_request_frame_dump(const char* dir);
void dvk_get_stats(dvk_stats* out);

/* Name of the Vulkan device in use (for logs/overlays). */
const char* dvk_device_name(void);

#ifdef __cplusplus
}
#endif

#endif
