// The mouse pointer ([cursor] pointer, a leek by default, or a crosshair). The Windows cursor is
// hidden over the game window and the pointer is drawn into the frame instead, where the game
// itself sees the mouse. In side-by-side 3D the game reads the mouse against its 16:9
// picture's area of the window, while each eye shows that picture in its own half, so the
// real cursor is in the wrong place for both eyes; the drawn pointer is in the right place in
// both, on the screen plane. It pulses gently and fades out when the mouse is left alone.

#include "overlay.hpp"
#include <math.h>
#include <stdlib.h>
#include <wchar.h>

extern "C" void divavk_log_text(const char* text); // DivaLinux.log (vk_bridge.cpp)

namespace overlay {
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

    void cursor_attach(HWND hwnd) {
        if (!g_cursor_on || !hwnd || g_cursor_hwnd == hwnd)
            return;
        g_cursor_hwnd = hwnd;
        g_cursor_prev_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&cursor_wnd_proc);
        divavk_log_text(g_pointer_kind == OverlayGL::POINTER_LEEK
            ? "Mouse pointer: leek, drawn into the picture ([cursor] pointer=0 brings back the Windows cursor)"
            : "Mouse pointer: crosshair, drawn into the picture ([cursor] pointer=0 brings back the Windows cursor)");
    }

    // Updates the pointer from the mouse and draws it where the game sees the mouse.
    void cursor_frame(HWND hwnd, const Layout& L) {
        if (!g_cursor_on || !hwnd || compositor().Failed())
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
        if (!compositor().DrawCrosshair(c, L.rects, L.n) && !g_cursor_failed_logged) {
            g_cursor_failed_logged = true;
            log("Mouse pointer can't be drawn: %s", compositor().error.c_str());
        }
    }

    bool cursor_drawn() { return g_cursor_on; }
}

using namespace overlay;

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
