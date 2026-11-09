#include "env_file.hpp"
#include "ov_platform.hpp"

namespace dov {
    static bool is_space(char c) { return c == ' ' || c == '\t'; }

    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && is_space(s[a])) a++;
        while (b > a && (is_space(s[b - 1]) || s[b - 1] == '\r')) b--;
        return s.substr(a, b - a);
    }

    static bool same_key(const std::string& a, const std::string& b) {
        return a == b;   // the runtime's setenv is case-sensitive
    }

    // Same reading as the runtime's load_env_file: KEY=VALUE, spaces around both trimmed, a #
    // at the start of the value or after a space starting a comment. A commented-out line is
    // the same with a leading # (and spaces after it).
    EnvFile::Parsed EnvFile::parse(const std::string& line) {
        Parsed p;
        size_t i = 0;
        while (i < line.size() && is_space(line[i])) i++;
        p.indent = i;
        if (i < line.size() && line[i] == '#') {
            p.commented = true;
            i++;
            while (i < line.size() && is_space(line[i])) i++;
        }
        size_t eq = line.find('=', i);
        if (eq == std::string::npos)
            return p;
        p.key = trim(line.substr(i, eq - i));
        if (p.key.empty())
            return p;
        for (char c : p.key)   // a sentence in a comment isn't a switch
            if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                return p;
        std::string rest = line.substr(eq + 1);
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
        size_t vstart = 0;
        while (vstart < rest.size() && is_space(rest[vstart])) vstart++;
        size_t hash = std::string::npos;
        for (size_t k = vstart; k < rest.size(); k++)
            if (rest[k] == '#' && (k == vstart || is_space(rest[k - 1]))) { hash = k; break; }
        std::string value = hash == std::string::npos ? rest.substr(vstart) : rest.substr(vstart, hash - vstart);
        if (hash != std::string::npos) {
            size_t cs = hash;
            while (cs > vstart && is_space(rest[cs - 1])) cs--;
            p.comment = rest.substr(cs);
        }
        p.value = trim(value);
        p.ok = true;
        return p;
    }

    std::string EnvFile::format(const Parsed& p, bool commented, const std::string& value) {
        std::string s(p.indent, ' ');
        if (commented) s += '#';
        s += p.key + "=" + value;
        if (!p.comment.empty()) {
            if (!is_space(p.comment[0])) s += ' ';
            s += p.comment;
        }
        return s;
    }

    void EnvFile::set_text(const std::string& text) {
        m_lines.clear();
        m_crlf = text.find("\r\n") != std::string::npos;
        size_t a = 0;
        while (a < text.size()) {
            size_t e = text.find('\n', a);
            std::string line = text.substr(a, e == std::string::npos ? std::string::npos : e - a);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            m_lines.push_back(line);
            if (e == std::string::npos) break;
            a = e + 1;
        }
    }

    std::string EnvFile::text() const {
        std::string out;
        const char* nl = m_crlf ? "\r\n" : "\n";
        for (const std::string& l : m_lines)
            out += l + nl;
        return out;
    }

    bool EnvFile::load() {
        std::string data;
        if (!read_file(m_path, data)) {
            m_lines.clear();
            return false;
        }
        if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
            data.erase(0, 3);
        set_text(data);
        return true;
    }

    bool EnvFile::save() const {
        return write_file(m_path, text());
    }

    EnvFile::State EnvFile::get(const std::string& key, std::string* value) const {
        State st = State::Missing;
        for (const std::string& l : m_lines) {
            Parsed p = parse(l);
            if (!p.ok || !same_key(p.key, key))
                continue;
            if (!p.commented) {
                if (value) *value = p.value;   // the runtime's setenv: the last line wins
                st = State::On;
            }
            else if (st == State::Missing) {
                if (value) *value = p.value;
                st = State::Off;
            }
        }
        return st;
    }

    void EnvFile::set(const std::string& key, const std::string& value) {
        int active = -1, commented = -1;
        for (size_t i = 0; i < m_lines.size(); i++) {
            Parsed p = parse(m_lines[i]);
            if (!p.ok || !same_key(p.key, key))
                continue;
            if (!p.commented) active = (int)i;
            else if (commented < 0) commented = (int)i;
        }
        if (active >= 0) {
            // Later duplicates win in the runtime, so the last active line is the one to change;
            // earlier active ones are commented out so the file says one thing.
            for (size_t i = 0; i < m_lines.size(); i++) {
                Parsed p = parse(m_lines[i]);
                if (p.ok && !p.commented && same_key(p.key, key))
                    m_lines[i] = format(p, (int)i != active, (int)i == active ? value : p.value);
            }
            return;
        }
        if (commented >= 0) {
            Parsed p = parse(m_lines[commented]);
            m_lines[commented] = format(p, false, value);
            return;
        }
        while (!m_lines.empty() && trim(m_lines.back()).empty())
            m_lines.pop_back();
        m_lines.push_back(key + "=" + value);
    }

    void EnvFile::unset(const std::string& key) {
        for (std::string& l : m_lines) {
            Parsed p = parse(l);
            if (p.ok && !p.commented && same_key(p.key, key))
                l = format(p, true, p.value);
        }
    }
}
