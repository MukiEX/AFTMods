#include "iat_patch.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <psapi.h>

namespace dl {
    static IMAGE_IMPORT_DESCRIPTOR* first_import(HMODULE module) {
        uint8_t* base = (uint8_t*)module;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        return dir.VirtualAddress ? (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress) : nullptr;
    }

    void for_each_import(HMODULE module, bool (*dll_filter)(const char*), const ImportVisitor& visit) {
        if (!module)
            return;
        uint8_t* base = (uint8_t*)module;
        IMAGE_IMPORT_DESCRIPTOR* imp = first_import(module);
        if (!imp)
            return;
        for (; imp->Name; imp++) {
            const char* dll = (const char*)(base + imp->Name);
            if ((dll_filter && !dll_filter(dll)) || !imp->OriginalFirstThunk)
                continue; // not wanted, or bound imports without names (nothing to match)
            IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
            IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; names++, slots++) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                    continue;
                const char* name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
                visit(dll, name, &slots->u1.Function);
            }
        }
    }

    bool imports_from(HMODULE module, bool (*dll_filter)(const char*)) {
        IMAGE_IMPORT_DESCRIPTOR* imp = module ? first_import(module) : nullptr;
        if (!imp)
            return false;
        for (; imp->Name; imp++)
            if (dll_filter((const char*)((uint8_t*)module + imp->Name)))
                return true;
        return false;
    }

    void write_slot(ULONG_PTR* slot, void* fn) {
        DWORD old;
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
        *slot = (ULONG_PTR)fn;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }

    bool is_kernel_dll(const char* dll) {
        return !_stricmp(dll, "kernel32.dll") || !_stricmp(dll, "kernelbase.dll") || !_strnicmp(dll, "api-ms-win-core-", 16);
    }

    void for_each_new_module(std::vector<HMODULE>& seen, HMODULE skip, const std::wstring& game_dir,
        const ModuleVisitor& visit) {
        wchar_t windir[MAX_PATH];
        UINT wn = GetWindowsDirectoryW(windir, MAX_PATH);
        HMODULE mods[1024];
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
            return;
        DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
        for (DWORD i = 0; i < count; i++) {
            HMODULE m = mods[i];
            if (m == skip || std::find(seen.begin(), seen.end(), m) != seen.end())
                continue;
            seen.push_back(m);
            wchar_t path[MAX_PATH * 2];
            DWORD n = GetModuleFileNameW(m, path, (DWORD)(sizeof(path) / sizeof(path[0])));
            if (!n || (wn && n > wn && !_wcsnicmp(path, windir, wn) && (path[wn] == L'\\' || path[wn] == L'/')))
                continue; // system DLLs
            if (!game_dir.empty() && (n <= game_dir.size() || _wcsnicmp(path, game_dir.c_str(), game_dir.size())))
                continue; // outside the game folder
            visit(m, path);
        }
    }

    const wchar_t* module_file_name(const wchar_t* path) {
        const wchar_t* a = wcsrchr(path, L'\\');
        const wchar_t* b = wcsrchr(path, L'/');
        const wchar_t* s = a > b ? a : b;
        return s ? s + 1 : path;
    }
}
