/* Checks that, with DivaOnline on, a C runtime read of plugins/components.ini
   (how TLAC reads it) gets the copy with player_data_manager and score_saver
   off, and that the real file is untouched. Run under Wine or Windows next to
   plugins/DivaOnline.dva, plugins/DivaOnline.ini and plugins/components.ini.
   Build: x86_64-w64-mingw32-clang -O1 -o tlac_test.exe tlac_test.c */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* TLAC's ConfigFile: first "key = value" for a key wins */
static void first_value(const wchar_t *path, const char *key, char *out, size_t n)
{
    char line[512];
    FILE *f = _wfopen(path, L"r");
    out[0] = 0;
    if (!f) { strcpy(out, "(no file)"); return; }
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == '[' || !eq) continue;
        char k[128]; size_t kl = (size_t)(eq - line);
        while (kl && line[kl - 1] == ' ') kl--;
        memcpy(k, line, kl); k[kl] = 0;
        if (strcmp(k, key) == 0) {
            char *v = eq + 1; while (*v == ' ') v++;
            v[strcspn(v, "\r\n ")] = 0;
            snprintf(out, n, "%s", v);
            break;
        }
    }
    fclose(f);
}

int main(void)
{
    wchar_t dir[MAX_PATH], tlac_path[MAX_PATH];
    char v[64];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = 0;
    swprintf(tlac_path, MAX_PATH, L"%ls\\plugins/components.ini", dir);   /* TLAC: dir + "/" + name */

    first_value(tlac_path, "player_data_manager", v, sizeof(v));
    CHECK(strcmp(v, "true") == 0, "before DivaOnline: player_data_manager = %s", v);

    HMODULE m = LoadLibraryW(L"plugins\\DivaOnline.dva");
    ((void (*)(void)) GetProcAddress(m, "InitializeDVA"))();

    first_value(tlac_path, "player_data_manager", v, sizeof(v));
    CHECK(strcmp(v, "false") == 0, "TLAC-style read: player_data_manager = %s", v);
    first_value(tlac_path, "score_saver", v, sizeof(v));
    CHECK(strcmp(v, "false") == 0, "TLAC-style read: score_saver = %s", v);
    first_value(tlac_path, "input_emulator", v, sizeof(v));
    CHECK(strcmp(v, "true") == 0, "other components unchanged: input_emulator = %s", v);

    /* a writer must see (and change) the real file */
    {
        HANDLE h = CreateFileW(tlac_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char buf[64] = {0}; DWORD r = 0;
        if (h != INVALID_HANDLE_VALUE) { ReadFile(h, buf, 20, &r, NULL); CloseHandle(h); }
        CHECK(strncmp(buf, "# Written by", 12) != 0, "read/write open gets the real file");
    }

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures != 0;
}
