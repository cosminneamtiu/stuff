// SPDX-License-Identifier: GPL-2.0-only
// The on-console converter (DESIGN.md S5 and S6): the user's dumps (.nsp, .nsz, .xci, .xcz) of a
// base game, optionally with its update, become a SeedSwap library game, the same as the PC's
// prepare_game.py + push_prepared.py make:
//   1. the dumps are read and checked (complete, tickets usable, the update belongs to the game);
//   2. the program's code (the update's when there is one) gets the host's signed NPDM with the game's
//      process fields, and the known per-game fixes (fixes.c), into a 32 MiB exefs.nsp;
//   3. the game data (base, or base patched by the update) is written to romfs.bin, every block
//      checked against the game's hash tree when read back;
//   4. both are written beside whatever is there as *.part, read back, and only then put in place;
//   5. name, version, icon and save size go into game.ini; Nintendo's patches are applied when the
//      patcher file is on the card; add-on content the game had is built in again from its dumps.
// An update replaces the library entry's files in place (DESIGN.md "Updates"): one entry per game,
// newer or the same version always, an older one only when asked; the save stays.
#pragma once
#include "model.h"
#include "pkg.h"

#define CONVERT_MAX_PROGRAMS 8

typedef struct {
    int index;                  // the program's number in its title
    uint64_t program_id;
    char name[128];             // for the library (a collection's programs get their own names)
    char note[160];             // what is known about it ("needs JIT ...")
    bool with_update;
    uint64_t romfs_size;
    int library;                // the library entry of this program, -1 = new
    VersionRelation relation;   // to that entry's version
} ConvertProgram;

typedef struct {
    char base_path[320], update_path[320];  // update_path "" = none; == base_path when one dump holds both
    char base_file[128];
    uint64_t app_id;
    uint32_t base_version, update_version;  // version numbers (0, 65536, ...)
    bool has_update;
    char display_version[32];
    char base_kind[8], update_kind[8];      // ".nsp", ".nsz", ...
    ConvertProgram programs[CONVERT_MAX_PROGRAMS];
    int count;
    int64_t sd_free;
} ConvertPlan;

typedef struct {
    char id[24], name[128], version[32];
    int64_t version_code;
    uint64_t romfs_size;
    int fixes_applied, fixes_failed;
    char fixes[200];            // what was applied, in words
    char compat[160];           // what happened about Nintendo's patches
    char dlc[200];              // add-on content built in again (or why not)
    char warnings[600];         // permission problems and other notes
    bool replaced;              // an existing library entry was updated
    char replaced_version[32];
    int seconds;
} ConvertResult;

// Finds the base game's dump and (optionally) its update's, and what can be made of them. Cheap:
// headers, content lists and names only. update_path may be NULL or "".
bool convert_plan(const Library *L, const char *base_path, const char *update_path, const char *keys_path,
                  ConvertPlan *plan, char *err, size_t errn);
// Why a program cannot be converted now (NULL when it can). allow_older: the user confirmed a downgrade.
const char *convert_blocker(const Library *L, const ConvertPlan *plan, int program, bool allow_older);
// Converts one program of the plan (by its position in plan->programs) into the library.
bool convert_run(Library *L, const ConvertPlan *plan, int program, bool allow_older, const char *keys_path, ProgressFn cb,
                 ConvertResult *res, char *err, size_t errn);
