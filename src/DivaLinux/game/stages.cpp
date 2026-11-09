// Songs per credit, by mode: [Patches] Normal_Stages and Protected_Stages in config.ini
// (1-10; 0 or missing = the game's own 2 or 3). "Protected" is the test menu's ASSURE NEXT
// STAGE (the PROTECTED gauge: failing a song doesn't end the credit); "normal" is with it off.
//
// The game keeps a credit's songs in 4 slots and its screens only know a first, a middle and
// a final stage, so the stage index stays in those: the first song is 0, every song before
// the last is 1, the last is the game's final (count - 1), and the count the game sees is
// min(songs, 3). A counter of our own walks through the songs. (PD Loader's
// Enhanced_Stage_Manager also reuses the slots, with one count for both modes; with it on
// this stays off.)
//
// Under DivaRecomp the runtime does the same natively (DivaDroid runtime/src/pdloader_compat.c),
// so this stays off there.

#include <windows.h>
#include <detours.h>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uintptr_t STAGE_COUNT = 0x14038AEE0;    // int (record*): songs this credit (2 or 3)
constexpr uintptr_t STAGE_ADVANCE = 0x1403F6570;  // bool (): after a song, the next one or the end
constexpr uintptr_t STAGE_RECORD = 0x141197E00;   // the credit's record; +8 the stage index
constexpr uintptr_t ASSURE_NEXT = 0x1401E8850;    // int (): ASSURE NEXT STAGE is on

using CountFn = int32_t(__fastcall*)(void*);
using AdvanceFn = bool(__fastcall*)();
using AssureFn = int32_t(__fastcall*)();

CountFn o_count = (CountFn)STAGE_COUNT;
AdvanceFn o_advance = (AdvanceFn)STAGE_ADVANCE;
int g_normal, g_protected, g_song;   // g_song: this song, 0-based

int32_t& stage_index() { return *(int32_t*)(STAGE_RECORD + 8); }

int songs_wanted() {
    return ((AssureFn)ASSURE_NEXT)() ? g_protected : g_normal;
}

int stage_index_for(int n, int songs) {
    const int k = songs < 3 ? songs : 3;
    return n >= songs - 1 ? k - 1 : (n < k - 2 ? n : k - 2);
}

int32_t __fastcall hk_count(void* record) {
    const int songs = songs_wanted();
    return songs ? (songs < 3 ? songs : 3) : o_count(record);
}

bool __fastcall hk_advance() {
    const int songs = songs_wanted();
    const int before = stage_index();
    const bool r = o_advance();
    const int after = stage_index();
    if (songs && after != before) {   // on to the next song (not the credit's end)
        g_song = (before == 0 ? 0 : g_song) + 1;
        stage_index() = stage_index_for(g_song, songs);
    }
    return r;
}

int read_count(const wchar_t* key, const wchar_t* config) {
    const int n = (int)GetPrivateProfileIntW(L"patches", key, 0, config);
    return n < 0 ? 0 : n > 10 ? 10 : n;
}

} // namespace

extern "C" void divalinux_stages_install(const wchar_t* game_dir) {
    char tmp[8];
    if (GetEnvironmentVariableA("DIVA_ROOT", tmp, sizeof(tmp)) || GetEnvironmentVariableA("DIVARECOMP_OVERLAY", tmp, sizeof(tmp)))
        return;   // DivaRecomp does it itself
    wchar_t config[MAX_PATH];
    swprintf(config, MAX_PATH, L"%ls\\plugins\\config.ini", game_dir);
    g_normal = read_count(L"normal_stages", config);
    g_protected = read_count(L"protected_stages", config);
    if (!g_normal && !g_protected)
        return;
    if (GetPrivateProfileIntW(L"patches", L"enhanced_stage_manager", 0, config) > 0) {
        printf("[DivaLinux] Normal_Stages/Protected_Stages are off: Enhanced_Stage_Manager is on\n");
        return;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)o_count, (PVOID)hk_count);
    DetourAttach(&(PVOID&)o_advance, (PVOID)hk_advance);
    if (DetourTransactionCommit() == NO_ERROR)
        printf("[DivaLinux] songs per credit: normal %d, protected %d (0 = the game's)\n", g_normal, g_protected);
}
