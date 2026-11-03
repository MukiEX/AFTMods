/*
    DivaVK - OpenGL subset on Vulkan for PD Loader / DivaGL
    Shared internal header.
*/

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <functional>
#include <algorithm>

#include <volk.h>
#include <vk_mem_alloc.h>

#include <GL/glcorearb.h>

// Enums that are not part of the core profile header but are used by DivaGL / the game.
#ifndef GL_ALPHA8
#define GL_ALPHA8                           0x803C
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE                        0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA                  0x190A
#endif
#ifndef GL_LUMINANCE8
#define GL_LUMINANCE8                       0x8040
#endif
#ifndef GL_LUMINANCE8_ALPHA8
#define GL_LUMINANCE8_ALPHA8                0x8045
#endif
#ifndef GL_INTENSITY8
#define GL_INTENSITY8                       0x804B
#endif
#ifndef GL_LUMINANCE_ALPHA16F_ARB
#define GL_LUMINANCE_ALPHA16F_ARB           0x881F
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT     0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT    0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT    0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT    0x83F3
#endif
#ifndef GL_CLAMP
#define GL_CLAMP                            0x2900
#endif
#ifndef GL_QUADS
#define GL_QUADS                            0x0007
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY           0x84FE
#endif

#include <atomic>

namespace dvk {
    // What DivaVK is doing right now, for the plugin's watchdog (dvk_debug_status): a string
    // literal naming the step, set around calls that can take long or block.
    extern std::atomic<const char*> g_stage;
#define DVK_STAGE(s) (::dvk::g_stage.store((s), std::memory_order_relaxed))

    void log(const char* fmt, ...);
    void log_once(const char* key, const char* fmt, ...);

    inline void hash_combine(uint64_t& seed, uint64_t v) {
        seed ^= v + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
    }

    inline uint64_t hash_bytes(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < size; i++) {
            seed ^= p[i];
            seed *= 0x100000001B3ull;
        }
        return seed;
    }

    template <typename T>
    inline T align_up(T v, T a) {
        return (v + a - 1) / a * a;
    }

#define DVK_CHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) \
    ::dvk::log("Vulkan error %d at %s:%d: %s", (int)_r, __FILE__, __LINE__, #expr); } while (0)
}
