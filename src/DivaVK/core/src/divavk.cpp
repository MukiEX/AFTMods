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
    if (!current_ctx())
        return;
    Context& c = *current_ctx();
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

extern "C" void dvk_get_framebuffer_size(int32_t* width, int32_t* height) {
    int32_t w = 0, h = 0;
    if (g_ctx && g_ctx->default_color) {
        w = g_ctx->default_color->width;
        h = g_ctx->default_color->height;
    }
    if (width) *width = w;
    if (height) *height = h;
}

extern "C" void dvk_set_vsync(bool vsync) {
    if (!g_ctx || g_ctx->vsync == vsync)
        return;
    g_ctx->vsync = vsync;
    g_ctx->swapchain_dirty = g_ctx->surface != VK_NULL_HANDLE;
}

extern "C" void dvk_push_gl_state(void) {
    if (!current_ctx())
        return;
    Context& c = *current_ctx();
    GLStateSnapshot st;
    st.active_texture = c.active_texture;
    memcpy(st.tex_2d, c.tex_2d, sizeof(st.tex_2d));
    memcpy(st.tex_cube, c.tex_cube, sizeof(st.tex_cube));
    memcpy(st.sampler_units, c.sampler_units, sizeof(st.sampler_units));
    st.array_buffer = c.array_buffer;
    st.copy_read_buffer = c.copy_read_buffer;
    st.copy_write_buffer = c.copy_write_buffer;
    st.pixel_pack_buffer = c.pixel_pack_buffer;
    st.pixel_unpack_buffer = c.pixel_unpack_buffer;
    st.generic_uniform_buffer = c.generic_uniform_buffer;
    st.generic_storage_buffer = c.generic_storage_buffer;
    for (uint32_t i = 0; i < MAX_UBO_BINDINGS; i++)
        st.ubo[i] = c.ubo[i];
    for (uint32_t i = 0; i < MAX_SSBO_BINDINGS; i++)
        st.ssbo[i] = c.ssbo[i];
    st.vertex_array = c.vertex_array;
    st.draw_fbo = c.draw_fbo;
    st.read_fbo = c.read_fbo;
    st.renderbuffer = c.renderbuffer;
    st.program = c.program;
    st.unpack_alignment = c.unpack_alignment;
    st.pack_alignment = c.pack_alignment;
    st.unpack_row_length = c.unpack_row_length;
    st.error = c.error;
    st.rs = c.rs;
    memcpy(st.clear_color_value, c.clear_color_value, sizeof(st.clear_color_value));
    st.clear_depth_value = c.clear_depth_value;
    st.clear_stencil_value = c.clear_stencil_value;
    c.state_stack.push_back(st);
}

extern "C" void dvk_pop_gl_state(void) {
    if (!current_ctx() || current_ctx()->state_stack.empty())
        return;
    Context& c = *current_ctx();
    const GLStateSnapshot st = c.state_stack.back();
    c.state_stack.pop_back();
    c.active_texture = st.active_texture;
    memcpy(c.tex_2d, st.tex_2d, sizeof(st.tex_2d));
    memcpy(c.tex_cube, st.tex_cube, sizeof(st.tex_cube));
    memcpy(c.sampler_units, st.sampler_units, sizeof(st.sampler_units));
    c.array_buffer = st.array_buffer;
    c.copy_read_buffer = st.copy_read_buffer;
    c.copy_write_buffer = st.copy_write_buffer;
    c.pixel_pack_buffer = st.pixel_pack_buffer;
    c.pixel_unpack_buffer = st.pixel_unpack_buffer;
    c.generic_uniform_buffer = st.generic_uniform_buffer;
    c.generic_storage_buffer = st.generic_storage_buffer;
    for (uint32_t i = 0; i < MAX_UBO_BINDINGS; i++)
        c.ubo[i] = st.ubo[i];
    for (uint32_t i = 0; i < MAX_SSBO_BINDINGS; i++)
        c.ssbo[i] = st.ssbo[i];
    c.vertex_array = st.vertex_array;
    c.draw_fbo = st.draw_fbo;
    c.read_fbo = st.read_fbo;
    c.renderbuffer = st.renderbuffer;
    c.program = st.program;
    c.unpack_alignment = st.unpack_alignment;
    c.pack_alignment = st.pack_alignment;
    c.unpack_row_length = st.unpack_row_length;
    c.error = st.error;
    c.rs = st.rs;
    memcpy(c.clear_color_value, st.clear_color_value, sizeof(st.clear_color_value));
    c.clear_depth_value = st.clear_depth_value;
    c.clear_stencil_value = st.clear_stencil_value;
    // The next draw rebinds its pipeline and descriptors and switches render pass if needed.
    c.bound_pipeline = VK_NULL_HANDLE;
    c.descriptors_dirty = true;
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

/* ---------------------------------------------------------------- shared contexts */

extern "C" dvk_context dvk_context_create_shared(void) {
    if (!g_ctx)
        return nullptr;
    Context* c = new Context(g_ctx->sh, g_ctx->sh->queues[1] ? 1 : 0);
    if (!c->init_shared()) {
        log("A second context (for another drawing thread) isn't possible here: no timeline semaphores");
        delete c;
        return nullptr;
    }
    log("Second context created: its own GL state and frames, %s",
        c->queue_slot ? "on the second, lower-priority queue" : "on the game's queue (only one queue)");
    return (dvk_context)c;
}

extern "C" void dvk_context_destroy(dvk_context ctx) {
    Context* c = (Context*)ctx;
    if (!c || c == g_ctx)
        return;
    if (t_ctx == c)
        t_ctx = nullptr;
    c->shutdown();
    delete c;
}

extern "C" void dvk_make_current(dvk_context ctx) {
    t_ctx = (Context*)ctx;
}

extern "C" uint64_t dvk_flush(void) {
    Context* c = current_ctx();
    if (!c)
        return 0;
    if (c->recording) {
        if (c->debug_depth) {
            while (c->debug_depth) {
                if (vkCmdEndDebugUtilsLabelEXT)
                    vkCmdEndDebugUtilsLabelEXT(c->frame().cmd);
                c->debug_depth--;
            }
        }
        c->submit_frame(false);
    }
    return c->last_submitted.load();
}

extern "C" bool dvk_ticket_done(dvk_context ctx, uint64_t ticket) {
    Context* c = (Context*)ctx;
    if (!c || !c->timeline || !ticket)
        return true;
    uint64_t v = 0;
    if (vkGetSemaphoreCounterValueKHR(c->device, c->timeline, &v) != VK_SUCCESS)
        return false;
    return v >= ticket;
}

extern "C" void dvk_ticket_use(dvk_context ctx, uint64_t ticket) {
    Context* from = (Context*)ctx;
    Context* c = current_ctx();
    if (!from || !c || from == c || !from->timeline || !ticket)
        return;
    for (auto& w : c->submit_waits)
        if (w.first == from->timeline) {
            w.second = std::max(w.second, ticket);
            return;
        }
    c->submit_waits.push_back({ from->timeline, ticket });
}

extern "C" double dvk_context_take_gpu_ms(dvk_context ctx, uint32_t* frames) {
    Context* c = ctx ? (Context*)ctx : g_ctx;
    if (!c)
        return 0.0;
    double ms = c->gpu_ms_shared.exchange(0.0);
    uint32_t n = c->gpu_frames_shared.exchange(0);
    if (frames)
        *frames = n;
    return ms;
}

extern "C" dvk_context dvk_main_context(void) {
    return (dvk_context)g_ctx;
}

extern "C" uint64_t dvk_last_ticket(dvk_context ctx) {
    Context* c = ctx ? (Context*)ctx : current_ctx();
    return c ? c->last_submitted.load() : 0;
}

extern "C" void dvk_context_get_stats(dvk_context ctx, dvk_stats* out) {
    Context* c = ctx ? (Context*)ctx : g_ctx;
    if (!c || !out)
        return;
    const Context::Stats& s = c->stats;
    out->frames = c->serial;
    out->programs_linked = s.programs_linked;
    out->draws = s.draws;
    out->render_passes = s.passes;
    out->render_pass_breaks = s.pass_breaks;
    out->pipelines_created = s.pipelines_created;
    out->hoisted_uploads = s.hoisted_uploads;
    out->inline_uploads = s.inline_uploads;
}
