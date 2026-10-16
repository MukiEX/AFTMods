/*
    DivaVK - PD Loader bridge (Windows / Wine).

    Turns DivaGL into DivaVK without touching DivaGL's renderer:
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
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"plugins\\DivaVK.log");
        _wfopen_s(&s_log, path, L"w");
    }
    if (s_log) {
        fprintf(s_log, "%s\n", msg);
        fflush(s_log);
    }
    OutputDebugStringA("[DivaVK] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
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
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - cache_path), "plugins\\DivaVK.pipeline_cache");

    dvk_create_info ci = {};
    ci.app_name = "PD Loader (DivaVK)";
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
        MessageBoxA(nullptr, "DivaVK could not initialise Vulkan.\nSee plugins\\DivaVK.log for details.", "DivaVK", MB_OK | MB_ICONERROR);
    else
        bridge_logf("Context created %dx%d on %s", w, h, dvk_device_name());
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
        RECT rc;
        if (GetClientRect(s_hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top)
            dvk_resize(rc.right - rc.left, rc.bottom - rc.top);
    }
    dvk_swap_buffers();
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

static PROC WINAPI bridge_wglGetProcAddress(LPCSTR name) {
    return name ? (PROC)resolve_gl(name) : nullptr;
}

// ---------------------------------------------------------------- import patching

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
        if (!gl && !gdi)
            continue;
        if (!imp->OriginalFirstThunk)
            continue; // bound imports without names; nothing we can match

        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const char* name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            void* fn = gl ? resolve_gl(name) : lookup(s_gdi, sizeof(s_gdi) / sizeof(s_gdi[0]), name);
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
    patch_imports(GetModuleHandleW(nullptr), "diva.exe");
    patch_imports(GetModuleHandleW(L"glut32.dll"), "glut32.dll");
    patch_imports(self, "DivaVK");
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
