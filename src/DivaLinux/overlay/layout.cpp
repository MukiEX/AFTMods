// Where the overlay goes and where the game sees the mouse.

#include "overlay.hpp"
#include "../../DivaGL/static_var.hpp"

namespace overlay {
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

    // Flat: the menu covers the whole screen, and the mouse is where it is. Side by side: each
    // eye shows the game's picture (content_rect) the way stereo_eye_end puts it there (squeezed
    // into its half in half SBS, fitted with its shape in full SBS); the menu is laid out for
    // that picture, and the mouse maps from the same picture's area of the window, which is how
    // the game reads it.
    Layout make_layout(int32_t fb_w, int32_t fb_h, int32_t mode, int32_t win_w, int32_t win_h) {
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
}
