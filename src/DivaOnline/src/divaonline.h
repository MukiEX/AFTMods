#pragma once

#include <windows.h>

#include <stdbool.h>

/* plugins\DivaOnline.ini, as an absolute path */
const wchar_t *divaonline_ini_path(void);

void divaonline_log_init(const wchar_t *log_path);

/* While DivaOnline is on: TLAC gets components.ini with player_data_manager
   and score_saver off (src/tlac.c). Called once every plugin has loaded. */
void tlac_ignore_player_files(const wchar_t *plugins_dir, const wchar_t *game_dir);

const wchar_t *divaonline_plugins_dir(void);
const wchar_t *divaonline_game_dir_path(void);

/* "0x2D", "45", "Insert", "F3", "C" -> virtual-key code */
int divaonline_parse_key(const wchar_t *s, int fallback);

struct fixups_config {
    bool undo_quick_start;
    bool undo_network_patches;
    bool undo_keychip_patches;
    bool allnet_log;
    unsigned int game_log_mask;
    bool ignore_tlac_player_data;
};

/* Puts back game code PD Loader's Patches plugin changes that would keep the
   card screen or the network checks from running. Applied once all plugins
   have loaded (when the game calls glutInit). */
void fixups_init(const struct fixups_config *cfg);
