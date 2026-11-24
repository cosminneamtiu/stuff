// SPDX-License-Identifier: GPL-2.0-only
// Add-on content (DLC) for a game in the SeedSwap library, on the console: the same result as the
// PC's prepare_game.py + push_prepared.py (DLC.md section 0), from the user's DLC .nsp on the SD card.
//
// Adding: the dump is read and checked (aoc: belongs to the game, common ticket, game version new
// enough), the game's code images get the new sdk bodies (exefs.nsp and, when kept, the image
// without Nintendo's patches), each written beside the old one and read back; then the DLC's RomFS
// image is appended to romfs.bin and 72 header bytes are rewritten, both read back; only then are
// the images swapped in. The image without DLC is kept as exefs-nodlc.nsp (with compat-nodlc.ini,
// exefs-nocompat-nodlc.nsp, compat-nodlc.json), the same names push_prepared.py uses.
// Removing puts those back and restores romfs.bin's old header; the appended tail stays, unused.
#pragma once
#include "util.h"

#define DLC_KEPT_IMAGE "exefs-nodlc.nsp"

typedef struct {
    char title_id[17];      // the DLC's ID
    char name[96];          // "Booster Course Pass" or "add-on content 1"
    int index;
    uint32_t version, required_version;
    int64_t tail_bytes;     // appended to romfs.bin
    int changed;            // sdk instructions changed
    bool first;             // the first DLC of this game (the image without DLC was kept just now)
    char warning[420];      // built in, but not an original dump (dumpcheck.h); "" = nothing to say
} DlcResult;

// A short name for a DLC ID, from SeedSwap's own table; "" when unknown.
const char *dlc_known_name(uint64_t title_id);
// folder: where the game's exefs.nsp and romfs.bin are now (the overlay folder or its own folder);
// game_dir: SD:/seedswap/games/<id>. siblings: other DLC dumps on the card (any game; the ones of this
// game are read to compare with when the new dump is not an original, see dumpcheck.h). A dump that is
// not an original and lacks files the game's original packs hold is refused.
bool dlc_add(const char *nsp_path, const char *keys_path, const char *folder, const char *game_dir, uint64_t program_id,
             int64_t version_code, const char *const *siblings, int nsiblings, ProgressFn cb, DlcResult *out, char *err,
             size_t errn);
bool dlc_remove(const char *folder, const char *game_dir, char *err, size_t errn);
