// Which paths the file layer handles: rom\ and mdata\ in the game folder.

#include "vfs_internal.hpp"

namespace vfs {
    // Absolute, lower case, backslashes. False for paths outside rom\ and mdata\ (and rom /
    // mdata themselves count as inside, as names in the game folder).
    bool Absolute(const wchar_t* path, std::wstring& full) {
        if (!path || !*path)
            return false;
        if (!wcsncmp(path, L"\\\\?\\", 4) || !wcsncmp(path, L"\\\\.\\", 4))
            return false;
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetFullPathNameW(path, (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
        if (!n || n >= sizeof(buf) / sizeof(buf[0]))
            return false;
        full.assign(buf, n);
        dl::lower(full);
        if (full.compare(0, g_base.size(), g_base) != 0)
            return false;
        const wchar_t* rest = full.c_str() + g_base.size();
        return !wcscmp(rest, L"rom") || !wcscmp(rest, L"mdata") || !wcsncmp(rest, L"rom\\", 4) || !wcsncmp(rest, L"mdata\\", 6);
    }

    // A single name: folder + name. False for wildcards, a trailing backslash, out of scope.
    bool Resolve(const wchar_t* path, std::wstring& dir, std::wstring& name) {
        if (!path || !*path || wcspbrk(path, L"*?<>\""))
            return false;
        size_t len = wcslen(path);
        if (path[len - 1] == L'\\' || path[len - 1] == L'/')
            return false;
        std::wstring full;
        if (!Absolute(path, full))
            return false;
        size_t slash = full.find_last_of(L'\\');
        dir = full.substr(0, slash);
        name = full.substr(slash + 1);
        return !name.empty() && name != L"." && name != L"..";
    }

    // A search: folder + pattern (wildcards in the last part only).
    bool ResolveSearch(const wchar_t* path, std::wstring& dir, std::wstring& pattern) {
        if (!path || !*path || wcspbrk(path, L"<>\""))
            return false;
        std::wstring p = path;
        for (wchar_t& c : p)
            if (c == L'/')
                c = L'\\';
        size_t slash = p.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash + 1 >= p.size())
            return false;
        std::wstring folder = p.substr(0, slash), pat = p.substr(slash + 1);
        if (wcspbrk(folder.c_str(), L"*?") || !wcspbrk(pat.c_str(), L"*?"))
            return false;
        std::wstring full;
        if (!Absolute(folder.c_str(), full))
            return false;
        dir = full;
        pattern = pat;
        dl::lower(pattern);
        return true;
    }
}
