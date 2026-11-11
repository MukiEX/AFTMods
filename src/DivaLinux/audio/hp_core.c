/* See hp_core.h. */
#include "hp_core.h"
#include "lakehp.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A filter blob's header, validated as LakeHP's tools do, so scanning a DLL image doesn't
 * pick up false positives. */
static int blob_ok(const uint8_t *d, size_t avail, lhp_filter_info *h) {
    if (avail < 0x40 || (d[0] != 1 && d[0] != 2))
        return 0;
    const uint32_t total = rd32(d + 1);
    const unsigned hlen = d[5] | d[6] << 8;
    if (total < 0x40 || total > 0x40000 || total > avail)
        return 0;
    if (hlen < 0x30 || hlen > 0x200 || hlen >= total)
        return 0;
    if (lhp_parse_header(d, total, h))
        return 0;
    if (h->room < 0 || h->room > 3 || h->max_bits < 1 || h->max_bits > 32 || h->scale_exp < 1 || h->scale_exp > 31)
        return 0;
    return 1;
}

/* Calls fn for every filter in the image; stops when it returns nonzero. */
static int each_filter(const uint8_t *d, size_t len,
                       int (*fn)(void *ctx, const uint8_t *blob, const lhp_filter_info *h), void *ctx) {
    lhp_filter_info h;
    if (len >= 20 && !memcmp(d, "LHPF", 4)) {   /* a LakeHP filter region */
        const uint32_t count = rd32(d + 8), total = rd32(d + 12);
        if (rd32(d + 4) != 1 || total > len || 20 + 16ull * count > total)
            return 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *t = d + 20 + 16 * i;
            const uint32_t off = rd32(t + 8), n = rd32(t + 12);
            if (off > total || n > total - off)
                return 0;
            if (blob_ok(d + off, n, &h) && fn(ctx, d + off, &h))
                return 1;
        }
        return 0;
    }
    if (len >= 2 && d[0] == 'M' && d[1] == 'Z') {   /* a DLL: scan its image */
        for (size_t off = 0; off + 0x60 < len;) {
            if (blob_ok(d + off, len - off, &h)) {
                if (fn(ctx, d + off, &h))
                    return 1;
                off += h.total_len;
            } else
                off++;
        }
        return 0;
    }
    return blob_ok(d, len, &h) && fn(ctx, d, &h);   /* one raw filter */
}

typedef struct { int room; uint32_t rate; const uint8_t *blob; size_t len; } find_ctx;

static int find_fn(void *p, const uint8_t *blob, const lhp_filter_info *h) {
    find_ctx *c = (find_ctx *)p;
    if (h->room != c->room)
        return 0;
    if (h->sample_rate == c->rate) {   /* exact rate wins */
        c->blob = blob;
        c->len = h->total_len;
        return 1;
    }
    if (h->sample_rate == 0xFFFFFFFFu && !c->blob) {   /* any rate: keep looking for an exact one */
        c->blob = blob;
        c->len = h->total_len;
    }
    return 0;
}

int hp_find_filter(const uint8_t *data, size_t len, int room, uint32_t rate,
                   const uint8_t **blob, size_t *blob_len) {
    find_ctx c = { room, rate, NULL, 0 };
    each_filter(data, len, find_fn, &c);
    if (!c.blob)
        return -1;
    *blob = c.blob;
    *blob_len = c.len;
    return 0;
}

typedef struct { int room; uint32_t *rates; int n, max; } rates_ctx;

static int rates_fn(void *p, const uint8_t *blob, const lhp_filter_info *h) {
    rates_ctx *c = (rates_ctx *)p;
    (void)blob;
    if (h->room == c->room && c->n < c->max)
        c->rates[c->n++] = h->sample_rate;
    return 0;
}

int hp_filter_rates(const uint8_t *data, size_t len, int room, uint32_t *rates, int max) {
    rates_ctx c = { room, rates, 0, max };
    each_filter(data, len, rates_fn, &c);
    return c.n;
}

/* ---- processing ---------------------------------------------------------------------- */

struct hp_state {
    lhp_engine engine;   /* ~330 KB */
    int16_t in[LHP_BLOCK * 2];
    int16_t block[LHP_BLOCK * 2];
    int16_t *out;        /* rendered stereo waiting to go out */
    int out_n, out_cap;  /* frames */
};

hp_state *hp_create(void) {
    hp_state *s = (hp_state *)calloc(1, sizeof(hp_state));
    if (s)
        lhp_init(&s->engine);
    return s;
}

void hp_destroy(hp_state *s) {
    if (!s)
        return;
    free(s->out);
    free(s);
}

static int out_reserve(hp_state *s, int frames) {
    if (frames <= s->out_cap)
        return 1;
    int cap = s->out_cap ? s->out_cap : 4096;
    while (cap < frames)
        cap *= 2;
    int16_t *p = (int16_t *)realloc(s->out, (size_t)cap * 2 * sizeof(int16_t));
    if (!p)
        return 0;
    s->out = p;
    s->out_cap = cap;
    return 1;
}

int hp_load(hp_state *s, const uint8_t *blob, size_t len) {
    lhp_init(&s->engine);
    /* stereo in: the two front speakers, nothing else (positions L, C, R, Ls, Rs, LFE) */
    const int8_t map[LHP_NPOS] = { 0, -1, 1, -1, -1, -1 };
    if (lhp_set_map(&s->engine, map, 2) || lhp_set_map_back(&s->engine, -1, -1))
        return -1;
    const int r = lhp_load_filter(&s->engine, blob, len, 1.0f);
    if (r < 0)
        return r;
    lhp_reset(&s->engine);
    /* one block of silence ahead, so every call can hand back as many frames as it got */
    if (!out_reserve(s, LHP_BLOCK * 2))
        return -1;
    memset(s->out, 0, (size_t)LHP_BLOCK * 2 * sizeof(int16_t));
    s->out_n = LHP_BLOCK;
    return 0;
}

void hp_process(hp_state *s, int16_t *buf, int frames, int channels) {
    if (!s || !buf || frames <= 0 || channels < 2)
        return;
    if (!out_reserve(s, s->out_n + frames + LHP_BLOCK))
        return;
    /* in: the first pair, through the engine block by block */
    for (int done = 0; done < frames;) {
        int n = LHP_BLOCK - s->engine.fill;
        if (n > frames - done)
            n = frames - done;
        if (n > LHP_BLOCK)
            n = LHP_BLOCK;
        for (int i = 0; i < n; i++) {
            s->in[2 * i] = buf[(size_t)(done + i) * channels];
            s->in[2 * i + 1] = buf[(size_t)(done + i) * channels + 1];
        }
        if (lhp_push_s16(&s->engine, s->in, n, s->block) == LHP_BLOCK) {
            memcpy(s->out + (size_t)s->out_n * 2, s->block, sizeof(s->block));
            s->out_n += LHP_BLOCK;
        }
        done += n;
    }
    /* out: the oldest rendered frames, to every pair */
    const int n = frames < s->out_n ? frames : s->out_n;   /* always frames (see hp_load) */
    for (int i = 0; i < n; i++)
        for (int c = 0; c + 1 < channels; c += 2) {
            buf[(size_t)i * channels + c] = s->out[2 * i];
            buf[(size_t)i * channels + c + 1] = s->out[2 * i + 1];
        }
    s->out_n -= n;
    memmove(s->out, s->out + (size_t)n * 2, (size_t)s->out_n * 2 * sizeof(int16_t));
}
