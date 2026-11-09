// DivaLinux's in-game overlay: what its parts share (overlay/*.cpp).
//
//   compositor.cpp    once per frame, right before present: the launcher's in-game menu or
//                     DivaOverlay, then the mouse pointer, blended over the frame
//   launcher_api.cpp  DivaLinux_SetOverlay, the C ABI Launcher.dva registers its menu through
//   layout.cpp        where the overlay goes (flat, or each eye's half in side-by-side 3D) and
//                     where the game sees the mouse
//   cursor.cpp        the mouse pointer drawn into the picture ([cursor] in DivaLinux.ini)
//   overlay_gl.*      the GL drawing (images, the pointer, ImGui meshes)
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include "overlay_gl.hpp"

namespace overlay {
    struct Layout {
        OverlayGL::Rect rects[2]; // where the picture goes, in GL's coordinates (origin bottom left)
        int n = 1;
        int32_t lay_w = 0, lay_h = 0;  // the picture's own size, to lay the menu out for
        // window position -> picture position (where the game sees the mouse)
        float mouse_ox = 0, mouse_oy = 0, mouse_sx = 1, mouse_sy = 1;
    };

    // fb: the framebuffer; mode: 0 flat, 1 half SBS, 2 full SBS; win: the window's client area.
    Layout make_layout(int32_t fb_w, int32_t fb_h, int32_t mode, int32_t win_w, int32_t win_h);

    // The one compositor (render thread).
    OverlayGL::Compositor& compositor();
    // A line to DivaLinux.log with one string argument.
    void log(const char* fmt, const char* arg);

    // The pointer: hooks the window once, then draws each frame (cursor.cpp).
    bool cursor_drawn();
    void cursor_attach(HWND hwnd);
    void cursor_frame(HWND hwnd, const Layout& L);

    // The launcher's in-game menu draws this frame (launcher_api.cpp). False when it isn't
    // registered; then DivaOverlay gets the frame.
    bool launcher_frame(HWND hwnd, int32_t sbs, int32_t win_w, int32_t win_h, const Layout& L);
}
