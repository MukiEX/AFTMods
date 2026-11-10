#include "ov_ui.hpp"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

namespace dov {
    namespace {
        // ------------------------------------------------ colours
        inline ImU32 rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(std::max(0.0f, std::min(1.0f, a)) * 255.0f + 0.5f)); }
        const int TEAL[3] = { 0x26, 0xC6, 0xD0 };
        const int TEAL_D[3] = { 0x10, 0x8F, 0xA0 };
        const int NAVY[3] = { 0x1B, 0x25, 0x4B };
        const int GREY[3] = { 0x6A, 0x76, 0x88 };
        const int AMBER[3] = { 0xFF, 0xAE, 0x34 };
        const int PANEL[3] = { 0x0B, 0x15, 0x29 };
        inline ImU32 col(const int c[3], float a) { return rgba(c[0], c[1], c[2], a); }

        float ease_out(float t) { t = std::max(0.0f, std::min(1.0f, t)); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
        float approach(float cur, float target, float rate, float dt) {
            const float k = 1.0f - std::exp(-rate * dt);
            float v = cur + (target - cur) * k;
            return std::fabs(target - v) < 0.01f ? target : v;
        }

        struct Rect {
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
        };

        // Key repeat for a held button: fires on the press, then after a pause, faster and faster.
        struct Repeat {
            float held = -1.0f, next = 0.0f;
            int fire(bool down, float dt, float delay = 0.38f, float rate = 0.075f) {
                if (!down) { held = -1.0f; return 0; }
                if (held < 0.0f) { held = 0.0f; next = delay; return 1; }
                held += dt;
                int n = 0;
                while (held >= next && n < 4) { n++; next += rate; }
                return n;
            }
            // how much to step an integer by while held: 1, then 5, then 25
            int amount() const { return held > 2.2f ? 25 : held > 1.1f ? 5 : 1; }
        };

        struct Page {
            SectionPtr section;              // null = home
            std::string title, crumb;
            std::vector<ItemPtr> rows;
            int sel = 0;
            float scroll = 0, scroll_target = 0;
            float sel_y = -1;                // animated selection position (content y)
            float enter = 0;                 // 0..1 slide-in
        };
    }

    struct Ui::Impl {
        Model& model;
        UiHost host;
        ImFont* bold;
        ImFont* reg;
        std::vector<Page> stack;
        float U = 1.0f, W = 1920, H = 1080;
        float RS = 1.5f;   // rows (buttons) and their text, relative to the rest of the menu
        float HS = 2.0f;   // the help panel (the explanation beside the rows)
        float open_t = 0;                    // 0..1 menu opening
        float save_timer = -1;
        int pending = 0;
        Repeat rep_up, rep_down, rep_left, rep_right;
        bool prev_ok = false, prev_back = false, prev_pu = false, prev_pd = false;
        // pointer
        bool p_down_prev = false, dragging = false;
        float p0x = 0, p0y = 0, p_scroll0 = 0, last_px = -1e9f, last_py = -1e9f;
        int press_row = -1, press_zone = 0, press_button = 0;
        Rect back_btn, close_btn, restart_btn;
        bool animating = false;
        Device device = Device::Pad;
        ItemPtr restart_item, close_item;

        Impl(Model& m, UiHost h, ImFont* b, ImFont* r) : model(m), host(std::move(h)), bold(b), reg(r) {}

        float px(float v) const { return v * U; }
        float rp(float v) const { return v * U * RS; }   // inside the rows

        // ------------------------------------------------ pages
        Page make_home() {
            Page p;
            p.title = "Settings";
            p.crumb = "DivaOverlay";
            const char* order[] = { "DivaLinux", "Game", "Android" };
            const char* file_of[] = { "DivaLinux.ini", "config.ini", "divarecomp_env.txt" };
            for (int g = 0; g < 3; g++) {
                bool any = false;
                for (auto& s : model.sections)
                    if (s->group == order[g]) {
                        if (!any) {
                            auto h = std::make_shared<Item>();
                            h->type = Item::Type::Header;
                            h->label = std::string(order[g]) + "\t" + file_of[g];
                            p.rows.push_back(h);
                            any = true;
                        }
                        auto it = std::make_shared<Item>();
                        it->type = Item::Type::Section;
                        it->label = s->title;
                        it->help = s->subtitle;
                        it->section = s;
                        p.rows.push_back(it);
                    }
            }
            auto h = std::make_shared<Item>();
            h->type = Item::Type::Header;
            h->label = "DivaOverlay";
            p.rows.push_back(h);
            restart_item = std::make_shared<Item>();
            restart_item->type = Item::Type::Action;
            restart_item->label = "Restart game";
            restart_item->help = "Saves your changes and restarts the game, so the settings that are read at start-up take effect.";
            restart_item->action = [this]() { flush(); if (host.restart) host.restart(); };
            close_item = std::make_shared<Item>();
            close_item->type = Item::Type::Action;
            close_item->label = "Back to the game";
            close_item->help = "Closes the menu. Your changes are saved; the ones marked \"after a restart\" wait until the next start.";
            close_item->action = [this]() { close(); };
            p.rows.push_back(restart_item);
            p.rows.push_back(close_item);
            p.enter = 1.0f;
            return p;
        }

        Page make_page(const SectionPtr& s, const std::string& crumb) {
            Page p;
            p.section = s;
            p.title = s->title;
            p.crumb = crumb;
            p.rows = s->items;
            return p;
        }

        static bool selectable(const ItemPtr& it) { return it->type != Item::Type::Header; }

        void fix_sel(Page& p, int dir = 1) {
            if (p.rows.empty()) { p.sel = 0; return; }
            p.sel = std::max(0, std::min((int)p.rows.size() - 1, p.sel));
            if (selectable(p.rows[p.sel])) return;
            for (int k = p.sel; k >= 0 && k < (int)p.rows.size(); k += dir)
                if (selectable(p.rows[k])) { p.sel = k; return; }
            for (int k = p.sel; k >= 0 && k < (int)p.rows.size(); k -= dir)
                if (selectable(p.rows[k])) { p.sel = k; return; }
        }

        void move_sel(int dir, int count = 1) {
            Page& p = stack.back();
            int s = p.sel;
            for (int c = 0; c < count; c++) {
                int k = s + dir;
                while (k >= 0 && k < (int)p.rows.size() && !selectable(p.rows[k])) k += dir;
                if (k < 0 || k >= (int)p.rows.size()) {
                    if (count == 1) {   // wrap around, like the game's menus
                        k = dir > 0 ? 0 : (int)p.rows.size() - 1;
                        while (k >= 0 && k < (int)p.rows.size() && !selectable(p.rows[k])) k += dir;
                        if (k >= 0 && k < (int)p.rows.size()) s = k;
                    }
                    break;
                }
                s = k;
            }
            p.sel = s;
        }

        void push(const SectionPtr& s) {
            std::string crumb = stack.back().section ? stack.back().crumb + "  \xE2\x80\xBA  " + stack.back().title : s->group;
            stack.push_back(make_page(s, crumb));
            fix_sel(stack.back());
        }

        void pop() {
            if (stack.size() > 1) {
                stack.pop_back();
                return;
            }
            close();
        }

        void close() {
            flush();
            if (host.close) host.close();
        }

        void flush() {
            if (model.any_dirty())
                model.save_dirty();
            save_timer = -1;
        }

        void changed() {
            save_timer = 0.6f;
            pending = model.pending_restart_count();
        }

        void activate(const ItemPtr& it) {
            switch (it->type) {
            case Item::Type::Section: push(it->section); break;
            case Item::Type::Action: if (it->action) it->action(); break;
            case Item::Type::Setting: if (it->activate()) changed(); break;
            default: break;
            }
        }

        // ------------------------------------------------ layout
        struct RowGeo { float y, h; };   // content coordinates
        float list_top() const { return px(178); }
        float list_bottom() const { return H - px(112); }
        float row_h(const ItemPtr& it) const { return it->type == Item::Type::Header ? rp(50) : rp(74); }

        std::vector<RowGeo> geometry(const Page& p) const {
            std::vector<RowGeo> g;
            float y = 0;
            for (size_t i = 0; i < p.rows.size(); i++) {
                float h = row_h(p.rows[i]);
                if (p.rows[i]->type == Item::Type::Header && i > 0) y += px(10);
                g.push_back({ y, h });
                y += h;
            }
            return g;
        }

        float arc_x(float screen_y) const {
            const float top = list_top(), bot = list_bottom();
            const float c = (top + bot) * 0.5f, half = std::max(1.0f, (bot - top) * 0.5f);
            float d = (screen_y - c) / half;
            d = std::max(-1.2f, std::min(1.2f, d));
            return px(62) + px(46) * (1.0f - d * d);
        }

        float row_width() const { return std::min(px(860), W * 0.56f); }
        float skew() const { return px(20); }

        // ------------------------------------------------ drawing helpers
        static void para(ImDrawList* dl, float x, float y, float w, float h, float sk, ImU32 c) {
            ImVec2 pts[4] = { { x + sk, y }, { x + w + sk, y }, { x + w, y + h }, { x, y + h } };
            dl->AddConvexPolyFilled(pts, 4, c);
        }
        static void para_line(ImDrawList* dl, float x, float y, float w, float h, float sk, ImU32 c, float t) {
            ImVec2 pts[4] = { { x + sk, y }, { x + w + sk, y }, { x + w, y + h }, { x, y + h } };
            dl->AddPolyline(pts, 4, c, t, ImDrawFlags_Closed);
        }
        ImVec2 measure(ImFont* f, float size, const std::string& s, float wrap = 0.0f) const {
            return f->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str());
        }
        void text(ImDrawList* dl, ImFont* f, float size, float x, float y, ImU32 c, const std::string& s, float wrap = 0.0f) const {
            dl->AddText(f, size, ImVec2(std::floor(x), std::floor(y)), c, s.c_str(), nullptr, wrap);
        }
        // text cut with "..." to fit a width
        std::string fit(ImFont* f, float size, const std::string& s, float w) const {
            if (measure(f, size, s).x <= w) return s;
            std::string t = s;
            while (!t.empty()) {
                // drop one UTF-8 character
                size_t n = t.size() - 1;
                while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
                t.resize(n);
                if (measure(f, size, t + "\xE2\x80\xA6").x <= w) return t + "\xE2\x80\xA6";
            }
            return t;
        }
        void tri(ImDrawList* dl, float cx, float cy, float s, int dir, ImU32 c) const {
            if (dir < 0) dl->AddTriangleFilled(ImVec2(cx - s * 0.55f, cy), ImVec2(cx + s * 0.45f, cy - s * 0.7f), ImVec2(cx + s * 0.45f, cy + s * 0.7f), c);
            else dl->AddTriangleFilled(ImVec2(cx + s * 0.55f, cy), ImVec2(cx - s * 0.45f, cy - s * 0.7f), ImVec2(cx - s * 0.45f, cy + s * 0.7f), c);
        }

        // value box geometry inside a row (screen coordinates)
        Rect value_box(float rx, float ry, float rw, float rh) const {
            const float bw = std::min(rp(300), rw * 0.42f), inset = rp(9);
            Rect r;
            r.x0 = rx + rw - bw - rp(16);
            r.x1 = r.x0 + bw;
            r.y0 = ry + inset;
            r.y1 = ry + rh - inset;
            return r;
        }

        // ------------------------------------------------ one row
        void draw_row(ImDrawList* dl, const ItemPtr& it, float rx, float ry, float rw, float rh, float sel_amt, float alpha) {
            const float sk = skew() * RS;
            if (it->type == Item::Type::Header) {
                std::string a = it->label, b;
                size_t tab = a.find('\t');
                if (tab != std::string::npos) { b = a.substr(tab + 1); a = a.substr(0, tab); }
                const float ty = ry + rh - rp(38);
                // a slanted tag like the game's section labels
                const float tw = measure(bold, rp(25), a).x + rp(40);
                para(dl, rx - rp(6), ty, tw, rp(32), rp(12), col(TEAL_D, 0.92f * alpha));
                text(dl, bold, rp(25), rx + rp(14), ty + rp(2), rgba(255, 255, 255, alpha), a);
                if (!b.empty())
                    text(dl, reg, rp(21), rx + tw + rp(16), ty + rp(5), rgba(255, 255, 255, 0.82f * alpha), b);
                return;
            }
            const bool enabled = it->type != Item::Type::Setting || it->is_enabled();
            const float shift = rp(16) * sel_amt;
            rx += shift;
            // glow and body
            if (sel_amt > 0.01f) {
                for (int k = 3; k >= 1; k--) {
                    const float g = rp(3.0f * k);
                    para_line(dl, rx - g, ry - g, rw + g * 2, rh + g * 2, sk, col(TEAL, 0.18f * sel_amt * alpha / k), rp(3));
                }
            }
            ImU32 fill = rgba(255, 255, 255, (0.80f + 0.17f * sel_amt) * alpha);
            if (it->type == Item::Type::Action && it->accent)
                fill = rgba(255, 0xE2, 0xA8, (0.86f + 0.12f * sel_amt) * alpha);
            para(dl, rx, ry, rw, rh, sk, fill);
            para_line(dl, rx, ry, rw, rh, sk, col(sel_amt > 0.5f ? TEAL : TEAL_D, (0.55f + 0.45f * sel_amt) * alpha), rp(sel_amt > 0.5f ? 3.0f : 1.6f));
            // the selection marker: a teal wedge at the left end
            if (sel_amt > 0.01f) {
                ImVec2 w[4] = { { rx + rp(4) + sk * 0.9f, ry + rp(6) }, { rx + rp(16) + sk * 0.9f, ry + rp(6) },
                                { rx + rp(16) + rp(1), ry + rh - rp(6) }, { rx + rp(4) + rp(1), ry + rh - rp(6) } };
                dl->AddConvexPolyFilled(w, 4, col(TEAL, sel_amt * alpha));
            }

            const float label_x = rx + sk * 0.5f + rp(34);
            const float label_size = rp(30);
            const ImU32 label_c = col(NAVY, (enabled ? 1.0f : 0.42f) * alpha);
            float label_w = rw - rp(60);

            if (it->type == Item::Type::Setting) {
                Rect vb = value_box(rx, ry, rw, rh);
                label_w = vb.x0 - label_x - rp(12);
                const bool on_off = it->opt && (it->opt->kind == lfs::Kind::Bool || it->opt->kind == lfs::Kind::Plugin);
                ImU32 box = col(GREY, 0.92f * alpha * (enabled ? 1.0f : 0.5f));
                if (on_off && it->opt->b && enabled) box = col(TEAL_D, 0.95f * alpha);
                para(dl, vb.x0, vb.y0, vb.x1 - vb.x0, vb.y1 - vb.y0, sk * (vb.y1 - vb.y0) / rh, box);
                const float bh = vb.y1 - vb.y0, bsk = sk * bh / rh;
                const std::string v = fit(bold, rp(26), it->value_text(), vb.x1 - vb.x0 - rp(70));
                const ImVec2 vs = measure(bold, rp(26), v);
                const float cx = (vb.x0 + vb.x1) * 0.5f + bsk * 0.5f;
                text(dl, bold, rp(26), cx - vs.x * 0.5f, vb.y0 + (bh - vs.y) * 0.5f, rgba(255, 255, 255, (enabled ? 1.0f : 0.6f) * alpha), v);
                if (it->steppable() && enabled) {
                    const float a = (0.55f + 0.45f * sel_amt) * alpha;
                    tri(dl, vb.x0 + bsk * 0.5f + rp(20), vb.y0 + bh * 0.5f, rp(11), -1, rgba(255, 255, 255, a));
                    tri(dl, vb.x1 + bsk * 0.5f - rp(20), vb.y0 + bh * 0.5f, rp(11), 1, rgba(255, 255, 255, a));
                }
                // changed, waiting for a restart
                if (it->pending_restart()) {
                    dl->AddCircleFilled(ImVec2(vb.x0 - rp(14), vb.y0 + bh * 0.5f), rp(6), col(AMBER, alpha));
                    label_w -= rp(18);
                }
            }
            else if (it->type == Item::Type::Section) {
                const float ax = rx + rw + sk * 0.5f - rp(34);
                tri(dl, ax, ry + rh * 0.5f, rp(12), 1, col(TEAL_D, alpha));
                if (!it->help.empty()) {
                    const float max_w = rw * 0.46f;
                    const std::string sub = fit(reg, rp(21), it->help, max_w);
                    const ImVec2 ss = measure(reg, rp(21), sub);
                    text(dl, reg, rp(21), ax - rp(22) - ss.x, ry + (rh - ss.y) * 0.5f, col(GREY, 0.95f * alpha), sub);
                    label_w = ax - rp(22) - ss.x - label_x - rp(16);
                }
            }
            else if (it->type == Item::Type::Action && it == restart_item && pending > 0) {
                char buf[64];
                snprintf(buf, sizeof(buf), pending == 1 ? "1 change waiting" : "%d changes waiting", pending);
                const ImVec2 ss = measure(bold, rp(22), buf);
                text(dl, bold, rp(22), rx + rw + sk * 0.5f - rp(30) - ss.x, ry + (rh - ss.y) * 0.5f, rgba(0xB8, 0x5C, 0x00, alpha), buf);
                label_w = rw - ss.x - rp(90);
            }
            const std::string label = fit(bold, label_size, it->label, std::max(rp(40), label_w));
            const ImVec2 ls = measure(bold, label_size, label);
            text(dl, bold, label_size, label_x, ry + (rh - ls.y) * 0.5f, label_c, label);
        }

        // ------------------------------------------------ help panel
        void draw_help(ImDrawList* dl, const ItemPtr& it, float alpha) {
            const float x0 = std::max(W * 0.60f, px(62) + row_width() + px(110)), x1 = W - px(44);
            if (x1 - x0 < px(260) || !it) return;
            std::string body = it->help;
            if (it->type == Item::Type::Section && it->section) {
                std::vector<ItemPtr> all;
                it->section->collect(all);
                char buf[64];
                snprintf(buf, sizeof(buf), "%s%d setting%s", body.empty() ? "" : "\n\n", (int)all.size(), all.size() == 1 ? "" : "s");
                body += buf;
            }
            std::string badge, hint;
            int badge_kind = 0;
            if (it->type == Item::Type::Setting) {
                if (!it->is_enabled() && !it->disabled_hint.empty())
                    hint = it->disabled_hint;
                if (it->pending_restart()) { badge = "Changed: restart to apply"; badge_kind = 2; }
                else if (it->live) { badge = "Applies right away"; badge_kind = 1; }
                else { badge = "Applies after a restart"; badge_kind = 3; }
            }

            // At HS times the menu's own size, smaller (down to 1) for a text too long for the
            // space down to the footer.
            float sc = HS;
            auto hp = [&](float v) { return v * U * sc; };
            const float y0 = list_top() + px(6), limit = host.slider_controls ? controls_top() - px(14) : H - px(40);
            float pad, wrap, h;
            ImVec2 ts, bs, hs, ws;
            for (;;) {
                pad = hp(26);
                wrap = x1 - x0 - pad * 2;
                h = pad;
                ts = measure(bold, hp(32), it->label, wrap);
                h += ts.y + hp(12);
                bs = measure(reg, hp(24), body, wrap);
                h += bs.y + hp(18);
                hs = hint.empty() ? ImVec2(0, 0) : measure(reg, hp(22), hint, wrap);
                if (!hint.empty()) h += hs.y + hp(12);
                if (!badge.empty()) h += hp(40) + hp(10);
                ws = it->where.empty() ? ImVec2(0, 0) : measure(reg, hp(19), it->where, wrap);
                if (!it->where.empty()) h += ws.y + hp(6);
                h += pad;
                if (y0 + h <= limit || sc <= 1.0f)
                    break;
                sc = std::max(1.0f, sc - 0.1f);
            }

            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + h), col(PANEL, 0.80f * alpha), hp(14));
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + hp(5)), col(TEAL, 0.95f * alpha), hp(14), ImDrawFlags_RoundCornersTop);
            float y = y0 + pad;
            text(dl, bold, hp(32), x0 + pad, y, rgba(255, 255, 255, alpha), it->label, wrap);
            y += ts.y + hp(12);
            text(dl, reg, hp(24), x0 + pad, y, rgba(0xE4, 0xEC, 0xF4, alpha), body, wrap);
            y += bs.y + hp(18);
            if (!hint.empty()) {
                text(dl, reg, hp(22), x0 + pad, y, col(AMBER, alpha), hint, wrap);
                y += hs.y + hp(12);
            }
            if (!badge.empty()) {
                const ImVec2 bsz = measure(bold, hp(21), badge);
                const int* c = badge_kind == 1 ? TEAL_D : badge_kind == 2 ? AMBER : GREY;
                para(dl, x0 + pad, y, bsz.x + hp(34), hp(36), hp(10), col(c, 0.95f * alpha));
                text(dl, bold, hp(21), x0 + pad + hp(20), y + (hp(36) - bsz.y) * 0.5f, badge_kind == 2 ? col(NAVY, alpha) : rgba(255, 255, 255, alpha), badge);
                y += hp(40) + hp(10);
            }
            if (!it->where.empty())
                text(dl, reg, hp(19), x0 + pad, y, rgba(0x9F, 0xB3, 0xC6, alpha), it->where, wrap);
        }

        // ------------------------------------------------ slider controller diagram
        // Bottom right, under the help: the controller as the game's menus use it.
        float controls_h() const { return px(290); }
        float controls_top() const { return list_bottom() - controls_h(); }

        void draw_controls(ImDrawList* dl, float alpha) {
            const float x0 = std::max(W * 0.60f, px(62) + row_width() + px(110)), x1 = W - px(44);
            if (x1 - x0 < px(260)) return;
            const float y0 = controls_top(), y1 = list_bottom();
            const float pad = px(22);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(PANEL, 0.80f * alpha), px(14));
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + px(5)), col(TEAL, 0.95f * alpha), px(14), ImDrawFlags_RoundCornersTop);
            text(dl, bold, px(26), x0 + pad, y0 + px(16), rgba(255, 255, 255, alpha), "Slider controller");
            const std::string close = "Select + Start: close";
            const ImVec2 cs = measure(reg, px(20), close);
            text(dl, reg, px(20), x1 - pad - cs.x, y0 + px(21), rgba(0x9F, 0xB3, 0xC6, alpha), close);

            // the controller: a body with the slider along its top and four buttons under it
            const float bx0 = x0 + pad, bx1 = x1 - pad, by0 = y0 + px(62), by1 = y1 - px(20);
            dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), rgba(0x1C, 0x2A, 0x3A, 0.92f * alpha), px(18));
            dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), col(TEAL, 0.55f * alpha), px(18), 0, px(2));

            // slider strip; its edges light up as up and down
            const float sx0 = bx0 + px(18), sx1 = bx1 - px(18), sy0 = by0 + px(18), sy1 = sy0 + px(46);
            const float edge = (sx1 - sx0) * 0.16f;
            dl->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1), rgba(0xD8, 0xE2, 0xEC, 0.85f * alpha), px(10));
            for (float x = sx0 + edge; x < sx1 - edge - 1; x += (sx1 - sx0 - edge * 2) / 24.0f)
                dl->AddLine(ImVec2(x, sy0 + px(10)), ImVec2(x, sy1 - px(10)), rgba(0x9F, 0xB3, 0xC6, 0.6f * alpha), px(1));
            dl->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx0 + edge, sy1), col(TEAL, 0.95f * alpha), px(10), ImDrawFlags_RoundCornersLeft);
            dl->AddRectFilled(ImVec2(sx1 - edge, sy0), ImVec2(sx1, sy1), col(TEAL, 0.95f * alpha), px(10), ImDrawFlags_RoundCornersRight);
            auto edge_label = [&](float cx, bool up, const char* s) {
                const ImVec2 ts = measure(bold, px(20), s);
                const float w = px(16) + px(8) + ts.x, lx = cx - w * 0.5f, cy = (sy0 + sy1) * 0.5f;
                const ImU32 c = rgba(255, 255, 255, alpha);
                if (up) dl->AddTriangleFilled(ImVec2(lx + px(8), cy - px(7)), ImVec2(lx, cy + px(6)), ImVec2(lx + px(16), cy + px(6)), c);
                else dl->AddTriangleFilled(ImVec2(lx, cy - px(6)), ImVec2(lx + px(16), cy - px(6)), ImVec2(lx + px(8), cy + px(7)), c);
                text(dl, bold, px(20), lx + px(24), cy - ts.y * 0.5f, c, s);
            };
            edge_label(sx0 + edge * 0.5f, true, "Up");
            edge_label(sx1 - edge * 0.5f, false, "Down");

            // buttons: triangle, square, cross, circle
            struct Btn { int kind; int c[3]; const char* label; int arrow; };
            static const Btn btns[4] = {
                { 0, { 0x3A, 0xC6, 0x8A }, "Change", -1 },
                { 1, { 0xE8, 0x6A, 0xB8 }, "Change", 1 },
                { 2, { 0x4A, 0x8C, 0xF0 }, "Back", 0 },
                { 3, { 0xF0, 0x5A, 0x5A }, "Select", 0 },
            };
            const float r = px(30), cyb = sy1 + px(22) + r, step = (sx1 - sx0) / 4.0f;
            for (int i = 0; i < 4; i++) {
                const Btn& b = btns[i];
                const float cx = sx0 + step * (i + 0.5f);
                dl->AddCircleFilled(ImVec2(cx, cyb), r, col(b.c, 0.95f * alpha), 40);
                dl->AddCircle(ImVec2(cx, cyb), r, rgba(255, 255, 255, 0.85f * alpha), 40, px(2.5f));
                const ImU32 w = rgba(255, 255, 255, alpha);
                const float s = r * 0.48f, t = px(3.5f);
                switch (b.kind) {
                case 0: { ImVec2 p[3] = { { cx, cyb - s }, { cx + s * 0.95f, cyb + s * 0.65f }, { cx - s * 0.95f, cyb + s * 0.65f } };
                          dl->AddPolyline(p, 3, w, ImDrawFlags_Closed, t); break; }
                case 1: dl->AddRect(ImVec2(cx - s * 0.8f, cyb - s * 0.8f), ImVec2(cx + s * 0.8f, cyb + s * 0.8f), w, 0, 0, t); break;
                case 2: dl->AddLine(ImVec2(cx - s * 0.8f, cyb - s * 0.8f), ImVec2(cx + s * 0.8f, cyb + s * 0.8f), w, t);
                        dl->AddLine(ImVec2(cx + s * 0.8f, cyb - s * 0.8f), ImVec2(cx - s * 0.8f, cyb + s * 0.8f), w, t); break;
                default: dl->AddCircle(ImVec2(cx, cyb), s * 0.9f, w, 32, t); break;
                }
                // label under it, with an arrow for the value buttons
                const ImVec2 ts = measure(bold, px(20), b.label);
                const float aw = b.arrow ? px(12) + px(6) : 0.0f, lw = ts.x + aw;
                float lx = cx - lw * 0.5f;
                const float ly = cyb + r + px(10), acy = ly + ts.y * 0.5f;
                const ImU32 lc = rgba(255, 255, 255, alpha);
                if (b.arrow < 0) {
                    dl->AddTriangleFilled(ImVec2(lx, acy), ImVec2(lx + px(12), acy - px(8)), ImVec2(lx + px(12), acy + px(8)), col(TEAL, alpha));
                    lx += aw;
                }
                text(dl, bold, px(20), lx, ly, lc, b.label);
                if (b.arrow > 0) {
                    const float ax = lx + ts.x + px(6);
                    dl->AddTriangleFilled(ImVec2(ax + px(12), acy), ImVec2(ax, acy - px(8)), ImVec2(ax, acy + px(8)), col(TEAL, alpha));
                }
            }
        }

        // ------------------------------------------------ header and footer
        void draw_title(ImDrawList* dl, const Page& p, float alpha) {
            const float y = px(70), h = px(70);
            const float w = std::min(W * 0.5f, px(780));
            // banner: teal, lighter towards the top, white edge
            ImVec2 pts[4] = { { -px(20) + px(24), y }, { w + px(24), y }, { w, y + h }, { -px(20), y + h } };
            dl->AddConvexPolyFilled(pts, 4, col(TEAL_D, 0.94f * alpha));
            ImVec2 hi[4] = { { -px(20) + px(24), y }, { w + px(24), y }, { w + px(24) - px(9), y + h * 0.36f }, { -px(20) + px(24) - px(9), y + h * 0.36f } };
            dl->AddConvexPolyFilled(hi, 4, col(TEAL, 0.65f * alpha));
            dl->AddLine(ImVec2(w + px(24), y), ImVec2(w, y + h), rgba(255, 255, 255, 0.9f * alpha), px(3));
            dl->AddLine(ImVec2(0, y + h), ImVec2(w, y + h), rgba(255, 255, 255, 0.9f * alpha), px(2));
            float tx = px(60);
            back_btn = Rect();
            if (stack.size() > 1) {
                // back arrow (also a button)
                back_btn = { px(14), y, px(64), y + h };
                tri(dl, px(38), y + h * 0.5f, px(16), -1, rgba(255, 255, 255, alpha));
                tx = px(72);
            }
            const std::string t = fit(bold, px(42), p.title, w - tx - px(20));
            const ImVec2 ts = measure(bold, px(42), t);
            text(dl, bold, px(42), tx + px(2), y + (h - ts.y) * 0.5f + px(2), rgba(0, 0x40, 0x48, 0.45f * alpha), t);
            text(dl, bold, px(42), tx, y + (h - ts.y) * 0.5f, rgba(255, 255, 255, alpha), t);
            text(dl, bold, px(22), tx, y - px(32), rgba(255, 255, 255, 0.92f * alpha), p.crumb);

            // close (top right)
            const float cr = px(26), cx = W - px(70), cy = y + h * 0.5f;
            close_btn = { cx - cr - px(8), cy - cr - px(8), cx + cr + px(8), cy + cr + px(8) };
            dl->AddCircleFilled(ImVec2(cx, cy), cr, rgba(255, 255, 255, 0.88f * alpha));
            dl->AddCircle(ImVec2(cx, cy), cr, col(TEAL_D, alpha), 0, px(2.5f));
            const float k = px(9);
            dl->AddLine(ImVec2(cx - k, cy - k), ImVec2(cx + k, cy + k), col(NAVY, alpha), px(4));
            dl->AddLine(ImVec2(cx + k, cy - k), ImVec2(cx - k, cy + k), col(NAVY, alpha), px(4));
        }

        // A button prompt: the glyph, then its label. Returns the x after it.
        float prompt(ImDrawList* dl, float x, float cy, const char* glyph, const char* label, float alpha, int shape = 0) {
            const float r = px(17);
            if (shape == 0) {   // round button with a letter
                dl->AddCircleFilled(ImVec2(x + r, cy), r, rgba(255, 255, 255, 0.92f * alpha));
                const ImVec2 gs = measure(bold, px(20), glyph);
                text(dl, bold, px(20), x + r - gs.x * 0.5f, cy - gs.y * 0.5f, col(NAVY, alpha), glyph);
                x += r * 2 + px(10);
            }
            else if (shape == 1) {   // key cap
                const ImVec2 gs = measure(bold, px(19), glyph);
                const float kw = std::max(r * 2, gs.x + px(18));
                dl->AddRectFilled(ImVec2(x, cy - r), ImVec2(x + kw, cy + r), rgba(255, 255, 255, 0.92f * alpha), px(6));
                text(dl, bold, px(19), x + (kw - gs.x) * 0.5f, cy - gs.y * 0.5f, col(NAVY, alpha), glyph);
                x += kw + px(10);
            }
            else if (shape == 3) {   // no glyph
            }
            else if (shape == 2) {   // left/right arrows
                dl->AddCircleFilled(ImVec2(x + r, cy), r, rgba(255, 255, 255, 0.92f * alpha));
                tri(dl, x + r - px(6), cy, px(7), -1, col(NAVY, alpha));
                tri(dl, x + r + px(6), cy, px(7), 1, col(NAVY, alpha));
                x += r * 2 + px(10);
            }
            const ImVec2 ls = measure(bold, px(22), label);
            text(dl, bold, px(22), x + px(1), cy - ls.y * 0.5f + px(1), rgba(0, 0, 0, 0.5f * alpha), label);
            text(dl, bold, px(22), x, cy - ls.y * 0.5f, rgba(255, 255, 255, alpha), label);
            return x + ls.x + px(30);
        }

        void draw_footer(ImDrawList* dl, float alpha) {
            const float cy = H - px(56);
            // a dark band so the hints read over the game's own HUD
            const ImU32 dark = rgba(4, 10, 24, 0.62f * alpha), clear = rgba(4, 10, 24, 0.0f);
            dl->AddRectFilledMultiColor(ImVec2(0, H - px(130)), ImVec2(W, H), clear, clear, dark, dark);
            float x = px(62);
            const ItemPtr sel = stack.back().rows.empty() ? nullptr : stack.back().rows[stack.back().sel];
            const bool setting = sel && sel->type == Item::Type::Setting;
            if (device == Device::Keys) {
                x = prompt(dl, x, cy, "Enter", "Select", alpha, 1);
                x = prompt(dl, x, cy, "Esc", stack.size() > 1 ? "Back" : "Close", alpha, 1);
                if (setting) x = prompt(dl, x, cy, "", "Change", alpha, 2);
                if (!host.close_hint.empty()) x = prompt(dl, x, cy, host.close_hint.c_str(), "Close", alpha, 1);
            }
            else if (device == Device::Pointer) {
                x = prompt(dl, x, cy, "", setting ? "Tap the arrows to change, the row to switch" : "Tap to open", alpha, 3);
                x = prompt(dl, x, cy, "", "Drag to scroll", alpha, 3);
            }
            else {
                x = prompt(dl, x, cy, host.pad_ps || host.slider_controls ? "\xE2\x97\x8B" : "B", "Select", alpha);
                x = prompt(dl, x, cy, host.pad_ps || host.slider_controls ? "\xC3\x97" : "A", stack.size() > 1 ? "Back" : "Close", alpha);
                if (setting) x = prompt(dl, x, cy, "", "Change", alpha, 2);
                x = prompt(dl, x, cy, "L/R", "Page", alpha, 1);
                if (!host.close_hint.empty()) x = prompt(dl, x, cy, host.close_hint.c_str(), "Close", alpha, 1);
            }
            // restart reminder (also a button)
            restart_btn = Rect();
            if (pending > 0 && host.restart) {
                char buf[96];
                snprintf(buf, sizeof(buf), pending == 1 ? "Restart to apply 1 change" : "Restart to apply %d changes", pending);
                const ImVec2 ts = measure(bold, px(23), buf);
                const float w = ts.x + px(52), h = px(44);
                const float bx = W - px(60) - w;
                para(dl, bx, cy - h * 0.5f, w, h, px(12), col(AMBER, 0.96f * alpha));
                text(dl, bold, px(23), bx + px(28), cy - ts.y * 0.5f, col(NAVY, alpha), buf);
                restart_btn = { bx, cy - h * 0.5f, bx + w + px(12), cy + h * 0.5f };
            }
        }

        // ------------------------------------------------ input
        int hit_row(const Page& p, const std::vector<RowGeo>& g, float x, float y, int* zone) const {
            const float top = list_top();
            if (y < top - px(10) || y > list_bottom() + px(10)) return -1;
            for (size_t i = 0; i < p.rows.size(); i++) {
                if (!selectable(p.rows[i])) continue;
                const float ry = top + g[i].y - p.scroll;
                if (y < ry || y >= ry + g[i].h) continue;
                const float rx = arc_x(ry + g[i].h * 0.5f) + ((int)i == p.sel ? rp(16) : 0.0f);
                if (x < rx - px(10) || x > rx + row_width() + skew() + px(10)) continue;
                if (zone) {
                    *zone = 0;
                    if (p.rows[i]->type == Item::Type::Setting) {
                        Rect vb = value_box(rx, ry, row_width(), g[i].h);
                        if (x >= vb.x0 - px(10) && x <= vb.x1 + skew()) *zone = x < (vb.x0 + vb.x1) * 0.5f ? -1 : 1;
                    }
                }
                return (int)i;
            }
            return -1;
        }

        void handle_input(const Input& in, const std::vector<RowGeo>& g) {
            Page& p = stack.back();
            const float dt = in.dt;
            if (in.up || in.down || in.left || in.right || in.ok || in.back || in.page_up || in.page_down)
                device = in.device == Device::Pointer ? Device::Pad : in.device;

            for (int n = rep_up.fire(in.up, dt); n > 0; n--) move_sel(-1);
            for (int n = rep_down.fire(in.down, dt); n > 0; n--) move_sel(1);
            const ItemPtr cur = p.rows.empty() ? nullptr : p.rows[p.sel];
            const bool fast = cur && cur->is_int();
            for (int n = rep_left.fire(in.left, dt, 0.38f, fast ? 0.05f : 0.12f); n > 0; n--)
                if (cur && cur->type == Item::Type::Setting && cur->step(-1, fast ? rep_left.amount() : 1)) changed();
            for (int n = rep_right.fire(in.right, dt, 0.38f, fast ? 0.05f : 0.12f); n > 0; n--)
                if (cur && cur->type == Item::Type::Setting && cur->step(1, fast ? rep_right.amount() : 1)) changed();
            if (in.page_up && !prev_pu) move_sel(-1, 7);
            if (in.page_down && !prev_pd) move_sel(1, 7);
            const bool ok_edge = in.ok && !prev_ok, back_edge = in.back && !prev_back;
            prev_ok = in.ok; prev_back = in.back; prev_pu = in.page_up; prev_pd = in.page_down;
            if (ok_edge && cur) { activate(cur); return; }
            if (back_edge) { pop(); return; }

            // wheel
            if (in.wheel != 0.0f) {
                p.scroll_target -= in.wheel * rp(74) * 1.5f;
                p.scroll_target = std::max(0.0f, p.scroll_target);
                device = Device::Pointer;
            }

            // pointer
            if (!in.pointer_valid) { p_down_prev = false; dragging = false; return; }
            const bool moved = std::fabs(in.px - last_px) > 0.5f || std::fabs(in.py - last_py) > 0.5f;
            last_px = in.px; last_py = in.py;
            if (in.pointer_down && !p_down_prev) {
                device = Device::Pointer;
                p0x = in.px; p0y = in.py;
                p_scroll0 = p.scroll;
                dragging = false;
                press_button = back_btn.contains(in.px, in.py) ? 1 : close_btn.contains(in.px, in.py) ? 2 : restart_btn.contains(in.px, in.py) ? 3 : 0;
                press_row = press_button ? -1 : hit_row(p, g, in.px, in.py, &press_zone);
                if (press_row >= 0) p.sel = press_row;
            }
            else if (in.pointer_down) {
                if (!dragging && std::fabs(in.py - p0y) > px(18) && press_button == 0) dragging = true;
                if (dragging) {
                    p.scroll = p.scroll_target = std::max(0.0f, p_scroll0 - (in.py - p0y));
                    press_row = -1;
                }
            }
            else if (p_down_prev) {
                // release: a tap
                if (!dragging) {
                    if (press_button == 1 && back_btn.contains(in.px, in.py)) { pop(); }
                    else if (press_button == 2 && close_btn.contains(in.px, in.py)) { close(); }
                    else if (press_button == 3 && restart_btn.contains(in.px, in.py)) { if (restart_item) restart_item->action(); }
                    else if (press_row >= 0) {
                        int zone = 0;
                        if (hit_row(p, g, in.px, in.py, &zone) == press_row) {
                            ItemPtr it = p.rows[press_row];
                            if (it->type == Item::Type::Setting && zone != 0 && it->steppable()) {
                                if (it->step(zone, 1)) changed();
                            }
                            else
                                activate(it);
                        }
                    }
                }
                dragging = false;
                press_row = -1;
                press_button = 0;
            }
            else if (moved && in.device == Device::Pointer) {
                // hover selects, like a mouse in the game's menus
                int r = hit_row(p, g, in.px, in.py, nullptr);
                if (r >= 0) p.sel = r;
                device = Device::Pointer;
            }
            p_down_prev = in.pointer_down;
        }

        // ------------------------------------------------ frame
        void frame(const Input& in, float w, float h, ImDrawList* dl) {
            W = w; H = h;
            U = std::min(h / 1080.0f, w / 1920.0f * 1.18f);
            if (stack.empty()) { stack.push_back(make_home()); fix_sel(stack.back()); }
            animating = false;

            if (save_timer >= 0.0f) {
                save_timer -= in.dt;
                if (save_timer < 0.0f) flush();
            }

            {
                Page& p = stack.back();
                std::vector<RowGeo> g = geometry(p);
                handle_input(in, g);
            }
            if (stack.empty()) return;
            Page& p = stack.back();
            fix_sel(p);
            std::vector<RowGeo> g = geometry(p);

            // keep the selection in view (unless the pointer is scrolling)
            const float view_h = list_bottom() - list_top();
            const float content_h = g.empty() ? 0 : g.back().y + g.back().h;
            if (!g.empty() && !dragging && device != Device::Pointer) {
                const float sy = g[p.sel].y, sh = g[p.sel].h, m = rp(74) * 1.6f;
                if (sy - m < p.scroll_target) p.scroll_target = sy - m;
                if (sy + sh + m > p.scroll_target + view_h) p.scroll_target = sy + sh + m - view_h;
            }
            p.scroll_target = std::max(0.0f, std::min(p.scroll_target, std::max(0.0f, content_h - view_h + px(20))));
            if (!dragging) p.scroll = approach(p.scroll, p.scroll_target, 16.0f, in.dt);
            if (std::fabs(p.scroll - p.scroll_target) > 0.5f) animating = true;
            if (!g.empty()) {
                const float target = g[p.sel].y;
                p.sel_y = p.sel_y < 0 ? target : approach(p.sel_y, target, 22.0f, in.dt);
                if (std::fabs(p.sel_y - target) > 0.5f) animating = true;
            }
            open_t = std::min(1.0f, open_t + in.dt / 0.22f);
            p.enter = std::min(1.0f, p.enter + in.dt / 0.18f);
            if (open_t < 1.0f || p.enter < 1.0f) animating = true;
            const float alpha = ease_out(open_t);
            const float slide = (1.0f - ease_out(open_t)) * -px(60) + (1.0f - ease_out(p.enter)) * px(50);

            // a soft shade behind the list so the bars read on bright scenes; the rest stays clear
            const ImU32 dark = rgba(4, 10, 24, 0.50f * alpha), clear = rgba(4, 10, 24, 0.0f);
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W * 0.66f, H), dark, clear, clear, dark);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), rgba(4, 10, 24, 0.16f * alpha));

            if (restart_item) restart_item->accent = pending > 0 ? 1 : 0;
            draw_title(dl, p, alpha);

            // rows
            const float top = list_top(), bot = list_bottom();
            dl->PushClipRect(ImVec2(0, top - px(14)), ImVec2(W, bot + px(14)), true);
            const float rw = row_width();
            for (size_t i = 0; i < p.rows.size(); i++) {
                const float ry = top + g[i].y - p.scroll;
                if (ry + g[i].h < top - px(30) || ry > bot + px(30)) continue;
                const float rx = arc_x(ry + g[i].h * 0.5f) + slide;
                // selection amount follows the animated selection position
                float sel_amt = 0.0f;
                if ((int)i == p.sel) {
                    const float d = std::fabs(p.sel_y - g[i].y) / rp(74);
                    sel_amt = std::max(0.0f, 1.0f - d);
                }
                // fade rows near the list's ends
                float fade = 1.0f;
                const float edge = px(40);
                if (ry < top) fade = std::max(0.0f, 1.0f - (top - ry) / edge);
                if (ry + g[i].h > bot) fade = std::min(fade, std::max(0.0f, 1.0f - (ry + g[i].h - bot) / edge));
                draw_row(dl, p.rows[i], rx, ry, rw, g[i].h - px(12), sel_amt, alpha * fade);
            }
            dl->PopClipRect();

            // scroll hint arrows
            if (p.scroll > px(4))
                tri_up(dl, px(62) + rw * 0.5f, top - px(22), alpha);
            if (p.scroll + view_h < content_h - px(4))
                tri_down(dl, px(62) + rw * 0.5f, bot + px(4), alpha);

            if (!p.rows.empty()) draw_help(dl, p.rows[p.sel], alpha);
            if (host.slider_controls) draw_controls(dl, alpha);
            draw_footer(dl, alpha);
        }

        void tri_up(ImDrawList* dl, float cx, float y, float a) const {
            dl->AddTriangleFilled(ImVec2(cx, y), ImVec2(cx - px(14), y + px(12)), ImVec2(cx + px(14), y + px(12)), rgba(255, 255, 255, 0.85f * a));
        }
        void tri_down(ImDrawList* dl, float cx, float y, float a) const {
            dl->AddTriangleFilled(ImVec2(cx, y + px(12)), ImVec2(cx - px(14), y), ImVec2(cx + px(14), y), rgba(255, 255, 255, 0.85f * a));
        }
    };

    Ui::Ui(Model& model, UiHost host, ImFont* bold, ImFont* regular) : m(new Impl(model, std::move(host), bold, regular)) {}
    Ui::~Ui() { delete m; }

    void Ui::open() {
        m->model.load_all();
        m->stack.clear();
        m->stack.push_back(m->make_home());
        m->fix_sel(m->stack.back());
        m->open_t = 0;
        m->pending = m->model.pending_restart_count();
        m->prev_ok = m->prev_back = m->prev_pu = m->prev_pd = true;   // buttons held while opening aren't presses
        m->rep_up = m->rep_down = m->rep_left = m->rep_right = Repeat();
        m->p_down_prev = true;
        m->dragging = false;
        m->press_row = -1;
        m->press_button = 0;
    }

    void Ui::frame(const Input& in, float w, float h, ImDrawList* dl) { m->frame(in, w, h, dl); }
    void Ui::flush() { m->flush(); }
    void Ui::set_row_scale(float scale) { m->RS = scale < 0.5f ? 0.5f : scale > 3.0f ? 3.0f : scale; }
    void Ui::set_slider_controls(bool on) { m->host.slider_controls = on; }
    void Ui::set_help_scale(float scale) { m->HS = scale < 0.5f ? 0.5f : scale > 3.0f ? 3.0f : scale; }
    bool Ui::animating() const { return m->animating || m->save_timer >= 0.0f; }
}
