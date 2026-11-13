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

/* devicezip.c: DEVICE\ and billing\ from plugins\DivaOnline.zip, and [divaonline]
   reset_device. Called at startup, before the hooks open any of those files. */
void divaonline_device_files(const wchar_t *ini, const wchar_t *plugins_dir, const wchar_t *game_dir);
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

/* reader.c: a PC/SC card reader ([aime] reader=) */
bool divaonline_reader_start(const wchar_t *ini);
int divaonline_reader_card(uint64_t *idm, uint8_t code[10]);
