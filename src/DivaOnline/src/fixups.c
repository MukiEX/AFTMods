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

   - Two always-on "skip unnecessary checks" patches: one makes the game
     believe the keychip was found before the keychip driver has answered,
     the other stubs out a check next to it. Undone with
     [divaonline] undo_keychip_patches=1.

   Patches.dva loads after DivaOnline.dva (and writes its patches in its
   DllMain), so these run at the first file the game opens once Patches.dva is
   loaded, or when the game calls glutInit, whichever comes first: by then
   every plugin has loaded, and the game hasn't reached its startup checks or
   the card screen yet. Each site is only written when it
   holds exactly what PD Loader writes there, so another game version or
   another patch is never overwritten. */

#include <windows.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "hook/iohook.h"
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

static const struct fixup_site keychip_sites[] = {
    {
        0x14066E820, "keychip found (forced true)", 6,
        { 0x0F, 0xB6, 0x05, 0xE3, 0x65, 0x62 },
        { { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 } },
        1,
    }, {
        0x140210820, "skip check (forced 0)", 6,
        { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 },
        { { 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3 } },
        1,
    },
};

static bool fixups_readable(uintptr_t addr, size_t len);

/* diva.exe 7.10's ALL.Net library (alw/alpb/alAbEx) logs through one function
   that is silent unless its level and mask are raised. Its lines go to a
   callback when one is set, else to stderr. */
#define ALLNET_LOG_LEVEL    ((volatile int32_t *) 0x14CE12320)
#define ALLNET_LOG_MASK     ((volatile uint32_t *) 0x14CE12330)
#define ALLNET_LOG_CALLBACK ((void (* volatile *)(const char *)) 0x14CE12338)

static void allnet_log_line(const char *line)
{
    size_t n = strlen(line);

    dprintf("[ALL.Net] %s%s", line, (n > 0 && line[n - 1] == '\n') ? "" : "\n");
}

static void allnet_log_enable(void)
{
    /* Only if the code that uses these globals is diva.exe 7.10's: the log
       function reads them at these addresses */
    static const uint8_t level_getter[] = { 0x8B, 0x05, 0xAA, 0x10, 0xE1, 0x0C, 0xC3 };

    if (!fixups_readable(0x140001270, sizeof(level_getter))
            || memcmp((void *) 0x140001270, level_getter, sizeof(level_getter)) != 0) {
        return;
    }

    *ALLNET_LOG_LEVEL = 4;
    *ALLNET_LOG_MASK = 0xFFFFFFFF;
    *ALLNET_LOG_CALLBACK = allnet_log_line;
}

/* The game's own debug print, void dbg(int category, const char *fmt, ...) at
   0x14017B730, forwards to a va_list version at 0x14017B760 that keeps the
   text in a ring buffer nobody reads. Category 0x10 is the Aime reader task
   ("TaskAime::start", "->TaskAime::open(...)"). The varargs entry starts with
   three register spills, so it can be replaced by a jump to our own version,
   which logs and then calls the va_list one itself. */
#define GAME_DBG_ENTRY  0x14017B730
#define GAME_DBG_VPRINT 0x14017B760

static void (*game_dbg_vprint)(int category, const char *fmt, va_list ap) =
        (void (*)(int, const char *, va_list)) GAME_DBG_VPRINT;
static unsigned int game_dbg_mask;

static void game_dbg_hook(int category, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (category >= 0 && category < 32 && (game_dbg_mask & (1u << (category & 31))) != 0) {
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        buf[sizeof(buf) - 1] = '\0';
        dprintf("[Game %02x] %s%s", category, buf,
                (buf[0] != '\0' && buf[strlen(buf) - 1] == '\n') ? "" : "\n");
    }

    va_start(ap, fmt);
    game_dbg_vprint(category, fmt, ap);
    va_end(ap);
}

static void game_dbg_install(unsigned int mask)
{
    static const uint8_t entry[] = {
        0x48, 0x89, 0x54, 0x24, 0x10,   /* mov [rsp+10h], rdx */
        0x4C, 0x89, 0x44, 0x24, 0x18,   /* mov [rsp+18h], r8 */
        0x4C, 0x89, 0x4C, 0x24, 0x20,   /* mov [rsp+20h], r9 */
    };
    static const uint8_t vprint[] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00 };
    uint8_t jump[12] = { 0x48, 0xB8 };  /* mov rax, imm64 ; jmp rax */
    uintptr_t target = (uintptr_t) game_dbg_hook;
    DWORD old;

    game_dbg_mask = mask;

    if (mask == 0
            || !fixups_readable(GAME_DBG_ENTRY, sizeof(entry))
            || memcmp((void *) GAME_DBG_ENTRY, entry, sizeof(entry)) != 0
            || memcmp((void *) GAME_DBG_VPRINT, vprint, sizeof(vprint)) != 0) {
        return;
    }

    memcpy(jump + 2, &target, sizeof(target));
    jump[10] = 0xFF;
    jump[11] = 0xE0;

    if (VirtualProtect((void *) GAME_DBG_ENTRY, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void *) GAME_DBG_ENTRY, jump, sizeof(jump));
        VirtualProtect((void *) GAME_DBG_ENTRY, sizeof(jump), old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void *) GAME_DBG_ENTRY, sizeof(jump));
        dprintf("Fixups: game debug log categories %08x copied here\n", mask);
    }
}

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

static void fixups_report(const struct fixup_site *sites, size_t n);

static void fixups_apply(void)
{
    if (fixups_done) {
        return;
    }

    fixups_done = true;

    if (fixups_cfg.allnet_log) {
        allnet_log_enable();
    }

    if (fixups_cfg.ignore_tlac_player_data) {
        tlac_ignore_player_files(divaonline_plugins_dir(), divaonline_game_dir_path());
    }

    dprintf("Fixups: game code as the game starts:\n");
    fixups_report(quick_start_sites, _countof(quick_start_sites));
    fixups_report(network_sites, _countof(network_sites));
    fixups_report(keychip_sites, _countof(keychip_sites));

    if (fixups_cfg.undo_quick_start) {
        fixups_restore(quick_start_sites, _countof(quick_start_sites));
    }

    if (fixups_cfg.undo_network_patches) {
        fixups_restore(network_sites, _countof(network_sites));
    }

    if (fixups_cfg.undo_keychip_patches) {
        fixups_restore(keychip_sites, _countof(keychip_sites));
    }
}

/* Logs which of PD Loader's patches are in place, whatever is undone */
static void fixups_report(const struct fixup_site *sites, size_t n)
{
    size_t i;
    size_t j;

    for (i = 0 ; i < n ; i++) {
        const struct fixup_site *s = &sites[i];
        const char *state = "unexpected bytes";

        if (!fixups_readable(s->addr, s->len)) {
            state = "not mapped";
        } else if (memcmp((void *) s->addr, s->original, s->len) == 0) {
            state = "original";
        } else {
            for (j = 0 ; j < s->npatched ; j++) {
                if (memcmp((void *) s->addr, s->patched[j], s->len) == 0) {
                    state = "PD Loader patch";
                }
            }
        }

        dprintf("Fixups: %-32s %s\n", s->what, state);
    }
}

static unsigned int fixups_opens_seen;

static HRESULT fixups_handle_irp(struct irp *irp)
{
    if (irp->op == IRP_OP_OPEN && irp->open_filename != NULL
            && (wcsstr(irp->open_filename, L"COM") != NULL || wcsstr(irp->open_filename, L"com") != NULL)) {
        dprintf("Serial: game opened %S (not emulated here, passed on)\n", irp->open_filename);
    }

    if (!fixups_done && irp->op == IRP_OP_OPEN && fixups_opens_seen < 100000) {
        fixups_opens_seen++;

        if (GetModuleHandleW(L"Patches.dva") != NULL) {
            fixups_apply();
        }
    }

    return iohook_invoke_next(irp);
}

static void hook_glutInitWithExit(int *argcp, char **argv, void (*exitfunc)(int))
{
    fixups_apply();
    next_glutInitWithExit(argcp, argv, exitfunc);
}

void fixups_init(const struct fixups_config *cfg)
{
    fixups_cfg = *cfg;

    if (cfg->allnet_log) {
        allnet_log_enable();
    }

    game_dbg_install(cfg->game_log_mask);

    iohook_push_handler(fixups_handle_irp);

    hook_table_apply(GetModuleHandleW(NULL), "glut32.dll", fixups_glut_syms, _countof(fixups_glut_syms));

    if (next_glutInitWithExit == NULL) {
        /* No glut import to ride on (shouldn't happen with diva.exe 7.10):
           patch now, which catches everything loaded before us. */
        dprintf("Fixups: diva.exe doesn't import __glutInitWithExit, applying now\n");
        fixups_apply();
    }
}
