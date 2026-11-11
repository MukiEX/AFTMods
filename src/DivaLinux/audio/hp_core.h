/* Headphone surround for the game's sound: the portable part (no Windows), so it can be
 * tested on its own (tests/hp_core_test.c). headphones.cpp hooks the game's mixer and calls it.
 *
 * The rendering is LakeHP's engine (lakehp.c/.h, from github.com/MukiEX/LakeHeadphones,
 * engine/ at e4a443b): the game's stereo goes to two virtual speakers in a small, medium or
 * large room. The room filters come from a filter DLL in plugins: the original headphone
 * engine's DLL, or LakeHP's open stand-in, LakeHP_OpenRooms.dll. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The room filter for `room` (1 small, 2 medium, 3 large) at `rate` Hz in a filter DLL's
 * image (or a LakeHP filter region, or a single raw filter). 0 and the blob, or -1. */
int hp_find_filter(const uint8_t *data, size_t len, int room, uint32_t rate,
                   const uint8_t **blob, size_t *blob_len);
/* The sample rates the image has filters for room `room`, up to `max` of them. */
int hp_filter_rates(const uint8_t *data, size_t len, int room, uint32_t *rates, int max);

typedef struct hp_state hp_state;
hp_state *hp_create(void);
void hp_destroy(hp_state *s);
/* Loads a room filter. 0, or <0 when the engine rejects it. Clears the sound in flight. */
int hp_load(hp_state *s, const uint8_t *blob, size_t len);
/* In place: the first two of `channels` interleaved 16-bit channels are rendered for
 * headphones and written to every pair of channels (the game's 4-channel output carries
 * the same stereo twice, for speakers and headphones). Adds 1024 frames of latency. */
void hp_process(hp_state *s, int16_t *buf, int frames, int channels);

#ifdef __cplusplus
}
#endif
