// SPDX-License-Identifier: GPL-2.0-only
#include "crypto.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static uint8_t INV[256];
static bool inv_ready;

static uint8_t xt(uint8_t x) { return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }
static uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static uint32_t sub_word(uint32_t w) {
    return (uint32_t)SBOX[w >> 24] << 24 | (uint32_t)SBOX[(w >> 16) & 255] << 16 | (uint32_t)SBOX[(w >> 8) & 255] << 8 |
           SBOX[w & 255];
}

void aes128_init(Aes128 *ctx, const uint8_t key[16]) {
    if (!inv_ready) {
        for (int i = 0; i < 256; i++) INV[SBOX[i]] = (uint8_t)i;
        inv_ready = true;
    }
    uint32_t *w = ctx->enc;
    for (int i = 0; i < 4; i++)
        w[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            t = sub_word(t << 8 | t >> 24) ^ (uint32_t)rcon << 24;
            rcon = xt(rcon);
        }
        w[i] = w[i - 4] ^ t;
    }
    // the decryption schedule is the encryption one in reverse round order (straight inverse cipher)
    for (int r = 0; r <= 10; r++)
        for (int c = 0; c < 4; c++) ctx->dec[4 * r + c] = w[4 * (10 - r) + c];
}

static void add_key(uint8_t s[16], const uint32_t *k) {
    for (int c = 0; c < 4; c++) {
        s[4 * c] ^= (uint8_t)(k[c] >> 24);
        s[4 * c + 1] ^= (uint8_t)(k[c] >> 16);
        s[4 * c + 2] ^= (uint8_t)(k[c] >> 8);
        s[4 * c + 3] ^= (uint8_t)k[c];
    }
}

void aes128_encrypt_block(const Aes128 *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_key(s, ctx->enc);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
        for (int c = 0; c < 4; c++)  // shift rows: row r of column c comes from column c + r
            for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c] = xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3;
                s[4 * c + 1] = a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3;
                s[4 * c + 2] = a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3);
                s[4 * c + 3] = (xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3);
            }
        } else {
            memcpy(s, t, 16);
        }
        add_key(s, ctx->enc + 4 * round);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt_block(const Aes128 *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_key(s, ctx->dec);
    for (int round = 1; round <= 10; round++) {
        for (int c = 0; c < 4; c++)  // inverse shift rows
            for (int r = 0; r < 4; r++) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
        for (int i = 0; i < 16; i++) t[i] = INV[t[i]];
        memcpy(s, t, 16);
        add_key(s, ctx->dec + 4 * round);
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
                s[4 * c + 1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
                s[4 * c + 2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
                s[4 * c + 3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
            }
        }
    }
    memcpy(out, s, 16);
}

void aes128_ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n) {
    Aes128 ctx;
    aes128_init(&ctx, key);
    for (size_t i = 0; i + 16 <= n; i += 16) aes128_decrypt_block(&ctx, in + i, out + i);
    memset(&ctx, 0, sizeof(ctx));
}

void aes128_ctr(const Aes128 *ctx, const uint8_t counter[16], size_t skip, const uint8_t *in, uint8_t *out, size_t n) {
    uint8_t c[16], ks[16];
    memcpy(c, counter, 16);
    size_t at = 0;
    while (at < n) {
        aes128_encrypt_block(ctx, c, ks);
        for (size_t i = skip; i < 16 && at < n; i++, at++) out[at] = in[at] ^ ks[i];
        skip = 0;
        for (int i = 15; i >= 0 && ++c[i] == 0; i--) {
        }
    }
}

void aes128_xts_decrypt(const uint8_t key[32], uint64_t first_sector, const uint8_t *in, uint8_t *out, size_t n) {
    Aes128 data, tweak;
    aes128_init(&data, key);
    aes128_init(&tweak, key + 16);
    for (size_t sector = 0; sector * 0x200 < n; sector++) {
        uint8_t t[16] = {0}, b[16];
        uint64_t number = first_sector + sector;
        for (int i = 0; i < 8; i++) t[15 - i] = (uint8_t)(number >> (8 * i));
        aes128_encrypt_block(&tweak, t, t);
        for (size_t j = 0; j < 0x200; j += 16) {
            const uint8_t *src = in + sector * 0x200 + j;
            uint8_t *dst = out + sector * 0x200 + j;
            for (int i = 0; i < 16; i++) b[i] = src[i] ^ t[i];
            aes128_decrypt_block(&data, b, b);
            for (int i = 0; i < 16; i++) dst[i] = b[i] ^ t[i];
            uint8_t carry = 0;  // tweak times x in GF(2^128), little-endian bytes
            for (int i = 0; i < 16; i++) {
                uint8_t next = t[i] >> 7;
                t[i] = (uint8_t)(t[i] << 1 | carry);
                carry = next;
            }
            if (carry) t[0] ^= 0x87;
        }
    }
    memset(&data, 0, sizeof(data));
    memset(&tweak, 0, sizeof(tweak));
}

void nca_counter(uint64_t upper, uint64_t nca_offset, uint8_t out[16]) {
    uint64_t block = nca_offset >> 4;
    for (int k = 0; k < 8; k++) {
        out[7 - k] = (uint8_t)(upper >> (8 * k));
        out[15 - k] = (uint8_t)(block >> (8 * k));
    }
}

#if defined(__SWITCH__)
#include <switch/crypto/aes_ctr.h>
#include <switch/crypto/sha256.h>

void ctr_crypt(const uint8_t key[16], const uint8_t counter[16], size_t skip, const uint8_t *in, uint8_t *out, size_t n) {
    Aes128CtrContext ctx;
    aes128CtrContextCreate(&ctx, key, counter);
    if (skip) {
        uint8_t pad[16] = {0};
        aes128CtrCrypt(&ctx, pad, pad, skip);
    }
    aes128CtrCrypt(&ctx, out, in, n);
    memset(&ctx, 0, sizeof(ctx));
}

_Static_assert(sizeof(Sha256Context) <= sizeof(FastSha256), "FastSha256 too small");
void fsha256_init(FastSha256 *c) { sha256ContextCreate((Sha256Context *)c); }
void fsha256_update(FastSha256 *c, const void *d, size_t n) { sha256ContextUpdate((Sha256Context *)c, d, n); }
void fsha256_final(FastSha256 *c, uint8_t out[32]) { sha256ContextGetHash((Sha256Context *)c, out); }
void fsha256(const void *d, size_t n, uint8_t out[32]) { sha256CalculateHash(out, d, n); }

#else
#include "compat/nsofile.h"

#if defined(SEEDSWAP_OPENSSL)
#include <openssl/evp.h>
void ctr_crypt(const uint8_t key[16], const uint8_t counter[16], size_t skip, const uint8_t *in, uint8_t *out, size_t n) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, key, counter);
    if (skip) {
        uint8_t pad[16] = {0};
        EVP_EncryptUpdate(ctx, pad, &len, pad, (int)skip);
    }
    while (n) {
        int k = n > (1u << 30) ? 1 << 30 : (int)n;
        EVP_EncryptUpdate(ctx, out, &len, in, k);
        in += k;
        out += k;
        n -= (size_t)k;
    }
    EVP_CIPHER_CTX_free(ctx);
}
#else
void ctr_crypt(const uint8_t key[16], const uint8_t counter[16], size_t skip, const uint8_t *in, uint8_t *out, size_t n) {
    Aes128 ctx;
    aes128_init(&ctx, key);
    aes128_ctr(&ctx, counter, skip, in, out, n);
    memset(&ctx, 0, sizeof(ctx));
}
#endif

_Static_assert(sizeof(Sha256) <= sizeof(FastSha256), "FastSha256 too small");
void fsha256_init(FastSha256 *c) { sha256_init((Sha256 *)c); }
void fsha256_update(FastSha256 *c, const void *d, size_t n) { sha256_update((Sha256 *)c, d, n); }
void fsha256_final(FastSha256 *c, uint8_t out[32]) { sha256_final((Sha256 *)c, out); }
void fsha256(const void *d, size_t n, uint8_t out[32]) { sha256(d, n, out); }
#endif

static int hexval(int c) {
    c = tolower(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool keys_get(const char *text, const char *name, uint8_t *out, size_t len) {
    size_t nl = strlen(name);
    for (const char *line = text; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncasecmp(line, name, nl)) continue;
        const char *p = line + nl;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        size_t i = 0;
        for (; i < len; i++) {
            int hi = hexval((unsigned char)p[2 * i]), lo = hi < 0 ? -1 : hexval((unsigned char)p[2 * i + 1]);
            if (hi < 0 || lo < 0) break;
            out[i] = (uint8_t)(hi << 4 | lo);
        }
        if (i == len && hexval((unsigned char)p[2 * len]) < 0) return true;
    }
    return false;
}
