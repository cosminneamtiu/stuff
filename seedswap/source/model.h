// SPDX-License-Identifier: GPL-2.0-only
// SeedSwap library: games on the SD card, the hosts' folders (hosts.h), per-game saves.
// The library is shown for one host at a time (L->host, state.ini host=): "active" is the game in
// that host's folder, the save is that host's, and the host itself is the built-in entry.
#pragma once
#include "compat_game.h"
#include "dlc_game.h"
#include "gfx.h"
#include "hosts.h"
#include "util.h"

#define MAX_GAMES 48
#define MAX_BACKUPS 40
#define SEEDSOW_ID "seedsow"
#define HOST_ID_PREFIX "host-"   // the built-in entry of another host: "host-<program id>"

// where the play user came from (users.h)
typedef enum { PLAY_USER_NONE, PLAY_USER_CONFIG, PLAY_USER_GUARD } PlayUserSource;

typedef struct {
    char id[24], name[128], publisher[96], version[32], status[24], notes[320], source[200], added[32];
    char prev_version[32];  // the version an update replaced ("" = never updated)
    int64_t version_code;   // the package's version number (0 = base game, 65536 per update); -1 unknown
    uint64_t title_id;      // 0 when game.ini has none
    int64_t romfs_size, exefs_size;
    bool active, parked, builtin, unknown;
    int host;               // the host whose folder holds its files (-1: none); built-in: the host it is
    Image icon_small, icon_big;
    bool slot;              // a per-game save slot exists for the play user
    TreeStats slot_stats;
    CompatInfo compat;      // Nintendo's compatibility patches for this game (compat_game.h)
    char dlc[200], dlc_names[200];  // add-on content built in: IDs and names (game.ini dlc, dlc_names)
    uint64_t compat_words[4];  // Nintendo's settings put in (game.ini compat_words: graphics, movie, audio, misc)
    bool has_compat_words;
    bool dlc_removable;     // the image without it is kept (exefs-nodlc.nsp)
    // the account save the game may grow to, data and journal in bytes (game.ini save_size, save_journal;
    // 0 = not recorded): the play user's save is made at least this big before the game is switched in
    int64_t save_need, journal_need;
} Game;

typedef struct { char dir[128], when[32], game[24]; TreeStats st; } Backup;

// The modloader as the launcher's A run installed it (read from SD:/kaihen/seedsow_run.log).
typedef enum {
    LOADER_OFF,    // not in memory this boot (or never installed): the next A run uses the SD image
    LOADER_LIVE,   // in memory with live sizes: a switch applies at the next Seedsow launch
    LOADER_FIXED,  // in memory with sizes fixed at install: switching needs a reboot + A run first
} LoaderState;

typedef struct {
    Game games[MAX_GAMES];
    int count;
    Backup backups[MAX_BACKUPS];
    int backup_count;
    HostSet hs;             // the hosts and their folders (hosts.h)
    int host;               // the host shown (index into hs.list)
    bool host_chosen;       // a host was picked (state.ini host=, or one holds a game); else SeedSwap asks
    char active_id[24];     // the shown host's game (state.ini)
    bool overlay_present;   // the shown host's folder holds a game
    int64_t overlay_romfs, overlay_exefs;
    char image_guard_program[17];   // the install image's guard program ("" = none, ffffffffffffffff = every hosting title)
    char loader_titles[400];        // the titles the loader in memory serves (seedsow_run.log), comma-separated
    char loader_guard_program[17];
    bool live_sizes;        // the install image on SD accepts any game size (switching on the console)
    char image_guard_user[33];  // the image's save-guard user ("" = no guard)
    LoaderState loader;     // what is in memory now
    bool loader_guard;      // the running loader has the save guard
    char loader_guard_user[33], loader_report[96];
    bool journal_pending;
    int64_t sd_free;
    Uid play_user;          // valid when play_user_set (users.h)
    char play_user_hex[33], play_user_name[64];
    bool play_user_set;
    PlayUserSource play_user_source;
    bool play_user_has_save, current_save_ok;
    TreeStats current_save;
    bool patcher_present;   // the console's patcher file is on the SD card
    char switch_note[240];  // what the last switch did about Nintendo's patches ("" = nothing to say)
    bool switch_backed_up;  // the last switch got as far as backing up the save
} Library;

// Updates (DESIGN.md "Updates"): one library entry per game; an update replaces its files in place.
// How a candidate version number relates to the one the library holds.
typedef enum {
    VER_UNKNOWN,  // one of the two has no version number: nothing can be said
    VER_NEWER,    // an update: allowed
    VER_SAME,     // the same version again (e.g. new fixes): allowed
    VER_OLDER,    // a downgrade: refused unless the user insists, because saves may not load
} VersionRelation;
// "v1507328", "1507328", "update v1507328" -> 1507328; a display version ("v1.0.0") or "" -> -1
int64_t version_code_from_text(const char *text);
VersionRelation lib_version_relation(const Game *g, int64_t candidate);

void lib_load(Library *L);
// The host shown and its name.
const Host *lib_host(const Library *L);
// Shows another host (kept in state.ini) and loads the library again.
void lib_set_host(Library *L, int host);
// The loader in memory serves h (true when that isn't known: no loader in memory or no titles logged).
bool lib_host_served(const Library *L, const Host *h);
// The host whose folder holds game g (NULL: none).
const Host *lib_game_host(const Library *L, const Game *g);
// Installed games that could become a host (apps_list minus the hosts). -1: the console can't tell.
int lib_host_candidates(const Library *L, AppEntry *out, int max);
// Gives a free slot of the install image to the installed game `id` (hosts.h hostimg_retitle) and
// shows it.
bool lib_host_add(Library *L, uint64_t id, const char *name, char *err, size_t errn);
// Moving game idx, which is in another host, into the host shown, with its save: NULL when possible.
const char *lib_move_blocker(const Library *L, int idx);
// Nintendo's settings record of the host (filed under its ID) applies to every game it runs. True when
// it has words the game's own settings lack; desc: those words ("misc 0x20"). False when it adds none
// or the console's list can't be read (the PC preview without SEEDSWAP_COMPAT_INFO).
bool lib_host_adds_settings(const Game *g, const Host *H, char *desc, size_t n);
// Switches its host back to itself, then the host shown to the game. On failure the game may be out
// of both (its files in SeedSwap's folder, its save in its slot); err says so.
bool lib_move(Library *L, int idx, ProgressFn cb, char *err, size_t errn);
void lib_free(Library *L);
int lib_active(const Library *L);
const char *lib_switch_blocker(const Library *L, int idx);  // NULL when switching is possible
bool lib_switch(Library *L, int idx, ProgressFn cb, char *err, size_t errn);
bool saves_backup_now(Library *L, const char *why, ProgressFn cb, char *err, size_t errn);
bool saves_restore(Library *L, int backup, ProgressFn cb, char *err, size_t errn);
bool uid_protected(const Uid *u);   // a user that can never be the play user (users.h)
// Nintendo's compatibility patches. lib_compat_wanted: the game has files, the patcher is there, and
// the game has not been looked at or still needs its patches. lib_compat inspects the game and, with
// apply, patches it; lib_compat_revert puts the image from before back.
bool lib_compat_wanted(const Library *L, int idx);
bool lib_compat(Library *L, int idx, bool apply, ProgressFn cb, char *err, size_t errn);
bool lib_compat_revert(Library *L, int idx, char *err, size_t errn);
// Add-on content (dlc_game.h). lib_dlc_blocker: NULL when the game's files may change now (an active
// game's romfs.bin grows, which needs a loader that reads sizes live).
const char *lib_dlc_blocker(const Library *L, int idx);
bool lib_dlc_built_in(const Game *g, uint64_t dlc_title_id);
// siblings: the game's other DLC dumps on the card (dlc_game.h)
bool lib_dlc_add(Library *L, int idx, const char *nsp_path, const char *keys_path, const char *const *siblings,
                 int nsiblings, ProgressFn cb, DlcResult *res, char *err, size_t errn);
bool lib_dlc_remove(Library *L, int idx, char *err, size_t errn);
