// DivaLinux: import-table patching, shared by the GL bridge (vk_bridge.cpp), the file layer
// (vfs) and the I/O trace. Each feature decides which imports to replace; this walks the
// tables and writes the slots.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace dl {
    // Calls `visit` for every import by name in `module` from a DLL `dll_filter` accepts (all
    // DLLs when null). `slot` is the import's address slot; change it with write_slot.
    using ImportVisitor = std::function<void(const char* dll, const char* name, ULONG_PTR* slot)>;
    void for_each_import(HMODULE module, bool (*dll_filter)(const char* dll), const ImportVisitor& visit);

    // True if `module` imports from a DLL `dll_filter` accepts.
    bool imports_from(HMODULE module, bool (*dll_filter)(const char* dll));

    // Points an import slot at `fn`.
    void write_slot(ULONG_PTR* slot, void* fn);

    // kernel32, kernelbase and the api-ms-win-core-* sets.
    bool is_kernel_dll(const char* dll);

    // Loaded modules not in `seen` yet (they're added to it), outside the Windows folder.
    // With `game_dir` (a path prefix), only modules inside it: the game and its
    // plugins, not overlays (OBS, Fences) or drivers loaded into the process. `skip` is left
    // out (DivaLinux itself). `path` is the module's full path.
    using ModuleVisitor = std::function<void(HMODULE module, const wchar_t* path)>;
    void for_each_new_module(std::vector<HMODULE>& seen, HMODULE skip, const std::wstring& game_dir,
        const ModuleVisitor& visit);

    // The file name part of a module path.
    const wchar_t* module_file_name(const wchar_t* path);
}
