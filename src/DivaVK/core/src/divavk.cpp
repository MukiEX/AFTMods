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

extern "C" void dvk_push_gl_state(void) {
    if (!g_ctx)
        return;
    Context& c = *g_ctx;
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
    if (!g_ctx || g_ctx->state_stack.empty())
        return;
    Context& c = *g_ctx;
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
