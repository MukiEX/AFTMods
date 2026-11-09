// The in-game overlay ABI for the launcher. Launcher.dva can run as an in-game menu instead of
// a window before the game ([launcher] overlay=1). It draws the menu on the CPU and hands
// DivaLinux the picture through DivaLinux_SetOverlay; once per frame, just before the frame is
// presented, DivaLinux asks for the current picture and blends it over the frame. In
// side-by-side stereo, a copy goes into each eye's half so the menu sits on the screen plane.
//
// The ABI is plain C so the launcher, built with MinGW, and DivaLinux, built with MSVC, can
// share it; LauncherFS/src/overlay_abi.hpp mirrors this declaration.

#include "overlay.hpp"
#include <stddef.h>

#define DIVALINUX_OVERLAY_VERSION 3

extern "C" {
    typedef struct DivaLinuxOverlayFrame {
        // Filled in by DivaLinux:
        uint32_t version;        // DIVALINUX_OVERLAY_VERSION
        HWND hwnd;               // the game window
        int32_t width, height;   // the size to lay the menu out for, in pixels
        int32_t sbs;             // 0 = flat, 1 = half side-by-side, 2 = full side-by-side
        int32_t window_width, window_height; // the window's client area (for mouse positions)
        // Filled in by the launcher (pixels stays NULL when the menu is closed):
        const uint32_t* pixels;  // 0xAARRGGBB, top row first, not premultiplied
        int32_t image_width, image_height, image_stride; // stride in pixels
        uint32_t serial;         // changes whenever the pixels do
        // Version 2, filled in by DivaLinux:
        int32_t cursor_drawn;    // DivaLinux draws the mouse pointer: leave the Windows cursor hidden
        // Version 3: where the game sees the mouse, in the menu's layout pixels:
        //   layout = (window position - mouse_offset) * mouse_scale
        float mouse_offset_x, mouse_offset_y;
        float mouse_scale_x, mouse_scale_y;
        int32_t reserved;
    } DivaLinuxOverlayFrame;

    typedef void(__cdecl* DivaLinuxOverlayFn)(DivaLinuxOverlayFrame* frame);
}

// Same layout on both sides (MinGW launcher, MSVC DivaLinux); x64.
static_assert(sizeof(void*) != 8 || (offsetof(DivaLinuxOverlayFrame, hwnd) == 8 && offsetof(DivaLinuxOverlayFrame, sbs) == 24
    && offsetof(DivaLinuxOverlayFrame, pixels) == 40 && offsetof(DivaLinuxOverlayFrame, serial) == 60
    && offsetof(DivaLinuxOverlayFrame, cursor_drawn) == 64 && offsetof(DivaLinuxOverlayFrame, mouse_scale_y) == 80
    && sizeof(DivaLinuxOverlayFrame) == 88),
    "DivaLinuxOverlayFrame layout changed");

static DivaLinuxOverlayFn g_overlay_fn = nullptr;
static bool g_failed_logged = false;

// Called by the launcher, from its divaMain hook (before the game creates its window).
// Returns the ABI version, so the launcher knows its frames will be drawn.
extern "C" __declspec(dllexport) int32_t __cdecl DivaLinux_SetOverlay(DivaLinuxOverlayFn fn) {
    g_overlay_fn = fn;
    overlay::log("In-game overlay %s by the launcher", fn ? "registered" : "removed");
    return DIVALINUX_OVERLAY_VERSION;
}

namespace overlay {
    bool launcher_frame(HWND hwnd, int32_t sbs, int32_t win_w, int32_t win_h, const Layout& L) {
        DivaLinuxOverlayFn fn = g_overlay_fn;
        if (!fn)
            return false;
        DivaLinuxOverlayFrame f = {};
        f.version = DIVALINUX_OVERLAY_VERSION;
        f.hwnd = hwnd;
        f.sbs = sbs;
        f.width = L.lay_w;
        f.height = L.lay_h;
        f.window_width = win_w;
        f.window_height = win_h;
        f.cursor_drawn = cursor_drawn() ? 1 : 0;
        f.mouse_offset_x = L.mouse_ox;
        f.mouse_offset_y = L.mouse_oy;
        f.mouse_scale_x = L.mouse_sx;
        f.mouse_scale_y = L.mouse_sy;
        fn(&f);
        if (f.pixels && f.image_width > 0 && f.image_height > 0) {
            OverlayGL::Image img = { f.pixels, f.image_width, f.image_height,
                f.image_stride > 0 ? f.image_stride : f.image_width, f.serial };
            if (!compositor().Draw(img, L.rects, L.n) && !g_failed_logged) {
                g_failed_logged = true;
                log("In-game overlay can't be drawn: %s", compositor().error.c_str());
            }
        }
        return true;
    }
}
