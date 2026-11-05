// INI access with the same semantics as PD Launcher's IniReader.h (SimpleIni, one cached
// document, reloaded when the file or the UTF-8 mode changes, saved on every write), so the
// files PD Loader and its plugins read look exactly as if the original launcher wrote them.
#pragma once

#include <string>

namespace lfs {
    std::wstring ini_get_string(const wchar_t* section, const wchar_t* key, const wchar_t* def, const wchar_t* file, bool utf8);
    int ini_get_int(const wchar_t* section, const wchar_t* key, int def, const wchar_t* file);
    bool ini_get_bool(const wchar_t* section, const wchar_t* key, bool def, const wchar_t* file);

    void ini_set_string(const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* file, bool utf8);
    void ini_set_int(const wchar_t* section, const wchar_t* key, int value, const wchar_t* file);
    void ini_set_bool(const wchar_t* section, const wchar_t* key, bool value, const wchar_t* file, bool as_string);

    // Forget the cached document (call after something else may have written the file).
    void ini_invalidate();

    std::string to_utf8(const std::wstring& w);
    std::wstring from_utf8(const std::string& s);
}
