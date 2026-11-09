// DivaOverlay's settings: what the menu lists and how each row reads and writes its value.
//
// Three sources, each a group of sections on the home page:
//   - DivaLinux (plugins\DivaLinux.ini): stereo 3D, VR, movies, the pointer, ... Some apply
//     straight away (DivaGL's SetOptionInt / the pointer's settings); the rest at the next start.
//   - Game (plugins\config.ini and PD Loader's other files): PD Launcher's own pages, from
//     LauncherFS (same keys, defaults and help). On Android, only what the runtime uses.
//   - Android (divarecomp_env.txt): the DivaRecomp runtime's switches, read before the game
//     starts. Only shown when running under the runtime.
#pragma once

#include "env_file.hpp"
#include "model.hpp"   // LauncherFS
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dov {
    struct Section;

    // One row: a heading, a setting, a section to open, or an action.
    struct Item {
        enum class Type { Header, Setting, Section, Action };
        Type type = Type::Setting;
        std::string label, help;
        std::string where;              // file and key, shown under the help text

        // Setting: an INI value (LauncherFS option) or a runtime switch (env)
        lfs::OptionPtr opt;
        struct Env {
            EnvFile* file = nullptr;
            std::string key;
            std::vector<std::string> values;   // [0] unused: "Default" (the switch off)
            std::vector<std::string> labels;
            int index = 0, loaded = 0;
        };
        std::unique_ptr<Env> env;

        bool live = false;                  // takes effect straight away
        std::function<void(Item&)> apply;   // live: called after each change
        std::function<bool()> enabled;
        std::string disabled_hint;

        std::shared_ptr<Section> section;   // Type::Section
        std::function<void()> action;       // Type::Action
        int accent = 0;                     // Action: 0 normal, 1 highlighted (pending restart)

        // The value the game started with (read when the menu first opened), to tell which
        // changes wait for a restart.
        std::string baseline;
        bool has_baseline = false;

        bool is_enabled() const { return !enabled || enabled(); }
        bool is_setting() const { return type == Type::Setting; }
        std::string value_text() const;
        bool steppable() const;
        // Moves the value (left/right). Returns true if it changed.
        bool step(int dir, int amount = 1);
        // Confirm on a setting: toggles a switch, otherwise steps forward (wrapping).
        bool activate();
        bool is_int() const { return opt && opt->kind == lfs::Kind::Int; }
        bool dirty() const;
        void load();
        void save();
        bool pending_restart() const { return is_setting() && !live && value_text() != baseline; }
    };
    using ItemPtr = std::shared_ptr<Item>;

    struct Section {
        std::string title;
        std::string group;     // "DivaLinux", "Game", "Android"
        std::string subtitle;  // a line under the title on its button
        std::vector<ItemPtr> items;
        // all settings inside, including nested sections
        void collect(std::vector<ItemPtr>& out) const;
    };
    using SectionPtr = std::shared_ptr<Section>;

    struct Host;   // ov_host: live hooks, restart

    struct ModelPaths {
        std::wstring game_dir;      // ends without a separator
        std::wstring divalinux_ini; // plugins\DivaLinux.ini
        lfs::Paths pd;              // config.ini, components.ini, ...
        std::wstring env_file;      // divarecomp_env.txt (empty: not under the runtime)
    };

    struct LiveHooks {
        // DivaGL's SetOptionInt (stereo, reflections); null in the preview
        std::function<bool(const char* name, int value)> set_option_int;
        // re-reads [cursor] from DivaLinux.ini
        std::function<void()> reload_cursor;
    };

    struct Model {
        ModelPaths paths;
        bool recomp = false;        // under the DivaRecomp runtime (Android)
        bool xr = false;            // showing in a headset (OpenXR)
        EnvFile env;
        std::vector<SectionPtr> sections;   // home page order, grouped
        std::vector<ItemPtr> all;           // every setting

        void build(const ModelPaths& p, bool under_recomp, bool xr_on, const LiveHooks& hooks);
        void load_all();            // re-reads every file (the menu opens)
        void save_dirty();          // writes changed values
        bool any_dirty() const;
        int pending_restart_count() const;
    };
}
