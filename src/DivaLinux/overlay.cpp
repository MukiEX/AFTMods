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
// Also here: the mouse pointer ([cursor] pointer, a leek by default, or a crosshair). The
// Windows cursor is hidden over the game window and the pointer is drawn into the frame
// instead, where the game itself sees the mouse. In side-by-side 3D the game reads the mouse
// against its 16:9 picture's area of the window, while each eye shows that picture in its own
// half, so the real cursor is in the wrong place for both eyes; the drawn pointer is in the
// right place in both, on the screen plane. It pulses gently and fades out when the mouse is
// left alone.

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

extern "C" void divavk_log_text(const char* text); // DivaLinux.log (vk_bridge.cpp)
// DivaOverlay (src/DivaOverlay/ov_host.cpp): the settings menu, when the launcher's in-game
// menu isn't registered. While it's open it draws itself into the rects on the GPU (1), or
// hands back its picture for us to blend (2, its CPU fallback).
extern "C" int divaoverlay_frame(HWND hwnd, int32_t lay_w, int32_t lay_h,
    float mouse_ox, float mouse_oy, float mouse_sx, float mouse_sy,
    const OverlayGL::Rect* rects, int rect_count,
    const uint32_t** pixels, int32_t* width, int32_t* height, uint32_t* serial);

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

// The game's own picture inside a w x h screen: 16:9 (stereo_content_aspect), centred, with
// bars on screens of any other shape. Stereo takes just this part into each eye
// (stereo_eye_end), and the game reads the mouse against it.
static void content_rect(int32_t w, int32_t h, int32_t& x, int32_t& y, int32_t& cw, int32_t& ch) {
    x = 0; y = 0; cw = w; ch = h;
    if (sv_stereo_content_aspect <= 0 || w <= 0 || h <= 0)
        return;
    const double aspect = sv_stereo_content_aspect / 1000.0;
    if ((double)w / h > aspect + 0.01) {
        cw = (int32_t)(h * aspect + 0.5);
        x = (w - cw) / 2;
    }
    else if ((double)w / h < aspect - 0.01) {
        ch = (int32_t)(w / aspect + 0.5);
        y = (h - ch) / 2;
    }
}

struct Layout {
    OverlayGL::Rect rects[2]; // where the picture goes, in GL's coordinates (origin bottom left)
    int n = 1;
    int32_t lay_w = 0, lay_h = 0;  // the picture's own size, to lay the menu out for
    // window position -> picture position (where the game sees the mouse)
    float mouse_ox = 0, mouse_oy = 0, mouse_sx = 1, mouse_sy = 1;
};

// Flat: the menu covers the whole screen, and the mouse is where it is. Side by side: each
// eye shows the game's picture (content_rect) the way stereo_eye_end puts it there (squeezed
// into its half in half SBS, fitted with its shape in full SBS); the menu is laid out for
// that picture, and the mouse maps from the same picture's area of the window, which is how
// the game reads it.
static Layout make_layout(int32_t fb_w, int32_t fb_h, int32_t mode, int32_t win_w, int32_t win_h) {
    Layout L;
    if (win_w <= 0 || win_h <= 0) {
        win_w = fb_w;
        win_h = fb_h;
    }
    if (mode != 1 && mode != 2) {
        L.rects[0] = { 0, 0, fb_w, fb_h };
        L.lay_w = fb_w;
        L.lay_h = fb_h;
        L.mouse_sx = (float)fb_w / win_w;
        L.mouse_sy = (float)fb_h / win_h;
        return L;
    }
    int32_t sx, sy, sw, sh;
    content_rect(fb_w, fb_h, sx, sy, sw, sh);
    L.lay_w = sw;
    L.lay_h = sh;
    const int32_t half = fb_w / 2;
    int32_t dx = 0, dy = 0, dw = half, dh = fb_h;
    if (mode == 2) {
        const double src_aspect = (double)sw / sh;
        if ((double)half / fb_h > src_aspect) {
            dw = (int32_t)(fb_h * src_aspect + 0.5);
            dx = (half - dw) / 2;
        }
        else {
            dh = (int32_t)(half / src_aspect + 0.5);
            dy = (fb_h - dh) / 2;
        }
    }
    L.rects[0] = { dx, dy, dw, dh };
    L.rects[1] = { fb_w - half + dx, dy, dw, dh };
    L.n = 2;
    int32_t wx, wy, ww, wh;
    content_rect(win_w, win_h, wx, wy, ww, wh);
    L.mouse_ox = (float)wx;
    L.mouse_oy = (float)wy;
    L.mouse_sx = (float)sw / ww;
    L.mouse_sy = (float)sh / wh;
    return L;
}

// ---------------------------------------------------------------- mouse pointer

static bool g_cursor_on = false;          // DivaLinux draws the pointer (Windows cursor hidden)
static int g_pointer_kind = OverlayGL::POINTER_LEEK;
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

// The Windows cursor stays hidden over the game's window (the drawn pointer replaces it).
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

// [cursor] pointer: 0 = the Windows cursor, 1 = leek (default), 2 = crosshair
extern "C" void divalinux_cursor_install(const wchar_t* config_file) {
    int pointer = (int)GetPrivateProfileIntW(L"cursor", L"pointer", 1, config_file);
    g_cursor_on = pointer == 1 || pointer == 2;
    g_pointer_kind = pointer == 2 ? OverlayGL::POINTER_CROSSHAIR : OverlayGL::POINTER_LEEK;
    g_cursor_hide_after = (float)GetPrivateProfileIntW(L"cursor", L"hide_after", 2, config_file);
    int size = (int)GetPrivateProfileIntW(L"cursor", L"size", 100, config_file);
    g_cursor_size = (size < 25 ? 25 : size > 400 ? 400 : size) / 100.0f;
    wchar_t col[16] = {};
    GetPrivateProfileStringW(L"cursor", L"color", L"39C5BB", col, 16, config_file);
    const wchar_t* hex = col[0] == L'#' ? col + 1 : col;
    if (wcslen(hex) >= 6) {
        unsigned long rgb = wcstoul(hex, nullptr, 16);
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
    divavk_log_text(g_pointer_kind == OverlayGL::POINTER_LEEK
        ? "Mouse pointer: leek, drawn into the picture ([cursor] pointer=0 brings back the Windows cursor)"
        : "Mouse pointer: crosshair, drawn into the picture ([cursor] pointer=0 brings back the Windows cursor)");
}

// Updates the pointer from the mouse and draws it where the game sees the mouse.
static void cursor_frame(HWND hwnd, const Layout& L) {
    if (!g_cursor_on || !hwnd || g_compositor.Failed())
        return;
    cursor_attach(hwnd);

    const double now = now_seconds();
    const float dt = (float)(g_cs.last_time > 0.0 ? now - g_cs.last_time : 0.0);
    g_cs.last_time = now;

    RECT rc;
    POINT p = {};
    bool inside = false;
    float lx = 0, ly = 0;
    if (GetClientRect(hwnd, &rc) && GetCursorPos(&p) && ScreenToClient(hwnd, &p)) {
        const int32_t ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        lx = (p.x + 0.5f - L.mouse_ox) * L.mouse_sx;
        ly = (p.y + 0.5f - L.mouse_oy) * L.mouse_sy;
        inside = ww > 0 && wh > 0 && p.x >= 0 && p.y >= 0 && p.x < ww && p.y < wh
            && lx >= 0 && ly >= 0 && lx < L.lay_w && ly < L.lay_h  // not in the bars beside the picture
            && GetForegroundWindow() == hwnd;
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
    // (0 = never), and quickly when the mouse leaves the picture.
    const bool active = inside && (g_cursor_hide_after <= 0.0f || now - g_cs.last_active < g_cursor_hide_after);
    if (active)
        g_cs.alpha = g_cs.alpha + (1.0f - g_cs.alpha) * (dt * 14.0f > 1.0f ? 1.0f : dt * 14.0f);
    else
        g_cs.alpha -= dt / (inside ? 0.6f : 0.15f);
    g_cs.alpha = g_cs.alpha < 0.0f ? 0.0f : g_cs.alpha > 1.0f ? 1.0f : g_cs.alpha;
    if (g_cs.alpha <= 0.003f || L.lay_w <= 0 || L.lay_h <= 0)
        return;

    const float pulse = 0.5f + 0.5f * (float)sin(now * 6.2831853 / 1.6);
    const float unit = L.lay_h / 1080.0f * g_cursor_size;  // sizes are for a 1080-line picture
    OverlayGL::Crosshair c = {};
    c.kind = g_pointer_kind;
    c.layWidth = (float)L.lay_w;
    c.layHeight = (float)L.lay_h;
    c.x = lx;
    c.y = ly;
    if (g_pointer_kind == OverlayGL::POINTER_LEEK) {
        c.radius = 46.0f * unit * (0.985f + 0.03f * pulse);
        c.radius = c.radius < 16.0f ? 16.0f : c.radius;
    }
    else {
        c.radius = 18.0f * unit * (0.97f + 0.06f * pulse);
        c.radius = c.radius < 6.0f ? 6.0f : c.radius;
    }
    c.alpha = g_cs.alpha;
    c.glow = 0.16f + 0.16f * pulse;
    const double since_click = now - g_cs.click_time;
    c.ripple = since_click >= 0.0 && since_click < 0.35 ? (float)(since_click / 0.35) : -1.0f;
    c.r = g_cursor_rgb[0];
    c.g = g_cursor_rgb[1];
    c.b = g_cursor_rgb[2];
    if (!g_compositor.DrawCrosshair(c, L.rects, L.n) && !g_cursor_failed_logged) {
        g_cursor_failed_logged = true;
        overlay_log("Mouse pointer can't be drawn: %s", g_compositor.error.c_str());
    }
}

// Called by the WGL bridge right before each present.
extern "C" void divalinux_overlay_before_swap(HWND hwnd) {
    DivaLinuxOverlayFn fn = g_overlay_fn;
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
    if (fn) {
        DivaLinuxOverlayFrame f = {};
        f.version = DIVALINUX_OVERLAY_VERSION;
        f.hwnd = hwnd;
        f.sbs = sbs;
        f.width = L.lay_w;
        f.height = L.lay_h;
        f.window_width = win_w;
        f.window_height = win_h;
        f.cursor_drawn = g_cursor_on ? 1 : 0;
        f.mouse_offset_x = L.mouse_ox;
        f.mouse_offset_y = L.mouse_oy;
        f.mouse_scale_x = L.mouse_sx;
        f.mouse_scale_y = L.mouse_sy;
        fn(&f);
        if (f.pixels && f.image_width > 0 && f.image_height > 0) {
            OverlayGL::Image img = { f.pixels, f.image_width, f.image_height,
                f.image_stride > 0 ? f.image_stride : f.image_width, f.serial };
            if (!g_compositor.Draw(img, L.rects, L.n) && !g_failed_logged) {
                g_failed_logged = true;
                overlay_log("In-game overlay can't be drawn: %s", g_compositor.error.c_str());
            }
        }
    }
    else {
        const uint32_t* px = nullptr;
        int32_t w = 0, h = 0;
        uint32_t serial = 0;
        if (divaoverlay_frame(hwnd, L.lay_w, L.lay_h, L.mouse_ox, L.mouse_oy, L.mouse_sx, L.mouse_sy,
                L.rects, L.n, &px, &w, &h, &serial) == 2) {
            OverlayGL::Image img = { px, w, h, w, serial };
            if (!g_compositor.Draw(img, L.rects, L.n) && !g_failed_logged) {
                g_failed_logged = true;
                overlay_log("Settings menu can't be drawn: %s", g_compositor.error.c_str());
            }
        }
    }
    cursor_frame(hwnd, L); // over the menu too
}
