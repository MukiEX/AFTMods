// DivaLinux: file lookup cache for rom\ and mdata\ ([loading] lookup_cache, on by default).
//
// While loading, the game asks every mdata pack for every file it might override, one
// FindFirstFileExW per pack per file: tens of thousands of lookups for a start-up or a song
// load, most of which find nothing. On Windows each costs microseconds. Under Wine every one
// is a directory scan (Windows names ignore case, the Linux filesystem doesn't), and under
// FEX on a Quest that's around 5 ms: a [debug] io_trace log showed 200 s of a 160 s start-up
// (summed over the loading threads) spent in lookups that missed, and under 2 s reading data.
//
// So each folder under rom\ and mdata\ is listed once, the first time something in it is
// looked for, and FindFirstFileExW / GetFileAttributes(Ex)W on single names there are answered
// from memory: a miss fails at once with the error Windows would give, a hit returns the
// entry's details (with a find handle that FindNextFileW/FindClose recognise). Wildcard
// searches, other folders and anything unusual go to Wine as before.
//
// Only import slots that still hold kernel32's own function are hooked: if another plugin
// already redirects a function (a virtual file system, say), its view wins and the cache
// stays out of that module's way. A file created in a cached folder through CreateFileW drops
// that folder's listing, so the next lookup sees it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <stdio.h>
#include <wchar.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

extern "C" void divavk_log_text(const char* text);

namespace {
    struct Entry {
        std::wstring name;   // as on disk
        DWORD attributes;
        FILETIME created, accessed, written;
        DWORD size_high, size_low;
    };

    struct Dir {
        bool exists = false;
        bool unknown = false; // couldn't be listed: lookups in it go to Wine
        std::wstring real_path;                          // as on disk, no trailing backslash
        std::unordered_map<std::wstring, Entry> entries; // lower-case name -> entry
    };

    typedef HANDLE (WINAPI* P_FindFirstFileExW)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    typedef BOOL (WINAPI* P_FindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
    typedef BOOL (WINAPI* P_FindClose)(HANDLE);
    typedef DWORD (WINAPI* P_GetFileAttributesW)(LPCWSTR);
    typedef BOOL (WINAPI* P_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    typedef HANDLE (WINAPI* P_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

    P_FindFirstFileExW real_FindFirstFileExW;
    P_FindNextFileW real_FindNextFileW;
    P_FindClose real_FindClose;
    P_GetFileAttributesW real_GetFileAttributesW;
    P_GetFileAttributesExW real_GetFileAttributesExW;
    P_CreateFileW real_CreateFileW;

    bool g_enabled = false;
    HMODULE g_self = nullptr;
    std::wstring g_base;        // game folder, lower case, trailing backslash
    std::wstring g_base_dir;    // the same without the trailing backslash
    std::wstring g_base_real;   // the game folder as on disk, no trailing backslash

    SRWLOCK g_lock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, Dir> g_dirs; // lower-case absolute path -> listing

    // Statistics (approximate: updated without a lock)
    volatile LONG g_misses = 0, g_hits = 0, g_passed = 0, g_listed = 0, g_dropped = 0;
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
        char buf[512];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        divavk_log_text(buf);
    }

    void Lower(std::wstring& s) {
        for (wchar_t& c : s)
            c = (wchar_t)towlower(c);
    }

    // ------------------------------------------------------------ paths

    // Absolute, lower case, backslashes, split into folder and name. False for anything the
    // cache doesn't answer: wildcards, a trailing backslash, outside rom\ and mdata\.
    bool Resolve(const wchar_t* path, std::wstring& dir, std::wstring& name) {
        if (!path || !*path || wcspbrk(path, L"*?<>\""))
            return false;
        if (!wcsncmp(path, L"\\\\?\\", 4) || !wcsncmp(path, L"\\\\.\\", 4))
            return false;
        size_t len = wcslen(path);
        if (path[len - 1] == L'\\' || path[len - 1] == L'/')
            return false;
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetFullPathNameW(path, (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
        if (!n || n >= sizeof(buf) / sizeof(buf[0]))
            return false;
        std::wstring full(buf, n);
        Lower(full);
        if (full.compare(0, g_base.size(), g_base) != 0)
            return false;
        std::wstring rest = full.substr(g_base.size());
        if (!(rest == L"rom" || rest == L"mdata" || !rest.compare(0, 4, L"rom\\") || !rest.compare(0, 6, L"mdata\\")))
            return false;
        size_t slash = full.find_last_of(L'\\');
        dir = full.substr(0, slash);
        name = full.substr(slash + 1);
        return !name.empty() && name != L"." && name != L"..";
    }

    // ------------------------------------------------------------ listings

    void List(Dir& d) {
        double t0 = NowMs();
        WIN32_FIND_DATAW fd;
        HANDLE h = real_FindFirstFileExW((d.real_path + L"\\*").c_str(), FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            // Empty (no entries at all) is fine; anything else (access denied, ...) isn't known
            DWORD e = GetLastError();
            d.exists = e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES;
            d.unknown = !d.exists;
            g_list_ms += NowMs() - t0;
            return;
        }
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
            e.size_high = fd.nFileSizeHigh;
            e.size_low = fd.nFileSizeLow;
            std::wstring key = e.name;
            Lower(key);
            d.entries.emplace(std::move(key), std::move(e));
        } while (real_FindNextFileW(h, &fd));
        real_FindClose(h);
        InterlockedIncrement(&g_listed);
        g_list_ms += NowMs() - t0;
    }

    // The listing of folder `dir` (lower case), made on first use. Caller holds g_lock
    // exclusively. Parents are listed first, so a folder that doesn't exist costs nothing.
    Dir& GetDirLocked(const std::wstring& dir) {
        auto it = g_dirs.find(dir);
        if (it != g_dirs.end())
            return it->second;
        Dir d;
        if (dir == g_base_dir) {
            d.real_path = g_base_real;
            List(d);
        }
        else {
            size_t slash = dir.find_last_of(L'\\');
            Dir& parent = GetDirLocked(dir.substr(0, slash));
            auto e = parent.exists ? parent.entries.find(dir.substr(slash + 1)) : parent.entries.end();
            if (parent.unknown)
                d.unknown = true;
            else if (e != parent.entries.end() && (e->second.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                d.real_path = parent.real_path + L"\\" + e->second.name;
                List(d);
            }
        }
        return g_dirs.emplace(dir, std::move(d)).first->second;
    }

    enum Answer { PASS, FOUND, MISSING_FILE, MISSING_PATH };

    Answer Lookup(const wchar_t* path, Entry& out) {
        std::wstring dir, name;
        if (!Resolve(path, dir, name))
            return PASS;
        AcquireSRWLockShared(&g_lock);
        auto it = g_dirs.find(dir);
        bool cached = it != g_dirs.end();
        Answer a = PASS;
        if (cached) {
            const Dir& d = it->second;
            auto e = d.exists ? d.entries.find(name) : d.entries.end();
            a = d.unknown ? PASS : !d.exists ? MISSING_PATH : e == d.entries.end() ? MISSING_FILE : FOUND;
            if (a == FOUND)
                out = e->second;
        }
        ReleaseSRWLockShared(&g_lock);
        if (cached)
            return a;

        AcquireSRWLockExclusive(&g_lock);
        const Dir& d = GetDirLocked(dir);
        auto e = d.exists ? d.entries.find(name) : d.entries.end();
        a = d.unknown ? PASS : !d.exists ? MISSING_PATH : e == d.entries.end() ? MISSING_FILE : FOUND;
        if (a == FOUND)
            out = e->second;
        ReleaseSRWLockExclusive(&g_lock);
        return a;
    }

    void Drop(const wchar_t* path) {
        std::wstring dir, name;
        if (!Resolve(path, dir, name))
            return;
        AcquireSRWLockExclusive(&g_lock);
        // The folder itself, and the file if it is a folder that was listed
        if (g_dirs.erase(dir))
            InterlockedIncrement(&g_dropped);
        g_dirs.erase(dir + L"\\" + name);
        ReleaseSRWLockExclusive(&g_lock);
    }

    // ------------------------------------------------------------ find handles
    //
    // Handles for answered FindFirstFileExW calls come from a reserved, never-committed
    // address range, so they can't collide with anything Wine hands out.

    uint8_t* g_handle_base = nullptr;
    const size_t HANDLE_SLOTS = 4096;
    volatile LONG g_handle_used[HANDLE_SLOTS];
    volatile LONG g_handle_next = 0;

    HANDLE NewHandle() {
        if (!g_handle_base)
            return nullptr;
        for (size_t tries = 0; tries < HANDLE_SLOTS; tries++) {
            size_t i = (size_t)InterlockedIncrement(&g_handle_next) % HANDLE_SLOTS;
            if (!InterlockedCompareExchange(&g_handle_used[i], 1, 0))
                return (HANDLE)(g_handle_base + i * 16);
        }
        return nullptr;
    }

    bool IsOurHandle(HANDLE h, size_t* index) {
        uint8_t* p = (uint8_t*)h;
        if (!g_handle_base || p < g_handle_base || p >= g_handle_base + HANDLE_SLOTS * 16 || (p - g_handle_base) % 16)
            return false;
        *index = (size_t)(p - g_handle_base) / 16;
        return g_handle_used[*index] != 0;
    }

    // ------------------------------------------------------------ hooks

    void Fill(WIN32_FIND_DATAW* fd, const Entry& e) {
        memset(fd, 0, sizeof(*fd));
        fd->dwFileAttributes = e.attributes;
        fd->ftCreationTime = e.created;
        fd->ftLastAccessTime = e.accessed;
        fd->ftLastWriteTime = e.written;
        fd->nFileSizeHigh = e.size_high;
        fd->nFileSizeLow = e.size_low;
        wcsncpy(fd->cFileName, e.name.c_str(), MAX_PATH - 1);
    }

    // answer_hits: the calling module's FindClose is hooked, so it can be given our handles.
    HANDLE FindFirst(bool answer_hits, LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
        FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
        if (data && !filter && op == FindExSearchNameMatch && !(flags & FIND_FIRST_EX_CASE_SENSITIVE)
            && (level == FindExInfoStandard || level == FindExInfoBasic)) {
            Entry e;
            switch (Lookup(path, e)) {
            case MISSING_FILE:
                InterlockedIncrement(&g_misses);
                SetLastError(ERROR_FILE_NOT_FOUND);
                return INVALID_HANDLE_VALUE;
            case MISSING_PATH:
                InterlockedIncrement(&g_misses);
                SetLastError(ERROR_PATH_NOT_FOUND);
                return INVALID_HANDLE_VALUE;
            case FOUND:
                if (HANDLE h = answer_hits ? NewHandle() : nullptr) {
                    InterlockedIncrement(&g_hits);
                    Fill((WIN32_FIND_DATAW*)data, e);
                    SetLastError(ERROR_SUCCESS);
                    return h;
                }
                break;
            default:
                break;
            }
        }
        InterlockedIncrement(&g_passed);
        return real_FindFirstFileExW(path, level, data, op, filter, flags);
    }

    HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
        FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
        return FindFirst(true, path, level, data, op, filter, flags);
    }

    HANDLE WINAPI Hook_FindFirstFileExW_MissesOnly(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
        FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
        return FindFirst(false, path, level, data, op, filter, flags);
    }

    BOOL WINAPI Hook_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data) {
        size_t i;
        if (IsOurHandle(h, &i)) {
            SetLastError(ERROR_NO_MORE_FILES); // a single name matches only itself
            return FALSE;
        }
        return real_FindNextFileW(h, data);
    }

    BOOL WINAPI Hook_FindClose(HANDLE h) {
        size_t i;
        if (IsOurHandle(h, &i)) {
            InterlockedExchange(&g_handle_used[i], 0);
            return TRUE;
        }
        return real_FindClose(h);
    }

    DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR path) {
        Entry e;
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
            InterlockedIncrement(&g_passed);
            return real_GetFileAttributesW(path);
        }
    }

    BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
        if (level == GetFileExInfoStandard && info) {
            Entry e;
            switch (Lookup(path, e)) {
            case FOUND: {
                InterlockedIncrement(&g_hits);
                WIN32_FILE_ATTRIBUTE_DATA* d = (WIN32_FILE_ATTRIBUTE_DATA*)info;
                d->dwFileAttributes = e.attributes;
                d->ftCreationTime = e.created;
                d->ftLastAccessTime = e.accessed;
                d->ftLastWriteTime = e.written;
                d->nFileSizeHigh = e.size_high;
                d->nFileSizeLow = e.size_low;
                return TRUE;
            }
            case MISSING_FILE:
                InterlockedIncrement(&g_misses);
                SetLastError(ERROR_FILE_NOT_FOUND);
                return FALSE;
            case MISSING_PATH:
                InterlockedIncrement(&g_misses);
                SetLastError(ERROR_PATH_NOT_FOUND);
                return FALSE;
            default:
                break;
            }
        }
        InterlockedIncrement(&g_passed);
        return real_GetFileAttributesExW(path, level, info);
    }

    // Opening files goes to Wine as before; creating one in a cached folder drops its listing.
    HANDLE WINAPI Hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
        DWORD disposition, DWORD flags, HANDLE templ) {
        HANDLE h = real_CreateFileW(path, access, share, sa, disposition, flags, templ);
        if (h != INVALID_HANDLE_VALUE && disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING) {
            DWORD e = GetLastError();
            Drop(path);
            SetLastError(e);
        }
        return h;
    }

    // ------------------------------------------------------------ patching

    struct HookDef { const char* name; void** real; void* hook; };
    HookDef g_hooks[] = {
        { "FindFirstFileExW", (void**)&real_FindFirstFileExW, (void*)&Hook_FindFirstFileExW },
        { "FindNextFileW", (void**)&real_FindNextFileW, (void*)&Hook_FindNextFileW },
        { "FindClose", (void**)&real_FindClose, (void*)&Hook_FindClose },
        { "GetFileAttributesW", (void**)&real_GetFileAttributesW, (void*)&Hook_GetFileAttributesW },
        { "GetFileAttributesExW", (void**)&real_GetFileAttributesExW, (void*)&Hook_GetFileAttributesExW },
        { "CreateFileW", (void**)&real_CreateFileW, (void*)&Hook_CreateFileW },
    };

    std::vector<HMODULE> g_seen;

    bool IsKernelDll(const char* dll) {
        return !_stricmp(dll, "kernel32.dll") || !_stricmp(dll, "kernelbase.dll")
            || !_strnicmp(dll, "api-ms-win-core-file-", 21);
    }

    // A module's FindFirstFileExW gets found entries answered only if its FindClose goes
    // through the hook too (it must release our handles); otherwise only misses are answered.
    int PatchModule(HMODULE module, int* skipped) {
        uint8_t* base = (uint8_t*)module;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return 0;

        struct Slot { ULONG_PTR* where; int hook; };
        std::vector<Slot> slots;
        bool close = false;
        for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
            if (!IsKernelDll((const char*)(base + imp->Name)) || !imp->OriginalFirstThunk)
                continue;
            IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
            IMAGE_THUNK_DATA* thunks = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; names++, thunks++) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                    continue;
                const char* name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
                for (int i = 0; i < (int)(sizeof(g_hooks) / sizeof(g_hooks[0])); i++) {
                    if (strcmp(name, g_hooks[i].name))
                        continue;
                    if ((void*)thunks->u1.Function != *g_hooks[i].real) {
                        if ((void*)thunks->u1.Function != g_hooks[i].hook
                            && (void*)thunks->u1.Function != (void*)&Hook_FindFirstFileExW_MissesOnly)
                            (*skipped)++; // someone else's hook: leave it
                        break;
                    }
                    slots.push_back({ &thunks->u1.Function, i });
                    close |= i == 2;
                    break;
                }
            }
        }

        int patched = 0;
        for (const Slot& s : slots) {
            void* hook = s.hook == 0 && !close ? (void*)&Hook_FindFirstFileExW_MissesOnly : g_hooks[s.hook].hook;
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
            LONG now = g_misses + g_hits + g_passed;
            if (now == last)
                continue;
            last = now;
            Log("Lookup cache: %ld misses and %ld hits answered from memory, %ld lookups passed to Wine; "
                "%ld folders listed in %.0f ms, %ld listings dropped",
                (long)g_misses, (long)g_hits, (long)g_passed, (long)g_listed, g_list_ms, (long)g_dropped);
        }
    }
}

extern "C" void divalinux_lookup_cache_patch_modules() {
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
        int skipped = 0;
        int patched = PatchModule(m, &skipped);
        if (patched || skipped) {
            const wchar_t* base = wcsrchr(path, L'\\');
            char label[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, label, sizeof(label), nullptr, nullptr);
            Log("Lookup cache: %d functions hooked in %s%s", patched, label,
                skipped ? " (some already hooked by another plugin; those are left alone)" : "");
        }
    }
}

extern "C" void divalinux_lookup_cache_install(HMODULE self, const wchar_t* config_file) {
    if (g_enabled || GetPrivateProfileIntW(L"loading", L"lookup_cache", 1, config_file) <= 0)
        return;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    for (HookDef& h : g_hooks)
        if (!(*h.real = (void*)GetProcAddress(k32, h.name))) {
            Log("Lookup cache off: kernel32 has no %s", h.name);
            return;
        }
    g_handle_base = (uint8_t*)VirtualAlloc(nullptr, HANDLE_SLOTS * 16, MEM_RESERVE, PAGE_NOACCESS);

    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    wchar_t full[MAX_PATH * 2];
    DWORD fn = GetFullPathNameW(std::wstring(exe, n).c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    std::wstring real(full, fn);
    size_t slash = real.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        Log("Lookup cache off: couldn't find the game folder");
        return;
    }
    g_base_real = real.substr(0, slash);
    g_base_dir = g_base_real;
    Lower(g_base_dir);
    g_base = g_base_dir + L"\\";

    g_enabled = true;
    g_self = self;
    Log("Lookup cache on: file lookups under rom\\ and mdata\\ are answered from folder listings ([loading] lookup_cache=0 turns this off)");
    divalinux_lookup_cache_patch_modules();
    CloseHandle(CreateThread(nullptr, 0, ReporterThread, nullptr, 0, nullptr));
}
