// divarecomp_env.txt: the DivaRecomp runtime's switches (Android builds), one KEY=VALUE per
// line, read before the game starts. A line starting with # is off, and " #" after a value
// starts a comment (runtime/src/android/platform_android.c, load_env_file).
//
// Edits keep the file as the user wrote it: a switch that's turned off is commented out
// rather than deleted, so its value is still there to turn back on, and a switch that's
// turned on reuses its commented-out line when there is one.
#pragma once

#include <string>
#include <vector>

namespace dov {
    class EnvFile {
    public:
        enum class State { Missing, Off, On };   // no line / only "#KEY=..." / "KEY=..."

        explicit EnvFile(std::wstring path = {}) : m_path(std::move(path)) {}

        const std::wstring& path() const { return m_path; }
        void set_path(std::wstring p) { m_path = std::move(p); m_lines.clear(); }

        bool load();            // a missing file is an empty one
        bool save() const;

        // The switch's state and value (the commented-out value when it's off).
        State get(const std::string& key, std::string* value = nullptr) const;
        // Turns the switch on with this value: the active line, else the first commented-out
        // one, else a new line at the end.
        void set(const std::string& key, const std::string& value);
        // Turns the switch off: active lines are commented out (value kept).
        void unset(const std::string& key);

        const std::vector<std::string>& lines() const { return m_lines; }
        void set_text(const std::string& text);   // for tests and the preview
        std::string text() const;

    private:
        struct Parsed {
            bool ok = false, commented = false;
            std::string key, value, comment;   // comment: " # ..." after the value, verbatim
            size_t indent = 0;
        };
        static Parsed parse(const std::string& line);
        static std::string format(const Parsed& p, bool commented, const std::string& value);

        std::wstring m_path;
        std::vector<std::string> m_lines;
        bool m_crlf = false;
    };
}
