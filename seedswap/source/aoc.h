// SPDX-License-Identifier: GPL-2.0-only
// Building add-on content (DLC) into a hosted game (DLC.md, option B), the C port of the PC tools'
// aoc_shim.py and romfs_append.py:
//   * four functions of the game's own sdk get new bodies: CountAddOnContent / ListAddOnContent
//     return the numbers built in; QueryMountAddOnContentCacheSize / MountAddOnContent open
//     "<the game's RomFS mount>:/__aoc/NNNN.romfs" and hand it to MountRomOnFile;
//   * the DLC's RomFS image is appended to the game's romfs.bin as /__aoc/NNNN.romfs, followed by
//     new copies of the file tables; 72 header bytes are rewritten to point at them.
// Plain C with no platform calls, so the PC tests run the same code.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compat/nsofile.h"

#define AOC_DIRECTORY "__aoc"
#define AOC_MAX_INDEX 9999
#define AOC_MAX_ADD_ONS 16
#define AOC_NOT_FOUND 0x7D402u     // nn::fs::ResultTargetNotFound

enum { AOC_COUNT, AOC_LIST, AOC_QUERY, AOC_MOUNT, AOC_TARGETS };
enum { AOC_OPEN, AOC_CLOSE, AOC_MOUNT_FILE, AOC_QUERY_FILE, AOC_CALLEES };

typedef struct {
    uint32_t target[AOC_TARGETS], room[AOC_TARGETS];   // the replaced functions in sdk: offset, size
    uint32_t callee[AOC_CALLEES];                      // what the new bodies call, in sdk
    char mount_name[32];                               // the game's RomFS mount name (from main)
} AocSites;

// Whether main asks for add-on content at all (imports one of the four functions).
bool aoc_main_uses(const NsoImage *main);
// Finds everything in the game's sdk and main. False with a sentence in err when the game does not fit.
bool aoc_locate(const NsoImage *sdk, const NsoImage *main, AocSites *out, char *err, size_t errn);
// Writes the four bodies into sdk->image (text). Returns the number of changed instructions, or -1.
int aoc_apply(NsoImage *sdk, const AocSites *s, const int *indices, int count, char *err, size_t errn);
// Runs the four bodies of a patched sdk in the interpreter (compat/a64.c) with the called functions
// replaced by recorders, and compares what they do with what they should (the same checks as the PC
// tools run in Unicorn). That each /__aoc/NNNN.romfs is really in the RomFS is checked separately.
bool aoc_check(const NsoImage *sdk, const AocSites *s, const int *indices, int count, char *err, size_t errn);
void aoc_image_name(int index, char out[16]);   // "0001.romfs"

// ---- the RomFS append
typedef int (*RomfsReadFn)(void *user, uint64_t off, void *buf, size_t n);  // 0 = read all n bytes

typedef struct {
    const char *name;          // a plain file name in /__aoc
    const uint8_t *data;
    size_t size;
} RomfsNewFile;

typedef struct {
    uint8_t old_header[0x50], new_header[0x50];
    uint64_t base_size;        // the image the tables belong to (its old end)
    uint64_t append_at;        // where the tail goes (normally base_size; later when a tail is replaced)
    uint64_t tables_at, new_size;
    uint8_t *tail;             // malloc'd
    size_t tail_size;
    uint64_t file_offset[AOC_MAX_ADD_ONS];  // where each new file lies in the image
} RomfsPlan;

// Plans an append of `files` in a new top-level directory to an image whose header is `header` and
// whose tables are read through `read`. The tail is placed at append_at (>= the end of the tables).
bool romfs_plan(RomfsReadFn read, void *user, const uint8_t header[0x50], uint64_t append_at, const char *directory,
                const RomfsNewFile *files, int count, RomfsPlan *out, char *err, size_t errn);
void romfs_plan_free(RomfsPlan *p);
// Looks a file up through the hash tables of an image: true and (offset in the image, size).
bool romfs_find(RomfsReadFn read, void *user, const uint8_t header[0x50], const char *path, uint64_t *offset,
                uint64_t *size);
// The number of files in an image, by reading the file records in order (-1 when unreadable).
long romfs_count_files(RomfsReadFn read, void *user, const uint8_t header[0x50]);
