/* While DivaOnline is on, the player's data comes from the server, so TLAC's
   player_data_manager (which writes plugins\playerdata.ini over the game's
   player data) and score_saver (plugins\scores.ini) are switched off for this
   run. Neither file, nor components.ini, is changed: TLAC is handed a copy of
   components.ini with

       player_data_manager = false
       score_saver = false

   on top (TLAC keeps the first value it reads for a key). With
   [divaonline] enable=0 DivaOnline does nothing, and TLAC reads the real file.

   TLAC reads components.ini with std::ifstream, which opens it through the
   C runtime (ucrtbase.dll) or, for a statically linked TLAC, TLAC's own
   imports. CreateFileW is hooked in those modules once every plugin has
   loaded (PD Loader's launcher has run by then), and only read-only opens of
   plugins\components.ini are redirected, for a few seconds after the first
   one: long enough for TLAC's ComponentsManager and FastLoader, which read it
   back to back on the game's first frame, and not for a settings menu opened
   later, which could otherwise save the copy's values over the real file. */

#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "hook/table.h"

#include "util/dprintf.h"

#include "divaonline.h"

#define TLAC_REDIRECT_WINDOW_MS 10000

static wchar_t tlac_real[MAX_PATH];     /* ...\plugins\components.ini */
static wchar_t tlac_copy[MAX_PATH];     /* ...\DEVICE\tlac_components.ini */
static DWORD tlac_first_redirect;
static LONG tlac_redirects;
static bool tlac_ready;

static HANDLE (WINAPI *tlac_next_CreateFileW)(
        LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
        DWORD disposition, DWORD flags, HANDLE templ);

static HANDLE WINAPI tlac_CreateFileW(
        LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
        DWORD disposition, DWORD flags, HANDLE templ);

static const struct hook_symbol tlac_syms[] = {
    {
        .name   = "CreateFileW",
        .patch  = tlac_CreateFileW,
        .link   = (void **) &tlac_next_CreateFileW,
    },
};

/* Same file? / and \ count alike, runs of them count as one (TLAC builds
   the path as directory + "/" + name), and case doesn't matter. */
static bool tlac_same_path(const wchar_t *a, const wchar_t *b)
{
    bool sa;
    bool sb;

    while (*a != L'\0' && *b != L'\0') {
        sa = *a == L'/' || *a == L'\\';
        sb = *b == L'/' || *b == L'\\';

        if (sa || sb) {
            if (!(sa && sb)) {
                return false;
            }

            while (*a == L'/' || *a == L'\\') {
                a++;
            }

            while (*b == L'/' || *b == L'\\') {
                b++;
            }

            continue;
        }

        if (towlower(*a) != towlower(*b)) {
            return false;
        }

        a++;
        b++;
    }

    return *a == *b;
}

static HANDLE WINAPI tlac_CreateFileW(
        LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
        DWORD disposition, DWORD flags, HANDLE templ)
{
    DWORD now;

    if (tlac_ready
            && name != NULL
            && disposition == OPEN_EXISTING
            && (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) == 0
            && tlac_same_path(name, tlac_real)) {
        now = GetTickCount();

        if (tlac_redirects == 0) {
            tlac_first_redirect = now;
        }

        if (now - tlac_first_redirect < TLAC_REDIRECT_WINDOW_MS) {
            InterlockedIncrement(&tlac_redirects);
            dprintf("TLAC: components.ini read; handing over a copy with player_data_manager"
                    " and score_saver off (playerdata.ini and scores.ini are left alone)\n");
            name = tlac_copy;
        }
    }

    return tlac_next_CreateFileW(name, access, share, sa, disposition, flags, templ);
}

static bool tlac_write_copy(void)
{
    static const char header[] =
        "# Written by DivaOnline.dva from plugins\\components.ini for this run:\n"
        "# with DivaOnline on, the player's data comes from the server.\n"
        "player_data_manager = false\n"
        "score_saver = false\n"
        "\n";
    FILE *in;
    FILE *out;
    char buf[4096];
    size_t n;

    in = _wfopen(tlac_real, L"rb");

    if (in == NULL) {
        return false;   /* no components.ini: nothing for TLAC to enable */
    }

    out = _wfopen(tlac_copy, L"wb");

    if (out == NULL) {
        fclose(in);
        dprintf("TLAC: could not write %S\n", tlac_copy);

        return false;
    }

    fwrite(header, 1, sizeof(header) - 1, out);

    /* Skip a UTF-8 BOM: TLAC only looks for one at the very start */

    n = fread(buf, 1, 3, in);

    if (!(n == 3 && memcmp(buf, "\xEF\xBB\xBF", 3) == 0)) {
        fwrite(buf, 1, n, out);
    }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }

    fclose(in);
    fclose(out);

    return true;
}

void tlac_ignore_player_files(const wchar_t *plugins_dir, const wchar_t *game_dir)
{
    static const wchar_t *const modules[] = {
        L"TLAC.dva", L"ucrtbase.dll", L"msvcp140.dll",
    };
    HMODULE mod;
    size_t i;

    if (tlac_ready) {
        return;
    }

    _snwprintf(tlac_real, _countof(tlac_real), L"%s\\components.ini", plugins_dir);
    tlac_real[_countof(tlac_real) - 1] = L'\0';
    _snwprintf(tlac_copy, _countof(tlac_copy), L"%s\\DEVICE\\tlac_components.ini", game_dir);
    tlac_copy[_countof(tlac_copy) - 1] = L'\0';

    if (!tlac_write_copy()) {
        return;
    }

    for (i = 0 ; i < _countof(modules) ; i++) {
        mod = GetModuleHandleW(modules[i]);

        if (mod != NULL) {
            hook_table_apply(mod, "kernel32.dll", tlac_syms, _countof(tlac_syms));
        }
    }

    if (tlac_next_CreateFileW == NULL) {
        dprintf("TLAC: no CreateFileW import to hook; playerdata.ini/scores.ini stay in use\n");

        return;
    }

    tlac_ready = true;
}
