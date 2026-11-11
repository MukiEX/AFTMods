/* PD Loader's Patches plugin changes some of diva.exe 7.10's code for offline
   play. Two kinds matter here:

   - "quick_start" ([patches] in config.ini) jumps past the card/guest screen,
     so a card can never be tapped. PD Loader keeps it off only when launching
     through divahook.bat (use_divahook_bat). DivaOnline puts the original
     jump back.
   - Three always-on patches shorten the network check at startup (skip part
     of the check, no DHCP wait, ignore the location server result). Players
     have gone online with segatools' divahook.dll alongside them, so they're
     left alone unless [divaonline] undo_network_patches=1.

   Patches.dva loads after DivaOnline.dva, so these run when the game calls
   glutInit: by then every plugin has loaded, and the game hasn't reached its
   startup checks or the card screen yet. Each site is only written when it
   holds exactly what PD Loader writes there, so another game version or
   another patch is never overwritten. */

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hook/table.h"

#include "util/dprintf.h"

#include "divaonline.h"

struct fixup_site {
    uintptr_t addr;
    const char *what;
    uint8_t len;
    uint8_t original[8];
    uint8_t patched[3][8];  /* what PD Loader may have written, unused rows zero-length */
    uint8_t npatched;
};

static const struct fixup_site quick_start_sites[] = {
    {
        0x140578EA9, "quick start (skip card screen)", 5,
        { 0xE8, 0x92, 0xC5, 0xFE, 0xFF },
        { { 0xE9, 0x8E, 0x00, 0x00, 0x00 },     /* quick_start=1 */
          { 0xE9, 0xF1, 0x00, 0x00, 0x00 } },   /* quick_start=2 */
        2,
    }, {
        0x140578E9D, "quick start (back button state)", 7,
        { 0xC7, 0x47, 0x68, 0x0A, 0x00, 0x00, 0x00 },
        { { 0xC7, 0x47, 0x68, 0x28, 0x00, 0x00, 0x00 } },
        1,
    },
};

static const struct fixup_site network_sites[] = {
    {
        0x1406717B1, "skip parts of the network check", 4,
        { 0x0F, 0x85, 0x21, 0x03 },
        { { 0xE9, 0x22, 0x03, 0x00 } },
        1,
    }, {
        0x1406724E7, "DHCP wait timer", 2,
        { 0xA0, 0x8C },
        { { 0x00, 0x00 } },
        1,
    }, {
        0x1406732A2, "ignore location server checks", 2,
        { 0x75, 0x1E },
        { { 0x90, 0x90 } },
        1,
    },
};

static struct fixups_config fixups_cfg;
static bool fixups_done;

static void (*next_glutInitWithExit)(int *argcp, char **argv, void (*exitfunc)(int));

static void hook_glutInitWithExit(int *argcp, char **argv, void (*exitfunc)(int));

static const struct hook_symbol fixups_glut_syms[] = {
    {
        .name   = "__glutInitWithExit",
        .patch  = hook_glutInitWithExit,
        .link   = (void **) &next_glutInitWithExit,
    },
};

static bool fixups_readable(uintptr_t addr, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (VirtualQuery((void *) addr, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) {
        return false;
    }

    return addr + len <= (uintptr_t) mbi.BaseAddress + mbi.RegionSize;
}

static void fixups_restore(const struct fixup_site *sites, size_t n)
{
    const struct fixup_site *s;
    DWORD old;
    size_t i;
    size_t j;

    for (i = 0 ; i < n ; i++) {
        s = &sites[i];

        if (!fixups_readable(s->addr, s->len)) {
            dprintf("Fixups: %s: address not mapped, skipped (not diva.exe 7.10?)\n", s->what);
            continue;
        }

        if (memcmp((void *) s->addr, s->original, s->len) == 0) {
            continue;   /* not patched */
        }

        for (j = 0 ; j < s->npatched ; j++) {
            if (memcmp((void *) s->addr, s->patched[j], s->len) == 0) {
                break;
            }
        }

        if (j == s->npatched) {
            dprintf("Fixups: %s: unexpected bytes, left alone\n", s->what);
            continue;
        }

        if (!VirtualProtect((void *) s->addr, s->len, PAGE_EXECUTE_READWRITE, &old)) {
            dprintf("Fixups: %s: VirtualProtect failed (%lu)\n", s->what, GetLastError());
            continue;
        }

        memcpy((void *) s->addr, s->original, s->len);
        VirtualProtect((void *) s->addr, s->len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void *) s->addr, s->len);

        dprintf("Fixups: undid PD Loader's %s patch\n", s->what);
    }
}

static void fixups_apply(void)
{
    if (fixups_done) {
        return;
    }

    fixups_done = true;

    if (fixups_cfg.undo_quick_start) {
        fixups_restore(quick_start_sites, _countof(quick_start_sites));
    }

    if (fixups_cfg.undo_network_patches) {
        fixups_restore(network_sites, _countof(network_sites));
    }
}

static void hook_glutInitWithExit(int *argcp, char **argv, void (*exitfunc)(int))
{
    fixups_apply();
    next_glutInitWithExit(argcp, argv, exitfunc);
}

void fixups_init(const struct fixups_config *cfg)
{
    fixups_cfg = *cfg;

    if (!cfg->undo_quick_start && !cfg->undo_network_patches) {
        return;
    }

    hook_table_apply(GetModuleHandleW(NULL), "glut32.dll", fixups_glut_syms, _countof(fixups_glut_syms));

    if (next_glutInitWithExit == NULL) {
        /* No glut import to ride on (shouldn't happen with diva.exe 7.10):
           patch now, which catches everything loaded before us. */
        dprintf("Fixups: diva.exe doesn't import __glutInitWithExit, applying now\n");
        fixups_apply();
    }
}
