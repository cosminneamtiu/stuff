/* SPDX-License-Identifier: GPL-2.0-only */
#include "nsofile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/* --- SHA-256 (FIPS 180-4) ---------------------------------------------------- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t state[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(Sha256 *ctx) {
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->used = 0;
    ctx->total = 0;
}

void sha256_update(Sha256 *ctx, const uint8_t *data, size_t size) {
    ctx->total += size;
    if (ctx->used) {
        size_t take = 64 - ctx->used;
        if (take > size)
            take = size;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        size -= take;
        if (ctx->used < 64)
            return;
        sha256_block(ctx->state, ctx->block);
        ctx->used = 0;
    }
    for (; size >= 64; data += 64, size -= 64)
        sha256_block(ctx->state, data);
    if (size) {
        memcpy(ctx->block, data, size);
        ctx->used = size;
    }
}

void sha256_final(Sha256 *ctx, uint8_t out[32]) {
    uint8_t tail[128];
    size_t rest = ctx->used;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, ctx->block, rest);
    tail[rest] = 0x80;
    size_t total = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = ctx->total * 8;
    for (int i = 0; i < 8; i++)
        tail[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(ctx->state, tail);
    if (total == 128)
        sha256_block(ctx->state, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(ctx->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)ctx->state[i];
    }
}

void sha256(const uint8_t *data, size_t size, uint8_t out[32]) {
    Sha256 ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, size);
    sha256_final(&ctx, out);
}

/* --- LZ4 block format -------------------------------------------------------- */

long lz4_decompress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size) {
    size_t s = 0, d = 0;
    while (s < src_size) {
        unsigned token = src[s++];
        size_t literals = token >> 4;
        if (literals == 15) {
            unsigned b;
            do {
                if (s >= src_size)
                    return -1;
                b = src[s++];
                literals += b;
            } while (b == 255);
        }
        if (literals > src_size - s || literals > dst_size - d)
            return -1;
        memcpy(dst + d, src + s, literals);
        s += literals;
        d += literals;
        if (s >= src_size)
            break; /* the last sequence has no match */
        if (src_size - s < 2)
            return -1;
        size_t offset = src[s] | (size_t)src[s + 1] << 8;
        s += 2;
        size_t length = (token & 15) + 4;
        if ((token & 15) == 15) {
            unsigned b;
            do {
                if (s >= src_size)
                    return -1;
                b = src[s++];
                length += b;
            } while (b == 255);
        }
        if (!offset || offset > d || length > dst_size - d)
            return -1;
        for (size_t i = 0; i < length; i++, d++) /* may overlap */
            dst[d] = dst[d - offset];
    }
    return (long)d;
}

size_t lz4_bound(size_t src_size) {
    return src_size + src_size / 255 + 16;
}

static size_t put_length(uint8_t *dst, size_t d, size_t length) {
    while (length >= 255) {
        dst[d++] = 255;
        length -= 255;
    }
    dst[d++] = (uint8_t)length;
    return d;
}

/* A plain greedy compressor: a hash table of the last position of each 4-byte
 * sequence. The format's end rules: the last 5 bytes are literals and the
 * last match starts at least 12 bytes before the end. */
long lz4_compress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_capacity) {
    enum { HASH_BITS = 16 };
    if (dst_capacity < lz4_bound(src_size))
        return -1;
    uint32_t *table = calloc((size_t)1 << HASH_BITS, sizeof(uint32_t));
    if (!table)
        return -1;
    size_t d = 0, anchor = 0, i = 0;
    size_t match_limit = src_size > 12 ? src_size - 12 : 0;
    while (i < match_limit) {
        uint32_t sequence = rd32(src + i);
        uint32_t hash = (sequence * 2654435761u) >> (32 - HASH_BITS);
        size_t candidate = table[hash];
        table[hash] = (uint32_t)(i + 1);
        if (candidate && i - (candidate - 1) <= 0xffff && rd32(src + candidate - 1) == sequence) {
            size_t from = candidate - 1, length = 4, limit = src_size - 5;
            while (i + length < limit && src[from + length] == src[i + length])
                length++;
            size_t literals = i - anchor;
            uint8_t *token = dst + d++;
            *token = (uint8_t)((literals >= 15 ? 15 : literals) << 4);
            if (literals >= 15)
                d = put_length(dst, d, literals - 15);
            memcpy(dst + d, src + anchor, literals);
            d += literals;
            dst[d++] = (uint8_t)(i - from);
            dst[d++] = (uint8_t)((i - from) >> 8);
            if (length - 4 >= 15) {
                *token |= 15;
                d = put_length(dst, d, length - 4 - 15);
            } else {
                *token |= (uint8_t)(length - 4);
            }
            i += length;
            anchor = i;
        } else {
            i++;
        }
    }
    size_t literals = src_size - anchor;
    uint8_t *token = dst + d++;
    *token = (uint8_t)((literals >= 15 ? 15 : literals) << 4);
    if (literals >= 15)
        d = put_length(dst, d, literals - 15);
    memcpy(dst + d, src + anchor, literals);
    d += literals;
    free(table);
    return (long)d;
}

/* --- NSO --------------------------------------------------------------------- */

static int fail(char *error, size_t error_size, const char *text) {
    if (error && error_size)
        snprintf(error, error_size, "%s", text);
    return -1;
}

int nso_load(const uint8_t *file, size_t file_size, NsoImage *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    if (file_size < 0x100 || memcmp(file, "NSO0", 4))
        return fail(error, error_size, "not an NSO file");
    uint32_t flags = rd32(file + 0x0c);
    uint64_t end = 0;
    for (int i = 0; i < 3; i++) {
        out->file_off[i] = rd32(file + 0x10 + 0x10 * i);
        out->mem[i] = rd32(file + 0x14 + 0x10 * i);
        out->size[i] = rd32(file + 0x18 + 0x10 * i);
        out->file_size_of[i] = rd32(file + 0x60 + 4 * i);
        out->compressed[i] = (flags >> i) & 1;
        if (!out->compressed[i])
            out->file_size_of[i] = out->size[i];
        if ((uint64_t)out->file_off[i] + out->file_size_of[i] > file_size)
            return fail(error, error_size, "an NSO segment lies outside the file");
        if ((uint64_t)out->mem[i] + out->size[i] > end)
            end = (uint64_t)out->mem[i] + out->size[i];
    }
    if (out->mem[0] > out->mem[1] || out->mem[1] > out->mem[2] ||
        (uint64_t)out->mem[0] + out->size[0] > out->mem[1] || (uint64_t)out->mem[1] + out->size[1] > out->mem[2])
        return fail(error, error_size, "NSO segments overlap or are out of order");
    out->bss_size = rd32(file + 0x3c);
    out->api_off = rd32(file + 0x88);
    out->api_size = rd32(file + 0x8c);
    memcpy(out->build_id, file + 0x40, 32);
    end += out->bss_size;
    out->image_size = (end + 0xfff) & ~(uint64_t)0xfff;
    if (out->image_size > ((uint64_t)1 << 31))
        return fail(error, error_size, "NSO image is too large");
    out->image = calloc(1, (size_t)out->image_size);
    if (!out->image)
        return fail(error, error_size, "not enough memory for the module image");
    for (int i = 0; i < 3; i++) {
        uint8_t *target = out->image + out->mem[i];
        if (out->compressed[i]) {
            if (lz4_decompress(file + out->file_off[i], out->file_size_of[i], target, out->size[i]) !=
                (long)out->size[i]) {
                nso_free(out);
                return fail(error, error_size, "an NSO segment does not decompress");
            }
        } else {
            memcpy(target, file + out->file_off[i], out->size[i]);
        }
        if (flags & (8u << i)) {
            uint8_t digest[32];
            sha256(target, out->size[i], digest);
            if (memcmp(digest, file + 0xa0 + 0x20 * i, 32)) {
                nso_free(out);
                return fail(error, error_size, "an NSO segment does not match its hash");
            }
        }
    }
    out->file = file;
    out->file_size = file_size;
    return 0;
}

void nso_free(NsoImage *nso) {
    free(nso->image);
    nso->image = NULL;
}

int nso_is32(const NsoImage *nso) {
    /* The same test as the PC tools: does the dynamic section read as 32-bit entries? */
    if (nso->image_size < 8)
        return 0;
    uint32_t mod0 = rd32(nso->image + 4);
    if ((uint64_t)mod0 + 8 > nso->image_size || memcmp(nso->image + mod0, "MOD0", 4))
        return 0;
    uint64_t dynamic = (uint64_t)mod0 + (int32_t)rd32(nso->image + mod0 + 4);
    int strtab32 = 0, symtab32 = 0, strtab64 = 0, symtab64 = 0;
    for (int i = 0; i < 64 && dynamic + 8 * (uint64_t)(i + 1) <= nso->image_size; i++) {
        int32_t tag = (int32_t)rd32(nso->image + dynamic + 8 * i);
        if (!tag)
            break;
        strtab32 |= tag == 5;
        symtab32 |= tag == 6;
    }
    for (int i = 0; i < 64 && dynamic + 16 * (uint64_t)(i + 1) <= nso->image_size; i++) {
        int64_t tag = (int64_t)rd64(nso->image + dynamic + 16 * i);
        if (!tag)
            break;
        strtab64 |= tag == 5;
        symtab64 |= tag == 6;
    }
    return strtab32 && symtab32 && !(strtab64 && symtab64);
}

int nso_build(const NsoImage *nso, unsigned changed, uint8_t **out, size_t *out_size) {
    uint32_t name_off = rd32(nso->file + 0x1c), name_size = rd32(nso->file + 0x2c);
    if ((uint64_t)name_off + name_size > nso->file_size)
        return -1;
    size_t capacity = 0x100 + name_size;
    for (int i = 0; i < 3; i++)
        capacity += (changed & (1u << i)) ? lz4_bound(nso->size[i]) : nso->file_size_of[i];
    uint8_t *file = malloc(capacity);
    if (!file)
        return -1;
    memcpy(file, nso->file, 0x100);
    memcpy(file + 0x100, nso->file + name_off, name_size);
    wr32(file + 0x1c, 0x100);
    size_t at = 0x100 + name_size;
    uint32_t flags = rd32(nso->file + 0x0c);
    for (int i = 0; i < 3; i++) {
        size_t length;
        if (!(changed & (1u << i))) {
            length = nso->file_size_of[i];
            memcpy(file + at, nso->file + nso->file_off[i], length);
        } else {
            const uint8_t *data = nso->image + nso->mem[i];
            if (nso->compressed[i]) {
                long packed = lz4_compress(data, nso->size[i], file + at, capacity - at);
                if (packed < 0) {
                    free(file);
                    return -1;
                }
                length = (size_t)packed;
            } else {
                length = nso->size[i];
                memcpy(file + at, data, length);
            }
            sha256(data, nso->size[i], file + 0xa0 + 0x20 * i);
            flags |= 8u << i;
        }
        wr32(file + 0x10 + 0x10 * i, (uint32_t)at);
        wr32(file + 0x60 + 4 * i, (uint32_t)length);
        at += length;
    }
    wr32(file + 0x0c, flags);
    *out = file;
    *out_size = at;
    return 0;
}

/* --- PFS0 -------------------------------------------------------------------- */

int pfs0_list(const uint8_t *image, size_t size, Pfs0Entry *entries, int max) {
    size_t needed;
    int count = pfs0_parse(image, size, size, entries, max, &needed);
    return count < 0 ? -1 : count;
}

size_t pfs0_header(const uint8_t *old_header, const Pfs0Entry *entries, int count, uint8_t *out) {
    uint32_t names_size = rd32(old_header + 8);
    size_t header = 0x10 + (size_t)count * 0x18 + names_size;
    memcpy(out, old_header, header);
    uint64_t at = 0;
    for (int i = 0; i < count; i++) {
        wr64(out + 0x10 + 0x18 * i, at);
        wr64(out + 0x10 + 0x18 * i + 8, entries[i].size);
        at += entries[i].size;
    }
    return header;
}

int pfs0_parse(const uint8_t *image, size_t header_size, uint64_t size, Pfs0Entry *entries, int max,
               size_t *needed) {
    *needed = 0x10;
    if (header_size < 0x10)
        return -2;
    if (memcmp(image, "PFS0", 4))
        return -1;
    uint32_t count = rd32(image + 4), names_size = rd32(image + 8);
    uint64_t names = 0x10 + (uint64_t)count * 0x18, base = names + names_size;
    if (count > 64 || base > size || (int)count > max)
        return -1;
    *needed = (size_t)base;
    if (base > header_size)
        return -2;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = image + 0x10 + 0x18 * i;
        uint64_t offset = rd64(e), length = rd64(e + 8);
        uint32_t name = rd32(e + 16);
        if (name >= names_size || offset > size - base || length > size - base - offset)
            return -1;
        size_t n = 0;
        while (name + n < names_size && image[names + name + n] && n < sizeof(entries[i].name) - 1) {
            entries[i].name[n] = (char)image[names + name + n];
            n++;
        }
        entries[i].name[n] = 0;
        entries[i].offset = base + offset;
        entries[i].size = length;
    }
    return (int)count;
}

int pfs0_replace(const uint8_t *image, size_t size, const char *name, const uint8_t *data, size_t data_size,
                 uint8_t **out) {
    Pfs0Entry entries[64];
    int count = pfs0_list(image, size, entries, 64);
    if (count < 0)
        return -1;
    uint32_t names_size = rd32(image + 8);
    size_t header = 0x10 + (size_t)count * 0x18 + names_size;
    uint64_t total = header;
    int found = 0;
    for (int i = 0; i < count; i++) {
        int is_it = !strcmp(entries[i].name, name);
        found |= is_it;
        total += is_it ? data_size : entries[i].size;
    }
    if (!found)
        return -1;
    if (total > size)
        return -2;
    uint8_t *copy = calloc(1, size);
    if (!copy)
        return -1;
    memcpy(copy, image, header);
    uint64_t at = 0;
    for (int i = 0; i < count; i++) {
        int is_it = !strcmp(entries[i].name, name);
        uint64_t length = is_it ? data_size : entries[i].size;
        wr64(copy + 0x10 + 0x18 * i, at);
        wr64(copy + 0x10 + 0x18 * i + 8, length);
        memcpy(copy + header + at, is_it ? data : image + entries[i].offset, (size_t)length);
        at += length;
    }
    *out = copy;
    return 0;
}
