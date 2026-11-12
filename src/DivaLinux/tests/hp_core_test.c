/* hp_core on its own: finds the rooms in a filter DLL and renders a test tone through each.
 *   cc -O2 -I../audio hp_core_test.c ../audio/hp_core.c ../audio/lakehp.c -lm -o hp_core_test
 *   ./hp_core_test plugins/LakeHP_OpenRooms.dll [out.wav]    (out.wav: the medium room)
 * The test signal is noise on the left channel for a second: the left ear should hear more. */
#include "hp_core.h"
#include "lakehp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static void wav(const char *path, const int16_t *s, int frames, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned data = (unsigned)frames * 4, v;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); unsigned short a[2] = { 1, 2 }; fwrite(a, 2, 2, f);
    v = rate; fwrite(&v, 4, 1, f); v = rate * 4; fwrite(&v, 4, 1, f);
    unsigned short b[2] = { 4, 16 }; fwrite(b, 2, 2, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s, 4, frames, f); fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s filters.dll [out.wav]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(len); fread(d, 1, len, f); fclose(f);
    const int rate = 44100, ch = 4, total = rate * 3;
    for (int room = 1; room <= 3; room++) {
        uint32_t rates[16]; int nr = hp_filter_rates(d, len, room, rates, 16);
        printf("room %d: %d filter(s):", room, nr);
        for (int i = 0; i < nr; i++) printf(" %u", rates[i]);
        printf("\n");
        const uint8_t *blob; size_t blen;
        CHECK(hp_find_filter(d, len, room, rate, &blob, &blen) == 0);
        hp_state *s = hp_create();
        CHECK(s && hp_load(s, blob, blen) == 0);
        /* noise on the left for 1 s, then silence; odd call sizes, 4 channels */
        uint32_t seed = 12345;
        int16_t *all = calloc((size_t)total * 2, sizeof(int16_t));
        int16_t buf[700 * 4];
        double el = 0, er = 0;
        int pos = 0, step = 0;
        static const int sizes[] = { 512, 441, 700, 1, 333 };
        while (pos < total) {
            int n = sizes[step++ % 5];
            if (n > total - pos) n = total - pos;
            for (int i = 0; i < n; i++) {
                seed = seed * 1664525u + 1013904223u;
                int16_t v = (pos + i) < rate ? (int16_t)((int32_t)(seed >> 16) % 8000) : 0;
                buf[i * ch + 0] = v; buf[i * ch + 1] = 0; buf[i * ch + 2] = v; buf[i * ch + 3] = 0;
            }
            hp_process(s, buf, n, ch);
            for (int i = 0; i < n; i++) {
                CHECK(buf[i * ch + 0] == buf[i * ch + 2] && buf[i * ch + 1] == buf[i * ch + 3]);
                all[(pos + i) * 2] = buf[i * ch]; all[(pos + i) * 2 + 1] = buf[i * ch + 1];
                el += (double)buf[i * ch] * buf[i * ch]; er += (double)buf[i * ch + 1] * buf[i * ch + 1];
            }
            pos += n;
        }
        /* the first hp_latency() frames are the delay: silent */
        int silent = 1;
        for (int i = 0; i < hp_latency() * 2; i++) if (all[i]) silent = 0;
        CHECK(silent);
        CHECK(hp_latency() <= 128);
        {   /* the same as the Lake engine itself (1024 frames later): the difference well below the sound */
            static lhp_engine e;
            lhp_init(&e);
            const int8_t map[LHP_NPOS] = { 0, -1, 1, -1, -1, -1 };
            lhp_set_map(&e, map, 2); lhp_set_map_back(&e, -1, -1);
            CHECK(lhp_load_filter(&e, blob, blen, 1.0f) == 0);
            lhp_reset(&e);
            int16_t *ref = calloc((size_t)total * 2, sizeof(int16_t));
            int16_t in[LHP_BLOCK * 2], out[LHP_BLOCK * 2];
            seed = 12345;
            for (int p = 0; p + LHP_BLOCK <= total; p += LHP_BLOCK) {
                for (int i = 0; i < LHP_BLOCK; i++) {
                    int n2 = p + i; (void)n2;
                    in[2 * i] = 0; in[2 * i + 1] = 0;
                }
                /* the same noise as above: regenerate it sample by sample */
                for (int i = 0; i < LHP_BLOCK; i++) {
                    seed = seed * 1664525u + 1013904223u;
                    in[2 * i] = (p + i) < rate ? (int16_t)((int32_t)(seed >> 16) % 8000) : 0;
                }
                lhp_push_s16(&e, in, LHP_BLOCK, out);
                memcpy(ref + (size_t)p * 2, out, sizeof(out));
            }
            double err = 0, sig = 0;
            const int lat = hp_latency();
            for (int i = 0; i + lat < total - LHP_BLOCK; i++)
                for (int c = 0; c < 2; c++) {
                    double x = ref[2 * i + c], y = all[2 * (i + lat) + c];
                    err += (x - y) * (x - y); sig += x * x;
                }
            double db = 10 * log10(err / sig + 1e-30);
            printf("  difference from the Lake engine: %.1f dB\n", db);
            CHECK(db < -45);
            free(ref);
        }
        printf("  left %.1f dB, right %.1f dB (both ears hear the left speaker)\n",
               10 * log10(el / total + 1e-9), 10 * log10(er / total + 1e-9));
        CHECK(el > 0 && er > 0 && el > er);
        if (room == 2 && argc > 2) wav(argv[2], all, total, rate);
        free(all);
        hp_destroy(s);
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
