#pragma once

#include <windows.h>

#include <stdbool.h>

/* plugins\DivaOnline.ini, as an absolute path */
const wchar_t *divaonline_ini_path(void);

void divaonline_log_init(const wchar_t *log_path);

/* "0x2D", "45", "Insert", "F3", "C" -> virtual-key code */
int divaonline_parse_key(const wchar_t *s, int fallback);

struct fixups_config {
    bool undo_quick_start;
    bool undo_network_patches;
};

/* Puts back game code PD Loader's Patches plugin changes that would keep the
   card screen or the network checks from running. Applied once all plugins
   have loaded (when the game calls glutInit). */
void fixups_init(const struct fixups_config *cfg);
