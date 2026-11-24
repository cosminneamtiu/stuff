// SPDX-License-Identifier: GPL-2.0-only
// Compressed NCAs (.ncz, inside .nsz and .xcz dumps): the NCA's first 0x4000 bytes as they are, a list
// of its sections with their keys and counters, then everything after 0x4000 decrypted and
// zstd-compressed, as one stream ("solid") or as independent blocks (NCZBLOCK). Reading gives the
// decrypted bytes at an NCA offset; pkg.c turns them into what a reader of the real NCA would see.
// Plain C (zstd and the read callback only), so the PC tests run the same code.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*NczReadFn)(void *user, uint64_t off, void *buf, size_t n);  // 0 = all n bytes read

typedef struct {
    uint64_t offset, size;     // within the NCA
    uint32_t crypto;           // 1 = stored as it is in the NCA, 3/4 = AES-CTR (decrypted here)
    uint8_t key[16], counter[16];
} NczSection;

typedef struct Ncz Ncz;

// start, size: where the .ncz lies in the dump. Returns NULL with a sentence in err.
Ncz *ncz_open(NczReadFn read, void *user, uint64_t start, uint64_t size, char *err, size_t errn);
void ncz_close(Ncz *z);
uint64_t ncz_nca_size(const Ncz *z);   // the size of the original NCA
bool ncz_is_block(const Ncz *z);
// The decrypted bytes [off, off + n) of the NCA, off >= 0x4000. Fastest in increasing order: a solid
// stream is read forwards and starts over from its beginning when asked for something earlier.
bool ncz_read(Ncz *z, uint64_t off, void *buf, size_t n);
// The section that holds NCA offset off (NULL outside them); *end = where it ends.
const NczSection *ncz_section(const Ncz *z, uint64_t off);
uint64_t ncz_restarts(const Ncz *z);
const char *ncz_error(const Ncz *z);
