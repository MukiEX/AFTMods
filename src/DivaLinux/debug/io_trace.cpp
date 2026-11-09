// DivaLinux: optional file I/O trace ([debug] io_trace=1 in DivaLinux.ini).
//
// Answers "where does loading spend its time?" on slow setups (a Quest running the game
// through FEX, a phone, a Steam Deck SD card): how many files are opened, looked up and
// searched for, how many of those lookups miss, how long Wine takes for each kind, which
// folders get the most traffic, and how much data is read.
//
// It hooks the file functions in the import tables of the game and its plugins (the same
// way vk_bridge.cpp redirects GL), never the system DLLs themselves. Each hook calls the
// function that was in that import slot before, so path redirection by other plugins keeps
// working. Activity comes in bursts (start-up, each song load); a summary of each burst is
// written to DivaLinux.log once the game has been quiet for two seconds.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <stdio.h>
#include <wchar.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

#include "../common/iat_patch.hpp"
#include "../common/util.hpp"

namespace {
    // ------------------------------------------------------------ hooked functions

    enum Fn {
        F_CreateFileA, F_CreateFileW, F_GetFileAttributesA, F_GetFileAttributesW,
        F_GetFileAttributesExA, F_GetFileAttributesExW, F_FindFirstFileA, F_FindFirstFileW,
        F_FindFirstFileExA, F_FindFirstFileExW, F_FindNextFileA, F_FindNextFileW, F_ReadFile,
        F_fopen, F_wfopen, F_fopen_s, F_wfopen_s, F_fsopen, F_wfsopen,
        F_stat, F_wstat, F_access, F_waccess, F_fread,
        F_COUNT
    };

    const char* const FN_NAMES[F_COUNT] = {
        "CreateFileA", "CreateFileW", "GetFileAttributesA", "GetFileAttributesW",
        "GetFileAttributesExA", "GetFileAttributesExW", "FindFirstFileA", "FindFirstFileW",
        "FindFirstFileExA", "FindFirstFileExW", "FindNextFileA", "FindNextFileW", "ReadFile",
        "fopen", "_wfopen", "fopen_s", "_wfopen_s", "_fsopen", "_wfsopen",
        "_stat", "_wstat", "_access", "_waccess", "fread",
    };

    // The import table spellings for each hook (the CRT has several _stat variants).
    struct ImportName { const char* name; Fn fn; };
    const ImportName IMPORTS[] = {
        { "CreateFileA", F_CreateFileA }, { "CreateFileW", F_CreateFileW },
        { "GetFileAttributesA", F_GetFileAttributesA }, { "GetFileAttributesW", F_GetFileAttributesW },
        { "GetFileAttributesExA", F_GetFileAttributesExA }, { "GetFileAttributesExW", F_GetFileAttributesExW },
        { "FindFirstFileA", F_FindFirstFileA }, { "FindFirstFileW", F_FindFirstFileW },
        { "FindFirstFileExA", F_FindFirstFileExA }, { "FindFirstFileExW", F_FindFirstFileExW },
        { "FindNextFileA", F_FindNextFileA }, { "FindNextFileW", F_FindNextFileW },
        { "ReadFile", F_ReadFile },
        { "fopen", F_fopen }, { "_wfopen", F_wfopen }, { "fopen_s", F_fopen_s }, { "_wfopen_s", F_wfopen_s },
        { "_fsopen", F_fsopen }, { "_wfsopen", F_wfsopen },
        { "_stat", F_stat }, { "_stat32", F_stat }, { "_stat64", F_stat }, { "_stat32i64", F_stat }, { "_stat64i32", F_stat },
        { "_wstat", F_wstat }, { "_wstat32", F_wstat }, { "_wstat64", F_wstat }, { "_wstat32i64", F_wstat }, { "_wstat64i32", F_wstat },
        { "_access", F_access }, { "_waccess", F_waccess },
        { "fread", F_fread },
    };

    bool IsFileDll(const char* dll) {
        return !_stricmp(dll, "kernel32.dll") || !_stricmp(dll, "kernelbase.dll")
            || !_strnicmp(dll, "api-ms-win-core-file-", 21)
            || !_stricmp(dll, "ucrtbase.dll") || !_strnicmp(dll, "api-ms-win-crt-", 15)
            || !_strnicmp(dll, "msvcr", 5) || !_stricmp(dll, "msvcrt.dll");
    }

    // ------------------------------------------------------------ originals
    //
    // Normally every module's slot holds the same function. If another plugin hooked one
    // module's slot, that module gets its own original, found from the caller's address.

    struct Original { HMODULE module; void* fn; };
    SRWLOCK g_orig_lock = SRWLOCK_INIT;
    std::vector<Original> g_orig[F_COUNT];
    void* g_shared[F_COUNT];     // the original when all modules agree
    bool g_mixed[F_COUNT];

    void* OriginalFor(Fn f, void* return_address) {
        if (!g_mixed[f])
            return g_shared[f];
        HMODULE caller = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)return_address, &caller);
        AcquireSRWLockShared(&g_orig_lock);
        void* fn = g_shared[f];
        for (const Original& o : g_orig[f])
            if (o.module == caller) {
                fn = o.fn;
                break;
            }
        ReleaseSRWLockShared(&g_orig_lock);
        return fn;
    }

    // ------------------------------------------------------------ statistics

    struct Counter {
        uint64_t calls = 0, misses = 0;
        double ms = 0.0, miss_ms = 0.0;
        void Add(bool miss, double t) {
            calls++;
            ms += t;
            if (miss) {
                misses++;
                miss_ms += t;
            }
        }
    };

    struct Slow { double ms; Fn fn; std::wstring path; };

    struct Burst {
        double first_ms = 0.0, last_ms = 0.0;
        Counter fn[F_COUNT];
        std::map<std::wstring, Counter> folders;
        std::unordered_map<std::wstring, uint32_t> missed_names;
        uint64_t bytes = 0;
        std::vector<Slow> slowest;
        bool active = false;
    };

    SRWLOCK g_stats_lock = SRWLOCK_INIT;
    Burst g_burst;
    std::wstring g_game_dir;   // lower case, backslashes, trailing backslash
    double g_start_ms = 0.0;
    int g_burst_number = 0;


    // Lower case, backslashes, relative to the game folder when inside it.
    std::wstring Normalize(const wchar_t* path) {
        std::wstring p = path ? path : L"";
        for (wchar_t& c : p) {
            if (c == L'/')
                c = L'\\';
            c = (wchar_t)towlower(c);
        }
        if (!g_game_dir.empty() && p.compare(0, g_game_dir.size(), g_game_dir) == 0)
            p.erase(0, g_game_dir.size());
        while (p.compare(0, 2, L".\\") == 0)
            p.erase(0, 2);
        return p;
    }

    // "mdata\<pack>" for mod data, otherwise the first folder ("rom", "ram", ...).
    std::wstring FolderOf(const std::wstring& p) {
        size_t a = p.find(L'\\');
        if (a == std::wstring::npos)
            return L"(game folder)";
        if (p.compare(0, a, L"mdata") == 0) {
            size_t b = p.find(L'\\', a + 1);
            return b == std::wstring::npos ? p : p.substr(0, b);
        }
        if (a == 2 && p[1] == L':') { // another drive: keep one more level
            size_t b = p.find(L'\\', a + 1);
            return b == std::wstring::npos ? p : p.substr(0, b);
        }
        return p.substr(0, a);
    }

    void Record(Fn f, const std::wstring& raw_path, bool miss, double t, uint64_t bytes = 0) {
        double now = dl::now_ms();
        std::wstring path = raw_path.empty() ? std::wstring() : Normalize(raw_path.c_str());
        AcquireSRWLockExclusive(&g_stats_lock);
        Burst& b = g_burst;
        if (!b.active) {
            b.active = true;
            b.first_ms = now - t;
        }
        b.last_ms = now;
        b.fn[f].Add(miss, t);
        b.bytes += bytes;
        if (!path.empty()) {
            b.folders[FolderOf(path)].Add(miss, t);
            if (miss) {
                size_t s = path.find_last_of(L'\\');
                b.missed_names[s == std::wstring::npos ? path : path.substr(s + 1)]++;
            }
            if (b.slowest.size() < 8 || t > b.slowest.back().ms) {
                b.slowest.push_back({ t, f, path });
                std::sort(b.slowest.begin(), b.slowest.end(), [](const Slow& x, const Slow& y) { return x.ms > y.ms; });
                if (b.slowest.size() > 8)
                    b.slowest.pop_back();
            }
        }
        ReleaseSRWLockExclusive(&g_stats_lock);
    }

    void Report(Burst& b) {
        uint64_t calls = 0, misses = 0;
        double ms = 0.0, miss_ms = 0.0;
        for (const Counter& c : b.fn) {
            calls += c.calls;
            misses += c.misses;
            ms += c.ms;
            miss_ms += c.miss_ms;
        }
        double read_ms = b.fn[F_ReadFile].ms + b.fn[F_fread].ms;
        g_burst_number++;
        dl::log("I/O trace, burst %d (%.1f s to %.1f s after start, %.1f s long): %llu calls taking %.1f s "
            "(%llu missed, %.1f s); %.1f MB read in %.1f s",
            g_burst_number, (b.first_ms - g_start_ms) / 1000.0, (b.last_ms - g_start_ms) / 1000.0,
            (b.last_ms - b.first_ms) / 1000.0, (unsigned long long)calls, ms / 1000.0,
            (unsigned long long)misses, miss_ms / 1000.0, b.bytes / 1048576.0, read_ms / 1000.0);

        for (int f = 0; f < F_COUNT; f++) {
            const Counter& c = b.fn[f];
            if (!c.calls)
                continue;
            dl::log("  %-20s %8llu calls %9.1f ms (%.3f ms each); %llu missed, %.1f ms (%.3f ms each)",
                FN_NAMES[f], (unsigned long long)c.calls, c.ms, c.ms / c.calls, (unsigned long long)c.misses,
                c.miss_ms, c.misses ? c.miss_ms / c.misses : 0.0);
        }

        std::vector<std::pair<std::wstring, Counter>> folders(b.folders.begin(), b.folders.end());
        std::sort(folders.begin(), folders.end(),
            [](const std::pair<std::wstring, Counter>& x, const std::pair<std::wstring, Counter>& y) { return x.second.ms > y.second.ms; });
        std::string line = "  busiest folders:";
        for (size_t i = 0; i < folders.size() && i < 12; i++) {
            char item[256];
            snprintf(item, sizeof(item), " %s %llu (%llu missed) %.0f ms;", dl::narrow(folders[i].first).c_str(),
                (unsigned long long)folders[i].second.calls, (unsigned long long)folders[i].second.misses, folders[i].second.ms);
            line += item;
        }
        dl::log("%s %zu folders in all", line.c_str(), folders.size());

        std::vector<std::pair<std::wstring, uint32_t>> names(b.missed_names.begin(), b.missed_names.end());
        std::sort(names.begin(), names.end(),
            [](const std::pair<std::wstring, uint32_t>& x, const std::pair<std::wstring, uint32_t>& y) { return x.second > y.second; });
        if (!names.empty()) {
            line = "  most-missed names:";
            for (size_t i = 0; i < names.size() && i < 12; i++) {
                char item[256];
                snprintf(item, sizeof(item), " %s x%u;", dl::narrow(names[i].first).c_str(), names[i].second);
                line += item;
            }
            dl::log("%s %zu different names missed", line.c_str(), names.size());
        }

        for (const Slow& s : b.slowest)
            dl::log("  slow: %.1f ms %s %s", s.ms, FN_NAMES[s.fn], dl::narrow(s.path).c_str());
    }

    // Writes a burst's summary once the game has been quiet for two seconds.
    DWORD WINAPI ReporterThread(void*) {
        for (;;) {
            Sleep(500);
            Burst done;
            bool report = false;
            AcquireSRWLockExclusive(&g_stats_lock);
            if (g_burst.active && dl::now_ms() - g_burst.last_ms > 2000.0) {
                done = std::move(g_burst);
                g_burst = Burst();
                report = true;
            }
            ReleaseSRWLockExclusive(&g_stats_lock);
            if (report)
                Report(done);
        }
    }

    // ------------------------------------------------------------ hooks

    template <typename T> T Orig(Fn f, void* ra) { return (T)OriginalFor(f, ra); }

    HANDLE WINAPI Hook_CreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD fl, HANDLE t) {
        double t0 = dl::now_ms();
        HANDLE h = Orig<decltype(&CreateFileA)>(F_CreateFileA, _ReturnAddress())(n, a, s, sa, c, fl, t);
        DWORD e = GetLastError();
        Record(F_CreateFileA, dl::widen_acp(n), h == INVALID_HANDLE_VALUE, dl::now_ms() - t0);
        SetLastError(e);
        return h;
    }
    HANDLE WINAPI Hook_CreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD fl, HANDLE t) {
        double t0 = dl::now_ms();
        HANDLE h = Orig<decltype(&CreateFileW)>(F_CreateFileW, _ReturnAddress())(n, a, s, sa, c, fl, t);
        DWORD e = GetLastError();
        Record(F_CreateFileW, n ? n : L"", h == INVALID_HANDLE_VALUE, dl::now_ms() - t0);
        SetLastError(e);
        return h;
    }
    DWORD WINAPI Hook_GetFileAttributesA(LPCSTR n) {
        double t0 = dl::now_ms();
        DWORD r = Orig<decltype(&GetFileAttributesA)>(F_GetFileAttributesA, _ReturnAddress())(n);
        DWORD e = GetLastError();
        Record(F_GetFileAttributesA, dl::widen_acp(n), r == INVALID_FILE_ATTRIBUTES, dl::now_ms() - t0);
        SetLastError(e);
        return r;
    }
    DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR n) {
        double t0 = dl::now_ms();
        DWORD r = Orig<decltype(&GetFileAttributesW)>(F_GetFileAttributesW, _ReturnAddress())(n);
        DWORD e = GetLastError();
        Record(F_GetFileAttributesW, n ? n : L"", r == INVALID_FILE_ATTRIBUTES, dl::now_ms() - t0);
        SetLastError(e);
        return r;
    }
    BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID i) {
        double t0 = dl::now_ms();
        BOOL r = Orig<decltype(&GetFileAttributesExA)>(F_GetFileAttributesExA, _ReturnAddress())(n, l, i);
        DWORD e = GetLastError();
        Record(F_GetFileAttributesExA, dl::widen_acp(n), !r, dl::now_ms() - t0);
        SetLastError(e);
        return r;
    }
    BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID i) {
        double t0 = dl::now_ms();
        BOOL r = Orig<decltype(&GetFileAttributesExW)>(F_GetFileAttributesExW, _ReturnAddress())(n, l, i);
        DWORD e = GetLastError();
        Record(F_GetFileAttributesExW, n ? n : L"", !r, dl::now_ms() - t0);
        SetLastError(e);
        return r;
    }
    HANDLE WINAPI Hook_FindFirstFileA(LPCSTR n, LPWIN32_FIND_DATAA d) {
        double t0 = dl::now_ms();
        HANDLE h = Orig<decltype(&FindFirstFileA)>(F_FindFirstFileA, _ReturnAddress())(n, d);
        DWORD e = GetLastError();
        Record(F_FindFirstFileA, dl::widen_acp(n), h == INVALID_HANDLE_VALUE, dl::now_ms() - t0);
        SetLastError(e);
        return h;
    }
    HANDLE WINAPI Hook_FindFirstFileW(LPCWSTR n, LPWIN32_FIND_DATAW d) {
        double t0 = dl::now_ms();
        HANDLE h = Orig<decltype(&FindFirstFileW)>(F_FindFirstFileW, _ReturnAddress())(n, d);
        DWORD e = GetLastError();
        Record(F_FindFirstFileW, n ? n : L"", h == INVALID_HANDLE_VALUE, dl::now_ms() - t0);
        SetLastError(e);
        return h;
    }
    HANDLE WINAPI Hook_FindFirstFileExA(LPCSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        double t0 = dl::now_ms();
        HANDLE h = Orig<decltype(&FindFirstFileExA)>(F_FindFirstFileExA, _ReturnAddress())(n, l, d, o, f, fl);
        DWORD e = GetLastError();
        Record(F_FindFirstFileExA, dl::widen_acp(n), h == INVALID_HANDLE_VALUE, dl::now_ms() - t0);
        SetLastError(e);
        return h;
    }
    HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        double t0 = dl::now_ms();
        HANDLE h = Orig<decltype(&FindFirstFileExW)>(F_FindFirstFileExW, _ReturnAddress())(n, l, d, o, f, fl);
        DWORD e = GetLastError();
        Record(F_FindFirstFileExW, n ? n : L"", h == INVALID_HANDLE_VALUE, dl::now_ms() - t0);
        SetLastError(e);
        return h;
    }
    BOOL WINAPI Hook_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA d) {
        double t0 = dl::now_ms();
        BOOL r = Orig<decltype(&FindNextFileA)>(F_FindNextFileA, _ReturnAddress())(h, d);
        DWORD e = GetLastError();
        Record(F_FindNextFileA, std::wstring(), false, dl::now_ms() - t0);
        SetLastError(e);
        return r;
    }
    BOOL WINAPI Hook_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW d) {
        double t0 = dl::now_ms();
        BOOL r = Orig<decltype(&FindNextFileW)>(F_FindNextFileW, _ReturnAddress())(h, d);
        DWORD e = GetLastError();
        Record(F_FindNextFileW, std::wstring(), false, dl::now_ms() - t0);
        SetLastError(e);
        return r;
    }
    BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD read, LPOVERLAPPED o) {
        double t0 = dl::now_ms();
        DWORD local = 0;
        BOOL r = Orig<decltype(&ReadFile)>(F_ReadFile, _ReturnAddress())(h, b, n, read ? read : (o ? nullptr : &local), o);
        DWORD e = GetLastError();
        Record(F_ReadFile, std::wstring(), !r && e != ERROR_IO_PENDING, dl::now_ms() - t0, r ? (read ? *read : local) : 0);
        SetLastError(e);
        return r;
    }

    // CRT: errno is preserved by not calling anything that sets it.
    typedef FILE* (__cdecl* P_fopen)(const char*, const char*);
    typedef FILE* (__cdecl* P_wfopen)(const wchar_t*, const wchar_t*);
    typedef int (__cdecl* P_fopen_s)(FILE**, const char*, const char*);
    typedef int (__cdecl* P_wfopen_s)(FILE**, const wchar_t*, const wchar_t*);
    typedef FILE* (__cdecl* P_fsopen)(const char*, const char*, int);
    typedef FILE* (__cdecl* P_wfsopen)(const wchar_t*, const wchar_t*, int);
    typedef int (__cdecl* P_stat)(const char*, void*);
    typedef int (__cdecl* P_wstat)(const wchar_t*, void*);
    typedef int (__cdecl* P_access)(const char*, int);
    typedef int (__cdecl* P_waccess)(const wchar_t*, int);
    typedef size_t (__cdecl* P_fread)(void*, size_t, size_t, FILE*);

    FILE* __cdecl Hook_fopen(const char* n, const char* m) {
        double t0 = dl::now_ms();
        FILE* f = Orig<P_fopen>(F_fopen, _ReturnAddress())(n, m);
        Record(F_fopen, dl::widen_acp(n), !f, dl::now_ms() - t0);
        return f;
    }
    FILE* __cdecl Hook_wfopen(const wchar_t* n, const wchar_t* m) {
        double t0 = dl::now_ms();
        FILE* f = Orig<P_wfopen>(F_wfopen, _ReturnAddress())(n, m);
        Record(F_wfopen, n ? n : L"", !f, dl::now_ms() - t0);
        return f;
    }
    int __cdecl Hook_fopen_s(FILE** o, const char* n, const char* m) {
        double t0 = dl::now_ms();
        int r = Orig<P_fopen_s>(F_fopen_s, _ReturnAddress())(o, n, m);
        Record(F_fopen_s, dl::widen_acp(n), r != 0, dl::now_ms() - t0);
        return r;
    }
    int __cdecl Hook_wfopen_s(FILE** o, const wchar_t* n, const wchar_t* m) {
        double t0 = dl::now_ms();
        int r = Orig<P_wfopen_s>(F_wfopen_s, _ReturnAddress())(o, n, m);
        Record(F_wfopen_s, n ? n : L"", r != 0, dl::now_ms() - t0);
        return r;
    }
    FILE* __cdecl Hook_fsopen(const char* n, const char* m, int s) {
        double t0 = dl::now_ms();
        FILE* f = Orig<P_fsopen>(F_fsopen, _ReturnAddress())(n, m, s);
        Record(F_fsopen, dl::widen_acp(n), !f, dl::now_ms() - t0);
        return f;
    }
    FILE* __cdecl Hook_wfsopen(const wchar_t* n, const wchar_t* m, int s) {
        double t0 = dl::now_ms();
        FILE* f = Orig<P_wfsopen>(F_wfsopen, _ReturnAddress())(n, m, s);
        Record(F_wfsopen, n ? n : L"", !f, dl::now_ms() - t0);
        return f;
    }
    int __cdecl Hook_stat(const char* n, void* st) {
        double t0 = dl::now_ms();
        int r = Orig<P_stat>(F_stat, _ReturnAddress())(n, st);
        Record(F_stat, dl::widen_acp(n), r != 0, dl::now_ms() - t0);
        return r;
    }
    int __cdecl Hook_wstat(const wchar_t* n, void* st) {
        double t0 = dl::now_ms();
        int r = Orig<P_wstat>(F_wstat, _ReturnAddress())(n, st);
        Record(F_wstat, n ? n : L"", r != 0, dl::now_ms() - t0);
        return r;
    }
    int __cdecl Hook_access(const char* n, int m) {
        double t0 = dl::now_ms();
        int r = Orig<P_access>(F_access, _ReturnAddress())(n, m);
        Record(F_access, dl::widen_acp(n), r != 0, dl::now_ms() - t0);
        return r;
    }
    int __cdecl Hook_waccess(const wchar_t* n, int m) {
        double t0 = dl::now_ms();
        int r = Orig<P_waccess>(F_waccess, _ReturnAddress())(n, m);
        Record(F_waccess, n ? n : L"", r != 0, dl::now_ms() - t0);
        return r;
    }
    size_t __cdecl Hook_fread(void* b, size_t s, size_t c, FILE* f) {
        double t0 = dl::now_ms();
        size_t r = Orig<P_fread>(F_fread, _ReturnAddress())(b, s, c, f);
        Record(F_fread, std::wstring(), false, dl::now_ms() - t0, (uint64_t)r * s);
        return r;
    }

    void* const HOOKS[F_COUNT] = {
        (void*)&Hook_CreateFileA, (void*)&Hook_CreateFileW, (void*)&Hook_GetFileAttributesA, (void*)&Hook_GetFileAttributesW,
        (void*)&Hook_GetFileAttributesExA, (void*)&Hook_GetFileAttributesExW, (void*)&Hook_FindFirstFileA, (void*)&Hook_FindFirstFileW,
        (void*)&Hook_FindFirstFileExA, (void*)&Hook_FindFirstFileExW, (void*)&Hook_FindNextFileA, (void*)&Hook_FindNextFileW,
        (void*)&Hook_ReadFile,
        (void*)&Hook_fopen, (void*)&Hook_wfopen, (void*)&Hook_fopen_s, (void*)&Hook_wfopen_s, (void*)&Hook_fsopen, (void*)&Hook_wfsopen,
        (void*)&Hook_stat, (void*)&Hook_wstat, (void*)&Hook_access, (void*)&Hook_waccess, (void*)&Hook_fread,
    };

    // ------------------------------------------------------------ patching

    bool g_enabled = false;
    HMODULE g_self = nullptr;
    std::vector<HMODULE> g_seen;

    int PatchModule(HMODULE module) {
        int patched = 0;
        dl::for_each_import(module, IsFileDll, [&](const char*, const char* name, ULONG_PTR* slot) {
            int f = -1;
            for (const ImportName& in : IMPORTS)
                if (!strcmp(in.name, name)) {
                    f = in.fn;
                    break;
                }
            // Two CRT _stat variants with different struct layouts must not share one
            // original: only the first spelling seen for F_stat/F_wstat is hooked.
            void* current = (void*)*slot;
            if (f < 0 || current == HOOKS[f])
                return;
            AcquireSRWLockExclusive(&g_orig_lock);
            bool conflict = false;
            if (!g_shared[f])
                g_shared[f] = current;
            else if (g_shared[f] != current) {
                if (f == F_stat || f == F_wstat)
                    conflict = true; // a different _stat variant: leave it alone
                else {
                    g_mixed[f] = true;
                    g_orig[f].push_back({ module, current });
                }
            }
            ReleaseSRWLockExclusive(&g_orig_lock);
            if (conflict)
                return;
            dl::write_slot(slot, HOOKS[f]);
            patched++;
        });
        return patched;
    }
}

// Hooks the file functions of every loaded module outside the Windows folder (the game,
// GLUT, PD Loader and its plugins). Safe to call again: modules already done are skipped.
extern "C" void divalinux_io_trace_patch_modules() {
    if (!g_enabled || g_game_dir.empty())
        return;
    // only the game and its plugins: not overlays (OBS, Fences) or drivers loaded into it
    dl::for_each_new_module(g_seen, g_self, g_game_dir, [](HMODULE m, const wchar_t* path) {
        int patched = PatchModule(m);
        if (patched)
            dl::log("I/O trace: %d file functions hooked in %s", patched, dl::narrow(dl::module_file_name(path)).c_str());
    });
}

extern "C" void divalinux_io_trace_install(HMODULE self, const wchar_t* config_file) {
    if (g_enabled || GetPrivateProfileIntW(L"debug", L"io_trace", 0, config_file) <= 0)
        return;
    g_enabled = true;
    g_self = self;
    g_start_ms = dl::now_ms();

    wchar_t dir[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, dir, (DWORD)(sizeof(dir) / sizeof(dir[0])));
    std::wstring d(dir, n);
    size_t slash = d.find_last_of(L"\\/");
    d = slash == std::wstring::npos ? std::wstring() : d.substr(0, slash + 1);
    for (wchar_t& c : d) {
        if (c == L'/')
            c = L'\\';
        c = (wchar_t)towlower(c);
    }
    g_game_dir = d;

    dl::log("I/O trace on ([debug] io_trace=1): a summary is logged after each burst of file activity");
    divalinux_io_trace_patch_modules();
    CloseHandle(CreateThread(nullptr, 0, ReporterThread, nullptr, 0, nullptr));
}
