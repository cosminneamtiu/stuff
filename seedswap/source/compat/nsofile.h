/* SPDX-License-Identifier: GPL-2.0-only */
/* NSO modules and PFS0 (ExeFS) images in memory: load, change, write back.
 * Plain C with no platform calls, so the PC tests run the same code. */
#ifndef SEEDSWAP_NSOFILE_H
#define SEEDSWAP_NSOFILE_H

#include <stddef.h>
#include <stdint.h>

typedef struct Sha256 {
    uint32_t state[8];
    uint8_t block[64];
    size_t used;
    uint64_t total;
} Sha256;

void sha256_init(Sha256 *ctx);
void sha256_update(Sha256 *ctx, const uint8_t *data, size_t size);
void sha256_final(Sha256 *ctx, uint8_t out[32]);
void sha256(const uint8_t *data, size_t size, uint8_t out[32]);

/* LZ4 block format. Both return the number of bytes produced, or -1. */
long lz4_decompress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size);
long lz4_compress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_capacity);
size_t lz4_bound(size_t src_size);

typedef struct NsoImage {
    const uint8_t *file;         /* the NSO file this was loaded from (borrowed) */
    size_t file_size;
    uint8_t *image;              /* text, rodata, data and zeroed bss at their memory offsets */
    uint64_t image_size;         /* a whole number of pages */
    uint32_t mem[3], size[3];    /* text, rodata, data: memory offset and size */
    uint32_t file_off[3], file_size_of[3];
    int compressed[3];
    uint32_t bss_size;
    uint32_t api_off, api_size;  /* the SDK version strings, relative to rodata */
    uint8_t build_id[32];
} NsoImage;

/* Allocates out->image. Checks the segment hashes the file asks to be checked. */
int nso_load(const uint8_t *file, size_t file_size, NsoImage *out, char *error, size_t error_size);
void nso_free(NsoImage *nso);
/* Whether the module is 32-bit code (from its dynamic section). */
int nso_is32(const NsoImage *nso);
/* Writes a new NSO file. A segment whose bit is clear in changed keeps its bytes from the original
 * file; a changed one is taken from the image and compressed again if it was compressed.
 * *out is allocated; free() it. */
int nso_build(const NsoImage *nso, unsigned changed, uint8_t **out, size_t *out_size);

typedef struct Pfs0Entry {
    char name[64];
    uint64_t offset;             /* from the start of the image */
    uint64_t size;
} Pfs0Entry;

/* Reads the file table. Returns the number of entries (at most max), or -1. */
int pfs0_list(const uint8_t *image, size_t size, Pfs0Entry *entries, int max);
/* The same from the first header_size bytes of an image that is total_size long. Returns -2 when
 * header_size does not cover the table; *needed then says how many bytes do. */
int pfs0_parse(const uint8_t *header, size_t header_size, uint64_t total_size, Pfs0Entry *entries, int max,
               size_t *needed);
/* Writes the header of an image with these files stored back to back, in the layout of the header it
 * was read from (same names, same table size). Returns the header's size. */
size_t pfs0_header(const uint8_t *old_header, const Pfs0Entry *entries, int count, uint8_t *out);
/* A copy of the image with one file replaced, padded with zeros to the old size.
 * Returns 0, -1 for a bad image, -2 when it no longer fits. *out is allocated. */
int pfs0_replace(const uint8_t *image, size_t size, const char *name, const uint8_t *data, size_t data_size,
                 uint8_t **out);

#endif
