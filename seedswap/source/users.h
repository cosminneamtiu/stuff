// SPDX-License-Identifier: GPL-2.0-only
// The play user (DESIGN.md "Users"): the one console user whose Seedsow save SeedSwap swaps game saves
// in and out of. Chosen on the console (Saves tab, Y) and kept in SD:/seedswap/config.ini as
// play_user=<uid>; without that line the loader's save-guard user is taken, as the loader lets only that
// user open Seedsow's save anyway. Users that can never be the play user, and whose saves are never opened:
//  * config.ini exploit_user=<uid>[,<uid>], the users whose Seedsow save has the exploit; while it names
//    none, SeedSwap looks for the kaiHEN chain's files in each user's Seedsow save (read-only) and
//    writes the users that have them there;
//  * the loader's own protected users (PROTECTED_USERS in SD:/kaihen/s2_ml.lua);
//  * users kept out by hand (Y in the list; config.ini protected_users=<uid>,<uid>);
//  * with the loader's save guard on, every user but the guard's, unless the guard follows the play user:
//    when the install image's guard can be changed (hosts.h hostimg_set_guard) and the exploit save's
//    user is known, choosing a play user sets the image's guard to it, for the launcher's next A run.
//    A release image's guard is ffff...ffff (nobody) until then.
// The system is not asked which user runs the exploit: under kaiHEN the account service reports whoever
// last started a game, which is usually the play user. No save but the play user's is ever opened (the
// platform layer refuses the others too).
#pragma once
#include "model.h"

#define USERS_MAX 12

typedef struct {
    Uid uid;
    char hex[33], name[64];
    bool exploit;    // config.ini exploit_user: its Seedsow save has the exploit
    bool loader;     // the loader protects it (its PROTECTED_USERS)
    bool kept_out;   // kept out by hand (config.ini protected_users)
    bool guard;      // the loader's save guard lets only this user open Seedsow's save
    bool not_guard;  // the save guard is on, for another user
    bool play;       // the play user now
    bool demo_only;  // has a demo save and no save for the selected full-game host
} UserRow;

// Works out the play user (L->play_user, play_user_set, play_user_source, names) from config.ini and
// the loader's guard, and tells the platform layer which save may be opened. lib_load calls it after
// reading the loader's state and before anything opens a save.
void users_load(Library *L);
// A user that can never be the play user (see above).
bool users_protected(const Uid *u);
// Nothing tells SeedSwap which user has the exploit save (no exploit_user, no loader list, no guard).
bool users_exploit_unknown(void);
// Choosing a play user moves the install image's save guard to it (see above).
bool users_guard_follows(void);
// The guard follows and the install image's guard is not on this user (choosing it changes the image).
bool users_guard_differs(const char *hex);
// Users with a save for the selected host, plus demo save owners for the full-game host.
int users_rows(const Library *L, UserRow *out, int max);
// NULL when row may become the play user now; otherwise why not, as a sentence.
const char *users_change_blocker(const Library *L, const UserRow *row);
bool users_set_play(Library *L, const UserRow *row, char *err, size_t errn);
// Keeps a user out (or lets it in again). The play user and the always-protected users cannot change.
bool users_toggle_kept_out(Library *L, const UserRow *row, char *err, size_t errn);
