// SPDX-License-Identifier: GPL-2.0-only
// Reads add-on content (DLC) from the user's own .nsp dump: the content list, the ticket, and the
// DLC's RomFS image, decrypted and checked block by block against the dump's own hash tree.
// Plain C with no platform calls (the file is read through a callback), so the PC tests run it too.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 0 when all n bytes at off were read
typedef int (*DlcReadFn)(void *user, uint64_t off, void *buf, size_t n);

#define DLCPKG_MAX_IMAGE (64u * 1024 * 1024)  // add-on content held in memory: unlock-style content only

typedef struct {
    uint64_t title_id;            // the add-on content's ID
    uint64_t application_id;      // the game it belongs to
    uint32_t version;             // its version
    uint32_t required_version;    // the oldest game version it works with
    int key_generation;
    uint8_t *image;               // its RomFS image (malloc'd), as the game would mount it
    size_t image_size;
    char data_nca[40];
    bool is_signed;               // both NCA headers carry Nintendo's signature (dumpcheck.h)
} DlcPackage;

// Returns true and fills out, or false with a plain sentence in err (the dump cannot be used, and why).
bool dlcpkg_read(DlcReadFn read, void *user, uint64_t size, const char *keys_text, DlcPackage *out, char *err,
                 size_t errn);
void dlcpkg_free(DlcPackage *p);
