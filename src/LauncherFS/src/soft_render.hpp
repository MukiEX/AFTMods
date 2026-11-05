// CPU renderer for Dear ImGui draw data.
//
// Used where the GPU paths can't be: under WinlatorXR on a Quest, the launcher's Direct3D 11
// (DXVK) or OpenGL (Zink) window leaves images behind in the X server that the game's own
// Vulkan window then collides with (X error BadIDChoice). Drawing on the CPU and handing the
// result to GDI never touches Vulkan, so the game starts cleanly afterwards.
//
// Platform-independent: the Win32 side blits SoftCanvas::px with SetDIBitsToDevice, and the
// Linux preview tool uses it for screenshots.
#pragma once

#include <cstdint>
#include <vector>

struct ImDrawData;

namespace lfs {
    struct SoftCanvas {
        int w = 0, h = 0;
        std::vector<uint32_t> px;   // 0xAARRGGBB (BGRA in memory), top-down
        // Keep real alpha (for an image composited over something else, such as the in-game
        // overlay): drawing uses the "over" operator, so a canvas cleared to transparent ends up
        // with straight (not premultiplied) colour and the coverage in alpha. Off, every drawn
        // pixel is opaque, as on a window.
        bool keep_alpha = false;

        void resize(int width, int height);
        void clear(uint32_t argb);
    };

    // Marks the draw data's textures as ready (the renderer reads their pixels directly).
    void soft_update_textures(ImDrawData* dd);
    // Draws ImGui's draw data into the canvas (call soft_update_textures first).
    void soft_render(SoftCanvas& cv, ImDrawData* dd);
}
