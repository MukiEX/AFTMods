/* DivaOnline.dva: segatools' Project DIVA Arcade emulation for ALL.Net and the
   Aime card reader, as a PD Loader plugin for diva.exe 7.10 (Future Tone).

   This is divahook's diva_pre_startup() (segatools, games/divahook/dllmain.c)
   without the parts PD Loader already does: no JVS I/O board, touch slider,
   touch screen, printer or graphics hooks. What's left:

     platform  keychip (nusec), DNS redirection to the ALL.Net server, network
               environment, clock, PCB id, hardware monitor/reset, misc, and
               vfs (the E:\ and Y:\ drives)
     amex      DS (keychip EEPROM), EEPROM, SRAM, GPIO (off by default)
     aime      the TN32MSEC003S Aime reader on COM10, with the card number
               from DivaOnline.ini

   Settings are read from plugins\DivaOnline.ini, with segatools.ini's section
   and key names, so a working segatools.ini can be pasted in. */

#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>

#include "amex/config.h"
#include "amex/ds.h"
#include "amex/eeprom.h"
#include "amex/gpio.h"
#include "amex/sram.h"

#include "board/config.h"
#include "board/sg-reader.h"

#include "hook/table.h"

#include "hooklib/serial.h"

#include "platform/clock.h"
#include "platform/config.h"
#include "platform/dns.h"
#include "platform/hwmon.h"
#include "platform/hwreset.h"
#include "platform/misc.h"
#include "platform/netenv.h"
#include "platform/nusec.h"
#include "platform/pcbid.h"
#include "platform/platform.h"
#include "platform/vfs.h"

#include "util/dprintf.h"

#include "divaonline.h"

#define DIVA_GAME_ID        "SBZV"
#define DIVA_PLATFORM_ID    "AAV0"
#define DIVA_AIME_PORT      10

static HMODULE divaonline_mod;
static wchar_t divaonline_ini[MAX_PATH];
static wchar_t divaonline_game_dir[MAX_PATH];
static bool divaonline_started;

static struct platform_config divaonline_platform;
static struct amex_config divaonline_amex;
static struct aime_config divaonline_aime;

static wchar_t divaonline_plugins[MAX_PATH];

const wchar_t *divaonline_ini_path(void)
{
    return divaonline_ini;
}

const wchar_t *divaonline_plugins_dir(void)
{
    return divaonline_plugins;
}

const wchar_t *divaonline_game_dir_path(void)
{
    return divaonline_game_dir;
}

static void divaonline_paths(void)
{
    wchar_t *slash;

    GetModuleFileNameW(divaonline_mod, divaonline_ini, _countof(divaonline_ini));
    slash = wcsrchr(divaonline_ini, L'\\');

    if (slash != NULL) {
        slash[1] = L'\0';
    }

    wcsncpy(divaonline_plugins, divaonline_ini, _countof(divaonline_plugins) - 1);
    divaonline_plugins[wcslen(divaonline_plugins) - 1] = L'\0';    /* drop the trailing \ */

    wcsncat(divaonline_ini, L"DivaOnline.ini", _countof(divaonline_ini) - wcslen(divaonline_ini) - 1);

    GetModuleFileNameW(NULL, divaonline_game_dir, _countof(divaonline_game_dir));
    slash = wcsrchr(divaonline_game_dir, L'\\');

    if (slash != NULL) {
        slash[0] = L'\0';
    }
}

/* DivaLinux's file layer hooks diva.exe's CreateFile/ReadFile/CloseHandle
   imports, and leaves any import that another plugin hooked first alone. The
   hooks below chain on to whatever is in those imports, so DivaLinux has to
   be in first. PD Loader loads plugins in directory order, which puts
   DivaLinux first; loading it here makes sure of it (PD Loader then skips it,
   and it has no InitializeDVA to miss). */
static void divaonline_preload_divalinux(void)
{
    wchar_t path[MAX_PATH];
    wchar_t pd_config[MAX_PATH];
    wchar_t *slash;
    DWORD attr;

    if (GetModuleHandleW(L"DivaLinux.dva") != NULL) {
        return;
    }

    /* PD Loader's config.ini lives in plugins\ (next to us); older setups kept
       it in the game folder */

    GetModuleFileNameW(divaonline_mod, pd_config, _countof(pd_config));
    slash = wcsrchr(pd_config, L'\\');

    if (slash != NULL) {
        slash[1] = L'\0';
    }

    wcsncat(pd_config, L"config.ini", _countof(pd_config) - wcslen(pd_config) - 1);

    if (GetFileAttributesW(pd_config) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(pd_config, _countof(pd_config), L"%s\\config.ini", divaonline_game_dir);
        pd_config[_countof(pd_config) - 1] = L'\0';
    }

    if (!GetPrivateProfileIntW(L"global", L"enable", 1, pd_config)
            || !GetPrivateProfileIntW(L"plugins", L"DivaLinux.dva", 1, pd_config)) {
        return;
    }

    GetModuleFileNameW(divaonline_mod, path, _countof(path));
    slash = wcsrchr(path, L'\\');

    if (slash == NULL) {
        return;
    }

    slash[1] = L'\0';
    wcsncat(path, L"DivaLinux.dva", _countof(path) - wcslen(path) - 1);
    attr = GetFileAttributesW(path);

    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    dprintf("Loading DivaLinux.dva first, so its file hooks are in place before ours\n");

    if (LoadLibraryW(path) == NULL) {
        dprintf("Could not load DivaLinux.dva (error %lu)\n", GetLastError());
    }
}

/* segatools resolves [vfs] paths against the current directory, which PD
   Loader points at plugins\ while loading: anchor them to the game folder,
   and give AMFS/APPDATA folders there when the .ini leaves them out (segatools
   refuses to start without them). */
static void divaonline_vfs_path(wchar_t *path, size_t max, const wchar_t *fallback)
{
    wchar_t tmp[MAX_PATH];

    if (path[0] == L'\0') {
        if (fallback == NULL) {
            return;
        }

        wcsncpy(path, fallback, max - 1);
        path[max - 1] = L'\0';
    }

    if (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\')) {
        return;     /* already absolute */
    }

    _snwprintf(tmp, _countof(tmp), L"%s\\%s", divaonline_game_dir, path);
    tmp[_countof(tmp) - 1] = L'\0';
    wcsncpy(path, tmp, max - 1);
    path[max - 1] = L'\0';
}

#define DIVAONLINE_STEP(expr, what) \
    do { \
        HRESULT step_hr = (expr); \
        if (FAILED(step_hr)) { \
            dprintf("%s failed: %08lx\n", what, (unsigned long) step_hr); \
            ok = false; \
        } \
    } while (0)

static void divaonline_start(void)
{
    struct fixups_config fixups;
    wchar_t log_path[MAX_PATH];
    wchar_t device_dir[MAX_PATH];
    bool ok = true;

    divaonline_paths();

    GetPrivateProfileStringW(L"divaonline", L"log_file", L"", log_path, _countof(log_path), divaonline_ini);
    divaonline_log_init(log_path);

    if (!GetPrivateProfileIntW(L"divaonline", L"enable", 1, divaonline_ini)) {
        dprintf("Disabled in %S ([divaonline] enable=0)\n", divaonline_ini);

        return;
    }

    if (GetFileAttributesW(divaonline_ini) == INVALID_FILE_ATTRIBUTES) {
        dprintf("%S not found: using segatools' defaults, which point ALL.Net at localhost\n",
                divaonline_ini);
    }

    dprintf("--- Begin DivaOnline init (%S) ---\n", divaonline_ini);

    if (GetPrivateProfileIntW(L"divaonline", L"preload_divalinux", 1, divaonline_ini)) {
        divaonline_preload_divalinux();
    }

    /* segatools keeps its emulated EEPROM/SRAM/card files in DEVICE\ by default */

    _snwprintf(device_dir, _countof(device_dir), L"%s\\DEVICE", divaonline_game_dir);
    device_dir[_countof(device_dir) - 1] = L'\0';
    CreateDirectoryW(device_dir, NULL);

    /* DivaOnline.zip's DEVICE\, billing\ and firm\ files, and the overlay's reset */
    divaonline_device_files(divaonline_ini, divaonline_plugins, divaonline_game_dir);

    /* Config load: segatools.ini's sections ([keychip], [dns], [netenv],
       [ds], [eeprom], [sram], [gpio], [aime], [aimeio], ...) */

    platform_config_load(&divaonline_platform, divaonline_ini);
    amex_config_load(&divaonline_amex, divaonline_ini);
    aime_config_load(&divaonline_aime, divaonline_ini);

    /* Defaults that differ from segatools' when the key isn't in the .ini:
       - [netenv] on: segatools' Diva setups turn it on, so ALL.Net sees the
         LAN (router, subnet) it expects whatever the PC's network is.
       ([gpio] and [keychip] subnet: below.) */

    divaonline_platform.netenv.enable =
            GetPrivateProfileIntW(L"netenv", L"enable", 1, divaonline_ini) != 0;
    /* [gpio] follows the TeamTofuShop fork's dist/diva/segatools.ini when keys
       are missing: on, DIP switch 1 (distribution server) set, so the cabinet
       starts as SATELLITE:MAIN and is its own location server (as SUB it
       looks for one on the LAN: "Location Server NG", and no card system).
       Test/Service on F4/F5, clear of TLAC's defaults. */

    divaonline_amex.gpio.enable =
            GetPrivateProfileIntW(L"gpio", L"enable", 1, divaonline_ini) != 0;
    divaonline_amex.gpio.dipsw[0] =
            GetPrivateProfileIntW(L"gpio", L"dipsw1", 1, divaonline_ini) != 0;
    divaonline_amex.gpio.vk_sw1 =
            (uint8_t) GetPrivateProfileIntW(L"gpio", L"sw1", VK_F4, divaonline_ini);
    divaonline_amex.gpio.vk_sw2 =
            (uint8_t) GetPrivateProfileIntW(L"gpio", L"sw2", VK_F5, divaonline_ini);

    /* ...and so does the keychip's subnet (Diva's is 192.168.78.0) */
    {
        wchar_t subnet[16];
        unsigned int ip[4] = { 0 };

        GetPrivateProfileStringW(L"keychip", L"subnet", L"192.168.78.0", subnet, _countof(subnet), divaonline_ini);

        if (swscanf(subnet, L"%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]) == 4) {
            divaonline_platform.nusec.subnet = (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8);
        }
    }

    divaonline_vfs_path(divaonline_platform.vfs.amfs, _countof(divaonline_platform.vfs.amfs), L"amfs");
    divaonline_vfs_path(divaonline_platform.vfs.appdata, _countof(divaonline_platform.vfs.appdata), L"appdata");
    divaonline_vfs_path(divaonline_platform.vfs.option, _countof(divaonline_platform.vfs.option), NULL);

    /* Pin dbghelp/imagehlp so the path hooks reach them (see hook/table.c) */

    LoadLibraryW(L"imagehlp.dll");
    LoadLibraryW(L"dbghelp.dll");

    /* Hook Win32 APIs (diva.exe's imports, plus imagehlp/dbghelp) */

    serial_hook_init();

    /* platform_hook_init() minus amvideo (diva.exe doesn't use amvideo.dll) */

    DIVAONLINE_STEP(clock_hook_init(&divaonline_platform.clock), "Clock hook");
    DIVAONLINE_STEP(dns_platform_hook_init(&divaonline_platform.dns), "DNS hook");
    DIVAONLINE_STEP(hwmon_hook_init(&divaonline_platform.hwmon), "Hardware monitor hook");
    DIVAONLINE_STEP(hwreset_hook_init(&divaonline_platform.hwreset), "Hardware reset hook");
    DIVAONLINE_STEP(misc_hook_init(&divaonline_platform.misc, DIVA_PLATFORM_ID), "Misc hook");
    DIVAONLINE_STEP(netenv_hook_init(&divaonline_platform.netenv, &divaonline_platform.nusec), "Netenv hook");
    DIVAONLINE_STEP(nusec_hook_init(&divaonline_platform.nusec, DIVA_GAME_ID, DIVA_PLATFORM_ID), "Keychip hook");
    DIVAONLINE_STEP(pcbid_hook_init(&divaonline_platform.pcbid), "PCB id hook");

    /* The SEGA drives: E:\ (AMFS) and Y:\ (APPDATA), plus the registry values
       that point ALL.Net at them. Its billing keeps its play counter and trace
       log on APPDATA; without this it fails with ERROR 0800. Goes on top of
       DivaLinux's file layer, and only rewrites E:, Y: and the AppUser home. */
    DIVAONLINE_STEP(vfs_hook_init(&divaonline_platform.vfs), "VFS hook");

    /* amex_hook_init() minus JVS: PD Loader's TLAC feeds the game's input */

    DIVAONLINE_STEP(ds_hook_init(&divaonline_amex.ds), "DS hook");
    DIVAONLINE_STEP(eeprom_hook_init(&divaonline_amex.eeprom), "EEPROM hook");
    DIVAONLINE_STEP(gpio_hook_init(&divaonline_amex.gpio), "GPIO hook");
    DIVAONLINE_STEP(sram_hook_init(&divaonline_amex.sram), "SRAM hook");

    /* The Aime reader */

    DIVAONLINE_STEP(sg_reader_hook_init(&divaonline_aime, DIVA_AIME_PORT, divaonline_mod), "Aime reader hook");

    fixups.undo_quick_start =
            GetPrivateProfileIntW(L"divaonline", L"undo_quick_start", 1, divaonline_ini) != 0;
    fixups.undo_network_patches =
            GetPrivateProfileIntW(L"divaonline", L"undo_network_patches", 0, divaonline_ini) != 0;
    fixups.undo_keychip_patches =
            GetPrivateProfileIntW(L"divaonline", L"undo_keychip_patches", 0, divaonline_ini) != 0;
    fixups.allnet_log =
            GetPrivateProfileIntW(L"divaonline", L"allnet_log", 1, divaonline_ini) != 0;
    {
        wchar_t mask[32];

        GetPrivateProfileStringW(L"divaonline", L"game_log", L"0x10000", mask, _countof(mask), divaonline_ini);
        fixups.game_log_mask = (unsigned int) wcstoul(mask, NULL, 0);
    }
    fixups.ignore_tlac_player_data =
            GetPrivateProfileIntW(L"divaonline", L"keep_tlac_player_data", 0, divaonline_ini) == 0;
    fixups_init(&fixups);

    /* ALL.Net billing asks the keychip for these two files (segatools serves
       them from disk). */

    if (divaonline_platform.nusec.enable) {
        const wchar_t *files[2] = {
            divaonline_platform.nusec.billing_ca,
            divaonline_platform.nusec.billing_pub,
        };
        wchar_t full[MAX_PATH];
        size_t i;

        for (i = 0 ; i < 2 ; i++) {
            if (files[i][0] != L'\0' && files[i][1] != L':' && files[i][0] != L'\\') {
                _snwprintf(full, _countof(full), L"%s\\%s", divaonline_game_dir, files[i]);
            } else {
                _snwprintf(full, _countof(full), L"%s", files[i]);
            }

            full[_countof(full) - 1] = L'\0';

            if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
                dprintf("WARNING: %S is missing. ALL.Net billing reads it;"
                        " it comes with segatools / your server's setup files\n", full);
            }
        }
    }

    if (divaonline_platform.dns.enable) {
        dprintf("ALL.Net: naominet.jp -> %S, aime.naominet.jp -> %S, ib.naominet.jp -> %S\n",
                divaonline_platform.dns.startup,
                divaonline_platform.dns.aimedb,
                divaonline_platform.dns.billing);
    }

    dprintf("---  End  DivaOnline init%s ---\n", ok ? "" : " (with errors, see above)");
}

__declspec(dllexport) void InitializeDVA(void)
{
    if (divaonline_started) {
        return;
    }

    divaonline_started = true;
    divaonline_start();
}

__declspec(dllexport) LPCWSTR GetPluginName(void)
{
    return L"DivaOnline";
}

__declspec(dllexport) LPCWSTR GetPluginDescription(void)
{
    return L"DivaOnline Plugin by MukiEX\n\n"
           L"Aime card and ALL.Net support for ARTEMiS servers, from segatools' divahook.";
}

__declspec(dllexport) LPCWSTR GetBuildDate(void)
{
    return L"" __DATE__;
}

BOOL WINAPI DllMain(HMODULE mod, DWORD cause, void *ctx)
{
    (void) ctx;

    if (cause == DLL_PROCESS_ATTACH) {
        divaonline_mod = mod;
        DisableThreadLibraryCalls(mod);
    }

    return TRUE;
}
