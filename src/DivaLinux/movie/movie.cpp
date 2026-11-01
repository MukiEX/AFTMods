// DivaLinux: FFmpeg movie player (from DivaMovie's divamovie-ffmpeg branch).
//
// Replaces the game's Media Foundation movie player through its CreatePlayer factory. Frames
// are decoded on the CPU by FFmpeg and uploaded with DivaVK's own GL functions into a game
// texture, so movies never touch Media Foundation, DXVA2, Direct3D or WGL_NV_DX_interop, none
// of which work (or exist) here. DivaMovie's Windows paths (DXVA/MF hooks, interop emulation,
// diagnostics) are left out: under DivaLinux there is no real OpenGL for them to talk to.
//
// FFmpeg is loaded at run time from the plugins folder (avutil-61, swresample-7, swscale-10,
// avcodec-63, avformat-63). Without it the game's own player is used.

#include <windows.h>
#include <unknwn.h>
#include <mferror.h>   // MF_E_INVALIDREQUEST, which the game expects from some player calls
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <Helpers.h>

extern "C" void divavk_log_text(const char* text);

static void movie_log(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    divavk_log_text(buf);
}

#define PRINT(format, ...) movie_log(format, ##__VA_ARGS__)

static std::wstring DirPath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    std::wstring::size_type pos = path.find_last_of(L"\\/");
    return path.substr(0, pos);
}

#include "movie_gl.h"
#include "FFPlayer.h"

static bool s_ffmpeg_player = true;

HOOK(HRESULT, __fastcall, CreatePlayer, FFPlayer::ADDR_CREATE_PLAYER, void** player) {
    if (s_ffmpeg_player && player && FFPlayer::EnsureFFmpeg()) {
        *player = new FFPlayer::Player();
        PRINT("[Movie] FFmpeg player created\n");
        return S_OK;
    }
    return originalCreatePlayer(player);
}

extern "C" void divalinux_movie_install(const wchar_t* config_file) {
    s_ffmpeg_player = GetPrivateProfileIntW(L"movie", L"ffmpeg_player", 1, config_file) > 0;
    FFPlayer::movieAudio = GetPrivateProfileIntW(L"movie", L"movie_audio", 0, config_file) > 0;

    if (!s_ffmpeg_player) {
        PRINT("[Movie] FFmpeg player off ([movie] ffmpeg_player=0); using the game's movie player\n");
        return;
    }
    // Check before hooking: Detours overwrites the bytes being checked.
    if (!FFPlayer::IsSupportedExecutable()) {
        PRINT("[Movie] Unrecognised movie player code (not diva.exe 7.10?); FFmpeg player unavailable\n");
        return;
    }
    INSTALL_HOOK(CreatePlayer);
    PRINT("[Movie] FFmpeg player installed (movie audio %s)\n", FFPlayer::movieAudio ? "on" : "off");
}
