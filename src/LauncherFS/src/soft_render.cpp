// CPU renderer for Dear ImGui draw data (see soft_render.hpp).
//
// ImGui only draws triangles with per-vertex colours and one texture (the font atlas, whose
// white pixel stands in for "no texture"). Most of the launcher is solid shapes, so triangles
// whose texture coordinates are all the same skip texture sampling, and solid-colour ones
// that cover whole pixels go through a plain fill.
#include "soft_render.hpp"

#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace lfs {
    void SoftCanvas::resize(int width, int height) {
        if (width == w && height == h)
            return;
        w = std::max(width, 0);
        h = std::max(height, 0);
        px.assign((size_t)w * h, 0xFF000000u);
    }

    void SoftCanvas::clear(uint32_t argb) {
        std::fill(px.begin(), px.end(), argb);
    }

    void soft_update_textures(ImDrawData* dd) {
        if (!dd || !dd->Textures)
            return;
        for (ImTextureData* t : *dd->Textures) {
            if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
                t->SetTexID((ImTextureID)(intptr_t)t);
                t->SetStatus(ImTextureStatus_OK);
            }
            else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    namespace {
        struct Rgba {
            float r, g, b, a;
        };

        inline Rgba unpack(ImU32 c) {
            const float k = 1.0f / 255.0f;
            return { (c & 0xFF) * k, ((c >> 8) & 0xFF) * k, ((c >> 16) & 0xFF) * k, ((c >> 24) & 0xFF) * k };
        }

        // Nearest texel; ImGui places glyphs on whole pixels, so this matches the GPU closely.
        inline Rgba texel(const ImTextureData* t, float u, float v) {
            if (!t)
                return { 1, 1, 1, 1 };
            int x = (int)(u * t->Width);
            int y = (int)(v * t->Height);
            x = x < 0 ? 0 : x >= t->Width ? t->Width - 1 : x;
            y = y < 0 ? 0 : y >= t->Height ? t->Height - 1 : y;
            const unsigned char* p = (const unsigned char*)const_cast<ImTextureData*>(t)->GetPixelsAt(x, y);
            const float k = 1.0f / 255.0f;
            if (t->Format == ImTextureFormat_Alpha8)
                return { 1, 1, 1, p[0] * k };
            return { p[0] * k, p[1] * k, p[2] * k, p[3] * k };
        }

        // "Over" with the destination's alpha kept (SoftCanvas::keep_alpha).
        inline void blend_alpha(uint32_t& d, float r, float g, float b, float a) {
            if (a <= 0.002f)
                return;
            const float k = 1.0f / 255.0f;
            const float da = (d >> 24) * k;
            const float oa = a + da * (1.0f - a);
            if (oa <= 0.0f)
                return;
            const float wd = da * (1.0f - a) / oa, ws = a / oa;
            float dr = ((d >> 16) & 0xFF) * k, dg = ((d >> 8) & 0xFF) * k, db = (d & 0xFF) * k;
            d = ((uint32_t)(oa * 255.0f + 0.5f) << 24) | ((uint32_t)((r * ws + dr * wd) * 255.0f + 0.5f) << 16)
                | ((uint32_t)((g * ws + dg * wd) * 255.0f + 0.5f) << 8) | (uint32_t)((b * ws + db * wd) * 255.0f + 0.5f);
        }

        inline void blend(uint32_t& d, float r, float g, float b, float a) {
            if (a <= 0.002f)
                return;
            if (a >= 0.998f) {
                d = 0xFF000000u | ((uint32_t)(r * 255.0f + 0.5f) << 16) | ((uint32_t)(g * 255.0f + 0.5f) << 8)
                    | (uint32_t)(b * 255.0f + 0.5f);
                return;
            }
            const float k = 1.0f / 255.0f;
            float dr = ((d >> 16) & 0xFF) * k, dg = ((d >> 8) & 0xFF) * k, db = (d & 0xFF) * k;
            dr += (r - dr) * a;
            dg += (g - dg) * a;
            db += (b - db) * a;
            d = 0xFF000000u | ((uint32_t)(dr * 255.0f + 0.5f) << 16) | ((uint32_t)(dg * 255.0f + 0.5f) << 8)
                | (uint32_t)(db * 255.0f + 0.5f);
        }

        template <bool KeepAlpha>
        inline void put(uint32_t& d, float r, float g, float b, float a) {
            if (KeepAlpha) blend_alpha(d, r, g, b, a);
            else blend(d, r, g, b, a);
        }

        template <bool KeepAlpha>
        void triangle(SoftCanvas& cv, const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c,
                      const ImTextureData* tex, const ImVec4& clip) {
            float minx = std::max({ clip.x, 0.0f, std::floor(std::min({ a.pos.x, b.pos.x, c.pos.x })) });
            float maxx = std::min({ clip.z, (float)cv.w, std::ceil(std::max({ a.pos.x, b.pos.x, c.pos.x })) });
            float miny = std::max({ clip.y, 0.0f, std::floor(std::min({ a.pos.y, b.pos.y, c.pos.y })) });
            float maxy = std::min({ clip.w, (float)cv.h, std::ceil(std::max({ a.pos.y, b.pos.y, c.pos.y })) });
            if (minx >= maxx || miny >= maxy)
                return;
            float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
            if (std::fabs(area) < 1e-8f)
                return;
            const float inv = 1.0f / area;

            const bool same_uv = a.uv.x == b.uv.x && a.uv.x == c.uv.x && a.uv.y == b.uv.y && a.uv.y == c.uv.y;
            const bool same_col = a.col == b.col && a.col == c.col;
            const Rgba ca = unpack(a.col), cb = unpack(b.col), cc = unpack(c.col);
            Rgba solid = { 1, 1, 1, 1 };
            if (same_uv) {
                Rgba t = texel(tex, a.uv.x, a.uv.y);
                solid = { ca.r * t.r, ca.g * t.g, ca.b * t.b, ca.a * t.a };
            }

            // Edge functions, stepped per pixel: w0 for edge b-c, w1 for c-a (w2 = 1 - w0 - w1).
            const float e0x = (b.pos.y - c.pos.y) * inv;
            const float e1x = (c.pos.y - a.pos.y) * inv;
            const int x0 = (int)minx, x1 = (int)maxx, y0 = (int)miny, y1 = (int)maxy;
            const float eps = -1e-5f;
            for (int y = y0; y < y1; y++) {
                const float py = y + 0.5f, px0 = x0 + 0.5f;
                float w0 = ((b.pos.x - px0) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px0)) * inv;
                float w1 = ((c.pos.x - px0) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px0)) * inv;
                uint32_t* row = &cv.px[(size_t)y * cv.w];
                for (int x = x0; x < x1; x++, w0 += e0x, w1 += e1x) {
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < eps || w1 < eps || w2 < eps)
                        continue;
                    if (same_uv && same_col) {
                        put<KeepAlpha>(row[x], solid.r, solid.g, solid.b, solid.a);
                        continue;
                    }
                    Rgba k;
                    if (same_col)
                        k = ca;
                    else
                        k = { w0 * ca.r + w1 * cb.r + w2 * cc.r, w0 * ca.g + w1 * cb.g + w2 * cc.g,
                              w0 * ca.b + w1 * cb.b + w2 * cc.b, w0 * ca.a + w1 * cb.a + w2 * cc.a };
                    if (same_uv) {
                        Rgba t = texel(tex, a.uv.x, a.uv.y);
                        put<KeepAlpha>(row[x], k.r * t.r, k.g * t.g, k.b * t.b, k.a * t.a);
                    }
                    else {
                        Rgba t = texel(tex, w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y);
                        put<KeepAlpha>(row[x], k.r * t.r, k.g * t.g, k.b * t.b, k.a * t.a);
                    }
                }
            }
        }
    }

    void soft_render(SoftCanvas& cv, ImDrawData* dd) {
        if (!dd || cv.w <= 0 || cv.h <= 0)
            return;
        const ImVec2 off = dd->DisplayPos;
        for (ImDrawList* dl : dd->CmdLists) {
            for (const ImDrawCmd& cmd : dl->CmdBuffer) {
                if (cmd.UserCallback) {
                    if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                        cmd.UserCallback(dl, &cmd);
                    continue;
                }
                const ImTextureData* tex = cmd.TexRef._TexData;
                if (!tex && cmd.TexRef._TexID != ImTextureID_Invalid)
                    tex = (const ImTextureData*)(intptr_t)cmd.TexRef._TexID;
                const ImVec4 clip(cmd.ClipRect.x - off.x, cmd.ClipRect.y - off.y, cmd.ClipRect.z - off.x, cmd.ClipRect.w - off.y);
                const ImDrawVert* v = dl->VtxBuffer.Data + cmd.VtxOffset;
                const ImDrawIdx* idx = dl->IdxBuffer.Data + cmd.IdxOffset;
                for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                    if (cv.keep_alpha)
                        triangle<true>(cv, v[idx[i]], v[idx[i + 1]], v[idx[i + 2]], tex, clip);
                    else
                        triangle<false>(cv, v[idx[i]], v[idx[i + 1]], v[idx[i + 2]], tex, clip);
                }
            }
        }
    }
}
