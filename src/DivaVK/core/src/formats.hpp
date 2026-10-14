/*
    DivaVK - GL <-> Vulkan format translation.
*/

#pragma once

#include "common.hpp"

namespace dvk {
    struct FormatInfo {
        GLenum gl_internal;
        VkFormat vk;
        VkImageAspectFlags aspect;
        uint32_t block_w, block_h;   // 1x1 for uncompressed
        uint32_t block_bytes;        // bytes per texel / per block
        bool compressed;
        bool depth;
        bool stencil;
        bool is_float;               // for clears and readback
        bool is_int;                 // integer (non-normalized) color format
        VkComponentSwizzle swizzle[4]; // how GL channels map onto the storage format
        uint32_t channels;           // storage channel count
    };

    const FormatInfo* format_from_gl(GLenum internal_format, GLenum format_hint, GLenum type_hint);
    const FormatInfo* format_for_depth_stencil(VkFormat chosen);

    // Converts client pixel data to tightly packed storage-format data.
    // Returns false (and leaves out empty) if the combination isn't supported.
    bool convert_upload(const FormatInfo* fi, GLenum format, GLenum type,
        const void* src, int32_t width, int32_t height, int32_t row_length, int32_t alignment,
        std::vector<uint8_t>& out);

    // Converts tightly packed storage-format data to client format/type.
    bool convert_readback(const FormatInfo* fi, const void* src, int32_t width, int32_t height,
        GLenum format, GLenum type, void* dst, int32_t alignment);

    size_t image_size(const FormatInfo* fi, int32_t width, int32_t height);
    size_t gl_type_size(GLenum type);
    uint32_t gl_format_components(GLenum format);

    uint16_t float_to_half(float f);
    float half_to_float(uint16_t h);

    VkComponentSwizzle gl_swizzle_to_vk(GLint s);
}
