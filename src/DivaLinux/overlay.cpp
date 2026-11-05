// In-game overlay hook for the launcher (DivaLinux).
//
// Launcher.dva can run as an in-game menu instead of a window before the game ([launcher]
// overlay=1). It draws the menu on the CPU and hands DivaLinux the picture through
// DivaLinux_SetOverlay; once per frame, just before the frame is presented, DivaLinux asks
// for the current picture and blends it over the frame (overlay_gl.h). In side-by-side
// stereo, a copy goes into each eye's half so the menu sits on the screen plane.
//
// The ABI is plain C so the launcher, built with MinGW, and DivaLinux, built with MSVC, can
// share it; LauncherFS/src/overlay_abi.hpp mirrors this declaration.

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <divavk/divavk.h>
#include "overlay_gl.h"
#include "../DivaGL/static_var.hpp"

#define DIVALINUX_OVERLAY_VERSION 1

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
    } DivaLinuxOverlayFrame;

    typedef void(__cdecl* DivaLinuxOverlayFn)(DivaLinuxOverlayFrame* frame);
}

// Same layout on both sides (MinGW launcher, MSVC DivaLinux); x64.
static_assert(sizeof(void*) != 8 || (offsetof(DivaLinuxOverlayFrame, hwnd) == 8 && offsetof(DivaLinuxOverlayFrame, sbs) == 24
    && offsetof(DivaLinuxOverlayFrame, pixels) == 40 && offsetof(DivaLinuxOverlayFrame, serial) == 60
    && sizeof(DivaLinuxOverlayFrame) == 64), "DivaLinuxOverlayFrame layout changed");

extern "C" void divavk_log_text(const char* text); // DivaLinux.log (vk_bridge.cpp)

static void overlay_log(const char* fmt, const char* arg) {
    char buf[512];
    snprintf(buf, sizeof(buf), fmt, arg);
    divavk_log_text(buf);
}

static DivaLinuxOverlayFn g_overlay_fn = nullptr;
static OverlayGL::Compositor g_compositor;
static bool g_failed_logged = false;

// Called by the launcher, from its divaMain hook (before the game creates its window).
// Returns the ABI version, so the launcher knows its frames will be drawn.
extern "C" __declspec(dllexport) int32_t __cdecl DivaLinux_SetOverlay(DivaLinuxOverlayFn fn) {
    g_overlay_fn = fn;
    overlay_log("In-game overlay %s by the launcher", fn ? "registered" : "removed");
    return DIVALINUX_OVERLAY_VERSION;
}

// Where each copy of the menu goes, in GL's coordinates (origin bottom left). Matches how
// stereo_eye_end places each eye's picture: half SBS squeezes it into its half; full SBS keeps
// the picture's shape inside its half.
static int overlay_rects(int32_t fb_w, int32_t fb_h, int32_t mode, OverlayGL::Rect rects[2], int32_t& lay_w, int32_t& lay_h) {
    if (mode != 1 && mode != 2) {
        rects[0] = { 0, 0, fb_w, fb_h };
        lay_w = fb_w;
        lay_h = fb_h;
        return 1;
    }
    const int32_t half = fb_w / 2;
    int32_t w = half, h = fb_h, y = 0, xoff = 0;
    if (mode == 2) {
        double aspect = sv_stereo_content_aspect > 0 ? sv_stereo_content_aspect / 1000.0 : (double)fb_w / fb_h;
        if ((double)half / fb_h > aspect) {
            w = (int32_t)(fb_h * aspect + 0.5);
            xoff = (half - w) / 2;
        }
        else {
            h = (int32_t)(half / aspect + 0.5);
            y = (fb_h - h) / 2;
        }
        lay_w = w;
        lay_h = h;
    }
    else {
        lay_w = fb_w;  // laid out for the whole screen, then squeezed like the game's picture
        lay_h = fb_h;
    }
    rects[0] = { xoff, y, w, h };
    rects[1] = { fb_w - half + xoff, y, w, h };
    return 2;
}

// Called by the WGL bridge right before each present.
extern "C" void divalinux_overlay_before_swap(HWND hwnd) {
    DivaLinuxOverlayFn fn = g_overlay_fn;
    if (!fn || g_compositor.Failed())
        return;
    int32_t fb_w = 0, fb_h = 0;
    dvk_get_framebuffer_size(&fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0)
        return;

    OverlayGL::Rect rects[2];
    DivaLinuxOverlayFrame f = {};
    f.version = DIVALINUX_OVERLAY_VERSION;
    f.hwnd = hwnd;
    f.sbs = (sv_stereo_mode == 1 || sv_stereo_mode == 2) ? sv_stereo_mode : 0;
    int n = overlay_rects(fb_w, fb_h, f.sbs, rects, f.width, f.height);
    RECT rc;
    if (hwnd && GetClientRect(hwnd, &rc)) {
        f.window_width = rc.right - rc.left;
        f.window_height = rc.bottom - rc.top;
    }
    fn(&f);
    if (!f.pixels || f.image_width <= 0 || f.image_height <= 0)
        return;

    OverlayGL::Image img = { f.pixels, f.image_width, f.image_height,
        f.image_stride > 0 ? f.image_stride : f.image_width, f.serial };
    if (!g_compositor.Draw(img, rects, n) && !g_failed_logged) {
        g_failed_logged = true;
        overlay_log("In-game overlay can't be drawn: %s", g_compositor.error.c_str());
    }
}
