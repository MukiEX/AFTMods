/*
    DivaVK - framebuffers, implicit render passes, clears, blits and copies.

    GL has no render passes, so they are inferred: a pass begins at the first draw or
    clear that targets a framebuffer and ends when anything else needs the GPU outside a
    pass (binding a different framebuffer and drawing, a blit, a copy, an inline upload,
    a readback, end of frame). A glClear issued before a pass has begun becomes the pass's
    load op, which is what tile-based mobile GPUs want; a clear issued mid-pass uses
    vkCmdClearAttachments.
*/

#include "context.hpp"
#include <map>
#include <tuple>

namespace dvk {
    struct FbEntry {
        VkFramebuffer fb;
        std::vector<VkImageView> views;
    };
    static std::unordered_map<uint64_t, FbEntry> s_fb_entries;

    bool PendingClear::any() const {
        for (bool c : color)
            if (c)
                return true;
        return depth || stencil;
    }

    FramebufferObj* Context::fbo(GLuint name) {
        return name ? framebuffers.get(name) : &default_fbo;
    }

    static TextureObj* attachment_texture(Context& ctx, const Attachment& a) {
        if (!a.texture)
            return nullptr;
        TextureObj* t = ctx.textures.get(a.texture);
        if (!t || !t->image || !t->renderable)
            return nullptr;
        return t;
    }

    bool Context::resolve_target(GLuint fbo_name, PassTarget& out, bool for_read) {
        out = PassTarget();
        if (!fbo_name) {
            out.color[0] = default_color.get();
            out.num_color = 1;
            out.depth = default_depth.get();
            out.width = default_color->width;
            out.height = default_color->height;
            out.samples = 1;
            return true;
        }

        FramebufferObj* f = framebuffers.get(fbo_name);
        if (!f)
            return false;

        int32_t w = INT32_MAX, h = INT32_MAX;
        uint32_t samples = 0;
        auto add_size = [&](TextureObj* t, uint32_t level) {
            w = std::min(w, std::max(t->width >> level, 1));
            h = std::min(h, std::max(t->height >> level, 1));
            samples = t->samples;
        };

        if (for_read) {
            if (f->read_buffer >= GL_COLOR_ATTACHMENT0 && f->read_buffer < GL_COLOR_ATTACHMENT0 + MAX_COLOR_ATTACHMENTS) {
                const Attachment& a = f->color[f->read_buffer - GL_COLOR_ATTACHMENT0];
                if (TextureObj* t = attachment_texture(*this, a)) {
                    out.color[0] = t;
                    out.color_level[0] = (uint32_t)a.level;
                    out.color_layer[0] = (uint32_t)a.layer;
                    out.num_color = 1;
                    add_size(t, (uint32_t)a.level);
                }
            }
        }
        else {
            for (uint32_t i = 0; i < f->num_draw_buffers && i < MAX_COLOR_ATTACHMENTS; i++) {
                GLenum db = f->draw_buffers[i];
                if (db < GL_COLOR_ATTACHMENT0 || db >= GL_COLOR_ATTACHMENT0 + MAX_COLOR_ATTACHMENTS)
                    continue;
                const Attachment& a = f->color[db - GL_COLOR_ATTACHMENT0];
                if (TextureObj* t = attachment_texture(*this, a)) {
                    out.color[i] = t;
                    out.color_level[i] = (uint32_t)a.level;
                    out.color_layer[i] = (uint32_t)a.layer;
                    out.num_color = i + 1;
                    add_size(t, (uint32_t)a.level);
                }
            }
        }

        const Attachment& da = f->depth.texture ? f->depth : f->stencil;
        if (TextureObj* t = attachment_texture(*this, da)) {
            out.depth = t;
            out.depth_level = (uint32_t)da.level;
            add_size(t, (uint32_t)da.level);
        }

        if (!out.num_color && !out.depth)
            return false;
        out.width = w;
        out.height = h;
        out.samples = samples ? samples : 1;
        return true;
    }

    // ------------------------------------------------------------ render passes

    static uint8_t layout_code(VkImageLayout l) {
        switch (l) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return 1;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return 2;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return 3;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return 4;
        default: return 0;
        }
    }

    static VkImageLayout layout_from_code(uint8_t c) {
        switch (c) {
        case 1: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case 2: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case 3: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case 4: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_GENERAL;
        }
    }

    VkRenderPass Context::get_render_pass(const PassKey& key) {
        uint64_t h = hash_bytes(&key, sizeof(key));
        {
            auto lit = local_render_passes.find(h);
            if (lit != local_render_passes.end())
                return lit->second;
            std::lock_guard<std::mutex> l(sh->cache_mutex);
            auto it = render_passes.find(h);
            if (it != render_passes.end()) {
                local_render_passes[h] = it->second;
                return it->second;
            }
        }

        VkAttachmentDescription atts[MAX_COLOR_ATTACHMENTS + 1];
        VkAttachmentReference color_refs[MAX_COLOR_ATTACHMENTS];
        VkAttachmentReference depth_ref = {};
        uint32_t n = 0;
        for (uint32_t i = 0; i < key.num_color; i++) {
            const PassKeyAttachment& a = key.color[i];
            if (a.format == VK_FORMAT_UNDEFINED) {
                color_refs[i] = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
                continue;
            }
            VkAttachmentDescription& d = atts[n];
            d = {};
            d.format = a.format;
            d.samples = (VkSampleCountFlagBits)a.samples;
            d.loadOp = (VkAttachmentLoadOp)a.load_op;
            d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            d.initialLayout = layout_from_code(a.layout);
            d.finalLayout = d.initialLayout;
            color_refs[i] = { n, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            n++;
        }
        if (key.has_depth) {
            VkAttachmentDescription& d = atts[n];
            d = {};
            d.format = key.depth.format;
            d.samples = (VkSampleCountFlagBits)key.depth.samples;
            d.loadOp = (VkAttachmentLoadOp)(key.depth.load_op & 0xF);
            d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            d.stencilLoadOp = (VkAttachmentLoadOp)(key.depth.load_op >> 4);
            d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            d.initialLayout = layout_from_code(key.depth.layout);
            d.finalLayout = d.initialLayout;
            depth_ref = { n, key.depth.read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            n++;
        }

        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = key.num_color;
        sub.pColorAttachments = key.num_color ? color_refs : nullptr;
        sub.pDepthStencilAttachment = key.has_depth ? &depth_ref : nullptr;

        // Broad dependencies: GL orders everything, so do we.
        VkSubpassDependency deps[2] = {};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = n;
        rpci.pAttachments = atts;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies = deps;
        VkRenderPass rp = VK_NULL_HANDLE;
        DVK_CHECK(vkCreateRenderPass(device, &rpci, nullptr, &rp));
        std::lock_guard<std::mutex> l(sh->cache_mutex);
        auto it = render_passes.find(h);
        if (it != render_passes.end()) {
            vkDestroyRenderPass(device, rp, nullptr);
            return it->second;
        }
        render_passes[h] = rp;
        return rp;
    }

    static bool begin_pass_for(Context& ctx, GLuint fbo_name, const PendingClear* clears, bool depth_ro = false) {
        PassTarget& tgt = ctx.pass_target;
        if (!ctx.resolve_target(fbo_name, tgt, false))
            return false;

        PassKey key;
        memset(&key, 0, sizeof(key));
        key.num_color = tgt.num_color;
        std::vector<VkImageView> views;
        VkClearValue clear_values[MAX_COLOR_ATTACHMENTS + 1] = {};
        uint32_t n = 0;
        for (uint32_t i = 0; i < tgt.num_color; i++) {
            TextureObj* t = tgt.color[i];
            if (!t)
                continue;
            PassKeyAttachment& a = key.color[i];
            a.format = t->format->vk;
            a.samples = (uint8_t)t->samples;
            a.layout = layout_code(t->rest_layout);
            bool clear = clears && clears->color[i];
            a.load_op = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            if (clear)
                clear_values[n].color = clears->color_value[i];
            views.push_back(ctx.texture_attach_view(t, tgt.color_level[i], tgt.color_layer[i]));
            t->last_used_serial = ctx.serial;
            n++;
        }
        if (tgt.depth) {
            TextureObj* t = tgt.depth;
            key.has_depth = 1;
            key.depth.format = t->format->vk;
            key.depth.samples = (uint8_t)t->samples;
            key.depth.layout = layout_code(t->rest_layout);
            // A read-only depth attachment can't be cleared by its load op.
            if (depth_ro && clears && (clears->depth || clears->stencil))
                depth_ro = false;
            key.depth.read_only = depth_ro ? 1 : 0;
            uint8_t dl = clears && clears->depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            uint8_t sl = !t->format->stencil ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                : (clears && clears->stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
            key.depth.load_op = (uint8_t)(dl | (sl << 4));
            clear_values[n].depthStencil = { clears ? clears->depth_value : 1.0f, clears ? clears->stencil_value : 0 };
            views.push_back(ctx.texture_attach_view(t, tgt.depth_level, 0));
            t->last_used_serial = ctx.serial;
            n++;
        }

        VkRenderPass rp = ctx.get_render_pass(key);

        uint64_t fh = hash_bytes(views.data(), views.size() * sizeof(VkImageView), (uint64_t)(uintptr_t)rp);
        hash_combine(fh, ((uint64_t)tgt.width << 32) | (uint32_t)tgt.height);
        VkFramebuffer fb;
        std::unique_lock<std::mutex> fb_lock(ctx.sh->cache_mutex);
        auto it = s_fb_entries.find(fh);
        if (it != s_fb_entries.end())
            fb = it->second.fb;
        else {
            VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fci.renderPass = rp;
            fci.attachmentCount = (uint32_t)views.size();
            fci.pAttachments = views.data();
            fci.width = (uint32_t)tgt.width;
            fci.height = (uint32_t)tgt.height;
            fci.layers = 1;
            DVK_CHECK(vkCreateFramebuffer(ctx.device, &fci, nullptr, &fb));
            s_fb_entries[fh] = { fb, views };
            ctx.vk_framebuffers[fh] = fb;
        }
        fb_lock.unlock();

        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rbi.renderPass = rp;
        rbi.framebuffer = fb;
        rbi.renderArea = { { 0, 0 }, { (uint32_t)tgt.width, (uint32_t)tgt.height } };
        rbi.clearValueCount = n;
        rbi.pClearValues = clear_values;
        vkCmdBeginRenderPass(ctx.cmd(), &rbi, VK_SUBPASS_CONTENTS_INLINE);

        ctx.pass_active = true;
        ctx.pass_depth_ro = key.depth.read_only != 0;
        ctx.pass_fbo = fbo_name;
        FramebufferObj* f = ctx.fbo(fbo_name);
        ctx.pass_fbo_generation = f ? f->generation : 0;
        ctx.pass_key = key;
        ctx.bound_pipeline = VK_NULL_HANDLE;
        ctx.stats.passes++;
        return true;
    }

    void Context::end_pass() {
        if (!pass_active)
            return;
        query_on_pass_end();
        vkCmdEndRenderPass(frame().cmd);
        pass_active = false;
        pass_depth_ro = false;
        pass_fbo = ~0u;
        bound_pipeline = VK_NULL_HANDLE;
        if (dump_active) {
            PassTarget done = pass_target;
            dump_pass_targets(done, "pass");
        }
    }

    void Context::flush_pending_clear() {
        if (pending_clear_fbo == ~0u)
            return;
        GLuint target = pending_clear_fbo;
        PendingClear pc = pending_clear;
        pending_clear = PendingClear();
        pending_clear_fbo = ~0u;
        FramebufferObj* f = fbo(target);
        if (!f || f->generation != pending_clear_generation)
            return; // attachments changed since the clear was recorded
        end_pass();
        if (begin_pass_for(*this, target, &pc))
            end_pass();
    }

    bool Context::ensure_pass(int depth_mode) {
        FramebufferObj* f = fbo(draw_fbo);
        if (!f)
            return false;
        bool same = pass_active && pass_fbo == draw_fbo && pass_fbo_generation == f->generation;
        if (same && (depth_mode < 0 || (depth_mode == 1) == pass_depth_ro))
            return true;
        bool depth_ro = depth_mode < 0 ? false : depth_mode == 1;

        if (pass_active)
            stats.pass_breaks++;
        end_pass();

        // Clears pending for another framebuffer must happen before this pass.
        if (pending_clear_fbo != ~0u && pending_clear_fbo != draw_fbo)
            flush_pending_clear();

        bool own_clear = pending_clear_fbo == draw_fbo && pending_clear_generation == f->generation;
        PendingClear pc = own_clear ? pending_clear : PendingClear();
        if (pending_clear_fbo == draw_fbo) {
            pending_clear = PendingClear();
            pending_clear_fbo = ~0u;
        }
        return begin_pass_for(*this, draw_fbo, own_clear ? &pc : nullptr, depth_ro);
    }

    static VkClearColorValue make_clear_color(const float* c) {
        VkClearColorValue v;
        for (int i = 0; i < 4; i++)
            v.float32[i] = c[i];
        return v;
    }

    // Records a clear of the given attachments of the current draw framebuffer.
    static void do_clear(Context& ctx, const PendingClear& req) {
        FramebufferObj* f = ctx.fbo(ctx.draw_fbo);
        if (!f)
            return;

        const RasterState& rs = ctx.rs;
        bool full_mask = rs.color_mask[0] && rs.color_mask[1] && rs.color_mask[2] && rs.color_mask[3];
        bool any_color = false;
        for (bool c : req.color)
            any_color |= c;
        if (any_color && !full_mask)
            log_once("clear_mask", "glClear with a partial color mask clears all channels (not emulated yet)");

        PassTarget tgt;
        if (!ctx.resolve_target(ctx.draw_fbo, tgt, false))
            return;
        bool full_area = !rs.scissor_test || (rs.scissor[0] <= 0 && rs.scissor[1] <= 0
            && rs.scissor[0] + rs.scissor[2] >= tgt.width && rs.scissor[1] + rs.scissor[3] >= tgt.height);

        // Before the pass begins: fold into load ops
        if (!(ctx.pass_active && ctx.pass_fbo == ctx.draw_fbo && ctx.pass_fbo_generation == f->generation) && full_area) {
            // A pass on another framebuffer must not keep running past this clear: later
            // draws there could sample this framebuffer and must see the cleared contents.
            if (ctx.pass_active) {
                ctx.stats.pass_breaks++;
                ctx.end_pass();
            }
            if (ctx.pending_clear_fbo != ctx.draw_fbo || ctx.pending_clear_generation != f->generation) {
                ctx.end_pass();
                ctx.flush_pending_clear();
                ctx.pending_clear = PendingClear();
                ctx.pending_clear_fbo = ctx.draw_fbo;
                ctx.pending_clear_generation = f->generation;
            }
            PendingClear& pc = ctx.pending_clear;
            for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENTS; i++)
                if (req.color[i]) {
                    pc.color[i] = true;
                    pc.color_value[i] = req.color_value[i];
                }
            if (req.depth) {
                pc.depth = true;
                pc.depth_value = req.depth_value;
            }
            if (req.stencil) {
                pc.stencil = true;
                pc.stencil_value = req.stencil_value;
            }
            return;
        }

        if (!ctx.ensure_pass((req.depth || req.stencil) ? 0 : -1))
            return;
        VkClearAttachment atts[MAX_COLOR_ATTACHMENTS + 1];
        uint32_t n = 0;
        for (uint32_t i = 0; i < ctx.pass_target.num_color; i++)
            if (req.color[i] && ctx.pass_target.color[i]) {
                atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                atts[n].colorAttachment = i;
                atts[n].clearValue.color = req.color_value[i];
                n++;
            }
        if (ctx.pass_target.depth && (req.depth || req.stencil)) {
            VkImageAspectFlags aspect = 0;
            if (req.depth)
                aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (req.stencil && ctx.pass_target.depth->format->stencil)
                aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (aspect) {
                atts[n].aspectMask = aspect;
                atts[n].colorAttachment = 0;
                atts[n].clearValue.depthStencil = { req.depth_value, req.stencil_value };
                n++;
            }
        }
        if (!n)
            return;

        VkClearRect rect = {};
        rect.layerCount = 1;
        int32_t x0 = 0, y0 = 0, x1 = tgt.width, y1 = tgt.height;
        if (rs.scissor_test) {
            x0 = std::max(rs.scissor[0], 0);
            y0 = std::max(rs.scissor[1], 0);
            x1 = std::min(rs.scissor[0] + rs.scissor[2], tgt.width);
            y1 = std::min(rs.scissor[1] + rs.scissor[3], tgt.height);
        }
        if (x1 <= x0 || y1 <= y0)
            return;
        rect.rect = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
        vkCmdClearAttachments(ctx.cmd(), n, atts, 1, &rect);
    }

    void Context::clear(GLbitfield mask) {
        PendingClear req;
        if (mask & GL_COLOR_BUFFER_BIT) {
            FramebufferObj* f = fbo(draw_fbo);
            uint32_t n = draw_fbo ? (f ? f->num_draw_buffers : 0) : 1;
            for (uint32_t i = 0; i < n && i < MAX_COLOR_ATTACHMENTS; i++) {
                req.color[i] = true;
                req.color_value[i] = make_clear_color(clear_color_value);
            }
        }
        if ((mask & GL_DEPTH_BUFFER_BIT) && rs.depth_write) {
            req.depth = true;
            req.depth_value = clear_depth_value;
        }
        if (mask & GL_STENCIL_BUFFER_BIT) {
            req.stencil = true;
            req.stencil_value = (uint32_t)clear_stencil_value;
        }
        if (req.any())
            do_clear(*this, req);
    }

    void Context::clear_buffer(GLenum buffer, GLint drawbuffer, const float* fv, const GLint* iv) {
        PendingClear req;
        switch (buffer) {
        case GL_COLOR:
            if (drawbuffer < 0 || drawbuffer >= (GLint)MAX_COLOR_ATTACHMENTS)
                return;
            req.color[drawbuffer] = true;
            for (int i = 0; i < 4; i++)
                req.color_value[drawbuffer].float32[i] = fv ? fv[i] : (float)iv[i];
            if (iv && !fv)
                for (int i = 0; i < 4; i++)
                    req.color_value[drawbuffer].int32[i] = iv[i];
            break;
        case GL_DEPTH:
            req.depth = true;
            req.depth_value = fv ? fv[0] : (float)iv[0];
            break;
        case GL_STENCIL:
            req.stencil = true;
            req.stencil_value = iv ? (uint32_t)iv[0] : (uint32_t)fv[0];
            break;
        case GL_DEPTH_STENCIL:
            req.depth = req.stencil = true;
            req.depth_value = fv ? fv[0] : 1.0f;
            req.stencil_value = iv ? (uint32_t)iv[0] : 0;
            break;
        default:
            return;
        }
        do_clear(*this, req);
    }

    // ------------------------------------------------------------ transfers

    void Context::on_texture_destroyed(TextureObj* t) {
        if (pass_active) {
            bool in_pass = pass_target.depth == t;
            for (TextureObj* c : pass_target.color)
                in_pass |= c == t;
            if (in_pass)
                end_pass();
        }
        if (t->attach_views.empty())
            return;
        std::lock_guard<std::mutex> fb_lock(sh->cache_mutex);
        for (auto it = s_fb_entries.begin(); it != s_fb_entries.end();) {
            bool uses = false;
            for (VkImageView v : it->second.views)
                for (auto& av : t->attach_views)
                    uses |= av.second == v;
            if (uses) {
                VkDevice dev = device;
                VkFramebuffer fb = it->second.fb;
                defer_destroy([dev, fb]() { vkDestroyFramebuffer(dev, fb, nullptr); });
                vk_framebuffers.erase(it->first);
                it = s_fb_entries.erase(it);
            }
            else
                ++it;
        }
    }

    static void sync_point(Context& ctx) {
        ctx.end_pass();
        ctx.flush_pending_clear();
        ctx.end_pass();
    }

    // Transition helper for two images that may be the same image.
    // Every transfer is fenced off from everything before and after it with a full memory
    // barrier, even when no layout changes (a texture whose resting layout is already the
    // transfer's skips its image barrier): a copy into the default framebuffer followed by
    // the present blit, or a blit followed by another, ran into each other on Adreno 830.
    static void transfer_fence(VkCommandBuffer cb) {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    static void begin_transfer(Context& ctx, VkCommandBuffer cb, TextureObj* src, TextureObj* dst,
        VkImageLayout& src_layout, VkImageLayout& dst_layout) {
        transfer_fence(cb);
        if (src == dst) {
            src_layout = dst_layout = VK_IMAGE_LAYOUT_GENERAL;
            ctx.texture_transition(cb, src, src->rest_layout, VK_IMAGE_LAYOUT_GENERAL);
            return;
        }
        src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ctx.texture_transition(cb, src, src->rest_layout, src_layout);
        ctx.texture_transition(cb, dst, dst->rest_layout, dst_layout);
    }

    static void end_transfer(Context& ctx, VkCommandBuffer cb, TextureObj* src, TextureObj* dst,
        VkImageLayout src_layout, VkImageLayout dst_layout) {
        if (src == dst)
            ctx.texture_transition(cb, src, VK_IMAGE_LAYOUT_GENERAL, src->rest_layout);
        else {
            ctx.texture_transition(cb, src, src_layout, src->rest_layout);
            ctx.texture_transition(cb, dst, dst_layout, dst->rest_layout);
        }
        transfer_fence(cb);
    }

    // A whole-image multisample resolve done by a render pass (its resolve attachment) rather
    // than vkCmdResolveImage: Adreno 830 drew half of transfers that read the last one's output,
    // and the MSAA resolve into the PV's screen buffer was the last full-screen one left (the
    // right eye in SBS showed the old picture in one triangle). DIVAVK_RESOLVE_PASS=0: off.
    static bool resolve_by_pass(Context& ctx, VkCommandBuffer cb, TextureObj* s, TextureObj* d,
        uint32_t d_level, uint32_t d_layer) {
        static int on = -1;
        if (on < 0) {
            const char* e = env("DIVAVK_RESOLVE_PASS");
            on = !(e && e[0] == '0');
            log(on ? "Multisample resolves: by a render pass (DIVAVK_RESOLVE_PASS=0 for transfers)"
                   : "Multisample resolves: transfers (DIVAVK_RESOLVE_PASS=0)");
        }
        if (!on || !s->image || !d->image || !d->renderable || s->format->vk != d->format->vk
            || s->format->depth || s->format->stencil)
            return false;
        const VkFormat fmt = s->format->vk;
        const VkImageLayout sl = s->rest_layout, dl = d->rest_layout;
        if (sl == VK_IMAGE_LAYOUT_UNDEFINED || dl == VK_IMAGE_LAYOUT_UNDEFINED)
            return false;

        static std::map<std::tuple<VkFormat, uint32_t, VkImageLayout, VkImageLayout>, VkRenderPass> passes;
        VkRenderPass rp = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> l(ctx.sh->cache_mutex);
            auto key = std::make_tuple(fmt, s->samples, sl, dl);
            auto it = passes.find(key);
            if (it != passes.end())
                rp = it->second;
            else {
                VkAttachmentDescription atts[2] = {};
                atts[0].format = fmt;
                atts[0].samples = (VkSampleCountFlagBits)s->samples;
                atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                atts[0].initialLayout = atts[0].finalLayout = sl;
                atts[1] = atts[0];
                atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
                atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // all of it is written
                atts[1].initialLayout = atts[1].finalLayout = dl;
                VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
                VkAttachmentReference rref = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
                VkSubpassDescription sub = {};
                sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                sub.colorAttachmentCount = 1;
                sub.pColorAttachments = &cref;
                sub.pResolveAttachments = &rref;
                VkSubpassDependency deps[2] = {};
                deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
                deps[0].dstSubpass = 0;
                deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                deps[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
                deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                deps[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                deps[1].srcSubpass = 0;
                deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
                deps[1].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
                deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                deps[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
                rpci.attachmentCount = 2;
                rpci.pAttachments = atts;
                rpci.subpassCount = 1;
                rpci.pSubpasses = &sub;
                rpci.dependencyCount = 2;
                rpci.pDependencies = deps;
                if (vkCreateRenderPass(ctx.device, &rpci, nullptr, &rp) != VK_SUCCESS)
                    return false;
                passes[key] = rp;
            }
        }

        VkImageView views[2] = { ctx.texture_attach_view(s, 0, 0), ctx.texture_attach_view(d, d_level, d_layer) };
        if (!views[0] || !views[1])
            return false;
        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = rp;
        fci.attachmentCount = 2;
        fci.pAttachments = views;
        fci.width = (uint32_t)s->width;
        fci.height = (uint32_t)s->height;
        fci.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &fb) != VK_SUCCESS)
            return false;
        VkDevice dev = ctx.device;
        ctx.defer_destroy([dev, fb]() { vkDestroyFramebuffer(dev, fb, nullptr); });

        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rbi.renderPass = rp;
        rbi.framebuffer = fb;
        rbi.renderArea = { { 0, 0 }, { (uint32_t)s->width, (uint32_t)s->height } };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cb);
        s->last_used_serial = d->last_used_serial = ctx.serial;
        ctx.stats.passes++;
        log_once("resolve_pass", "Multisample resolve by a render pass (%dx%d, %u samples first)", s->width, s->height, s->samples);
        return true;
    }

    void Context::blit_framebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
        GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
        sync_point(*this);
        PassTarget src, dst;
        if (!resolve_target(read_fbo, src, true) && !(mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)))
            return;
        if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
            PassTarget src_d;
            resolve_target(read_fbo, src_d, false);
            src.depth = src_d.depth;
            src.depth_level = src_d.depth_level;
        }
        if (!resolve_target(draw_fbo, dst, false))
            return;

        VkCommandBuffer cb = cmd();
        auto clamp_x = [](GLint v, int32_t m) { return std::min(std::max(v, 0), m); };

        auto blit_one = [&](TextureObj* s, uint32_t s_level, uint32_t s_layer,
            TextureObj* d, uint32_t d_level, uint32_t d_layer, VkImageAspectFlags aspect, VkFilter f) {
            if (!s || !d)
                return;
            int32_t sw = std::max(s->width >> s_level, 1), sh = std::max(s->height >> s_level, 1);
            int32_t dw = std::max(d->width >> d_level, 1), dh = std::max(d->height >> d_level, 1);
            if (s->samples > 1 && d->samples == 1 && aspect == VK_IMAGE_ASPECT_COLOR_BIT
                && s_level == 0 && s_layer == 0 && sw == dw && sh == dh
                && sx0 == 0 && sy0 == 0 && sx1 == sw && sy1 == sh
                && dx0 == 0 && dy0 == 0 && dx1 == dw && dy1 == dh
                && resolve_by_pass(*this, cb, s, d, d_level, d_layer))
                return;
            VkImageLayout sl, dl;
            begin_transfer(*this, cb, s, d, sl, dl);

            if (s->samples > 1 && d->samples == 1) {
                // Resolve: GL requires identical rectangles here.
                if (sx1 - sx0 != dx1 - dx0 || sy1 - sy0 != dy1 - dy0)
                    log_once("blit_resolve_scale", "Scaled multisample resolve blit; resolving without scaling");
                VkImageResolve r = {};
                r.srcSubresource = { aspect, s_level, s_layer, 1 };
                r.srcOffset = { clamp_x(std::min(sx0, sx1), sw), clamp_x(std::min(sy0, sy1), sh), 0 };
                r.dstSubresource = { aspect, d_level, d_layer, 1 };
                r.dstOffset = { clamp_x(std::min(dx0, dx1), dw), clamp_x(std::min(dy0, dy1), dh), 0 };
                r.extent = { (uint32_t)std::min(std::abs(sx1 - sx0), std::min(sw - r.srcOffset.x, dw - r.dstOffset.x)),
                    (uint32_t)std::min(std::abs(sy1 - sy0), std::min(sh - r.srcOffset.y, dh - r.dstOffset.y)), 1 };
                vkCmdResolveImage(cb, s->image, sl, d->image, dl, 1, &r);
            }
            else if (s->format == d->format && s->samples == d->samples
                && sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0
                && sx0 >= 0 && sy0 >= 0 && dx0 >= 0 && dy0 >= 0
                && sx1 <= sw && sy1 <= sh && dx1 <= dw && dy1 <= dh) {
                // Same size, same format, no flip: a copy. (Adreno 830's driver drew only one of
                // the two triangles of such a blit: half the PV picture under the HUD showed the
                // screen's clear colour, split along the diagonal.)
                VkImageCopy r = {};
                r.srcSubresource = { aspect, s_level, s_layer, 1 };
                r.srcOffset = { sx0, sy0, 0 };
                r.dstSubresource = { aspect, d_level, d_layer, 1 };
                r.dstOffset = { dx0, dy0, 0 };
                r.extent = { (uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0), 1 };
                vkCmdCopyImage(cb, s->image, sl, d->image, dl, 1, &r);
                log_once("blit_as_copy", "Same-size blits are done as image copies (%dx%d first)", sx1 - sx0, sy1 - sy0);
            }
            else {
                VkImageBlit b = {};
                b.srcSubresource = { aspect, s_level, s_layer, 1 };
                b.srcOffsets[0] = { clamp_x(sx0, sw), clamp_x(sy0, sh), 0 };
                b.srcOffsets[1] = { clamp_x(sx1, sw), clamp_x(sy1, sh), 1 };
                b.dstSubresource = { aspect, d_level, d_layer, 1 };
                b.dstOffsets[0] = { clamp_x(dx0, dw), clamp_x(dy0, dh), 0 };
                b.dstOffsets[1] = { clamp_x(dx1, dw), clamp_x(dy1, dh), 1 };
                if (b.srcOffsets[0].x != b.srcOffsets[1].x && b.srcOffsets[0].y != b.srcOffsets[1].y
                    && b.dstOffsets[0].x != b.dstOffsets[1].x && b.dstOffsets[0].y != b.dstOffsets[1].y)
                    vkCmdBlitImage(cb, s->image, sl, d->image, dl, 1, &b, f);
            }
            end_transfer(*this, cb, s, d, sl, dl);
            s->last_used_serial = d->last_used_serial = serial;
        };

        if (mask & GL_COLOR_BUFFER_BIT) {
            TextureObj* s = src.color[0];
            if (s) {
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(phys, s->format->vk, &fp);
                VkFilter f = filter == GL_LINEAR && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                    ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                for (uint32_t i = 0; i < dst.num_color; i++)
                    blit_one(s, src.color_level[0], src.color_layer[0],
                        dst.color[i], dst.color_level[i], dst.color_layer[i], VK_IMAGE_ASPECT_COLOR_BIT, f);
            }
        }
        if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && src.depth && dst.depth) {
            VkImageAspectFlags aspect = 0;
            if (mask & GL_DEPTH_BUFFER_BIT)
                aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if ((mask & GL_STENCIL_BUFFER_BIT) && src.depth->format->stencil && dst.depth->format->stencil)
                aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (src.depth->format->vk != dst.depth->format->vk)
                log_once("blit_depth_fmt", "Depth blit between different formats is not supported");
            else
                blit_one(src.depth, src.depth_level, 0, dst.depth, dst.depth_level, 0, aspect, VK_FILTER_NEAREST);
        }
        if (dump_active && draw_fbo != 0) // the default framebuffer is dumped once, at present
            dump_pass_targets(dst, "blit");
    }

    void Context::copy_tex_sub_image(TextureObj* t, GLenum target, GLint level, GLint xoff, GLint yoff,
        GLint x, GLint y, GLsizei w, GLsizei h) {
        if (!t || !t->image || w <= 0 || h <= 0)
            return;
        sync_point(*this);
        PassTarget src;
        bool depth = t->format->depth;
        if (depth) {
            if (!resolve_target(read_fbo, src, false) || !src.depth)
                return;
        }
        else if (!resolve_target(read_fbo, src, true) || !src.color[0])
            return;

        TextureObj* s = depth ? src.depth : src.color[0];
        uint32_t s_level = depth ? src.depth_level : src.color_level[0];
        uint32_t s_layer = depth ? 0 : src.color_layer[0];
        uint32_t layer = (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
            ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;

        VkCommandBuffer cb = cmd();
        VkImageLayout sl, dl;
        begin_transfer(*this, cb, s, t, sl, dl);
        VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (s->format->vk == t->format->vk) {
            VkImageCopy c = {};
            c.srcSubresource = { aspect, s_level, s_layer, 1 };
            c.srcOffset = { x, y, 0 };
            c.dstSubresource = { aspect, (uint32_t)level, layer, 1 };
            c.dstOffset = { xoff, yoff, 0 };
            c.extent = { (uint32_t)w, (uint32_t)h, 1 };
            vkCmdCopyImage(cb, s->image, sl, t->image, dl, 1, &c);
        }
        else {
            VkImageBlit b = {};
            b.srcSubresource = { aspect, s_level, s_layer, 1 };
            b.srcOffsets[0] = { x, y, 0 };
            b.srcOffsets[1] = { x + w, y + h, 1 };
            b.dstSubresource = { aspect, (uint32_t)level, layer, 1 };
            b.dstOffsets[0] = { xoff, yoff, 0 };
            b.dstOffsets[1] = { xoff + w, yoff + h, 1 };
            vkCmdBlitImage(cb, s->image, sl, t->image, dl, 1, &b, VK_FILTER_NEAREST);
        }
        end_transfer(*this, cb, s, t, sl, dl);
        s->last_used_serial = t->last_used_serial = serial;
        t->defined_levels |= 1u << level;
        if (dump_active)
            dump_texture(t, (uint32_t)level, layer, "copytex");
    }

    void Context::copy_image_sub_data(TextureObj* src, GLint src_level, GLint sx, GLint sy, GLint sz,
        TextureObj* dst, GLint dst_level, GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei d) {
        if (!src || !dst || !src->image || !dst->image || w <= 0 || h <= 0)
            return;
        sync_point(*this);
        VkCommandBuffer cb = cmd();
        VkImageLayout sl, dl;
        begin_transfer(*this, cb, src, dst, sl, dl);
        VkImageCopy c = {};
        c.srcSubresource = { src->format->aspect, (uint32_t)src_level, (uint32_t)sz, (uint32_t)std::max(d, 1) };
        c.srcOffset = { sx, sy, 0 };
        c.dstSubresource = { dst->format->aspect, (uint32_t)dst_level, (uint32_t)dz, (uint32_t)std::max(d, 1) };
        c.dstOffset = { dx, dy, 0 };
        c.extent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImage(cb, src->image, sl, dst->image, dl, 1, &c);
        end_transfer(*this, cb, src, dst, sl, dl);
        src->last_used_serial = dst->last_used_serial = serial;
        dst->defined_levels |= 1u << dst_level;
    }

    void Context::read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* out) {
        sync_point(*this);
        PassTarget src;
        TextureObj* t = nullptr;
        uint32_t level = 0, layer = 0;
        if (format == GL_DEPTH_COMPONENT) {
            if (resolve_target(read_fbo, src, false)) {
                t = src.depth;
                level = src.depth_level;
            }
        }
        else if (resolve_target(read_fbo, src, true)) {
            t = src.color[0];
            level = src.color_level[0];
            layer = src.color_layer[0];
        }
        if (!t) {
            set_error(GL_INVALID_OPERATION);
            return;
        }

        if (pixel_pack_buffer) {
            BufferObj* b = buffers.get(pixel_pack_buffer);
            if (!b)
                return;
            size_t size = (size_t)h * align_up((size_t)w * gl_format_components(format) * gl_type_size(type), (size_t)pack_alignment);
            std::vector<uint8_t> tmp(size);
            texture_read(t, level, layer, x, y, w, h, format, type, tmp.data(), pack_alignment);
            buffer_write(b, (GLintptr)(uintptr_t)out, (GLsizeiptr)size, tmp.data());
            return;
        }
        texture_read(t, level, layer, x, y, w, h, format, type, out, pack_alignment);
    }
}
