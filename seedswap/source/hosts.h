// SPDX-License-Identifier: GPL-2.0-only
// Host titles (DESIGN.md "Hosts"): the installed Switch 1 games whose identity SeedSwap's games run
// under. Each host has its own folder that the resident loader serves
// (/ns2jb-modloader/contents/<id>/{exefs.nsp,romfs.bin}); a host with an empty folder runs as itself.
//
//  * Which titles are hosts: the install image says so in hosts.ini (semi-untethered-port/tools/
//    build_host_image.py), with spare slots (placeholder IDs no game has) that SeedSwap can give to
//    another installed game. Without hosts.ini (older images) Seedsow Lullaby is the only host.
//  * A hosted game's main.npdm must be the host's own, signed one: Loader checks the code's
//    verification data against the ACID key of the NPDM it is served, so a retitled Seedsow NPDM is
//    refused. The host's NPDM comes from the loader's report: the runtime logs every listed title's
//    main.npdm and code verification data ("Host NPDM" / "Host code info" rows) when it starts.
//    SeedSwap keeps the ones that check out (ACID signature, program ID, the code data verifies under
//    the NPDM's own key) in SD:/seedswap/hosts/<id>/.
//  * Moving a game to another host rewrites only its exefs.nsp's main.npdm: the new host's NPDM with
//    the game's process fields (pc/rehost.py does the same on the PC).
//  * The system patches a Switch 1 program by the ID it runs as (cmptldr), so a host that has
//    patches of its own would have them put into the hosted game; hosts_patch_check runs the patcher
//    with the host's ID on the game's code and refuses the pair when it would change anything.
// Plain C over the platform's file calls, so the PC tests run the same code.
#pragma once
#include "util.h"

#define HOSTS_MAX 16
#define HOST_SPARES_MAX 15
#define INSTALL_DIR "/kaihen/ml/install"
#define HOSTS_INI INSTALL_DIR "/hosts.ini"
#define HOSTS_DIR SS_ROOT "/hosts"
#define LOADER_LOGS "/ns2jb-modloader/logs"
#define LEGACY_HOST_NPDM SS_ROOT "/system/seedsow-main.npdm"
// A host's data from the loader in the same boot: SeedSwap leaves an empty file here; the loader
// (runtime with lfs_host_file, 2026-11-23) fills it the next time that title starts and closes it.
// "HPRB", u32 1, u64 program, u32 npdm size, u32 info size, 8 zero bytes, npdm, info.
#define HOST_FILES_DIR "/ns2jb-modloader/hosts"
#define HOST_FILE_BYTES 0x2800

typedef struct {
    uint64_t id;
    char hex[17];
    char name[64];
    char dir[64];          // its overlay folder
    bool npdm;             // its main.npdm (and code data) is on the SD card (hosts_npdm checks them when read)
    // filled by lib_load (model.c)
    char active_id[24];    // state.ini: the game in its folder
    bool overlay_present;
    int64_t overlay_romfs, overlay_exefs;
    bool user_save;        // the play user has an account save for it
} Host;

typedef struct {
    Host list[HOSTS_MAX];
    int count;
    uint64_t spares[HOST_SPARES_MAX];
    int spare_count;
    bool ini;              // hosts.ini was found
    char note[160];        // what hosts_load or hosts_import had to say ("" = nothing)
} HostSet;

// Reads hosts.ini (else Seedsow alone), the hosts' names (SD:/seedswap/hosts/<id>/host.ini, else
// the console's list of installed games) and whether their NPDM is known.
void hosts_load(HostSet *s);
int hosts_find(const HostSet *s, uint64_t id);
// Display name for a program ID: a host's name, else "0100…" in hex.
void hosts_hex(uint64_t id, char out[17]);

// ---- NPDM
uint64_t npdm_program(const uint8_t *npdm, size_t n);            // ACI0 program ID, 0 when not an NPDM
bool npdm_acid_signed(const uint8_t *npdm, size_t n);            // Nintendo's ACID signature checks out
// The code verification data (0x1a4 bytes: RSA-2048-PSS signature, SHA-256 at 0x180, flag at 0x1a0)
// verifies under the NPDM's own ACID key.
bool npdm_code_info_ok(const uint8_t *npdm, size_t n, const uint8_t *info, size_t in);
// A host's own NPDM: signed, names `id`, its ACID allows `id`, and (info != NULL) its code data verifies.
bool npdm_host_ok(const uint8_t *npdm, size_t n, uint64_t id, const uint8_t *info, size_t in, char *why, size_t whyn);
// The host's NPDM with the game's process fields (META 0x0C-0x20) from `game`. *out: malloc'd, hn bytes.
bool npdm_for_game(const uint8_t *host, size_t hn, const uint8_t *game, size_t gn, uint8_t **out);

// The host's signed main.npdm (malloc'd). Seedsow: also the file older SeedSwap versions kept
// (LEGACY_HOST_NPDM) or one found in a game image built for it; without those it comes from the
// loader like any other host's (hosts_load counts those sources as known).
bool hosts_npdm(const Host *h, uint8_t **out, size_t *n, char *err, size_t errn);

// ---- loader reports
typedef struct HostProbe HostProbe;
// The rows of one report: how many programs have a complete NPDM and code data in it.
HostProbe *probe_parse(const char *text, int *count);
// Program `i` of a parsed report (false when its rows were incomplete or disagree).
bool probe_get(const HostProbe *p, int i, uint64_t *program, const uint8_t **npdm, size_t *n, const uint8_t **info,
               size_t *in);
void probe_free(HostProbe *p);
// For the hosts whose NPDM is not known yet: reads the loader's host files, then the readable loader
// reports, newest first; stores each one that checks out. Returns how many were stored.
int hosts_import(HostSet *s);
// Leaves an empty host file for a host whose NPDM isn't known, Seedsow included (nothing for a known one).
bool hosts_request(const Host *h);

// ---- ExeFS images
// main.npdm of an exefs.nsp on the SD card (malloc'd).
bool exefs_read_npdm(const char *path, uint8_t **npdm, size_t *n);
// Moves the image at path to the host whose signed NPDM is given: its main.npdm becomes the host's
// with the image's process fields; every other file is copied byte for byte, in the same order, and
// the image keeps its padding. Written beside it, read back, then swapped in. *changed: false when
// the image was already the host's.
bool exefs_rehost(const char *path, const uint8_t *host_npdm, size_t hn, bool *changed, ProgressFn cb, char *err,
                  size_t errn);

// ---- the install image's slots
// Gives the spare slot `spare` to the title `to`: every copy of the spare's ID in context.bin and
// image.lua becomes `to`, image.lua's checksums of context.bin follow, and hosts.ini lists `to` as a
// host. The three files are first copied to backup_dir. As build_host_image.py retitle.
bool hostimg_retitle(const char *install_dir, uint64_t spare, uint64_t to, const char *backup_dir, char *err,
                     size_t errn);

// The install image's save guard lets games open the Seedsow saves of one user only. Its user is in
// image.lua's guard block, once in context.bin (with image.lua's checksums of it) and in hosts.ini
// guard_user. hostimg_guard_user: that user, when the image is as expected (else false, out "").
// hostimg_set_guard: makes it another user (copies the files to backup_dir first); the running loader
// keeps the old user until the launcher's next A run. As build_host_image.py build --guard-user.
bool hostimg_guard_user(const char *install_dir, char out[33]);
bool hostimg_set_guard(const char *install_dir, const char *user_hex, const char *backup_dir, char *err, size_t errn);

// ---- Nintendo's patches under the host's ID
// Would the console's patcher change the game's code when it runs as `host`? *changes: instructions
// (0 = safe). Remembered per game and host in game_dir/hostcheck.ini (keyed by main's hash). False
// when it can't tell (no patcher on the SD card, unreadable image); err says why.
bool hosts_patch_check(const char *exefs_path, const char *game_dir, uint64_t host, int *changes, ProgressFn cb,
                       char *err, size_t errn);
