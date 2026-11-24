// SPDX-License-Identifier: GPL-2.0-only
// Staging one program of a game for a host (Seedsow or another, hosts.h), on the console: the port of the PC's
// sw2_stage_base_overlay.py and sw2_stage_update_overlay2.py.
//   * ExeFS: the game's code modules from its Program NCA (the update's when there is one), with
//     the host's signed main.npdm, into which the game's process fields (META 0x0C-0x20) are copied;
//     a PFS0 image padded to 32 MiB.
//   * RomFS: the game data from the base game, or the base game patched by the update (BKTR:
//     indirect storage + AES-CTR-EX), checked against the game's own hash tree: the hash levels
//     against the master hash before anything is written, every data block when read back.
//   * The game's permissions compared with the host's, in sentences.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pkg.h"
#include "util.h"

#define STAGE_EXEFS_PAD (32u * 1024 * 1024)
// Larger code (Unity/IL2CPP games): the image keeps its own size, rounded up to 1 MiB, plus room for later
// in-place patches (Nintendo's patches, add-on content) to grow a module. Needs the loader's live SD sizes.
#define STAGE_EXEFS_ROOM (4u * 1024 * 1024)
#define STAGE_EXEFS_MAX (512u * 1024 * 1024)
#define STAGE_MAX_FILES 16

typedef struct {
    char name[32];
    uint8_t *data;
    size_t size;
} StageFile;

typedef struct {
    StageFile files[STAGE_MAX_FILES];   // the code modules, without main.npdm
    int count;
    uint8_t *game_npdm;
    size_t game_npdm_size;
} StageExefs;

bool stage_exefs(Nca *program, uint64_t program_id, StageExefs *out, char *err, size_t errn);
void stage_exefs_free(StageExefs *x);
StageFile *stage_file(StageExefs *x, const char *name);

// Seedsow's signed NPDM, whatever process fields an image copied into it.
bool host_npdm_valid(const uint8_t *npdm, size_t n);
// main.npdm for the game: the host's (Seedsow's, or any NPDM whose ACID Nintendo signed) with the game's
// process fields. *out is malloc'd (host_size bytes).
bool stage_npdm(const uint8_t *host, size_t host_size, const uint8_t *game, size_t game_size, uint8_t **out, char *err,
                size_t errn);
// The ExeFS image: the modules and main.npdm sorted by name, padded with zeros to stage_image_size(unpadded):
// 32 MiB, or for larger code its size rounded to 1 MiB plus STAGE_EXEFS_ROOM.
size_t stage_image_size(size_t unpadded);
bool stage_image(const StageExefs *x, const uint8_t *npdm, size_t npdm_size, uint8_t **image, size_t *unpadded,
                 char *err, size_t errn);

enum { POLICY_OK, POLICY_NOTE, POLICY_WARNING, POLICY_BLOCKER };
typedef struct {
    int level;
    char text[320];
} PolicyLine;
// device_save: 0 = main does not mount device save data, 1 = it does, 2 = a fix redirects it.
// host_name: for the sentences ("Seedsow Lullaby").
int stage_policy(const uint8_t *game, size_t game_size, const uint8_t *host, size_t host_size, int device_save,
                 const char *host_name, PolicyLine *out, int max);

typedef struct {
    uint64_t size;             // romfs.bin
    uint8_t sha256[32];
    int levels;
    bool patched;              // base game + update
    uint64_t from_base, from_patch;
    uint64_t restarts;         // times a compressed dump was read again from its start
} RomfsResult;

// Writes the RomFS of base (patched by `patch` when not NULL) to path on the SD card, then reads it
// back and checks every block. On failure (or when cb says stop) the file is deleted.
bool stage_romfs(Nca *base, Nca *patch, const char *path, ProgressFn cb, RomfsResult *out, char *err, size_t errn);
// The size romfs.bin will have, without reading the data (for the free-space check).
bool stage_romfs_size(Nca *base, Nca *patch, uint64_t *size, char *err, size_t errn);
