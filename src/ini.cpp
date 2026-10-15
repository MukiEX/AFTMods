#include "ini.hpp"

#ifndef _WIN32
#define SI_CONVERT_GENERIC
#endif
#include <SimpleIni.h>
#include <cstdio>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace lfs {
    static CSimpleIniW s_ini;
    static std::wstring s_loaded;

    std::string to_utf8(const std::wstring& w) {
        std::string out;
        out.reserve(w.size());
        for (size_t i = 0; i < w.size(); i++) {
            uint32_t c = (uint32_t)w[i];
            if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) {
                uint32_t lo = (uint32_t)w[i + 1];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }
            if (c < 0x80)
                out += (char)c;
            else if (c < 0x800) {
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000) {
                out += (char)(0xE0 | (c >> 12));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
            else {
                out += (char)(0xF0 | (c >> 18));
                out += (char)(0x80 | ((c >> 12) & 0x3F));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
        }
        return out;
    }

    std::wstring from_utf8(const std::string& s) {
        std::wstring out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            uint8_t b = (uint8_t)s[i];
            uint32_t c;
            int n;
            if (b < 0x80) { c = b; n = 1; }
            else if ((b >> 5) == 6) { c = b & 0x1F; n = 2; }
            else if ((b >> 4) == 14) { c = b & 0x0F; n = 3; }
            else if ((b >> 3) == 30) { c = b & 0x07; n = 4; }
            else { i++; continue; }
            if (i + n > s.size())
                break;
            for (int k = 1; k < n; k++)
                c = (c << 6) | ((uint8_t)s[i + k] & 0x3F);
            i += n;
            if (sizeof(wchar_t) == 2 && c >= 0x10000) {
                c -= 0x10000;
                out += (wchar_t)(0xD800 + (c >> 10));
                out += (wchar_t)(0xDC00 + (c & 0x3FF));
            }
            else
                out += (wchar_t)c;
        }
        return out;
    }

    static bool file_exists(const wchar_t* path) {
#ifdef _WIN32
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
#else
        struct stat st;
        return stat(to_utf8(path).c_str(), &st) == 0;
#endif
    }

    static SI_Error load_file(const wchar_t* path) {
#ifdef _WIN32
        return s_ini.LoadFile(path);
#else
        return s_ini.LoadFile(to_utf8(path).c_str());
#endif
    }

    static void save_file(const wchar_t* path) {
#ifdef _WIN32
        s_ini.SaveFile(path);
#else
        s_ini.SaveFile(to_utf8(path).c_str());
#endif
    }

    // IniReader.h's LoadIni
    static bool load(const wchar_t* file, bool utf8 = false) {
        if (s_ini.IsUnicode() == utf8 && s_loaded == file)
            return true;
        s_ini.Reset();
        s_ini.SetUnicode(utf8);
        if (load_file(file) != SI_OK) {
            // A missing file isn't an error: writing creates it.
            if (!file_exists(file)) {
                s_loaded = file;
                return true;
            }
            s_loaded.clear();
            return false;
        }
        s_loaded = file;
        return true;
    }

    void ini_invalidate() {
        s_loaded.clear();
    }

    std::wstring ini_get_string(const wchar_t* section, const wchar_t* key, const wchar_t* def, const wchar_t* file, bool utf8) {
        if (!load(file, utf8))
            return def ? def : L"";
        const wchar_t* v = s_ini.GetValue(section, key, def);
        return v ? v : L"";
    }

    int ini_get_int(const wchar_t* section, const wchar_t* key, int def, const wchar_t* file) {
        if (!load(file))
            return def;
        return (int)s_ini.GetLongValue(section, key, def);
    }

    bool ini_get_bool(const wchar_t* section, const wchar_t* key, bool def, const wchar_t* file) {
        if (!load(file))
            return def;
        return s_ini.GetBoolValue(section, key, def);
    }

    void ini_set_string(const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* file, bool utf8) {
        if (!load(file, utf8))
            return;
        s_ini.SetValue(section, key, value);
        save_file(file);
    }

    void ini_set_int(const wchar_t* section, const wchar_t* key, int value, const wchar_t* file) {
        if (!load(file))
            return;
        s_ini.SetLongValue(section, key, value);
        save_file(file);
    }

    void ini_set_bool(const wchar_t* section, const wchar_t* key, bool value, const wchar_t* file, bool as_string) {
        if (!load(file))
            return;
        s_ini.SetValue(section, key, as_string ? (value ? L"true" : L"false") : (value ? L"1" : L"0"));
        save_file(file);
    }
}
