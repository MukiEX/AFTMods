/* DivaOnline.zip, next to DivaOnline.dva: a known-good copy of the files segatools and
   ALL.Net keep in the game folder, with the folders at the top of the archive:
     DEVICE\eeprom.bin, DEVICE\sram.bin, ...   the arcade board's EEPROM and SRAM
     billing\ca.crt, billing\billing.pub       ALL.Net's billing certificates
   At startup, before anything opens them:
   - [divaonline] reset_device = 1 deletes DEVICE\eeprom.bin and DEVICE\sram.bin, and every
     file the zip has a copy of, then sets itself back to 0 (the overlay's "Reset device data").
   - If any folder copy of a file in the zip is newer than the zip, the folders are kept as
     they are (the game has been writing them); only files missing from them come from the
     zip. Otherwise every file is written out from the zip. Either way the game then reads
     and writes the folders as before, so its changes stay until the zip is replaced.
   Only DEVICE\ and billing\ entries are used. */

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "divaonline.h"
#include "zipread.h"
#include "util/dprintf.h"

#define DEVICEZIP_MAX 64

struct devicezip_file {
    struct zip_entry_info entry;
    wchar_t path[MAX_PATH];   /* where it goes in the game folder */
};

static uint8_t *devicezip_read_file(const wchar_t *path, size_t *n)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    uint8_t *data = NULL;
    DWORD got = 0;

    if (f == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 256 * 1024 * 1024) {
        data = malloc((size_t) size.QuadPart);

        if (data != NULL && (!ReadFile(f, data, (DWORD) size.QuadPart, &got, NULL) || got != (DWORD) size.QuadPart)) {
            free(data);
            data = NULL;
        }
    }

    CloseHandle(f);
    *n = got;

    return data;
}

static bool devicezip_write_file(const wchar_t *path, const uint8_t *data, size_t n)
{
    wchar_t tmp[MAX_PATH + 8];
    DWORD put = 0;
    bool ok;

    _snwprintf(tmp, _countof(tmp), L"%s.new", path);
    tmp[_countof(tmp) - 1] = L'\0';

    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }

    ok = n == 0 || (WriteFile(f, data, (DWORD) n, &put, NULL) && put == (DWORD) n);
    CloseHandle(f);

    if (ok) {
        ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    }

    if (!ok) {
        DeleteFileW(tmp);
    }

    return ok;
}

/* DEVICE/x or billing/x (either slash, any case), no "..", ASCII; game_dir\DEVICE\x */
static bool devicezip_dest(const wchar_t *game_dir, const char *name, wchar_t *out, size_t cap)
{
    size_t gl = wcslen(game_dir), nl = strlen(name);

    if (nl == 0 || name[nl - 1] == '/' || name[nl - 1] == '\\' || strstr(name, "..") != NULL) {
        return false;
    }

    if (_strnicmp(name, "DEVICE", 6) != 0 && _strnicmp(name, "billing", 7) != 0) {
        return false;
    }

    size_t top = name[0] == 'D' || name[0] == 'd' ? 6 : 7;

    if (name[top] != '/' && name[top] != '\\') {
        return false;
    }

    if (gl + 1 + nl + 1 > cap) {
        return false;
    }

    wcscpy(out, game_dir);
    out[gl] = L'\\';

    for (size_t i = 0; i <= nl; i++) {
        unsigned char c = (unsigned char) name[i];

        if (c >= 0x80) {
            return false;
        }

        out[gl + 1 + i] = c == '/' ? L'\\' : (wchar_t) c;
    }

    return true;
}

/* the folders between game_dir and the file */
static void devicezip_make_dirs(const wchar_t *game_dir, const wchar_t *path)
{
    wchar_t dir[MAX_PATH];

    wcsncpy(dir, path, _countof(dir) - 1);
    dir[_countof(dir) - 1] = L'\0';

    for (wchar_t *p = dir + wcslen(game_dir) + 1; *p; p++) {
        if (*p == L'\\') {
            *p = L'\0';
            CreateDirectoryW(dir, NULL);
            *p = L'\\';
        }
    }
}

static bool devicezip_mtime(const wchar_t *path, FILETIME *t)
{
    WIN32_FILE_ATTRIBUTE_DATA a;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    *t = a.ftLastWriteTime;

    return true;
}

static void devicezip_reset(const wchar_t *ini, const wchar_t *game_dir,
        const struct devicezip_file *files, size_t count)
{
    static const wchar_t *always[] = { L"DEVICE\\eeprom.bin", L"DEVICE\\sram.bin" };
    wchar_t path[MAX_PATH];
    unsigned int deleted = 0;

    for (size_t i = 0; i < _countof(always); i++) {
        _snwprintf(path, _countof(path), L"%s\\%s", game_dir, always[i]);
        path[_countof(path) - 1] = L'\0';
        deleted += DeleteFileW(path) ? 1 : 0;
    }

    for (size_t i = 0; i < count; i++) {
        deleted += DeleteFileW(files[i].path) ? 1 : 0;
    }

    WritePrivateProfileStringW(L"divaonline", L"reset_device", L"0", ini);
    dprintf("Device files: reset (reset_device=1): %u files deleted%s\n", deleted,
            count ? "; DivaOnline.zip's copies go back in" : "; the game makes new EEPROM and SRAM files");
}

void divaonline_device_files(const wchar_t *ini, const wchar_t *plugins_dir, const wchar_t *game_dir)
{
    static struct devicezip_file files[DEVICEZIP_MAX];
    wchar_t zip_path[MAX_PATH];
    struct zip_archive z;
    uint8_t *data = NULL;
    size_t n = 0, count = 0;
    FILETIME zip_time;

    _snwprintf(zip_path, _countof(zip_path), L"%s\\DivaOnline.zip", plugins_dir);
    zip_path[_countof(zip_path) - 1] = L'\0';

    if (devicezip_mtime(zip_path, &zip_time)) {
        data = devicezip_read_file(zip_path, &n);

        if (data == NULL || zip_open(&z, data, n) != 0) {
            dprintf("Device files: couldn't read %S as a zip; using the folders as they are\n", zip_path);
            free(data);
            data = NULL;
        } else {
            for (uint32_t i = 0; i < z.count && count < DEVICEZIP_MAX; i++) {
                struct devicezip_file *f = &files[count];

                if (zip_entry(&z, i, &f->entry) == 0
                        && devicezip_dest(game_dir, f->entry.name, f->path, _countof(f->path))) {
                    count++;
                }
            }
        }
    }

    if (GetPrivateProfileIntW(L"divaonline", L"reset_device", 0, ini) != 0) {
        devicezip_reset(ini, game_dir, files, count);
    }

    if (data == NULL) {
        return;
    }

    if (count == 0) {
        dprintf("Device files: %S has no DEVICE\\ or billing\\ files\n", zip_path);
        free(data);

        return;
    }

    /* Has the game written any of them since the zip was made? */
    bool folders_newer = false;

    for (size_t i = 0; i < count; i++) {
        FILETIME t;

        if (devicezip_mtime(files[i].path, &t) && CompareFileTime(&t, &zip_time) > 0) {
            folders_newer = true;
        }
    }

    unsigned int written = 0, failed = 0;

    for (size_t i = 0; i < count; i++) {
        FILETIME t;

        if (folders_newer && devicezip_mtime(files[i].path, &t)) {
            continue;
        }

        uint8_t *contents = zip_extract(&z, &files[i].entry);

        devicezip_make_dirs(game_dir, files[i].path);

        if (contents != NULL && devicezip_write_file(files[i].path, contents, files[i].entry.size)) {
            written++;
        } else {
            failed++;
            dprintf("Device files: couldn't write %s from DivaOnline.zip%s\n", files[i].entry.name,
                    contents == NULL ? " (damaged, or an unsupported compression)" : "");
        }

        free(contents);
    }

    if (folders_newer) {
        dprintf("Device files: the folders are newer than DivaOnline.zip, so they're kept"
                "; files missing from them written from it: %u\n", written);
    } else {
        dprintf("Device files: %u files written from DivaOnline.zip%s\n", written,
                failed ? "; some couldn't be" : "");
    }

    free(data);
}
