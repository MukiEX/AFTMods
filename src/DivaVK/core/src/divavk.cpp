/*
    DivaVK - public API.
*/

#include "context.hpp"

using namespace dvk;

extern "C" bool dvk_create(const dvk_create_info* info) {
    if (g_ctx || !info)
        return false;
    g_ctx = new Context();
    if (!g_ctx->init(*info)) {
        g_ctx->shutdown();
        delete g_ctx;
        g_ctx = nullptr;
        return false;
    }
    return true;
}

extern "C" void dvk_destroy(void) {
    if (!g_ctx)
        return;
    g_ctx->shutdown();
    delete g_ctx;
    g_ctx = nullptr;
}

extern "C" void dvk_swap_buffers(void) {
    if (!g_ctx)
        return;
    Context& c = *g_ctx;
    if (c.recording && c.debug_depth) {
        // Labels can't stay open across command buffers
        while (c.debug_depth) {
            if (vkCmdEndDebugUtilsLabelEXT)
                vkCmdEndDebugUtilsLabelEXT(c.frame().cmd);
            c.debug_depth--;
        }
    }
    c.present();
}

extern "C" void dvk_resize(int32_t width, int32_t height) {
    if (!g_ctx || width <= 0 || height <= 0)
        return;
    Context& c = *g_ctx;
    if (c.default_color->width == width && c.default_color->height == height)
        return;
    if (c.recording)
        c.submit_frame(false);
    c.texture_alloc(c.default_color.get(), GL_RENDERBUFFER, GL_RGBA8, width, height, 1, 1);
    c.texture_alloc(c.default_depth.get(), GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height, 1, 1);
    c.swapchain_dirty = c.surface != VK_NULL_HANDLE;
}

extern "C" void dvk_set_vsync(bool vsync) {
    if (!g_ctx || g_ctx->vsync == vsync)
        return;
    g_ctx->vsync = vsync;
    g_ctx->swapchain_dirty = g_ctx->surface != VK_NULL_HANDLE;
}

extern "C" void dvk_get_stats(dvk_stats* out) {
    if (!g_ctx || !out)
        return;
    const Context::Stats& s = g_ctx->stats;
    out->frames = g_ctx->serial;
    out->programs_linked = s.programs_linked;
    out->draws = s.draws;
    out->render_passes = s.passes;
    out->render_pass_breaks = s.pass_breaks;
    out->pipelines_created = s.pipelines_created;
    out->hoisted_uploads = s.hoisted_uploads;
    out->inline_uploads = s.inline_uploads;
}

extern "C" const char* dvk_debug_stage(void) {
    return dvk::g_stage.load(std::memory_order_relaxed);
}

extern "C" const char* dvk_device_name(void) {
    return g_ctx ? g_ctx->props.deviceName : "";
}

extern "C" void dvk_save_pipeline_cache(void) {
    if (g_ctx)
        g_ctx->save_pipeline_cache();
}
