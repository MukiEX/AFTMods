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
//
// Also here: the mouse crosshair ([cursor] crosshair=1). The Windows cursor is hidden over the
// game window and a crosshair is drawn into the frame instead, at the point the game itself
// sees the mouse at. In side-by-side 3D the game gets the mouse's position over the whole
// window while its picture is squeezed into each half, so the real cursor is in the wrong place
// for both eyes; the crosshair shows the game's idea of it, in both eyes, on the screen plane.
// It pulses gently and fades out when the mouse is left alone.

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include <wchar.h>
#include <stdlib.h>
#include <divavk/divavk.h>
#include "overlay_gl.h"
#include "../DivaGL/static_var.hpp"

#define DIVALINUX_OVERLAY_VERSION 2

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
        int32_t cursor_drawn;    // DivaLinux draws the mouse crosshair: leave the Windows cursor hidden
        int32_t reserved;
    } DivaLinuxOverlayFrame;

    typedef void(__cdecl* DivaLinuxOverlayFn)(DivaLinuxOverlayFrame* frame);
}

// Same layout on both sides (MinGW launcher, MSVC DivaLinux); x64.
static_assert(sizeof(void*) != 8 || (offsetof(DivaLinuxOverlayFrame, hwnd) == 8 && offsetof(DivaLinuxOverlayFrame, sbs) == 24
    && offsetof(DivaLinuxOverlayFrame, pixels) == 40 && offsetof(DivaLinuxOverlayFrame, serial) == 60
    && offsetof(DivaLinuxOverlayFrame, cursor_drawn) == 64 && sizeof(DivaLinuxOverlayFrame) == 72),
    "DivaLinuxOverlayFrame layout changed");

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

// ---------------------------------------------------------------- mouse crosshair

static bool g_cursor_on = false;
static float g_cursor_hide_after = 2.0f;  // seconds without mouse activity before it fades
static float g_cursor_size = 1.0f;
static float g_cursor_rgb[3] = { 0x39 / 255.0f, 0xC5 / 255.0f, 0xBB / 255.0f };
static HWND g_cursor_hwnd = nullptr;
static WNDPROC g_cursor_prev_proc = nullptr;
static bool g_cursor_failed_logged = false;

struct CursorState {
    double last_active = -1e9;   // seconds; never moved yet = hidden
    double click_time = -1e9;
    double last_time = 0.0;
    POINT last_pos = { -100000, -100000 };
    bool button_was_down = false;
    float alpha = 0.0f;
};
static CursorState g_cs;

static double now_seconds() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// The Windows cursor stays hidden over the game's picture (the crosshair replaces it).
static LRESULT CALLBACK cursor_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && g_cursor_on) {
        SetCursor(nullptr);
        return TRUE;
    }
    LRESULT r = CallWindowProcW(g_cursor_prev_proc, hwnd, msg, wp, lp);
    if (msg == WM_MOUSEMOVE && g_cursor_on)
        SetCursor(nullptr); // in case the game set one of its own while handling it
    return r;
}

extern "C" void divalinux_cursor_install(const wchar_t* config_file) {
    g_cursor_on = GetPrivateProfileIntW(L"cursor", L"crosshair", 1, config_file) > 0;
    g_cursor_hide_after = (float)GetPrivateProfileIntW(L"cursor", L"hide_after", 2, config_file);
    int size = (int)GetPrivateProfileIntW(L"cursor", L"size", 100, config_file);
    g_cursor_size = (size < 25 ? 25 : size > 400 ? 400 : size) / 100.0f;
    wchar_t col[16] = {};
    GetPrivateProfileStringW(L"cursor", L"color", L"39C5BB", col, 16, config_file);
    unsigned long rgb = wcstoul(col[0] == L'#' ? col + 1 : col, nullptr, 16);
    if (wcslen(col) >= 6) {
        g_cursor_rgb[0] = ((rgb >> 16) & 0xFF) / 255.0f;
        g_cursor_rgb[1] = ((rgb >> 8) & 0xFF) / 255.0f;
        g_cursor_rgb[2] = (rgb & 0xFF) / 255.0f;
    }
}

static void cursor_attach(HWND hwnd) {
    if (!g_cursor_on || !hwnd || g_cursor_hwnd == hwnd)
        return;
    g_cursor_hwnd = hwnd;
    g_cursor_prev_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&cursor_wnd_proc);
    divavk_log_text("Mouse crosshair on: the Windows cursor is hidden over the game ([cursor] crosshair=0 turns this off)");
}

// Updates the crosshair from the mouse and draws it. lay_w x lay_h is the picture's size (as
// for the menu), rects where it's shown.
static void cursor_frame(HWND hwnd, const OverlayGL::Rect* rects, int n, int32_t lay_w, int32_t lay_h) {
    if (!g_cursor_on || !hwnd || g_compositor.Failed())
        return;
    cursor_attach(hwnd);

    const double now = now_seconds();
    const float dt = (float)(g_cs.last_time > 0.0 ? now - g_cs.last_time : 0.0);
    g_cs.last_time = now;

    RECT rc;
    POINT p;
    bool inside = false;
    int32_t ww = 0, wh = 0;
    if (GetClientRect(hwnd, &rc) && GetCursorPos(&p) && ScreenToClient(hwnd, &p)) {
        ww = rc.right - rc.left;
        wh = rc.bottom - rc.top;
        inside = ww > 0 && wh > 0 && p.x >= 0 && p.y >= 0 && p.x < ww && p.y < wh && GetForegroundWindow() == hwnd;
    }
    if (inside) {
        if (p.x != g_cs.last_pos.x || p.y != g_cs.last_pos.y) {
            g_cs.last_pos = p;
            g_cs.last_active = now;
        }
        bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (down) {
            g_cs.last_active = now;
            if (!g_cs.button_was_down)
                g_cs.click_time = now;
        }
        g_cs.button_was_down = down;
    }

    // Shown while the mouse is in use; fades out over 0.6 s after hide_after seconds idle
    // (0 = never), and quickly when the mouse leaves the window.
    const bool active = inside && (g_cursor_hide_after <= 0.0f || now - g_cs.last_active < g_cursor_hide_after);
    if (active)
        g_cs.alpha = g_cs.alpha + (1.0f - g_cs.alpha) * (dt * 14.0f > 1.0f ? 1.0f : dt * 14.0f);
    else
        g_cs.alpha -= dt / (inside ? 0.6f : 0.15f);
    g_cs.alpha = g_cs.alpha < 0.0f ? 0.0f : g_cs.alpha > 1.0f ? 1.0f : g_cs.alpha;
    if (g_cs.alpha <= 0.003f || ww <= 0 || wh <= 0)
        return;

    // Where the game sees the mouse: its position across the whole window, mapped onto the
    // picture (in SBS, the same spot in each eye).
    const float pulse = 0.5f + 0.5f * (float)sin(now * 6.2831853 / 1.6);
    OverlayGL::Crosshair c = {};
    c.layWidth = (float)lay_w;
    c.layHeight = (float)lay_h;
    c.x = (p.x + 0.5f) * lay_w / ww;
    c.y = (p.y + 0.5f) * lay_h / wh;
    c.radius = 14.0f * (lay_h / 1080.0f) * g_cursor_size * (0.97f + 0.06f * pulse);
    if (c.radius < 6.0f)
        c.radius = 6.0f;
    c.alpha = g_cs.alpha;
    c.glow = 0.18f + 0.16f * pulse;
    const double since_click = now - g_cs.click_time;
    c.ripple = since_click >= 0.0 && since_click < 0.35 ? (float)(since_click / 0.35) : -1.0f;
    c.r = g_cursor_rgb[0];
    c.g = g_cursor_rgb[1];
    c.b = g_cursor_rgb[2];
    if (!g_compositor.DrawCrosshair(c, rects, n) && !g_cursor_failed_logged) {
        g_cursor_failed_logged = true;
        overlay_log("Mouse crosshair can't be drawn: %s", g_compositor.error.c_str());
    }
}

// Called by the WGL bridge right before each present.
extern "C" void divalinux_overlay_before_swap(HWND hwnd) {
    DivaLinuxOverlayFn fn = g_overlay_fn;
    if ((!fn && !g_cursor_on) || g_compositor.Failed())
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
    f.cursor_drawn = g_cursor_on ? 1 : 0;
    int n = overlay_rects(fb_w, fb_h, f.sbs, rects, f.width, f.height);
    RECT rc;
    if (hwnd && GetClientRect(hwnd, &rc)) {
        f.window_width = rc.right - rc.left;
        f.window_height = rc.bottom - rc.top;
    }
    // The crosshair's window hook goes in before the launcher's, so the launcher's sits on
    // top and passes the cursor through to it.
    cursor_attach(hwnd);
    if (fn) {
        fn(&f);
        if (f.pixels && f.image_width > 0 && f.image_height > 0) {
            OverlayGL::Image img = { f.pixels, f.image_width, f.image_height,
                f.image_stride > 0 ? f.image_stride : f.image_width, f.serial };
            if (!g_compositor.Draw(img, rects, n) && !g_failed_logged) {
                g_failed_logged = true;
                overlay_log("In-game overlay can't be drawn: %s", g_compositor.error.c_str());
            }
        }
    }
    cursor_frame(hwnd, rects, n, f.width, f.height); // over the menu too
}
