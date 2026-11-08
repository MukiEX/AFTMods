/*
    DivaVK - one-frame render target dump (debugging aid).

    After dvk_request_frame_dump(dir), the next whole frame writes every render target as
    it is finished (end of each render pass, blit and copy destination, and the final
    default framebuffer) to <dir>/NNN_<what>_tex<name>_<w>x<h>_<format>.png, plus
    <dir>/index.txt with per-channel min/max/mean of the full-resolution float data.
    Images are written with values clamped to 0..1 and downscaled to at most 960 pixels
    wide; HDR values above 1 are visible in index.txt.
*/

#include "context.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include <stb/stb_image_write.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>

namespace dvk {
    static const char* vk_format_name(VkFormat f) {
        switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
        case VK_FORMAT_R32G32B32A32_SFLOAT: return "RGBA32F";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "R11G11B10F";
        case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
        case VK_FORMAT_R32G32_SFLOAT: return "RG32F";
        case VK_FORMAT_R16_SFLOAT: return "R16F";
        case VK_FORMAT_R32_SFLOAT: return "R32F";
        case VK_FORMAT_R8_UNORM: return "R8";
        case VK_FORMAT_R8G8_UNORM: return "RG8";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "RGB10A2";
        case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        case VK_FORMAT_D32_SFLOAT: return "D32F";
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32FS8";
        case VK_FORMAT_D16_UNORM: return "D16";
        default: return "fmt";
        }
    }

    void Context::request_frame_dump(const std::string& dir) {
        dump_dir = dir;
        dump_pending = true;
    }

    void Context::dump_frame_begin() {
        if (!dump_pending)
            return;
        dump_pending = false;
        dump_active = true;
        dump_index = 0;
        std::string idx = dump_dir + "/" + dump_prefix + "index.txt";
        dump_file = fopen(idx.c_str(), "w");
        if (dump_file)
            fprintf(dump_file, "# n  what  texture  size  format  min(r g b a)  max(r g b a)  mean(r g b a)\n");
        log("Frame dump started: %s", dump_dir.c_str());
    }

    void Context::dump_frame_end() {
        if (!dump_active)
            return;
        PassTarget t;
        if (is_main() && resolve_target(0, t, true) && t.color[0])
            dump_texture(t.color[0], t.color_level[0], t.color_layer[0], "final");
        dump_active = false;
        if (dump_file) {
            fclose(dump_file);
            dump_file = nullptr;
        }
        log("Frame dump finished: %d images", dump_index);
    }

    void Context::dump_pass_targets(const PassTarget& tgt, const char* what) {
        if (!dump_active)
            return;
        char label[64];
        for (uint32_t i = 0; i < tgt.num_color; i++)
            if (tgt.color[i]) {
                snprintf(label, sizeof(label), "%s_c%u_prog%u", what, i, program);
                dump_texture(tgt.color[i], tgt.color_level[i], tgt.color_layer[i], label);
            }
        if (tgt.depth && is_main()) {   // (a shared context's depth readback crashed on Quest)
            snprintf(label, sizeof(label), "%s_depth_prog%u", what, program);
            dump_texture(tgt.depth, tgt.depth_level, 0, label);
        }
    }

    void Context::dump_texture(TextureObj* t, uint32_t level, uint32_t layer, const char* what) {
        if (!dump_active || !t || !t->image || t->samples > 1 || t->format->compressed)
            return;
        bool depth = (t->format->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        int32_t w = std::max(t->width >> level, 1), h = std::max(t->height >> level, 1);
        int comps = depth ? 1 : 4;
        std::vector<float> px((size_t)w * h * comps);
        // texture_read may end the current pass; don't recurse into another dump from it.
        dump_active = false;
        texture_read(t, level, layer, 0, 0, w, h, depth ? GL_DEPTH_COMPONENT : GL_RGBA, GL_FLOAT, px.data(), 4);
        dump_active = true;

        float mn[4] = { INFINITY, INFINITY, INFINITY, INFINITY }, mx[4] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };
        double sum[4] = {};
        size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < comps; c++) {
                float v = px[i * comps + c];
                if (std::isnan(v)) continue;
                mn[c] = std::min(mn[c], v);
                mx[c] = std::max(mx[c], v);
                sum[c] += v;
            }

        int step = std::max(1, (w + 959) / 960);
        int ow = std::max(1, w / step), oh = std::max(1, h / step);
        std::vector<uint8_t> out((size_t)ow * oh * 4);
        for (int y = 0; y < oh; y++) {
            int sy = h - 1 - y * step; // GL row 0 is the bottom; PNG row 0 is the top
            for (int x = 0; x < ow; x++) {
                const float* s = &px[((size_t)sy * w + (size_t)x * step) * comps];
                uint8_t* d = &out[((size_t)y * ow + x) * 4];
                for (int c = 0; c < 4; c++) {
                    float v = depth ? (c == 3 ? 1.0f : s[0]) : (c == 3 ? 1.0f : s[c]); // alpha shown as opaque
                    v = std::isnan(v) ? 1.0f : std::min(std::max(v, 0.0f), 1.0f);
                    d[c] = (uint8_t)(v * 255.0f + 0.5f);
                }
            }
        }
        char name[512];
        snprintf(name, sizeof(name), "%s/%s%03d_%s_tex%u_%dx%d_%s.png", dump_dir.c_str(), dump_prefix.c_str(), dump_index, what,
            t->name, w, h, vk_format_name(t->format->vk));
        stbi_write_png(name, ow, oh, 4, out.data(), ow * 4);
        if (dump_file) {
            fprintf(dump_file, "%03d %s tex%u %dx%d %s", dump_index, what, t->name, w, h, vk_format_name(t->format->vk));
            fprintf(dump_file, "  min");
            for (int c = 0; c < comps; c++) fprintf(dump_file, " %.4g", mn[c]);
            fprintf(dump_file, "  max");
            for (int c = 0; c < comps; c++) fprintf(dump_file, " %.4g", mx[c]);
            fprintf(dump_file, "  mean");
            for (int c = 0; c < comps; c++) fprintf(dump_file, " %.4g", sum[c] / (double)n);
            fprintf(dump_file, "\n");
            fflush(dump_file);
        }
        dump_index++;
    }
}

extern "C" void dvk_request_frame_dump(const char* dir) {
    if (!dvk::g_ctx)
        return;
    const std::string d = dir ? dir : ".";
    dvk::g_ctx->request_frame_dump(d);
    // shared contexts (a PV render thread): their next picture too (dvk_dump_begin/end), as pv_*
    std::lock_guard<std::mutex> l(dvk::g_ctx->sh->contexts_mutex);
    for (dvk::Context* c : dvk::g_ctx->sh->contexts)
        if (c != dvk::g_ctx) {
            std::lock_guard<std::mutex> r(c->dump_req_mutex);
            c->dump_req_dir = d;
        }
}

extern "C" void dvk_dump_begin(void) {
    dvk::Context* c = dvk::current_ctx();
    if (!c || c->is_main())
        return;
    std::string d;
    {
        std::lock_guard<std::mutex> r(c->dump_req_mutex);
        d.swap(c->dump_req_dir);
    }
    if (d.empty())
        return;
    c->dump_prefix = "pv_";
    c->request_frame_dump(d);
    c->dump_frame_begin();
}

// While a frame dump is being written on this thread's context: one GL texture as it is now.
extern "C" void dvk_dump_gl_texture(unsigned int name, const char* what) {
    dvk::Context* c = dvk::current_ctx();
    if (!c || !c->dump_active || !name)
        return;
    dvk::TextureObj* t = c->textures.get(name);
    if (!t)
        return;
    c->end_pass();   // what's recorded so far into it is in the readback
    c->dump_texture(t, 0, 0, what ? what : "texture");
}

extern "C" void dvk_dump_end(void) {
    dvk::Context* c = dvk::current_ctx();
    if (c && !c->is_main())
        c->dump_frame_end();
}
