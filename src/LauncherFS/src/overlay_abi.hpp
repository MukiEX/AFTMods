// The in-game overlay interface DivaLinux.dva exports (src/DivaLinux/overlay.cpp in AFTMods).
//
// Plain C, so this MinGW-built launcher and the MSVC-built DivaLinux agree on it. The launcher
// registers a callback with DivaLinux_SetOverlay; DivaLinux calls it on the game's render
// thread right before each frame is presented, and blends the picture the callback returns
// over the frame (one copy per eye in side-by-side stereo).
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define DIVALINUX_OVERLAY_VERSION 1

extern "C" {
    struct DivaLinuxOverlayFrame {
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
    };

    typedef void(__cdecl* DivaLinuxOverlayFn)(DivaLinuxOverlayFrame* frame);
    typedef int32_t(__cdecl* PFN_DivaLinux_SetOverlay)(DivaLinuxOverlayFn fn);
}

// Same layout on both sides (MinGW launcher, MSVC DivaLinux); x64.
static_assert(sizeof(void*) != 8 || (offsetof(DivaLinuxOverlayFrame, hwnd) == 8 && offsetof(DivaLinuxOverlayFrame, sbs) == 24
    && offsetof(DivaLinuxOverlayFrame, pixels) == 40 && offsetof(DivaLinuxOverlayFrame, serial) == 60
    && sizeof(DivaLinuxOverlayFrame) == 64), "DivaLinuxOverlayFrame layout changed");
