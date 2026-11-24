// SPDX-License-Identifier: GPL-2.0-only
// Nintendo's per-game settings for hosted games: the C port of pc/fixes/compat_words.py.
//
// On Switch 2, ns gives every Switch 1 game the record for its application ID from
// ApplicationCompatibilityInfo.json (system data 0400000000000848): application_layer_info
// {graphics,movie,audio,misc}_config, four 64-bit words in the block rtld's argument page points to
// (+0xb8, +0xc0, +0xc8, +0xd0). A hosted game runs as Seedsow, which has no record, so they are 0.
// The fix adds code to the sdk that ORs the game's own words into the block at the start of the sdk's
// nnosInitialize (x1 = the argument page), before nnCompatThin or the game reads them. It must build
// the same code as the PC tool (preview/compat_words_test.sh compares them).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fixes.h"

#define CW_FIELDS 4
#define CW_ID "compat-words"
#define CW_DATA_ID 0x0400000000000848ull

typedef struct {
    uint64_t id;               // application_id
    bool id_ok;
    uint64_t limit;            // target_max_version (0xffffffff when missing)
    bool limit_ok;             // false: negative, never applies
    uint64_t words[CW_FIELDS]; // graphics, movie, audio, misc (0 = not set)
} CwRecord;

typedef struct {
    CwRecord *records;
    int count;
} CwInfo;

typedef struct {
    uint64_t record_id;          // the ID the record is filed under (the program's or its application's)
    uint64_t words[CW_FIELDS];
} CompatWords;

enum { CW_UNAVAILABLE = 0, CW_UNREADABLE = -1, CW_NONE = 1, CW_FOUND = 2 };

// Parses the JSON, or a RomFS image that holds it (system data 0400000000000848 as stored).
bool cw_parse(const uint8_t *data, size_t size, CwInfo *out, char *err, size_t errn);
void cw_free(CwInfo *info);
// The record for the program, else for its application, while version <= its target_max_version;
// false when there is none or its application-layer words are all 0.
bool cw_lookup(const CwInfo *info, uint64_t program_id, uint64_t app_id, uint64_t version, CompatWords *out);
// The words for a game from the console's own system data (read once per run). CW_FOUND: *out is set;
// CW_NONE: no record (why says so); CW_UNREADABLE: the data could not be read (why); CW_UNAVAILABLE:
// there is no system data here (the PC preview without SEEDSWAP_COMPAT_INFO).
int cw_for_game(uint64_t program_id, uint64_t app_id, uint64_t version, CompatWords *out, char *why, size_t whyn);
// Adds the code to the sdk (FixModule "sdk"), as pc/fixes/compat_words.py apply(). Fills r (applied or
// not placed); sets the sdk's bit in *changed when applied.
bool cw_apply(FixModule *mods, int count, const CompatWords *w, FixRecord *r, unsigned *changed);
// "graphics 0x2, misc 0x2020"
void cw_describe(const uint64_t words[CW_FIELDS], char *out, size_t n);
// game.ini compat_words: "0x2,0x0,0x0,0x0"
void cw_ini_value(const uint64_t words[CW_FIELDS], char *out, size_t n);
bool cw_ini_parse(const char *text, uint64_t words[CW_FIELDS]);
