#include "model.hpp"
#include "ini.hpp"
#include <algorithm>
#include <climits>
#include <cstdio>

namespace lfs {
    std::string clean_label(const std::wstring& w) {
        std::string s = to_utf8(w);
        while (!s.empty() && (s.back() == ':' || s.back() == ' '))
            s.pop_back();
        return s;
    }

    static OptionPtr base(Kind kind, const wchar_t* key, const wchar_t* section, const std::wstring& file,
        const std::wstring& label, const std::wstring& help) {
        auto o = std::make_shared<Option>();
        o->kind = kind;
        o->key = key ? key : L"";
        o->section = section ? section : L"";
        o->file = file;
        o->label = clean_label(label);
        o->help = to_utf8(help);
        return o;
    }

    OptionPtr make_section(const std::string& label) {
        auto o = std::make_shared<Option>();
        o->kind = Kind::Section;
        o->label = label;
        return o;
    }

    OptionPtr make_bool(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, bool def, bool as_string) {
        auto o = base(Kind::Bool, key, section, file, label, help);
        o->def_bool = def;
        o->save_as_string = as_string;
        return o;
    }

    OptionPtr make_int(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, int def, int min, int max) {
        auto o = base(Kind::Int, key, section, file, label, help);
        o->def_int = def;
        o->min_int = min;
        o->max_int = max;
        return o;
    }

    OptionPtr make_string(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, const std::wstring& def, bool utf8) {
        auto o = base(Kind::String, key, section, file, label, help);
        o->def_str = def;
        o->utf8 = utf8;
        return o;
    }

    OptionPtr make_choice(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, int def, const std::vector<std::wstring>& items, int index_offset) {
        auto o = base(Kind::Choice, key, section, file, label, help);
        o->def_int = def;
        o->items_w = items;
        for (auto& i : items)
            o->items.push_back(to_utf8(i));
        o->index_offset = index_offset;
        return o;
    }

    OptionPtr make_choice_text(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, const std::wstring& def, const std::vector<std::wstring>& items, bool editable, bool utf8) {
        auto o = base(Kind::ChoiceText, key, section, file, label, help);
        o->def_str = def;
        o->items_w = items;
        for (auto& i : items)
            o->items.push_back(to_utf8(i));
        o->editable = editable;
        o->utf8 = utf8;
        return o;
    }

    OptionPtr make_choice_number(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, int def, const std::vector<int>& ints, bool editable) {
        auto o = base(Kind::ChoiceNumber, key, section, file, label, help);
        o->def_int = def;
        o->ints = ints;
        o->editable = editable;
        return o;
    }

    OptionPtr make_resolution(const wchar_t* key, const wchar_t* key2, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, Res def, const std::vector<Res>& list, bool editable, int opts) {
        auto o = base(Kind::Resolution, key, section, file, label, help);
        o->key2 = key2 ? key2 : L"";
        o->def_res = def;
        o->resolutions = list;
        o->editable = editable;
        o->res_opts = opts;
        return o;
    }

    OptionPtr make_button(const std::wstring& label, const std::wstring& help, void (*func)()) {
        auto o = base(Kind::Button, nullptr, nullptr, L"", label, help);
        o->func = func;
        return o;
    }

    OptionPtr make_plugin(const std::wstring& file_name, const std::wstring& config, const std::wstring& name, const std::wstring& help, const std::string& version, std::vector<OptionPtr> children) {
        auto o = base(Kind::Plugin, file_name.c_str(), L"plugins", config, name, help);
        o->def_bool = true;
        o->version = version;
        o->children = std::move(children);
        return o;
    }

    // ------------------------------------------------------------ load / save

    void Option::load() {
        switch (kind) {
        case Kind::Bool:
        case Kind::Plugin:
            b = ini_get_bool(section.c_str(), key.c_str(), def_bool, file.c_str());
            break;
        case Kind::Int:
            i = ini_get_int(section.c_str(), key.c_str(), def_int, file.c_str());
            break;
        case Kind::Choice:
            i = ini_get_int(section.c_str(), key.c_str(), def_int, file.c_str());
            break;
        case Kind::ChoiceNumber:
            i = ini_get_int(section.c_str(), key.c_str(), def_int, file.c_str());
            break;
        case Kind::String:
        case Kind::ChoiceText:
            s = ini_get_string(section.c_str(), key.c_str(), def_str.c_str(), file.c_str(), utf8);
            break;
        case Kind::Resolution: {
            int w = ini_get_int(section.c_str(), key.c_str(), -39, file.c_str());
            int h = ini_get_int(section.c_str(), key2.c_str(), -39, file.c_str());
            if (w == -39 || h == -39) {
                w = def_res.w;
                h = def_res.h;
            }
            if (w == -1 || h == -1)
                r = { -1, -1 };
            else
                r = { w, h };
        } break;
        default:
            break;
        }
        for (auto& c : children)
            c->load();
        dirty = false;
    }

    void Option::save() {
        if (dirty) {
            switch (kind) {
            case Kind::Bool:
                ini_set_bool(section.c_str(), key.c_str(), b, file.c_str(), save_as_string);
                break;
            case Kind::Plugin:
                ini_set_bool(section.c_str(), key.c_str(), b, file.c_str(), false);
                break;
            case Kind::Int:
            case Kind::Choice:
                ini_set_int(section.c_str(), key.c_str(), i, file.c_str());
                break;
            case Kind::ChoiceNumber:
                ini_set_string(section.c_str(), key.c_str(), std::to_wstring(i).c_str(), file.c_str(), false);
                break;
            case Kind::String:
            case Kind::ChoiceText:
                ini_set_string(section.c_str(), key.c_str(), s.c_str(), file.c_str(), utf8);
                break;
            case Kind::Resolution:
                ini_set_string(section.c_str(), key.c_str(), std::to_wstring(r.w).c_str(), file.c_str(), false);
                ini_set_string(section.c_str(), key2.c_str(), std::to_wstring(r.h).c_str(), file.c_str(), false);
                break;
            default:
                break;
            }
            dirty = false;
        }
        for (auto& c : children)
            c->save();
    }

    bool Option::any_dirty() const {
        if (dirty)
            return true;
        for (auto& c : children)
            if (c->any_dirty())
                return true;
        return false;
    }

    // ------------------------------------------------------------ presentation

    std::string Option::res_special_label() const {
        if (res_opts & RES_MATCH_WINDOW) return "Match window";
        if (res_opts & RES_MATCH_SCREEN) return "Match screen";
        if (res_opts & RES_DEFAULT) return "Default";
        return "";
    }

    static std::string res_text(Res r) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d \xC3\x97 %d", r.w, r.h); // U+00D7
        return buf;
    }

    bool Option::cyclable() const {
        switch (kind) {
        case Kind::Bool: case Kind::Plugin: case Kind::Int: return true;
        case Kind::Choice: return items.size() > 1;
        case Kind::ChoiceText: return items.size() > 1;
        case Kind::ChoiceNumber: return ints.size() > 1;
        case Kind::Resolution: return choice_count() > 1;
        default: return false;
        }
    }

    int Option::choice_count() const {
        switch (kind) {
        case Kind::Choice: case Kind::ChoiceText: return (int)items.size();
        case Kind::ChoiceNumber: return (int)ints.size();
        case Kind::Resolution: return (int)resolutions.size() + (res_special_label().empty() ? 0 : 1);
        default: return 0;
        }
    }

    int Option::choice_index() const {
        switch (kind) {
        case Kind::Choice: {
            int idx = i - index_offset;
            return idx >= 0 && idx < (int)items.size() ? idx : -1;
        }
        case Kind::ChoiceText:
            for (size_t k = 0; k < items_w.size(); k++)
                if (items_w[k] == s)
                    return (int)k;
            return -1;
        case Kind::ChoiceNumber:
            for (size_t k = 0; k < ints.size(); k++)
                if (ints[k] == i)
                    return (int)k;
            return -1;
        case Kind::Resolution: {
            int base_idx = res_special_label().empty() ? 0 : 1;
            if (r.w == -1)
                return base_idx ? 0 : -1;
            for (size_t k = 0; k < resolutions.size(); k++)
                if (resolutions[k] == r)
                    return base_idx + (int)k;
            return -1;
        }
        default:
            return -1;
        }
    }

    std::string Option::choice_label(int idx) const {
        switch (kind) {
        case Kind::Choice: case Kind::ChoiceText:
            return idx >= 0 && idx < (int)items.size() ? items[idx] : "";
        case Kind::ChoiceNumber:
            return idx >= 0 && idx < (int)ints.size() ? std::to_string(ints[idx]) : "";
        case Kind::Resolution: {
            int base_idx = res_special_label().empty() ? 0 : 1;
            if (base_idx && idx == 0)
                return res_special_label();
            idx -= base_idx;
            return idx >= 0 && idx < (int)resolutions.size() ? res_text(resolutions[idx]) : "";
        }
        default:
            return "";
        }
    }

    void Option::set_choice(int idx) {
        if (idx < 0 || idx >= choice_count())
            return;
        switch (kind) {
        case Kind::Choice: i = idx + index_offset; break;
        case Kind::ChoiceText: s = items_w[idx]; break;
        case Kind::ChoiceNumber: i = ints[idx]; break;
        case Kind::Resolution: {
            int base_idx = res_special_label().empty() ? 0 : 1;
            r = (base_idx && idx == 0) ? Res{ -1, -1 } : resolutions[idx - base_idx];
        } break;
        default: return;
        }
        dirty = true;
    }

    void Option::step(int dir, int amount) {
        switch (kind) {
        case Kind::Bool:
        case Kind::Plugin:
            b = !b;
            dirty = true;
            return;
        case Kind::Int: {
            long long v = (long long)i + (long long)dir * amount;
            v = std::max<long long>(min_int, std::min<long long>(max_int, v));
            if ((int)v != i) {
                i = (int)v;
                dirty = true;
            }
            return;
        }
        default:
            break;
        }

        int n = choice_count();
        if (n <= 0)
            return;
        int cur = choice_index();
        int next;
        if (cur < 0) {
            // Custom value: move to the nearest list entry in that direction
            next = dir > 0 ? 0 : n - 1;
            if (kind == Kind::ChoiceNumber) {
                next = -1;
                for (int k = 0; k < n; k++)
                    if (dir > 0 ? ints[k] > i : ints[k] < i)
                        if (next < 0 || (dir > 0 ? ints[k] < ints[next] : ints[k] > ints[next]))
                            next = k;
                if (next < 0)
                    return;
            }
        }
        else
            next = std::max(0, std::min(n - 1, cur + dir));
        if (next != cur)
            set_choice(next);
    }

    std::string Option::value_text() const {
        switch (kind) {
        case Kind::Bool:
        case Kind::Plugin:
            return b ? "On" : "Off";
        case Kind::Int:
            return std::to_string(i);
        case Kind::String:
            return to_utf8(s);
        case Kind::Choice: {
            int idx = choice_index();
            return idx >= 0 ? items[idx] : std::to_string(i);
        }
        case Kind::ChoiceText:
            return to_utf8(s);
        case Kind::ChoiceNumber:
            return std::to_string(i);
        case Kind::Resolution:
            return r.w == -1 ? (res_special_label().empty() ? "Default" : res_special_label()) : res_text(r);
        default:
            return "";
        }
    }

    // ------------------------------------------------------------ built-in pages
    // Ported from PD Launcher's framework.h (same keys, files, defaults and help text).

    std::vector<Page> build_builtin_pages(const Paths& p, const ScreenInfo& screen) {
        const wchar_t* PATCHES = L"patches";
        const wchar_t* GRAPHICS = L"graphics";
        const wchar_t* RESOLUTION = L"resolution";
        const wchar_t* LAUNCHER = L"launcher";
        const wchar_t* COMPONENTS = L"components";
        const wchar_t* PLAYERDATA = L"playerdata";
        const wchar_t* KEYCONFIG = L"keyconfig";
        const std::wstring& CONFIG = p.config;

        std::vector<Page> pages;

        // Display -------------------------------------------------------
        {
            Page pg;
            pg.title = "Display";
            auto mode = make_choice(L"display", RESOLUTION, CONFIG, L"Display:", L"Sets the window/screen mode.", 1,
                { L"Windowed", L"Fast", L"Exclusive", L"Safe" });
            auto res = make_resolution(L"width", L"height", RESOLUTION, CONFIG, L"Resolution:", L"Sets the display resolution.",
                { -1, -1 }, screen.resolutions, true, RES_MATCH_SCREEN);
            auto rate = make_choice_number(L"refreshrate", RESOLUTION, CONFIG, L"Refresh Rate:", L"Sets the display refresh rate.",
                60, screen.refresh_rates, true);
            rate->enabled = [mode]() { return mode->i == 2; };
            rate->disabled_hint = "Used only when Display is set to Exclusive.";

            const std::vector<Res> internal = { {320,240}, {426,240}, {640,480}, {854,480}, {960,540}, {1280,720}, {1366,768},
                {1600,900}, {1920,1080}, {2560,1440}, {3200,1800}, {3840,2160}, {5120,2880}, {7680,4320} };
            auto ir_on = make_bool(L"r.enable", RESOLUTION, CONFIG, L"Custom internal resolution", L"Enable or disable custom internal resolution.", false, false);
            auto ir = make_resolution(L"r.width", L"r.height", RESOLUTION, CONFIG, L"Internal resolution:", L"Sets the internal resolution (instead of 1280x720).",
                { 1920, 1080 }, internal, true, RES_MATCH_WINDOW);
            ir->enabled = [ir_on]() { return ir_on->b; };
            ir->disabled_hint = "Turn on Custom internal resolution to change this.";

            pg.options = { make_section("Screen"), mode, res, rate, make_section("Rendering"), ir_on, ir };
            pages.push_back(pg);
        }

        // Graphics ------------------------------------------------------
        {
            Page pg;
            pg.title = "Graphics";
            std::vector<Res> refl = { {256,256}, {512,256}, {512,512}, {640,480}, {854,480}, {960,540}, {1280,720}, {1366,768},
                {1600,900}, {1920,1080}, {2560,1440}, {3200,1800}, {3840,2160}, {5120,2880}, {7680,4320} };
            std::vector<Res> refr = { {256,256}, {512,256}, {512,512}, {1024,1024}, {2048,2048}, {4096,4096} };
            std::vector<Res> shad = { {256,256}, {512,512}, {1024,1024}, {2048,2048}, {4096,4096}, {8192,8192} };
            pg.options = {
                make_choice(L"model", L"GPU", CONFIG, L"Nvidia GPU:", L"Select your Nvidia GPU's architecture to apply the necessary workarounds.\n\nNOTE: Automatic detection does not currently work on GNU/Linux.", -1,
                    { L"Automatic", L"Kepler", L"Maxwell/Pascal", L"Turing", L"Ampere", L"Ada Lovelace", L"Blackwell or newer" }, -1),
                make_bool(L"TAA", GRAPHICS, CONFIG, L"TAA", L"Temporal Anti-Aliasing", true, false),
                make_bool(L"MLAA", GRAPHICS, CONFIG, L"MLAA", L"Morphological Anti-Aliasing", true, false),
                make_choice(L"MAG", GRAPHICS, CONFIG, L"Filter:", L"Image filter.\n\nBilinear: default filter\nNearest-neighbour: sharpest, but blocky\nSharpen: sharp filter\nCone: smooth filter", 0,
                    { L"Bilinear", L"Nearest-neighbour", L"Sharpen", L"Cone" }),
                make_bool(L"DOF", GRAPHICS, CONFIG, L"Depth of Field", L"Blurs the background. Disable for better performance.", true, false),
                make_bool(L"reflections", GRAPHICS, CONFIG, L"Reflections", L"Enable or disable reflections.", true, false),
                make_resolution(L"reflect_res_width", L"reflect_res_height", GRAPHICS, CONFIG, L"Reflection Res:", L"Sets the reflection buffer resolution (instead of 512x256).", { 512, 256 }, refl, true, RES_NONE),
                make_resolution(L"refract_res_width", L"refract_res_height", GRAPHICS, CONFIG, L"Refraction Res:", L"Sets the refraction buffer resolution (instead of 512x256).", { 512, 256 }, refr, true, RES_NONE),
                make_bool(L"shadows", GRAPHICS, CONFIG, L"Shadows", L"Enable or disable shadows.", true, false),
                make_resolution(L"shadow_res_width", L"shadow_res_height", GRAPHICS, CONFIG, L"Shadow Res:", L"Sets the shadow buffer resolution (instead of 2048x2048).", { 2048, 2048 }, shad, true, RES_NONE),
                make_int(L"gamma", GRAPHICS, CONFIG, L"Gamma:", L"Increase to darken shadows.\nSet to 125 for a console-like experience.\n\nDefault: 0 or 100", 100, 0, 200),
                make_bool(L"punchthrough", GRAPHICS, CONFIG, L"Transparent Meshes", L"Show transparent meshes.", true, false),
                make_bool(L"glare", GRAPHICS, CONFIG, L"Glare", L"Enable or disable glare.", true, false),
                make_bool(L"shader", GRAPHICS, CONFIG, L"Shader", L"Enable or disable high-quality shaders.", true, false),
                make_choice(L"NPR1", GRAPHICS, CONFIG, L"Toon (F9):", L"NPR1 shader\n\nPress F9 to toggle.", 0, { L"Default", L"Force on", L"Force off" }),
                make_bool(L"2D", GRAPHICS, CONFIG, L"Disable 3D rendering", L"Disable all 3D passes.\n\nWARNING: The extended data will be deleted unless Prevent Data Deletion is enabled.", false, false),
                make_int(L"lag_compensation", GRAPHICS, CONFIG, L"Lag compensation (ms)", L"Shifts note timing to make up for audio/video latency.", 0, 0, 500),
            };
            pages.push_back(pg);
        }

        // Options -------------------------------------------------------
        {
            Page pg;
            pg.title = "Options";
            const std::wstring& KEYCFG = p.keyconfig;
            pg.options = {
                make_section("Compatibility"),
                make_bool(L"no_movies", PATCHES, CONFIG, L"Disable Movies", L"Disable movies (enable this if the game hangs when loading certain PVs).", false, false),
                make_bool(L"mp4_movies", PATCHES, CONFIG, L"Custom MP4 Adv Movies", L"Enable MP4 (instead of WMV) advertise/attract movies.", false, false),
                make_bool(L"cursor", PATCHES, CONFIG, L"Cursor", L"Enable or disable the mouse cursor.", true, false),
                make_bool(L"stereo", PATCHES, CONFIG, L"Stereo", L"Use 2 channels instead of 4 (when not using DivaSound).", true, false),

                make_section("Controller"),
                make_bool(L"rumble", KEYCONFIG, KEYCFG, L"XInput Rumble", L"Enables rumble during chainslides.", true, true),
                make_int(L"xinput_rumble_intensity_left", KEYCONFIG, KEYCFG, L"Left motor:", L"Left rumble motor intensity (XInput).\n\nDefault: 8000", 8000, 0, 65535),
                make_int(L"xinput_rumble_intensity_right", KEYCONFIG, KEYCFG, L"Right motor:", L"Right rumble motor intensity (XInput).\n\nDefault: 4000", 4000, 0, 65535),
                make_int(L"xinput_preferred", KEYCONFIG, KEYCFG, L"XInput Controller Num:", L"Sets the preferred XInput controller.\nIf unavailable, the next connected controller is used.", 0, 0, 3),
                make_bool(L"hardware_slider", PATCHES, CONFIG, L"Use Hardware Slider", L"Enable this if using a real arcade slider.\n(set the slider to port COM11)", false, false),

                make_section("PV selector"),
                make_choice(L"quick_start", PATCHES, CONFIG, L"Quick Start:", L"Skip one or more menus.", 1, { L"Disabled", L"Guest", L"Guest + Normal" }),
                make_bool(L"no_scrolling_sfx", PATCHES, CONFIG, L"Disable Scrolling SFX", L"Disable the scrolling sound effect.", false, false),
                make_bool(L"unlock_pseudo", PATCHES, CONFIG, L"Unlock PSEUDO modules (incomplete)", L"Lets you play any PV with any performer.\n(incomplete, recommended modules will default to Miku)", false, false),
                make_bool(L"card", PATCHES, CONFIG, L"Unlock card menu (incomplete)", L"Enables the card menu.\n(incomplete, it doesn't bypass the card prompt)", false, false),

                make_section("Stages and songs"),
                make_bool(L"Auto_Databank", PATCHES, CONFIG, L"Auto Databank", L"Automatically generate databank for missing entries and allow multiple PvList files to be used.", true, false),
                make_int(L"Enhanced_Stage_Manager", PATCHES, CONFIG, L"Number of Stages:", L"Set the number of stages (0 = default).", 0, 0, INT_MAX),
                make_bool(L"Enhanced_Stage_Manager_Encore", PATCHES, CONFIG, L"Encore", L"Enable encore stages.", true, false),
                make_bool(L"sing_missed", PATCHES, CONFIG, L"Sing Missed", L"Sing missed notes.", false, false),
                make_bool(L"autopause", KEYCONFIG, KEYCFG, L"Pause Automatically", L"Pause when focus is lost.", true, true),

                make_section("PV customization"),
                make_bool(L"enable_pv_customization", PATCHES, CONFIG, L"Enable PV Customization", L"Enable the options below.", false, false),
                make_choice(L"force_mouth", PATCHES, CONFIG, L"Mouth Type:", L"Change the mouth animations.", 0, { L"Default", L"Force PDA", L"Force FT" }),
                make_choice(L"force_expressions", PATCHES, CONFIG, L"Expression Type:", L"Change the expressions.", 0, { L"Default", L"Force PDA", L"Force FT" }),
                make_choice(L"force_look", PATCHES, CONFIG, L"Look Type:", L"Change the look animations.", 0, { L"Default", L"Force PDA", L"Force FT" }),
                make_bool(L"no_hand_scaling", PATCHES, CONFIG, L"No Hand Scaling", L"Disable hand scaling.", false, false),
                make_int(L"default_hand_size_uint", PATCHES, CONFIG, L"Default Hand Size:", L"0-10000: default\n12200: PDA\n1000000: maximum", 0, 0, 1000000),

                make_section("UI elements"),
                make_bool(L"hide_volume", PATCHES, CONFIG, L"Hide Volume Buttons", L"Hide the volume and SE control buttons.", false, false),
                make_bool(L"no_pv_ui", PATCHES, CONFIG, L"Disable PV UI", L"Remove the photo controls during PV playback.", false, false),
                make_bool(L"hide_pv_watermark", PATCHES, CONFIG, L"Hide PV Watermark", L"Hide the watermark that's usually shown in PV viewing mode.", false, false),
                make_bool(L"hide_status_icons", PATCHES, CONFIG, L"Hide Network Icons", L"Hide the top-right corner status icons.", false, false),
                make_choice(L"status_icons", PATCHES, CONFIG, L"Network Icons Status:", L"Set the state of card reader and network status icons.", 3, { L"Default", L"Hidden", L"Error", L"OK", L"Partial OK" }),
                make_bool(L"no_lyrics", PATCHES, CONFIG, L"Disable Lyrics", L"Disable showing lyrics.", false, false),
                make_bool(L"no_error", PATCHES, CONFIG, L"Disable Error Banner", L"Disable the error banner on the attract screen.", true, false),
                make_bool(L"hide_freeplay", PATCHES, CONFIG, L"Hide \"FREE PLAY\"/\"CREDIT(S)\"", L"Hide the \"FREE PLAY\"/\"CREDIT(S)\" text.", false, false),
                make_bool(L"freeplay", PATCHES, CONFIG, L"FREE PLAY", L"Show \"FREE PLAY\" instead of \"CREDIT(S)\" and don't require credits.", true, false),
                make_bool(L"pdloadertext", PATCHES, CONFIG, L"PD Loader FREE PLAY", L"Show \"PD Loader\" instead of \"FREE PLAY\".", true, false),
                make_bool(L"no_timer", PATCHES, CONFIG, L"Freeze Timer", L"Disable the timer.", true, false),
                make_bool(L"no_timer_sprite", PATCHES, CONFIG, L"Disable Timer Sprite", L"Disable the timer sprite.", true, false),

                make_section("Frame rate"),
                make_bool(L"FPS.Limit.LightMode", GRAPHICS, CONFIG, L"Use Lightweight Limiter", L"Makes the FPS limiter use less CPU.\nMay have less consistent performance.", true, false),
                make_int(L"FPS.Limit", GRAPHICS, CONFIG, L"FPS Limit:", L"Allows you to set a frame rate cap. Set to -1 to unlock the frame rate.", 60, -1, INT_MAX),
                make_int(L"frm.motion.rate", GRAPHICS, CONFIG, L"FRM Motion Rate:", L"Sets the motion rate (fps) for the Frame Rate Manager component.\nLarger values should be smoother, but more CPU intensive and possibly buggier.", 300, 1, INT_MAX),

                make_section("Loader"),
                make_bool(L"custom_patches", PATCHES, CONFIG, L"Enable Custom Patches", L"Enables all custom patches.", true, false),

                make_section("Launcher"),
                make_choice(L"button_prompts", LAUNCHER, CONFIG, L"Button prompts:", L"Which controller's buttons the launcher shows. Steam Deck and Xbox pads use A B X Y.", 0,
                    { L"Xbox (A B X Y)", L"PlayStation (\x00D7 \x25CB \x25A1 \x25B3)" }),
                make_choice(L"renderer", LAUNCHER, CONFIG, L"Launcher drawing:", L"How the launcher itself is drawn (takes effect the next time it opens).\nAutomatic uses the CPU under WinlatorXR, where the GPU paths make the game crash after the launcher,\nand the GPU everywhere else.", 0,
                    { L"Automatic", L"Direct3D 11", L"OpenGL", L"CPU (software)" }),
                make_bool(L"no_gpu_dialog", LAUNCHER, CONFIG, L"Disable GPU Warning", L"Disables the warning dialog for unsupported GPUs.", false, false),
                make_string(L"command_line", LAUNCHER, CONFIG, L"Command Line:", L"Allows setting command line parameters for the game when using the launcher.\nDisabling the launcher will bypass this.", L"", false),
                make_bool(L"use_divahook_bat", LAUNCHER, CONFIG, L"Use divahook.bat/start.bat", L"Launches divahook.bat/start.bat intead of diva.exe.", false, false),
                make_bool(L"skip", LAUNCHER, CONFIG, L"Skip launcher", L"Start the game directly next time. To get the launcher back, set skip=0 in the [launcher] section of plugins\\config.ini.", false, false),
                make_bool(L"overlay", LAUNCHER, CONFIG, L"In-game menu", L"Start the game directly and open these settings over it with F10, or View and Start together on a controller.\nRestart from the menu to apply changes. Needs DivaLinux.dva; without it, the launcher opens as usual.", false, false),
            };
            pages.push_back(pg);
        }

        // Player --------------------------------------------------------
        {
            Page pg;
            pg.title = "Player";
            const std::wstring& PD = p.playerdata;
            pg.options = {
                make_string(L"player_name", PLAYERDATA, PD, L"Player Name:", L"Player name shown in game.", L"\xFF2E\xFF2F\xFF0D\xFF2E\xFF21\xFF2D\xFF25", true),
                make_string(L"level_name", PLAYERDATA, PD, L"Level Name:", L"Level (plate) name shown in game.", L"\x5FD8\x308C\x306A\x3044\x3067\x306D\x79C1\x306E\x58F0\x3092", true),
                make_int(L"level_plate_id", PLAYERDATA, PD, L"Level Plate:", L"Sets the level background image (plate).", 0, 0, INT_MAX),
                make_int(L"level_plate_effect", PLAYERDATA, PD, L"Level Plate Effect:", L"Sets the effect on the level background image (plate).", -1, -1, 2),
                make_int(L"skin_equip", PLAYERDATA, PD, L"Skin:", L"Sets the gameplay UI skin.", 0, 0, INT_MAX),
                make_section("Sounds"),
                make_int(L"btn_se_equip", PLAYERDATA, PD, L"Button Sound:", L"Sets the sound effect for buttons.\n-1 = song default", -1, -1, INT_MAX),
                make_int(L"slide_se_equip", PLAYERDATA, PD, L"Slide Sound:", L"Sets the sound effect for slides.\n-1 = song default", -1, -1, INT_MAX),
                make_int(L"chainslide_se_equip", PLAYERDATA, PD, L"Chainslide Sound:", L"Sets the sound effect for chain slides.\n-1 = song default", -1, -1, INT_MAX),
                make_int(L"slidertouch_se_equip", PLAYERDATA, PD, L"Slider Touch Sound:", L"Sets the sound effect for touching the slider with no note.\n-1 = song default", -1, -1, INT_MAX),
                make_bool(L"act_toggle", PLAYERDATA, PD, L"Button SE", L"Enables button/slider sounds.", true, true),
                make_section("Progress bar"),
                make_bool(L"border_great", PLAYERDATA, PD, L"Clear Border (Great)", L"Shows the clear border for a great rating on the progress bar.", true, true),
                make_bool(L"border_excellent", PLAYERDATA, PD, L"Clear Border (Excellent)", L"Shows the clear border for an excellent rating on the progress bar.", true, true),
                make_bool(L"border_rival", PLAYERDATA, PD, L"Clear Border (Rival)", L"Shows the clear border for beating your rival on the progress bar.", false, true),
                make_section("Modules and modifiers"),
                make_bool(L"use_card", PLAYERDATA, PD, L"Use Card", L"Enables IC card. This allows module selection.", true, true),
                make_bool(L"module_card_workaround", PLAYERDATA, PD, L"Fast Module Selection (buggy)", L"Allows module selection without card and tries to improve menu performance.\nKnown to cause issues, especially with song-specific modules.", false, true),
                make_bool(L"use_pv_module_equip", PLAYERDATA, PD, L"Song Specific Modules", L"Allows song-specific module selection.", true, true),
                make_bool(L"use_pv_skin_equip", PLAYERDATA, PD, L"Song Specific Skins", L"Allows song-specific skin settings.\nValues stored in skins.ini", true, true),
                make_bool(L"use_pv_sfx_equip", PLAYERDATA, PD, L"Song Specific Sound Effects", L"Allows song-specific sound effect settings.\nValues stored in sfx.ini", true, true),
                make_bool(L"gamemode_options", PLAYERDATA, PD, L"Game Modifiers", L"Allows use of game mode modifiers (hi-speed, hidden, and sudden).", true, true),
            };
            pages.push_back(pg);
        }

        // Components ----------------------------------------------------
        {
            Page pg;
            pg.title = "Components";
            const std::wstring& C = p.components;
            pg.options = {
                make_bool(L"input_emulator", COMPONENTS, C, L"Input Emulator", L"Emulates input through keyboard and/or mouse.", false, true),
                make_bool(L"touch_slider_emulator", COMPONENTS, C, L"Slider Emulator", L"Emulates slider through keyboard and/or mouse.", false, true),
                make_bool(L"touch_panel_emulator", COMPONENTS, C, L"Touch Panel Emulator", L"Emulates touch panel through mouse.", false, true),
                make_bool(L"player_data_manager", COMPONENTS, C, L"Player Data Manager", L"Loads user-defined values into the PlayerData struct.\nRequired for modules and game mode modifiers.", false, true),
                make_bool(L"frame_rate_manager", COMPONENTS, C, L"Frame Rate Manager", L"Adjusts animations to the correct speed at different frame rates.\nOnly needed when FPS isn't locked at 60.", false, true),
                make_bool(L"fast_loader", COMPONENTS, C, L"Fast Loader", L"Skip or speed up unnecessary loading steps.", false, true),
                make_int(L"fast_loader_speed", COMPONENTS, C, L"Fast Loader Speed", L"Set this to 4 or less when playing online.\nDefault: 39", 39, 2, 1024),
                make_bool(L"camera_controller", COMPONENTS, C, L"Camera Controller", L"Enables freecam (toggled using F3).\nWASD to move, SPACE/CTRL for up/down, Q/R to rotate, R/F for zoom.\nHolding SHIFT/ALT changes control speed.", false, true),
                make_bool(L"scale_component", COMPONENTS, C, L"Scale Component", L"Scales the graphics output framebuffer to fill the screen/window.", false, true),
                make_bool(L"debug_component", COMPONENTS, C, L"Debug Component", L"Allows for changing game state (F4-F8 keys), using dev GUI and tests, and speeding up 2d animations/menus (hold SHIFT+TAB).", false, true),
                make_bool(L"target_inspector", COMPONENTS, C, L"Target Inspector", L"Enables hold transfers.", false, true),
                make_bool(L"score_saver", COMPONENTS, C, L"Score Saver", L"Saves high scores to plugins/scores.ini.", false, true),
                make_bool(L"pause", COMPONENTS, C, L"Pause", L"Adds a pause menu.", false, true),
            };
            pages.push_back(pg);
        }

        return pages;
    }
}
