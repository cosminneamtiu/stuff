// SPDX-License-Identifier: GPL-2.0-only
// AES-128 (ECB, CTR, Nintendo's XTS) and prod.keys lookup, for reading the user's own dumps.
// Plain C with no platform calls: the PC tests run the same code as the console.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t enc[44], dec[44];
} Aes128;

void aes128_init(Aes128 *ctx, const uint8_t key[16]);
void aes128_encrypt_block(const Aes128 *ctx, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt_block(const Aes128 *ctx, const uint8_t in[16], uint8_t out[16]);

// n must be a multiple of 16
void aes128_ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n);
// counter: 16 bytes, big-endian, incremented per block; `skip` bytes of the first block's key
// stream are dropped (data that does not start on a block boundary)
void aes128_ctr(const Aes128 *ctx, const uint8_t counter[16], size_t skip, const uint8_t *in, uint8_t *out, size_t n);
// Nintendo's XTS: 0x200-byte sectors, the sector number as a big-endian 16-byte tweak. key: 32 bytes
// (data key, then tweak key), as header_key in prod.keys. n: a multiple of 0x200.
void aes128_xts_decrypt(const uint8_t key[32], uint64_t first_sector, const uint8_t *in, uint8_t *out, size_t n);

// One key from the text of a prod.keys file ("name = hex"). False when it is missing or not len bytes.
bool keys_get(const char *text, const char *name, uint8_t *out, size_t len);

// ---- bulk work (whole games): the console's AES and SHA instructions through libnx; in the PC
// preview OpenSSL when built with SEEDSWAP_OPENSSL, else the plain C code above.
// AES-128-CTR over n bytes; counter as for aes128_ctr. Encrypting and decrypting are the same.
void ctr_crypt(const uint8_t key[16], const uint8_t counter[16], size_t skip, const uint8_t *in, uint8_t *out, size_t n);
// The counter Nintendo's NCA sections use: the 64-bit section value big-endian, then the byte offset
// within the NCA divided by 16, big-endian.
void nca_counter(uint64_t upper, uint64_t nca_offset, uint8_t out[16]);

typedef struct {
    uint64_t opaque[32];
} FastSha256;
void fsha256_init(FastSha256 *ctx);
void fsha256_update(FastSha256 *ctx, const void *data, size_t n);
void fsha256_final(FastSha256 *ctx, uint8_t out[32]);
void fsha256(const void *data, size_t n, uint8_t out[32]);
