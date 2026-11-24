// SPDX-License-Identifier: GPL-2.0-only
#include "dumpcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/nsofile.h"

// Nintendo's public keys for NCA header signature 0 (retail), by signature key generation. Public
// verification keys only; the same values as in Sphaira (yati/nx/nca.cpp) and hactool.
static const uint8_t NCA_HEADER_MODULI[2][256] = {
    {
        0xBF, 0xBE, 0x40, 0x6C, 0xF4, 0xA7, 0x80, 0xE9, 0xF0, 0x7D, 0x0C, 0x99, 0x61, 0x1D, 0x77, 0x2F,
        0x96, 0xBC, 0x4B, 0x9E, 0x58, 0x38, 0x1B, 0x03, 0xAB, 0xB1, 0x75, 0x49, 0x9F, 0x2B, 0x4D, 0x58,
        0x34, 0xB0, 0x05, 0xA3, 0x75, 0x22, 0xBE, 0x1A, 0x3F, 0x03, 0x73, 0xAC, 0x70, 0x68, 0xD1, 0x16,
        0xB9, 0x04, 0x46, 0x5E, 0xB7, 0x07, 0x91, 0x2F, 0x07, 0x8B, 0x26, 0xDE, 0xF6, 0x00, 0x07, 0xB2,
        0xB4, 0x51, 0xF8, 0x0D, 0x0A, 0x5E, 0x58, 0xAD, 0xEB, 0xBC, 0x9A, 0xD6, 0x49, 0xB9, 0x64, 0xEF,
        0xA7, 0x82, 0xB5, 0xCF, 0x6D, 0x70, 0x13, 0xB0, 0x0F, 0x85, 0xF6, 0xA9, 0x08, 0xAA, 0x4D, 0x67,
        0x66, 0x87, 0xFA, 0x89, 0xFF, 0x75, 0x90, 0x18, 0x1E, 0x6B, 0x3D, 0xE9, 0x8A, 0x68, 0xC9, 0x26,
        0x04, 0xD9, 0x80, 0xCE, 0x3F, 0x5E, 0x92, 0xCE, 0x01, 0xFF, 0x06, 0x3B, 0xF2, 0xC1, 0xA9, 0x0C,
        0xCE, 0x02, 0x6F, 0x16, 0xBC, 0x92, 0x42, 0x0A, 0x41, 0x64, 0xCD, 0x52, 0xB6, 0x34, 0x4D, 0xAE,
        0xC0, 0x2E, 0xDE, 0xA4, 0xDF, 0x27, 0x68, 0x3C, 0xC1, 0xA0, 0x60, 0xAD, 0x43, 0xF3, 0xFC, 0x86,
        0xC1, 0x3E, 0x6C, 0x46, 0xF7, 0x7C, 0x29, 0x9F, 0xFA, 0xFD, 0xF0, 0xE3, 0xCE, 0x64, 0xE7, 0x35,
        0xF2, 0xF6, 0x56, 0x56, 0x6F, 0x6D, 0xF1, 0xE2, 0x42, 0xB0, 0x83, 0x40, 0xA5, 0xC3, 0x20, 0x2B,
        0xCC, 0x9A, 0xAE, 0xCA, 0xED, 0x4D, 0x70, 0x30, 0xA8, 0x70, 0x1C, 0x70, 0xFD, 0x13, 0x63, 0x29,
        0x02, 0x79, 0xEA, 0xD2, 0xA7, 0xAF, 0x35, 0x28, 0x32, 0x1C, 0x7B, 0xE6, 0x2F, 0x1A, 0xAA, 0x40,
        0x7E, 0x32, 0x8C, 0x27, 0x42, 0xFE, 0x82, 0x78, 0xEC, 0x0D, 0xEB, 0xE6, 0x83, 0x4B, 0x6D, 0x81,
        0x04, 0x40, 0x1A, 0x9E, 0x9A, 0x67, 0xF6, 0x72, 0x29, 0xFA, 0x04, 0xF0, 0x9D, 0xE4, 0xF4, 0x03,
    },
    {
        0xAD, 0xE3, 0xE1, 0xFA, 0x04, 0x35, 0xE5, 0xB6, 0xDD, 0x49, 0xEA, 0x89, 0x29, 0xB1, 0xFF, 0xB6,
        0x43, 0xDF, 0xCA, 0x96, 0xA0, 0x4A, 0x13, 0xDF, 0x43, 0xD9, 0x94, 0x97, 0x96, 0x43, 0x65, 0x48,
        0x70, 0x58, 0x33, 0xA2, 0x7D, 0x35, 0x7B, 0x96, 0x74, 0x5E, 0x0B, 0x5C, 0x32, 0x18, 0x14, 0x24,
        0xC2, 0x58, 0xB3, 0x6C, 0x22, 0x7A, 0xA1, 0xB7, 0xCB, 0x90, 0xA7, 0xA3, 0xF9, 0x7D, 0x45, 0x16,
        0xA5, 0xC8, 0xED, 0x8F, 0xAD, 0x39, 0x5E, 0x9E, 0x4B, 0x51, 0x68, 0x7D, 0xF8, 0x0C, 0x35, 0xC6,
        0x3F, 0x91, 0xAE, 0x44, 0xA5, 0x92, 0x30, 0x0D, 0x46, 0xF8, 0x40, 0xFF, 0xD0, 0xFF, 0x06, 0xD2,
        0x1C, 0x7F, 0x96, 0x18, 0xDC, 0xB7, 0x1D, 0x66, 0x3E, 0xD1, 0x73, 0xBC, 0x15, 0x8A, 0x2F, 0x94,
        0xF3, 0x00, 0xC1, 0x83, 0xF1, 0xCD, 0xD7, 0x81, 0x88, 0xAB, 0xDF, 0x8C, 0xEF, 0x97, 0xDD, 0x1B,
        0x17, 0x5F, 0x58, 0xF6, 0x9A, 0xE9, 0xE8, 0xC2, 0x2F, 0x38, 0x15, 0xF5, 0x21, 0x07, 0xF8, 0x37,
        0x90, 0x5D, 0x2E, 0x02, 0x40, 0x24, 0x15, 0x0D, 0x25, 0xB7, 0x26, 0x5D, 0x09, 0xCC, 0x4C, 0xF4,
        0xF2, 0x1B, 0x94, 0x70, 0x5A, 0x9E, 0xEE, 0xED, 0x77, 0x77, 0xD4, 0x51, 0x99, 0xF5, 0xDC, 0x76,
        0x1E, 0xE3, 0x6C, 0x8C, 0xD1, 0x12, 0xD4, 0x57, 0xD1, 0xB6, 0x83, 0xE4, 0xE4, 0xFE, 0xDA, 0xE9,
        0xB4, 0x3B, 0x33, 0xE5, 0x37, 0x8A, 0xDF, 0xB5, 0x7F, 0x89, 0xF1, 0x9B, 0x9E, 0xB0, 0x15, 0xB2,
        0x3A, 0xFE, 0xEA, 0x61, 0x84, 0x5B, 0x7D, 0x4B, 0x23, 0x12, 0x0B, 0x83, 0x12, 0xF2, 0x22, 0x6B,
        0xB9, 0x22, 0x96, 0x4B, 0x26, 0x0B, 0x63, 0x5E, 0x96, 0x57, 0x52, 0xA3, 0x67, 0x64, 0x22, 0xCA,
        0xD0, 0x56, 0x3E, 0x74, 0xB5, 0x98, 0x1F, 0x0D, 0xF8, 0xB3, 0x34, 0xE6, 0x98, 0x68, 0x5A, 0xAD,
    },
};

// ---- RSA-2048 public operation: 64 limbs of 32 bits, little-endian, Montgomery multiplication
#define LIMBS 64
typedef struct { uint32_t v[LIMBS]; } Big;

static void big_from_be(Big *x, const uint8_t b[256]) {
    for (int i = 0; i < LIMBS; i++) {
        const uint8_t *p = b + 256 - 4 * (i + 1);
        x->v[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
}

static void big_to_be(const Big *x, uint8_t b[256]) {
    for (int i = 0; i < LIMBS; i++) {
        uint8_t *p = b + 256 - 4 * (i + 1);
        p[0] = (uint8_t)(x->v[i] >> 24);
        p[1] = (uint8_t)(x->v[i] >> 16);
        p[2] = (uint8_t)(x->v[i] >> 8);
        p[3] = (uint8_t)x->v[i];
    }
}

static int big_cmp(const Big *a, const Big *b) {
    for (int i = LIMBS - 1; i >= 0; i--)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}

static void big_sub(uint32_t *a, const uint32_t *b) {  // a -= b over LIMBS limbs (the borrow out is dropped)
    uint64_t borrow = 0;
    for (int i = 0; i < LIMBS; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
}

// r = a * b / 2^2048 mod n (CIOS); a, b < n
static void mont_mul(Big *r, const Big *a, const Big *b, const Big *n, uint32_t n0inv) {
    uint32_t t[LIMBS + 2] = {0};
    for (int i = 0; i < LIMBS; i++) {
        uint64_t c = 0;
        for (int j = 0; j < LIMBS; j++) {
            uint64_t x = (uint64_t)t[j] + (uint64_t)a->v[j] * b->v[i] + c;
            t[j] = (uint32_t)x;
            c = x >> 32;
        }
        uint64_t x = (uint64_t)t[LIMBS] + c;
        t[LIMBS] = (uint32_t)x;
        t[LIMBS + 1] = (uint32_t)(x >> 32);
        uint32_t m = t[0] * n0inv;
        x = (uint64_t)t[0] + (uint64_t)m * n->v[0];
        c = x >> 32;
        for (int j = 1; j < LIMBS; j++) {
            x = (uint64_t)t[j] + (uint64_t)m * n->v[j] + c;
            t[j - 1] = (uint32_t)x;
            c = x >> 32;
        }
        x = (uint64_t)t[LIMBS] + c;
        t[LIMBS - 1] = (uint32_t)x;
        t[LIMBS] = t[LIMBS + 1] + (uint32_t)(x >> 32);
        t[LIMBS + 1] = 0;
    }
    Big out;
    memcpy(out.v, t, sizeof(out.v));
    if (t[LIMBS] || big_cmp(&out, n) >= 0) big_sub(out.v, n->v);
    *r = out;
}

// m = s^65537 mod n
static bool rsa_public(const uint8_t modulus[256], const uint8_t signature[256], uint8_t out[256]) {
    Big n, s, r2, one = {{0}}, a, sm;
    big_from_be(&n, modulus);
    big_from_be(&s, signature);
    if (!(n.v[0] & 1) || !n.v[LIMBS - 1] || big_cmp(&s, &n) >= 0) return false;
    uint32_t inv = n.v[0];                      // n^-1 mod 2^32 by Newton's iteration
    for (int i = 0; i < 5; i++) inv *= 2 - n.v[0] * inv;
    uint32_t n0inv = (uint32_t)0 - inv;
    // r2 = 2^4096 mod n, by doubling
    memset(&r2, 0, sizeof(r2));
    r2.v[0] = 1;
    for (int k = 0; k < 2 * 32 * LIMBS; k++) {
        uint32_t carry = r2.v[LIMBS - 1] >> 31;
        for (int i = LIMBS - 1; i > 0; i--) r2.v[i] = r2.v[i] << 1 | r2.v[i - 1] >> 31;
        r2.v[0] <<= 1;
        if (carry || big_cmp(&r2, &n) >= 0) big_sub(r2.v, n.v);
    }
    mont_mul(&sm, &s, &r2, &n, n0inv);          // s * R mod n
    a = sm;
    for (int i = 0; i < 16; i++) mont_mul(&a, &a, &a, &n, n0inv);
    mont_mul(&a, &a, &sm, &n, n0inv);           // s^65537 * R
    one.v[0] = 1;
    mont_mul(&a, &a, &one, &n, n0inv);          // s^65537
    big_to_be(&a, out);
    return true;
}

static void mgf1_sha256(const uint8_t *seed, size_t seed_len, uint8_t *out, size_t n) {
    uint8_t block[32], counter[4];
    for (uint32_t c = 0, done = 0; done < n; c++) {
        counter[0] = (uint8_t)(c >> 24);
        counter[1] = (uint8_t)(c >> 16);
        counter[2] = (uint8_t)(c >> 8);
        counter[3] = (uint8_t)c;
        Sha256 ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, seed, seed_len);
        sha256_update(&ctx, counter, 4);
        sha256_final(&ctx, block);
        for (int i = 0; i < 32 && done < n; i++) out[done++] ^= block[i];
    }
}

bool rsa2048_pss_sha256_verify(const uint8_t modulus[256], const uint8_t signature[256], const uint8_t *msg, size_t n) {
    uint8_t mhash[32];
    sha256(msg, n, mhash);
    return rsa2048_pss_sha256_verify_digest(modulus, signature, mhash);
}

bool rsa2048_pss_sha256_verify_digest(const uint8_t modulus[256], const uint8_t signature[256], const uint8_t mhash[32]) {
    uint8_t em[256];
    if (!rsa_public(modulus, signature, em)) return false;
    // emBits = 2047: one leading zero bit, then maskedDB (223 bytes), H (32 bytes), 0xBC
    if (em[255] != 0xbc || (em[0] & 0x80)) return false;
    const uint8_t *h = em + 223;
    uint8_t db[223];
    memcpy(db, em, sizeof(db));
    mgf1_sha256(h, 32, db, sizeof(db));
    db[0] &= 0x7f;
    size_t i = 0;
    while (i < sizeof(db) && !db[i]) i++;
    if (i == sizeof(db) || db[i] != 1) return false;
    const uint8_t *salt = db + i + 1;
    size_t salt_len = sizeof(db) - i - 1;
    uint8_t check[32];
    static const uint8_t zero[8] = {0};
    Sha256 ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, zero, 8);
    sha256_update(&ctx, mhash, 32);
    sha256_update(&ctx, salt, salt_len);
    sha256_final(&ctx, check);
    return !memcmp(check, h, 32);
}

bool nca_header_signed(const uint8_t *header) {
    unsigned gen = header[0x221];
    if (gen >= 2) return false;
    if (rsa2048_pss_sha256_verify(NCA_HEADER_MODULI[gen], header, header + 0x200, 0x200)) return true;
    uint8_t flipped[0x200];   // a cartridge dump converted to .nsp: only the distribution flag differs
    memcpy(flipped, header + 0x200, sizeof(flipped));
    flipped[4] ^= 1;
    return rsa2048_pss_sha256_verify(NCA_HEADER_MODULI[gen], header, flipped, sizeof(flipped));
}

// ---- RomFS file names
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

typedef struct {
    const uint8_t *dirs, *files;
    uint64_t dirs_size, files_size;
    char *out;
    size_t used, cap;
    int count, steps;
} Walk;

static bool add_name(Walk *w, const char *prefix, const uint8_t *name, uint32_t len) {
    size_t need = strlen(prefix) + len + 2;
    if (w->used + need + 1 > w->cap) {
        size_t cap = (w->cap + need) * 2;
        char *o = realloc(w->out, cap);
        if (!o) return false;
        w->out = o;
        w->cap = cap;
    }
    w->used += (size_t)sprintf(w->out + w->used, "%s%.*s\n", prefix, (int)len, (const char *)name);
    w->count++;
    return true;
}

static bool walk_dir(Walk *w, uint32_t dir, const char *prefix, int depth) {
    if (depth > 32 || (uint64_t)dir + 0x18 > w->dirs_size) return false;
    const uint8_t *d = w->dirs + dir;
    for (uint32_t f = rd32(d + 0xc); f != 0xffffffffu; ) {
        if (++w->steps > 100000 || (uint64_t)f + 0x20 > w->files_size) return false;
        const uint8_t *e = w->files + f;
        uint32_t len = rd32(e + 0x1c);
        if ((uint64_t)f + 0x20 + len > w->files_size || !add_name(w, prefix, e + 0x20, len)) return false;
        f = rd32(e + 4);
    }
    for (uint32_t c = rd32(d + 8); c != 0xffffffffu; ) {
        if (++w->steps > 100000 || (uint64_t)c + 0x18 > w->dirs_size) return false;
        const uint8_t *e = w->dirs + c;
        uint32_t len = rd32(e + 0x14);
        if ((uint64_t)c + 0x18 + len > w->dirs_size || len > 255 || strlen(prefix) + len + 2 > 512) return false;
        char sub[512];
        snprintf(sub, sizeof(sub), "%s%.*s/", prefix, (int)len, (const char *)(e + 0x18));
        if (!walk_dir(w, c, sub, depth + 1)) return false;
        c = rd32(e + 4);
    }
    return true;
}

char *romfs_names(const uint8_t *image, size_t size, int *count) {
    *count = 0;
    if (size < 0x50 || rd64(image) != 0x50) return NULL;
    uint64_t doff = rd64(image + 0x18), dsize = rd64(image + 0x20), foff = rd64(image + 0x38), fsize = rd64(image + 0x40);
    if (doff > size || dsize > size - doff || foff > size || fsize > size - foff) return NULL;
    Walk w = {image + doff, image + foff, dsize, fsize, malloc(64), 1, 64, 0, 0};
    if (!w.out) return NULL;
    w.out[0] = '\n';
    w.out[1] = 0;
    if (!walk_dir(&w, 0, "", 0)) {
        free(w.out);
        return NULL;
    }
    *count = w.count;
    return w.out;
}

// ---- add-on content
static bool has_name(const char *names, const char *line) {   // line = "\npath\n"
    return strstr(names, line) != NULL;
}

static DlcVerdict judge(const DlcPack *pack, const DlcPack *others, int n, char *msg, size_t msgn);

DlcVerdict dlc_judge(const DlcPack *pack, const DlcPack *others, int n, char *msg, size_t msgn) {
    DlcVerdict v = judge(pack, others, n, msg, msgn);
    if (msg[0] >= 'a' && msg[0] <= 'z') msg[0] = (char)(msg[0] - 32);   // a sentence ("Add-on content 5 is ...")
    return v;
}

static DlcVerdict judge(const DlcPack *pack, const DlcPack *others, int n, char *msg, size_t msgn) {
    msg[0] = 0;
    if (pack->is_signed != 0) return DLC_OK;   // signed, or not known: nothing to say
    int count = 0;
    char *mine = romfs_names(pack->image, pack->size, &count);
    if (!mine) {
        snprintf(msg, msgn, "%s is a bad dump: it isn't signed by Nintendo and can't be read. Get a good dump of it.",
                 pack->name);
        return DLC_REFUSE;
    }
    if (!count) {
        free(mine);
        snprintf(msg, msgn, "%s is a bad dump: it isn't signed by Nintendo and has no files. Get a good dump of it.",
                 pack->name);
        return DLC_REFUSE;
    }
    // the files every original pack of the game holds
    char *common = NULL;
    for (int k = 0; k < n; k++) {
        if (others[k].is_signed != 1) continue;
        int c = 0;
        char *theirs = romfs_names(others[k].image, others[k].size, &c);
        if (!theirs) continue;
        if (!common) { common = theirs; continue; }
        char *keep = malloc(strlen(common) + 1);   // common ∩ theirs
        if (!keep) { free(theirs); break; }
        size_t used = 0;
        keep[used++] = '\n';
        for (const char *p = common + 1; *p;) {
            const char *nl = strchr(p, '\n');
            if (!nl) break;
            char line[520];
            int len = (int)(nl - p);
            if (len < (int)sizeof(line) - 2) {
                snprintf(line, sizeof(line), "\n%.*s\n", len, p);
                if (has_name(theirs, line)) {
                    memcpy(keep + used, p, (size_t)len + 1);
                    used += (size_t)len + 1;
                }
            }
            p = nl + 1;
        }
        keep[used] = 0;
        free(common);
        free(theirs);
        common = keep;
    }
    char missing[300] = "";
    int nmissing = 0;
    for (const char *p = common ? common + 1 : ""; *p;) {
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        char line[520];
        int len = (int)(nl - p);
        if (len < (int)sizeof(line) - 2) {
            snprintf(line, sizeof(line), "\n%.*s\n", len, p);
            if (!has_name(mine, line)) {
                if (nmissing < 4)
                    snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s%.*s",
                             nmissing ? ", " : "", len, p);
                nmissing++;
            }
        }
        p = nl + 1;
    }
    free(common);
    free(mine);
    if (nmissing) {
        snprintf(msg, msgn, "%s is a bad dump: it isn't signed by Nintendo and is missing %s%s, which the game needs. "
                 "It would crash the game. Get a good dump of it.", pack->name, missing, nmissing > 4 ? " (and more)" : "");
        return DLC_REFUSE;
    }
    snprintf(msg, msgn, "%s isn't signed by Nintendo, so it may be a rebuilt or fake dump. If the game crashes, "
             "remove the DLC again.", pack->name);
    return DLC_WARN;
}
