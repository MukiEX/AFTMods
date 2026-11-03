/*
    by korenkonder
    GitHub/GitLab: korenkonder
*/

#include <Windows.h>
#include "../KKdLib/default.hpp"
#include "hook.hpp"
#include "print.hpp"
#include "static_var.hpp"
#include "wrap.hpp"
#include <intrin.h>
#include <knownfolders.h>
#include <shlobj_core.h>
#include <PluginConfigApi/PluginConfigApi.h>

size_t diva_handle;
size_t glut_handle;
size_t opengl32_handle;
size_t dll_handle;
bool cpu_caps_aes_ni;
bool cpu_caps_f16c;

static std::wstring GetDirPath();

static bool parse_bool(const char* str, size_t length, int32_t& value);
static bool parse_int(const char* str, int32_t& value);
static void trim_space(const char*& str, size_t& length);

#ifdef DIVAVK
// DivaLinux: same renderer on a Vulkan backend (src/DivaVK), plus the FFmpeg movie player
// (src/DivaLinux/movie). Built only as DivaLinux.dva, for Wine/Proton.
extern "C" void divavk_bridge_install(HMODULE self);
extern "C" void divavk_bridge_shutdown();
extern "C" void divavk_log_text(const char* text);
extern "C" void divalinux_movie_install(const wchar_t* config_file);
#include <divavk/divavk.h>
#define DIVAGL_PLUGIN_NAME L"DivaLinux"
std::wstring CONFIG_FILE_STRING = GetDirPath() + L"\\plugins\\DivaLinux.ini";

// First run after switching from the separate plugins: start from their settings.
// DivaVK.ini (or DivaGL.ini) has the renderer and stereo options; DivaMovie.ini's [general]
// ffmpeg_player and movie_audio move to [movie]. Only done while DivaLinux.ini doesn't exist.
// [vr]: show the game in a headset through OpenXR (src/DivaVK/core/src/xr.cpp), as a virtual
// screen. With SBS stereo on, each eye gets its own half, so the screen is in 3D.
// Called by the bridge just before it creates the Vulkan context.
extern "C" bool divalinux_get_xr_info(dvk_xr_info* out) {
    const wchar_t* ini = CONFIG_FILE_STRING.c_str();
    if (GetPrivateProfileIntW(L"vr", L"enabled", 0, ini) <= 0)
        return false;
    static char runtime_json[MAX_PATH * 3] = {};
    wchar_t wjson[MAX_PATH] = {};
    GetPrivateProfileStringW(L"vr", L"runtime_json", L"", wjson, MAX_PATH, ini);
    WideCharToMultiByte(CP_UTF8, 0, wjson, -1, runtime_json, sizeof(runtime_json), nullptr, nullptr);

    *out = {};
    out->runtime_json = runtime_json[0] ? runtime_json : nullptr;
    out->app_name = "PD Loader (DivaLinux)";
    out->layout = sv_stereo_mode == 1 ? DVK_XR_LAYOUT_SBS_HALF
        : sv_stereo_mode == 2 ? DVK_XR_LAYOUT_SBS_FULL : DVK_XR_LAYOUT_MONO;
    out->swap_eyes = sv_stereo_swap_eyes;
    out->content_aspect = out->layout != DVK_XR_LAYOUT_MONO && sv_stereo_content_aspect > 0
        ? sv_stereo_content_aspect / 1000.0f : 0.0f;
    out->screen_width = clamp_def((int32_t)GetPrivateProfileIntW(L"vr", L"screen_width_cm", 300, ini), 20, 5000) / 100.0f;
    out->screen_distance = clamp_def((int32_t)GetPrivateProfileIntW(L"vr", L"screen_distance_cm", 250, ini), 20, 5000) / 100.0f;
    out->screen_height = clamp_def((int32_t)GetPrivateProfileIntW(L"vr", L"screen_height_cm", 0, ini), -500, 500) / 100.0f;
    return true;
}

static void divalinux_migrate_config() {
    if (GetFileAttributesW(CONFIG_FILE_STRING.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    std::wstring dir = GetDirPath() + L"\\plugins\\";
    for (const wchar_t* old : { L"DivaVK.ini", L"DivaGL.ini" })
        if (CopyFileW((dir + old).c_str(), CONFIG_FILE_STRING.c_str(), TRUE)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Created DivaLinux.ini from %ls", old);
            divavk_log_text(msg);
            break;
        }
    std::wstring movie_ini = dir + L"DivaMovie.ini";
    if (GetFileAttributesW(movie_ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
        for (const wchar_t* key : { L"ffmpeg_player", L"movie_audio" }) {
            wchar_t value[16] = {};
            if (GetPrivateProfileStringW(L"general", key, L"", value, 16, movie_ini.c_str()))
                WritePrivateProfileStringW(L"movie", key, value, CONFIG_FILE_STRING.c_str());
        }
        divavk_log_text("Copied movie settings from DivaMovie.ini");
    }
}
#else
#define DIVAGL_PLUGIN_NAME L"DivaGL"
std::wstring CONFIG_FILE_STRING = GetDirPath() + L"\\plugins\\DivaGL.ini";
#endif
LPCWSTR CONFIG_FILE = CONFIG_FILE_STRING.c_str();

void FASTCALL divagl_main() {
#ifdef DIVAVK
    // Must run before GLUT creates its context so every GL/WGL import lands in DivaVK.
    divavk_bridge_install((HMODULE)dll_handle);
#endif

    int32_t cpuid_data[4] = {};
    __cpuid(cpuid_data, 1);
    cpu_caps_aes_ni = (cpuid_data[2] & (1 << 25)) ? true : false;
    cpu_caps_f16c = (cpuid_data[2] & (1 << 29)) ? true : false;

    glut_handle = (size_t)GetModuleHandleW(L"glut32.dll");
    opengl32_handle = (size_t)GetModuleHandleW(L"opengl32.dll");

    if (memcmp((void*)(glut_handle + 0x0004CA68),
        "D:\\F\\DIVA\\DIVAAC\\package\\glut-3.7.6\\vc11\\lib\\x64\\Release\\glut32.pdb", 0x43)) {
        printf_divagl("Error! Unsupported glut32.dll.");
        printf_divagl("Please use original glut32.dll.");
        MessageBoxA(0, "Unsupported glut32.dll.\r\nPlease use original glut32.dll.", 0, MB_OK);
        return;
    }

    wrap_addresses();

    printf_divagl("Patching memory");
    hook_funcs();
    wrap_patch();
}

bool APIENTRY DllMain(HMODULE handle, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
#ifdef DIVAVK
        divalinux_migrate_config();
#endif
        sv_reflect_full = GetPrivateProfileIntW(L"general", L"reflect_full", 0, CONFIG_FILE) > 0 ? true : false;
        sv_reflect_res_scale = GetPrivateProfileIntW(L"general", L"reflect_res_scale", 100, CONFIG_FILE);
        sv_reflect_res_scale = sv_reflect_res_scale > 0 ? clamp_def(sv_reflect_res_scale, 25, 100) : 100;

        sv_shared_storage_uniform_buffer = GetPrivateProfileIntW(L"experimentals",
            L"shared_storage_uniform_buffer", 1, CONFIG_FILE) > 0 ? true : false;

        sv_task_movie_player_no_interop = GetPrivateProfileIntW(L"experimentals",
            L"task_movie_player_no_interop", 0, CONFIG_FILE) > 0 ? true : false;

        sv_texture_skinning_buffer = GetPrivateProfileIntW(L"experimentals",
            L"texture_skinning_buffer", 0, CONFIG_FILE) > 0 ? true : false;

        sv_stereo_mode = GetPrivateProfileIntW(L"stereo", L"stereo_mode", 0, CONFIG_FILE);
        sv_stereo_mode = clamp_def(sv_stereo_mode, 0, 2);
        sv_stereo_separation = clamp_def((int32_t)GetPrivateProfileIntW(L"stereo",
            L"stereo_separation", 65, CONFIG_FILE), 0, 500);
        sv_stereo_convergence = clamp_def((int32_t)GetPrivateProfileIntW(L"stereo",
            L"stereo_convergence", 0, CONFIG_FILE), 0, 100000);
        sv_stereo_swap_eyes = GetPrivateProfileIntW(L"stereo",
            L"stereo_swap_eyes", 0, CONFIG_FILE) > 0 ? true : false;
        sv_stereo_hud_parallax = clamp_def((int32_t)GetPrivateProfileIntW(L"stereo",
            L"stereo_hud_parallax", 0, CONFIG_FILE), -50, 50);
        sv_stereo_bg_infinity = GetPrivateProfileIntW(L"stereo",
            L"stereo_bg_infinity", 1, CONFIG_FILE) > 0 ? true : false;
        sv_stereo_content_aspect = clamp_def((int32_t)GetPrivateProfileIntW(L"stereo",
            L"stereo_content_aspect", 1778, CONFIG_FILE), 0, 10000);

#ifdef DIVAVK
        // No WGL_NV_DX_interop2 under Vulkan: always use the CPU upload path for videos.
        sv_task_movie_player_no_interop = true;
        divalinux_movie_install(CONFIG_FILE);
#endif

        dll_handle = (size_t)handle;
        printf_divagl("Current version - v0.11.0.0 (Build date: " __DATE__ ")");
        printf_divagl("Attach");
        divagl_main();
        break;
#ifdef DIVAVK
    case DLL_PROCESS_DETACH:
        divavk_bridge_shutdown();
        break;
#endif
    }
    return true;
}

extern "C" __declspec(dllexport) LPCWSTR GetPluginName(void) {
    return DIVAGL_PLUGIN_NAME;
}

extern "C" __declspec(dllexport) LPCWSTR GetPluginDescription(void) {
#ifdef DIVAVK
    return L"DivaLinux: DivaGL by korenkonder on Vulkan, with an FFmpeg movie player (from DivaMovie).\nFor Wine/Proton; on Windows use DivaGL.";
#else
    return L"DivaGL Plugin by korenkonder";
#endif
}

extern "C" __declspec(dllexport) LPCWSTR GetBuildDate(void) {
    return L"v0.11.0.0 (Build date: " __DATE__ ")";
}

extern "C" __declspec(dllexport) BOOL SetOption(const char* str) {
    if (!str)
        return FALSE;

    size_t str_len = strlen(str);

    const char* div = strchr(str, '=');
    if (!div)
        return FALSE;

    const char* name = str;
    size_t name_len = div - str;
    trim_space(name, name_len);
    if (!*name || !name_len)
        return FALSE;

    const char* val = div + 1;
    size_t val_len = str_len - (val - str);
    trim_space(val, val_len);
    if (!*val || !val_len)
        return FALSE;

    if (!strncmp(str, "reflect_full", name_len)) {
        int32_t value = 0;
        if (parse_bool(val, val_len, value))
            sv_reflect_full = !!value;
    }
    else if (!strncmp(name, "reflect_res_scale", name_len)) {
        int32_t value = 0;
        if (parse_int(val, value))
            sv_reflect_res_scale = value > 0 ? clamp_def(value, 25, 100) : 100;
    }
    else if (!strncmp(name, "shared_storage_uniform_buffer", name_len)) {
        int32_t value = 0;
        if (parse_bool(val, val_len, value))
            sv_shared_storage_uniform_buffer = !!value;
    }
    else if (!strncmp(name, "task_movie_player_no_interop", name_len)) {
        int32_t value = 0;
        if (parse_bool(val, val_len, value))
            sv_task_movie_player_no_interop = !!value;
    }
    else if (!strncmp(name, "texture_skinning_buffer", name_len)) {
        int32_t value = 0;
        if (parse_bool(val, val_len, value))
            sv_texture_skinning_buffer = !!value;
    }
    else if (!strncmp(name, "stereo_mode", name_len)) {
        int32_t value = 0;
        if (parse_int(val, value))
            sv_stereo_mode = clamp_def(value, 0, 2);
    }
    else if (!strncmp(name, "stereo_separation", name_len)) {
        int32_t value = 0;
        if (parse_int(val, value))
            sv_stereo_separation = clamp_def(value, 0, 500);
    }
    else if (!strncmp(name, "stereo_convergence", name_len)) {
        int32_t value = 0;
        if (parse_int(val, value))
            sv_stereo_convergence = clamp_def(value, 0, 100000);
    }
    else if (!strncmp(name, "stereo_swap_eyes", name_len)) {
        int32_t value = 0;
        if (parse_bool(val, val_len, value))
            sv_stereo_swap_eyes = !!value;
    }
    else if (!strncmp(name, "stereo_hud_parallax", name_len)) {
        int32_t value = 0;
        if (parse_int(val, value))
            sv_stereo_hud_parallax = clamp_def(value, -50, 50);
    }
    else if (!strncmp(name, "stereo_bg_infinity", name_len)) {
        int32_t value = 0;
        if (parse_bool(val, val_len, value))
            sv_stereo_bg_infinity = !!value;
    }
    else if (!strncmp(name, "stereo_content_aspect", name_len)) {
        int32_t value = 0;
        if (parse_int(val, value))
            sv_stereo_content_aspect = clamp_def(value, 0, 10000);
    }
    else
        return FALSE;

    return TRUE;
}

extern "C" __declspec(dllexport) BOOL SetOptionInt(const char* name, int value) {
    if (!strcmp(name, "reflect_full"))
        sv_reflect_full = !!value;
    else if (!strcmp(name, "reflect_res_scale"))
        sv_reflect_res_scale = value > 0 ? clamp_def(value, 25, 100) : 100;
    else if (!strcmp(name, "shared_storage_uniform_buffer"))
        sv_shared_storage_uniform_buffer = !!value;
    else if (!strcmp(name, "task_movie_player_no_interop"))
        sv_task_movie_player_no_interop = !!value;
    else if (!strcmp(name, "texture_skinning_buffer"))
        sv_texture_skinning_buffer = !!value;
    else if (!strcmp(name, "stereo_mode"))
        sv_stereo_mode = clamp_def(value, 0, 2);
    else if (!strcmp(name, "stereo_separation"))
        sv_stereo_separation = clamp_def(value, 0, 500);
    else if (!strcmp(name, "stereo_convergence"))
        sv_stereo_convergence = clamp_def(value, 0, 100000);
    else if (!strcmp(name, "stereo_swap_eyes"))
        sv_stereo_swap_eyes = !!value;
    else if (!strcmp(name, "stereo_hud_parallax"))
        sv_stereo_hud_parallax = clamp_def(value, -50, 50);
    else if (!strcmp(name, "stereo_bg_infinity"))
        sv_stereo_bg_infinity = !!value;
    else if (!strcmp(name, "stereo_content_aspect"))
        sv_stereo_content_aspect = clamp_def(value, 0, 10000);
    else
        return FALSE;

    return TRUE;
}

static std::wstring GetDirPath() {
    wchar_t exe_file_path[MAX_PATH];
    GetModuleFileNameW(0, exe_file_path, MAX_PATH);

    wchar_t* exe_file_name = wcsrchr(exe_file_path, '\\');
    if (!exe_file_name)
        exe_file_name = wcsrchr(exe_file_path, '/');

    if (!exe_file_name)
        return L"";

    WCHAR buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    return std::wstring(exe_file_path, exe_file_name - exe_file_path);
}

static bool parse_bool(const char* str, size_t length, int32_t& value) {
    if (sscanf_s(str, "%d", &value) == 1)
        return true;
    else if (!strncmp(str, "true", length)) {
        value = 1;
        return true;
    }
    else if (!strncmp(str, "false", length)) {
        value = 0;
        return true;
    }
    return false;
}

static bool parse_int(const char* str, int32_t& value) {
    if (sscanf_s(str, "%d", &value) == 1)
        return true;
    return false;
}

static void trim_space(const char*& str, size_t& length) {
    while (*str && isspace(*str)) {
        str++;
        length--;
    }

    while (length && isspace(str[length - 1]))
        length--;
}

PluginConfig::PluginConfigOption config[] = {
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"reflect_full",
            L"general",
            CONFIG_FILE,
            L"Full Reflect",
            L"Enables Full Reflect for PVs from X Pack (MPFX).",
            false,
            false,
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"reflect_res_scale",
            L"general",
            CONFIG_FILE,
            L"Reflect Resolution Scale",
            L"Sets resolution for Full Reflect as a percentage relative to the Internal Resolution values.\nImportant note! Scale can't go lower than 256x144 or Internal Resolution.",
            100,
            25,
            100,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"shared_storage_uniform_buffer",
            L"experimentals",
            CONFIG_FILE,
            L"Shared Storage/Uniform Buffer",
            L"May be useful for running AFT via Mesa or on Apple devices\nor when driver doesn't support OpenGL 4.3.",
            true,
            false,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"task_movie_player_no_interop",
            L"experimentals",
            CONFIG_FILE,
            L"TaskMovie Player No Interop",
            L"Should enable video playback if WGL_NV_DX_interop2 isn't supported.",
            false,
            false,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"texture_skinning_buffer",
            L"experimentals",
            CONFIG_FILE,
            L"Texture Skinning Buffer",
            L"If Shared Storage/Uniform Buffer isn't working for you.\nTry this option if you experience frame drops with multiple characters on screen.",
            false,
            false,
        }
    },
    {
        PluginConfig::CONFIG_DROPDOWN_INDEX,
        new PluginConfig::PluginConfigDropdownIndexData {
            L"stereo_mode",
            L"stereo",
            CONFIG_FILE,
            L"Stereoscopic 3D",
            L"Renders the game twice from two eye positions and outputs them side by side.\n"
            L"Half SBS: each eye squeezed to half width (3D TVs/monitors, XR glasses).\n"
            L"Full SBS: each eye keeps its aspect ratio, letterboxed (cross-eyed/parallel viewing, VR players).\n"
            L"Roughly doubles GPU load. TAA is disabled while stereo is on.",
            0,
            { L"Off", L"Half Side-by-Side", L"Full Side-by-Side" },
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"stereo_separation",
            L"stereo",
            CONFIG_FILE,
            L"Eye Separation (mm)",
            L"Distance between the two virtual eyes. 65 is a typical human IPD.\nHigher values give stronger depth.",
            65,
            0,
            500,
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"stereo_convergence",
            L"stereo",
            CONFIG_FILE,
            L"Convergence Distance (cm)",
            L"Distance that appears on the screen plane. Closer things pop out, farther things recede.\n0 = automatic (the point the PV camera is looking at, usually the singer).",
            0,
            0,
            100000,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"stereo_swap_eyes",
            L"stereo",
            CONFIG_FILE,
            L"Swap Eyes",
            L"Puts the right eye on the left half. Use for cross-eyed viewing or if depth looks inverted.",
            false,
            false,
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"stereo_hud_parallax",
            L"stereo",
            CONFIG_FILE,
            L"HUD Depth",
            L"Parallax of the HUD/notes in 0.1% of image width. 0 = on the screen plane,\npositive = behind the screen, negative = in front.",
            0,
            -50,
            50,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"stereo_bg_infinity",
            L"stereo",
            CONFIG_FILE,
            L"2D Backgrounds at Infinity",
            L"Places sprites drawn behind the 3D scene (2D stage backgrounds, PV movies) at infinite depth\ninstead of on the screen plane.",
            true,
            false,
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"stereo_content_aspect",
            L"stereo",
            CONFIG_FILE,
            L"Picture Aspect",
            L"Shape of the game's picture, width / height x 1000 (1778 = 16:9). Only this part of the screen\nis put into each eye, so black bars on wider or taller screens are left out.\n0 = use the whole screen.",
            1778,
            0,
            10000,
        }
    },
#ifdef DIVAVK
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"ffmpeg_player",
            L"movie",
            CONFIG_FILE,
            L"FFmpeg Movie Player",
            L"Play movies with FFmpeg (needs the FFmpeg DLLs in the plugins folder: avutil-61, swresample-7,\nswscale-10, avcodec-63, avformat-63). Off, or without them, the game's own player is used.",
            true,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"movie_audio",
            L"movie",
            CONFIG_FILE,
            L"Movie Audio",
            L"Play the audio tracks of movies (FFmpeg player only).\nNever plays during songs, where the video follows the song.",
            false,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"fast_decode",
            L"movie",
            CONFIG_FILE,
            L"Fast Movie Decoding",
            L"Decode movies faster at a small cost in picture quality (no deblocking filter), for slow CPUs\nsuch as a Quest or phone running the game through FEX. The player already does this by itself\nwhen it falls behind; this turns it on from the start.",
            false,
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"decode_threads",
            L"movie",
            CONFIG_FILE,
            L"Movie Decoding Threads",
            L"Threads FFmpeg uses to decode movies. 0 = one per CPU.",
            0,
            0,
            16,
        }
    },
    {
        PluginConfig::CONFIG_BOOLEAN,
        new PluginConfig::PluginConfigBooleanData {
            L"enabled",
            L"vr",
            CONFIG_FILE,
            L"VR Headset (OpenXR)",
            L"Show the game on a virtual screen in a headset, through the OpenXR runtime (SteamVR, Monado,\nWinlatorXR...). With Stereo 3D set to Full or Half SBS, the screen is in 3D.\nPress Home to put the screen back in front of you.",
            false,
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"screen_width_cm",
            L"vr",
            CONFIG_FILE,
            L"VR Screen Width (cm)",
            L"Width of the virtual screen in centimetres.",
            300,
            20,
            5000,
        }
    },
    {
        PluginConfig::CONFIG_NUMERIC,
        new PluginConfig::PluginConfigNumericData {
            L"screen_distance_cm",
            L"vr",
            CONFIG_FILE,
            L"VR Screen Distance (cm)",
            L"How far in front of you the virtual screen floats, in centimetres.",
            250,
            20,
            5000,
        }
    },
#endif
};

extern "C" __declspec(dllexport) PluginConfig::PluginConfigArray GetPluginOptions(void) {
    return PluginConfig::PluginConfigArray{ _countof(config), config };
}
