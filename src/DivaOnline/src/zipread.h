#pragma once

/* A small ZIP reader (stored and deflated entries) for DivaOnline.zip */

#include <stddef.h>
#include <stdint.h>

struct zip_archive {
    const uint8_t *data;
    size_t n;
    uint32_t count;
    const uint8_t *cd;
    size_t cd_n;
};

struct zip_entry_info {
    uint32_t flags, method, crc, comp_size, size, local_offset;
    uint32_t name_n;
    char name[260];
};

/* data must stay valid while the archive is used; 0 on success */
int zip_open(struct zip_archive *z, const uint8_t *data, size_t n);
int zip_entry(const struct zip_archive *z, uint32_t index, struct zip_entry_info *e);
/* the entry's contents (malloc'd, e->size bytes, CRC checked), or NULL */
uint8_t *zip_extract(const struct zip_archive *z, const struct zip_entry_info *e);

int zip_inflate(const uint8_t *src, size_t src_n, uint8_t *out, size_t out_n);
uint32_t zip_crc32(const uint8_t *p, size_t n);
