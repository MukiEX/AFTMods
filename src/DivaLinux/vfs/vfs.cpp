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

#include "vfs_internal.hpp"
#include "../common/iat_patch.hpp"
#include <psapi.h>

namespace vfs {
    // ------------------------------------------------------------ kernel32's own functions

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

    int PatchModule(HMODULE module, int* skipped, std::string* partial) {
        struct Slot { ULONG_PTR* where; int hook; };
        std::vector<Slot> slots;
        bool have[H_COUNT] = {};
        dl::for_each_import(module, dl::is_kernel_dll, [&](const char*, const char* name, ULONG_PTR* slot) {
            for (int i = 0; i < H_COUNT; i++) {
                if (strcmp(name, g_hooks[i].name))
                    continue;
                void* current = (void*)*slot;
                if (current != *g_hooks[i].real) {
                    if (current != g_hooks[i].hook && current != g_hooks[i].lookups_only)
                        (*skipped)++; // someone else's hook: leave it
                    return;
                }
                slots.push_back({ slot, i });
                have[i] = true;
                return;
            }
        });

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
            dl::write_slot(s.where, hook);
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
            dl::log("Files: %ld misses and %ld hits answered from memory, %ld lookups passed to Wine, %ld folders "
                "listed in %.0f ms; %ld searches answered; %ld archive members opened, %.1f MB read from archives",
                (long)g_misses, (long)g_hits, (long)g_passed, (long)g_listed, g_list_ms, (long)g_enumerated,
                (long)g_opened, g_bytes / 1048576.0);
        }
    }
}

using namespace vfs;

extern "C" void divalinux_vfs_patch_modules() {
    if (!g_enabled)
        return;
    // only the game and its plugins: not overlays (OBS, Fences) or drivers loaded into it
    dl::for_each_new_module(g_seen, g_self, g_base, [](HMODULE m, const wchar_t* path) {
        int skipped = 0;
        std::string partial;
        int patched = PatchModule(m, &skipped, &partial);
        if (patched || skipped)
            dl::log("Files: %d functions hooked in %s%s%s%s", patched, dl::narrow(dl::module_file_name(path)).c_str(),
                skipped ? " (some already hooked by another plugin; those are left alone)" : "",
                partial.empty() ? "" : "; lookups only for ", partial.c_str());
    });
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
            dl::log("File layer off: kernel32 has no %s", h.name);
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
        dl::log("File layer off: couldn't find the game folder");
        return;
    }
    g_base_real = real.substr(0, slash);
    g_base_dir = g_base_real;
    dl::lower(g_base_dir);
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
    dl::log("Files: lookup cache %s, %zu archive%s in use ([loading] lookup_cache / archives)", g_cache ? "on" : "off",
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
