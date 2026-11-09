// Once per frame, right before present (the WGL bridge calls divalinux_overlay_before_swap):
// the launcher's in-game menu when it's registered, otherwise DivaOverlay, the settings menu;
// then the mouse pointer over both.

#include "overlay.hpp"
#include <stdio.h>
#include <divavk/divavk.h>
#include "../../DivaGL/static_var.hpp"

extern "C" void divavk_log_text(const char* text); // DivaLinux.log (vk_bridge.cpp)
// DivaOverlay (src/DivaOverlay/ov_host.cpp): the settings menu, when the launcher's in-game
// menu isn't registered. While it's open it draws itself into the rects on the GPU (1), or
// hands back its picture for us to blend (2, its CPU fallback).
extern "C" int divaoverlay_frame(HWND hwnd, int32_t lay_w, int32_t lay_h,
    float mouse_ox, float mouse_oy, float mouse_sx, float mouse_sy,
    const OverlayGL::Rect* rects, int rect_count,
    const uint32_t** pixels, int32_t* width, int32_t* height, uint32_t* serial);

namespace overlay {
    static OverlayGL::Compositor g_compositor;
    static bool g_failed_logged = false;

    OverlayGL::Compositor& compositor() { return g_compositor; }

    void log(const char* fmt, const char* arg) {
        char buf[512];
        snprintf(buf, sizeof(buf), fmt, arg);
        divavk_log_text(buf);
    }
}

using namespace overlay;

// Called by the WGL bridge right before each present.
extern "C" void divalinux_overlay_before_swap(HWND hwnd) {
    if (g_compositor.Failed())
        return;
    int32_t fb_w = 0, fb_h = 0;
    dvk_get_framebuffer_size(&fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0)
        return;

    int32_t win_w = 0, win_h = 0;
    RECT rc;
    if (hwnd && GetClientRect(hwnd, &rc)) {
        win_w = rc.right - rc.left;
        win_h = rc.bottom - rc.top;
    }
    const int32_t sbs = (sv_stereo_mode == 1 || sv_stereo_mode == 2) ? sv_stereo_mode : 0;
    const Layout L = make_layout(fb_w, fb_h, sbs, win_w, win_h);

    // The pointer's window hook goes in before the launcher's, so the launcher's sits on top
    // and passes the cursor through to it.
    cursor_attach(hwnd);
    if (!launcher_frame(hwnd, sbs, win_w, win_h, L)) {
        const uint32_t* px = nullptr;
        int32_t w = 0, h = 0;
        uint32_t serial = 0;
        if (divaoverlay_frame(hwnd, L.lay_w, L.lay_h, L.mouse_ox, L.mouse_oy, L.mouse_sx, L.mouse_sy,
                L.rects, L.n, &px, &w, &h, &serial) == 2) {
            OverlayGL::Image img = { px, w, h, w, serial };
            if (!g_compositor.Draw(img, L.rects, L.n) && !g_failed_logged) {
                g_failed_logged = true;
                log("Settings menu can't be drawn: %s", g_compositor.error.c_str());
            }
        }
    }
    cursor_frame(hwnd, L); // over the menu too
}
