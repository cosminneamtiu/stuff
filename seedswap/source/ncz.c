// SPDX-License-Identifier: GPL-2.0-only
#include "ncz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define NCA_PLAIN 0x4000u
#define IN_CHUNK (1u << 20)
#define WINDOW (24u << 20)        // decompressed bytes kept: the largest read plus some history
#define KEEP (2u << 20)           // history kept when the window moves on (small reads just behind)

struct Ncz {
    NczReadFn read;
    void *user;
    uint64_t start, size;        // the .ncz in the dump
    NczSection *sec;
    int nsec;
    uint64_t nca_size;
    uint64_t body;               // where the compressed data starts, relative to start
    // NCZBLOCK
    bool block;
    uint32_t block_size, nblocks;
    uint64_t total;
    uint64_t *block_at;          // compressed offset of each block, relative to start
    uint32_t *block_csize;
    // solid
    ZSTD_DStream *ds;
    uint8_t *in;
    size_t in_len, in_pos;
    uint64_t in_next;            // next compressed byte to read, relative to start
    bool ended;
    // decompressed window: NCA bytes [win_off, win_off + win_len)
    uint8_t *win;
    uint64_t win_off;
    size_t win_len;
    uint8_t *scratch;            // a decompressed block
    uint64_t restarts;
    char err[160];
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static bool fail(Ncz *z, const char *what) {
    snprintf(z->err, sizeof(z->err), "%s", what);
    return false;
}

static bool raw(Ncz *z, uint64_t off, void *buf, size_t n) {
    return off + n <= z->size && z->read(z->user, z->start + off, buf, n) == 0;
}

static bool solid_restart(Ncz *z) {
    if (!z->ds && !(z->ds = ZSTD_createDStream())) return fail(z, "Not enough memory to decompress the dump.");
    ZSTD_initDStream(z->ds);
    z->in_len = z->in_pos = 0;
    z->in_next = z->body;
    z->ended = false;
    z->win_off = NCA_PLAIN;
    z->win_len = 0;
    return true;
}

Ncz *ncz_open(NczReadFn read, void *user, uint64_t start, uint64_t size, char *err, size_t errn) {
    Ncz *z = calloc(1, sizeof(Ncz));
    if (!z) { snprintf(err, errn, "Not enough memory."); return NULL; }
    z->read = read;
    z->user = user;
    z->start = start;
    z->size = size;
    uint8_t head[16];
    if (!raw(z, NCA_PLAIN, head, 16) || memcmp(head, "NCZSECTN", 8)) {
        snprintf(err, errn, "A compressed NCA in the dump has no section list; the file is damaged.");
        free(z);
        return NULL;
    }
    uint64_t count = rd64(head + 8);
    if (!count || count > 100000) {
        snprintf(err, errn, "A compressed NCA in the dump has a damaged section list.");
        free(z);
        return NULL;
    }
    z->sec = calloc((size_t)count + 1, sizeof(NczSection));
    uint8_t *table = malloc((size_t)count * 0x40);
    bool ok = z->sec && table && raw(z, NCA_PLAIN + 16, table, (size_t)count * 0x40);
    for (uint64_t i = 0; ok && i < count; i++) {
        const uint8_t *t = table + 0x40 * i;
        NczSection *s = &z->sec[z->nsec];
        s->offset = rd64(t);
        s->size = rd64(t + 8);
        s->crypto = (uint32_t)rd64(t + 16);
        memcpy(s->key, t + 32, 16);
        memcpy(s->counter, t + 48, 16);
        if (s->crypto != 1 && s->crypto != 3 && s->crypto != 4) ok = false;
        if (!s->size) continue;
        z->nsec++;
    }
    free(table);
    if (ok && z->nsec && z->sec[0].offset > NCA_PLAIN) {  // the unencrypted part before the first section
        memmove(z->sec + 1, z->sec, sizeof(NczSection) * (size_t)z->nsec);
        memset(z->sec, 0, sizeof(NczSection));
        z->sec[0].offset = NCA_PLAIN;
        z->sec[0].size = z->sec[1].offset - NCA_PLAIN;
        z->sec[0].crypto = 1;
        z->nsec++;
    }
    uint64_t at = NCA_PLAIN;
    for (int i = 0; ok && i < z->nsec; i++) {
        NczSection *s = &z->sec[i];
        if (s->offset < at) {  // a section that starts inside the plain header
            if (s->offset + s->size <= at) ok = false;
            else {
                s->size -= at - s->offset;
                s->offset = at;
            }
        }
        if (s->offset != at) ok = false;
        at = s->offset + s->size;
    }
    if (!ok || !z->nsec) {
        snprintf(err, errn, "A compressed NCA in the dump has sections this tool does not understand.");
        ncz_close(z);
        return NULL;
    }
    z->nca_size = at;
    z->body = NCA_PLAIN + 16 + 0x40 * count;
    uint8_t bh[24];
    if (raw(z, z->body, bh, sizeof(bh)) && !memcmp(bh, "NCZBLOCK", 8)) {
        uint8_t exponent = bh[11];
        z->nblocks = rd32(bh + 12);
        z->total = rd64(bh + 16);
        if (exponent < 14 || exponent > 32 || !z->nblocks || z->total + NCA_PLAIN != z->nca_size) {
            snprintf(err, errn, "A compressed NCA in the dump has a damaged block list.");
            ncz_close(z);
            return NULL;
        }
        z->block = true;
        z->block_size = (uint32_t)1 << exponent;
        z->block_at = malloc(sizeof(uint64_t) * z->nblocks);
        z->block_csize = malloc(sizeof(uint32_t) * z->nblocks);
        uint8_t *sizes = malloc(4 * (size_t)z->nblocks);
        ok = z->block_at && z->block_csize && sizes && raw(z, z->body + 24, sizes, 4 * (size_t)z->nblocks);
        uint64_t c = z->body + 24 + 4 * (uint64_t)z->nblocks;
        for (uint32_t i = 0; ok && i < z->nblocks; i++) {
            z->block_csize[i] = rd32(sizes + 4 * i);
            z->block_at[i] = c;
            c += z->block_csize[i];
        }
        free(sizes);
        if (!ok || c > size) {
            snprintf(err, errn, "A compressed NCA in the dump is cut short.");
            ncz_close(z);
            return NULL;
        }
        z->scratch = malloc(z->block_size);
        z->win_off = UINT64_MAX;
        if (!z->scratch) {
            snprintf(err, errn, "Not enough memory to decompress the dump.");
            ncz_close(z);
            return NULL;
        }
    } else {
        z->in = malloc(IN_CHUNK);
        z->win = malloc(WINDOW);
        if (!z->in || !z->win || !solid_restart(z)) {
            snprintf(err, errn, "Not enough memory to decompress the dump.");
            ncz_close(z);
            return NULL;
        }
    }
    return z;
}

void ncz_close(Ncz *z) {
    if (!z) return;
    if (z->ds) ZSTD_freeDStream(z->ds);
    free(z->sec);
    free(z->block_at);
    free(z->block_csize);
    free(z->scratch);
    free(z->in);
    free(z->win);
    memset(z, 0, sizeof(*z));
    free(z);
}

uint64_t ncz_nca_size(const Ncz *z) { return z->nca_size; }
bool ncz_is_block(const Ncz *z) { return z->block; }
uint64_t ncz_restarts(const Ncz *z) { return z->restarts; }
const char *ncz_error(const Ncz *z) { return z->err; }

const NczSection *ncz_section(const Ncz *z, uint64_t off) {
    int lo = 0, hi = z->nsec - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const NczSection *s = &z->sec[mid];
        if (off < s->offset) hi = mid - 1;
        else if (off >= s->offset + s->size) lo = mid + 1;
        else return s;
    }
    return NULL;
}

// Decompresses until the window reaches `until` (NCA offset), keeping KEEP bytes before `from`.
static bool solid_fill(Ncz *z, uint64_t from, uint64_t until) {
    if (from > z->win_off + KEEP) {  // drop what lies too far behind
        uint64_t drop = from - KEEP - z->win_off;
        if (drop >= z->win_len) {
            z->win_off += z->win_len;
            z->win_len = 0;
        } else {
            memmove(z->win, z->win + drop, z->win_len - (size_t)drop);
            z->win_len -= (size_t)drop;
            z->win_off += drop;
        }
    }
    while (z->win_off + z->win_len < until) {
        if (z->win_len == WINDOW) {  // full: keep the newest half
            size_t drop = WINDOW / 2;
            if (z->win_off + drop > from) return fail(z, "A read from the compressed dump was larger than expected.");
            memmove(z->win, z->win + drop, WINDOW - drop);
            z->win_len -= drop;
            z->win_off += drop;
        }
        if (z->in_pos == z->in_len) {
            if (z->in_next >= z->size) return fail(z, "The compressed data ends early; the dump is cut short.");
            size_t k = z->size - z->in_next < IN_CHUNK ? (size_t)(z->size - z->in_next) : IN_CHUNK;
            if (!raw(z, z->in_next, z->in, k)) return fail(z, "The dump could not be read.");
            z->in_next += k;
            z->in_len = k;
            z->in_pos = 0;
        }
        ZSTD_inBuffer in = {z->in, z->in_len, z->in_pos};
        ZSTD_outBuffer out = {z->win + z->win_len, WINDOW - z->win_len, 0};
        size_t r = ZSTD_decompressStream(z->ds, &out, &in);
        if (ZSTD_isError(r)) return fail(z, "The compressed data is damaged (zstd could not read it).");
        z->in_pos = in.pos;
        z->win_len += out.pos;
    }
    return true;
}

static bool block_load(Ncz *z, uint32_t i) {
    uint64_t at = (uint64_t)i * z->block_size;
    size_t want = z->total - at < z->block_size ? (size_t)(z->total - at) : z->block_size;
    uint32_t c = z->block_csize[i];
    if (c == want) {
        if (!raw(z, z->block_at[i], z->scratch, want)) return fail(z, "The dump could not be read.");
    } else {
        uint8_t *src = malloc(c ? c : 1);
        if (!src) return fail(z, "Not enough memory to decompress the dump.");
        bool ok = raw(z, z->block_at[i], src, c);
        size_t r = ok ? ZSTD_decompress(z->scratch, want, src, c) : 0;
        free(src);
        if (!ok) return fail(z, "The dump could not be read.");
        if (ZSTD_isError(r) || r != want) return fail(z, "A compressed block is damaged (zstd could not read it).");
    }
    z->win_off = NCA_PLAIN + at;
    z->win_len = want;
    return true;
}

bool ncz_read(Ncz *z, uint64_t off, void *buf, size_t n) {
    uint8_t *out = buf;
    if (off < NCA_PLAIN || off + n > z->nca_size) return fail(z, "A read outside the compressed NCA.");
    if (z->block) {
        while (n) {
            if (z->win_off == UINT64_MAX || off < z->win_off || off >= z->win_off + z->win_len)
                if (!block_load(z, (uint32_t)((off - NCA_PLAIN) / z->block_size))) return false;
            size_t k = (size_t)(z->win_off + z->win_len - off);
            if (k > n) k = n;
            memcpy(out, z->scratch + (off - z->win_off), k);
            out += k;
            off += k;
            n -= k;
        }
        return true;
    }
    while (n) {
        if (off < z->win_off) {  // behind the window: start the stream over
            z->restarts++;
            if (!solid_restart(z)) return false;
        }
        size_t k = n > WINDOW / 4 ? WINDOW / 4 : n;
        if (!solid_fill(z, off, off + k)) return false;
        memcpy(out, z->win + (off - z->win_off), k);
        out += k;
        off += k;
        n -= k;
    }
    return true;
}
