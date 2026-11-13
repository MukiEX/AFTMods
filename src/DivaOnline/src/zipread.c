/* A small ZIP reader for DivaOnline.zip: the central directory, and entries stored
   (7z -mx0) or deflated (zip's default). No encryption, ZIP64 or multi-disk archives.
   Inflate follows RFC 1951 directly, decoding Huffman codes canonically. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zipread.h"

/* ---------------------------------------------------------------- inflate */

struct inf_in {
    const uint8_t *p;
    size_t n, pos;
    uint32_t bits;
    int nbits;
};

struct inf_out {
    uint8_t *p;
    size_t n, pos;
};

struct huff {
    uint16_t count[16];    /* codes of each length */
    uint16_t symbol[320];  /* symbols ordered by code */
};

static int inf_bits(struct inf_in *in, int need, uint32_t *out)
{
    while (in->nbits < need) {
        if (in->pos >= in->n) {
            return -1;
        }

        in->bits |= (uint32_t) in->p[in->pos++] << in->nbits;
        in->nbits += 8;
    }

    *out = in->bits & ((1u << need) - 1);
    in->bits >>= need;
    in->nbits -= need;

    return 0;
}

/* Build a canonical decoding table from code lengths; 0 on success. Incomplete codes
   are allowed (a distance code with one symbol), oversubscribed ones aren't. */
static int huff_build(struct huff *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    int left = 1;

    memset(h->count, 0, sizeof(h->count));

    for (int i = 0; i < n; i++) {
        h->count[lengths[i]]++;
    }

    if (h->count[0] == n) {
        return 0;
    }

    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];

        if (left < 0) {
            return -1;
        }
    }

    offs[1] = 0;

    for (int len = 1; len < 15; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }

    for (int i = 0; i < n; i++) {
        if (lengths[i]) {
            h->symbol[offs[lengths[i]]++] = (uint16_t) i;
        }
    }

    return 0;
}

static int huff_decode(struct inf_in *in, const struct huff *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len < 16; len++) {
        uint32_t bit;

        if (inf_bits(in, 1, &bit)) {
            return -1;
        }

        code |= (int) bit;
        int count = h->count[len];

        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }

        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }

    return -1;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inf_codes(struct inf_in *in, struct inf_out *out, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        int sym = huff_decode(in, lit);

        if (sym < 0) {
            return -1;
        }

        if (sym < 256) {
            if (out->pos >= out->n) {
                return -1;
            }

            out->p[out->pos++] = (uint8_t) sym;
        } else if (sym == 256) {
            return 0;
        } else {
            uint32_t extra;

            sym -= 257;

            if (sym >= 29 || inf_bits(in, len_extra[sym], &extra)) {
                return -1;
            }

            size_t len = len_base[sym] + extra;
            int dsym = huff_decode(in, dist);

            if (dsym < 0 || dsym >= 30 || inf_bits(in, dist_extra[dsym], &extra)) {
                return -1;
            }

            size_t d = dist_base[dsym] + extra;

            if (d > out->pos || len > out->n - out->pos) {
                return -1;
            }

            for (size_t i = 0; i < len; i++, out->pos++) {
                out->p[out->pos] = out->p[out->pos - d];
            }
        }
    }
}

static int inf_fixed(struct inf_in *in, struct inf_out *out)
{
    static struct huff lit, dist;
    static int built;

    if (!built) {
        uint8_t l[288];
        int i = 0;

        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lit, l, 288);

        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dist, l, 30);
        built = 1;
    }

    return inf_codes(in, out, &lit, &dist);
}

static int inf_dynamic(struct inf_in *in, struct inf_out *out)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    struct huff lencode, lit, dist;
    uint32_t nlen, ndist, ncode, v;

    if (inf_bits(in, 5, &nlen) || inf_bits(in, 5, &ndist) || inf_bits(in, 4, &ncode)) {
        return -1;
    }

    nlen += 257;
    ndist += 1;
    ncode += 4;

    if (nlen > 286 || ndist > 30) {
        return -1;
    }

    memset(lengths, 0, 19);

    for (uint32_t i = 0; i < ncode; i++) {
        if (inf_bits(in, 3, &v)) {
            return -1;
        }

        lengths[order[i]] = (uint8_t) v;
    }

    if (huff_build(&lencode, lengths, 19)) {
        return -1;
    }

    for (uint32_t i = 0; i < nlen + ndist;) {
        int sym = huff_decode(in, &lencode);
        uint32_t rep;
        uint8_t val = 0;

        if (sym < 0) {
            return -1;
        }

        if (sym < 16) {
            lengths[i++] = (uint8_t) sym;
            continue;
        }

        if (sym == 16) {
            if (i == 0 || inf_bits(in, 2, &rep)) {
                return -1;
            }

            val = lengths[i - 1];
            rep += 3;
        } else if (sym == 17) {
            if (inf_bits(in, 3, &rep)) {
                return -1;
            }

            rep += 3;
        } else {
            if (inf_bits(in, 7, &rep)) {
                return -1;
            }

            rep += 11;
        }

        if (i + rep > nlen + ndist) {
            return -1;
        }

        while (rep--) {
            lengths[i++] = val;
        }
    }

    if (lengths[256] == 0 || huff_build(&lit, lengths, (int) nlen) || huff_build(&dist, lengths + nlen, (int) ndist)) {
        return -1;
    }

    return inf_codes(in, out, &lit, &dist);
}

/* Raw deflate (no zlib header) into out[0..out_n); 0 when it filled exactly out_n bytes */
int zip_inflate(const uint8_t *src, size_t src_n, uint8_t *out, size_t out_n)
{
    struct inf_in in = { src, src_n, 0, 0, 0 };
    struct inf_out o = { out, out_n, 0 };
    uint32_t last, type;

    do {
        if (inf_bits(&in, 1, &last) || inf_bits(&in, 2, &type)) {
            return -1;
        }

        if (type == 0) {
            in.bits = 0;
            in.nbits = 0;   /* to a byte boundary */

            if (in.pos + 4 > in.n) {
                return -1;
            }

            uint32_t len = in.p[in.pos] | (uint32_t) in.p[in.pos + 1] << 8;
            uint32_t nlen = in.p[in.pos + 2] | (uint32_t) in.p[in.pos + 3] << 8;
            in.pos += 4;

            if ((len ^ 0xffff) != nlen || len > in.n - in.pos || len > o.n - o.pos) {
                return -1;
            }

            memcpy(o.p + o.pos, in.p + in.pos, len);
            in.pos += len;
            o.pos += len;
        } else if (type == 1) {
            if (inf_fixed(&in, &o)) {
                return -1;
            }
        } else if (type == 2) {
            if (inf_dynamic(&in, &o)) {
                return -1;
            }
        } else {
            return -1;
        }
    } while (!last);

    return o.pos == out_n ? 0 : -1;
}

/* ---------------------------------------------------------------- CRC-32 */

uint32_t zip_crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int built;

    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;

            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }

            table[i] = c;
        }

        built = 1;
    }

    uint32_t c = 0xffffffffu;

    for (size_t i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }

    return c ^ 0xffffffffu;
}

/* ---------------------------------------------------------------- the archive */

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t) p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }

int zip_open(struct zip_archive *z, const uint8_t *data, size_t n)
{
    memset(z, 0, sizeof(*z));
    z->data = data;
    z->n = n;

    if (n < 22) {
        return -1;
    }

    /* End of central directory: the last 22 bytes, or earlier with a comment */
    size_t lo = n > 22 + 0xffff ? n - 22 - 0xffff : 0;

    for (size_t i = n - 22 + 1; i-- > lo;) {
        if (rd32(data + i) == 0x06054b50) {
            uint32_t count = rd16(data + i + 10);
            uint32_t size = rd32(data + i + 12);
            uint32_t offset = rd32(data + i + 16);

            if (offset > n || size > n - offset) {
                return -1;
            }

            z->count = count;
            z->cd = data + offset;
            z->cd_n = size;

            return 0;
        }
    }

    return -1;
}

int zip_entry(const struct zip_archive *z, uint32_t index, struct zip_entry_info *e)
{
    const uint8_t *p = z->cd, *end = z->cd + z->cd_n;

    for (uint32_t i = 0;; i++) {
        if (p + 46 > end || rd32(p) != 0x02014b50) {
            return -1;
        }

        uint32_t name_n = rd16(p + 28), extra_n = rd16(p + 30), comment_n = rd16(p + 32);

        if (p + 46 + name_n + extra_n + comment_n > end) {
            return -1;
        }

        if (i == index) {
            memset(e, 0, sizeof(*e));
            e->flags = rd16(p + 8);
            e->method = rd16(p + 10);
            e->crc = rd32(p + 16);
            e->comp_size = rd32(p + 20);
            e->size = rd32(p + 24);
            e->local_offset = rd32(p + 42);
            e->name_n = name_n < sizeof(e->name) - 1 ? name_n : sizeof(e->name) - 1;
            memcpy(e->name, p + 46, e->name_n);
            e->name[e->name_n] = '\0';

            return 0;
        }

        p += 46 + name_n + extra_n + comment_n;
    }
}

uint8_t *zip_extract(const struct zip_archive *z, const struct zip_entry_info *e)
{
    if (e->flags & 1) {                      /* encrypted */
        return NULL;
    }

    if (e->local_offset > z->n || z->n - e->local_offset < 30) {
        return NULL;
    }

    const uint8_t *lh = z->data + e->local_offset;

    if (rd32(lh) != 0x04034b50) {
        return NULL;
    }

    size_t start = e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28);

    if (start > z->n || e->comp_size > z->n - start) {
        return NULL;
    }

    uint8_t *out = malloc(e->size ? e->size : 1);

    if (out == NULL) {
        return NULL;
    }

    int ok;

    if (e->method == 0) {
        ok = e->comp_size == e->size;

        if (ok) {
            memcpy(out, z->data + start, e->size);
        }
    } else if (e->method == 8) {
        ok = zip_inflate(z->data + start, e->comp_size, out, e->size) == 0;
    } else {
        ok = 0;
    }

    if (!ok || zip_crc32(out, e->size) != e->crc) {
        free(out);

        return NULL;
    }

    return out;
}
