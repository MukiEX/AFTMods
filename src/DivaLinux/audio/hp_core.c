/* See hp_core.h. */
#include "hp_core.h"
#include "lakehp.h"
#include <math.h>
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

/* ---- processing ----------------------------------------------------------------------
 *
 * The Lake engine works in 1024-frame blocks: a block's output needs all of its input, so it
 * adds 1024 frames (23 ms at 44.1 kHz) of delay whatever the CPU. It is linear and, to about
 * -55 dB, time-invariant: its effect on each input channel is an impulse response. hp_load
 * runs an impulse through it from each input channel (left, right), keeps the four responses
 * (each channel to each ear), and hp_process convolves with them in HP_BLOCK-frame pieces:
 * a uniformly partitioned overlap-save convolution with a HP_FFT-point FFT. Same sound
 * (the difference is below -55 dB), HP_BLOCK frames of delay. */

#define HP_BLOCK 128                 /* frames per piece: the delay */
#define HP_FFT (2 * HP_BLOCK)
#define HP_BINS (HP_BLOCK + 1)       /* real signal: bins 0..HP_BLOCK */
#define HP_IR_MAX 16384              /* longest response kept (the rooms' are ~8192) */
#define HP_PARTS_MAX (HP_IR_MAX / HP_BLOCK)

typedef struct { float re, im; } hp_cpx;

struct hp_state {
    int parts;                                   /* partitions in use */
    hp_cpx *h;                                   /* [parts][4][HP_BINS]: LL, LR (left in -> ears), RL, RR */
    hp_cpx *fdl;                                 /* [parts][2][HP_BINS]: input spectra, newest first */
    int head;                                    /* index of the newest partition in fdl */
    float in_prev[2][HP_BLOCK];                  /* overlap-save: the previous piece */
    float in_cur[2][HP_BLOCK];
    int fill;                                    /* frames gathered in in_cur */
    hp_cpx tw[HP_FFT / 2];                       /* FFT twiddles */
    uint16_t rev[HP_FFT];
    int16_t *out;                                /* rendered stereo waiting to go out */
    int out_n, out_cap;                          /* frames */
};

int hp_latency(void) { return HP_BLOCK; }

static void fft_init(hp_state *s) {
    for (int k = 0; k < HP_FFT / 2; k++) {
        double a = -2.0 * 3.14159265358979323846 * k / HP_FFT;
        s->tw[k].re = (float)cos(a); s->tw[k].im = (float)sin(a);
    }
    int bits = 0;
    while ((1 << bits) < HP_FFT) bits++;
    for (int i = 0; i < HP_FFT; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i >> b & 1) r |= 1 << (bits - 1 - b);
        s->rev[i] = (uint16_t)r;
    }
}

/* in place, forward (inverse: conjugate before and after, scale by 1/N) */
static void fft(const hp_state *s, hp_cpx *x) {
    for (int i = 0; i < HP_FFT; i++)
        if (s->rev[i] > i) { hp_cpx t = x[i]; x[i] = x[s->rev[i]]; x[s->rev[i]] = t; }
    for (int len = 2; len <= HP_FFT; len <<= 1) {
        const int half = len >> 1, step = HP_FFT / len;
        for (int i = 0; i < HP_FFT; i += len)
            for (int j = 0; j < half; j++) {
                const hp_cpx w = s->tw[j * step], b = x[i + j + half];
                const float tr = b.re * w.re - b.im * w.im, ti = b.re * w.im + b.im * w.re;
                x[i + j + half].re = x[i + j].re - tr; x[i + j + half].im = x[i + j].im - ti;
                x[i + j].re += tr; x[i + j].im += ti;
            }
    }
}

/* two real signals at once: FFT(a + jb), then split into the spectra of a and b (bins 0..N/2) */
static void fft_two_real(const hp_state *s, const float *a, const float *b, hp_cpx *A, hp_cpx *B) {
    hp_cpx x[HP_FFT];
    for (int i = 0; i < HP_FFT; i++) { x[i].re = a[i]; x[i].im = b[i]; }
    fft(s, x);
    for (int k = 0; k <= HP_FFT / 2; k++) {
        const hp_cpx p = x[k], q = x[(HP_FFT - k) & (HP_FFT - 1)];
        A[k].re = 0.5f * (p.re + q.re); A[k].im = 0.5f * (p.im - q.im);
        B[k].re = 0.5f * (p.im + q.im); B[k].im = 0.5f * (q.re - p.re);
    }
}

/* the real signals whose spectra (bins 0..N/2) are A and B, through one complex inverse FFT */
static void ifft_two_real(const hp_state *s, const hp_cpx *A, const hp_cpx *B, float *a, float *b) {
    hp_cpx x[HP_FFT];
    for (int k = 0; k <= HP_FFT / 2; k++) {        /* X = A + jB, conjugated for the inverse */
        x[k].re = A[k].re - B[k].im; x[k].im = -(A[k].im + B[k].re);
    }
    for (int k = HP_FFT / 2 + 1; k < HP_FFT; k++) {
        const hp_cpx a2 = A[HP_FFT - k], b2 = B[HP_FFT - k];   /* conj symmetric halves */
        x[k].re = a2.re + b2.im; x[k].im = -(-a2.im + b2.re);
    }
    fft(s, x);
    const float sc = 1.0f / HP_FFT;
    for (int i = 0; i < HP_FFT; i++) { a[i] = x[i].re * sc; b[i] = -x[i].im * sc; }
}

hp_state *hp_create(void) {
    hp_state *s = (hp_state *)calloc(1, sizeof(hp_state));
    if (s)
        fft_init(s);
    return s;
}

void hp_destroy(hp_state *s) {
    if (!s)
        return;
    free(s->h);
    free(s->fdl);
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

/* The engine's response to a full-scale impulse on input channel ch, per ear, divided back
 * to a unit impulse. ir: [2][HP_IR_MAX]. */
static void measure(lhp_engine *e, const uint8_t *blob, size_t len, int ch, float *ir) {
    lhp_init(e);
    /* stereo in: the two front speakers, nothing else (positions L, C, R, Ls, Rs, LFE) */
    const int8_t map[LHP_NPOS] = { 0, -1, 1, -1, -1, -1 };
    lhp_set_map(e, map, 2);
    lhp_set_map_back(e, -1, -1);
    lhp_load_filter(e, blob, len, 1.0f);
    lhp_reset(e);
    float in[LHP_BLOCK * 2], out[LHP_BLOCK * 2];
    for (int b = 0; b * LHP_BLOCK < HP_IR_MAX; b++) {
        memset(in, 0, sizeof(in));
        if (b == 0) in[ch] = 32767.0f;
        lhp_push_f32(e, in, LHP_BLOCK, out);
        for (int i = 0; i < LHP_BLOCK; i++) {
            ir[b * LHP_BLOCK + i] = out[2 * i] / 32767.0f;
            ir[HP_IR_MAX + b * LHP_BLOCK + i] = out[2 * i + 1] / 32767.0f;
        }
    }
}

int hp_load(hp_state *s, const uint8_t *blob, size_t len) {
    lhp_engine *e = (lhp_engine *)malloc(sizeof(lhp_engine));   /* ~330 KB, only while loading */
    float *ir = (float *)malloc(sizeof(float) * 4 * HP_IR_MAX);  /* LL LR RL RR */
    int r = -1;
    if (!e || !ir)
        goto done;
    {   /* the filter must load before anything is measured */
        lhp_init(e);
        const int8_t map[LHP_NPOS] = { 0, -1, 1, -1, -1, -1 };
        if (lhp_set_map(e, map, 2) || lhp_set_map_back(e, -1, -1))
            goto done;
        if ((r = lhp_load_filter(e, blob, len, 1.0f)) < 0)
            goto done;
        r = -1;
    }
    measure(e, blob, len, 0, ir);                    /* left in: LL, LR */
    measure(e, blob, len, 1, ir + 2 * HP_IR_MAX);    /* right in: RL, RR */

    /* keep the response up to where all four have died away (-100 dB of the peak) */
    float peak = 0;
    for (int i = 0; i < 4 * HP_IR_MAX; i++) peak = fabsf(ir[i]) > peak ? fabsf(ir[i]) : peak;
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int i = HP_IR_MAX - 1; i >= n; i--)
            if (fabsf(ir[c * HP_IR_MAX + i]) > peak * 1e-5f) { n = i + 1; break; }
    int parts = (n + HP_BLOCK - 1) / HP_BLOCK;
    if (parts < 1) parts = 1;
    if (parts > HP_PARTS_MAX) parts = HP_PARTS_MAX;

    hp_cpx *h = (hp_cpx *)calloc((size_t)parts * 4 * HP_BINS, sizeof(hp_cpx));
    hp_cpx *fdl = (hp_cpx *)calloc((size_t)parts * 2 * HP_BINS, sizeof(hp_cpx));
    if (!h || !fdl || !out_reserve(s, HP_BLOCK * 2)) { free(h); free(fdl); goto done; }
    for (int p = 0; p < parts; p++)
        for (int c = 0; c < 4; c += 2) {   /* each piece of response, zero-padded to HP_FFT */
            float a[HP_FFT] = { 0 }, b[HP_FFT] = { 0 };
            memcpy(a, ir + c * HP_IR_MAX + p * HP_BLOCK, sizeof(float) * HP_BLOCK);
            memcpy(b, ir + (c + 1) * HP_IR_MAX + p * HP_BLOCK, sizeof(float) * HP_BLOCK);
            fft_two_real(s, a, b, h + ((size_t)p * 4 + c) * HP_BINS, h + ((size_t)p * 4 + c + 1) * HP_BINS);
        }
    free(s->h); free(s->fdl);
    s->h = h; s->fdl = fdl; s->parts = parts; s->head = 0;
    memset(s->in_prev, 0, sizeof(s->in_prev));
    s->fill = 0;
    /* one piece of silence ahead, so every call can hand back as many frames as it got */
    memset(s->out, 0, (size_t)HP_BLOCK * 2 * sizeof(int16_t));
    s->out_n = HP_BLOCK;
    r = 0;
done:
    free(ir);
    free(e);
    return r;
}

static int16_t sat16(float v) {
    long i = lrintf(v);
    return (int16_t)(i > 32767 ? 32767 : i < -32768 ? -32768 : i);
}

/* one HP_BLOCK piece of input (in_cur) -> HP_BLOCK frames appended to out */
static void run_piece(hp_state *s) {
    float a[HP_FFT], b[HP_FFT];
    memcpy(a, s->in_prev[0], sizeof(float) * HP_BLOCK); memcpy(a + HP_BLOCK, s->in_cur[0], sizeof(float) * HP_BLOCK);
    memcpy(b, s->in_prev[1], sizeof(float) * HP_BLOCK); memcpy(b + HP_BLOCK, s->in_cur[1], sizeof(float) * HP_BLOCK);
    memcpy(s->in_prev, s->in_cur, sizeof(s->in_prev));
    s->head = (s->head + s->parts - 1) % s->parts;               /* the newest goes in front */
    hp_cpx *X = s->fdl + (size_t)s->head * 2 * HP_BINS;
    fft_two_real(s, a, b, X, X + HP_BINS);
    hp_cpx Yl[HP_BINS], Yr[HP_BINS];
    memset(Yl, 0, sizeof(Yl)); memset(Yr, 0, sizeof(Yr));
    for (int p = 0; p < s->parts; p++) {
        const hp_cpx *xl = s->fdl + (size_t)((s->head + p) % s->parts) * 2 * HP_BINS, *xr = xl + HP_BINS;
        const hp_cpx *hll = s->h + (size_t)p * 4 * HP_BINS, *hlr = hll + HP_BINS, *hrl = hlr + HP_BINS, *hrr = hrl + HP_BINS;
        for (int k = 0; k < HP_BINS; k++) {
            const float lr = xl[k].re, li = xl[k].im, rr = xr[k].re, ri = xr[k].im;
            Yl[k].re += lr * hll[k].re - li * hll[k].im + rr * hrl[k].re - ri * hrl[k].im;
            Yl[k].im += lr * hll[k].im + li * hll[k].re + rr * hrl[k].im + ri * hrl[k].re;
            Yr[k].re += lr * hlr[k].re - li * hlr[k].im + rr * hrr[k].re - ri * hrr[k].im;
            Yr[k].im += lr * hlr[k].im + li * hlr[k].re + rr * hrr[k].im + ri * hrr[k].re;
        }
    }
    ifft_two_real(s, Yl, Yr, a, b);
    int16_t *o = s->out + (size_t)s->out_n * 2;
    for (int i = 0; i < HP_BLOCK; i++) {           /* overlap-save: the second half is valid */
        o[2 * i] = sat16(a[HP_BLOCK + i]);
        o[2 * i + 1] = sat16(b[HP_BLOCK + i]);
    }
    s->out_n += HP_BLOCK;
}

void hp_process(hp_state *s, int16_t *buf, int frames, int channels) {
    if (!s || !s->h || !buf || frames <= 0 || channels < 2)
        return;
    if (!out_reserve(s, s->out_n + frames + HP_BLOCK))
        return;
    /* in: the first pair, piece by piece */
    for (int i = 0; i < frames; i++) {
        s->in_cur[0][s->fill] = buf[(size_t)i * channels];
        s->in_cur[1][s->fill] = buf[(size_t)i * channels + 1];
        if (++s->fill == HP_BLOCK) {
            run_piece(s);
            s->fill = 0;
        }
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
