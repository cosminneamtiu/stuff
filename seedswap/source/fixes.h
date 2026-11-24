// SPDX-License-Identifier: GPL-2.0-only
// Per-game fixes a hosted game needs to run as Seedsow on Switch 2: the C port of the PC's
// pc/fixes/ (mk8_devicesave.py, smo_pretouch.py). A fix finds its sites by the shape of the code, so a
// build it has not seen is still patched when the same code is there, and reports when it is not.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compat/nsofile.h"

#define FIX_MAX_CHANGES 8

typedef struct {
    char module[16];
    uint32_t addr, old_word, new_word;
    char what[120];
} FixChange;

enum { FIX_APPLIED, FIX_NOT_PLACED };

typedef struct {
    char id[32], title[120];
    int status;
    char reason[220];            // why it was not placed
    FixChange changes[FIX_MAX_CHANGES];
    int count;
    uint32_t cave_start, cave_size;   // code added after main's text (0 = none)
    char build[33];              // main's build ID (first 16 bytes, hex)
    char build_note[160];        // what is known about this build ("" = not seen before)
    char note[200];
} FixRecord;

typedef struct {
    char name[32];
    NsoImage *nso;               // loaded; a fix changes nso->image (and main's text size) in place
} FixModule;

// Applies every fix for the program to the modules (main, sdk, and the others for symbol lookups).
// Returns the number of records written to out (fixes known for the program, applied or not).
// A module a fix changed is marked in *changed (bit i = modules[i]).
int fixes_apply(uint64_t program_id, FixModule *modules, int count, FixRecord *out, int max, unsigned *changed);
// Where a module defines a symbol (its offset), -1 when it does not (compat_words.c).
long long fixes_defined(const NsoImage *nso, const char *name);
// Whether main imports a symbol (for the permission sentences).
bool fixes_imports(const NsoImage *nso, const char *name);
