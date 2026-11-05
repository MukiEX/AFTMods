// DivaLinux: virtual file layer for rom\ and mdata\ ([loading] section of DivaLinux.ini).
//
// Two jobs, both for slow setups (a Quest or phone running the game through Wine and FEX):
//
// 1. Lookup cache (lookup_cache=1). While loading, the game asks every mdata pack for every
//    file it might override: tens of thousands of single-name FindFirstFileExW calls per
//    start-up or song load, most of which miss. Under Wine each one scans a folder (Windows
//    names ignore case, Linux ones don't), about 5 ms under FEX on a Quest; a [debug]
//    io_trace log showed a 160 s start-up spending 200 s of loading-thread time on misses and
//    1 s reading data. Each folder is listed once, on first use, and later lookups are
//    answered from memory, with the results and errors Windows would give.
//
// 2. Archives (archives=1). rom.zip (next to diva.exe) and mdata\<pack>.zip stand in for the
//    rom folder and mdata\<pack> folders, so a game copied to a device is a few dozen files
//    instead of tens of thousands (MTP copies files one by one). The zips must be stored
//    (no compression): a file is then a plain range of the archive, read with one positional
//    read, and nothing is decompressed on the emulated CPU. The central directory is read once
//    at start-up and is the index. When an archive is present it replaces the folder of the
//    same name; without it, the folder is used as before. tools/divalinux_pack.py makes them.
//
// Everything works through the import tables of the game and its plugins, the same way
// vk_bridge.cpp redirects GL: only slots that still hold kernel32's own function are hooked,
// so another plugin's redirection wins in its own module. Modules that can't handle our
// handles (they don't import FindClose/FindNextFile*, ReadFile, CloseHandle) only get misses
// answered. Opened archive members are real handles (duplicates of the archive's handle), so
// even a call that isn't hooked gets a valid handle; the hooks make reads, seeks, sizes and
// file mappings see only the member.
//
// Known limit: a plugin that reads rom/mdata files through the C runtime DLL (ucrtbase's
// fopen) goes around these hooks and sees only real folders. The game's own reads go through
// its import table.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <wchar.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void divavk_log_text(const char* text);

namespace {
    // ------------------------------------------------------------ real functions

    typedef HANDLE (WINAPI* P_FindFirstFileExW)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    typedef HANDLE (WINAPI* P_FindFirstFileExA)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    typedef BOOL (WINAPI* P_FindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
    typedef BOOL (WINAPI* P_FindNextFileA)(HANDLE, LPWIN32_FIND_DATAA);
    typedef BOOL (WINAPI* P_FindClose)(HANDLE);
    typedef DWORD (WINAPI* P_GetFileAttributesW)(LPCWSTR);
    typedef DWORD (WINAPI* P_GetFileAttributesA)(LPCSTR);
    typedef BOOL (WINAPI* P_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    typedef BOOL (WINAPI* P_GetFileAttributesExA)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    typedef HANDLE (WINAPI* P_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    typedef HANDLE (WINAPI* P_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    typedef BOOL (WINAPI* P_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
    typedef DWORD (WINAPI* P_SetFilePointer)(HANDLE, LONG, PLONG, DWORD);
    typedef BOOL (WINAPI* P_SetFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
    typedef DWORD (WINAPI* P_GetFileSize)(HANDLE, LPDWORD);
    typedef BOOL (WINAPI* P_GetFileSizeEx)(HANDLE, PLARGE_INTEGER);
    typedef BOOL (WINAPI* P_GetFileInformationByHandle)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
    typedef BOOL (WINAPI* P_GetFileInformationByHandleEx)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
    typedef BOOL (WINAPI* P_GetFileTime)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
    typedef BOOL (WINAPI* P_CloseHandle)(HANDLE);
    typedef HANDLE (WINAPI* P_CreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
    typedef HANDLE (WINAPI* P_CreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);

    P_FindFirstFileExW real_FindFirstFileExW;
    P_FindFirstFileExA real_FindFirstFileExA;
    P_FindNextFileW real_FindNextFileW;
    P_FindNextFileA real_FindNextFileA;
    P_FindClose real_FindClose;
    P_GetFileAttributesW real_GetFileAttributesW;
    P_GetFileAttributesA real_GetFileAttributesA;
    P_GetFileAttributesExW real_GetFileAttributesExW;
    P_GetFileAttributesExA real_GetFileAttributesExA;
    P_CreateFileW real_CreateFileW;
    P_CreateFileA real_CreateFileA;
    P_ReadFile real_ReadFile;
    P_SetFilePointer real_SetFilePointer;
    P_SetFilePointerEx real_SetFilePointerEx;
    P_GetFileSize real_GetFileSize;
    P_GetFileSizeEx real_GetFileSizeEx;
    P_GetFileInformationByHandle real_GetFileInformationByHandle;
    P_GetFileInformationByHandleEx real_GetFileInformationByHandleEx;
    P_GetFileTime real_GetFileTime;
    P_CloseHandle real_CloseHandle;
    P_CreateFileMappingW real_CreateFileMappingW;
    P_CreateFileMappingA real_CreateFileMappingA;

    // ------------------------------------------------------------ data

    struct Archive;

    struct ZipItem {               // one stored member of an archive
        Archive* archive;
        uint64_t header_offset;    // local file header
        uint64_t size;
        std::atomic<int64_t> data_offset{ -1 }; // found from the local header on first open
        uint32_t index;            // for BY_HANDLE_FILE_INFORMATION's file index
        ZipItem(Archive* a, uint64_t h, uint64_t s, uint32_t i) : archive(a), header_offset(h), size(s), index(i) {}
    };

    struct Entry {
        std::wstring name;         // as on disk / in the archive
        DWORD attributes = 0;
        FILETIME created = {}, accessed = {}, written = {};
        uint64_t size = 0;
        ZipItem* item = nullptr;   // set for archive members
    };

    struct Dir {
        bool exists = false;
        bool unknown = false;      // couldn't be listed: lookups in it go to Wine
        bool archived = false;     // inside an archive
        std::wstring real_path;    // as on disk, no trailing backslash (real folders)
        std::unordered_map<std::wstring, Entry> entries; // lower-case name -> entry
    };

    struct Archive {
        std::wstring path;         // the .zip, as on disk
        std::wstring root;         // the folder it stands for, lower case absolute
        std::wstring root_name;    // that folder's name as shown in listings ("MZKO", "rom")
        HANDLE file = INVALID_HANDLE_VALUE;
        FILETIME written = {};
        uint64_t file_size = 0;
        std::deque<ZipItem> items;
        bool ok = false;
    };

    bool g_cache = false, g_archives = false, g_enabled = false;
    HMODULE g_self = nullptr;
    std::wstring g_base;        // game folder, lower case, trailing backslash
    std::wstring g_base_dir;    // the same without the trailing backslash
    std::wstring g_base_real;   // the game folder as on disk, no trailing backslash
    std::vector<Archive*> g_archive_list;

    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, Dir> g_dirs; // lower-case absolute path -> listing

    // Statistics (approximate)
    volatile LONG g_misses = 0, g_hits = 0, g_passed = 0, g_listed = 0, g_dropped = 0;
    volatile LONG g_opened = 0, g_enumerated = 0;
    volatile LONG64 g_bytes = 0;
    double g_list_ms = 0.0;

    double NowMs() {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart * 1000.0 / freq.QuadPart;
    }

    void Log(const char* format, ...) {
        char buf[768];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        divavk_log_text(buf);
    }

    std::string Narrow(const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring WidenAcp(const char* s) {
        if (!s)
            return std::wstring();
        int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
        return w;
    }

    void Lower(std::wstring& s) {
        for (wchar_t& c : s)
            c = (wchar_t)towlower(c);
    }

    // ------------------------------------------------------------ paths

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
        Lower(full);
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
        Lower(pattern);
        return true;
    }

    // Windows-style match: '*' any run, '?' one character, ignoring case ("*.*" is "*").
    bool Match(const wchar_t* p, const wchar_t* s) {
        if (!wcscmp(p, L"*.*"))
            p = L"*";
        const wchar_t *star = nullptr, *ss = nullptr;
        while (*s) {
            if (*p == L'?' || *p == (wchar_t)towlower(*s)) {
                p++;
                s++;
            }
            else if (*p == L'*') {
                star = p++;
                ss = s;
            }
            else if (star) {
                p = star + 1;
                s = ++ss;
            }
            else
                return false;
        }
        while (*p == L'*')
            p++;
        return !*p;
    }

    // ------------------------------------------------------------ archives

    Archive* ArchiveFor(const std::wstring& dir) {
        for (Archive* a : g_archive_list)
            if (a->ok && (dir == a->root || (dir.size() > a->root.size() && !dir.compare(0, a->root.size(), a->root)
                && dir[a->root.size()] == L'\\')))
                return a;
        return nullptr;
    }

    // The folder holding an archive's stand-in folder (the game folder for rom.zip, the mdata
    // folder for packs): its listings must show the archives even with the lookup cache off.
    bool IsArchiveParent(const std::wstring& dir) {
        for (Archive* a : g_archive_list) {
            if (!a->ok)
                continue;
            size_t slash = a->root.find_last_of(L'\\');
            if (slash == dir.size() && !a->root.compare(0, slash, dir))
                return true;
        }
        return false;
    }

    bool ReadAt(HANDLE file, uint64_t offset, void* buffer, DWORD size) {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        return real_ReadFile(file, buffer, size, &got, &ov) && got == size;
    }

    uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
    uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
    uint64_t U64(const uint8_t* p) { return (uint64_t)U32(p) | (uint64_t)U32(p + 4) << 32; }

    Dir& ArchiveDir(const std::wstring& dir) {
        Dir& d = g_dirs[dir];
        d.exists = true;
        d.archived = true;
        return d;
    }

    // Adds folder `rel` (archive-relative, as in the archive, backslashes) and its parents.
    void AddArchiveDirs(Archive* a, const std::wstring& rel) {
        std::wstring lower = rel;
        Lower(lower);
        ArchiveDir(a->root);
        size_t pos = 0;
        while (pos < rel.size()) {
            size_t next = rel.find(L'\\', pos);
            if (next == std::wstring::npos)
                next = rel.size();
            std::wstring parent = pos ? a->root + L"\\" + lower.substr(0, pos - 1) : a->root;
            std::wstring key = lower.substr(pos, next - pos);
            Dir& p = ArchiveDir(parent);
            if (!p.entries.count(key)) {
                Entry e;
                e.name = rel.substr(pos, next - pos);
                e.attributes = FILE_ATTRIBUTE_DIRECTORY;
                e.created = e.accessed = e.written = a->written;
                p.entries.emplace(key, e);
            }
            ArchiveDir(a->root + L"\\" + lower.substr(0, next));
            pos = next + 1;
        }
    }

    // Reads the central directory. False (with the reason logged) leaves the folder in use.
    bool LoadArchive(Archive* a) {
        a->file = real_CreateFileW(a->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (a->file == INVALID_HANDLE_VALUE) {
            Log("Archive %s: can't open it (error %lu)", Narrow(a->path).c_str(), GetLastError());
            return false;
        }
        LARGE_INTEGER size;
        if (!real_GetFileSizeEx(a->file, &size) || size.QuadPart < 22) {
            Log("Archive %s: too small to be a zip", Narrow(a->path).c_str());
            return false;
        }
        a->file_size = (uint64_t)size.QuadPart;
        real_GetFileTime(a->file, nullptr, nullptr, &a->written);

        // End of central directory: in the last 64 KB + 22 bytes
        uint64_t tail_size = std::min<uint64_t>(a->file_size, 65557 + 20);
        std::vector<uint8_t> tail((size_t)tail_size);
        if (!ReadAt(a->file, a->file_size - tail_size, tail.data(), (DWORD)tail_size)) {
            Log("Archive %s: read error", Narrow(a->path).c_str());
            return false;
        }
        int64_t eocd = -1;
        for (int64_t i = (int64_t)tail_size - 22; i >= 0; i--)
            if (U32(&tail[(size_t)i]) == 0x06054b50) {
                eocd = i;
                break;
            }
        if (eocd < 0) {
            Log("Archive %s: not a zip (no end of central directory)", Narrow(a->path).c_str());
            return false;
        }
        const uint8_t* e = &tail[(size_t)eocd];
        uint64_t count = U16(e + 10), cd_size = U32(e + 12), cd_offset = U32(e + 16);
        if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF) {
            // Zip64: the locator sits just before the end record
            uint8_t z[56];
            if (eocd < 20 || U32(&tail[(size_t)eocd - 20]) != 0x07064b50
                || !ReadAt(a->file, U64(&tail[(size_t)eocd - 20 + 8]), z, sizeof(z)) || U32(z) != 0x06064b50) {
                Log("Archive %s: broken zip64 end record", Narrow(a->path).c_str());
                return false;
            }
            count = U64(z + 32);
            cd_size = U64(z + 40);
            cd_offset = U64(z + 48);
        }
        if (cd_offset + cd_size > a->file_size || cd_size > 512ull << 20) {
            Log("Archive %s: central directory out of range", Narrow(a->path).c_str());
            return false;
        }
        std::vector<uint8_t> cd((size_t)cd_size);
        if (cd_size && !ReadAt(a->file, cd_offset, cd.data(), (DWORD)cd_size)) {
            Log("Archive %s: read error in the central directory", Narrow(a->path).c_str());
            return false;
        }

        struct Raw { std::wstring name; uint64_t header, size; uint16_t date, time; bool dir; };
        std::vector<Raw> raw;
        raw.reserve((size_t)count);
        size_t p = 0;
        for (uint64_t i = 0; i < count; i++) {
            if (p + 46 > cd.size() || U32(&cd[p]) != 0x02014b50) {
                Log("Archive %s: broken central directory", Narrow(a->path).c_str());
                return false;
            }
            const uint8_t* c = &cd[p];
            uint16_t flags = U16(c + 8), method = U16(c + 10);
            uint64_t csize = U32(c + 20), usize = U32(c + 24), header = U32(c + 42);
            size_t nlen = U16(c + 28), xlen = U16(c + 30), clen = U16(c + 32);
            if (p + 46 + nlen + xlen + clen > cd.size()) {
                Log("Archive %s: broken central directory", Narrow(a->path).c_str());
                return false;
            }
            std::string name8((const char*)c + 46, nlen);
            // Zip64 sizes and offset
            for (size_t x = 0; x + 4 <= xlen;) {
                const uint8_t* f = c + 46 + nlen + x;
                uint16_t id = U16(f), len = U16(f + 2);
                if (id == 0x0001) {
                    const uint8_t* v = f + 4;
                    const uint8_t* end = v + len;
                    if (usize == 0xFFFFFFFF && v + 8 <= end) { usize = U64(v); v += 8; }
                    if (csize == 0xFFFFFFFF && v + 8 <= end) { csize = U64(v); v += 8; }
                    if (header == 0xFFFFFFFF && v + 8 <= end) { header = U64(v); v += 8; }
                }
                x += 4 + (size_t)len;
            }
            p += 46 + nlen + xlen + clen;

            bool dir = !name8.empty() && name8.back() == '/';
            if (!dir && (method != 0 || (flags & 1) || csize != usize)) {
                Log("Archive %s: %s is %s; DivaLinux reads only stored (uncompressed) zips, so the folder is used "
                    "instead. Repack with tools/divalinux_pack.py, or 7-Zip with compression level 0 (Store).",
                    Narrow(a->path).c_str(), name8.c_str(), (flags & 1) ? "encrypted" : "compressed");
                return false;
            }
            int wn = MultiByteToWideChar(CP_UTF8, 0, name8.c_str(), (int)name8.size(), nullptr, 0);
            std::wstring name(wn, L'\0');
            if (wn)
                MultiByteToWideChar(CP_UTF8, 0, name8.c_str(), (int)name8.size(), &name[0], wn);
            for (wchar_t& ch : name)
                if (ch == L'/')
                    ch = L'\\';
            while (!name.empty() && name.back() == L'\\')
                name.pop_back();
            while (!name.compare(0, 2, L".\\"))
                name.erase(0, 2);
            if (name.empty() || name[0] == L'\\' || name.find(L':') != std::wstring::npos
                || name.find(L"..\\") != std::wstring::npos || name == L"..")
                continue;
            raw.push_back({ name, header, usize, U16(c + 14), U16(c + 12), dir });
        }

        // Archives carry their place in the game folder, so "extract here" in the game folder
        // rebuilds it: rom.zip holds rom\..., mdata\MZKO.zip holds mdata\MZKO\.... Zips of just
        // the folder (MZKO\...) or of its contents are read too. Strip whichever prefix every
        // entry shares, dropping the entries for the prefix's own folders.
        {
            std::vector<std::wstring> prefixes;
            std::wstring top = a->root.substr(g_base.size()); // "rom" or "mdata\mzko"
            prefixes.push_back(top + L"\\");
            if (top.compare(0, 6, L"mdata\\") == 0)
                prefixes.push_back(top.substr(6) + L"\\");
            for (const std::wstring& prefix : prefixes) {
                bool all = !raw.empty(), any = false;
                for (const Raw& r : raw) {
                    std::wstring n = r.name + L"\\";
                    if (n.size() > prefix.size() && !_wcsnicmp(n.c_str(), prefix.c_str(), prefix.size()))
                        any = true;
                    else if (!(r.dir && n.size() <= prefix.size() && !_wcsnicmp(prefix.c_str(), n.c_str(), n.size()))) {
                        all = false; // outside the prefix (and not one of its own folders)
                        break;
                    }
                }
                if (!all || !any)
                    continue;
                std::vector<Raw> kept;
                for (Raw& r : raw)
                    if (r.name.size() >= prefix.size()) {
                        r.name.erase(0, prefix.size());
                        if (!r.name.empty())
                            kept.push_back(r);
                    }
                raw.swap(kept);
                break;
            }
        }

        ArchiveDir(a->root);
        uint32_t index = 0;
        for (const Raw& r : raw) {
            if (r.dir) {
                AddArchiveDirs(a, r.name);
                continue;
            }
            size_t slash = r.name.find_last_of(L'\\');
            std::wstring parent_rel = slash == std::wstring::npos ? std::wstring() : r.name.substr(0, slash);
            std::wstring file = slash == std::wstring::npos ? r.name : r.name.substr(slash + 1);
            if (!parent_rel.empty())
                AddArchiveDirs(a, parent_rel);
            std::wstring parent = a->root;
            if (!parent_rel.empty()) {
                std::wstring l = parent_rel;
                Lower(l);
                parent += L"\\" + l;
            }
            a->items.emplace_back(a, r.header, r.size, index++);
            Entry e;
            e.name = file;
            e.attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
            FILETIME local;
            if (DosDateTimeToFileTime(r.date, r.time, &local))
                LocalFileTimeToFileTime(&local, &e.written);
            e.created = e.accessed = e.written;
            e.size = r.size;
            e.item = &a->items.back();
            std::wstring key = file;
            Lower(key);
            ArchiveDir(parent).entries[key] = e;
        }
        Log("Archive %s: %zu files, replaces the %s folder", Narrow(a->path).c_str(), a->items.size(),
            Narrow(a->root.substr(g_base.size())).c_str());
        return true;
    }

    // Finds rom.zip and mdata\*.zip (called once, at install).
    void FindArchives() {
        auto add = [](const std::wstring& zip, const std::wstring& root, const std::wstring& name) {
            Archive* a = new Archive();
            a->path = zip;
            a->root = root;
            a->root_name = name;
            a->ok = LoadArchive(a);
            if (!a->ok) {
                // Forget anything a failed load added; the folder is listed as usual
                for (auto it = g_dirs.begin(); it != g_dirs.end();)
                    if (it->first == root || !it->first.compare(0, root.size() + 1, root + L"\\"))
                        it = g_dirs.erase(it);
                    else
                        ++it;
            }
            g_archive_list.push_back(a);
        };

        WIN32_FIND_DATAW fd;
        HANDLE h = real_FindFirstFileExW((g_base_real + L"\\*.zip").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!_wcsicmp(fd.cFileName, L"rom.zip") && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    add(g_base_real + L"\\" + fd.cFileName, g_base + L"rom", L"rom");
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
        }

        // mdata: find the folder's real name first (case)
        std::wstring mdata_real;
        h = real_FindFirstFileExW((g_base_real + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!_wcsicmp(fd.cFileName, L"mdata") && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    mdata_real = g_base_real + L"\\" + fd.cFileName;
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
        }
        if (mdata_real.empty())
            return;
        h = real_FindFirstFileExW((mdata_real + L"\\*.zip").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                std::wstring file = fd.cFileName;
                if (file.size() <= 4 || _wcsicmp(file.c_str() + file.size() - 4, L".zip"))
                    continue;
                std::wstring name = file.substr(0, file.size() - 4), lower = name;
                Lower(lower);
                add(mdata_real + L"\\" + file, g_base + L"mdata\\" + lower, name);
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
        }
    }

    // ------------------------------------------------------------ listings

    void List(Dir& d, const std::wstring& key) {
        double t0 = NowMs();
        WIN32_FIND_DATAW fd;
        HANDLE h = real_FindFirstFileExW((d.real_path + L"\\*").c_str(), FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            d.exists = e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES;
            d.unknown = !d.exists;
        }
        else {
            d.exists = true;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                    continue;
                Entry e;
                e.name = fd.cFileName;
                e.attributes = fd.dwFileAttributes;
                e.created = fd.ftCreationTime;
                e.accessed = fd.ftLastAccessTime;
                e.written = fd.ftLastWriteTime;
                e.size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
                std::wstring k = e.name;
                Lower(k);
                d.entries.emplace(std::move(k), std::move(e));
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
            InterlockedIncrement(&g_listed);
        }
        // Archives show up as the folders they stand for
        if (d.exists)
            for (Archive* a : g_archive_list) {
                if (!a->ok)
                    continue;
                size_t slash = a->root.find_last_of(L'\\');
                if (a->root.compare(0, slash, key) || slash != key.size())
                    continue;
                Entry e;
                e.name = a->root_name;
                e.attributes = FILE_ATTRIBUTE_DIRECTORY;
                e.created = e.accessed = e.written = a->written;
                d.entries[a->root.substr(slash + 1)] = e;
            }
        g_list_ms += NowMs() - t0;
    }

    // The listing of folder `dir` (lower case), made on first use. Caller holds g_lock
    // exclusively. Parents are listed first, so a folder that doesn't exist costs nothing.
    Dir& GetDirLocked(const std::wstring& dir) {
        auto it = g_dirs.find(dir);
        if (it != g_dirs.end())
            return it->second;
        Dir d;
        if (ArchiveFor(dir)) {
            d.archived = true; // inside an archive, and not one of its folders: missing
        }
        else if (dir == g_base_dir) {
            d.real_path = g_base_real;
            List(d, dir);
        }
        else {
            size_t slash = dir.find_last_of(L'\\');
            Dir& parent = GetDirLocked(dir.substr(0, slash));
            auto e = parent.exists ? parent.entries.find(dir.substr(slash + 1)) : parent.entries.end();
            if (parent.unknown)
                d.unknown = true;
            else if (e != parent.entries.end() && (e->second.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                d.real_path = parent.real_path + L"\\" + e->second.name;
                List(d, dir);
            }
        }
        return g_dirs.emplace(dir, std::move(d)).first->second;
    }

    enum Answer { PASS, FOUND, MISSING_FILE, MISSING_PATH };

    Answer Classify(const Dir& d, const std::wstring& name, Entry* out) {
        if (d.unknown)
            return PASS;
        if (!d.exists)
            return MISSING_PATH;
        auto e = d.entries.find(name);
        if (e == d.entries.end())
            return MISSING_FILE;
        if (out)
            *out = e->second;
        return FOUND;
    }

    // The cache answers real folders only when lookup_cache is on; archive folders, and the
    // folders that hold archives, always.
    bool Answerable(const std::wstring& key, const Dir& d) { return g_cache || d.archived || IsArchiveParent(key); }

    Answer Lookup(const wchar_t* path, Entry& out) {
        std::wstring dir, name;
        if (!Resolve(path, dir, name))
            return PASS;
        if (!g_cache && !ArchiveFor(dir) && !IsArchiveParent(dir))
            return PASS;
        AcquireSRWLockShared(&g_lock);
        auto it = g_dirs.find(dir);
        bool cached = it != g_dirs.end();
        Answer a = PASS;
        if (cached)
            a = Answerable(dir, it->second) ? Classify(it->second, name, &out) : PASS;
        ReleaseSRWLockShared(&g_lock);
        if (cached)
            return a;
        AcquireSRWLockExclusive(&g_lock);
        const Dir& d = GetDirLocked(dir);
        a = Answerable(dir, d) ? Classify(d, name, &out) : PASS;
        ReleaseSRWLockExclusive(&g_lock);
        return a;
    }

    void Drop(const wchar_t* path) {
        std::wstring dir, name;
        if (!Resolve(path, dir, name) || ArchiveFor(dir))
            return;
        AcquireSRWLockExclusive(&g_lock);
        if (g_dirs.erase(dir))
            InterlockedIncrement(&g_dropped);
        if (!ArchiveFor(dir + L"\\" + name))
            g_dirs.erase(dir + L"\\" + name);
        ReleaseSRWLockExclusive(&g_lock);
    }

    // ------------------------------------------------------------ find handles

    struct FindState {
        std::vector<Entry> entries;
        size_t next = 0;
    };

    // Handles come from a reserved, never-committed range, so they can't collide with
    // anything Wine hands out.
    uint8_t* g_find_base = nullptr;
    const size_t FIND_SLOTS = 4096;
    FindState* volatile g_find[FIND_SLOTS];
    volatile LONG g_find_next = 0;

    HANDLE NewFind(FindState* s) {
        if (!g_find_base)
            return nullptr;
        for (size_t tries = 0; tries < FIND_SLOTS; tries++) {
            size_t i = (size_t)InterlockedIncrement(&g_find_next) % FIND_SLOTS;
            if (!InterlockedCompareExchangePointer((PVOID volatile*)&g_find[i], s, nullptr))
                return (HANDLE)(g_find_base + i * 16);
        }
        return nullptr;
    }

    FindState* GetFind(HANDLE h, size_t* index) {
        uint8_t* p = (uint8_t*)h;
        if (!g_find_base || p < g_find_base || p >= g_find_base + FIND_SLOTS * 16 || (p - g_find_base) % 16)
            return nullptr;
        *index = (size_t)(p - g_find_base) / 16;
        return g_find[*index];
    }

    void FillW(WIN32_FIND_DATAW* fd, const Entry& e) {
        memset(fd, 0, sizeof(*fd));
        fd->dwFileAttributes = e.attributes;
        fd->ftCreationTime = e.created;
        fd->ftLastAccessTime = e.accessed;
        fd->ftLastWriteTime = e.written;
        fd->nFileSizeHigh = (DWORD)(e.size >> 32);
        fd->nFileSizeLow = (DWORD)e.size;
        wcsncpy(fd->cFileName, e.name.c_str(), MAX_PATH - 1);
    }

    void FillA(WIN32_FIND_DATAA* fd, const Entry& e) {
        WIN32_FIND_DATAW w;
        FillW(&w, e);
        memset(fd, 0, sizeof(*fd));
        fd->dwFileAttributes = w.dwFileAttributes;
        fd->ftCreationTime = w.ftCreationTime;
        fd->ftLastAccessTime = w.ftLastAccessTime;
        fd->ftLastWriteTime = w.ftLastWriteTime;
        fd->nFileSizeHigh = w.nFileSizeHigh;
        fd->nFileSizeLow = w.nFileSizeLow;
        WideCharToMultiByte(CP_ACP, 0, w.cFileName, -1, fd->cFileName, MAX_PATH, nullptr, nullptr);
    }

    // FindFirstFileEx for a single name or a search. Returns PASS when Wine should do it.
    // answer_found: the caller's FindClose and FindNextFile* go through our hooks.
    Answer FindFirst(const wchar_t* path, FINDEX_INFO_LEVELS level, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags,
        bool answer_found, FindState** state) {
        if (filter || (op != FindExSearchNameMatch && op != FindExSearchLimitToDirectories)
            || (flags & FIND_FIRST_EX_CASE_SENSITIVE) || (level != FindExInfoStandard && level != FindExInfoBasic))
            return PASS;
        std::wstring dir, pattern;
        if (ResolveSearch(path, dir, pattern)) {
            if (!answer_found || (!g_cache && !ArchiveFor(dir) && !IsArchiveParent(dir)))
                return PASS;
            AcquireSRWLockExclusive(&g_lock);
            const Dir& d = GetDirLocked(dir);
            Answer a = !Answerable(dir, d) || d.unknown ? PASS : !d.exists ? MISSING_PATH : FOUND;
            FindState* s = nullptr;
            if (a == FOUND) {
                s = new FindState();
                // "." and ".." come first, as Windows lists them
                for (const wchar_t* dot : { L".", L".." })
                    if (Match(pattern.c_str(), dot)) {
                        Entry e;
                        e.name = dot;
                        e.attributes = FILE_ATTRIBUTE_DIRECTORY;
                        s->entries.push_back(e);
                    }
                std::vector<std::pair<std::wstring, const Entry*>> sorted;
                for (const auto& kv : d.entries)
                    if (Match(pattern.c_str(), kv.first.c_str())
                        && (op != FindExSearchLimitToDirectories || (kv.second.attributes & FILE_ATTRIBUTE_DIRECTORY)))
                        sorted.push_back({ kv.first, &kv.second });
                std::sort(sorted.begin(), sorted.end());
                for (const auto& kv : sorted)
                    s->entries.push_back(*kv.second);
                if (s->entries.empty()) {
                    delete s;
                    s = nullptr;
                    a = MISSING_FILE;
                }
            }
            ReleaseSRWLockExclusive(&g_lock);
            if (a == FOUND)
                InterlockedIncrement(&g_enumerated);
            *state = s;
            return a;
        }
        Entry e;
        Answer a = Lookup(path, e);
        if (a == FOUND) {
            if (!answer_found)
                return PASS;
            FindState* s = new FindState();
            s->entries.push_back(e);
            *state = s;
        }
        return a;
    }

    HANDLE FinishFind(Answer a, FindState* s, HANDLE (*fill_and_pass)(void*), void* ctx) {
        switch (a) {
        case MISSING_FILE:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        case MISSING_PATH:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        case FOUND:
            if (HANDLE h = NewFind(s)) {
                InterlockedIncrement(&g_hits);
                SetLastError(ERROR_SUCCESS);
                return h;
            }
            delete s;
            break;
        default:
            break;
        }
        InterlockedIncrement(&g_passed);
        return fill_and_pass(ctx);
    }

    // ------------------------------------------------------------ archive file handles

    struct VFile {
        ZipItem* item;
        uint64_t data;     // offset of the member's data in the archive
        uint64_t size;
        uint64_t pos = 0;
        FILETIME written;
        DWORD attributes;
    };

    SRWLOCK g_vfile_lock = SRWLOCK_INIT;
    std::unordered_map<HANDLE, VFile> g_vfiles;
    std::atomic<long> g_vfile_count{ 0 }; // quick check before taking the lock

    bool GetVFile(HANDLE h, VFile* out) {
        if (!g_vfile_count)
            return false;
        AcquireSRWLockShared(&g_vfile_lock);
        auto it = g_vfiles.find(h);
        bool found = it != g_vfiles.end();
        if (found && out)
            *out = it->second;
        ReleaseSRWLockShared(&g_vfile_lock);
        return found;
    }

    void SetPos(HANDLE h, uint64_t pos) {
        AcquireSRWLockExclusive(&g_vfile_lock);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end())
            it->second.pos = pos;
        ReleaseSRWLockExclusive(&g_vfile_lock);
    }

    bool IsReadOnlyAccess(DWORD access) {
        const DWORD read = GENERIC_READ | GENERIC_EXECUTE | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA
            | FILE_EXECUTE | READ_CONTROL | SYNCHRONIZE;
        return !(access & ~read);
    }

    HANDLE OpenMember(const Entry& e) {
        ZipItem* item = e.item;
        Archive* a = item->archive;
        int64_t data = item->data_offset.load();
        if (data < 0) {
            uint8_t h[30];
            if (!ReadAt(a->file, item->header_offset, h, sizeof(h)) || U32(h) != 0x04034b50) {
                Log("Archive %s: bad local header for %s", Narrow(a->path).c_str(), Narrow(e.name).c_str());
                SetLastError(ERROR_FILE_CORRUPT);
                return INVALID_HANDLE_VALUE;
            }
            data = (int64_t)(item->header_offset + 30 + U16(h + 26) + U16(h + 28));
            if ((uint64_t)data + item->size > a->file_size) {
                SetLastError(ERROR_FILE_CORRUPT);
                return INVALID_HANDLE_VALUE;
            }
            item->data_offset.store(data);
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), a->file, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return INVALID_HANDLE_VALUE;
        VFile v;
        v.item = item;
        v.data = (uint64_t)data;
        v.size = item->size;
        v.written = e.written;
        v.attributes = e.attributes;
        AcquireSRWLockExclusive(&g_vfile_lock);
        g_vfiles[dup] = v;
        g_vfile_count++;
        ReleaseSRWLockExclusive(&g_vfile_lock);
        InterlockedIncrement(&g_opened);
        SetLastError(ERROR_SUCCESS);
        return dup;
    }

    // CreateFile for a path: PASS to Wine, or an answer. serve_members: the caller's ReadFile
    // and CloseHandle go through our hooks.
    HANDLE CreateFileCommon(const wchar_t* path, DWORD access, DWORD disposition, DWORD flags, bool serve_members, bool* pass) {
        *pass = true;
        Entry e;
        Answer a = Lookup(path, e);
        bool creates = disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS;
        if (a == MISSING_FILE || a == MISSING_PATH) {
            std::wstring dir, name;
            bool in_archive = Resolve(path, dir, name) && ArchiveFor(dir);
            if (creates && !in_archive)
                return INVALID_HANDLE_VALUE; // let Wine create it (and drop the listing)
            *pass = false;
            InterlockedIncrement(&g_misses);
            SetLastError(in_archive && creates ? ERROR_ACCESS_DENIED : a == MISSING_FILE ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        if (a != FOUND || !e.item)
            return INVALID_HANDLE_VALUE; // real files, folders: Wine opens them
        *pass = false;
        if (!serve_members) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (!IsReadOnlyAccess(access) || disposition == CREATE_NEW || disposition == CREATE_ALWAYS
            || disposition == TRUNCATE_EXISTING || (flags & FILE_FLAG_DELETE_ON_CLOSE)) {
            SetLastError(ERROR_ACCESS_DENIED); // archives are read-only
            return INVALID_HANDLE_VALUE;
        }
        InterlockedIncrement(&g_hits);
        return OpenMember(e);
    }

    // ------------------------------------------------------------ hooks: lookups

    struct FindCtxW { LPCWSTR path; FINDEX_INFO_LEVELS level; LPVOID data; FINDEX_SEARCH_OPS op; LPVOID filter; DWORD flags; };
    struct FindCtxA { LPCSTR path; FINDEX_INFO_LEVELS level; LPVOID data; FINDEX_SEARCH_OPS op; LPVOID filter; DWORD flags; };

    HANDLE PassFindW(void* c) {
        FindCtxW* x = (FindCtxW*)c;
        return real_FindFirstFileExW(x->path, x->level, x->data, x->op, x->filter, x->flags);
    }
    HANDLE PassFindA(void* c) {
        FindCtxA* x = (FindCtxA*)c;
        return real_FindFirstFileExA(x->path, x->level, x->data, x->op, x->filter, x->flags);
    }

    HANDLE FindFirstW(bool full, LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
        FindCtxW ctx = { path, level, data, op, filter, flags };
        FindState* s = nullptr;
        Answer a = data ? FindFirst(path, level, op, filter, flags, full, &s) : PASS;
        if (a == FOUND)
            FillW((WIN32_FIND_DATAW*)data, s->entries[s->next++]);
        return FinishFind(a, s, PassFindW, &ctx);
    }

    HANDLE FindFirstA(bool full, LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
        FindCtxA ctx = { path, level, data, op, filter, flags };
        FindState* s = nullptr;
        Answer a = data && path ? FindFirst(WidenAcp(path).c_str(), level, op, filter, flags, full, &s) : PASS;
        if (a == FOUND)
            FillA((WIN32_FIND_DATAA*)data, s->entries[s->next++]);
        return FinishFind(a, s, PassFindA, &ctx);
    }

    HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstW(true, p, l, d, o, f, fl);
    }
    HANDLE WINAPI Hook_FindFirstFileExW_Lookups(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstW(false, p, l, d, o, f, fl);
    }
    HANDLE WINAPI Hook_FindFirstFileExA(LPCSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstA(true, p, l, d, o, f, fl);
    }
    HANDLE WINAPI Hook_FindFirstFileExA_Lookups(LPCSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstA(false, p, l, d, o, f, fl);
    }

    BOOL WINAPI Hook_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data) {
        size_t i;
        if (FindState* s = GetFind(h, &i)) {
            if (s->next >= s->entries.size()) {
                SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            FillW(data, s->entries[s->next++]);
            return TRUE;
        }
        return real_FindNextFileW(h, data);
    }

    BOOL WINAPI Hook_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) {
        size_t i;
        if (FindState* s = GetFind(h, &i)) {
            if (s->next >= s->entries.size()) {
                SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            FillA(data, s->entries[s->next++]);
            return TRUE;
        }
        return real_FindNextFileA(h, data);
    }

    BOOL WINAPI Hook_FindClose(HANDLE h) {
        size_t i;
        if (FindState* s = GetFind(h, &i)) {
            if (InterlockedCompareExchangePointer((PVOID volatile*)&g_find[i], nullptr, s) == s)
                delete s;
            return TRUE;
        }
        return real_FindClose(h);
    }

    DWORD AttributesCommon(const wchar_t* path, bool* pass) {
        Entry e;
        *pass = false;
        switch (Lookup(path, e)) {
        case FOUND:
            InterlockedIncrement(&g_hits);
            return e.attributes;
        case MISSING_FILE:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        case MISSING_PATH:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        default:
            *pass = true;
            InterlockedIncrement(&g_passed);
            return 0;
        }
    }

    DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR path) {
        bool pass;
        DWORD r = AttributesCommon(path, &pass);
        return pass ? real_GetFileAttributesW(path) : r;
    }

    DWORD WINAPI Hook_GetFileAttributesA(LPCSTR path) {
        bool pass;
        DWORD r = path ? AttributesCommon(WidenAcp(path).c_str(), &pass) : (pass = true, 0);
        return pass ? real_GetFileAttributesA(path) : r;
    }

    // 1 answered, 0 failed (error set), -1 pass to Wine
    int AttributesExCommon(const wchar_t* path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
        if (level != GetFileExInfoStandard || !info)
            return -1;
        Entry e;
        switch (Lookup(path, e)) {
        case FOUND: {
            InterlockedIncrement(&g_hits);
            WIN32_FILE_ATTRIBUTE_DATA* d = (WIN32_FILE_ATTRIBUTE_DATA*)info;
            d->dwFileAttributes = e.attributes;
            d->ftCreationTime = e.created;
            d->ftLastAccessTime = e.accessed;
            d->ftLastWriteTime = e.written;
            d->nFileSizeHigh = (DWORD)(e.size >> 32);
            d->nFileSizeLow = (DWORD)e.size;
            return 1;
        }
        case MISSING_FILE:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return 0;
        case MISSING_PATH:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_PATH_NOT_FOUND);
            return 0;
        default:
            InterlockedIncrement(&g_passed);
            return -1;
        }
    }

    BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
        int r = AttributesExCommon(path, level, info);
        return r < 0 ? real_GetFileAttributesExW(path, level, info) : r;
    }

    BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
        int r = path ? AttributesExCommon(WidenAcp(path).c_str(), level, info) : -1;
        return r < 0 ? real_GetFileAttributesExA(path, level, info) : r;
    }

    // ------------------------------------------------------------ hooks: files

    HANDLE CreateW(bool full, LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ) {
        bool pass;
        HANDLE h = CreateFileCommon(path, access, disposition, flags, full, &pass);
        if (!pass)
            return h;
        h = real_CreateFileW(path, access, share, sa, disposition, flags, templ);
        if (h != INVALID_HANDLE_VALUE && disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING) {
            DWORD e = GetLastError();
            Drop(path);
            SetLastError(e);
        }
        return h;
    }

    HANDLE CreateA(bool full, LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ) {
        bool pass = true;
        HANDLE h = INVALID_HANDLE_VALUE;
        std::wstring w = WidenAcp(path);
        if (path)
            h = CreateFileCommon(w.c_str(), access, disposition, flags, full, &pass);
        if (!pass)
            return h;
        h = real_CreateFileA(path, access, share, sa, disposition, flags, templ);
        if (path && h != INVALID_HANDLE_VALUE && disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING) {
            DWORD e = GetLastError();
            Drop(w.c_str());
            SetLastError(e);
        }
        return h;
    }

    HANDLE WINAPI Hook_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateW(true, p, a, s, sa, d, f, t);
    }
    HANDLE WINAPI Hook_CreateFileW_Lookups(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateW(false, p, a, s, sa, d, f, t);
    }
    HANDLE WINAPI Hook_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateA(true, p, a, s, sa, d, f, t);
    }
    HANDLE WINAPI Hook_CreateFileA_Lookups(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateA(false, p, a, s, sa, d, f, t);
    }

    BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buffer, DWORD n, LPDWORD read, LPOVERLAPPED ov) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_ReadFile(h, buffer, n, read, ov);
        uint64_t offset = ov ? ((uint64_t)ov->OffsetHigh << 32 | ov->Offset) : v.pos;
        DWORD count = offset >= v.size ? 0 : (DWORD)std::min<uint64_t>(n, v.size - offset);
        DWORD got = 0;
        BOOL ok = TRUE;
        if (count) {
            OVERLAPPED at = {};
            uint64_t where = v.data + offset;
            at.Offset = (DWORD)where;
            at.OffsetHigh = (DWORD)(where >> 32);
            ok = real_ReadFile(v.item->archive->file, buffer, count, &got, &at);
        }
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (ok && !ov)
            SetPos(h, offset + got);
        if (read)
            *read = got;
        if (ov) {
            ov->Internal = ok ? 0 : (ULONG_PTR)err; // completes at once
            ov->InternalHigh = got;
            if (ov->hEvent)
                SetEvent(ov->hEvent);
        }
        InterlockedExchangeAdd64(&g_bytes, got);
        SetLastError(err);
        return ok;
    }

    // New position for a seek, or false (error set) if it would be negative.
    bool Seek(HANDLE h, const VFile& v, int64_t distance, DWORD method, uint64_t* out) {
        int64_t base = method == FILE_BEGIN ? 0 : method == FILE_CURRENT ? (int64_t)v.pos : (int64_t)v.size;
        if (method > FILE_END) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        int64_t pos = base + distance;
        if (pos < 0) {
            SetLastError(ERROR_NEGATIVE_SEEK);
            return false;
        }
        SetPos(h, (uint64_t)pos);
        *out = (uint64_t)pos;
        SetLastError(ERROR_SUCCESS);
        return true;
    }

    DWORD WINAPI Hook_SetFilePointer(HANDLE h, LONG low, PLONG high, DWORD method) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_SetFilePointer(h, low, high, method);
        int64_t distance = high ? ((int64_t)*high << 32 | (uint32_t)low) : (int64_t)low;
        uint64_t pos;
        if (!Seek(h, v, distance, method, &pos))
            return INVALID_SET_FILE_POINTER;
        if (high)
            *high = (LONG)(pos >> 32);
        return (DWORD)pos;
    }

    BOOL WINAPI Hook_SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER out, DWORD method) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_SetFilePointerEx(h, distance, out, method);
        uint64_t pos;
        if (!Seek(h, v, distance.QuadPart, method, &pos))
            return FALSE;
        if (out)
            out->QuadPart = (LONGLONG)pos;
        return TRUE;
    }

    DWORD WINAPI Hook_GetFileSize(HANDLE h, LPDWORD high) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileSize(h, high);
        if (high)
            *high = (DWORD)(v.size >> 32);
        SetLastError(ERROR_SUCCESS);
        return (DWORD)v.size;
    }

    BOOL WINAPI Hook_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileSizeEx(h, size);
        size->QuadPart = (LONGLONG)v.size;
        return TRUE;
    }

    BOOL WINAPI Hook_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileInformationByHandle(h, info);
        if (!real_GetFileInformationByHandle(h, info)) // the archive's volume serial etc.
            memset(info, 0, sizeof(*info));
        info->dwFileAttributes = v.attributes;
        info->ftCreationTime = info->ftLastAccessTime = info->ftLastWriteTime = v.written;
        info->nFileSizeHigh = (DWORD)(v.size >> 32);
        info->nFileSizeLow = (DWORD)v.size;
        info->nNumberOfLinks = 1;
        // A file index of its own, so code that compares indices sees different files
        info->nFileIndexHigh ^= 0x80000000u | (DWORD)(uintptr_t)v.item->archive;
        info->nFileIndexLow ^= v.item->index + 1;
        return TRUE;
    }

    BOOL WINAPI Hook_GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buffer, DWORD size) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileInformationByHandleEx(h, cls, buffer, size);
        if (cls == FileStandardInfo && size >= sizeof(FILE_STANDARD_INFO)) {
            FILE_STANDARD_INFO* s = (FILE_STANDARD_INFO*)buffer;
            s->AllocationSize.QuadPart = (LONGLONG)v.size;
            s->EndOfFile.QuadPart = (LONGLONG)v.size;
            s->NumberOfLinks = 1;
            s->DeletePending = FALSE;
            s->Directory = FALSE;
            return TRUE;
        }
        if (cls == FileBasicInfo && size >= sizeof(FILE_BASIC_INFO)) {
            FILE_BASIC_INFO* b = (FILE_BASIC_INFO*)buffer;
            b->CreationTime.LowPart = b->LastAccessTime.LowPart = b->LastWriteTime.LowPart = b->ChangeTime.LowPart = v.written.dwLowDateTime;
            b->CreationTime.HighPart = b->LastAccessTime.HighPart = b->LastWriteTime.HighPart = b->ChangeTime.HighPart = (LONG)v.written.dwHighDateTime;
            b->FileAttributes = v.attributes;
            return TRUE;
        }
        return real_GetFileInformationByHandleEx(h, cls, buffer, size);
    }

    BOOL WINAPI Hook_GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileTime(h, created, accessed, written);
        if (created) *created = v.written;
        if (accessed) *accessed = v.written;
        if (written) *written = v.written;
        return TRUE;
    }

    BOOL WINAPI Hook_CloseHandle(HANDLE h) {
        if (g_vfile_count) {
            AcquireSRWLockExclusive(&g_vfile_lock);
            if (g_vfiles.erase(h))
                g_vfile_count--;
            ReleaseSRWLockExclusive(&g_vfile_lock);
        }
        return real_CloseHandle(h);
    }

    // Mapping a member: an unnamed memory section holding a copy of it (mapping the
    // archive handle would show the wrong bytes).
    HANDLE MapMember(const VFile& v, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD max_high, DWORD max_low, bool* handled) {
        *handled = true;
        uint64_t size = ((uint64_t)max_high << 32 | max_low) ? ((uint64_t)max_high << 32 | max_low) : v.size;
        if (!size || (protect & 0xFF) == PAGE_READWRITE || (protect & 0xFF) == PAGE_EXECUTE_READWRITE) {
            SetLastError(!size ? ERROR_FILE_INVALID : ERROR_ACCESS_DENIED);
            return nullptr;
        }
        HANDLE m = real_CreateFileMappingW(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, nullptr);
        if (!m)
            return nullptr;
        if (void* view = MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, (SIZE_T)size)) {
            uint64_t done = 0, want = std::min(size, v.size);
            while (done < want) {
                DWORD chunk = (DWORD)std::min<uint64_t>(want - done, 64u << 20);
                if (!ReadAt(v.item->archive->file, v.data + done, (uint8_t*)view + done, chunk))
                    break;
                done += chunk;
            }
            UnmapViewOfFile(view);
            if (done == want)
                return m;
        }
        DWORD e = GetLastError();
        real_CloseHandle(m);
        SetLastError(e ? e : ERROR_READ_FAULT);
        return nullptr;
    }

    HANDLE WINAPI Hook_CreateFileMappingW(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name) {
        VFile v;
        if (h && h != INVALID_HANDLE_VALUE && !name && GetVFile(h, &v)) {
            bool handled;
            return MapMember(v, sa, protect, hi, lo, &handled);
        }
        return real_CreateFileMappingW(h, sa, protect, hi, lo, name);
    }

    HANDLE WINAPI Hook_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCSTR name) {
        VFile v;
        if (h && h != INVALID_HANDLE_VALUE && !name && GetVFile(h, &v)) {
            bool handled;
            return MapMember(v, sa, protect, hi, lo, &handled);
        }
        return real_CreateFileMappingA(h, sa, protect, hi, lo, name);
    }

    // ------------------------------------------------------------ patching

    enum HookId {
        H_FindFirstFileExW, H_FindFirstFileExA, H_FindNextFileW, H_FindNextFileA, H_FindClose,
        H_GetFileAttributesW, H_GetFileAttributesA, H_GetFileAttributesExW, H_GetFileAttributesExA,
        H_CreateFileW, H_CreateFileA, H_ReadFile, H_SetFilePointer, H_SetFilePointerEx, H_GetFileSize,
        H_GetFileSizeEx, H_GetFileInformationByHandle, H_GetFileInformationByHandleEx, H_GetFileTime,
        H_CloseHandle, H_CreateFileMappingW, H_CreateFileMappingA, H_COUNT
    };

    struct HookDef { const char* name; void** real; void* hook; void* lookups_only; };
    HookDef g_hooks[H_COUNT] = {
        { "FindFirstFileExW", (void**)&real_FindFirstFileExW, (void*)&Hook_FindFirstFileExW, (void*)&Hook_FindFirstFileExW_Lookups },
        { "FindFirstFileExA", (void**)&real_FindFirstFileExA, (void*)&Hook_FindFirstFileExA, (void*)&Hook_FindFirstFileExA_Lookups },
        { "FindNextFileW", (void**)&real_FindNextFileW, (void*)&Hook_FindNextFileW, nullptr },
        { "FindNextFileA", (void**)&real_FindNextFileA, (void*)&Hook_FindNextFileA, nullptr },
        { "FindClose", (void**)&real_FindClose, (void*)&Hook_FindClose, nullptr },
        { "GetFileAttributesW", (void**)&real_GetFileAttributesW, (void*)&Hook_GetFileAttributesW, nullptr },
        { "GetFileAttributesA", (void**)&real_GetFileAttributesA, (void*)&Hook_GetFileAttributesA, nullptr },
        { "GetFileAttributesExW", (void**)&real_GetFileAttributesExW, (void*)&Hook_GetFileAttributesExW, nullptr },
        { "GetFileAttributesExA", (void**)&real_GetFileAttributesExA, (void*)&Hook_GetFileAttributesExA, nullptr },
        { "CreateFileW", (void**)&real_CreateFileW, (void*)&Hook_CreateFileW, (void*)&Hook_CreateFileW_Lookups },
        { "CreateFileA", (void**)&real_CreateFileA, (void*)&Hook_CreateFileA, (void*)&Hook_CreateFileA_Lookups },
        { "ReadFile", (void**)&real_ReadFile, (void*)&Hook_ReadFile, nullptr },
        { "SetFilePointer", (void**)&real_SetFilePointer, (void*)&Hook_SetFilePointer, nullptr },
        { "SetFilePointerEx", (void**)&real_SetFilePointerEx, (void*)&Hook_SetFilePointerEx, nullptr },
        { "GetFileSize", (void**)&real_GetFileSize, (void*)&Hook_GetFileSize, nullptr },
        { "GetFileSizeEx", (void**)&real_GetFileSizeEx, (void*)&Hook_GetFileSizeEx, nullptr },
        { "GetFileInformationByHandle", (void**)&real_GetFileInformationByHandle, (void*)&Hook_GetFileInformationByHandle, nullptr },
        { "GetFileInformationByHandleEx", (void**)&real_GetFileInformationByHandleEx, (void*)&Hook_GetFileInformationByHandleEx, nullptr },
        { "GetFileTime", (void**)&real_GetFileTime, (void*)&Hook_GetFileTime, nullptr },
        { "CloseHandle", (void**)&real_CloseHandle, (void*)&Hook_CloseHandle, nullptr },
        { "CreateFileMappingW", (void**)&real_CreateFileMappingW, (void*)&Hook_CreateFileMappingW, nullptr },
        { "CreateFileMappingA", (void**)&real_CreateFileMappingA, (void*)&Hook_CreateFileMappingA, nullptr },
    };

    std::vector<HMODULE> g_seen;

    bool IsKernelDll(const char* dll) {
        return !_stricmp(dll, "kernel32.dll") || !_stricmp(dll, "kernelbase.dll") || !_strnicmp(dll, "api-ms-win-core-", 16);
    }

    int PatchModule(HMODULE module, int* skipped, std::string* partial) {
        uint8_t* base = (uint8_t*)module;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return 0;

        struct Slot { ULONG_PTR* where; int hook; };
        std::vector<Slot> slots;
        bool have[H_COUNT] = {};
        for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
            if (!IsKernelDll((const char*)(base + imp->Name)) || !imp->OriginalFirstThunk)
                continue;
            IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
            IMAGE_THUNK_DATA* thunks = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; names++, thunks++) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                    continue;
                const char* name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
                for (int i = 0; i < H_COUNT; i++) {
                    if (strcmp(name, g_hooks[i].name))
                        continue;
                    void* current = (void*)thunks->u1.Function;
                    if (current != *g_hooks[i].real) {
                        if (current != g_hooks[i].hook && current != g_hooks[i].lookups_only)
                            (*skipped)++; // someone else's hook: leave it
                        break;
                    }
                    slots.push_back({ &thunks->u1.Function, i });
                    have[i] = true;
                    break;
                }
            }
        }

        // What this module can be given: our find handles need its FindClose and FindNextFile*
        // hooked, archive members its ReadFile and CloseHandle.
        bool find_w = have[H_FindClose] && have[H_FindNextFileW];
        bool find_a = have[H_FindClose] && have[H_FindNextFileA];
        bool members = have[H_ReadFile] && have[H_CloseHandle];
        int patched = 0;
        for (const Slot& s : slots) {
            void* hook = g_hooks[s.hook].hook;
            if ((s.hook == H_FindFirstFileExW && !find_w) || (s.hook == H_FindFirstFileExA && !find_a)
                || ((s.hook == H_CreateFileW || s.hook == H_CreateFileA) && !members)) {
                hook = g_hooks[s.hook].lookups_only;
                *partial += std::string(*partial == "" ? "" : ", ") + g_hooks[s.hook].name;
            }
            DWORD old;
            VirtualProtect(s.where, sizeof(void*), PAGE_READWRITE, &old);
            *s.where = (ULONG_PTR)hook;
            VirtualProtect(s.where, sizeof(void*), old, &old);
            patched++;
        }
        return patched;
    }

    DWORD WINAPI ReporterThread(void*) {
        LONG last = -1;
        for (;;) {
            Sleep(10000);
            LONG now = g_misses + g_hits + g_passed + g_opened;
            if (now == last)
                continue;
            last = now;
            Log("Files: %ld misses and %ld hits answered from memory, %ld lookups passed to Wine, %ld folders "
                "listed in %.0f ms; %ld searches answered; %ld archive members opened, %.1f MB read from archives",
                (long)g_misses, (long)g_hits, (long)g_passed, (long)g_listed, g_list_ms, (long)g_enumerated,
                (long)g_opened, g_bytes / 1048576.0);
        }
    }
}

extern "C" void divalinux_vfs_patch_modules() {
    if (!g_enabled)
        return;
    wchar_t windir[MAX_PATH];
    UINT wn = GetWindowsDirectoryW(windir, MAX_PATH);
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;
    DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; i++) {
        HMODULE m = mods[i];
        if (m == g_self || std::find(g_seen.begin(), g_seen.end(), m) != g_seen.end())
            continue;
        g_seen.push_back(m);
        wchar_t path[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(m, path, (DWORD)(sizeof(path) / sizeof(path[0])));
        if (!n || (wn && n > wn && !_wcsnicmp(path, windir, wn) && (path[wn] == L'\\' || path[wn] == L'/')))
            continue; // system DLLs
        if (n <= g_base.size() || _wcsnicmp(path, g_base.c_str(), g_base.size()))
            continue; // only the game and its plugins: not overlays (OBS, Fences) or drivers loaded into it
        int skipped = 0;
        std::string partial;
        int patched = PatchModule(m, &skipped, &partial);
        if (patched || skipped) {
            const wchar_t* base = wcsrchr(path, L'\\');
            Log("Files: %d functions hooked in %s%s%s%s", patched, Narrow(base ? base + 1 : path).c_str(),
                skipped ? " (some already hooked by another plugin; those are left alone)" : "",
                partial.empty() ? "" : "; lookups only for ", partial.c_str());
        }
    }
}

extern "C" void divalinux_vfs_install(HMODULE self, const wchar_t* config_file) {
    if (g_enabled)
        return;
    g_cache = GetPrivateProfileIntW(L"loading", L"lookup_cache", 1, config_file) > 0;
    g_archives = GetPrivateProfileIntW(L"loading", L"archives", 1, config_file) > 0;
    if (!g_cache && !g_archives)
        return;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    for (HookDef& h : g_hooks)
        if (!(*h.real = (void*)GetProcAddress(k32, h.name))) {
            Log("File layer off: kernel32 has no %s", h.name);
            return;
        }
    g_find_base = (uint8_t*)VirtualAlloc(nullptr, FIND_SLOTS * 16, MEM_RESERVE, PAGE_NOACCESS);

    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    wchar_t full[MAX_PATH * 2];
    DWORD fn = GetFullPathNameW(std::wstring(exe, n).c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    std::wstring real(full, fn);
    size_t slash = real.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        Log("File layer off: couldn't find the game folder");
        return;
    }
    g_base_real = real.substr(0, slash);
    g_base_dir = g_base_real;
    Lower(g_base_dir);
    g_base = g_base_dir + L"\\";

    if (g_archives)
        FindArchives();
    size_t loaded = 0;
    for (Archive* a : g_archive_list)
        loaded += a->ok;
    if (!g_cache && !loaded)
        return; // nothing to do

    g_enabled = true;
    g_self = self;
    Log("Files: lookup cache %s, %zu archive%s in use ([loading] lookup_cache / archives)", g_cache ? "on" : "off",
        loaded, loaded == 1 ? "" : "s");
    divalinux_vfs_patch_modules();
    CloseHandle(CreateThread(nullptr, 0, ReporterThread, nullptr, 0, nullptr));
}

// ---------------------------------------------------------------- DivaLinux's own file access
//
// For code inside DivaLinux that opens game files itself (the FFmpeg movie player): the same
// view the game gets, archive members included. With the file layer off, plain kernel32.
// Read-only, sequential or seeking; these are what FFmpeg's I/O callbacks need.

extern "C" HANDLE divalinux_file_open(const wchar_t* path) {
    if (!g_enabled)
        return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
    return Hook_CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
}

extern "C" BOOL divalinux_file_read(HANDLE h, void* buffer, DWORD size, DWORD* read) {
    return g_enabled ? Hook_ReadFile(h, buffer, size, read, nullptr) : ReadFile(h, buffer, size, read, nullptr);
}

// whence: FILE_BEGIN, FILE_CURRENT or FILE_END. Returns the new position, or -1.
extern "C" long long divalinux_file_seek(HANDLE h, long long offset, DWORD whence) {
    LARGE_INTEGER d, out;
    d.QuadPart = offset;
    BOOL ok = g_enabled ? Hook_SetFilePointerEx(h, d, &out, whence) : SetFilePointerEx(h, d, &out, whence);
    return ok ? (long long)out.QuadPart : -1;
}

extern "C" long long divalinux_file_size(HANDLE h) {
    LARGE_INTEGER s;
    BOOL ok = g_enabled ? Hook_GetFileSizeEx(h, &s) : GetFileSizeEx(h, &s);
    return ok ? (long long)s.QuadPart : -1;
}

extern "C" void divalinux_file_close(HANDLE h) {
    if (g_enabled)
        Hook_CloseHandle(h);
    else
        CloseHandle(h);
}
