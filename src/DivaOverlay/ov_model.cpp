#include "ov_model.hpp"
#include "ini.hpp"   // LauncherFS
#include "ov_platform.hpp"
#include <algorithm>
#include <climits>
#include <cstdio>

using lfs::OptionPtr;

namespace dov {
    // ------------------------------------------------------------ Item

    std::string Item::value_text() const {
        if (opt)
            return opt->value_text();
        if (env)
            return env->index >= 0 && env->index < (int)env->labels.size() ? env->labels[env->index] : env->values[env->index];
        return "";
    }

    bool Item::steppable() const {
        if (opt) return opt->cyclable();
        if (env) return env->values.size() > 1;
        return false;
    }

    bool Item::step(int dir, int amount) {
        if (!is_enabled())
            return false;
        bool changed = false;
        if (opt) {
            const std::string before = opt->value_text();
            if (opt->kind == lfs::Kind::Bool || opt->kind == lfs::Kind::Plugin) {
                // left = off, right = on (a toggle in both directions would be confusing)
                const bool want = dir > 0;
                if (opt->b != want) { opt->b = want; opt->dirty = true; }
            }
            else
                opt->step(dir, amount);
            changed = opt->value_text() != before;
        }
        else if (env) {
            const int n = (int)env->values.size();
            const int next = std::max(0, std::min(n - 1, env->index + dir));
            changed = next != env->index;
            env->index = next;
        }
        if (changed && live && apply)
            apply(*this);
        return changed;
    }

    bool Item::activate() {
        if (!is_enabled())
            return false;
        bool changed = false;
        if (opt) {
            const std::string before = opt->value_text();
            switch (opt->kind) {
            case lfs::Kind::Bool:
            case lfs::Kind::Plugin:
                opt->b = !opt->b;
                opt->dirty = true;
                break;
            case lfs::Kind::Int:
                opt->step(1, 1);
                break;
            default: {
                // choices wrap around on confirm
                const int n = opt->choice_count();
                if (n > 1) {
                    const int cur = opt->choice_index();
                    opt->set_choice(cur < 0 || cur + 1 >= n ? 0 : cur + 1);
                }
            } break;
            }
            changed = opt->value_text() != before;
        }
        else if (env) {
            const int n = (int)env->values.size();
            env->index = env->index + 1 >= n ? 0 : env->index + 1;
            changed = n > 1;
        }
        if (changed && live && apply)
            apply(*this);
        return changed;
    }

    bool Item::dirty() const {
        if (opt) return opt->any_dirty();
        if (env) return env->index != env->loaded;
        return false;
    }

    void Item::load() {
        if (opt)
            opt->load();
        else if (env) {
            std::string v;
            env->index = 0;
            if (env->file->get(env->key, &v) == EnvFile::State::On) {
                env->index = -1;
                for (size_t k = 1; k < env->values.size(); k++)
                    if (env->values[k] == v) { env->index = (int)k; break; }
                if (env->index < 0) {
                    // a value the list doesn't have: keep it as its own entry
                    env->values.push_back(v);
                    env->labels.push_back(v);
                    env->index = (int)env->values.size() - 1;
                }
            }
            env->loaded = env->index;
        }
        if (!has_baseline) {
            baseline = value_text();
            has_baseline = true;
        }
    }

    void Item::save() {
        if (opt)
            opt->save();
        else if (env && env->index != env->loaded) {
            if (env->index <= 0)
                env->file->unset(env->key);
            else
                env->file->set(env->key, env->values[env->index]);
            env->loaded = env->index;
        }
    }

    void Section::collect(std::vector<ItemPtr>& out) const {
        for (const ItemPtr& it : items) {
            if (it->type == Item::Type::Setting)
                out.push_back(it);
            else if (it->type == Item::Type::Section && it->section)
                it->section->collect(out);
        }
    }

    // ------------------------------------------------------------ building

    static ItemPtr header(const std::string& label) {
        auto it = std::make_shared<Item>();
        it->type = Item::Type::Header;
        it->label = label;
        return it;
    }

    static std::string narrow(const std::wstring& w) { return lfs::to_utf8(w); }

    static std::string file_name(const std::wstring& path) {
        size_t s = path.find_last_of(L"\\/");
        return narrow(s == std::wstring::npos ? path : path.substr(s + 1));
    }

    static ItemPtr from_option(const OptionPtr& o) {
        if (o->kind == lfs::Kind::Section)
            return header(o->label);
        auto it = std::make_shared<Item>();
        it->label = o->label;
        it->help = o->help;
        it->opt = o;
        it->enabled = o->enabled;
        it->disabled_hint = o->disabled_hint;
        std::string keys = narrow(o->key);
        if (!o->key2.empty())
            keys += ", " + narrow(o->key2);
        it->where = file_name(o->file) + "  [" + narrow(o->section) + "] " + keys;
        return it;
    }

    static ItemPtr section_item(const SectionPtr& s) {
        auto it = std::make_shared<Item>();
        it->type = Item::Type::Section;
        it->label = s->title;
        it->help = s->subtitle;
        it->section = s;
        return it;
    }

    // A LauncherFS page as a section. Long pages with headings get a button per heading
    // (sub-sections); short ones keep the headings as dividers.
    static SectionPtr from_page(const lfs::Page& pg, const std::string& group, const std::string& subtitle,
        const std::function<bool(const lfs::Option&)>& keep) {
        auto s = std::make_shared<Section>();
        s->title = pg.title;
        s->group = group;
        s->subtitle = subtitle;
        std::vector<OptionPtr> opts;
        for (const OptionPtr& o : pg.options)
            if (o->kind == lfs::Kind::Section || ((o->kind != lfs::Kind::Button && o->kind != lfs::Kind::String) && (!keep || keep(*o))))
                opts.push_back(o);
        // drop headings with nothing under them
        std::vector<OptionPtr> clean;
        for (size_t i = 0; i < opts.size(); i++) {
            if (opts[i]->kind == lfs::Kind::Section && (i + 1 >= opts.size() || opts[i + 1]->kind == lfs::Kind::Section))
                continue;
            clean.push_back(opts[i]);
        }
        size_t settings = 0, headings = 0;
        for (auto& o : clean) (o->kind == lfs::Kind::Section ? headings : settings)++;
        const bool split = headings >= 2 && settings > 12;
        if (!split) {
            for (auto& o : clean)
                s->items.push_back(from_option(o));
            return s;
        }
        SectionPtr sub;
        for (auto& o : clean) {
            if (o->kind == lfs::Kind::Section) {
                sub = std::make_shared<Section>();
                sub->title = o->label;
                sub->group = group;
                s->items.push_back(section_item(sub));
                continue;
            }
            (sub ? sub->items : s->items).push_back(from_option(o));
        }
        for (auto& it : s->items)
            if (it->type == Item::Type::Section) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%d settings", (int)it->section->items.size());
                it->help = buf;
            }
        return s;
    }

    // ---- DivaLinux.ini

    static void set_live(const ItemPtr& it, const LiveHooks& hooks, const char* option_name) {
        it->live = (bool)hooks.set_option_int;
        const std::string name = option_name;
        auto set = hooks.set_option_int;
        it->apply = [set, name](Item& self) {
            if (!set || !self.opt) return;
            int v = 0;
            switch (self.opt->kind) {
            case lfs::Kind::Bool: v = self.opt->b ? 1 : 0; break;
            default: v = self.opt->i; break;
            }
            set(name.c_str(), v);
        };
    }

    static std::vector<SectionPtr> divalinux_sections(const std::wstring& ini, bool recomp, bool xr, const LiveHooks& hooks) {
        std::vector<SectionPtr> out;
        auto add = [&](const char* title, const char* subtitle, std::vector<ItemPtr> items) {
            auto s = std::make_shared<Section>();
            s->title = title;
            s->group = "DivaLinux";
            s->subtitle = subtitle;
            s->items = std::move(items);
            out.push_back(s);
        };
        auto opt = [&](OptionPtr o) { return from_option(o); };

        // Stereo 3D -------------------------------------------------------
        {
            auto mode = opt(lfs::make_choice(L"stereo_mode", L"stereo", ini, L"Stereo 3D",
                L"Side-by-side 3D: each eye gets its own picture, for 3D displays, glasses and headsets.\n\n"
                L"Half SBS squeezes each eye into half the screen (the display stretches it back); "
                L"Full SBS keeps each eye's shape. In VR, restart after changing this.",
                0, { L"Off", L"Half SBS", L"Full SBS" }));
            set_live(mode, hooks, "stereo_mode");
            auto on = [o = mode->opt]() { return o->i != 0; };
            const char* off_hint = "Turn on Stereo 3D to use this.";
            auto sep = opt(lfs::make_int(L"stereo_separation", L"stereo", ini, L"Eye separation (mm)",
                L"How far apart the two eyes are, in millimetres. Larger is a stronger 3D effect.\n\nDefault: 65", 65, 0, 500));
            set_live(sep, hooks, "stereo_separation");
            auto conv = opt(lfs::make_int(L"stereo_convergence", L"stereo", ini, L"Screen distance (cm)",
                L"Distance of the screen plane: things this far from the camera sit on the screen, nearer ones pop out.\n\n"
                L"0 follows the camera's point of interest (usually the singer).", 0, 0, 100000));
            set_live(conv, hooks, "stereo_convergence");
            auto hud = opt(lfs::make_int(L"stereo_hud_parallax", L"stereo", ini, L"HUD depth",
                L"Where the HUD floats: 0 is on the screen, below 0 in front of it, above 0 behind it "
                L"(in 0.1% of the picture's width).", 0, -50, 50));
            set_live(hud, hooks, "stereo_hud_parallax");
            auto maxp = opt(lfs::make_int(L"stereo_max_parallax", L"stereo", ini, L"Depth limit",
                L"The most the two eyes' pictures may differ, in 0.1% of the picture's width, so close-ups stay comfortable. 0 = no limit.\n\nDefault: 20", 20, 0, 200));
            set_live(maxp, hooks, "stereo_max_parallax");
            auto swap = opt(lfs::make_bool(L"stereo_swap_eyes", L"stereo", ini, L"Swap eyes",
                L"Right eye on the left: for cross-eyed viewing, or a display that expects that order.", false, false));
            set_live(swap, hooks, "stereo_swap_eyes");
            auto bg = opt(lfs::make_bool(L"stereo_bg_infinity", L"stereo", ini, L"Backgrounds at infinity",
                L"2D stage backgrounds and PV movies sit far behind the 3D scene instead of on the screen.", true, false));
            set_live(bg, hooks, "stereo_bg_infinity");
            auto shadow = opt(lfs::make_bool(L"stereo_shadow_once", L"stereo", ini, L"Shadows once per frame",
                L"Renders shadows for the first eye only and reuses them for the second: faster, and they look the same.", true, false));
            set_live(shadow, hooks, "stereo_shadow_once");
            auto aspect = opt(lfs::make_choice_number(L"stereo_content_aspect", L"stereo", ini, L"Picture shape (x1000)",
                L"The game picture's aspect ratio x1000: 1778 is 16:9. On screens of another shape, only this part goes into each eye.\n0 uses the whole screen.",
                1778, { 0, 1333, 1600, 1778, 2000, 2333 }, true));
            set_live(aspect, hooks, "stereo_content_aspect");
            for (auto& it : { sep, conv, hud, maxp, swap, bg, shadow, aspect }) {
                it->enabled = on;
                it->disabled_hint = off_hint;
            }
            add("Stereo 3D", "Side-by-side 3D, depth, HUD depth", { mode, header("Depth"), sep, conv, maxp, header("Picture"), hud, bg, swap, shadow, aspect });
        }

        // VR (not on Android without a headset: the flat build has no OpenXR) ----
        if (!recomp || xr) {
            auto en = opt(lfs::make_bool(L"enabled", L"vr", ini, L"VR",
                recomp ? L"Show the game in the headset as a virtual screen (on by default in the Quest build)."
                       : L"Show the game in a headset through OpenXR, as a virtual screen.", recomp && xr, false));
            auto hud = opt(lfs::make_bool(L"hud_layer", L"vr", ini, L"HUD as its own layer",
                L"The HUD floats a little in front of the picture as its own layer, so it stays sharp.", false, false));
            auto w = opt(lfs::make_int(L"screen_width_cm", L"vr", ini, L"Screen width (cm)", L"Size of the virtual screen.\n\nDefault: 300", 300, 50, 2000));
            auto d = opt(lfs::make_int(L"screen_distance_cm", L"vr", ini, L"Screen distance (cm)", L"How far away the virtual screen is.\n\nDefault: 250", 250, 50, 2000));
            auto h = opt(lfs::make_int(L"screen_height_cm", L"vr", ini, L"Screen height (cm)", L"Height of the screen's centre above eye level (0 = at eye level).", 0, -500, 500));
            add("VR", "Headset screen, HUD layer", { en, hud, header("Virtual screen"), w, d, h });
        }

        // Movies ----------------------------------------------------------
        add("Movies", "PV movie playback (FFmpeg)", {
            opt(lfs::make_bool(L"ffmpeg_player", L"movie", ini, L"FFmpeg movie player", L"Plays PV movies (MP4) with FFmpeg instead of the game's player.", true, false)),
            opt(lfs::make_bool(L"movie_audio", L"movie", ini, L"Movie audio", L"Plays the movie file's own audio track too.", false, false)),
            opt(lfs::make_bool(L"gpu_convert", L"movie", ini, L"Colour conversion on the GPU", L"Converts movie frames to RGB on the GPU (faster).", true, false)),
            opt(lfs::make_bool(L"fast_decode", L"movie", ini, L"Fast decode", L"Lets FFmpeg skip some decoding work: faster, slightly lower quality.", false, false)),
            opt(lfs::make_choice_number(L"decode_threads", L"movie", ini, L"Decode threads", L"Threads for decoding movies. 0 = automatic.", 0, { 0, 1, 2, 4, 6, 8, 12, 16 }, true)),
        });

        // Audio -----------------------------------------------------------
        {
            auto room = opt(lfs::make_choice(L"headphones", L"audio", ini, L"Headphone surround",
                L"Plays the game's sound as if from two speakers in a room, for headphones.\n\n"
                L"Needs a room filter DLL in plugins: DolbyHph.dll (from your own copy), or LakeHP_OpenRooms.dll, "
                L"which comes with DivaLinux. Adds about 3 ms of delay.", 0,
                { L"Off", L"Small room", L"Medium room", L"Large room" }));
            auto filters = opt(lfs::make_choice_text(L"filters", L"audio", ini, L"Room filters",
                L"The DLL in plugins the rooms come from.\n\nauto: DolbyHph.dll when it's there, else LakeHP_OpenRooms.dll.",
                L"auto", { L"auto", L"DolbyHph.dll", L"LakeHP_OpenRooms.dll" }, false, false));
            std::vector<ItemPtr> items = { room, filters };
            for (auto& it : items) {
                it->live = (bool)hooks.reload_audio;
                auto reload = hooks.reload_audio;
                it->apply = [reload](Item& self) {
                    if (!reload) return;
                    self.save();     // the audio hook reads DivaLinux.ini
                    reload();
                };
            }
            add("Audio", "Headphone surround", items);
        }

        // Pointer ---------------------------------------------------------
        {
            auto ptr = opt(lfs::make_choice(L"pointer", L"cursor", ini, L"Pointer",
                L"The mouse pointer drawn into the picture (where the game sees the mouse, in both eyes in 3D).", 1,
                { L"Windows cursor", L"Leek", L"Crosshair" }));
            auto hide = opt(lfs::make_int(L"hide_after", L"cursor", ini, L"Hide after (s)", L"Seconds without moving before the pointer fades out. 0 = never.", 2, 0, 60));
            auto size = opt(lfs::make_choice_number(L"size", L"cursor", ini, L"Size (%)", L"Pointer size.", 100, { 50, 75, 100, 125, 150, 200, 300 }, true));
            auto col = opt(lfs::make_choice_text(L"color", L"cursor", ini, L"Colour", L"Pointer colour (hex RGB).", L"39C5BB",
                { L"39C5BB", L"FFFFFF", L"FF7AA2", L"FFD23F", L"7FD1FF", L"9B6BFF" }, true, false));
            std::vector<ItemPtr> items = { ptr, hide, size, col };
            for (auto& it : items) {
                it->live = (bool)hooks.reload_cursor;
                auto reload = hooks.reload_cursor;
                it->apply = [reload](Item& self) {
                    if (!reload) return;
                    self.save();     // the pointer reads DivaLinux.ini
                    reload();
                };
            }
            add("Pointer", "The leek, its size and colour", items);
        }

        // Graphics --------------------------------------------------------
        {
            auto full = opt(lfs::make_bool(L"reflect_full", L"general", ini, L"Full reflections", L"Reflections at full quality.", false, false));
            set_live(full, hooks, "reflect_full");
            auto scale = opt(lfs::make_choice_number(L"reflect_res_scale", L"general", ini, L"Reflection resolution (%)", L"Reflection buffer size. Lower is faster.\n\nDefault: 100", 100, { 25, 50, 75, 100 }, true));
            set_live(scale, hooks, "reflect_res_scale");
            add("Graphics", "Reflections", { full, scale });
        }

        // Performance -----------------------------------------------------
        add("Performance", "PV frame skip, frame-time log", {
            opt(lfs::make_choice(L"pv_frame_skip", L"performance", ini, L"PV frame skip",
                L"Draws the 3D PV less often than the HUD when frames run late, so notes and judging stay smooth.\n\n"
                L"Auto: only while frames run late. 2-4: every Nth frame.", 1, { L"Off", L"Auto", L"Every 2nd", L"Every 3rd", L"Every 4th" })),
            opt(lfs::make_bool(L"perf_log", L"performance", ini, L"Frame-time log", L"A frame-time line in DivaLinux.log every 10 seconds.", true, false)),
        });

        // Loading ---------------------------------------------------------
        add("Loading", "Archives, lookup cache", {
            opt(lfs::make_bool(L"archives", L"loading", ini, L"ZIP archives", L"Reads rom and mdata folders from ZIP archives (rom.zip, mdata\\MXXX.zip).", true, false)),
            opt(lfs::make_bool(L"lookup_cache", L"loading", ini, L"Lookup cache", L"Remembers where files are, so loading checks fewer places.", true, false)),
        });

        // Experimental ----------------------------------------------------
        add("Experimental", "Renderer experiments", {
            opt(lfs::make_bool(L"shared_storage_uniform_buffer", L"experimentals", ini, L"Shared storage uniform buffer", L"Experimental renderer option.", true, false)),
            opt(lfs::make_bool(L"texture_skinning_buffer", L"experimentals", ini, L"Texture skinning buffer", L"Experimental renderer option.", false, false)),
            opt(lfs::make_bool(L"task_movie_player_no_interop", L"experimentals", ini, L"Movie player without interop", L"Experimental renderer option.", false, false)),
            opt(lfs::make_bool(L"io_trace", L"debug", ini, L"File access log", L"Logs every file the game opens (debugging).", false, false)),
        });
        return out;
    }

    // ---- divarecomp_env.txt

    struct EnvSpec {
        const char* key;
        const char* label;
        const char* help;
        const char* def;   // what "Default" means
        std::vector<std::pair<const char*, const char*>> values;   // value, label
    };

    static ItemPtr env_item(EnvFile* f, const EnvSpec& s) {
        auto it = std::make_shared<Item>();
        it->label = s.label;
        it->help = s.help;
        it->env = std::make_unique<Item::Env>();
        it->env->file = f;
        it->env->key = s.key;
        it->env->values.push_back("");
        it->env->labels.push_back(std::string("Default (") + s.def + ")");
        for (auto& v : s.values) {
            it->env->values.push_back(v.first);
            it->env->labels.push_back(v.second);
        }
        it->where = std::string("divarecomp_env.txt  ") + s.key;
        return it;
    }

    static const std::vector<std::pair<const char*, const char*>> ON_OFF = { { "1", "On" }, { "0", "Off" } };

    static std::vector<SectionPtr> env_sections(EnvFile* f, const std::wstring& game_dir, bool xr) {
        std::vector<SectionPtr> out;
        auto add = [&](const char* title, const char* subtitle, const std::vector<EnvSpec>& specs) {
            auto s = std::make_shared<Section>();
            s->title = title;
            s->group = "Android";
            s->subtitle = subtitle;
            for (auto& sp : specs)
                s->items.push_back(env_item(f, sp));
            out.push_back(s);
        };

        // custom Vulkan drivers: folders in <game>\drivers
        std::vector<std::pair<const char*, const char*>> drivers;
        static std::vector<std::string> s_driver_names;
        s_driver_names.clear();
        for (const std::wstring& d : list_dirs(game_dir + L"\\drivers"))
            s_driver_names.push_back(lfs::to_utf8(d));
        if (s_driver_names.empty())
            s_driver_names.push_back("turnip");
        for (const std::string& n : s_driver_names)
            drivers.push_back({ n.c_str(), n.c_str() });

        add("Graphics", "Vulkan driver, depth of field, presentation", {
            { "DIVA_VULKAN_DRIVER", "Vulkan driver", "A custom Vulkan driver from the game folder's drivers\\<name>\\ (meta.json and the .so), such as Mesa Turnip on Adreno.", "the system's", drivers },
            { "DIVA_DOF", "Depth of field", "The PV camera's background blur.", "on", { { "0", "Off" }, { "1", "On" } } },
            { "DIVAVK_PRESENT_MODE", "Present mode", "How finished frames reach the screen.", "mailbox", { { "fifo", "FIFO (vsync)" }, { "mailbox", "Mailbox" }, { "immediate", "Immediate" } } },
            { "DIVAVK_HUD_DEPTH_CM", "VR HUD depth (cm)", "With the VR HUD layer on: how far in front of the picture the HUD floats.", "10", { { "0", "0" }, { "5", "5" }, { "10", "10" }, { "20", "20" }, { "40", "40" } } },
        });
        add("PV and timing", "PV threading, pacing, loading", {
            { "DIVA_PV_CTRL_EVERY_FRAME", "PV updates every frame", "PV tasks (faces, hands) update every frame; their draw lists follow the PV's schedule.", "on", { { "1", "On" }, { "0", "Off" } } },
            { "DIVA_PV_THROTTLE", "PV frame skip", "Splits the PV over two frames when frames run late. Overrides DivaLinux.ini's PV frame skip.", "DivaLinux.ini", { { "0", "Off" }, { "1", "Auto" }, { "2", "Always" } } },
            { "DIVA_PV_ASYNC_OFF", "No threaded PV", "Fallback: the PV is drawn on the game thread, split over two frames, instead of on a thread of its own.", "off", { { "1", "On" } } },
            { "DIVA_PV_ROB_SCALE", "Motion step scaling", "Scales character motion steps when the PV updates less often.", "on", ON_OFF },
            { "DIVA_FRAME_PACING", "Frame pacing", "The game waits for the headset's frame time instead of vsync slots.", "with mailbox", ON_OFF },
            { "DIVA_FRAME_RATE_MANAGER", "Frame rate manager", "Character motion follows real time at any frame rate. Overrides components.ini.", "components.ini", ON_OFF },
            { "DIVA_FAST_LOADER", "Fast loader", "Skips data init and the warning screen. Overrides components.ini.", "components.ini", ON_OFF },
            { "DIVA_FAST_LOADER_SPEED", "Fast loader speed", "Loading steps per frame with the fast loader.", "components.ini", { { "4", "4" }, { "12", "12" }, { "39", "39" }, { "100", "100" } } },
        });
        if (xr) add("Headset", "Passthrough, refresh rate, pointer", {
            { "DIVA_PASSTHROUGH", "Passthrough", "Your room around the screen, or a black void.", "on", { { "0", "Off" }, { "1", "On" } } },
            { "DIVA_XR_REFRESH", "Refresh rate (Hz)", "The headset's display rate.", "the headset's", { { "72", "72" }, { "80", "80" }, { "90", "90" }, { "120", "120" } } },
            { "DIVA_XR_POINTER", "Controller pointer", "Point at the screen with the controllers to click and touch.", "on", { { "0", "Off" }, { "1", "On" } } },
            { "DIVA_SCREEN_GESTURE", "Screen grab gesture", "Hold both grips to move and zoom the screen.", "on", { { "0", "Off" }, { "1", "On" } } },
            { "DIVA_WINDOW_PARALLAX", "Window parallax", "The camera follows your head a little, like looking through a window.", "1.0", { { "0", "Off" }, { "0.5", "0.5" }, { "1", "1.0" }, { "1.5", "1.5" }, { "2", "2.0" } } },
        });
        add("Controllers", "Divaller, Bluetooth, USB, pause", {
            { "DIVA_DIVALLER", "Slider controllers", "Divaller and other slider controllers (USB, Bluetooth, network bridge).", "on", ON_OFF },
            { "DIVA_USB_CONTROLLERS", "USB controllers", "Slider controllers plugged in over USB.", "on", ON_OFF },
            { "DIVA_BLE", "Bluetooth bridge", "The DIVA Bridge ESP32 over Bluetooth.", "on", ON_OFF },
            { "DIVA_PAUSE", "Pause menu", "Start during a song opens the pause menu.", "on", ON_OFF },
        });
        add("Debugging", "Logs, frame dumps, tracing", {
            { "DIVAVK_DUMP_DRAWS", "Dump every draw", "Frame dumps save the target after every draw, not just every pass.", "off", { { "1", "On" } } },
            { "DIVA_TASK_PROFILE", "Task profile", "The busiest game tasks in the log every 10 seconds.", "on", ON_OFF },
            { "DIVA_TRACE_FILES", "Trace files", "Logs file access.", "off", { { "1", "On" } } },
            { "DIVAVK_VALIDATION", "Vulkan validation", "Vulkan validation layers, if the APK has them.", "off", { { "1", "On" } } },
            { "DIVA_CPU_PIN", "Pin to big cores", "Keeps the game thread on the fast CPU cores.", "on", ON_OFF },
        });
        return out;
    }

    // ---- PD Loader (config.ini & co.)

    static std::vector<SectionPtr> game_sections(const lfs::Paths& p, bool recomp) {
        lfs::ScreenInfo screen;
        screen.resolutions = { {1280,720}, {1366,768}, {1600,900}, {1920,1080}, {1920,1200}, {2560,1080}, {2560,1440}, {3840,2160} };
        screen.refresh_rates = { 60, 72, 75, 90, 120, 144, 165, 240 };
        std::vector<lfs::Page> pages = lfs::build_builtin_pages(p, screen);
        std::vector<SectionPtr> out;

        // Under the runtime, only what it reads (pdloader_compat.c, player_data.c): the window and
        // internal resolution, the frame rate manager's motion rate, the fast loader, No Lyrics,
        // the songs per credit, the player data manager and score saver, and playerdata.ini
        // (all but module_card_workaround).
        std::function<bool(const lfs::Option&)> keep;
        if (recomp)
            keep = [](const lfs::Option& o) {
                static const wchar_t* used[] = { L"width", L"r.enable", L"r.width", L"frm.motion.rate",
                    L"fast_loader", L"fast_loader_speed", L"frame_rate_manager", L"no_lyrics", L"normal_stages", L"protected_stages",
                    L"player_data_manager", L"score_saver",
                    L"player_name", L"level_name", L"level_plate_id", L"level_plate_effect", L"skin_equip",
                    L"btn_se_equip", L"slide_se_equip", L"chainslide_se_equip", L"slidertouch_se_equip", L"act_toggle",
                    L"border_great", L"border_excellent", L"border_rival", L"use_card", L"use_pv_module_equip",
                    L"use_pv_skin_equip", L"use_pv_sfx_equip", L"gamemode_options", L"difficulty" };
                for (const wchar_t* k : used)
                    if (o.key == k)
                        return true;
                return false;
            };
        static const struct { const char* title; const char* subtitle; } names[] = {
            { "Display", "Window, resolution, internal resolution" },
            { "Graphics", "Anti-aliasing, effects, buffers" },
            { "Options", "Patches, controller, UI, frame rate" },
            { "Player", "Name, plate, sounds, modules" },
            { "Components", "PD Loader's TLAC components" },
        };
        for (const lfs::Page& pg : pages) {
            std::string sub;
            for (auto& n : names)
                if (pg.title == n.title)
                    sub = n.subtitle;
            SectionPtr s = from_page(pg, "Game", sub, keep);
            std::vector<ItemPtr> settings;
            s->collect(settings);
            if (!settings.empty())
                out.push_back(s);
        }
        return out;
    }

    // ---- DivaOnline.ini (src/DivaOnline): online play, when the plugin is there

    static std::vector<SectionPtr> online_sections(const lfs::Paths& p, bool recomp) {
        std::vector<SectionPtr> out;
        const std::wstring dva = p.plugins_dir + L"\\DivaOnline.dva";
        if (!file_exists(dva))
            return out;
        const std::wstring ini = p.plugins_dir + L"\\DivaOnline.ini";
        auto s = std::make_shared<Section>();
        s->title = "Online";
        s->group = "Game";
        s->subtitle = "DivaOnline: ALL.Net and the Aime card";
        // DivaOnline's own default is on; the DivaRecomp runtime starts it only when the
        // .ini says enable=1 (its default DivaOnline.ini says 0)
        s->items.push_back(from_option(lfs::make_bool(L"enable", L"divaonline", ini, L"Online play",
            L"Log in with your Aime card and play on the ARTEMiS server in DivaOnline.ini ([dns] default, [aime] accessCode).\n\n"
            L"Off: the game is offline, with playerdata.ini and scores.ini.", !recomp, false)));
        s->items.push_back(from_option(lfs::make_bool(L"autoInsert", L"aime", ini, L"Auto card tap",
            L"The card stays on the reader: no key to hold. For controller-only setups and headsets.", recomp, false)));
        s->items.push_back(from_option(lfs::make_bool(L"keep_tlac_player_data", L"divaonline", ini, L"Keep TLAC data",
            L"Online, your player data and scores come from the server, so playerdata.ini and scores.ini (TLAC's player data manager and score saver) are left alone. On keeps using them.", false, false)));
        out.push_back(s);
        return out;
    }

    // ------------------------------------------------------------ Model

    void Model::build(const ModelPaths& p, bool under_recomp, bool xr_on, const LiveHooks& hooks) {
        paths = p;
        recomp = under_recomp;
        xr = xr_on;
        sections.clear();
        all.clear();
        for (auto& s : divalinux_sections(p.divalinux_ini, recomp, xr, hooks))
            sections.push_back(s);
        for (auto& s : game_sections(p.pd, recomp))
            sections.push_back(s);
        for (auto& s : online_sections(p.pd, recomp))
            sections.push_back(s);
        if (recomp && !p.env_file.empty()) {
            env.set_path(p.env_file);
            for (auto& s : env_sections(&env, p.game_dir, xr))
                sections.push_back(s);
        }
        for (auto& s : sections)
            s->collect(all);
    }

    void Model::load_all() {
        lfs::ini_invalidate();
        if (!env.path().empty())
            env.load();
        for (auto& it : all)
            it->load();
    }

    bool Model::any_dirty() const {
        for (auto& it : all)
            if (it->dirty())
                return true;
        return false;
    }

    void Model::save_dirty() {
        bool env_changed = false;
        for (auto& it : all)
            if (it->dirty()) {
                if (it->env) env_changed = true;
                it->save();
            }
        if (env_changed)
            env.save();
    }

    int Model::pending_restart_count() const {
        int n = 0;
        for (auto& it : all)
            if (it->pending_restart())
                n++;
        return n;
    }
}
