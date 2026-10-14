/*
    DivaVK - GL <-> Vulkan format translation.
*/

#include "formats.hpp"
#include <cmath>

namespace dvk {
    static const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R;
    static const VkComponentSwizzle G = VK_COMPONENT_SWIZZLE_G;
    static const VkComponentSwizzle B = VK_COMPONENT_SWIZZLE_B;
    static const VkComponentSwizzle A = VK_COMPONENT_SWIZZLE_A;
    static const VkComponentSwizzle Z = VK_COMPONENT_SWIZZLE_ZERO;
    static const VkComponentSwizzle O = VK_COMPONENT_SWIZZLE_ONE;

    static const VkImageAspectFlags COLOR = VK_IMAGE_ASPECT_COLOR_BIT;

    // Which logical (GL) channel is stored in each storage channel: 0..3 = R,G,B,A.
    struct StoreMap {
        GLenum gl;
        uint8_t from[4];
    };

#define FMT(gl, vk, bw, bh, bytes, comp, flt, ch, s0, s1, s2, s3) \
    { gl, vk, COLOR, bw, bh, bytes, comp, false, false, flt, false, { s0, s1, s2, s3 }, ch }

    static const FormatInfo color_formats[] = {
        FMT(GL_RGBA8,                VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, A),
        FMT(GL_RGBA,                 VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, A),
        FMT(GL_RGB8,                 VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, O),
        FMT(GL_RGB,                  VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, O),
        FMT(GL_RGB5,                 VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, O),
        FMT(GL_RGB565,               VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, O),
        FMT(GL_RGB5_A1,              VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, A),
        FMT(GL_RGBA4,                VK_FORMAT_R8G8B8A8_UNORM,   1, 1, 4,  false, false, 4, R, G, B, A),
        FMT(GL_SRGB8_ALPHA8,         VK_FORMAT_R8G8B8A8_SRGB,    1, 1, 4,  false, false, 4, R, G, B, A),
        FMT(GL_R8,                   VK_FORMAT_R8_UNORM,         1, 1, 1,  false, false, 1, R, G, B, A),
        FMT(GL_RED,                  VK_FORMAT_R8_UNORM,         1, 1, 1,  false, false, 1, R, G, B, A),
        FMT(GL_RG8,                  VK_FORMAT_R8G8_UNORM,       1, 1, 2,  false, false, 2, R, G, B, A),
        FMT(GL_RG,                   VK_FORMAT_R8G8_UNORM,       1, 1, 2,  false, false, 2, R, G, B, A),
        FMT(GL_RGBA16,               VK_FORMAT_R16G16B16A16_UNORM, 1, 1, 8, false, false, 4, R, G, B, A),
        FMT(GL_RGB10_A2,             VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1, 4, false, false, 4, R, G, B, A),
        FMT(GL_R16F,                 VK_FORMAT_R16_SFLOAT,       1, 1, 2,  false, true,  1, R, G, B, A),
        FMT(GL_RG16F,                VK_FORMAT_R16G16_SFLOAT,    1, 1, 4,  false, true,  2, R, G, B, A),
        FMT(GL_RGB16F,               VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8, false, true, 4, R, G, B, O),
        FMT(GL_RGBA16F,              VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8, false, true, 4, R, G, B, A),
        FMT(GL_R32F,                 VK_FORMAT_R32_SFLOAT,       1, 1, 4,  false, true,  1, R, G, B, A),
        FMT(GL_RG32F,                VK_FORMAT_R32G32_SFLOAT,    1, 1, 8,  false, true,  2, R, G, B, A),
        FMT(GL_RGB32F,               VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 16, false, true, 4, R, G, B, O),
        FMT(GL_RGBA32F,              VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 16, false, true, 4, R, G, B, A),
        FMT(GL_R11F_G11F_B10F,       VK_FORMAT_B10G11R11_UFLOAT_PACK32, 1, 1, 4, false, true, 3, R, G, B, O),
        // Legacy single/dual channel formats, emulated with swizzles
        FMT(GL_LUMINANCE8,           VK_FORMAT_R8_UNORM,         1, 1, 1,  false, false, 1, R, R, R, O),
        FMT(GL_LUMINANCE,            VK_FORMAT_R8_UNORM,         1, 1, 1,  false, false, 1, R, R, R, O),
        FMT(GL_LUMINANCE8_ALPHA8,    VK_FORMAT_R8G8_UNORM,       1, 1, 2,  false, false, 2, R, R, R, G),
        FMT(GL_LUMINANCE_ALPHA,      VK_FORMAT_R8G8_UNORM,       1, 1, 2,  false, false, 2, R, R, R, G),
        FMT(GL_ALPHA8,               VK_FORMAT_R8_UNORM,         1, 1, 1,  false, false, 1, Z, Z, Z, R),
        FMT(GL_ALPHA,                VK_FORMAT_R8_UNORM,         1, 1, 1,  false, false, 1, Z, Z, Z, R),
        FMT(GL_INTENSITY8,           VK_FORMAT_R8_UNORM,         1, 1, 1,  false, false, 1, R, R, R, R),
        FMT(GL_LUMINANCE_ALPHA16F_ARB, VK_FORMAT_R16G16_SFLOAT,  1, 1, 4,  false, true,  2, R, R, R, G),
        // Block compressed
        FMT(GL_COMPRESSED_RGB_S3TC_DXT1_EXT,  VK_FORMAT_BC1_RGB_UNORM_BLOCK,  4, 4, 8,  true, false, 4, R, G, B, O),
        FMT(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8,  true, false, 4, R, G, B, A),
        FMT(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, VK_FORMAT_BC2_UNORM_BLOCK,      4, 4, 16, true, false, 4, R, G, B, A),
        FMT(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, VK_FORMAT_BC3_UNORM_BLOCK,      4, 4, 16, true, false, 4, R, G, B, A),
        FMT(GL_COMPRESSED_RED_RGTC1,          VK_FORMAT_BC4_UNORM_BLOCK,      4, 4, 8,  true, false, 1, R, G, B, A),
        FMT(GL_COMPRESSED_RG_RGTC2,           VK_FORMAT_BC5_UNORM_BLOCK,      4, 4, 16, true, false, 2, R, G, B, A),
    };
#undef FMT

    static const StoreMap store_maps[] = {
        { GL_LUMINANCE8,             { 0, 0, 0, 0 } },
        { GL_LUMINANCE,              { 0, 0, 0, 0 } },
        { GL_LUMINANCE8_ALPHA8,      { 0, 3, 0, 0 } },
        { GL_LUMINANCE_ALPHA,        { 0, 3, 0, 0 } },
        { GL_LUMINANCE_ALPHA16F_ARB, { 0, 3, 0, 0 } },
        { GL_ALPHA8,                 { 3, 0, 0, 0 } },
        { GL_ALPHA,                  { 3, 0, 0, 0 } },
        { GL_INTENSITY8,             { 0, 0, 0, 0 } },
    };

    // Depth formats are chosen at device init (D24 support varies by vendor).
    static FormatInfo depth_formats[] = {
        { GL_DEPTH_COMPONENT16,  VK_FORMAT_D16_UNORM,  VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 2, false, true, false, false, false, { R, G, B, A }, 1 },
        { GL_DEPTH_COMPONENT24,  VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 4, false, true, false, true,  false, { R, G, B, A }, 1 },
        { GL_DEPTH_COMPONENT32,  VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 4, false, true, false, true,  false, { R, G, B, A }, 1 },
        { GL_DEPTH_COMPONENT32F, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 4, false, true, false, true,  false, { R, G, B, A }, 1 },
        { GL_DEPTH_COMPONENT,    VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, 4, false, true, false, true,  false, { R, G, B, A }, 1 },
        { GL_DEPTH24_STENCIL8,   VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1, 1, 4, false, true, true, false, false, { R, G, B, A }, 1 },
        { GL_DEPTH_STENCIL,      VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1, 1, 4, false, true, true, false, false, { R, G, B, A }, 1 },
        { GL_DEPTH32F_STENCIL8,  VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1, 1, 8, false, true, true, true, false, { R, G, B, A }, 1 },
        { GL_STENCIL_INDEX8,     VK_FORMAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT, 1, 1, 1, false, false, true, false, false, { R, G, B, A }, 1 },
    };

    const FormatInfo* format_for_depth_stencil(VkFormat chosen) {
        // Called once at init with the best D24S8 substitute.
        for (FormatInfo& f : depth_formats)
            if (f.gl_internal == GL_DEPTH24_STENCIL8 || f.gl_internal == GL_DEPTH_STENCIL) {
                f.vk = chosen;
                f.block_bytes = chosen == VK_FORMAT_D32_SFLOAT_S8_UINT ? 8 : 4;
                f.is_float = chosen == VK_FORMAT_D32_SFLOAT_S8_UINT;
            }
        return &depth_formats[5];
    }

    const FormatInfo* format_from_gl(GLenum internal_format, GLenum format_hint, GLenum type_hint) {
        // Unsized float formats derive from the client type, as in GL.
        if (internal_format == GL_RGBA && (type_hint == GL_FLOAT || type_hint == GL_HALF_FLOAT))
            internal_format = type_hint == GL_FLOAT ? GL_RGBA32F : GL_RGBA16F;
        else if (internal_format == GL_RGB && (type_hint == GL_FLOAT || type_hint == GL_HALF_FLOAT))
            internal_format = type_hint == GL_FLOAT ? GL_RGB32F : GL_RGB16F;

        for (const FormatInfo& f : color_formats)
            if (f.gl_internal == internal_format)
                return &f;
        for (const FormatInfo& f : depth_formats)
            if (f.gl_internal == internal_format)
                return &f;
        (void)format_hint;
        return nullptr;
    }

    static const uint8_t* store_map_for(GLenum gl) {
        static const uint8_t identity[4] = { 0, 1, 2, 3 };
        for (const StoreMap& m : store_maps)
            if (m.gl == gl)
                return m.from;
        return identity;
    }

    size_t gl_type_size(GLenum type) {
        switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
        case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return 2;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
        default: return 0; // packed types handled separately
        }
    }

    static size_t packed_type_size(GLenum type) {
        switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
        case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
        case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
            return 2;
        case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
        case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10_10_10_2:
        case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV:
            return 4;
        default:
            return 0;
        }
    }

    uint32_t gl_format_components(GLenum format) {
        switch (format) {
        case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE:
        case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: case GL_RED_INTEGER:
            return 1;
        case GL_RG: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: case GL_RG_INTEGER:
            return 2;
        case GL_RGB: case GL_BGR: case GL_RGB_INTEGER:
            return 3;
        case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER:
            return 4;
        default:
            return 0;
        }
    }

    static size_t client_pixel_size(GLenum format, GLenum type) {
        size_t p = packed_type_size(type);
        if (p)
            return p;
        return gl_format_components(format) * gl_type_size(type);
    }

    size_t image_size(const FormatInfo* fi, int32_t width, int32_t height) {
        size_t bw = (size_t)(width + fi->block_w - 1) / fi->block_w;
        size_t bh = (size_t)(height + fi->block_h - 1) / fi->block_h;
        return bw * bh * fi->block_bytes;
    }

    // ------------------------------------------------------------ half floats

    uint16_t float_to_half(float f) {
        uint32_t x;
        memcpy(&x, &f, 4);
        uint32_t sign = (x >> 16) & 0x8000;
        int32_t exp = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = x & 0x7FFFFF;
        if (((x >> 23) & 0xFF) == 0xFF)
            return (uint16_t)(sign | 0x7C00 | (mant ? 0x200 : 0));
        if (exp >= 31)
            return (uint16_t)(sign | 0x7C00);
        if (exp <= 0) {
            if (exp < -10)
                return (uint16_t)sign;
            mant |= 0x800000;
            uint32_t shift = (uint32_t)(14 - exp);
            uint32_t h = mant >> shift;
            if ((mant >> (shift - 1)) & 1)
                h++;
            return (uint16_t)(sign | h);
        }
        uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
        if (mant & 0x1000)
            h++;
        return (uint16_t)h;
    }

    float half_to_float(uint16_t h) {
        uint32_t sign = (uint32_t)(h & 0x8000) << 16;
        uint32_t exp = (h >> 10) & 0x1F;
        uint32_t mant = h & 0x3FF;
        uint32_t x;
        if (exp == 0) {
            if (!mant)
                x = sign;
            else {
                exp = 127 - 15 + 1;
                while (!(mant & 0x400)) {
                    mant <<= 1;
                    exp--;
                }
                mant &= 0x3FF;
                x = sign | (exp << 23) | (mant << 13);
            }
        }
        else if (exp == 31)
            x = sign | 0x7F800000 | (mant << 13);
        else
            x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
        float f;
        memcpy(&f, &x, 4);
        return f;
    }

    // ------------------------------------------------------------ texel decode / encode

    static float unorm(uint32_t v, uint32_t bits) {
        return (float)v / (float)((1u << bits) - 1u);
    }

    static uint32_t to_unorm(float f, uint32_t bits) {
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        return (uint32_t)(f * (float)((1u << bits) - 1u) + 0.5f);
    }

    // Decode one client pixel to logical RGBA following GL's rules for missing channels.
    static bool decode_client(GLenum format, GLenum type, const uint8_t* p, float out[4]) {
        float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float v[4] = {};
        uint32_t n = gl_format_components(format);

        switch (type) {
        case GL_UNSIGNED_BYTE:  for (uint32_t i = 0; i < n; i++) v[i] = p[i] / 255.0f; break;
        case GL_BYTE:           for (uint32_t i = 0; i < n; i++) v[i] = std::max(((int8_t*)p)[i] / 127.0f, -1.0f); break;
        case GL_UNSIGNED_SHORT: for (uint32_t i = 0; i < n; i++) v[i] = ((uint16_t*)p)[i] / 65535.0f; break;
        case GL_SHORT:          for (uint32_t i = 0; i < n; i++) v[i] = std::max(((int16_t*)p)[i] / 32767.0f, -1.0f); break;
        case GL_UNSIGNED_INT:   for (uint32_t i = 0; i < n; i++) v[i] = (float)(((uint32_t*)p)[i] / 4294967295.0); break;
        case GL_INT:            for (uint32_t i = 0; i < n; i++) v[i] = (float)(((int32_t*)p)[i] / 2147483647.0); break;
        case GL_HALF_FLOAT:     for (uint32_t i = 0; i < n; i++) v[i] = half_to_float(((uint16_t*)p)[i]); break;
        case GL_FLOAT:          for (uint32_t i = 0; i < n; i++) v[i] = ((float*)p)[i]; break;
        case GL_UNSIGNED_SHORT_5_6_5: {
            uint16_t x = *(uint16_t*)p;
            v[0] = unorm(x >> 11, 5); v[1] = unorm((x >> 5) & 0x3F, 6); v[2] = unorm(x & 0x1F, 5);
            n = 3;
        } break;
        case GL_UNSIGNED_SHORT_4_4_4_4: {
            uint16_t x = *(uint16_t*)p;
            v[0] = unorm(x >> 12, 4); v[1] = unorm((x >> 8) & 0xF, 4);
            v[2] = unorm((x >> 4) & 0xF, 4); v[3] = unorm(x & 0xF, 4);
            n = 4;
        } break;
        case GL_UNSIGNED_SHORT_5_5_5_1: {
            uint16_t x = *(uint16_t*)p;
            v[0] = unorm(x >> 11, 5); v[1] = unorm((x >> 6) & 0x1F, 5);
            v[2] = unorm((x >> 1) & 0x1F, 5); v[3] = (float)(x & 1);
            n = 4;
        } break;
        case GL_UNSIGNED_SHORT_1_5_5_5_REV: {
            uint16_t x = *(uint16_t*)p;
            v[0] = unorm(x & 0x1F, 5); v[1] = unorm((x >> 5) & 0x1F, 5);
            v[2] = unorm((x >> 10) & 0x1F, 5); v[3] = (float)(x >> 15);
            n = 4;
        } break;
        case GL_UNSIGNED_INT_8_8_8_8_REV: {
            for (uint32_t i = 0; i < 4; i++) v[i] = p[i] / 255.0f;
            n = 4;
        } break;
        case GL_UNSIGNED_INT_8_8_8_8: {
            for (uint32_t i = 0; i < 4; i++) v[i] = p[3 - i] / 255.0f;
            n = 4;
        } break;
        case GL_UNSIGNED_INT_2_10_10_10_REV: {
            uint32_t x = *(uint32_t*)p;
            v[0] = unorm(x & 0x3FF, 10); v[1] = unorm((x >> 10) & 0x3FF, 10);
            v[2] = unorm((x >> 20) & 0x3FF, 10); v[3] = unorm(x >> 30, 2);
            n = 4;
        } break;
        default:
            return false;
        }

        switch (format) {
        case GL_RED: case GL_DEPTH_COMPONENT: c[0] = v[0]; break;
        case GL_GREEN: c[1] = v[0]; break;
        case GL_BLUE:  c[2] = v[0]; break;
        case GL_ALPHA: c[3] = v[0]; break;
        case GL_LUMINANCE: c[0] = c[1] = c[2] = v[0]; break;
        case GL_LUMINANCE_ALPHA: c[0] = c[1] = c[2] = v[0]; c[3] = v[1]; break;
        case GL_RG:   c[0] = v[0]; c[1] = v[1]; break;
        case GL_RGB:  c[0] = v[0]; c[1] = v[1]; c[2] = v[2]; break;
        case GL_BGR:  c[0] = v[2]; c[1] = v[1]; c[2] = v[0]; break;
        case GL_RGBA: c[0] = v[0]; c[1] = v[1]; c[2] = v[2]; c[3] = v[3]; break;
        case GL_BGRA: c[0] = v[2]; c[1] = v[1]; c[2] = v[0]; c[3] = v[3]; break;
        default: return false;
        }
        memcpy(out, c, sizeof(c));
        return true;
    }

    static void encode_storage(const FormatInfo* fi, const float c[4], uint8_t* out) {
        const uint8_t* m = store_map_for(fi->gl_internal);
        float s[4] = { c[m[0]], c[m[1]], c[m[2]], c[m[3]] };
        switch (fi->vk) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            for (int i = 0; i < 4; i++) out[i] = (uint8_t)to_unorm(s[i], 8);
            break;
        case VK_FORMAT_R8_UNORM:
            out[0] = (uint8_t)to_unorm(s[0], 8);
            break;
        case VK_FORMAT_R8G8_UNORM:
            out[0] = (uint8_t)to_unorm(s[0], 8);
            out[1] = (uint8_t)to_unorm(s[1], 8);
            break;
        case VK_FORMAT_R16G16B16A16_UNORM:
            for (int i = 0; i < 4; i++) ((uint16_t*)out)[i] = (uint16_t)to_unorm(s[i], 16);
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            *(uint32_t*)out = to_unorm(s[0], 10) | (to_unorm(s[1], 10) << 10)
                | (to_unorm(s[2], 10) << 20) | (to_unorm(s[3], 2) << 30);
            break;
        case VK_FORMAT_R16_SFLOAT:
            ((uint16_t*)out)[0] = float_to_half(s[0]);
            break;
        case VK_FORMAT_R16G16_SFLOAT:
            for (int i = 0; i < 2; i++) ((uint16_t*)out)[i] = float_to_half(s[i]);
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            for (int i = 0; i < 4; i++) ((uint16_t*)out)[i] = float_to_half(s[i]);
            break;
        case VK_FORMAT_R32_SFLOAT:
            ((float*)out)[0] = s[0];
            break;
        case VK_FORMAT_R32G32_SFLOAT:
            for (int i = 0; i < 2; i++) ((float*)out)[i] = s[i];
            break;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            for (int i = 0; i < 4; i++) ((float*)out)[i] = s[i];
            break;
        case VK_FORMAT_D32_SFLOAT:
            ((float*)out)[0] = s[0];
            break;
        case VK_FORMAT_D16_UNORM:
            ((uint16_t*)out)[0] = (uint16_t)to_unorm(s[0], 16);
            break;
        default:
            memset(out, 0, fi->block_bytes);
            break;
        }
    }

    static void decode_storage(const FormatInfo* fi, const uint8_t* p, float s[4]) {
        s[0] = s[1] = s[2] = 0.0f;
        s[3] = 1.0f;
        switch (fi->vk) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            for (int i = 0; i < 4; i++) s[i] = p[i] / 255.0f;
            break;
        case VK_FORMAT_R8_UNORM: s[0] = p[0] / 255.0f; break;
        case VK_FORMAT_R8G8_UNORM: s[0] = p[0] / 255.0f; s[1] = p[1] / 255.0f; break;
        case VK_FORMAT_R16G16B16A16_UNORM:
            for (int i = 0; i < 4; i++) s[i] = ((uint16_t*)p)[i] / 65535.0f;
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
            uint32_t x = *(uint32_t*)p;
            s[0] = unorm(x & 0x3FF, 10); s[1] = unorm((x >> 10) & 0x3FF, 10);
            s[2] = unorm((x >> 20) & 0x3FF, 10); s[3] = unorm(x >> 30, 2);
        } break;
        case VK_FORMAT_R16_SFLOAT: s[0] = half_to_float(((uint16_t*)p)[0]); break;
        case VK_FORMAT_R16G16_SFLOAT:
            for (int i = 0; i < 2; i++) s[i] = half_to_float(((uint16_t*)p)[i]);
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            for (int i = 0; i < 4; i++) s[i] = half_to_float(((uint16_t*)p)[i]);
            break;
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: s[0] = ((float*)p)[0]; break;
        case VK_FORMAT_R32G32_SFLOAT: for (int i = 0; i < 2; i++) s[i] = ((float*)p)[i]; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: for (int i = 0; i < 4; i++) s[i] = ((float*)p)[i]; break;
        case VK_FORMAT_D16_UNORM: s[0] = ((uint16_t*)p)[0] / 65535.0f; break;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
            // 11/11/10 unsigned floats: 5-bit exponent, 6/6/5-bit mantissa
            uint32_t x = *(uint32_t*)p;
            auto uf = [](uint32_t v, uint32_t mbits) {
                uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
                if (!e) return std::ldexp((float)m, -14 - (int)mbits);
                if (e == 31) return m ? NAN : INFINITY;
                return std::ldexp(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
            };
            s[0] = uf(x & 0x7FF, 6); s[1] = uf((x >> 11) & 0x7FF, 6); s[2] = uf(x >> 22, 5);
        } break;
        default:
            break;
        }
    }

    bool convert_upload(const FormatInfo* fi, GLenum format, GLenum type,
        const void* src, int32_t width, int32_t height, int32_t row_length, int32_t alignment,
        std::vector<uint8_t>& out) {
        out.clear();
        if (!fi || fi->compressed || width <= 0 || height <= 0)
            return false;

        size_t px = client_pixel_size(format, type);
        if (!px)
            return false;

        int32_t row_px = row_length > 0 ? row_length : width;
        size_t src_pitch = align_up((size_t)row_px * px, (size_t)std::max(alignment, 1));
        size_t dst_px = fi->block_bytes;
        out.resize((size_t)width * height * dst_px);
        const uint8_t* s = (const uint8_t*)src;

        // Fast paths: identical layout
        bool same = false;
        if (type == GL_UNSIGNED_BYTE) {
            if (fi->vk == VK_FORMAT_R8G8B8A8_UNORM && format == GL_RGBA && fi->swizzle[3] == VK_COMPONENT_SWIZZLE_A)
                same = true;
            else if (fi->vk == VK_FORMAT_R8_UNORM && fi->channels == 1
                && (format == GL_RED || format == GL_LUMINANCE || format == GL_ALPHA))
                same = true;
            else if (fi->vk == VK_FORMAT_R8G8_UNORM && (format == GL_RG || format == GL_LUMINANCE_ALPHA))
                same = true;
        }
        else if (type == GL_HALF_FLOAT) {
            if ((fi->vk == VK_FORMAT_R16G16B16A16_SFLOAT && format == GL_RGBA)
                || (fi->vk == VK_FORMAT_R16G16_SFLOAT && (format == GL_RG || format == GL_LUMINANCE_ALPHA))
                || (fi->vk == VK_FORMAT_R16_SFLOAT && format == GL_RED))
                same = true;
        }
        else if (type == GL_FLOAT) {
            if ((fi->vk == VK_FORMAT_R32G32B32A32_SFLOAT && format == GL_RGBA)
                || (fi->vk == VK_FORMAT_R32G32_SFLOAT && format == GL_RG)
                || ((fi->vk == VK_FORMAT_R32_SFLOAT || fi->vk == VK_FORMAT_D32_SFLOAT)
                    && (format == GL_RED || format == GL_DEPTH_COMPONENT)))
                same = true;
        }

        if (same && px == dst_px) {
            for (int32_t y = 0; y < height; y++)
                memcpy(&out[(size_t)y * width * dst_px], s + y * src_pitch, (size_t)width * dst_px);
            return true;
        }

        // RGB8 -> RGBA8 is common enough to deserve its own path
        if (type == GL_UNSIGNED_BYTE && fi->vk == VK_FORMAT_R8G8B8A8_UNORM && (format == GL_RGB || format == GL_BGR)) {
            const bool bgr = format == GL_BGR;
            for (int32_t y = 0; y < height; y++) {
                const uint8_t* sr = s + y * src_pitch;
                uint8_t* dr = &out[(size_t)y * width * 4];
                for (int32_t x = 0; x < width; x++, sr += 3, dr += 4) {
                    dr[0] = bgr ? sr[2] : sr[0];
                    dr[1] = sr[1];
                    dr[2] = bgr ? sr[0] : sr[2];
                    dr[3] = 255;
                }
            }
            return true;
        }

        // Generic path
        for (int32_t y = 0; y < height; y++) {
            const uint8_t* sr = s + y * src_pitch;
            uint8_t* dr = &out[(size_t)y * width * dst_px];
            for (int32_t x = 0; x < width; x++, sr += px, dr += dst_px) {
                float c[4];
                if (!decode_client(format, type, sr, c)) {
                    out.clear();
                    return false;
                }
                encode_storage(fi, c, dr);
            }
        }
        return true;
    }

    static float apply_swizzle(VkComponentSwizzle sw, const float s[4], int self) {
        switch (sw) {
        case VK_COMPONENT_SWIZZLE_IDENTITY: return s[self];
        case VK_COMPONENT_SWIZZLE_ZERO: return 0.0f;
        case VK_COMPONENT_SWIZZLE_ONE: return 1.0f;
        case VK_COMPONENT_SWIZZLE_R: return s[0];
        case VK_COMPONENT_SWIZZLE_G: return s[1];
        case VK_COMPONENT_SWIZZLE_B: return s[2];
        case VK_COMPONENT_SWIZZLE_A: return s[3];
        default: return 0.0f;
        }
    }

    bool convert_readback(const FormatInfo* fi, const void* src, int32_t width, int32_t height,
        GLenum format, GLenum type, void* dst, int32_t alignment) {
        if (!fi || fi->compressed)
            return false;

        uint32_t n = gl_format_components(format);
        size_t comp = gl_type_size(type);
        if (!n || !comp)
            return false;
        size_t px = n * comp;
        size_t dst_pitch = align_up((size_t)width * px, (size_t)std::max(alignment, 1));

        const uint8_t* s = (const uint8_t*)src;
        uint8_t* d = (uint8_t*)dst;
        for (int32_t y = 0; y < height; y++) {
            uint8_t* dr = d + y * dst_pitch;
            for (int32_t x = 0; x < width; x++) {
                float st[4], c[4];
                decode_storage(fi, s + ((size_t)y * width + x) * fi->block_bytes, st);
                for (int i = 0; i < 4; i++)
                    c[i] = fi->depth ? st[0] : apply_swizzle(fi->swizzle[i], st, i);

                float o[4];
                switch (format) {
                case GL_RED: case GL_DEPTH_COMPONENT: case GL_LUMINANCE: o[0] = c[0]; break;
                case GL_GREEN: o[0] = c[1]; break;
                case GL_BLUE: o[0] = c[2]; break;
                case GL_ALPHA: o[0] = c[3]; break;
                case GL_RG: o[0] = c[0]; o[1] = c[1]; break;
                case GL_LUMINANCE_ALPHA: o[0] = c[0]; o[1] = c[3]; break;
                case GL_RGB: o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; break;
                case GL_BGR: o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; break;
                case GL_RGBA: memcpy(o, c, sizeof(o)); break;
                case GL_BGRA: o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = c[3]; break;
                default: return false;
                }

                for (uint32_t i = 0; i < n; i++, dr += comp) {
                    switch (type) {
                    case GL_UNSIGNED_BYTE: *dr = (uint8_t)to_unorm(o[i], 8); break;
                    case GL_UNSIGNED_SHORT: *(uint16_t*)dr = (uint16_t)to_unorm(o[i], 16); break;
                    case GL_UNSIGNED_INT: {
                        float f = std::min(std::max(o[i], 0.0f), 1.0f);
                        *(uint32_t*)dr = (uint32_t)((double)f * 4294967295.0);
                    } break;
                    case GL_HALF_FLOAT: *(uint16_t*)dr = float_to_half(o[i]); break;
                    case GL_FLOAT: *(float*)dr = o[i]; break;
                    default: return false;
                    }
                }
            }
        }
        return true;
    }

    VkComponentSwizzle gl_swizzle_to_vk(GLint s) {
        switch (s) {
        case GL_RED: return VK_COMPONENT_SWIZZLE_R;
        case GL_GREEN: return VK_COMPONENT_SWIZZLE_G;
        case GL_BLUE: return VK_COMPONENT_SWIZZLE_B;
        case GL_ALPHA: return VK_COMPONENT_SWIZZLE_A;
        case GL_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
        case GL_ONE: return VK_COMPONENT_SWIZZLE_ONE;
        default: return VK_COMPONENT_SWIZZLE_IDENTITY;
        }
    }
}
