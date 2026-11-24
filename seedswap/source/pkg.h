// SPDX-License-Identifier: GPL-2.0-only
// The user's game dumps, read on the console: .nsp and .nsz (PFS0), .xci and .xcz (cartridge images,
// their "secure" partition). Lists the NCAs and tickets, reads the content lists (.cnmt), opens NCAs
// (header with header_key, section key from the ticket or the key area) and reads their sections
// decrypted, also from compressed NCAs (.ncz). The port of the PC's dumps.py and the staging code's
// readers. Plain C with a read callback, so the PC tests run the same code.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ncz.h"

typedef int (*PkgReadFn)(void *user, uint64_t off, void *buf, size_t n);  // 0 = all n bytes read

#define PKG_MAX_ENTRIES 256
#define PKG_MAX_TICKETS 32
#define PKG_MAX_METAS 24
#define PKG_MAX_CONTENTS 24

enum { PKG_NSP, PKG_NSZ, PKG_XCI, PKG_XCZ };
enum { META_APPLICATION = 0x80, META_PATCH = 0x81, META_AOC = 0x82 };
// content record types in a content list
enum { CONTENT_META = 0, CONTENT_PROGRAM = 1, CONTENT_DATA = 2, CONTENT_CONTROL = 3 };
// NCA header content types
enum { NCA_PROGRAM = 0, NCA_META = 1, NCA_CONTROL = 2, NCA_MANUAL = 3, NCA_DATA = 4, NCA_PUBLIC_DATA = 5 };

typedef struct {
    char name[64];
    uint64_t offset, size;     // in the dump file
    bool ncz;                  // a compressed NCA
    bool nca;                  // an NCA (or .ncz) whose header decrypted
    uint8_t content_type;      // NCA_* (from its header)
    uint64_t program_id;       // from its header
    uint8_t rights_id[16];
    int key_generation;        // 0-based (the key names' number)
} PkgEntry;

typedef struct {
    uint8_t id[16];
    uint8_t type;              // CONTENT_*
    uint8_t index;             // id offset: which program of the title
    uint64_t size;
} PkgContent;

typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint8_t type;              // META_*
    uint64_t other_id;         // application: its patch ID; patch and add-on content: the application
    uint32_t required;         // patch: system version; add-on content: the oldest game version
    PkgContent contents[PKG_MAX_CONTENTS];
    int count;
} PkgMeta;

typedef struct {
    uint8_t rights_id[16], title_key[16];
    bool common, bad;
    uint64_t device_id;
} PkgTicket;

typedef struct Pkg {
    PkgReadFn read;
    void *user;
    uint64_t size;
    int kind;                  // PKG_*
    PkgEntry entries[PKG_MAX_ENTRIES];
    int count;
    PkgTicket tickets[PKG_MAX_TICKETS];
    int ticket_count;
    PkgMeta metas[PKG_MAX_METAS];
    int meta_count;
    char *keys;                // prod.keys text (owned copy)
    uint8_t header_key[32];
} Pkg;

// Opens a dump: container, tickets, every NCA header and the content lists. NULL with a sentence in err.
Pkg *pkg_open(PkgReadFn read, void *user, uint64_t size, const char *keys_text, char *err, size_t errn);
void pkg_close(Pkg *p);
const char *pkg_kind_name(int kind);   // ".nsp", ...
// "game 0100152000022000 v0; update v1507328 for game 0100152000022000"
void pkg_describe(const Pkg *p, char *out, size_t n);
const PkgMeta *pkg_meta(const Pkg *p, int type, int nth);   // the nth content list of a type
// The dump file entry that holds a content (by its ID: "<id>.nca" or "<id>.ncz"); -1 if absent.
int pkg_content_entry(const Pkg *p, const PkgContent *c);
const PkgContent *pkg_meta_content(const PkgMeta *m, int type, int index);

// ---- one NCA
typedef struct {
    int index;
    uint64_t start, end;       // within the NCA
    const uint8_t *fs;         // its 0x200-byte header
    uint8_t fs_type, hash_type, encryption;
    uint64_t upper;            // the counter value (fs + 0x140)
    bool sparse;               // stored sparse (a cartridge base with a bundled update): read it through its map,
                               // see stage.c; start..end is then the virtual range and may pass the NCA's end
} NcaSection;

typedef struct Nca {
    Pkg *pkg;
    const PkgEntry *e;
    uint8_t header[0xc00];
    uint64_t size;
    Ncz *ncz;
    uint8_t key[16];
    bool keyed;
    char err[240];
} Nca;

// Opens an NCA of the dump and works out its key (ticket or key area). False with a sentence in err.
bool nca_open(Pkg *p, int entry, Nca *n, char *err, size_t errn);
void nca_close(Nca *n);
// A section by kind: fs_type 0 = RomFS, 1 = PFS0; encryption 3 = CTR, 4 = patch (BKTR), 0 = any.
bool nca_section(Nca *n, int fs_type, int encryption, NcaSection *s);
// The NCA's bytes as stored (encrypted), at an offset within the NCA.
bool nca_read_raw(Nca *n, uint64_t off, void *buf, size_t len);
// The NCA's bytes decrypted with its key and the counter `upper` (AES-CTR), at an offset within the NCA.
bool nca_read_ctr(Nca *n, uint64_t off, void *buf, size_t len, uint64_t upper);
// Bytes stored at `pos`, decrypted with the counter for `ctr_off` (a sparse section keeps data encrypted for its
// virtual offset, packed elsewhere). pos and ctr_off must agree modulo 16.
bool nca_read_ctr_at(Nca *n, uint64_t pos, uint64_t ctr_off, void *buf, size_t len, uint64_t upper);
// Bytes of a section, decrypted with the section's own counter; off within the section.
bool nca_section_read(Nca *n, const NcaSection *s, uint64_t off, void *buf, size_t len);

// ---- programs, name and icon
typedef struct {
    int index;                 // the program's number in the title (id offset)
    uint64_t program_id;
    int base_entry, patch_entry;   // dump entries of the Program NCAs (patch_entry -1 without update)
} PkgProgram;

// The programs of the base game's title, each with its update's Program NCA when `update` has one.
int pkg_programs(Pkg *base, Pkg *update, PkgProgram *out, int max);

typedef struct {
    char name[128], publisher[96], display_version[32];
    uint8_t *icon;             // JPEG (malloc'd), or NULL
    size_t icon_size;
    uint64_t add_on_base_id;
    uint64_t save_user, save_user_journal, save_device, save_device_journal;
    // what the game may extend its saves to while it runs (0 = it doesn't)
    uint64_t save_user_max, save_user_journal_max, save_device_max, save_device_journal_max;
} PkgControl;

// The name, version, icon and save sizes of a program: from the update when given, else the base.
bool pkg_control(Pkg *base, Pkg *update, int index, PkgControl *out);
void pkg_control_free(PkgControl *c);
