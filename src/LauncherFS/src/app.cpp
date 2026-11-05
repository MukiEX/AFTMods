#include "app.hpp"
#include "ini.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace lfs {
    // ------------------------------------------------------------ palette

    namespace col {
        static const ImU32 bg_top   = IM_COL32(0x12, 0x10, 0x2B, 0xFF);
        static const ImU32 bg_bot   = IM_COL32(0x1A, 0x16, 0x40, 0xFF);
        static const ImU32 panel    = IM_COL32(0x1D, 0x1A, 0x42, 0xFF);
        static const ImU32 panel_hi = IM_COL32(0x29, 0x25, 0x5C, 0xFF);
        static const ImU32 line     = IM_COL32(0x2E, 0x2A, 0x63, 0xFF);
        static const ImU32 ink      = IM_COL32(0xEC, 0xEB, 0xFF, 0xFF);
        static const ImU32 muted    = IM_COL32(0x9C, 0x98, 0xC9, 0xFF);
        static const ImU32 faint    = IM_COL32(0x5E, 0x5A, 0x8C, 0xFF);
        static const ImU32 teal     = IM_COL32(0x39, 0xC5, 0xBB, 0xFF);
        static const ImU32 teal_ink = IM_COL32(0x0B, 0x2E, 0x2B, 0xFF);
        static const ImU32 pink     = IM_COL32(0xFF, 0x5A, 0xA8, 0xFF);
        static const ImU32 pink_ink = IM_COL32(0x3A, 0x08, 0x22, 0xFF);
        static const ImU32 warn     = IM_COL32(0xFF, 0xB6, 0x48, 0xFF);
        // DIVA note colours for the face-button prompts
        static const ImU32 n_tri    = IM_COL32(0x3D, 0xDB, 0x9A, 0xFF); // Y / triangle
        static const ImU32 n_cir    = IM_COL32(0xFF, 0x5F, 0x64, 0xFF); // B / circle
        static const ImU32 n_crs    = IM_COL32(0x56, 0x9E, 0xFF, 0xFF); // A / cross
        static const ImU32 n_sqr    = IM_COL32(0xF0, 0x7C, 0xF5, 0xFF); // X / square
    }

    static ImU32 alpha(ImU32 c, float a) {
        int al = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
        return (c & ~IM_COL32_A_MASK) | ((ImU32)std::max(0, std::min(255, al)) << IM_COL32_A_SHIFT);
    }

    enum class Btn { A, B, X, Y, LB, RB, Start, View, DPadH, DPadV };

    // ------------------------------------------------------------ input

    struct Input {
        bool up = false, down = false, left = false, right = false;
        bool accept = false, back = false, prev_tab = false, next_tab = false;
        bool start = false, view = false, y = false, x = false;
        int step_amount = 1;
        bool mouse_moved = false, click = false;
        ImVec2 mouse;
        float wheel = 0.0f;
        // text entry (keyboard overlay)
        std::string typed;
        bool key_backspace = false, key_enter = false, key_escape = false;
    };

    static bool pressed(ImGuiKey k, bool repeat = true) {
        return ImGui::IsKeyPressed(k, repeat);
    }

    static Input read_input(bool text_mode) {
        ImGuiIO& io = ImGui::GetIO();
        Input in;
        in.up = pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp) || pressed(ImGuiKey_GamepadLStickUp);
        in.down = pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_GamepadDpadDown) || pressed(ImGuiKey_GamepadLStickDown);
        in.left = pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft) || pressed(ImGuiKey_GamepadLStickLeft);
        in.right = pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight) || pressed(ImGuiKey_GamepadLStickRight);
        in.accept = pressed(ImGuiKey_GamepadFaceDown, false) || pressed(ImGuiKey_Enter, false) || pressed(ImGuiKey_KeypadEnter, false)
            || (!text_mode && pressed(ImGuiKey_Space, false));
        in.back = pressed(ImGuiKey_GamepadFaceRight, false) || pressed(ImGuiKey_Escape, false)
            || (!text_mode && pressed(ImGuiKey_Backspace, false));
        in.prev_tab = pressed(ImGuiKey_GamepadL1, false) || pressed(ImGuiKey_PageUp, false) || (!text_mode && pressed(ImGuiKey_Q, false));
        in.next_tab = pressed(ImGuiKey_GamepadR1, false) || pressed(ImGuiKey_PageDown, false) || (!text_mode && pressed(ImGuiKey_E, false));
        in.start = pressed(ImGuiKey_GamepadStart, false) || pressed(ImGuiKey_F5, false);
        in.view = pressed(ImGuiKey_GamepadBack, false);
        in.y = pressed(ImGuiKey_GamepadFaceUp, false) || (!text_mode && pressed(ImGuiKey_Tab, false));
        in.x = pressed(ImGuiKey_GamepadFaceLeft) || pressed(ImGuiKey_Delete);

        // Hold left/right longer to change numbers faster
        float held = 0.0f;
        for (ImGuiKey k : { ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight })
            held = std::max(held, ImGui::GetKeyData(k)->DownDuration);
        in.step_amount = held < 1.2f ? 1 : held < 2.4f ? 10 : held < 3.6f ? 100 : 1000;

        in.mouse = io.MousePos;
        in.mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
        in.click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        in.wheel = io.MouseWheel;

        if (text_mode) {
            for (ImWchar c : io.InputQueueCharacters)
                if (c >= 0x20 && c != 0x7F)
                    in.typed += to_utf8(std::wstring(1, (wchar_t)c));
            in.key_backspace = pressed(ImGuiKey_Backspace);
            in.key_enter = pressed(ImGuiKey_Enter, false) || pressed(ImGuiKey_KeypadEnter, false);
            in.key_escape = pressed(ImGuiKey_Escape, false);
        }
        return in;
    }

    // ------------------------------------------------------------ state

    struct View {
        std::string title;
        std::vector<OptionPtr>* opts = nullptr;
        std::string empty_text;
        int focus = -1;
        float scroll = 0.0f;
        OptionPtr owner;  // plugin, for sub-pages
    };

    enum class Overlay { None, Picker, Keys, Dialog };
    enum class KeysMode { Text, Number, Resolution };

    struct DialogButton {
        std::string label;
        std::function<void()> action;
        bool primary = false;
    };

    struct App::Impl {
        std::vector<Page> pages;
        Host host;
        Fonts fonts;

        int tab = 0;
        std::vector<View> base_views;       // one per page
        std::vector<View> sub_views;        // plugin option pages on top of the current tab
        int home_focus = 0;
        float focus_anim = 1.0f;            // 0..1, ring settle after focus moves
        ImVec2 last_size = { 0, 0 };
        float S = 1.0f;

        // Button prompts double as buttons: each frame records where it drew them, and a
        // click (or tap on a touch screen) on one next frame acts like pressing that button.
        struct PromptHit { ImRect rect; Btn btn; };
        mutable std::vector<PromptHit> prompt_hits_next;
        std::vector<PromptHit> prompt_hits;
        ImVec2 mouse_pos = { -1e9f, -1e9f };

        Overlay overlay = Overlay::None;
        // picker
        OptionPtr pick_opt;
        int pick_sel = 0;
        float pick_scroll = 0.0f;
        bool pick_custom = false;
        // keyboard
        OptionPtr key_opt;
        KeysMode key_mode = KeysMode::Text;
        std::string key_buf;
        int key_row = 0, key_col = 0;
        bool key_shift = false;
        std::string key_error;
        // dialog
        std::string dlg_title, dlg_text;
        std::vector<DialogButton> dlg_buttons;
        int dlg_sel = 0;
        int dlg_cancel = -1;

        std::string toast;
        float toast_time = 0.0f;
        double time = 0.0;

        View& view() { return sub_views.empty() ? base_views[tab] : sub_views.back(); }
        bool on_home() const { return pages[tab].kind == PageKind::Home && sub_views.empty(); }
        float px(float v) const { return v * S; }

        // ------------------------------------------------ text helpers
        ImVec2 measure(ImFont* f, float size, const std::string& s, float wrap = 0.0f) const {
            return f->CalcTextSizeA(px(size), FLT_MAX, wrap, s.c_str());
        }
        void text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 c, const std::string& s, float wrap = 0.0f) const {
            dl->AddText(f, px(size), pos, c, s.c_str(), nullptr, wrap);
        }

        // ------------------------------------------------ model queries
        OptionPtr find_plugin(const char* name) {
            for (Page& p : pages)
                if (p.kind == PageKind::Plugins)
                    for (auto& o : p.options)
                        if (o->kind == Kind::Plugin && o->label == name)
                            return o;
            return nullptr;
        }
        OptionPtr find_option(const wchar_t* section, const wchar_t* key) {
            for (Page& p : pages)
                for (auto& o : p.options)
                    if (o->key == key && o->section == section)
                        return o;
            return nullptr;
        }
        bool any_dirty() {
            for (Page& p : pages)
                for (auto& o : p.options)
                    if (o->any_dirty())
                        return true;
            return false;
        }
        void save_all() {
            ini_invalidate();
            for (Page& p : pages)
                for (auto& o : p.options)
                    o->save();
            ini_invalidate();
        }
        void show_toast(const std::string& t) {
            toast = t;
            toast_time = 2.4f;
        }

        // ------------------------------------------------ dialogs
        void dialog(const std::string& title, const std::string& text, std::vector<DialogButton> buttons, int cancel_idx, int initial = 0) {
            dlg_title = title;
            dlg_text = text;
            dlg_buttons = std::move(buttons);
            dlg_cancel = cancel_idx;
            dlg_sel = initial;
            overlay = Overlay::Dialog;
        }

        std::vector<std::string> launch_problems(bool& blocking) {
            std::vector<std::string> out;
            blocking = false;
            auto divagl = find_plugin("DivaGL");
            auto divavk = find_plugin("DivaVK");
            auto novidia = find_plugin("Novidia");
            auto shaderpatch = find_plugin("ShaderPatch");
            bool gl = divagl && divagl->b, vk = divavk && divavk->b;
            if (gl && vk) {
                out.push_back("DivaGL and DivaVK are both on. They replace the same renderer, so turn one of them off in Plugins.");
                blocking = true;
            }
            if ((gl || vk) && ((novidia && novidia->b) || (shaderpatch && shaderpatch->b)))
                out.push_back(std::string(gl ? "DivaGL" : "DivaVK") + " is on together with "
                    + ((novidia && novidia->b) ? "Novidia" : "ShaderPatch") + ". They usually conflict; turn the other plugin off in Plugins.");
            auto bat = find_option(L"launcher", L"use_divahook_bat");
            auto quick = find_option(L"patches", L"quick_start");
            if (bat && bat->b && quick && quick->i > 0) {
                out.push_back("Quick Start can't be used with divahook.bat/start.bat. Set Quick Start to Disabled or turn off Use divahook.bat/start.bat in Options.");
                blocking = true;
            }
            return out;
        }

        void do_launch() {
            bool blocking;
            auto problems = launch_problems(blocking);
            if (blocking) {
                dialog("Can't start the game", problems.front(), { { "OK", nullptr, true } }, 0);
                return;
            }
            if (!problems.empty()) {
                dialog("Start anyway?", problems.front(), {
                    { "Play anyway", [this]() { save_all(); if (host.launch) host.launch(); }, true },
                    { "Cancel", nullptr } }, 1, 1);
                return;
            }
            save_all();
            if (host.launch)
                host.launch();
        }

        void reload_all() {
            ini_invalidate();
            for (Page& p : pages)
                for (auto& o : p.options)
                    o->load();
        }

        void do_close() {
            if (host.in_game) {
                if (!any_dirty()) {
                    if (host.quit)
                        host.quit();
                    return;
                }
                dialog("Save your changes?", "Settings take effect when the game restarts.", {
                    { "Save and restart", [this]() { do_launch(); }, true },
                    { "Save for next time", [this]() { save_all(); show_toast("Settings saved"); if (host.quit) host.quit(); } },
                    { "Discard changes", [this]() { reload_all(); if (host.quit) host.quit(); } },
                    { "Cancel", nullptr } }, 3);
                return;
            }
            if (!any_dirty()) {
                if (host.quit)
                    host.quit();
                return;
            }
            dialog("Save your changes?", "Some settings changed since the launcher opened.", {
                { "Save and close", [this]() { save_all(); if (host.quit) host.quit(); }, true },
                { "Close without saving", [this]() { if (host.quit) host.quit(); } },
                { "Cancel", nullptr } }, 2);
        }

        void do_exit_game() {
            dialog("Quit the game?", any_dirty() ? "Your unsaved changes will be lost." : "The game closes.", {
                { "Quit", [this]() { if (host.exit_game) host.exit_game(); }, true },
                { "Cancel", nullptr } }, 1, 1);
        }

        // ------------------------------------------------ overlays: open
        void open_picker(const OptionPtr& o) {
            pick_opt = o;
            pick_custom = o->editable;
            int idx = o->choice_index();
            pick_sel = idx >= 0 ? idx : (pick_custom ? o->choice_count() : 0);
            pick_scroll = -1.0f;
            overlay = Overlay::Picker;
        }

        void open_keys(const OptionPtr& o) {
            key_opt = o;
            key_error.clear();
            key_shift = false;
            key_row = key_col = 0;
            switch (o->kind) {
            case Kind::Int: case Kind::ChoiceNumber:
                key_mode = KeysMode::Number;
                key_buf = std::to_string(o->i);
                break;
            case Kind::Resolution:
                key_mode = KeysMode::Resolution;
                key_buf = o->r.w > 0 ? std::to_string(o->r.w) + "x" + std::to_string(o->r.h) : "";
                break;
            default:
                key_mode = KeysMode::Text;
                key_buf = to_utf8(o->s);
                break;
            }
            overlay = Overlay::Keys;
        }

        void activate(const OptionPtr& o) {
            if (!o->is_enabled())
                return;
            switch (o->kind) {
            case Kind::Bool:
            case Kind::Plugin:
                o->step(1);
                break;
            case Kind::Int:
            case Kind::String:
                open_keys(o);
                break;
            case Kind::Choice:
            case Kind::ChoiceText:
            case Kind::ChoiceNumber:
            case Kind::Resolution:
                open_picker(o);
                break;
            case Kind::Button:
                if (o->func)
                    o->func();
                break;
            default:
                break;
            }
        }

        void open_plugin_options(const OptionPtr& o) {
            if (o->kind != Kind::Plugin || o->children.empty())
                return;
            View v;
            v.title = o->label + " options";
            v.opts = &o->children;
            v.owner = o;
            v.focus = first_focusable(v, 0, 1);
            sub_views.push_back(v);
            focus_anim = 0.0f;
        }

        static bool focusable(const OptionPtr& o) {
            return o->kind != Kind::Section;
        }

        int first_focusable(const View& v, int from, int dir) {
            if (!v.opts)
                return -1;
            int n = (int)v.opts->size();
            for (int i = from; i >= 0 && i < n; i += dir)
                if (focusable((*v.opts)[i]))
                    return i;
            return -1;
        }

        void ensure_focus(View& v) {
            if (!v.opts || v.opts->empty()) {
                v.focus = -1;
                return;
            }
            if (v.focus < 0 || v.focus >= (int)v.opts->size() || !focusable((*v.opts)[v.focus]))
                v.focus = first_focusable(v, 0, 1);
        }

        void move_focus(View& v, int dir) {
            int next = first_focusable(v, v.focus + dir, dir);
            if (next >= 0 && next != v.focus) {
                v.focus = next;
                focus_anim = 0.0f;
            }
        }

        void switch_tab(int dir) {
            int n = (int)pages.size();
            sub_views.clear();
            tab = (tab + dir + n) % n;
            focus_anim = 0.0f;
        }

        // ------------------------------------------------ overlays: input
        void picker_input(const Input& in) {
            int n = pick_opt->choice_count() + (pick_custom ? 1 : 0);
            if (in.up && pick_sel > 0) pick_sel--;
            if (in.down && pick_sel < n - 1) pick_sel++;
            if (in.back) overlay = Overlay::None;
            if (in.accept) {
                if (pick_custom && pick_sel == n - 1) {
                    open_keys(pick_opt);
                    return;
                }
                pick_opt->set_choice(pick_sel);
                overlay = Overlay::None;
            }
        }

        std::vector<std::vector<std::string>> key_rows() const {
            switch (key_mode) {
            case KeysMode::Number:
                return { { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "-", "0", "Delete" }, { "Done" } };
            case KeysMode::Resolution:
                return { { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "x", "0", "Delete" }, { "Done" } };
            default: {
                std::vector<std::vector<std::string>> rows = {
                    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
                    { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
                    { "a", "s", "d", "f", "g", "h", "j", "k", "l", "-" },
                    { "z", "x", "c", "v", "b", "n", "m", ",", ".", "=" },
                    { "Shift", "Space", "Delete", "Done" },
                };
                if (key_shift)
                    for (size_t r = 1; r < 4; r++)
                        for (auto& k : rows[r])
                            if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z')
                                k[0] = (char)(k[0] - 'a' + 'A');
                if (key_shift) {
                    rows[0] = { "!", "\"", "#", "$", "%", "&", "'", "(", ")", "_" };
                    rows[2][9] = "/";
                    rows[3][7] = ":";
                    rows[3][8] = ";";
                    rows[3][9] = "+";
                }
                return rows;
            }
            }
        }

        static void pop_utf8(std::string& s) {
            if (s.empty())
                return;
            size_t i = s.size() - 1;
            while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80)
                i--;
            s.erase(i);
        }

        bool commit_keys() {
            Option& o = *key_opt;
            key_error.clear();
            if (key_mode == KeysMode::Number) {
                if (key_buf.empty() || key_buf == "-") {
                    key_error = "Enter a number.";
                    return false;
                }
                long long v = strtoll(key_buf.c_str(), nullptr, 10);
                if (o.kind == Kind::Int && (v < o.min_int || v > o.max_int)) {
                    key_error = "Enter a number from " + std::to_string(o.min_int) + " to " + std::to_string(o.max_int) + ".";
                    return false;
                }
                if (v < INT32_MIN || v > INT32_MAX) {
                    key_error = "That number is too large.";
                    return false;
                }
                if ((int)v != o.i) {
                    o.i = (int)v;
                    o.dirty = true;
                }
                return true;
            }
            if (key_mode == KeysMode::Resolution) {
                int w = 0, h = 0;
                char sep = 0;
                if (sscanf(key_buf.c_str(), "%d%c%d", &w, &sep, &h) != 3 || (sep != 'x' && sep != 'X') || w <= 0 || h <= 0) {
                    key_error = "Enter a size like 1920x1080.";
                    return false;
                }
                if (!(o.r == Res{ w, h })) {
                    o.r = { w, h };
                    o.dirty = true;
                }
                return true;
            }
            std::wstring w = from_utf8(key_buf);
            if (w != o.s) {
                o.s = w;
                o.dirty = true;
            }
            return true;
        }

        void key_press(const std::string& k) {
            if (k == "Done") {
                if (commit_keys())
                    overlay = Overlay::None;
                return;
            }
            if (k == "Delete") { pop_utf8(key_buf); return; }
            if (k == "Shift") { key_shift = !key_shift; return; }
            if (k == "Space") { key_buf += ' '; return; }
            if (key_buf.size() < 250)
                key_buf += k;
        }

        void keys_input(const Input& in) {
            auto rows = key_rows();
            if (in.up) key_row = std::max(0, key_row - 1);
            if (in.down) key_row = std::min((int)rows.size() - 1, key_row + 1);
            key_col = std::min(key_col, (int)rows[key_row].size() - 1);
            if (in.left) key_col = std::max(0, key_col - 1);
            if (in.right) key_col = std::min((int)rows[key_row].size() - 1, key_col + 1);
            if (in.accept && !in.key_enter)
                key_press(rows[key_row][key_col]);
            if (in.x || in.key_backspace) pop_utf8(key_buf);
            if (in.y && key_mode == KeysMode::Text) key_shift = !key_shift;
            if (in.start || in.key_enter) {
                if (commit_keys())
                    overlay = Overlay::None;
                return;
            }
            if (!in.typed.empty()) {
                for (char c : in.typed) {
                    if (key_mode == KeysMode::Number && !(isdigit((unsigned char)c) || c == '-')) continue;
                    if (key_mode == KeysMode::Resolution && !(isdigit((unsigned char)c) || c == 'x' || c == 'X')) continue;
                    if (key_buf.size() < 250)
                        key_buf += c;
                }
            }
            if (in.back || in.key_escape)
                overlay = Overlay::None;
        }

        void dialog_input(const Input& in) {
            int n = (int)dlg_buttons.size();
            if (in.left || in.up) dlg_sel = std::max(0, dlg_sel - 1);
            if (in.right || in.down) dlg_sel = std::min(n - 1, dlg_sel + 1);
            if (in.back && dlg_cancel >= 0) {
                overlay = Overlay::None;
                return;
            }
            if (in.accept) {
                auto action = dlg_buttons[dlg_sel].action;
                overlay = Overlay::None;
                if (action)
                    action();
            }
        }

        // ------------------------------------------------ main input
        void home_input(const Input& in) {
            const int n = 3;
            if (in.up && home_focus > 0) { home_focus--; focus_anim = 0.0f; }
            if (in.down && home_focus < n - 1) { home_focus++; focus_anim = 0.0f; }
            if (in.accept) {
                if (home_focus == 0) do_launch();
                else if (home_focus == 1) { save_all(); show_toast(host.in_game ? "Saved; restart the game to apply" : "Settings saved"); }
                else if (host.in_game) do_exit_game();
                else do_close();
            }
            else if (in.back && host.in_game)
                do_close();
        }

        void list_input(const Input& in) {
            View& v = view();
            ensure_focus(v);
            if (in.up) move_focus(v, -1);
            if (in.down) move_focus(v, 1);
            if (in.wheel != 0.0f) move_focus(v, in.wheel > 0 ? -1 : 1);
            if (in.back && !sub_views.empty()) {
                sub_views.pop_back();
                focus_anim = 0.0f;
                return;
            }
            if (v.focus < 0)
                return;
            OptionPtr o = (*v.opts)[v.focus];
            if (o->is_enabled() && o->cyclable()) {
                if (in.left) o->step(-1, in.step_amount);
                if (in.right) o->step(1, in.step_amount);
            }
            if (in.accept) activate(o);
            if (in.y && o->kind == Kind::Plugin) open_plugin_options(o);
        }

        // ------------------------------------------------ drawing primitives
        void ring(ImDrawList* dl, ImVec2 c, float r, ImU32 color, float t) const {
            // DIVA-style target ring; t (0..1) settles it in after focus moves
            float k = 1.0f - t;
            float rr = r * (1.0f + 0.7f * k * k);
            dl->AddCircle(c, rr, alpha(color, 0.35f + 0.65f * t), 32, px(2.6f));
            dl->AddCircleFilled(c, px(3.2f), color, 16);
        }

        OptionPtr prompts_opt;   // [launcher] button_prompts: 0 = Xbox letters, 1 = PlayStation shapes
        bool ps_prompts() const { return prompts_opt && prompts_opt->i == 1; }

        // Xbox-style face button: a coloured disc with its letter.
        void face_letter(ImDrawList* dl, ImVec2 c, float r, const char* letter, ImU32 color) const {
            dl->AddCircleFilled(c, r, color, 24);
            ImVec2 ts = measure(fonts.bold, 14, letter);
            dl->AddText(fonts.bold, px(14), ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(0x14, 0x12, 0x2E, 0xFF), letter);
        }

        float prompt(ImDrawList* dl, float x, float cy, Btn b, const std::string& label) const {
            float end = prompt_draw(dl, x, cy, b, label, false);
            if (b == Btn::DPadH || b == Btn::DPadV)
                return end;   // directions aren't a single action
            ImRect hit(ImVec2(x - px(8), cy - px(17)), ImVec2(end - px(26) + px(8), cy + px(17)));
            prompt_hits_next.push_back({ hit, b });
            if (hit.Contains(mouse_pos)) {
                // Hover: a soft pill behind the prompt, drawn on top with the prompt redrawn over it
                dl->AddRectFilled(hit.Min, hit.Max, alpha(col::line, 0.55f), px(17));
                prompt_draw(dl, x, cy, b, label, true);
            }
            return end;
        }

        float prompt_draw(ImDrawList* dl, float x, float cy, Btn b, const std::string& label, bool) const {
            float r = px(11.0f);
            ImVec2 c(x + r, cy);
            float th = px(2.4f);
            const bool ps = ps_prompts();
            if (!ps && (b == Btn::A || b == Btn::B || b == Btn::X || b == Btn::Y)) {
                // Xbox / Steam Deck layout
                switch (b) {
                case Btn::A: face_letter(dl, c, r, "A", IM_COL32(0x6C, 0xC2, 0x4A, 0xFF)); break;
                case Btn::B: face_letter(dl, c, r, "B", IM_COL32(0xE8, 0x55, 0x4A, 0xFF)); break;
                case Btn::X: face_letter(dl, c, r, "X", IM_COL32(0x4A, 0x9B, 0xF0, 0xFF)); break;
                default:     face_letter(dl, c, r, "Y", IM_COL32(0xF2, 0xC2, 0x3A, 0xFF)); break;
                }
                goto label;
            }
            switch (b) {
            case Btn::A: {
                dl->AddCircleFilled(c, r, alpha(col::n_crs, 0.18f), 24);
                float d = r * 0.48f;
                dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), col::n_crs, th);
                dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), col::n_crs, th);
            } break;
            case Btn::B:
                dl->AddCircleFilled(c, r, alpha(col::n_cir, 0.18f), 24);
                dl->AddCircle(c, r * 0.52f, col::n_cir, 24, th);
                break;
            case Btn::Y: {
                dl->AddCircleFilled(c, r, alpha(col::n_tri, 0.18f), 24);
                float d = r * 0.58f;
                ImVec2 p[3] = { ImVec2(c.x, c.y - d), ImVec2(c.x + d * 0.92f, c.y + d * 0.6f), ImVec2(c.x - d * 0.92f, c.y + d * 0.6f) };
                dl->AddPolyline(p, 3, col::n_tri, ImDrawFlags_Closed, th);
            } break;
            case Btn::X: {
                dl->AddCircleFilled(c, r, alpha(col::n_sqr, 0.18f), 24);
                float d = r * 0.44f;
                dl->AddRect(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), col::n_sqr, 0.0f, 0, th);
            } break;
            case Btn::DPadH:
            case Btn::DPadV: {
                float a = r * 0.36f, l = r * 0.92f;
                ImU32 cc = col::muted;
                dl->AddRectFilled(ImVec2(c.x - l, c.y - a), ImVec2(c.x + l, c.y + a), alpha(cc, b == Btn::DPadH ? 0.9f : 0.35f), px(2));
                dl->AddRectFilled(ImVec2(c.x - a, c.y - l), ImVec2(c.x + a, c.y + l), alpha(cc, b == Btn::DPadV ? 0.9f : 0.35f), px(2));
            } break;
            default: {
                const char* t = ps
                    ? (b == Btn::LB ? "L1" : b == Btn::RB ? "R1" : b == Btn::Start ? "Options" : "Share")
                    : (b == Btn::LB ? "LB" : b == Btn::RB ? "RB" : b == Btn::Start ? "Start" : "View");
                ImVec2 ts = measure(fonts.bold, 13, t);
                float w = ts.x + px(14);
                dl->AddRect(ImVec2(x, cy - px(11)), ImVec2(x + w, cy + px(11)), col::muted, px(11), 0, px(1.6f));
                dl->AddText(fonts.bold, px(13), ImVec2(x + px(7), cy - ts.y * 0.5f), col::muted, t);
                r = w * 0.5f;
            } break;
            }
        label:
            float tx = x + r * 2 + px(8);
            if (!label.empty()) {
                ImVec2 ts = measure(fonts.regular, 17, label);
                text(dl, fonts.regular, 17, ImVec2(tx, cy - ts.y * 0.5f), col::ink, label);
                tx += ts.x;
            }
            return tx + px(26);
        }

        void toggle(ImDrawList* dl, ImVec2 right_center, bool on, bool enabled) const {
            float w = px(46), h = px(26);
            ImVec2 a(right_center.x - w, right_center.y - h * 0.5f), b(right_center.x, right_center.y + h * 0.5f);
            float k = enabled ? 1.0f : 0.4f;
            dl->AddRectFilled(a, b, alpha(on ? col::teal : col::line, k), h * 0.5f);
            float kx = on ? b.x - h * 0.5f : a.x + h * 0.5f;
            dl->AddCircleFilled(ImVec2(kx, right_center.y), h * 0.5f - px(4), alpha(on ? col::teal_ink : col::muted, k), 20);
        }

        void chevron(ImDrawList* dl, ImVec2 c, int dir, ImU32 color) const {
            float s = px(5.5f);
            ImVec2 p[3] = { ImVec2(c.x + dir * s, c.y), ImVec2(c.x - dir * s * 0.6f, c.y - s), ImVec2(c.x - dir * s * 0.6f, c.y + s) };
            dl->AddTriangleFilled(p[0], p[1], p[2], color);
        }

        // ------------------------------------------------ frame
        void frame(float W, float H);
        void draw_top(ImDrawList* dl, float W, float top_h, const Input& in);
        void draw_home(ImDrawList* dl, ImRect area, const Input& in, std::string& help);
        void draw_list(ImDrawList* dl, ImRect area, const Input& in, std::string& help, float dt);
        void draw_bottom(ImDrawList* dl, float W, float H, float bar_h, const std::string& help, float help_h);
        void draw_picker(ImDrawList* dl, float W, float H, const Input& in);
        void draw_keys(ImDrawList* dl, float W, float H, const Input& in);
        void draw_dialog(ImDrawList* dl, float W, float H, const Input& in);
        bool overlay_open() const { return overlay != Overlay::None; }
    };

    // ------------------------------------------------------------ top bar

    void App::Impl::draw_top(ImDrawList* dl, float W, float top_h, const Input& in) {
        float m = px(48);
        ImVec2 ts = measure(fonts.bold, 26, "PD Loader");
        text(dl, fonts.bold, 26, ImVec2(m, top_h * 0.5f - ts.y * 0.5f), col::ink, "PD Loader");

        // Tabs, right-aligned, with LB/RB at the ends
        float gap = px(26);
        std::vector<float> widths;
        float total = 0.0f;
        for (Page& p : pages) {
            float w = measure(fonts.regular, 20, p.title).x;
            widths.push_back(w);
            total += w + gap;
        }
        float pill = px(40);
        float x = W - m - total - pill * 2 + gap - px(10);
        x = std::max(x, m + ts.x + px(40));
        float cy = top_h * 0.5f;
        prompt(dl, x, cy, Btn::LB, "");
        x += pill + px(4);
        for (size_t i = 0; i < pages.size(); i++) {
            bool active = (int)i == tab;
            ImVec2 sz = measure(fonts.regular, 20, pages[i].title);
            ImVec2 pos(x, cy - sz.y * 0.5f);
            ImRect hit(ImVec2(x - gap * 0.5f, 0), ImVec2(x + sz.x + gap * 0.5f, top_h));
            bool hover = !overlay_open() && hit.Contains(in.mouse);
            text(dl, active ? fonts.bold : fonts.regular, 20, pos, active ? col::ink : (hover ? col::ink : col::muted), pages[i].title);
            if (active)
                dl->AddRectFilled(ImVec2(x, cy + sz.y * 0.5f + px(6)), ImVec2(x + sz.x, cy + sz.y * 0.5f + px(9)), col::teal, px(2));
            if (hover && in.click && (int)i != tab) {
                sub_views.clear();
                tab = (int)i;
                focus_anim = 0.0f;
            }
            x += widths[i] + gap;
        }
        prompt(dl, x - gap + px(14), cy, Btn::RB, "");
        dl->AddLine(ImVec2(m, top_h), ImVec2(W - m, top_h), col::line, px(1));
    }

    // ------------------------------------------------------------ home

    void App::Impl::draw_home(ImDrawList* dl, ImRect area, const Input& in, std::string& help) {
        float x0 = area.Min.x, y = area.Min.y + px(28);
        float col_w = std::min(px(460), area.GetWidth() * 0.44f);

        // Play
        ImRect play(ImVec2(x0, y), ImVec2(x0 + col_w, y + px(112)));
        bool f0 = home_focus == 0;
        dl->AddRectFilled(play.Min, play.Max, col::pink, px(20));
        if (f0)
            dl->AddRect(ImVec2(play.Min.x - px(5), play.Min.y - px(5)), ImVec2(play.Max.x + px(5), play.Max.y + px(5)),
                alpha(col::ink, 0.55f + 0.45f * focus_anim), px(24), 0, px(2.5f));
        const char* play_label = host.in_game ? "Restart" : "Play";
        ImVec2 ps = measure(fonts.bold, 44, play_label);
        text(dl, fonts.bold, 44, ImVec2(play.Min.x + px(36), play.GetCenter().y - ps.y * 0.5f), col::pink_ink, play_label);
        // note ring motif inside the button
        dl->AddCircle(ImVec2(play.Max.x - px(64), play.GetCenter().y), px(26), alpha(col::pink_ink, 0.85f), 40, px(4));
        dl->AddCircleFilled(ImVec2(play.Max.x - px(64), play.GetCenter().y), px(7), col::pink_ink, 20);
        y = play.Max.y + px(30);

        const char* rows[2] = { "Save settings", host.in_game ? "Quit game" : "Close launcher" };
        ImRect hits[3] = { play };
        for (int i = 0; i < 2; i++) {
            ImRect r(ImVec2(x0, y), ImVec2(x0 + col_w, y + px(52)));
            hits[i + 1] = r;
            bool f = home_focus == i + 1;
            if (f) {
                dl->AddRectFilled(r.Min, r.Max, col::panel, px(14));
                ring(dl, ImVec2(r.Min.x + px(24), r.GetCenter().y), px(9), col::teal, focus_anim);
            }
            ImVec2 ts = measure(fonts.regular, 21, rows[i]);
            text(dl, fonts.regular, 21, ImVec2(r.Min.x + px(50), r.GetCenter().y - ts.y * 0.5f), f ? col::ink : col::muted, rows[i]);
            y = r.Max.y + px(6);
        }
        for (int i = 0; i < 3; i++)
            if (!overlay_open() && hits[i].Contains(in.mouse)) {
                if (in.mouse_moved && home_focus != i) { home_focus = i; focus_anim = 0.0f; }
                if (in.click) {
                    home_focus = i;
                    if (i == 0) do_launch();
                    else if (i == 1) { save_all(); show_toast(host.in_game ? "Saved; restart the game to apply" : "Settings saved"); }
                    else if (host.in_game) do_exit_game();
                    else do_close();
                }
            }

        static const char* helps[3] = {
            "Saves your settings and starts the game.",
            "Writes your changes to the PD Loader config files without starting the game.",
            "Closes the launcher. You'll be asked about unsaved changes.",
        };
        static const char* helps_in_game[3] = {
            "Saves your settings and restarts the game with them.",
            "Writes your changes to the PD Loader config files. They take effect the next time the game starts.",
            "Closes the game.",
        };
        help = (host.in_game ? helps_in_game : helps)[home_focus];

        // Setup summary
        float sx = x0 + col_w + px(72);
        float sy = area.Min.y + px(28);
        if (sx + px(260) > area.Max.x)
            return;
        float lw = px(170);
        auto line_kv = [&](const std::string& k, const std::string& v, ImU32 vc) {
            text(dl, fonts.regular, 18, ImVec2(sx, sy), col::muted, k);
            text(dl, fonts.regular, 18, ImVec2(sx + lw, sy), vc, v, area.Max.x - sx - lw);
            sy += std::max(px(34), measure(fonts.regular, 18, v, area.Max.x - sx - lw).y + px(12));
        };

        auto divagl = find_plugin("DivaGL");
        auto divavk = find_plugin("DivaVK");
        std::string renderer = (divavk && divavk->b) ? "DivaVK (Vulkan)" : (divagl && divagl->b) ? "DivaGL (OpenGL 4.3)" : "Game's own OpenGL renderer";
        line_kv("Renderer", renderer, col::ink);
        auto mode = find_option(L"resolution", L"display");
        auto res = find_option(L"resolution", L"width");
        if (mode && res)
            line_kv("Display", mode->value_text() + ", " + res->value_text(), col::ink);
        if (!host.gpu.empty())
            line_kv("Graphics", host.gpu, col::ink);
        if (!host.wine.empty())
            line_kv("Wine", host.wine, col::ink);

        bool blocking;
        auto problems = launch_problems(blocking);
        if (!problems.empty()) {
            sy += px(14);
            text(dl, fonts.bold, 18, ImVec2(sx, sy), col::warn, "Check before playing");
            sy += px(34);
            for (auto& p : problems) {
                dl->AddCircle(ImVec2(sx + px(7), sy + px(11)), px(6), col::warn, 16, px(2));
                text(dl, fonts.regular, 18, ImVec2(sx + px(24), sy), col::ink, p, area.Max.x - sx - px(24));
                sy += measure(fonts.regular, 18, p, area.Max.x - sx - px(24)).y + px(14);
            }
        }
    }

    // ------------------------------------------------------------ option lists

    void App::Impl::draw_list(ImDrawList* dl, ImRect area, const Input& in, std::string& help, float dt) {
        View& v = view();
        ensure_focus(v);
        float x0 = area.Min.x, x1 = std::min(area.Max.x, area.Min.x + px(1180));

        float y = area.Min.y;
        if (!sub_views.empty()) {
            // Breadcrumb for plugin option pages
            ImVec2 bs = measure(fonts.bold, 22, v.title);
            text(dl, fonts.bold, 22, ImVec2(x0, y + px(6)), col::ink, v.title);
            prompt(dl, x0 + bs.x + px(24), y + px(6) + bs.y * 0.5f, Btn::B, "Back to Plugins");
            y += px(52);
        }
        ImRect clip(ImVec2(area.Min.x - px(8), y), area.Max);

        if (!v.opts || v.opts->empty()) {
            std::string t = v.empty_text.empty() ? "Nothing to set here." : v.empty_text;
            text(dl, fonts.regular, 20, ImVec2(x0 + px(50), y + px(24)), col::muted, t, px(760));
            help.clear();
            return;
        }

        // Layout pass
        const float row_h = px(52), sec_h = px(48);
        std::vector<float> tops(v.opts->size());
        float total = 0.0f;
        for (size_t i = 0; i < v.opts->size(); i++) {
            tops[i] = total;
            bool sec = (*v.opts)[i]->kind == Kind::Section;
            total += sec ? (i == 0 ? sec_h - px(12) : sec_h) : row_h + px(4);
        }

        // Keep focus in view, smoothly
        float view_h = clip.GetHeight();
        float target = v.scroll;
        if (v.focus >= 0) {
            float ft = tops[v.focus], fb = ft + row_h;
            // show the section header above the first row of a section
            if (v.focus > 0 && (*v.opts)[v.focus - 1]->kind == Kind::Section)
                ft = tops[v.focus - 1];
            float pad = px(40);
            if (ft - pad < target) target = ft - pad;
            if (fb + pad > target + view_h) target = fb + pad - view_h;
        }
        target = std::max(0.0f, std::min(target, std::max(0.0f, total - view_h)));
        v.scroll += (target - v.scroll) * std::min(1.0f, dt * 14.0f);
        if (std::fabs(target - v.scroll) < 0.5f)
            v.scroll = target;

        dl->PushClipRect(clip.Min, clip.Max, true);
        for (size_t i = 0; i < v.opts->size(); i++) {
            const OptionPtr& o = (*v.opts)[i];
            float ry = y + tops[i] - v.scroll;
            if (ry > clip.Max.y || ry + row_h < clip.Min.y)
                continue;
            if (o->kind == Kind::Section) {
                float sy = ry + (i == 0 ? px(6) : px(18));
                text(dl, fonts.bold, 17, ImVec2(x0 + px(50), sy), col::muted, o->label);
                continue;
            }
            ImRect r(ImVec2(x0, ry), ImVec2(x1, ry + row_h));
            bool focused = (int)i == v.focus;
            bool en = o->is_enabled();
            ImU32 label_c = en ? (focused ? col::ink : alpha(col::ink, 0.82f)) : alpha(col::muted, 0.5f);
            ImU32 value_c = en ? (focused ? col::ink : col::muted) : alpha(col::muted, 0.45f);

            if (!overlay_open() && r.Contains(in.mouse) && clip.Contains(in.mouse)) {
                if (in.mouse_moved && !focused) { v.focus = (int)i; focus_anim = 0.0f; focused = true; }
                if (in.click) {
                    v.focus = (int)i;
                    focused = true;
                    // Clicking a chevron steps the value; anywhere else acts like A
                    bool chev_area = o->cyclable() && o->kind != Kind::Bool && o->kind != Kind::Plugin && en;
                    if (chev_area && in.mouse.x > r.Max.x - px(40))
                        o->step(1);
                    else if (chev_area && o->kind == Kind::Int && in.mouse.x > r.Max.x - px(260))
                        o->step(-1);
                    else
                        activate(o);
                }
            }

            if (focused) {
                dl->AddRectFilled(r.Min, r.Max, col::panel, px(14));
                ring(dl, ImVec2(r.Min.x + px(24), r.GetCenter().y), px(9), en ? col::teal : col::faint, focus_anim);
            }
            ImVec2 ls = measure(fonts.regular, 21, o->label);
            text(dl, fonts.regular, 21, ImVec2(r.Min.x + px(50), r.GetCenter().y - ls.y * 0.5f), label_c, o->label);

            float vr = r.Max.x - px(22);
            float cy = r.GetCenter().y;
            if (o->kind == Kind::Bool || o->kind == Kind::Plugin) {
                toggle(dl, ImVec2(vr, cy), o->b, en);
                if (o->kind == Kind::Plugin && !o->children.empty()) {
                    const char* t = "Options";
                    ImVec2 ts = measure(fonts.regular, 16, t);
                    float tx = vr - px(46) - px(24) - ts.x;
                    if (focused)
                        prompt(dl, tx - px(30), cy, Btn::Y, "");
                    text(dl, fonts.regular, 16, ImVec2(tx, cy - ts.y * 0.5f), focused ? col::ink : col::faint, t);
                    if (focused)   // the word "Options" is clickable too
                        prompt_hits_next.push_back({ ImRect(ImVec2(tx - px(4), cy - px(17)), ImVec2(tx + ts.x + px(8), cy + px(17))), Btn::Y });
                }
                if (!o->version.empty() && o->kind == Kind::Plugin && focused && o->children.empty()) {
                    // nothing extra
                }
            }
            else if (o->kind == Kind::Button) {
                const char* t = "Run";
                ImVec2 ts = measure(fonts.regular, 19, t);
                dl->AddRect(ImVec2(vr - ts.x - px(28), cy - px(16)), ImVec2(vr, cy + px(16)), focused ? col::teal : col::line, px(16), 0, px(2));
                text(dl, fonts.regular, 19, ImVec2(vr - ts.x - px(14), cy - ts.y * 0.5f), value_c, t);
            }
            else {
                std::string val = o->value_text();
                bool placeholder = false;
                if (o->kind == Kind::String && val.empty()) {
                    val = "Empty";
                    placeholder = true;
                }
                float maxw = (r.GetWidth() - ls.x - px(140));
                ImVec2 vs = measure(fonts.regular, 21, val);
                while (vs.x > maxw && val.size() > 4) {
                    pop_utf8(val);
                    vs = measure(fonts.regular, 21, val + "\xE2\x80\xA6");
                    if (vs.x <= maxw) { val += "\xE2\x80\xA6"; break; }
                }
                bool chev = focused && en && o->cyclable();
                float right = chev ? vr - px(18) : vr;
                text(dl, fonts.regular, 21, ImVec2(right - vs.x, cy - vs.y * 0.5f), placeholder ? col::faint : value_c, val);
                if (chev) {
                    int idx = o->choice_index(), n = o->choice_count();
                    bool can_l = true, can_r = true;
                    if (o->kind == Kind::Int) { can_l = o->i > o->min_int; can_r = o->i < o->max_int; }
                    else if (idx >= 0) { can_l = idx > 0; can_r = idx < n - 1; }
                    chevron(dl, ImVec2(right - vs.x - px(16), cy), -1, can_l ? col::teal : col::faint);
                    chevron(dl, ImVec2(vr - px(4), cy), 1, can_r ? col::teal : col::faint);
                }
            }

            if (focused) {
                help = o->help;
                if (!en && !o->disabled_hint.empty())
                    help = o->disabled_hint;
                if (o->kind == Kind::Plugin && !o->version.empty())
                    help += (help.empty() ? "" : "\n") + std::string("Version: ") + o->version;
            }
        }
        dl->PopClipRect();

        // Scroll indicator
        if (total > view_h + 1.0f) {
            float track_h = view_h;
            float bar_h = std::max(px(36), track_h * view_h / total);
            float by = clip.Min.y + (track_h - bar_h) * (v.scroll / (total - view_h));
            dl->AddRectFilled(ImVec2(x1 + px(10), by), ImVec2(x1 + px(14), by + bar_h), col::line, px(2));
        }
    }

    // ------------------------------------------------------------ bottom bar

    void App::Impl::draw_bottom(ImDrawList* dl, float W, float H, float bar_h, const std::string& help, float help_h) {
        float m = px(48);
        float hy = H - bar_h - help_h;
        dl->AddLine(ImVec2(m, hy), ImVec2(W - m, hy), col::line, px(1));
        if (!help.empty())
            text(dl, fonts.regular, 18, ImVec2(m + px(50), hy + px(16)), col::muted, help, std::min(W - 2 * m - px(50), px(860)));

        float cy = H - bar_h * 0.5f;
        float x = m;
        if (overlay == Overlay::None) {
            if (on_home()) {
                x = prompt(dl, x, cy, Btn::A, "Select");
                x = prompt(dl, x, cy, Btn::DPadV, "Move");
            }
            else {
                View& v = view();
                OptionPtr o = (v.opts && v.focus >= 0 && v.focus < (int)v.opts->size()) ? (*v.opts)[v.focus] : nullptr;
                if (o && o->is_enabled()) {
                    const char* act = nullptr;
                    switch (o->kind) {
                    case Kind::Bool: case Kind::Plugin: act = "Switch"; break;
                    case Kind::Int: case Kind::String: act = "Type"; break;
                    case Kind::Choice: case Kind::ChoiceText: case Kind::ChoiceNumber: case Kind::Resolution: act = "Choose"; break;
                    case Kind::Button: act = "Run"; break;
                    default: break;
                    }
                    if (act) x = prompt(dl, x, cy, Btn::A, act);
                    if (o->cyclable() && o->kind != Kind::Bool && o->kind != Kind::Plugin)
                        x = prompt(dl, x, cy, Btn::DPadH, "Adjust");
                    if (o->kind == Kind::Plugin && !o->children.empty())
                        x = prompt(dl, x, cy, Btn::Y, "Plugin options");
                }
                if (!sub_views.empty())
                    x = prompt(dl, x, cy, Btn::B, "Back");
            }
            // right side
            float rx = W - m;
            const char* close_label = host.in_game ? "Back to game" : "Close";
            const char* play_label = host.in_game ? "Restart" : "Play";
            ImVec2 a = measure(fonts.regular, 17, close_label);
            ImVec2 b = measure(fonts.regular, 17, play_label);
            float w_close = px(64) + a.x, w_play = px(64) + b.x + px(26);
            prompt(dl, rx - w_close, cy, Btn::View, close_label);
            prompt(dl, rx - w_close - w_play, cy, Btn::Start, play_label);
        }
        else if (overlay == Overlay::Picker) {
            x = prompt(dl, x, cy, Btn::A, "Choose");
            x = prompt(dl, x, cy, Btn::B, "Cancel");
        }
        else if (overlay == Overlay::Keys) {
            x = prompt(dl, x, cy, Btn::A, "Type");
            x = prompt(dl, x, cy, Btn::X, "Delete");
            if (key_mode == KeysMode::Text)
                x = prompt(dl, x, cy, Btn::Y, "Shift");
            x = prompt(dl, x, cy, Btn::Start, "Done");
            x = prompt(dl, x, cy, Btn::B, "Cancel");
        }
        else {
            x = prompt(dl, x, cy, Btn::A, "Select");
            if (dlg_cancel >= 0)
                x = prompt(dl, x, cy, Btn::B, "Cancel");
        }

        if (toast_time > 0.0f) {
            float a = std::min(1.0f, toast_time / 0.4f);
            ImVec2 ts = measure(fonts.regular, 17, toast);
            float tx = W * 0.5f - ts.x * 0.5f;
            dl->AddRectFilled(ImVec2(tx - px(18), cy - px(18)), ImVec2(tx + ts.x + px(18), cy + px(18)), alpha(col::teal, a), px(18));
            text(dl, fonts.bold, 17, ImVec2(tx, cy - ts.y * 0.5f), alpha(col::teal_ink, a), toast);
        }
    }

    // ------------------------------------------------------------ overlays

    static void dim(ImDrawList* dl, float W, float H) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(6, 5, 18, 190));
    }

    void App::Impl::draw_picker(ImDrawList* dl, float W, float H, const Input& in) {
        dim(dl, W, H);
        int n = pick_opt->choice_count() + (pick_custom ? 1 : 0);
        float pw = std::min(px(560), W - px(64));
        float row = px(48);
        int visible = std::max(3, std::min(n, (int)((H - px(300)) / row)));
        float ph = px(84) + visible * row + px(20);
        ImVec2 p0(W * 0.5f - pw * 0.5f, H * 0.5f - ph * 0.5f - px(20));
        dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), col::panel, px(22));
        text(dl, fonts.bold, 22, ImVec2(p0.x + px(28), p0.y + px(24)), col::ink, pick_opt->label);

        if (pick_scroll < 0.0f)
            pick_scroll = (float)std::max(0, pick_sel - visible / 2);
        if (pick_sel < pick_scroll) pick_scroll = (float)pick_sel;
        if (pick_sel >= pick_scroll + visible) pick_scroll = (float)(pick_sel - visible + 1);
        int first = (int)pick_scroll;
        int cur = pick_opt->choice_index();
        float y = p0.y + px(74);
        for (int i = first; i < std::min(n, first + visible); i++) {
            ImRect r(ImVec2(p0.x + px(14), y), ImVec2(p0.x + pw - px(14), y + row));
            bool f = i == pick_sel;
            if (r.Contains(in.mouse)) {
                if (in.mouse_moved) pick_sel = i;
                if (in.click) {
                    pick_sel = i;
                    if (pick_custom && i == n - 1) { open_keys(pick_opt); return; }
                    pick_opt->set_choice(i);
                    overlay = Overlay::None;
                    return;
                }
            }
            if (f) {
                dl->AddRectFilled(r.Min, r.Max, col::panel_hi, px(12));
                ring(dl, ImVec2(r.Min.x + px(22), r.GetCenter().y), px(8), col::teal, 1.0f);
            }
            std::string t = (pick_custom && i == n - 1) ? "Custom value\xE2\x80\xA6" : pick_opt->choice_label(i);
            ImVec2 ts = measure(fonts.regular, 20, t);
            text(dl, fonts.regular, 20, ImVec2(r.Min.x + px(46), r.GetCenter().y - ts.y * 0.5f), f ? col::ink : col::muted, t);
            if (i == cur || (pick_custom && i == n - 1 && cur < 0)) {
                const char* c = "Current";
                ImVec2 cs = measure(fonts.regular, 16, c);
                text(dl, fonts.regular, 16, ImVec2(r.Max.x - cs.x - px(16), r.GetCenter().y - cs.y * 0.5f), col::teal, c);
            }
            y += row;
        }
        if (in.click && !ImRect(p0, ImVec2(p0.x + pw, p0.y + ph)).Contains(in.mouse))
            overlay = Overlay::None;
    }

    void App::Impl::draw_keys(ImDrawList* dl, float W, float H, const Input& in) {
        dim(dl, W, H);
        auto rows = key_rows();
        bool text_mode = key_mode == KeysMode::Text;
        float key = text_mode ? px(58) : px(76), gap = px(8);
        float grid_w = text_mode ? key * 10 + gap * 9 : key * 3 + gap * 2;
        float pw = std::max(grid_w + px(64), std::min(px(760), W - px(64)));
        float ph = px(170) + rows.size() * (key + gap) + px(24);
        ImVec2 p0(W * 0.5f - pw * 0.5f, H * 0.5f - ph * 0.5f - px(20));
        dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), col::panel, px(22));
        text(dl, fonts.bold, 22, ImVec2(p0.x + px(28), p0.y + px(24)), col::ink, key_opt->label);

        // field
        ImRect field(ImVec2(p0.x + px(28), p0.y + px(66)), ImVec2(p0.x + pw - px(28), p0.y + px(118)));
        dl->AddRectFilled(field.Min, field.Max, col::bg_top, px(12));
        dl->AddRect(field.Min, field.Max, col::teal, px(12), 0, px(2));
        ImVec2 ts = measure(fonts.regular, 22, key_buf.empty() ? "M" : key_buf);
        float tx = field.Min.x + px(18);
        float avail = field.GetWidth() - px(36);
        float off = std::max(0.0f, ts.x - avail);
        dl->PushClipRect(field.Min, field.Max, true);
        if (!key_buf.empty())
            text(dl, fonts.regular, 22, ImVec2(tx - off, field.GetCenter().y - ts.y * 0.5f), col::ink, key_buf);
        if (fmod(time, 1.0) < 0.6) {
            float cx = tx - off + (key_buf.empty() ? 0.0f : ts.x) + px(2);
            dl->AddRectFilled(ImVec2(cx, field.GetCenter().y - px(13)), ImVec2(cx + px(2.5f), field.GetCenter().y + px(13)), col::teal);
        }
        dl->PopClipRect();
        if (!key_error.empty())
            text(dl, fonts.regular, 16, ImVec2(field.Min.x, field.Max.y + px(8)), col::warn, key_error);
        else if (key_opt->kind == Kind::Int) {
            std::string range = "From " + std::to_string(key_opt->min_int) + " to " + std::to_string(key_opt->max_int);
            text(dl, fonts.regular, 16, ImVec2(field.Min.x, field.Max.y + px(8)), col::muted, range);
        }

        float y = p0.y + px(150);
        for (size_t ri = 0; ri < rows.size(); ri++) {
            auto& row = rows[ri];
            bool wide_row = ri == rows.size() - 1 || row.size() < (text_mode ? 10u : 3u);
            float total = grid_w;
            float kw = wide_row ? (total - gap * (row.size() - 1)) / row.size() : key;
            float x = W * 0.5f - total * 0.5f;
            for (size_t ci = 0; ci < row.size(); ci++) {
                ImRect r(ImVec2(x, y), ImVec2(x + kw, y + key));
                bool f = (int)ri == key_row && (int)ci == key_col;
                if (r.Contains(in.mouse)) {
                    if (in.mouse_moved) { key_row = (int)ri; key_col = (int)ci; }
                    if (in.click) { key_row = (int)ri; key_col = (int)ci; key_press(row[ci]); }
                }
                bool special = row[ci] == "Done" || (row[ci] == "Shift" && key_shift);
                ImU32 bgc = f ? col::teal : (special ? col::panel_hi : col::line);
                dl->AddRectFilled(r.Min, r.Max, bgc, px(10));
                ImVec2 ks = measure(fonts.regular, row[ci].size() > 1 ? 18 : 22, row[ci]);
                text(dl, fonts.regular, row[ci].size() > 1 ? 18 : 22, ImVec2(r.GetCenter().x - ks.x * 0.5f, r.GetCenter().y - ks.y * 0.5f),
                    f ? col::teal_ink : col::ink, row[ci]);
                x += kw + gap;
            }
            y += key + gap;
        }
    }

    void App::Impl::draw_dialog(ImDrawList* dl, float W, float H, const Input& in) {
        dim(dl, W, H);
        float pw = std::min(px(640), W - px(64));
        float wrap = pw - px(64);
        ImVec2 ms = measure(fonts.regular, 19, dlg_text, wrap);
        float ph = px(32) + px(40) + ms.y + px(36) + px(52) + px(32);
        ImVec2 p0(W * 0.5f - pw * 0.5f, H * 0.5f - ph * 0.5f - px(20));
        dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), col::panel, px(22));
        text(dl, fonts.bold, 24, ImVec2(p0.x + px(32), p0.y + px(28)), col::ink, dlg_title);
        text(dl, fonts.regular, 19, ImVec2(p0.x + px(32), p0.y + px(72)), col::muted, dlg_text, wrap);
        float x = p0.x + px(32);
        float y = p0.y + ph - px(32) - px(52);
        for (size_t i = 0; i < dlg_buttons.size(); i++) {
            auto& b = dlg_buttons[i];
            ImVec2 ts = measure(fonts.bold, 18, b.label);
            ImRect r(ImVec2(x, y), ImVec2(x + ts.x + px(40), y + px(52)));
            bool f = (int)i == dlg_sel;
            if (r.Contains(in.mouse)) {
                if (in.mouse_moved) dlg_sel = (int)i;
                if (in.click) {
                    auto action = b.action;
                    overlay = Overlay::None;
                    if (action) action();
                    return;
                }
            }
            ImU32 fill = f ? (b.primary ? col::pink : col::teal) : col::panel_hi;
            ImU32 tc = f ? (b.primary ? col::pink_ink : col::teal_ink) : col::ink;
            dl->AddRectFilled(r.Min, r.Max, fill, px(26));
            text(dl, fonts.bold, 18, ImVec2(r.Min.x + px(20), r.GetCenter().y - ts.y * 0.5f), tc, b.label);
            x = r.Max.x + px(12);
        }
    }

    // ------------------------------------------------------------ frame

    void App::Impl::frame(float W, float H) {
        ImGuiIO& io = ImGui::GetIO();
        float dt = std::max(io.DeltaTime, 1.0f / 1000.0f);
        time += dt;
        S = std::max(0.6f, std::min(W / 1280.0f, H / 720.0f));
        focus_anim = std::min(1.0f, focus_anim + dt * 7.0f);
        if (toast_time > 0.0f)
            toast_time -= dt;

        Input in = read_input(overlay == Overlay::Keys);

        // Clicks on last frame's button prompts press that button (and aren't seen as a click).
        prompt_hits.swap(prompt_hits_next);
        prompt_hits_next.clear();
        mouse_pos = in.mouse;
        if (in.click) {
            for (auto it = prompt_hits.rbegin(); it != prompt_hits.rend(); ++it) {
                if (!it->rect.Contains(in.mouse))
                    continue;
                switch (it->btn) {
                case Btn::A: in.accept = true; break;
                case Btn::B: in.back = true; if (overlay == Overlay::Keys) in.key_escape = true; break;
                case Btn::X: in.x = true; break;
                case Btn::Y: in.y = true; break;
                case Btn::LB: in.prev_tab = true; break;
                case Btn::RB: in.next_tab = true; break;
                case Btn::Start: in.start = true; break;
                case Btn::View: in.view = true; break;
                default: break;
                }
                in.click = false;
                break;
            }
        }

        // Input
        switch (overlay) {
        case Overlay::Picker: picker_input(in); break;
        case Overlay::Keys: keys_input(in); break;
        case Overlay::Dialog: dialog_input(in); break;
        case Overlay::None:
            if (in.prev_tab) switch_tab(-1);
            else if (in.next_tab) switch_tab(1);
            else if (in.start) do_launch();
            else if (in.view) do_close();
            else if (on_home()) home_input(in);
            else list_input(in);
            break;
        }
        // An overlay opened by this frame's input shouldn't also react to the same click
        Input draw_in = in;
        if (overlay != Overlay::None) {
            draw_in.click = false;
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        // Over the game, the background lets a little of it show through.
        const float bg_alpha = host.in_game ? 0.9f : 1.0f;
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), alpha(col::bg_top, bg_alpha), alpha(col::bg_top, bg_alpha),
            alpha(col::bg_bot, bg_alpha), alpha(col::bg_bot, bg_alpha));

        float top_h = px(76);
        float bar_h = px(60);
        float help_h = px(96);
        Input base_in = overlay == Overlay::None ? in : Input();
        base_in.mouse = overlay == Overlay::None ? in.mouse : ImVec2(-1e9f, -1e9f);
        draw_top(dl, W, top_h, base_in);

        ImRect area(ImVec2(px(48), top_h + px(18)), ImVec2(W - px(48), H - bar_h - help_h - px(8)));
        std::string help;
        if (on_home())
            draw_home(dl, area, base_in, help);
        else
            draw_list(dl, area, base_in, help, dt);
        draw_bottom(dl, W, H, bar_h, help, help_h);

        // Overlays draw on the foreground list so they sit above everything
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        Input ov_in = in;
        if (overlay == Overlay::Picker) draw_picker(fg, W, H, ov_in);
        else if (overlay == Overlay::Keys) draw_keys(fg, W, H, ov_in);
        else if (overlay == Overlay::Dialog) draw_dialog(fg, W, H, ov_in);
        if (overlay != Overlay::None) {
            // prompts for the overlay go above the dim layer
            draw_bottom(fg, W, H, bar_h, std::string(), help_h);
        }
        (void)draw_in;
    }

    // ------------------------------------------------------------ App

    App::App(std::vector<Page> pages, Host host, Fonts fonts) : m(new Impl) {
        m->pages = std::move(pages);
        m->host = std::move(host);
        m->fonts = fonts;
        for (Page& p : m->pages) {
            for (auto& o : p.options)
                o->load();
            View v;
            v.title = p.title;
            v.opts = &p.options;
            v.empty_text = p.empty_text;
            m->base_views.push_back(v);
        }
        m->prompts_opt = m->find_option(L"launcher", L"button_prompts");
    }

    App::~App() {
        delete m;
    }

    void App::frame(float width, float height) {
        m->frame(width, height);
    }

    void App::request_close() {
        if (m->overlay == Overlay::None)
            m->do_close();
    }
}
