// Win32 host for the launcher: the divaMain hook, a borderless fullscreen D3D11 window,
// XInput polling for every pad (Steam Input shows up as XInput under Proton), plugin and
// custom patch discovery, and starting the game.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <xinput.h>
#include <shellapi.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include "app.hpp"
#include "ini.hpp"
#include "plugin_abi.hpp"
#include <MinHook.h>
#include "../res/resource.h"

#include <algorithm>
#include <string>
#include <vector>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace lfs;

namespace {
    // ------------------------------------------------------------ paths

    HMODULE g_module = nullptr;

    std::wstring exe_path() {
        wchar_t buf[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
        return std::wstring(buf, n);
    }

    std::wstring dir_of(const std::wstring& p) {
        size_t pos = p.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"." : p.substr(0, pos);
    }

    bool file_exists(const std::wstring& p) {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    Paths make_paths() {
        Paths p;
        p.game_dir = dir_of(exe_path());
        p.plugins_dir = p.game_dir + L"\\plugins";
        p.patches_dir = p.game_dir + L"\\patches";
        p.config = p.plugins_dir + L"\\config.ini";
        p.components = p.plugins_dir + L"\\components.ini";
        p.playerdata = p.plugins_dir + L"\\playerdata.ini";
        p.keyconfig = p.plugins_dir + L"\\keyconfig.ini";
        return p;
    }

    // ------------------------------------------------------------ system info

    ScreenInfo query_screen() {
        ScreenInfo s;
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
            if (dm.dmBitsPerPel != 0 && dm.dmBitsPerPel < 24)
                continue;
            Res r = { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight };
            if (std::find(s.resolutions.begin(), s.resolutions.end(), r) == s.resolutions.end())
                s.resolutions.push_back(r);
            int hz = (int)dm.dmDisplayFrequency;
            if (hz > 1 && std::find(s.refresh_rates.begin(), s.refresh_rates.end(), hz) == s.refresh_rates.end())
                s.refresh_rates.push_back(hz);
        }
        Res def = { 1280, 720 };
        if (std::find(s.resolutions.begin(), s.resolutions.end(), def) == s.resolutions.end())
            s.resolutions.push_back(def);
        std::sort(s.resolutions.begin(), s.resolutions.end(), [](const Res& a, const Res& b) {
            return a.w != b.w ? a.w < b.w : a.h < b.h;
        });
        if (s.refresh_rates.empty())
            s.refresh_rates.push_back(60);
        std::sort(s.refresh_rates.begin(), s.refresh_rates.end());
        return s;
    }

    std::string wine_version() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return "";
        auto fn = (const char* (__cdecl*)())GetProcAddress(ntdll, "wine_get_version");
        if (!fn)
            return "";
        const char* v = fn();
        return v ? v : "";
    }

    // ------------------------------------------------------------ plugins

    std::wstring safe(LPCWSTR s) { return s ? s : L""; }

    std::vector<OptionPtr> convert_options(const pdl::ConfigArray& arr) {
        using namespace pdl;
        std::vector<OptionPtr> out;
        for (int i = 0; i < arr.len; i++) {
            const ConfigOption& o = arr.options[i];
            if (!o.data || !arr.options)
                continue;
            switch (o.cfgType) {
            case CONFIG_BOOLEAN: {
                auto d = (BooleanData*)o.data;
                out.push_back(make_bool(d->iniVarName, d->iniSectionName, safe(d->iniFilePath), safe(d->friendlyName), safe(d->description), d->defaultVal, d->saveAsString));
            } break;
            case CONFIG_NUMERIC: {
                auto d = (NumericData*)o.data;
                out.push_back(make_int(d->iniVarName, d->iniSectionName, safe(d->iniFilePath), safe(d->friendlyName), safe(d->description), d->defaultVal, d->minVal, d->maxVal));
            } break;
            case CONFIG_STRING: {
                auto d = (StringData*)o.data;
                out.push_back(make_string(d->iniVarName, d->iniSectionName, safe(d->iniFilePath), safe(d->friendlyName), safe(d->description), safe(d->defaultVal), d->useUtf8));
            } break;
            case CONFIG_DROPDOWN_INDEX: {
                auto d = (DropdownIndexData*)o.data;
                std::vector<std::wstring> items;
                for (LPCWSTR s : d->valueStrings) items.push_back(safe(s));
                out.push_back(make_choice(d->iniVarName, d->iniSectionName, safe(d->iniFilePath), safe(d->friendlyName), safe(d->description), d->defaultVal, items));
            } break;
            case CONFIG_DROPDOWN_TEXT: {
                auto d = (DropdownTextData*)o.data;
                std::vector<std::wstring> items;
                for (LPCWSTR s : d->valueStrings) items.push_back(safe(s));
                out.push_back(make_choice_text(d->iniVarName, d->iniSectionName, safe(d->iniFilePath), safe(d->friendlyName), safe(d->description), safe(d->defaultVal), items, d->editable, d->useUtf8));
            } break;
            case CONFIG_DROPDOWN_NUMBER: {
                auto d = (DropdownNumberData*)o.data;
                out.push_back(make_choice_number(d->iniVarName, d->iniSectionName, safe(d->iniFilePath), safe(d->friendlyName), safe(d->description), d->defaultVal,
                    std::vector<int>(d->valueInts.begin(), d->valueInts.end()), d->editable));
            } break;
            case CONFIG_RESOLUTION: {
                auto d = (ResolutionData*)o.data;
                std::vector<Res> list;
                for (auto& r : d->valueResolutions) list.push_back({ (int)r.width, (int)r.height });
                out.push_back(make_resolution(d->iniVarName, d->iniVarName2, d->iniSectionName, safe(d->iniFilePath), safe(d->friendlyName), safe(d->description),
                    { (int)d->defaultVal.width, (int)d->defaultVal.height }, list, d->editable, RES_NONE));
            } break;
            case CONFIG_GROUP_START: {
                auto d = (GroupData*)o.data;
                out.push_back(make_section(clean_label(safe(d->name))));
            } break;
            case CONFIG_BUTTON: {
                auto d = (ButtonData*)o.data;
                out.push_back(make_button(safe(d->friendlyName), safe(d->description), d->func));
            } break;
            default: // CONFIG_GROUP_END, CONFIG_SPACER: layout only
                break;
            }
        }
        return out;
    }

    bool has_ext(const std::wstring& name, const wchar_t* ext) {
        size_t n = wcslen(ext);
        return name.size() > n && _wcsicmp(name.c_str() + name.size() - n, ext) == 0;
    }

    template <typename F>
    void each_file(const std::wstring& pattern, F f) {
        WIN32_FIND_DATAW ffd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (ffd.cFileName[0] == L'.' && ffd.cFileName[1] == L'_')
                continue; // macOS metadata
            f(std::wstring(ffd.cFileName));
        } while (FindNextFileW(h, &ffd));
        FindClose(h);
    }

    Page plugins_page(const Paths& p) {
        Page pg;
        pg.title = "Plugins";
        pg.kind = PageKind::Plugins;
        pg.empty_text = "No plugins found in the plugins folder.";
        each_file(p.plugins_dir + L"\\*.dva", [&](const std::wstring& file) {
            if (!has_ext(file, L".dva"))
                return;
            // Plugins are already loaded by PD Loader, so this just takes another reference.
            // Like the original launcher, never free them: plugin buttons may still be called.
            HMODULE h = LoadLibraryW((p.plugins_dir + L"\\" + file).c_str());
            if (!h)
                return;
            if (h == g_module)
                return; // that's us
            auto name_fn = (LPCWSTR(*)())GetProcAddress(h, "GetPluginName");
            auto desc_fn = (LPCWSTR(*)())GetProcAddress(h, "GetPluginDescription");
            auto date_fn = (LPCWSTR(*)())GetProcAddress(h, "GetBuildDate");
            auto opts_fn = (pdl::ConfigArray(*)())GetProcAddress(h, "GetPluginOptions");
            std::wstring name = name_fn ? safe(name_fn()) : file;
            std::wstring desc = desc_fn ? safe(desc_fn()) : file + L" plugin";
            std::string date = date_fn ? to_utf8(safe(date_fn())) : "";
            std::vector<OptionPtr> children;
            if (opts_fn)
                children = convert_options(opts_fn());
            pg.options.push_back(make_plugin(file, p.config, name, desc, date, std::move(children)));
        });
        std::sort(pg.options.begin(), pg.options.end(), [](const OptionPtr& a, const OptionPtr& b) {
            return _stricmp(a->label.c_str(), b->label.c_str()) < 0;
        });
        return pg;
    }

    Page patches_page(const Paths& p) {
        Page pg;
        pg.title = "Patches";
        pg.kind = PageKind::Patches;
        pg.empty_text = "No custom patches found. Put .p or .p2 files in the patches folder to list them here.";
        each_file(p.patches_dir + L"\\*.p*", [&](const std::wstring& file) {
            std::wstring name;
            if (has_ext(file, L".p")) name = file.substr(0, file.size() - 2);
            else if (has_ext(file, L".p2")) name = file.substr(0, file.size() - 3);
            else return;
            pg.options.push_back(make_plugin(file, p.config, name, L"Custom patch from the patches folder.", "", {}));
        });
        return pg;
    }

    // ------------------------------------------------------------ launching

    void launch_game(const Paths& p) {
        ini_invalidate();
        std::wstring exe = exe_path();
        std::wstring cmd;
        const wchar_t* app = exe.c_str();
        if (ini_get_bool(L"launcher", L"use_divahook_bat", false, p.config.c_str())) {
            std::wstring bat = p.game_dir + L"\\divahook.bat";
            if (!file_exists(bat))
                bat = p.game_dir + L"\\start.bat";
            if (file_exists(bat)) {
                cmd = L"cmd.exe /C \"" + bat + L"\"";
                app = nullptr;
            }
        }
        if (cmd.empty()) {
            std::wstring extra = ini_get_string(L"launcher", L"command_line", L"", p.config.c_str(), false);
            cmd = L"\"" + exe + L"\" --launch";
            if (!extra.empty())
                cmd += L" " + extra;
        }
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (CreateProcessW(app, buf.data(), nullptr, nullptr, FALSE, app ? 0 : CREATE_NEW_CONSOLE, nullptr, p.game_dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        TerminateProcess(GetCurrentProcess(), EXIT_SUCCESS);
    }

    // ------------------------------------------------------------ XInput (pads 0-3)

    typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
    PFN_XInputGetState g_xinput_get_state = nullptr;
    DWORD g_pad_retry[XUSER_MAX_COUNT] = {};

    void load_xinput() {
        const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll" };
        for (const wchar_t* n : dlls)
            if (HMODULE h = LoadLibraryW(n)) {
                g_xinput_get_state = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState");
                if (g_xinput_get_state)
                    return;
            }
    }

    void poll_pads() {
        ImGuiIO& io = ImGui::GetIO();
        if (!g_xinput_get_state)
            return;
        WORD buttons = 0;
        float lx = 0, ly = 0;
        BYTE lt = 0, rt = 0;
        bool any = false;
        DWORD now = GetTickCount();
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            // Polling a missing pad is slow on real XInput, so re-check those every 2 s
            if (g_pad_retry[i] && now < g_pad_retry[i])
                continue;
            XINPUT_STATE st = {};
            if (g_xinput_get_state(i, &st) != ERROR_SUCCESS) {
                g_pad_retry[i] = now + 2000;
                continue;
            }
            g_pad_retry[i] = 0;
            any = true;
            buttons |= st.Gamepad.wButtons;
            auto norm = [](SHORT v) { return v < 0 ? v / 32768.0f : v / 32767.0f; };
            float x = norm(st.Gamepad.sThumbLX), y = norm(st.Gamepad.sThumbLY);
            if (std::fabs(x) > std::fabs(lx)) lx = x;
            if (std::fabs(y) > std::fabs(ly)) ly = y;
            lt = std::max(lt, st.Gamepad.bLeftTrigger);
            rt = std::max(rt, st.Gamepad.bRightTrigger);
        }
        if (any)
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        else
            io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

        auto btn = [&](ImGuiKey k, WORD mask) { io.AddKeyEvent(k, (buttons & mask) != 0); };
        btn(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);
        btn(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
        btn(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
        btn(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
        btn(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
        btn(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
        btn(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
        btn(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
        btn(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
        btn(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
        btn(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
        btn(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, lt > 60, lt / 255.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, rt > 60, rt / 255.0f);
        // Stick as digital directions with a generous deadzone, so it navigates like the d-pad
        const float dz = 0.5f;
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < -dz, std::max(0.0f, -lx));
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > dz, std::max(0.0f, lx));
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > dz, std::max(0.0f, ly));
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < -dz, std::max(0.0f, -ly));
    }

    // ------------------------------------------------------------ window + D3D11

    ID3D11Device* g_dev = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;
    IDXGISwapChain* g_swap = nullptr;
    ID3D11RenderTargetView* g_rtv = nullptr;
    bool g_flip = false;
    HWND g_hwnd = nullptr;
    bool g_windowed = false;
    App* g_app = nullptr;
    bool g_quit = false;
    UINT g_pending_w = 0, g_pending_h = 0;

    void create_rtv() {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
            g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
            back->Release();
        }
    }

    void release_rtv() {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    }

    bool create_device(HWND hwnd) {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got;
        for (int attempt = 0; attempt < 4 && !g_swap; attempt++) {
            // 0/1: flip model on hardware/WARP (Windows 10, DXVK); 2/3: blt model fallback
            bool flip = attempt < 2;
            D3D_DRIVER_TYPE type = (attempt & 1) ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
            DXGI_SWAP_CHAIN_DESC sd = {};
            sd.BufferCount = 2;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
            HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, (UINT)std::size(levels),
                D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
            if (FAILED(hr)) {
                g_swap = nullptr;
                g_dev = nullptr;
                g_ctx = nullptr;
                continue;
            }
            g_flip = flip;
        }
        if (!g_swap)
            return false;
        // Alt+Enter is handled by us (borderless <-> windowed), not DXGI's exclusive mode
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&factory)))) {
            factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            factory->Release();
        }
        create_rtv();
        return true;
    }

    std::string adapter_name() {
        std::string out;
        IDXGIDevice* dxgi = nullptr;
        if (g_dev && SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxgi->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC d;
                if (SUCCEEDED(ad->GetDesc(&d)))
                    out = to_utf8(d.Description);
                ad->Release();
            }
            dxgi->Release();
        }
        return out;
    }

    void destroy_device() {
        release_rtv();
        if (g_swap) { g_swap->Release(); g_swap = nullptr; }
        if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
        if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    }

    RECT monitor_rect(HWND hwnd) {
        MONITORINFO mi = { sizeof(mi) };
        HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(mon, &mi);
        return mi.rcMonitor;
    }

    void set_windowed(bool windowed) {
        g_windowed = windowed;
        if (windowed) {
            RECT m = monitor_rect(g_hwnd);
            int mw = m.right - m.left, mh = m.bottom - m.top;
            int w = std::max(960, mw * 2 / 3), h = std::max(540, mh * 2 / 3);
            RECT r = { 0, 0, w, h };
            AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left + (mw - (r.right - r.left)) / 2, m.top + (mh - (r.bottom - r.top)) / 2,
                r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else {
            RECT m = monitor_rect(g_hwnd);
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(g_hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
    }

    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return 1;
        switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                g_pending_w = LOWORD(lp);
                g_pending_h = HIWORD(lp);
            }
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (lp & (1 << 29))) { // Alt+Enter
                set_windowed(!g_windowed);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wp == VK_F11) {
                set_windowed(!g_windowed);
                return 0;
            }
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) // don't open the (invisible) window menu on Alt
                return 0;
            break;
        case WM_CLOSE:
            if (g_app)
                g_app->request_close();
            else
                g_quit = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_DPICHANGED:
            if (g_windowed) {
                const RECT* r = (const RECT*)lp;
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void enable_dpi_awareness() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
        auto ctx = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (ctx && ctx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */))
            return;
        SetProcessDPIAware();
    }

    struct FontBlob { void* data = nullptr; int size = 0; };

    FontBlob load_resource(int id) {
        FontBlob b;
        HRSRC r = FindResourceW(g_module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        if (!r) return b;
        HGLOBAL g = LoadResource(g_module, r);
        if (!g) return b;
        b.data = LockResource(g);
        b.size = (int)SizeofResource(g_module, r);
        return b;
    }

    Fonts load_fonts() {
        ImGuiIO& io = ImGui::GetIO();
        Fonts f;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;  // resource memory belongs to the DLL
        FontBlob reg = load_resource(IDR_FONT_REGULAR);
        FontBlob bold = load_resource(IDR_FONT_BOLD);
        if (reg.data)
            f.regular = io.Fonts->AddFontFromMemoryTTF(reg.data, reg.size, 21.0f, &cfg);
        if (bold.data)
            f.bold = io.Fonts->AddFontFromMemoryTTF(bold.data, bold.size, 21.0f, &cfg);
        if (!f.regular) {
            // Fall back to a system font with Japanese coverage, then ImGui's own
            wchar_t win[MAX_PATH];
            GetWindowsDirectoryW(win, MAX_PATH);
            std::string meiryo = to_utf8(std::wstring(win) + L"\\Fonts\\meiryo.ttc");
            f.regular = file_exists(from_utf8(meiryo)) ? io.Fonts->AddFontFromFileTTF(meiryo.c_str(), 21.0f) : nullptr;
            if (!f.regular)
                f.regular = io.Fonts->AddFontDefault();
        }
        if (!f.bold)
            f.bold = f.regular;
        return f;
    }

    // ------------------------------------------------------------ UI loop

    int run_ui() {
        enable_dpi_awareness();
        load_xinput();
        Paths paths = make_paths();

        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_LAUNCHER));
        wc.lpszClassName = L"PDLoaderLauncherFS";
        RegisterClassExW(&wc);

        RECT m = monitor_rect(nullptr);
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"PD Loader", WS_POPUP,
            m.left, m.top, m.right - m.left, m.bottom - m.top, nullptr, nullptr, inst, nullptr);
        if (!g_hwnd || !create_device(g_hwnd)) {
            MessageBoxW(nullptr, L"The launcher couldn't create its window (Direct3D 11 is unavailable).\n\nTo start the game without the launcher, set skip=1 in the [launcher] section of plugins\\config.ini.",
                L"PD Loader", MB_ICONERROR);
            destroy_device();
            if (g_hwnd) DestroyWindow(g_hwnd);
            UnregisterClassW(wc.lpszClassName, inst);
            return 1;
        }
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        UpdateWindow(g_hwnd);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_dev, g_ctx);
        Fonts fonts = load_fonts();

        std::vector<Page> pages;
        Page home;
        home.title = "Home";
        home.kind = PageKind::Home;
        pages.push_back(home);
        for (Page& p : build_builtin_pages(paths, query_screen()))
            pages.push_back(std::move(p));
        pages.push_back(plugins_page(paths));
        pages.push_back(patches_page(paths));

        // Custom patches only load when Options > Enable Custom Patches is on
        OptionPtr custom;
        for (Page& p : pages)
            for (auto& o : p.options)
                if (o->key == L"custom_patches")
                    custom = o;
        if (custom)
            for (auto& o : pages.back().options) {
                o->enabled = [custom]() { return custom->b; };
                o->disabled_hint = "Turn on Enable Custom Patches in Options to use custom patches.";
            }

        Host host;
        host.gpu = adapter_name();
        host.wine = wine_version();
        host.launch = [paths]() { launch_game(paths); };
        host.quit = []() { g_quit = true; };
        host.toggle_windowed = []() { set_windowed(!g_windowed); };

        App app(std::move(pages), host, fonts);
        g_app = &app;

        // Hide the cursor until the mouse is actually used (controller-first on Steam Deck)
        POINT last_cursor;
        GetCursorPos(&last_cursor);
        bool cursor_visible = false;

        while (!g_quit) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT)
                    g_quit = true;
            }
            if (g_quit)
                break;
            if (g_pending_w && g_pending_h) {
                release_rtv();
                g_swap->ResizeBuffers(0, g_pending_w, g_pending_h, DXGI_FORMAT_UNKNOWN, 0);
                g_pending_w = g_pending_h = 0;
                create_rtv();
            }
            if (IsIconic(g_hwnd)) {
                Sleep(50);
                continue;
            }

            poll_pads();
            POINT cur;
            GetCursorPos(&cur);
            if (cur.x != last_cursor.x || cur.y != last_cursor.y) {
                cursor_visible = true;
                last_cursor = cur;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadR1; k++)
                if (ImGui::IsKeyPressed((ImGuiKey)k, false))
                    cursor_visible = false;
            // The Win32 backend applies this on the next frame (ImGuiMouseCursor_None hides it)
            ImGui::SetMouseCursor(cursor_visible ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_None);
            app.frame(io.DisplaySize.x, io.DisplaySize.y);
            ImGui::Render();

            const float clear[4] = { 0.07f, 0.063f, 0.17f, 1.0f };
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_ctx->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            HRESULT hr = g_swap->Present(1, 0);
            if (hr == DXGI_STATUS_OCCLUDED)
                Sleep(16);
        }

        g_app = nullptr;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        destroy_device();
        DestroyWindow(g_hwnd);
        UnregisterClassW(wc.lpszClassName, inst);
        return 0;
    }

    // ------------------------------------------------------------ divaMain hook

    typedef int(__cdecl* PFN_divaMain)(int argc, const char** argv, const char** envp);
    void* const DIVA_MAIN = (void*)0x140194D90;
    PFN_divaMain g_original_main = nullptr;

    int __cdecl hooked_main(int argc, const char** argv, const char** envp) {
        bool launch = ini_get_int(L"launcher", L"skip", 0, make_paths().config.c_str()) != 0;
        for (int i = 0; i < argc && !launch; i++)
            if (argv[i] && strcmp(argv[i], "--launch") == 0)
                launch = true;
        ini_invalidate();
        if (launch)
            return g_original_main(argc, argv, envp);
        return run_ui();
    }

    void install_hook() {
        // Same hook point as the original launcher (Detours on divaMain), via MinHook so
        // other plugins hooking the same function still chain correctly.
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(DIVA_MAIN, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT
            || !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE)))
            return; // not the PD Arcade Future Tone 7.10 executable
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
            return;
        if (MH_CreateHook(DIVA_MAIN, (void*)&hooked_main, (void**)&g_original_main) == MH_OK)
            MH_EnableHook(DIVA_MAIN);
    }
}

// ------------------------------------------------------------ plugin exports

extern "C" __declspec(dllexport) LPCWSTR GetPluginName() {
    return L"Launcher";
}

extern "C" __declspec(dllexport) LPCWSTR GetPluginDescription() {
    return L"The fullscreen PD Loader launcher you're using now. Works with a controller, mouse or keyboard.\n"
        L"To get it back after turning it off, set Launcher.dva=1 in the [plugins] section of plugins\\config.ini.";
}

extern "C" __declspec(dllexport) LPCWSTR GetBuildDate() {
    return L"" __DATE__;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        install_hook();
    }
    return TRUE;
}
