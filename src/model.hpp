// Settings model: the option kinds PD Launcher and its plugins use (ConfigOption.h /
// PluginConfigApi.h), with load/save semantics matching the original launcher.
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace lfs {
    enum class Kind {
        Section,      // heading inside a page (OptionMetaSectionLabel / group start)
        Bool,
        Int,
        String,
        Choice,       // dropdown storing an index (DropdownOption)
        ChoiceText,   // dropdown storing text (DropdownTextOption)
        ChoiceNumber, // dropdown storing a number (DropdownNumberOption)
        Resolution,   // WxH stored as two keys (ResolutionOption)
        Button,       // calls a plugin function
        Plugin,       // enable toggle for a .dva or custom patch, with nested options
    };

    enum ResOpts : int {
        RES_NONE = 0,
        RES_MATCH_WINDOW = 1,
        RES_MATCH_SCREEN = 2,
        RES_DEFAULT = 4,
    };

    struct Res {
        int w, h;
        bool operator==(const Res& o) const { return w == o.w && h == o.h; }
    };

    struct Option {
        Kind kind = Kind::Section;
        std::wstring key, key2, section, file;
        std::string label;   // UTF-8, already without a trailing ':'
        std::string help;    // UTF-8

        // Defaults / constraints
        bool def_bool = false;
        bool save_as_string = false;
        int def_int = 0, min_int = 0, max_int = 0;
        std::wstring def_str;
        bool utf8 = false;
        std::vector<std::string> items;  // Choice / ChoiceText labels
        std::vector<std::wstring> items_w;
        int index_offset = 0;
        std::vector<int> ints;           // ChoiceNumber
        std::vector<Res> resolutions;    // Resolution
        Res def_res = { 0, 0 };
        int res_opts = RES_NONE;
        bool editable = false;
        void (*func)() = nullptr;        // Button

        // Plugin
        std::vector<std::shared_ptr<Option>> children;
        std::string version;

        // Current value
        bool b = false;
        int i = 0;
        std::wstring s;
        Res r = { 0, 0 };
        bool dirty = false;

        // Optional: whether the row is currently usable, and why not.
        std::function<bool()> enabled;
        std::string disabled_hint;

        void load();
        void save();                  // writes only if dirty (children too)
        bool any_dirty() const;

        // Presentation helpers
        bool cyclable() const;        // left/right changes the value
        std::string value_text() const;
        int choice_count() const;     // entries in the picker list (incl. Match/Default)
        int choice_index() const;     // current entry, -1 if a custom value
        std::string choice_label(int idx) const;
        void set_choice(int idx);
        void step(int dir, int amount = 1);
        bool is_enabled() const { return !enabled || enabled(); }
        std::string res_special_label() const;
    };

    using OptionPtr = std::shared_ptr<Option>;

    enum class PageKind { Home, Settings, Plugins, Patches };

    struct Page {
        std::string title;
        PageKind kind = PageKind::Settings;
        std::vector<OptionPtr> options;
        std::string empty_text;
    };

    struct Paths {
        std::wstring game_dir;
        std::wstring plugins_dir;
        std::wstring patches_dir;
        std::wstring config;      // plugins\config.ini
        std::wstring components;  // plugins\components.ini
        std::wstring playerdata;  // plugins\playerdata.ini
        std::wstring keyconfig;   // plugins\keyconfig.ini
    };

    struct ScreenInfo {
        std::vector<Res> resolutions;  // sorted, unique, includes 1280x720
        std::vector<int> refresh_rates;
    };

    // Builds the launcher's own pages (Display, Graphics, Options, Player, Components).
    // Plugins and Patches pages are filled by the caller.
    std::vector<Page> build_builtin_pages(const Paths& paths, const ScreenInfo& screen);

    // Constructors used by both built-in pages and plugin conversion.
    OptionPtr make_section(const std::string& label);
    OptionPtr make_bool(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, bool def, bool as_string);
    OptionPtr make_int(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, int def, int min, int max);
    OptionPtr make_string(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, const std::wstring& def, bool utf8);
    OptionPtr make_choice(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, int def, const std::vector<std::wstring>& items, int index_offset = 0);
    OptionPtr make_choice_text(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, const std::wstring& def, const std::vector<std::wstring>& items, bool editable, bool utf8);
    OptionPtr make_choice_number(const wchar_t* key, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, int def, const std::vector<int>& ints, bool editable);
    OptionPtr make_resolution(const wchar_t* key, const wchar_t* key2, const wchar_t* section, const std::wstring& file, const std::wstring& label, const std::wstring& help, Res def, const std::vector<Res>& list, bool editable, int opts);
    OptionPtr make_button(const std::wstring& label, const std::wstring& help, void (*func)());
    OptionPtr make_plugin(const std::wstring& file_name, const std::wstring& config, const std::wstring& name, const std::wstring& help, const std::string& version, std::vector<OptionPtr> children);

    std::string clean_label(const std::wstring& w);
}
