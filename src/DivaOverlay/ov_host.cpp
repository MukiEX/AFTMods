// DivaOverlay inside DivaLinux.dva: opens the menu over the running game and feeds it input.
//
// DivaLinux's overlay hook (src/DivaLinux/overlay/compositor.cpp) calls divaoverlay_frame before every
// present; while the menu is open its Dear ImGui draw lists are drawn on the GPU over the frame
// (overlay/overlay_gl.cpp's DrawMesh, in both halves in side-by-side 3D). If that fails, it falls back to
// LauncherFS's CPU renderer and hands DivaLinux the picture to blend.
//
// Two hosts:
//   - Windows / Wine: F10 or View + Start opens it. While it's open the game sees no keyboard,
//     mouse or XInput (its window's input messages are kept from it, and GetAsyncKeyState,
//     GetKeyState, GetKeyboardState and XInputGetState report nothing pressed to callers
//     outside DivaLinux, through Detours), until everything is released after closing.
//   - The DivaRecomp runtime (Android): the runtime does the input. Its shared struct, whose
//     address is in DIVARECOMP_OVERLAY, carries open requests (Select + Start, the Quest's menu
//     button, holding a corner of the touchscreen), the menu buttons, and back from us whether
//     the menu is open (the runtime then holds the game's input back) and restart requests.
//     The pointer (touch, XR controller ray) arrives as the Win32 cursor, as for the game.
//
// [overlay] in DivaLinux.ini: enabled=0 turns it off; key=<virtual-key code> (Windows, F10 = 121).
// When the launcher's own in-game menu is on (Launcher.dva, [launcher] overlay=1), it has the
// game window instead and this stays off.

#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include <detours.h>
#include <imgui.h>
#include "ov_model.hpp"
#include "ov_platform.hpp"
#include "ov_ui.hpp"
#include "ini.hpp"
#include "soft_render.hpp"
#include "../DivaLinux/overlay/overlay_gl.hpp"

static_assert(sizeof(ImDrawIdx) == 2, "the GPU path draws 16-bit indices");

extern const unsigned char dov_font_bold[];
extern const unsigned int dov_font_bold_size;
extern const unsigned char dov_font_regular[];
extern const unsigned int dov_font_regular_size;

extern "C" void divavk_log_text(const char* text);                 // DivaLinux.log (vk_bridge.cpp)
extern "C" BOOL SetOptionInt(const char* name, int value);         // DivaGL (dllmain.cpp)
extern "C" void divalinux_cursor_install(const wchar_t* config_file); // overlay/cursor.cpp
extern "C" void divalinux_audio_install(const wchar_t* config_file);  // audio/headphones.cpp
extern "C" bool dvk_xr_active(void);                                // DivaVK (xr.cpp)

// ------------------------------------------------------------ files (Win32; the runtime maps them)
namespace dov {
    bool read_file(const std::wstring& path, std::string& data) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        data.clear();
        char buf[16384];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
            data.append(buf, n);
        CloseHandle(h);
        return true;
    }
    bool write_file(const std::wstring& path, const std::string& data) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD n = 0;
        BOOL ok = data.empty() || WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr);
        CloseHandle(h);
        return ok && n == data.size();
    }
    bool file_exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
    std::vector<std::wstring> list_dirs(const std::wstring& dir) {
        std::vector<std::wstring> out;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                out.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(out.begin(), out.end());
        return out;
    }
}

namespace {
    // ------------------------------------------------------------ the runtime's shared struct
    // Mirrors runtime/src/overlay_io.c in DivaDroid.
    enum : uint32_t {
        DOV_UP = 1u << 0, DOV_DOWN = 1u << 1, DOV_LEFT = 1u << 2, DOV_RIGHT = 1u << 3,
        DOV_OK = 1u << 4, DOV_BACK = 1u << 5, DOV_PAGE_UP = 1u << 6, DOV_PAGE_DOWN = 1u << 7,
    };
    struct RecompOverlayIO {
        volatile uint32_t magic;     // 'DOVL'
        volatile uint32_t version;   // 1
        volatile uint32_t toggles;   // runtime: bumped on each open/close request
        volatile uint32_t buttons;   // runtime: menu buttons held (DOV_*)
        volatile uint32_t open;      // us: the menu is open (game input held back)
        volatile uint32_t restart;   // us: please restart the app (settings are saved)
        volatile uint32_t xr;        // runtime: showing in a headset
        volatile uint32_t seen;      // us: DivaOverlay is here (the runtime's open gestures work)
        volatile uint32_t opened_by; // runtime (version 2): what the last toggle came from (DOV_SRC_*)
    };
    enum { DOV_SRC_PAD = 0, DOV_SRC_XR = 1, DOV_SRC_SLIDER = 2, DOV_SRC_TOUCH = 3 };

    // Windows: the same struct, ours, for a plugin that reads a DIVA controller (DivallerBridge)
    // to drive the menu with, as the runtime does on Android. It finds it through
    // DivaOverlay_ControllerIO (below), bumps toggles with opened_by DOV_SRC_SLIDER, keeps the
    // menu buttons held in buttons, and holds the game's input back while open is set.
    RecompOverlayIO g_ctl = { 0x4C564F44u, 2 };
    void ov_log(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        divavk_log_text(buf);
    }

    std::wstring dir_of(const std::wstring& p) {
        size_t s = p.find_last_of(L"\\/");
        return s == std::wstring::npos ? L"." : p.substr(0, s);
    }

    // ------------------------------------------------------------ input blocking (Windows)
    std::atomic<bool> g_block{ false };
    HMODULE g_self = nullptr;

    bool from_outside(void* ret) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m))
            return true;
        return m != g_self;
    }

    typedef SHORT(WINAPI* PFN_GetAsyncKeyState)(int);
    typedef SHORT(WINAPI* PFN_GetKeyState)(int);
    typedef BOOL(WINAPI* PFN_GetKeyboardState)(PBYTE);
    struct XINPUT_GAMEPAD_ { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
    struct XINPUT_STATE_ { DWORD dwPacketNumber; XINPUT_GAMEPAD_ Gamepad; };
    typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE_*);

    PFN_GetAsyncKeyState o_GetAsyncKeyState = nullptr;
    PFN_GetKeyState o_GetKeyState = nullptr;
    PFN_GetKeyboardState o_GetKeyboardState = nullptr;
    PFN_XInputGetState o_XInputGetState[5] = {};
    PFN_XInputGetState g_xinput = nullptr;   // ours (unhooked when hooked)

    SHORT WINAPI hk_GetAsyncKeyState(int k) {
        if (g_block && from_outside(_ReturnAddress())) return 0;
        return o_GetAsyncKeyState(k);
    }
    SHORT WINAPI hk_GetKeyState(int k) {
        if (g_block && from_outside(_ReturnAddress())) return 0;
        return o_GetKeyState(k);
    }
    BOOL WINAPI hk_GetKeyboardState(PBYTE s) {
        BOOL r = o_GetKeyboardState(s);
        if (r && s && g_block && from_outside(_ReturnAddress()))
            memset(s, 0, 256);
        return r;
    }
    template <int N> DWORD WINAPI hk_XInputGetState(DWORD i, XINPUT_STATE_* st) {
        DWORD r = o_XInputGetState[N](i, st);
        if (r == ERROR_SUCCESS && st && g_block && from_outside(_ReturnAddress()))
            st->Gamepad = {};
        return r;
    }
    void* const XINPUT_DETOURS[5] = { (void*)&hk_XInputGetState<0>, (void*)&hk_XInputGetState<1>,
        (void*)&hk_XInputGetState<2>, (void*)&hk_XInputGetState<3>, (void*)&hk_XInputGetState<4> };
    const wchar_t* const XINPUT_DLLS[5] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll" };

    bool g_hooks_done = false;
    void install_hooks() {
        if (g_hooks_done) return;
        g_hooks_done = true;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        o_GetAsyncKeyState = (PFN_GetAsyncKeyState)GetProcAddress(u, "GetAsyncKeyState");
        o_GetKeyState = (PFN_GetKeyState)GetProcAddress(u, "GetKeyState");
        o_GetKeyboardState = (PFN_GetKeyboardState)GetProcAddress(u, "GetKeyboardState");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        int n = 0;
        if (o_GetAsyncKeyState && DetourAttach(&(PVOID&)o_GetAsyncKeyState, (PVOID)hk_GetAsyncKeyState) == NO_ERROR) n++;
        if (o_GetKeyState && DetourAttach(&(PVOID&)o_GetKeyState, (PVOID)hk_GetKeyState) == NO_ERROR) n++;
        if (o_GetKeyboardState && DetourAttach(&(PVOID&)o_GetKeyboardState, (PVOID)hk_GetKeyboardState) == NO_ERROR) n++;
        for (int i = 0; i < 5; i++)
            if (HMODULE h = GetModuleHandleW(XINPUT_DLLS[i]))
                if ((o_XInputGetState[i] = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState")))
                    if (DetourAttach(&(PVOID&)o_XInputGetState[i], XINPUT_DETOURS[i]) == NO_ERROR) n++;
        LONG r = DetourTransactionCommit();
        ov_log("DivaOverlay: %d input functions held back from the game while the menu is open%s", n, r == NO_ERROR ? "" : " (hooking failed)");
    }

    PFN_XInputGetState xinput() {
        if (g_xinput) return g_xinput;
        static bool tried = false;
        if (tried) return nullptr;
        tried = true;
        for (const wchar_t* n : XINPUT_DLLS) {
            HMODULE h = GetModuleHandleW(n);
            if (!h) h = LoadLibraryW(n);
            if (h && (g_xinput = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState")))
                return g_xinput;
        }
        return nullptr;
    }

    // ------------------------------------------------------------ state
    struct State {
        bool installed = false, enabled = true;
        std::wstring ini, game_dir;
        RecompOverlayIO* io = nullptr;
        uint32_t toggles_seen = 0;
        uint32_t ctl_toggles_seen = 0;   // Windows: g_ctl's
        bool open_slider = false;    // the menu was opened from a slider controller
        int key = VK_F10;
        bool open = false, draining = false, toggle_was_down = false;
        bool ui_ready = false;
        ImGuiContext* ctx = nullptr;
        ImFont* bold = nullptr;
        ImFont* reg = nullptr;
        dov::Model model;
        dov::Ui* ui = nullptr;
        lfs::SoftCanvas canvas;
        uint64_t hash = 0;
        uint32_t serial = 0;
        LARGE_INTEGER qpc_freq{}, qpc_last{};
        // window (Windows)
        HWND hwnd = nullptr;
        WNDPROC game_proc = nullptr;
        std::atomic<int> wheel{ 0 };
        // last input, for the hints
        dov::Device device = dov::Device::Pad;
        POINT last_cursor{ -100000, -100000 };
        bool restart_requested = false;
        // GPU drawing (DivaLinux's overlay/overlay_gl.cpp through DivaVK)
        OverlayGL::Compositor gl;
        uint32_t next_tex_id = 0;
        std::vector<OverlayGL::Vertex> vtx;
        std::vector<uint16_t> idx;
        std::vector<OverlayGL::MeshCmd> cmds;
    };
    State g;
    std::mutex g_lock;

    bool any_key_down() {
        for (int k = 1; k < 256; k++)
            if ((o_GetAsyncKeyState ? o_GetAsyncKeyState(k) : GetAsyncKeyState(k)) & 0x8000)
                return true;
        return false;
    }
    WORD pad_buttons(int* lx = nullptr, int* ly = nullptr) {
        PFN_XInputGetState get = o_XInputGetState[0] ? nullptr : xinput();
        WORD b = 0;
        for (DWORD i = 0; i < 4; i++) {
            XINPUT_STATE_ st = {};
            DWORD r = ERROR_DEVICE_NOT_CONNECTED;
            for (int k = 0; k < 5 && r != ERROR_SUCCESS; k++)
                if (o_XInputGetState[k]) r = o_XInputGetState[k](i, &st);
            if (r != ERROR_SUCCESS && get) r = get(i, &st);
            if (r != ERROR_SUCCESS) continue;
            b |= st.Gamepad.wButtons;
            if (lx && std::abs(st.Gamepad.sThumbLX) > std::abs(*lx)) *lx = st.Gamepad.sThumbLX;
            if (ly && std::abs(st.Gamepad.sThumbLY) > std::abs(*ly)) *ly = st.Gamepad.sThumbLY;
        }
        return b;
    }
    const WORD XI_UP = 0x0001, XI_DOWN = 0x0002, XI_LEFT = 0x0004, XI_RIGHT = 0x0008, XI_START = 0x0010, XI_BACK = 0x0020,
        XI_LB = 0x0100, XI_RB = 0x0200, XI_A = 0x1000, XI_B = 0x2000;

    // The game's window: while the menu is open (and until everything's released after), its
    // keyboard and mouse messages stop here. The mouse wheel goes to the menu.
    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (g_block) {
            switch (msg) {
            case WM_MOUSEWHEEL:
                if (g.open) g.wheel += GET_WHEEL_DELTA_WPARAM(wp);
                return 0;
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEMOVE: case WM_INPUT: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;   // Alt+F4 still works
                return 0;
            default: break;
            }
        }
        return CallWindowProcW(g.game_proc, hwnd, msg, wp, lp);
    }

    // ------------------------------------------------------------ menu
    uint64_t draw_data_hash(ImDrawData* dd) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const unsigned char* b = (const unsigned char*)p;
            for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
        };
        for (int i = 0; i < dd->CmdListsCount; i++) {
            const ImDrawList* l = dd->CmdLists[i];
            mix(l->VtxBuffer.Data, (size_t)l->VtxBuffer.Size * sizeof(ImDrawVert));
            mix(l->IdxBuffer.Data, (size_t)l->IdxBuffer.Size * sizeof(ImDrawIdx));
            for (const ImDrawCmd& c : l->CmdBuffer) mix(&c.ClipRect, sizeof(c.ClipRect));
        }
        if (dd->Textures)
            for (ImTextureData* t : *dd->Textures) { mix(&t->UniqueID, sizeof(t->UniqueID)); mix(&t->Status, sizeof(t->Status)); }
        return h;
    }

    void set_open(bool open);

    void ui_init() {
        g.ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g.ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendRendererName = "divaoverlay";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        g.bold = io.Fonts->AddFontFromMemoryTTF((void*)dov_font_bold, (int)dov_font_bold_size, 30.0f, &cfg);
        g.reg = io.Fonts->AddFontFromMemoryTTF((void*)dov_font_regular, (int)dov_font_regular_size, 24.0f, &cfg);

        dov::ModelPaths p;
        p.game_dir = g.game_dir;
        p.divalinux_ini = g.ini;
        p.pd.game_dir = g.game_dir;
        p.pd.plugins_dir = g.game_dir + L"\\plugins";
        p.pd.patches_dir = g.game_dir + L"\\patches";
        p.pd.config = p.pd.plugins_dir + L"\\config.ini";
        p.pd.components = p.pd.plugins_dir + L"\\components.ini";
        p.pd.playerdata = p.pd.plugins_dir + L"\\playerdata.ini";
        p.pd.keyconfig = p.pd.plugins_dir + L"\\keyconfig.ini";
        if (g.io) p.env_file = g.game_dir + L"\\divarecomp_env.txt";
        dov::LiveHooks hooks;
        hooks.set_option_int = [](const char* name, int v) { return SetOptionInt(name, v) != FALSE; };
        const std::wstring ini = g.ini;
        hooks.reload_cursor = [ini]() { divalinux_cursor_install(ini.c_str()); };
        hooks.reload_audio = [ini]() { divalinux_audio_install(ini.c_str()); };
        const bool xr = (g.io && g.io->xr) || dvk_xr_active();
        g.model.build(p, g.io != nullptr, xr, hooks);

        dov::UiHost host;
        if (g.io)
            host.close_hint = xr ? "Menu" : "Select+Start";
        else {
            wchar_t name[32] = L"F10";
            if (g.key != VK_F10) {
                UINT sc = MapVirtualKeyW(g.key, MAPVK_VK_TO_VSC);
                if (!sc || !GetKeyNameTextW((LONG)(sc << 16), name, 32)) swprintf(name, 32, L"Key %d", g.key);
            }
            host.close_hint = lfs::to_utf8(name);
        }
        host.close = []() { set_open(false); };
        host.restart = []() { g.restart_requested = true; };
        g.ui = new dov::Ui(g.model, host, g.bold, g.reg);
        g.canvas.keep_alpha = true;
        QueryPerformanceFrequency(&g.qpc_freq);
        QueryPerformanceCounter(&g.qpc_last);
        g.ui_ready = true;
        ov_log("DivaOverlay: menu ready (%d settings)", (int)g.model.all.size());
    }

    void set_open(bool open) {
        if (open == g.open) return;
        if (open) {
            if (!g.ui_ready) ui_init();
            ImGui::SetCurrentContext(g.ctx);
            g.ui->set_slider_controls(g.open_slider);
            g.ui->open();
            g.serial++;
            g.hash = 0;
            QueryPerformanceCounter(&g.qpc_last);
            if (!g.io) {
                install_hooks();
                g_block = true;
            }
            ov_log("DivaOverlay: menu opened");
        }
        else {
            if (g.ui) g.ui->flush();
            if (!g.io) g.draining = true;   // unblock once everything is released
            ov_log("DivaOverlay: menu closed");
        }
        g.open = open;
        if (g.io) g.io->open = open ? 1 : 0;
        g_ctl.open = open ? 1 : 0;
    }

    void restart_game() {
        ov_log("DivaOverlay: restarting the game");
        if (g.ui) g.ui->flush();
        if (g.io) {
            g.io->restart = 1;   // the runtime restarts the app
            return;
        }
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = GetCommandLineW();
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (CreateProcessW(exe, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        else
            ov_log("DivaOverlay: couldn't start the game again (error %lu)", GetLastError());
    }

    // ------------------------------------------------------------ one frame
    dov::Input gather(HWND hwnd, float scale, float mox, float moy, float msx, float msy, float dt) {
        dov::Input in;
        in.dt = dt;
        if (g.io) {
            const uint32_t b = g.io->buttons;
            in.up = b & DOV_UP; in.down = b & DOV_DOWN; in.left = b & DOV_LEFT; in.right = b & DOV_RIGHT;
            in.ok = b & DOV_OK; in.back = b & DOV_BACK; in.page_up = b & DOV_PAGE_UP; in.page_down = b & DOV_PAGE_DOWN;
            if (b) g.device = dov::Device::Pad;
        }
        else {
            // a DIVA controller, through a plugin (DivallerBridge)
            const uint32_t cb = g_ctl.buttons;
            in.up = cb & DOV_UP; in.down = cb & DOV_DOWN; in.left = cb & DOV_LEFT; in.right = cb & DOV_RIGHT;
            in.ok = cb & DOV_OK; in.back = cb & DOV_BACK;
            const bool ctl = cb != 0;
            auto key = [](int k) { return ((o_GetAsyncKeyState ? o_GetAsyncKeyState(k) : GetAsyncKeyState(k)) & 0x8000) != 0; };
            const bool focused = GetForegroundWindow() == hwnd;
            bool kb = false;
            if (focused) {
                in.up |= key(VK_UP); in.down |= key(VK_DOWN); in.left |= key(VK_LEFT); in.right |= key(VK_RIGHT);
                in.ok |= key(VK_RETURN) || key(VK_SPACE); in.back |= key(VK_ESCAPE) || key(VK_BACK);
                in.page_up = key(VK_PRIOR); in.page_down = key(VK_NEXT);
                kb = key(VK_UP) || key(VK_DOWN) || key(VK_LEFT) || key(VK_RIGHT) || key(VK_RETURN) || key(VK_SPACE)
                    || key(VK_ESCAPE) || key(VK_BACK) || in.page_up || in.page_down;
            }
            int lx = 0, ly = 0;
            const WORD pb = pad_buttons(&lx, &ly);
            const bool pad = pb != 0 || std::abs(lx) > 16000 || std::abs(ly) > 16000;
            in.up |= (pb & XI_UP) || ly > 16000; in.down |= (pb & XI_DOWN) || ly < -16000;
            in.left |= (pb & XI_LEFT) || lx < -16000; in.right |= (pb & XI_RIGHT) || lx > 16000;
            // B confirms and A goes back, as in the game (its circle and cross)
            in.ok |= (pb & XI_B) != 0; in.back |= (pb & XI_A) != 0;
            in.page_up |= (pb & XI_LB) != 0; in.page_down |= (pb & XI_RB) != 0;
            if (kb) g.device = dov::Device::Keys;
            else if (pad || ctl) g.device = dov::Device::Pad;
            const int w = g.wheel.exchange(0);
            in.wheel = w / 120.0f;
        }
        // pointer: the Win32 cursor (on Android, the runtime puts touches and the XR pointer there)
        POINT pt;
        RECT rc;
        if (hwnd && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && GetClientRect(hwnd, &rc)
            && pt.x >= 0 && pt.y >= 0 && pt.x < rc.right && pt.y < rc.bottom) {
            in.pointer_valid = true;
            in.px = (pt.x + 0.5f - mox) * msx * scale;
            in.py = (pt.y + 0.5f - moy) * msy * scale;
            const SHORT lb = o_GetAsyncKeyState ? o_GetAsyncKeyState(VK_LBUTTON) : GetAsyncKeyState(VK_LBUTTON);
            in.pointer_down = (lb & 0x8000) != 0;
            if (pt.x != g.last_cursor.x || pt.y != g.last_cursor.y || in.pointer_down) {
                if (g.last_cursor.x != -100000) g.device = dov::Device::Pointer;
                g.last_cursor = pt;
            }
        }
        in.device = g.device;
        return in;
    }
}

// ------------------------------------------------------------ DivaLinux's side

// The DivaRecomp runtime's struct, if we run under it.
static void find_runtime() {
    if (g.io) return;
    char buf[32];
    const DWORD n = GetEnvironmentVariableA("DIVARECOMP_OVERLAY", buf, sizeof(buf));
    if (n && n < sizeof(buf)) {
        auto* io = (RecompOverlayIO*)(uintptr_t)strtoull(buf, nullptr, 16);
        if (io && io->magic == 0x4C564F44u && io->version >= 1) {
            g.io = io;
            if (g.enabled) io->seen = 1;
        }
    }
}

// Windows: the menu's controller struct, for a plugin that reads a DIVA controller (DivallerBridge
// finds it with GetProcAddress on DivaLinux.dva). seen is set while the menu is on.
extern "C" __declspec(dllexport) void* __cdecl DivaOverlay_ControllerIO(void) {
    return &g_ctl;
}

// From DivaLinux's dllmain, with DivaLinux.ini's path.
extern "C" void divaoverlay_install(HMODULE self, const wchar_t* divalinux_ini) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g.installed) return;
    g.installed = true;
    g_self = self;
    g.ini = divalinux_ini;
    g.enabled = GetPrivateProfileIntW(L"overlay", L"enabled", 1, divalinux_ini) != 0;
    g.key = (int)GetPrivateProfileIntW(L"overlay", L"key", VK_F10, divalinux_ini);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g.game_dir = dir_of(exe);
    find_runtime();
    g_ctl.seen = g.enabled && !g.io ? 1 : 0;   // under the runtime, it reads the controller itself
    if (!g.enabled) {
        ov_log("DivaOverlay: off ([overlay] enabled=0 in DivaLinux.ini)");
        return;
    }
    if (g.io) {
        ov_log("DivaOverlay: settings menu on (Select + Start, the left controller's menu button, or hold the screen's top right corner)");
    }
    else
        ov_log("DivaOverlay: settings menu on (F10, or View + Start on a controller)");
}

// ImGui's textures (the font atlas) to the GPU renderer.
static void gpu_update_textures(ImDrawData* dd) {
    if (!dd->Textures) return;
    for (ImTextureData* t : *dd->Textures) {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            uint32_t id = t->TexID != ImTextureID_Invalid ? (uint32_t)(uintptr_t)t->TexID : ++g.next_tex_id;
            if (t->Format == ImTextureFormat_RGBA32) {
                if (!g.gl.SetTexture(id, t->Width, t->Height, t->GetPixels())) return;
            }
            else {   // Alpha8: white with the coverage in alpha
                std::vector<uint8_t> rgba((size_t)t->Width * t->Height * 4);
                const uint8_t* a = (const uint8_t*)t->GetPixels();
                for (size_t k = 0; k < (size_t)t->Width * t->Height; k++) {
                    rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = 255;
                    rgba[k * 4 + 3] = a[k];
                }
                if (!g.gl.SetTexture(id, t->Width, t->Height, rgba.data())) return;
            }
            t->SetTexID((ImTextureID)(uintptr_t)id);
            t->SetStatus(ImTextureStatus_OK);
        }
        else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// Draws the frame's draw lists on the GPU, over the game's picture, in each rect.
static bool gpu_draw(ImDrawData* dd, float lay_w, float lay_h, const OverlayGL::Rect* rects, int n) {
    gpu_update_textures(dd);
    if (g.gl.Failed()) return false;
    g.vtx.clear(); g.idx.clear(); g.cmds.clear();
    for (int l = 0; l < dd->CmdListsCount; l++) {
        const ImDrawList* list = dd->CmdLists[l];
        const uint32_t vbase = (uint32_t)g.vtx.size(), ibase = (uint32_t)g.idx.size();
        for (const ImDrawVert& v : list->VtxBuffer)
            g.vtx.push_back({ v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col });
        g.idx.insert(g.idx.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
        for (const ImDrawCmd& c : list->CmdBuffer) {
            if (c.UserCallback || !c.ElemCount) continue;
            OverlayGL::MeshCmd m;
            m.count = c.ElemCount;
            m.firstIndex = ibase + c.IdxOffset;
            m.baseVertex = (int32_t)(vbase + c.VtxOffset);
            m.clip[0] = c.ClipRect.x; m.clip[1] = c.ClipRect.y; m.clip[2] = c.ClipRect.z; m.clip[3] = c.ClipRect.w;
            m.texture = (uint32_t)(uintptr_t)c.GetTexID();
            g.cmds.push_back(m);
        }
    }
    OverlayGL::Mesh mesh = { lay_w, lay_h, g.vtx.data(), (int)g.vtx.size(), g.idx.data(), (int)g.idx.size(), g.cmds.data(), (int)g.cmds.size() };
    return g.gl.DrawMesh(mesh, rects, n);
}

// From DivaLinux's overlay hook before every present (the launcher's in-game menu isn't on),
// with where the picture goes (one rect, or one per eye in side-by-side 3D).
// Returns 0 when the menu is closed, 1 when it was drawn (on the GPU), 2 when its picture is
// in *pixels for DivaLinux to blend (the CPU fallback, if the GPU path failed).
extern "C" int divaoverlay_frame(HWND hwnd, int32_t lay_w, int32_t lay_h,
    float mouse_ox, float mouse_oy, float mouse_sx, float mouse_sy,
    const OverlayGL::Rect* rects, int rect_count,
    const uint32_t** pixels, int32_t* width, int32_t* height, uint32_t* serial) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g.installed || !g.enabled || lay_w <= 0 || lay_h <= 0)
        return 0;

    if (!g.hwnd)
        find_runtime();   // in case the runtime set it up after DivaLinux loaded
    // window hook (Windows): input messages stay away from the game while the menu is open
    if (!g.io && hwnd && !g.hwnd) {
        g.hwnd = hwnd;
        WNDPROC prev = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&wnd_proc);
        if (prev) g.game_proc = prev;
        else SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)prev);   // no window procedure to chain to
    }
    else if (g.io && hwnd && !g.hwnd)
        g.hwnd = hwnd;

    // open / close
    if (g.io) {
        const uint32_t t = g.io->toggles;
        if (t != g.toggles_seen) {
            g.toggles_seen = t;
            // opened from a slider controller: the menu shows how the controller drives it
            g.open_slider = g.io->version >= 2 && g.io->opened_by == DOV_SRC_SLIDER;
            set_open(!g.open);
        }
    }
    else {
        auto raw_key = [](int k) { return ((o_GetAsyncKeyState ? o_GetAsyncKeyState(k) : GetAsyncKeyState(k)) & 0x8000) != 0; };
        const bool key = GetForegroundWindow() == hwnd && raw_key(g.key);
        const bool combo = (pad_buttons() & (XI_BACK | XI_START)) == (XI_BACK | XI_START);
        const bool down = key || (!g.open && combo);
        if (down && !g.toggle_was_down) {
            g.open_slider = false;
            set_open(!g.open);
        }
        g.toggle_was_down = down;
        // Select + Start on a DIVA controller (DivallerBridge)
        const uint32_t t = g_ctl.toggles;
        if (t != g.ctl_toggles_seen) {
            g.ctl_toggles_seen = t;
            g.open_slider = g_ctl.opened_by == DOV_SRC_SLIDER;
            set_open(!g.open);
        }
    }
    if (g.restart_requested) {
        g.restart_requested = false;
        restart_game();
    }

    if (!g.open) {
        if (g.draining && pad_buttons() == 0 && !any_key_down()) {
            g.draining = false;
            g_block = false;
        }
        return 0;
    }

    ImGui::SetCurrentContext(g.ctx);
    // On the GPU the menu is laid out at the picture's size; the CPU fallback draws at most
    // 1080 lines (scaled up when blended).
    const bool gpu = !g.gl.Failed();
    const float scale = !gpu && lay_h > 1080 ? 1080.0f / lay_h : 1.0f;
    const int cw = std::max(1, (int)(lay_w * scale + 0.5f)), ch = std::max(1, (int)(lay_h * scale + 0.5f));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = std::max(1e-4f, std::min(0.1f, (float)((double)(now.QuadPart - g.qpc_last.QuadPart) / (double)g.qpc_freq.QuadPart)));
    g.qpc_last = now;

    dov::Input in = gather(hwnd, scale, mouse_ox, mouse_oy, mouse_sx, mouse_sy, dt);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)cw, (float)ch);
    io.DeltaTime = dt;
    ImGui::NewFrame();
    g.ui->frame(in, (float)cw, (float)ch, ImGui::GetBackgroundDrawList());
    ImGui::Render();
    if (!g.open)
        return 0;   // closed by this frame's input

    ImDrawData* dd = ImGui::GetDrawData();
    if (gpu) {
        if (gpu_draw(dd, (float)cw, (float)ch, rects, rect_count))
            return 1;
        ov_log("DivaOverlay: drawing on the GPU failed (%s); drawing on the CPU instead", g.gl.error.c_str());
        g.hash = 0;
        // the font atlas was uploaded to the GPU: have ImGui hand it over again for the CPU
        if (dd->Textures)
            for (ImTextureData* t : *dd->Textures)
                if (t->Status == ImTextureStatus_OK) {
                    t->SetTexID(ImTextureID_Invalid);
                    t->SetStatus(ImTextureStatus_Destroyed);   // ImGui asks for it to be created again
                }
        return 0;   // next frame: the CPU path, laid out for it
    }
    lfs::soft_update_textures(dd);
    g.canvas.resize(cw, ch);
    const uint64_t h = draw_data_hash(dd);
    if (h != g.hash) {
        g.hash = h;
        g.canvas.clear(0x00000000u);
        lfs::soft_render(g.canvas, dd);
        g.serial++;
    }
    *pixels = g.canvas.px.data();
    *width = g.canvas.w;
    *height = g.canvas.h;
    *serial = g.serial;
    return 2;
}

// The menu is open (DivaLinux leaves its pointer drawn over it).
extern "C" bool divaoverlay_is_open(void) { return g.open; }
