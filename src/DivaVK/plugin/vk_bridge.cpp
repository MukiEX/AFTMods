/*
    DivaLinux - PD Loader bridge (Wine / Proton), the DivaVK part of DivaLinux.dva.

    Turns DivaGL into DivaVK (DivaGL's renderer on Vulkan) without touching the renderer:
    - Patches the opengl32.dll and gdi32.dll imports of diva.exe, glut32.dll, this plugin and
      every other non-system module (TLAC, Render and other PD Loader plugins), so every
      GL/WGL call made in the process lands in DivaVK. Plugins that load after DivaVK are
      picked up when the game fills its GL tables and when the context is created.
    - Emulates WGL: contexts are fake handles; the DivaVK (Vulkan) context is created on the
      first wglMakeCurrent, using the GLUT window for the swapchain surface.
    - SwapBuffers presents the frame through the Vulkan swapchain.

    No real OpenGL context is ever created, which is what makes this usable where only
    Vulkan exists (Wine/Proton on Android via a Vulkan driver such as Turnip).
*/

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <psapi.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <volk.h>
#include <divavk/divavk.h>

static HDC s_hdc = nullptr;
static HWND s_hwnd = nullptr;
static bool s_created = false;
static int s_swap_interval = 1;
static FILE* s_log = nullptr;

static void bridge_log(const char* msg, void*) {
    if (!s_log) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash)
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"plugins\\DivaLinux.log");
        _wfopen_s(&s_log, path, L"w");
    }
    if (s_log) {
        fprintf(s_log, "%s\n", msg);
        fflush(s_log);
    }
    OutputDebugStringA("[DivaLinux] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

// For the rest of DivaLinux (the movie player): one log file for the whole plugin.
extern "C" void divavk_log_text(const char* text) {
    bridge_log(text, nullptr);
}

static void bridge_logf(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    bridge_log(buf, nullptr);
}

// ---------------------------------------------------------------- Vulkan context

static dvk_surface create_surface(dvk_instance instance, void*) {
    auto create = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr((VkInstance)instance, "vkCreateWin32SurfaceKHR");
    if (!create)
        return nullptr;
    VkWin32SurfaceCreateInfoKHR ci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = s_hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (create((VkInstance)instance, &ci, nullptr, &surface) != VK_SUCCESS)
        bridge_logf("vkCreateWin32SurfaceKHR failed");
    return (dvk_surface)surface;
}

static void patch_all_modules();

// ---------------------------------------------------------------- watchdog
// Logs where things are stuck when no frame has been shown for a while (slow devices such as
// a Quest under FEX can spend minutes compiling shaders; a real hang looks the same from
// outside). Also logs the first few frames' timings.
static std::atomic<uint64_t> s_frames_shown{ 0 };
static std::atomic<DWORD> s_last_frame_tick{ 0 };
static DWORD s_context_tick = 0;

static DWORD WINAPI watchdog_thread(void*) {
    DWORD last_report = 0;
    for (;;) {
        Sleep(1000);
        DWORD now = GetTickCount();
        DWORD since = now - s_last_frame_tick.load();
        if (since < 5000 || now - last_report < 10000)
            continue;
        last_report = now;
        dvk_stats st = {};
        dvk_get_stats(&st);
        bridge_logf("Watchdog: no new frame for %lu s (%llu shown so far). DivaVK is %s. "
            "So far: %llu shader programs, %llu pipelines, %llu draws.",
            since / 1000, (unsigned long long)s_frames_shown.load(), dvk_debug_stage(),
            (unsigned long long)st.programs_linked, (unsigned long long)st.pipelines_created,
            (unsigned long long)st.draws);
    }
}

extern "C" bool divalinux_get_xr_info(dvk_xr_info* out);   // dllmain.cpp ([vr] settings)

// DivaLinux.dva replaces DivaGL.dva, DivaVK.dva and DivaMovie.dva. With any of them loaded as
// well, two plugins patch the same renderer or movie player and the game breaks in odd ways,
// so say so once, by the time every plugin is loaded (the first context creation).
static void warn_about_replaced_plugins() {
    static const wchar_t* names[] = { L"DivaGL.dva", L"DivaVK.dva", L"DivaMovie.dva" };
    std::wstring found;
    for (const wchar_t* n : names)
        if (GetModuleHandleW(n)) {
            found += found.empty() ? L"" : L", ";
            found += n;
        }
    if (found.empty())
        return;
    char narrow[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, found.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
    bridge_logf("Also loaded: %s. DivaLinux.dva replaces these; disable or remove them.", narrow);
    std::wstring text = L"These plugins are loaded alongside DivaLinux:\n\n    " + found +
        L"\n\nDivaLinux.dva already contains DivaGL, DivaVK and DivaMovie, and running them together "
        L"breaks rendering or movies. Disable or remove them from the plugins folder.";
    MessageBoxW(nullptr, text.c_str(), L"DivaLinux", MB_OK | MB_ICONWARNING);
}

static bool ensure_context(HDC hdc) {
    if (s_created)
        return true;
    patch_all_modules(); // plugins loaded after DivaVK
    s_hdc = hdc;
    s_hwnd = WindowFromDC(hdc);
    RECT rc = {};
    if (s_hwnd)
        GetClientRect(s_hwnd, &rc);
    int w = rc.right - rc.left > 0 ? rc.right - rc.left : 1280;
    int h = rc.bottom - rc.top > 0 ? rc.bottom - rc.top : 720;

    static const char* exts[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    static char cache_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, cache_path, MAX_PATH);
    if (char* slash = strrchr(cache_path, '\\'))
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - cache_path), "plugins\\DivaLinux.pipeline_cache");

    // VR: the headset's runtime has to pick the GPU, so it comes before the context.
    dvk_xr_info xi = {};
    if (divalinux_get_xr_info(&xi)) {
        xi.log = bridge_log;
        if (!dvk_xr_setup(&xi))
            bridge_logf("VR is on ([vr] enabled=1) but OpenXR isn't available; showing the game on the monitor only");
    }

    dvk_create_info ci = {};
    ci.app_name = "PD Loader (DivaLinux)";
    ci.width = w;
    ci.height = h;
    ci.enable_validation = GetEnvironmentVariableA("DIVAVK_VALIDATION", nullptr, 0) > 0;
    ci.vsync = s_swap_interval != 0;
    ci.instance_extensions = exts;
    ci.instance_extension_count = s_hwnd ? 2 : 0;
    ci.create_surface = s_hwnd ? create_surface : nullptr;
    ci.log = bridge_log;
    ci.pipeline_cache_path = cache_path;
    s_created = dvk_create(&ci);
    if (!s_created)
        MessageBoxA(nullptr, "DivaLinux could not initialise Vulkan.\nSee plugins\\DivaLinux.log for details.", "DivaLinux", MB_OK | MB_ICONERROR);
    else
    {
        bridge_logf("Context created %dx%d on %s", w, h, dvk_device_name());
        s_context_tick = GetTickCount();
        s_last_frame_tick = s_context_tick;
        CloseHandle(CreateThread(nullptr, 0, watchdog_thread, nullptr, 0, nullptr));
    }
    warn_about_replaced_plugins();
    return s_created;
}

// ---------------------------------------------------------------- WGL emulation

static const HGLRC FAKE_CONTEXT = (HGLRC)(uintptr_t)0xD17A;
static HGLRC s_current = nullptr;

static HGLRC WINAPI bridge_wglCreateContext(HDC hdc) {
    s_hdc = hdc;
    return FAKE_CONTEXT;
}

static HGLRC WINAPI bridge_wglCreateContextAttribsARB(HDC hdc, HGLRC, const int*) {
    s_hdc = hdc;
    return FAKE_CONTEXT;
}

static BOOL WINAPI bridge_wglDeleteContext(HGLRC) {
    // The game tears the context down on exit; keep Vulkan alive until process detach.
    return TRUE;
}

static BOOL WINAPI bridge_wglMakeCurrent(HDC hdc, HGLRC rc) {
    s_current = rc;
    if (rc && hdc)
        return ensure_context(hdc) ? TRUE : FALSE;
    return TRUE;
}

static HGLRC WINAPI bridge_wglGetCurrentContext() { return s_current; }
static HDC WINAPI bridge_wglGetCurrentDC() { return s_current ? s_hdc : nullptr; }
static BOOL WINAPI bridge_wglShareLists(HGLRC, HGLRC) { return TRUE; }

static BOOL WINAPI bridge_SwapBuffers(HDC) {
    if (!s_created)
        return FALSE;
    if (s_hwnd) {
        // Follow the window's size, but only once it has stayed the same for a few frames:
        // windows often bounce between sizes while the game sets itself up, and each change
        // rebuilds the swapchain.
        static int s_pending_w = 0, s_pending_h = 0, s_stable = 0;
        RECT rc;
        if (GetClientRect(s_hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (w != s_pending_w || h != s_pending_h) {
                s_pending_w = w;
                s_pending_h = h;
                s_stable = 0;
            }
            else if (++s_stable == 5)
                dvk_resize(w, h);
        }
    }
    // VR: Home puts the virtual screen back in front of where the player is looking.
    {
        static bool s_home_was_down = false;
        bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (down && !s_home_was_down && dvk_xr_active())
            dvk_xr_recenter();
        s_home_was_down = down;
    }
    // Frame dump (debugging): Scroll Lock or Pause, or a file named DivaLinux_dump_request in the
    // game folder (handy without a keyboard), writes every render target of the next frame to
    // <game>\\DivaLinux_dump_<time>\\. Key presses are detected from the key-down state: TLAC
    // polls GetAsyncKeyState for every key each frame, which clears the "pressed since last
    // call" bit before we could see it.
    {
        static bool s_key_was_down = false;
        static unsigned s_frame = 0;
        bool down = (GetAsyncKeyState(VK_SCROLL) | GetAsyncKeyState(VK_PAUSE)) & 0x8000;
        bool trigger = down && !s_key_was_down;
        s_key_was_down = down;

        char game[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, game, MAX_PATH);
        while (n && game[n - 1] != '\\' && game[n - 1] != '/')
            n--;
        game[n] = 0;
        if ((++s_frame % 30) == 0) {
            std::string req = std::string(game) + "DivaLinux_dump_request";
            if (GetFileAttributesA(req.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(req.c_str());
                trigger = true;
            }
        }
        if (trigger) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char dir[MAX_PATH + 64];
            snprintf(dir, sizeof(dir), "%sDivaLinux_dump_%02d%02d%02d", game, t.wHour, t.wMinute, t.wSecond);
            CreateDirectoryA(dir, nullptr);
            bridge_logf("Frame dump requested: %s", dir);
            dvk_request_frame_dump(dir);
        }
    }
    DWORD t0 = GetTickCount();
    dvk_swap_buffers();
    uint64_t n = ++s_frames_shown;
    DWORD now = GetTickCount();
    s_last_frame_tick = now;
    if (n <= 3 || n == 60 || n == 600)
        bridge_logf("Frame %llu shown, %lu ms after the context was created (present took %lu ms)",
            (unsigned long long)n, now - s_context_tick, now - t0);
    return TRUE;
}

static BOOL WINAPI bridge_wglSwapBuffers(HDC hdc) { return bridge_SwapBuffers(hdc); }

static BOOL WINAPI bridge_wglSwapIntervalEXT(int interval) {
    s_swap_interval = interval;
    dvk_set_vsync(interval != 0);
    return TRUE;
}

static int WINAPI bridge_wglGetSwapIntervalEXT() { return s_swap_interval; }

static const char* WINAPI bridge_wglGetExtensionsString(HDC) {
    return "WGL_EXT_swap_control WGL_ARB_create_context WGL_ARB_extensions_string WGL_EXT_extensions_string";
}
static const char* WINAPI bridge_wglGetExtensionsStringEXT() { return bridge_wglGetExtensionsString(nullptr); }

static int WINAPI bridge_ChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR*) { return 1; }
static BOOL WINAPI bridge_SetPixelFormat(HDC, int, const PIXELFORMATDESCRIPTOR*) { return TRUE; }
static int WINAPI bridge_GetPixelFormat(HDC) { return 1; }

static int WINAPI bridge_DescribePixelFormat(HDC, int, UINT size, PIXELFORMATDESCRIPTOR* pfd) {
    if (pfd && size >= sizeof(PIXELFORMATDESCRIPTOR)) {
        memset(pfd, 0, sizeof(*pfd));
        pfd->nSize = sizeof(*pfd);
        pfd->nVersion = 1;
        pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd->iPixelType = PFD_TYPE_RGBA;
        pfd->cColorBits = 32;
        pfd->cRedBits = pfd->cGreenBits = pfd->cBlueBits = pfd->cAlphaBits = 8;
        pfd->cDepthBits = 24;
        pfd->cStencilBits = 8;
        pfd->iLayerType = PFD_MAIN_PLANE;
    }
    return 1;
}

static PROC WINAPI bridge_wglGetProcAddress(LPCSTR name);

struct BridgeEntry {
    const char* name;
    void* fn;
};

static const BridgeEntry s_wgl[] = {
    { "wglCreateContext", (void*)&bridge_wglCreateContext },
    { "wglCreateContextAttribsARB", (void*)&bridge_wglCreateContextAttribsARB },
    { "wglDeleteContext", (void*)&bridge_wglDeleteContext },
    { "wglMakeCurrent", (void*)&bridge_wglMakeCurrent },
    { "wglGetCurrentContext", (void*)&bridge_wglGetCurrentContext },
    { "wglGetCurrentDC", (void*)&bridge_wglGetCurrentDC },
    { "wglShareLists", (void*)&bridge_wglShareLists },
    { "wglSwapBuffers", (void*)&bridge_wglSwapBuffers },
    { "wglSwapIntervalEXT", (void*)&bridge_wglSwapIntervalEXT },
    { "wglGetSwapIntervalEXT", (void*)&bridge_wglGetSwapIntervalEXT },
    { "wglGetExtensionsStringARB", (void*)&bridge_wglGetExtensionsString },
    { "wglGetExtensionsStringEXT", (void*)&bridge_wglGetExtensionsStringEXT },
    { "wglGetProcAddress", (void*)&bridge_wglGetProcAddress },
    { "wglChoosePixelFormat", (void*)&bridge_ChoosePixelFormat },
    { "wglSetPixelFormat", (void*)&bridge_SetPixelFormat },
    { "wglGetPixelFormat", (void*)&bridge_GetPixelFormat },
    { "wglDescribePixelFormat", (void*)&bridge_DescribePixelFormat },
};

static const BridgeEntry s_gdi[] = {
    { "ChoosePixelFormat", (void*)&bridge_ChoosePixelFormat },
    { "SetPixelFormat", (void*)&bridge_SetPixelFormat },
    { "GetPixelFormat", (void*)&bridge_GetPixelFormat },
    { "DescribePixelFormat", (void*)&bridge_DescribePixelFormat },
    { "SwapBuffers", (void*)&bridge_SwapBuffers },
};

static void* lookup(const BridgeEntry* table, size_t count, const char* name) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(table[i].name, name))
            return table[i].fn;
    return nullptr;
}

static void* resolve_gl(const char* name) {
    if (void* fn = lookup(s_wgl, sizeof(s_wgl) / sizeof(s_wgl[0]), name))
        return fn;
    return dvk_get_proc_address(name);
}

// wglGetProcAddress is how GL code tests for extensions, so it must say "missing" for anything
// DivaVK doesn't implement (DivaMovie probes WGL_NV_DX_interop and falls back to CPU uploads).
// The game's own GL tables are still filled with logging stubs by divavk_bridge_fill_tables.
static PROC WINAPI bridge_wglGetProcAddress(LPCSTR name) {
    if (!name)
        return nullptr;
    if (void* fn = lookup(s_wgl, sizeof(s_wgl) / sizeof(s_wgl[0]), name))
        return (PROC)fn;
    if (void* fn = dvk_get_implemented_proc(name))
        return (PROC)fn;
    bridge_logf("wglGetProcAddress(\"%s\"): not implemented, reported as missing", name);
    return nullptr;
}

// ---------------------------------------------------------------- import patching

// GetProcAddress(opengl32.dll, "glSomething") would hand out the real opengl32 function,
// which has no context under DivaVK and silently does nothing. Answer from DivaVK instead.
typedef FARPROC(WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
static PFN_GetProcAddress s_real_GetProcAddress = nullptr;

static HMODULE s_self = nullptr;

static FARPROC WINAPI bridge_GetProcAddress(HMODULE module, LPCSTR name) {
    // s_real_GetProcAddress is captured before any import is patched (see install).
    if (module && HIWORD((ULONG_PTR)name) && module == GetModuleHandleW(L"opengl32.dll")) {
        // Callers use this to probe for features (DivaMovie checks for WGL_NV_DX_interop), so
        // never hand out a logging stub: only what DivaVK implements, or nullptr.
        if (void* fn = lookup(s_wgl, sizeof(s_wgl) / sizeof(s_wgl[0]), name))
            return (FARPROC)fn;
        return (FARPROC)dvk_get_implemented_proc(name);
    }
    return s_real_GetProcAddress(module, name);
}

static int patch_imports(HMODULE module, const char* label) {
    if (!module)
        return 0;
    uint8_t* base = (uint8_t*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;

    int patched = 0, unresolved = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        const char* dll = (const char*)(base + imp->Name);
        bool gl = !_stricmp(dll, "opengl32.dll");
        bool gdi = !_stricmp(dll, "gdi32.dll");
        bool k32 = !_stricmp(dll, "kernel32.dll");
        if (!gl && !gdi && !k32)
            continue;
        if (!imp->OriginalFirstThunk)
            continue; // bound imports without names; nothing we can match

        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const char* name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            // Never redirect our own GetProcAddress: bridge_GetProcAddress (and volk) call it.
            void* fn = gl ? resolve_gl(name)
                : gdi ? lookup(s_gdi, sizeof(s_gdi) / sizeof(s_gdi[0]), name)
                : (module != s_self && s_real_GetProcAddress && !strcmp(name, "GetProcAddress")
                    ? (void*)&bridge_GetProcAddress : nullptr);
            if (!fn) {
                if (gl)
                    unresolved++;
                continue;
            }
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            slots->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            patched++;
        }
    }
    bridge_logf("%s: %d imports redirected%s", label, patched, unresolved ? " (some unresolved)" : "");
    return patched;
}

// Every loaded module outside the Windows directory that imports opengl32/gdi32: the game,
// GLUT and PD Loader plugins (TLAC, Render, ...). Without this, a plugin that calls GL
// directly reaches the real opengl32.dll, which has no context under DivaVK, gets zeros
// back from queries like GL_VIEWPORT and can crash (TLAC divided by one).
static std::vector<HMODULE> s_patched;

static void patch_all_modules() {
    wchar_t windir[MAX_PATH];
    UINT wn = GetWindowsDirectoryW(windir, MAX_PATH);
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;
    DWORD count = needed / sizeof(HMODULE);
    if (count > 1024)
        count = 1024;
    for (DWORD i = 0; i < count; i++) {
        HMODULE m = mods[i];
        bool seen = false;
        for (HMODULE p : s_patched)
            if (p == m) { seen = true; break; }
        if (seen)
            continue;
        s_patched.push_back(m);
        wchar_t path[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(m, path, (DWORD)(sizeof(path) / sizeof(path[0])));
        if (!n)
            continue;
        if (wn && n > wn && !_wcsnicmp(path, windir, wn) && (path[wn] == L'\\' || path[wn] == L'/'))
            continue; // system DLLs (including opengl32 itself)
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        // The launcher draws its own window with the real OpenGL before the game starts.
        if (!_wcsicmp(base, L"Launcher.dva"))
            continue;
        char label[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, base, -1, label, sizeof(label), nullptr, nullptr);
        // patch_imports logs every module; keep the log readable by only logging hits
        uint8_t* b = (uint8_t*)m;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
            continue;
        bool imports_gl = false;
        for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress); imp->Name; imp++) {
            const char* dll = (const char*)(b + imp->Name);
            if (!_stricmp(dll, "opengl32.dll") || !_stricmp(dll, "gdi32.dll")) { imports_gl = true; break; }
            // A plugin that fetches GL functions with GetProcAddress needs patching too
            if (!_stricmp(dll, "opengl32.dll") || !_stricmp(dll, "kernel32.dll")) imports_gl = true;
        }
        if (imports_gl)
            patch_imports(m, label);
    }
}

// ---------------------------------------------------------------- game tables

struct Slot {
    uintptr_t address;
    const char* name;
};

static const Slot s_game_slots[] = {
#define DIVAVK_GAME_SLOT(addr, name) { (uintptr_t)addr, name },
#include "game_gl_tables.inc"
#undef DIVAVK_GAME_SLOT
};

static const Slot s_glut_slots[] = {
#define DIVAVK_GLUT_SLOT(off, name) { (uintptr_t)off, name },
#include "game_gl_tables.inc"
#undef DIVAVK_GLUT_SLOT
};

static int write_slots(const Slot* slots, size_t count, uintptr_t base) {
    int written = 0;
    for (size_t i = 0; i < count; i++) {
        void** slot = (void**)(base + slots[i].address);
        void* fn = resolve_gl(slots[i].name);
        if (!fn || *slot == fn)
            continue;
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
            continue;
        *slot = fn;
        VirtualProtect(slot, sizeof(void*), old, &old);
        written++;
    }
    return written;
}

// Writes DivaVK into every known GL slot of the game and GLUT (addresses from DivaGL's
// wrap.cpp). Called at install time and again after the game fills its extension table.
extern "C" void divavk_bridge_fill_tables() {
    patch_all_modules();
    int game = write_slots(s_game_slots, sizeof(s_game_slots) / sizeof(s_game_slots[0]), 0);
    int glut = 0;
    if (HMODULE g = GetModuleHandleW(L"glut32.dll"))
        glut = write_slots(s_glut_slots, sizeof(s_glut_slots) / sizeof(s_glut_slots[0]), (uintptr_t)g);
    bridge_logf("GL tables: %d game slots and %d GLUT slots updated", game, glut);
}

// Called from DivaGL's DllMain before anything creates a GL context.
extern "C" void divavk_bridge_install(HMODULE self) {
    s_self = self;
    // The real kernel32 export, fetched before any import table is touched.
    s_real_GetProcAddress = (PFN_GetProcAddress)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
    patch_imports(GetModuleHandleW(nullptr), "diva.exe");
    patch_imports(GetModuleHandleW(L"glut32.dll"), "glut32.dll");
    patch_imports(self, "DivaLinux");
    s_patched.push_back(GetModuleHandleW(nullptr));
    if (HMODULE g = GetModuleHandleW(L"glut32.dll"))
        s_patched.push_back(g);
    s_patched.push_back(self);
    divavk_bridge_fill_tables();
}

// Process exit: tearing Vulkan down under the loader lock can deadlock some drivers, so
// only persist the pipeline cache and let the OS reclaim the rest.
extern "C" void divavk_bridge_shutdown() {
    if (s_created)
        dvk_save_pipeline_cache();
    if (s_log) {
        fclose(s_log);
        s_log = nullptr;
    }
}
