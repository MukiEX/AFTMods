#pragma once

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DivaOnline: when true (the default), a NULL target hooks diva.exe only. */
extern bool hook_table_exe_only;

struct hook_symbol {
    const char *name;
    uint16_t ordinal;
    void *patch;
    void **link;
};

void hook_table_apply(
        HMODULE target,
        const char *depname,
        const struct hook_symbol *syms,
        size_t nsyms);

void hook_table_revert(
        HMODULE target,
        const char *depname,
        const struct hook_symbol *syms,
        size_t nsyms);
