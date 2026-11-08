/*
    DivaVK - textures, renderbuffers and samplers.

    GL lets applications define a texture one level (and one cube face) at a time, with
    any size. Vulkan images are immutable in size/format/level count, so a texture's image
    is (re)created when level 0 changes shape and grown to a full mip chain when a higher
    level or glGenerateMipmap needs it; existing levels are copied across.
*/

#include "context.hpp"
#include <cmath>

namespace dvk {
    static uint32_t full_chain(int32_t w, int32_t h) {
        uint32_t n = 1;
        int32_t m = std::max(w, h);
        while (m > 1) {
            m >>= 1;
            n++;
        }
        return n;
    }

    static VkAccessFlags access_for_layout(VkImageLayout l) {
        switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        default: return 0;
        }
    }

    void Context::texture_transition(VkCommandBuffer cb, TextureObj* t, VkImageLayout from, VkImageLayout to,
        uint32_t base_level, uint32_t level_count) {
        if (!t->image || (from == to && from != VK_IMAGE_LAYOUT_UNDEFINED))
            return;
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        // Broad on purpose: any earlier write, for any later access. Adreno 830 (Odin 3) didn't
        // finish a copy's writes before the next transfer read them with the narrow masks (the
        // window showed half the last copy into the default framebuffer, split on the diagonal).
        b.srcAccessMask = (access_for_layout(from) & (VK_ACCESS_TRANSFER_WRITE_BIT
            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))
            | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = access_for_layout(to) | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange = { t->format->aspect, base_level, level_count, 0, VK_REMAINING_ARRAY_LAYERS };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    TextureObj* Context::texture_for_target(GLenum target) {
        switch (target) {
        case GL_TEXTURE_2D:
            return textures.get(tex_2d[active_texture]);
        case GL_TEXTURE_CUBE_MAP:
        case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
        case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
        case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
            return textures.get(tex_cube[active_texture]);
        case GL_RENDERBUFFER:
            return textures.get(renderbuffer);
        default:
            log_once("tex_target", "Unsupported texture target 0x%04X", target);
            return nullptr;
        }
    }

    static void destroy_image(Context& ctx, TextureObj* t) {
        ctx.on_texture_destroyed(t);
        VkDevice dev = ctx.device;
        VmaAllocator vma = ctx.vma;
        VkImage img = t->image;
        VmaAllocation alloc = t->alloc;
        std::vector<VkImageView> views;
        if (t->view)
            views.push_back(t->view);
        for (auto& it : t->attach_views)
            views.push_back(it.second);
        ctx.defer_destroy([dev, vma, img, alloc, views]() {
            for (VkImageView v : views)
                vkDestroyImageView(dev, v, nullptr);
            if (img)
                vmaDestroyImage(vma, img, alloc);
        });
        t->image = VK_NULL_HANDLE;
        t->alloc = VK_NULL_HANDLE;
        t->view = VK_NULL_HANDLE;
        t->attach_views.clear();
        t->view_dirty = true;
    }

    void Context::texture_destroy(TextureObj* t) {
        if (t->image || t->view)
            destroy_image(*this, t);
        t->levels = 0;
        t->defined_levels = 0;
    }

    bool Context::texture_alloc(TextureObj* t, GLenum target, GLenum internal_format,
        int32_t width, int32_t height, uint32_t levels, uint32_t samples) {
        const FormatInfo* fi = format_from_gl(internal_format, 0, 0);
        if (!fi) {
            log_once(("fmt" + std::to_string(internal_format)).c_str(),
                "Unsupported internal format 0x%04X", internal_format);
            set_error(GL_INVALID_ENUM);
            return false;
        }
        if (fi->compressed && !features.textureCompressionBC)
            log_once("no_bc", "Device lacks BC texture support; compressed textures will be black (decoder is phase 2)");

        if (t->image)
            destroy_image(*this, t);

        const bool cube = target == GL_TEXTURE_CUBE_MAP;
        t->target = cube ? GL_TEXTURE_CUBE_MAP : (target == GL_RENDERBUFFER ? GL_RENDERBUFFER : GL_TEXTURE_2D);
        t->internal_format = internal_format;
        t->format = fi;
        t->width = std::max(width, 1);
        t->height = std::max(height, 1);
        t->layers = cube ? 6 : 1;
        t->levels = std::max(levels, 1u);
        t->defined_levels = 0;

        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, fi->vk, &fp);
        const VkFormatFeatureFlags ff = fp.optimalTilingFeatures;

        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        t->renderable = false;
        if (fi->depth || fi->stencil) {
            if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                t->renderable = true;
            }
        }
        else if (!fi->compressed && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            t->renderable = true;
        }

        uint32_t max_samples = fi->depth ? props.limits.framebufferDepthSampleCounts
            : props.limits.framebufferColorSampleCounts;
        // the most samples supported up to the request (counts can skip: llvmpipe offers 1 and 4)
        uint32_t s = 1;
        for (uint32_t c = 2; c <= std::max(samples, 1u); c *= 2)
            if (max_samples & c)
                s = c;
        t->samples = s;
        if (s > 1)
            t->levels = 1;

        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fi->vk;
        ici.extent = { (uint32_t)t->width, (uint32_t)t->height, 1 };
        ici.mipLevels = t->levels;
        ici.arrayLayers = t->layers;
        ici.samples = (VkSampleCountFlagBits)s;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        // DIVAVK_NO_COMPRESSION=1 (a test): colour render targets made so a mobile GPU's
        // framebuffer compression (Adreno's UBWC) is unlikely to be used: mutable format and,
        // where the format allows it, storage use. For the Odin 3's right-eye triangle, where
        // one triangle of a copy reads back as a flat colour or a stretched corner.
        static int no_compress = -1;
        if (no_compress < 0) {
            const char* e = env("DIVAVK_NO_COMPRESSION");
            no_compress = e && e[0] == '1';
            if (no_compress)
                log("Render targets: made without framebuffer compression where possible (DIVAVK_NO_COMPRESSION=1)");
        }
        if (no_compress && t->renderable && !fi->depth && !fi->stencil) {
            ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            if (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(vma, &ici, &aci, &t->image, &t->alloc, nullptr) != VK_SUCCESS) {
            log("Failed to create %dx%d image (format %d, %u levels)", t->width, t->height, (int)fi->vk, t->levels);
            t->image = VK_NULL_HANDLE;
            return false;
        }

        if (s > 1)
            t->rest_layout = fi->depth || fi->stencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        else
            t->rest_layout = (usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        // New images are unused, so the initial transition can always be hoisted.
        texture_transition(upload_cmd(), t, VK_IMAGE_LAYOUT_UNDEFINED, t->rest_layout);
        t->last_used_serial = 0;
        t->view_dirty = true;
        return true;
    }

    VkCommandBuffer Context::transfer_cmd_for(TextureObj* t) {
        if (t->last_used_serial != serial) {
            stats.hoisted_uploads++;
            return upload_cmd();
        }
        if (pass_active)
            stats.pass_breaks++;
        end_pass();
        stats.inline_uploads++;
        return cmd();
    }

    void Context::texture_upload_region(TextureObj* t, uint32_t level, uint32_t layer,
        int32_t x, int32_t y, int32_t w, int32_t h, const void* data, size_t size, size_t row_pitch_texels) {
        if (!t->image || !data || !size)
            return;
        begin_frame_if_needed();
        UploadArena::Alloc src = frame().arena.alloc(*this, size, std::max<VkDeviceSize>(t->format->block_bytes, 16));
        memcpy(src.ptr, data, size);

        VkCommandBuffer cb = transfer_cmd_for(t);
        texture_transition(cb, t, t->rest_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level, 1);
        VkBufferImageCopy region = {};
        region.bufferOffset = src.offset;
        region.bufferRowLength = (uint32_t)row_pitch_texels;
        region.imageSubresource = { t->format->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? t->format->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT : t->format->aspect, level, layer, 1 };
        region.imageOffset = { x, y, 0 };
        region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyBufferToImage(cb, src.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        texture_transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->rest_layout, level, 1);
    }

    // Grows the image to a full mip chain, copying defined levels across.
    static bool grow_levels(Context& ctx, TextureObj* t, uint32_t needed) {
        if (t->levels >= needed)
            return true;
        TextureObj old = TextureObj();
        old.image = t->image;
        old.alloc = t->alloc;
        old.format = t->format;
        old.levels = t->levels;
        old.layers = t->layers;
        old.rest_layout = t->rest_layout;
        old.view = t->view;
        old.attach_views = std::move(t->attach_views);
        uint32_t defined = t->defined_levels;
        uint32_t old_levels = t->levels;
        uint64_t used = t->last_used_serial;

        t->image = VK_NULL_HANDLE;
        t->view = VK_NULL_HANDLE;
        t->attach_views.clear();
        ctx.on_texture_destroyed(t);

        GLenum target = t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        if (!ctx.texture_alloc(t, target, t->internal_format, t->width, t->height,
            full_chain(t->width, t->height), 1)) {
            return false;
        }
        t->defined_levels = defined;
        t->last_used_serial = used;

        VkCommandBuffer cb = ctx.transfer_cmd_for(t);
        ctx.texture_transition(cb, &old, old.rest_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        ctx.texture_transition(cb, t, t->rest_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        std::vector<VkImageCopy> regions;
        for (uint32_t l = 0; l < old_levels; l++) {
            if (!(defined & (1u << l)))
                continue;
            VkImageCopy c = {};
            c.srcSubresource = { t->format->aspect, l, 0, t->layers };
            c.dstSubresource = c.srcSubresource;
            c.extent = { (uint32_t)std::max(t->width >> l, 1), (uint32_t)std::max(t->height >> l, 1), 1 };
            regions.push_back(c);
        }
        if (!regions.empty())
            vkCmdCopyImage(cb, old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
        ctx.texture_transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->rest_layout);

        VkDevice dev = ctx.device;
        VmaAllocator vma = ctx.vma;
        VkImage img = old.image;
        VmaAllocation alloc = old.alloc;
        std::vector<VkImageView> views;
        if (old.view)
            views.push_back(old.view);
        for (auto& it : old.attach_views)
            views.push_back(it.second);
        ctx.defer_destroy([dev, vma, img, alloc, views]() {
            for (VkImageView v : views)
                vkDestroyImageView(dev, v, nullptr);
            vmaDestroyImage(vma, img, alloc);
        });
        return true;
    }

    static uint32_t face_layer(GLenum target) {
        if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
            return target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
        return 0;
    }

    static GLenum base_target(GLenum target) {
        if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
            return GL_TEXTURE_CUBE_MAP;
        return target;
    }

    // Makes sure level `level` of a texture with level-0 size (w0,h0) exists.
    static bool prepare_level(Context& ctx, TextureObj* t, GLenum target, GLint level, GLenum internal_format,
        int32_t width, int32_t height) {
        GLenum bt = base_target(target);
        int32_t w0 = level ? width << level : width;
        int32_t h0 = level ? height << level : height;

        bool same_shape = t->image && t->internal_format == internal_format
            && (bt == GL_TEXTURE_CUBE_MAP) == (t->target == GL_TEXTURE_CUBE_MAP);
        if (level == 0) {
            if (!same_shape || t->width != width || t->height != height) {
                if (!ctx.texture_alloc(t, bt, internal_format, width, height, 1, 1))
                    return false;
            }
            return true;
        }

        if (!same_shape) {
            // A level > 0 arrived before level 0 (or with a different format)
            if (!ctx.texture_alloc(t, bt, internal_format, std::max(w0, 1), std::max(h0, 1),
                full_chain(std::max(w0, 1), std::max(h0, 1)), 1))
                return false;
            return true;
        }
        return grow_levels(ctx, t, (uint32_t)level + 1);
    }

    static const void* unpack_source(Context& ctx, const void* pixels, size_t size, std::vector<uint8_t>& tmp) {
        if (!ctx.pixel_unpack_buffer)
            return pixels;
        BufferObj* b = ctx.buffers.get(ctx.pixel_unpack_buffer);
        if (!b || !b->created)
            return nullptr;
        size_t off = (size_t)(uintptr_t)pixels;
        if (off + size > b->size)
            return nullptr;
        tmp.resize(size);
        ctx.buffer_read(b, (GLintptr)off, (GLsizeiptr)size, tmp.data());
        return tmp.data();
    }

    void Context::texture_image(TextureObj* t, GLenum target, GLint level, GLenum internal_format,
        int32_t width, int32_t height, GLenum format, GLenum type, const void* pixels) {
        if (!t || level < 0 || width < 0 || height < 0)
            return;
        // GL derives some formats from the client type (unsized float RGBA etc.)
        const FormatInfo* fi = format_from_gl(internal_format, format, type);
        if (fi && fi->gl_internal != internal_format)
            internal_format = fi->gl_internal;
        if (!width || !height)
            return;
        if (!prepare_level(*this, t, target, level, internal_format, width, height))
            return;

        t->defined_levels |= 1u << level;
        t->view_dirty = true;
        if (!pixels && !pixel_unpack_buffer)
            return;

        size_t client_size = (size_t)height * align_up((size_t)(unpack_row_length ? unpack_row_length : width)
            * std::max<size_t>(gl_format_components(format) * std::max<size_t>(gl_type_size(type), 1), 1),
            (size_t)unpack_alignment);
        std::vector<uint8_t> tmp;
        const void* src = unpack_source(*this, pixels, client_size, tmp);
        if (!src)
            return;

        std::vector<uint8_t> converted;
        if (!convert_upload(t->format, format, type, src, width, height, unpack_row_length, unpack_alignment, converted)) {
            log_once(("conv" + std::to_string(t->internal_format) + "_" + std::to_string(format) + "_" + std::to_string(type)).c_str(),
                "Unsupported upload: internal 0x%04X from format 0x%04X type 0x%04X", t->internal_format, format, type);
            return;
        }
        texture_upload_region(t, (uint32_t)level, face_layer(target), 0, 0, width, height,
            converted.data(), converted.size(), (size_t)width);
    }

    void Context::texture_sub_image(TextureObj* t, GLenum target, GLint level, GLint x, GLint y,
        int32_t width, int32_t height, GLenum format, GLenum type, const void* pixels) {
        if (!t || !t->image || width <= 0 || height <= 0 || (uint32_t)level >= t->levels)
            return;
        size_t client_size = (size_t)height * align_up((size_t)(unpack_row_length ? unpack_row_length : width)
            * std::max<size_t>(gl_format_components(format) * std::max<size_t>(gl_type_size(type), 1), 1),
            (size_t)unpack_alignment);
        std::vector<uint8_t> tmp;
        const void* src = unpack_source(*this, pixels, client_size, tmp);
        if (!src)
            return;
        std::vector<uint8_t> converted;
        if (!convert_upload(t->format, format, type, src, width, height, unpack_row_length, unpack_alignment, converted)) {
            log_once(("conv" + std::to_string(t->internal_format) + "_" + std::to_string(format) + "_" + std::to_string(type)).c_str(),
                "Unsupported upload: internal 0x%04X from format 0x%04X type 0x%04X", t->internal_format, format, type);
            return;
        }
        texture_upload_region(t, (uint32_t)level, face_layer(target), x, y, width, height,
            converted.data(), converted.size(), (size_t)width);
    }

    void Context::texture_compressed_image(TextureObj* t, GLenum target, GLint level, GLenum internal_format,
        int32_t width, int32_t height, GLsizei size, const void* data, bool sub, GLint x, GLint y) {
        if (!t || level < 0 || width <= 0 || height <= 0)
            return;
        if (!sub) {
            if (!prepare_level(*this, t, target, level, internal_format, width, height))
                return;
            t->defined_levels |= 1u << level;
            t->view_dirty = true;
        }
        else if (!t->image || (uint32_t)level >= t->levels)
            return;

        if (!data && !pixel_unpack_buffer)
            return;
        std::vector<uint8_t> tmp;
        const void* src = unpack_source(*this, data, (size_t)size, tmp);
        if (!src)
            return;
        size_t expected = image_size(t->format, width, height);
        if ((size_t)size < expected) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        // Vulkan wants block-aligned extents except at the edge of the level; clamp to the level.
        int32_t lw = std::max(t->width >> level, 1);
        int32_t lh = std::max(t->height >> level, 1);
        int32_t cw = std::min(align_up(width, (int32_t)t->format->block_w), lw - x);
        int32_t ch = std::min(align_up(height, (int32_t)t->format->block_h), lh - y);
        texture_upload_region(t, (uint32_t)level, face_layer(target), x, y, cw, ch, src, expected,
            (size_t)align_up(width, (int32_t)t->format->block_w));
    }

    void Context::texture_generate_mipmap(TextureObj* t) {
        if (!t || !t->image || t->samples > 1)
            return;
        if (t->format->compressed || t->format->depth) {
            log_once("genmip_fmt", "glGenerateMipmap on compressed/depth texture ignored");
            return;
        }
        uint32_t chain = full_chain(t->width, t->height);
        if (!grow_levels(*this, t, chain))
            return;

        VkCommandBuffer cb = transfer_cmd_for(t);
        for (uint32_t l = 1; l < t->levels; l++) {
            texture_transition(cb, t, t->rest_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l - 1, 1);
            texture_transition(cb, t, t->rest_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, l, 1);
            VkImageBlit b = {};
            b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, t->layers };
            b.srcOffsets[1] = { std::max(t->width >> (l - 1), 1), std::max(t->height >> (l - 1), 1), 1 };
            b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, t->layers };
            b.dstOffsets[1] = { std::max(t->width >> l, 1), std::max(t->height >> l, 1), 1 };
            vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            texture_transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rest_layout, l - 1, 1);
            texture_transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->rest_layout, l, 1);
        }
        t->defined_levels = (1u << t->levels) - 1;
        t->view_dirty = true;
    }

    VkImageView Context::texture_view(TextureObj* t) {
        if (!t->image)
            return VK_NULL_HANDLE;
        if (t->view && !t->view_dirty)   // the usual case, without the lock
            return t->view;
        std::lock_guard<std::mutex> view_lock(sh->view_mutex);
        if (t->view && !t->view_dirty)
            return t->view;
        if (t->view) {
            VkDevice dev = device;
            VkImageView v = t->view;
            defer_destroy([dev, v]() { vkDestroyImageView(dev, v, nullptr); });
            t->view = VK_NULL_HANDLE;
        }

        // Levels: contiguous defined range starting at base_level, clamped by max_level
        uint32_t base = (uint32_t)std::min<GLint>(std::max(t->base_level, 0), (GLint)t->levels - 1);
        uint32_t count = 0;
        while (base + count < t->levels && (GLint)(base + count) <= t->max_level
            && (t->defined_levels & (1u << (base + count))))
            count++;
        if (!count)
            count = 1;

        // GL swizzle selects among the texture's logical channels, which the format may
        // itself emulate with a swizzle (luminance/alpha formats).
        const FormatInfo* fi = t->format;
        VkComponentSwizzle comp[4];
        for (int i = 0; i < 4; i++) {
            switch (t->swizzle[i]) {
            case GL_RED: comp[i] = fi->swizzle[0]; break;
            case GL_GREEN: comp[i] = fi->swizzle[1]; break;
            case GL_BLUE: comp[i] = fi->swizzle[2]; break;
            case GL_ALPHA: comp[i] = fi->swizzle[3]; break;
            case GL_ZERO: comp[i] = VK_COMPONENT_SWIZZLE_ZERO; break;
            case GL_ONE: comp[i] = VK_COMPONENT_SWIZZLE_ONE; break;
            default: comp[i] = fi->swizzle[i]; break;
            }
        }

        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = t->image;
        vci.viewType = t->target == GL_TEXTURE_CUBE_MAP ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fi->vk;
        vci.components = { comp[0], comp[1], comp[2], comp[3] };
        VkImageAspectFlags aspect = fi->aspect;
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT; // sample depth, never stencil
        vci.subresourceRange = { aspect, base, count, 0, t->layers };
        DVK_CHECK(vkCreateImageView(device, &vci, nullptr, &t->view));
        t->view_dirty = false;
        return t->view;
    }

    VkImageView Context::texture_attach_view(TextureObj* t, uint32_t level, uint32_t layer) {
        uint32_t key = (level << 8) | layer;
        std::lock_guard<std::mutex> view_lock(sh->view_mutex);
        auto it = t->attach_views.find(key);
        if (it != t->attach_views.end())
            return it->second;
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = t->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = t->format->vk;
        vci.subresourceRange = { t->format->aspect, level, 1, layer, 1 };
        VkImageView v = VK_NULL_HANDLE;
        DVK_CHECK(vkCreateImageView(device, &vci, nullptr, &v));
        t->attach_views[key] = v;
        return v;
    }

    // ------------------------------------------------------------ samplers

    bool SamplerState::set_param(GLenum pname, const float* fv, const GLint* iv) {
        auto ival = [&]() { return iv ? (GLenum)iv[0] : (GLenum)fv[0]; };
        auto fval = [&]() { return fv ? fv[0] : (float)iv[0]; };
        switch (pname) {
        case GL_TEXTURE_MIN_FILTER: min_filter = ival(); return true;
        case GL_TEXTURE_MAG_FILTER: mag_filter = ival(); return true;
        case GL_TEXTURE_WRAP_S: wrap_s = ival(); return true;
        case GL_TEXTURE_WRAP_T: wrap_t = ival(); return true;
        case GL_TEXTURE_WRAP_R: wrap_r = ival(); return true;
        case GL_TEXTURE_COMPARE_MODE: compare_mode = ival(); return true;
        case GL_TEXTURE_COMPARE_FUNC: compare_func = ival(); return true;
        case GL_TEXTURE_MIN_LOD: min_lod = fval(); return true;
        case GL_TEXTURE_MAX_LOD: max_lod = fval(); return true;
        case GL_TEXTURE_LOD_BIAS: lod_bias = fval(); return true;
        case GL_TEXTURE_MAX_ANISOTROPY: max_anisotropy = fval(); return true;
        case GL_TEXTURE_BORDER_COLOR:
            for (int i = 0; i < 4; i++)
                border_color[i] = fv ? fv[i] : (float)iv[i] / 2147483647.0f;
            return true;
        default:
            return false;
        }
    }

    uint64_t SamplerState::hash() const {
        return hash_bytes(this, sizeof(*this));
    }

    static VkSamplerAddressMode address_mode(GLenum wrap) {
        switch (wrap) {
        case GL_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case GL_CLAMP:
        case GL_CLAMP_TO_EDGE:
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    }

    static VkCompareOp compare_op(GLenum f) {
        switch (f) {
        case GL_NEVER: return VK_COMPARE_OP_NEVER;
        case GL_LESS: return VK_COMPARE_OP_LESS;
        case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
        case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case GL_GREATER: return VK_COMPARE_OP_GREATER;
        case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    VkCompareOp gl_compare_op(GLenum f) {
        return compare_op(f);
    }

    VkSampler Context::get_sampler(const SamplerState& s, const TextureObj* t) {
        // Linear filtering support depends on the format (e.g. D32 on some GPUs)
        bool linear_ok = true;
        if (t && t->format) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(phys, t->format->vk, &fp);
            linear_ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        }

        uint64_t h = s.hash();
        hash_combine(h, linear_ok ? 1 : 0);
        {
            auto lit = local_samplers.find(h);
            if (lit != local_samplers.end())
                return lit->second;
            std::lock_guard<std::mutex> l(sh->cache_mutex);
            auto it = vk_samplers.find(h);
            if (it != vk_samplers.end()) {
                local_samplers[h] = it->second;
                return it->second;
            }
        }

        auto is_linear = [](GLenum f) {
            return f == GL_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_LINEAR;
        };
        bool mip = s.min_filter != GL_NEAREST && s.min_filter != GL_LINEAR;

        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = is_linear(s.mag_filter) && linear_ok ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = is_linear(s.min_filter) && linear_ok ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = (s.min_filter == GL_NEAREST_MIPMAP_LINEAR || s.min_filter == GL_LINEAR_MIPMAP_LINEAR)
            && linear_ok ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = address_mode(s.wrap_s);
        sci.addressModeV = address_mode(s.wrap_t);
        sci.addressModeW = address_mode(s.wrap_r);
        sci.mipLodBias = s.lod_bias;
        sci.anisotropyEnable = features.samplerAnisotropy && s.max_anisotropy > 1.0f && mip;
        sci.maxAnisotropy = std::min(s.max_anisotropy, props.limits.maxSamplerAnisotropy);
        sci.compareEnable = s.compare_mode == GL_COMPARE_REF_TO_TEXTURE;
        sci.compareOp = compare_op(s.compare_func);
        sci.minLod = mip ? std::max(s.min_lod, 0.0f) : 0.0f;
        sci.maxLod = mip ? std::max(s.max_lod, sci.minLod) : 0.25f; // spec-recommended non-mip emulation

        // Vulkan core only has three border colors; pick the closest.
        const float* bc = s.border_color;
        if (bc[3] < 0.5f)
            sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (bc[0] + bc[1] + bc[2] > 1.5f)
            sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else
            sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

        VkSampler smp = VK_NULL_HANDLE;
        DVK_CHECK(vkCreateSampler(device, &sci, nullptr, &smp));
        std::lock_guard<std::mutex> l(sh->cache_mutex);
        auto it = vk_samplers.find(h);
        if (it != vk_samplers.end()) {
            vkDestroySampler(device, smp, nullptr);
            return it->second;
        }
        vk_samplers[h] = smp;
        return smp;
    }

    // ------------------------------------------------------------ readback

    void Context::texture_read(TextureObj* t, uint32_t level, uint32_t layer, int32_t x, int32_t y,
        int32_t w, int32_t h, GLenum format, GLenum type, void* out, GLint alignment) {
        if (!t || !t->image || w <= 0 || h <= 0)
            return;

        // Multisampled sources must be resolved first; DivaGL never reads those directly.
        if (t->samples > 1) {
            log_once("read_msaa", "Readback from multisampled image is not supported");
            return;
        }

        const FormatInfo* fi = t->format;
        VkImageAspectFlags aspect = fi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : fi->aspect;
        size_t size = image_size(fi, w, h);
        if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT && fi->stencil)
            size = (size_t)w * h * 4; // depth aspect of D24S8/D32S8 copies as 32 bits

        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   // nothing here flushes or invalidates by hand
        VkBuffer rb;
        VmaAllocation ra;
        VmaAllocationInfo ai = {};
        DVK_CHECK(vmaCreateBuffer(vma, &bci, &aci, &rb, &ra, &ai));

        end_pass();
        flush_pending_clear();
        end_pass();
        VkCommandBuffer cb = cmd();
        texture_transition(cb, t, t->rest_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = {};
        region.imageSubresource = { aspect, level, layer, 1 };
        region.imageOffset = { x, y, 0 };
        region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
        texture_transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rest_layout);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        t->last_used_serial = serial;

        Frame& f = frame();
        submit_frame(false);
        wait_frame(f);
        vmaInvalidateAllocation(vma, ra, 0, VK_WHOLE_SIZE);

        if (fi->compressed)
            memcpy(out, ai.pMappedData, size);
        else if (fi->depth && fi->stencil && (format == GL_DEPTH_COMPONENT)) {
            // D24 in the low 24 bits, or D32F
            const uint32_t* src = (const uint32_t*)ai.pMappedData;
            for (int32_t i = 0; i < w * h; i++) {
                float d;
                if (fi->vk == VK_FORMAT_D24_UNORM_S8_UINT)
                    d = (float)(src[i] & 0xFFFFFF) / 16777215.0f;
                else
                    memcpy(&d, &src[i], 4);
                if (type == GL_FLOAT)
                    ((float*)out)[i] = d;
                else if (type == GL_UNSIGNED_INT)
                    ((uint32_t*)out)[i] = (uint32_t)((double)d * 4294967295.0);
            }
        }
        else if (!convert_readback(fi, ai.pMappedData, w, h, format, type, out, alignment))
            log_once(("rb" + std::to_string(fi->vk) + "_" + std::to_string(format) + "_" + std::to_string(type)).c_str(),
                "Unsupported readback: vk format %d to 0x%04X/0x%04X", (int)fi->vk, format, type);
        vmaDestroyBuffer(vma, rb, ra);
    }
}
