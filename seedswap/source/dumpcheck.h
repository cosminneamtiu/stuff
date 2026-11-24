// SPDX-License-Identifier: GPL-2.0-only
// Tells an original dump from a rebuilt or broken one (DESIGN.md, "Bad dumps").
//  * nca_header_signed: Nintendo signs every NCA header (signature 0: RSA-2048-PSS over SHA-256 with
//    one of two fixed public keys). A package built or rebuilt by a tool (also a ticketless
//    conversion) cannot carry that signature, so headers that fail it were not made by Nintendo.
//  * dlc_judge: add-on content whose headers are not signed and that lacks a file every original
//    (signed) pack of the same game holds is refused: the game opens that file and closes when it is
//    missing (Cult of the Lamb's Pilgrim Pack dump held no data.txt). Unsigned packs that do have
//    those files are allowed with a warning.
// The same rules are in pc/dump_check.py. Plain C with no platform calls, so the PC tests run it too.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RSA-2048 with e = 65537, EMSA-PSS with SHA-256 and MGF1-SHA-256 (any salt length).
bool rsa2048_pss_sha256_verify(const uint8_t modulus[256], const uint8_t signature[256], const uint8_t *msg, size_t n);
// The same over a SHA-256 the caller already has (the code verification data Loader checks).
bool rsa2048_pss_sha256_verify_digest(const uint8_t modulus[256], const uint8_t signature[256], const uint8_t mhash[32]);
// header: the first 0xC00 bytes of an NCA, decrypted. Cartridge dumps (distribution flag flipped
// when converted to .nsp) count as signed.
bool nca_header_signed(const uint8_t *header);

// The files of a RomFS image held in memory, as "\n" + "path\n" per file ("\ndata.txt\n"), paths
// without the leading "/". malloc'd; NULL when the image is not a readable RomFS. *count = files.
char *romfs_names(const uint8_t *image, size_t size, int *count);

typedef struct {
    const char *name;       // what the user sees ("add-on content 5")
    const uint8_t *image;   // its RomFS image
    size_t size;
    int is_signed;          // 1: Nintendo's signature checks out, 0: it does not, -1: not known (older record)
} DlcPack;

typedef enum { DLC_OK, DLC_WARN, DLC_REFUSE } DlcVerdict;
// Judges `pack` against the same game's other add-on content (`others`: built in already, or given
// together). DLC_WARN and DLC_REFUSE come with a sentence in msg.
DlcVerdict dlc_judge(const DlcPack *pack, const DlcPack *others, int n, char *msg, size_t msgn);
