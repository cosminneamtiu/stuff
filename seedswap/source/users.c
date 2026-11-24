// SPDX-License-Identifier: GPL-2.0-only
#include "users.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG SS_ROOT "/config.ini"
#define LOADER_LUA "/kaihen/s2_ml.lua"   // the loader; its PROTECTED_USERS are users it never lets games near

// The system cannot say which user has the exploit save: under kaiHEN the account service's "preselected"
// and "last opened" users are whoever last started a game, usually the play user. So the users to keep
// away from come from the saves themselves (the exploit save holds the kaiHEN chain's files, written to
// config.ini exploit_user when found), what the console's owner set up (config.ini, the loader's own
// protected list) and the loader's save guard (which lets games open only one user's Seedsow save).
static char g_exploit[400];     // config.ini exploit_user (one or more, comma-separated)
static char g_loader[400];      // the loader's PROTECTED_USERS
static char g_kept[400];        // config.ini protected_users
static char g_guard[33];        // the loader's save-guard user; "" = no guard
static char g_guard_name[64];
static char g_image_guard[33];  // the install image's guard user, when SeedSwap can change it
static bool g_follows;          // the guard follows the play user: the image's guard can be changed, and
                                // the exploit save's user is known (config.ini, or found in the saves)

// A release image's guard user before anyone is chosen: no user has this ID, so hosted games open no
// save until SeedSwap sets the play user in its place.
#define GUARD_NOBODY "ffffffffffffffffffffffffffffffff"
static bool guard_nobody(void) { return !strcmp(g_guard, GUARD_NOBODY); }

// The exploit chain's own files (alula's launcher and seedsow-semi-untethered): only the exploit save
// has them at its root; a normal Seedsow save holds .dat files only.
static const char *const CHAIN_FILES[] = {"exploita2.lua", "exploitb2.lua", "s2_kh_ro.lua", "s2_kaihen.lua", "s2_main.lua",
                                          "inject.iet", "inject.lua"};

static bool config_set(const char *key, const char *value);

static bool listed(const char *list, const char *hex) {
    for (const char *p = list; (p = strstr(p, hex)); p += 32)
        if ((p == list || p[-1] == ',' || p[-1] == ' ') && (p[32] == 0 || p[32] == ',' || p[32] == ' ')) return true;
    return false;
}

static void lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

// every 32-digit user ID inside the loader's PROTECTED_USERS = { ... }
static void read_loader_list(void) {
    g_loader[0] = 0;
    char *lua = read_text(VFS_SD, LOADER_LUA, 512 * 1024);
    if (!lua) return;
    const char *p = strstr(lua, "PROTECTED_USERS = {");
    const char *e = p ? strchr(p, '}') : NULL;
    if (p && e) {
        for (const char *q = strchr(p, '{'); q < e;) {
            int k = 0;
            while (q + k < e && isxdigit((unsigned char)q[k])) k++;
            if (k == 32 && strlen(g_loader) + 34 < sizeof(g_loader))
                snprintf(g_loader + strlen(g_loader), sizeof(g_loader) - strlen(g_loader), "%s%.32s",
                         g_loader[0] ? "," : "", q);
            q += k ? k : 1;
        }
        lower(g_loader);
    }
    free(lua);
}

// the user the loader's save guard lets open Seedsow's save: the running loader's, else the install image's
static const char *guard_user(const Library *L) {
    if (L->loader != LOADER_OFF && L->loader_guard && L->loader_guard_user[0]) return L->loader_guard_user;
    return L->image_guard_user;
}

// why a user can't be the play user, for the log; NULL when it can
static const char *refusal(const char *hex) {
    static char why[160];
    if (listed(g_exploit, hex)) return "it has the exploit save (config.ini exploit_user)";
    if (listed(g_loader, hex)) return "the loader protects it";
    if (listed(g_kept, hex)) return "it is kept out (config.ini protected_users)";
    if (g_guard[0] && strcmp(g_guard, hex) && !g_follows) {
        if (guard_nobody()) return "the loader isn't set up for anyone and the exploit save's user isn't known";
        snprintf(why, sizeof(why), "the loader's save guard only lets %s play", g_guard);
        return why;
    }
    return NULL;
}

// The users whose Seedsow save holds the chain's files: looked for once per run while config.ini names
// none, and written there (exploit_user), where the loader's install check reads it too. Each save is
// only opened read-only to see whether the files exist (platform.h save_find_files).
static void find_exploit(void) {
    static bool done;
    if (done || g_exploit[0]) return;
    done = true;
    uint64_t title = save_title();
    save_set_title(SEEDSOW_TITLE);
    Uid users[USERS_MAX];
    int n = save_list_users(users, USERS_MAX), unreadable = 0;
    save_set_title(title);
    char list[400] = "";
    for (int i = 0; i < n; i++) {
        char h[33], file[40];
        uid_hex(&users[i], h);
        int r = save_find_files(&users[i], CHAIN_FILES, (int)(sizeof(CHAIN_FILES) / sizeof(*CHAIN_FILES)), file, sizeof(file));
        if (r < 0) {
            unreadable++;
            app_log("exploit check: %s's Seedsow save couldn't be opened: %s", h, vfs_last_error());
        } else if (r == 1 && strlen(list) + 34 < sizeof(list)) {
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", list[0] ? "," : "", h);
            app_log("exploit check: %s's Seedsow save has %s, so it is never the play user", h, file);
        }
    }
    if (!list[0]) {
        app_log("exploit check: no Seedsow save has the exploit's files (%d checked, %d couldn't be opened)", n < 0 ? 0 : n,
                unreadable);
        return;
    }
    snprintf(g_exploit, sizeof(g_exploit), "%s", list);
    if (config_set("exploit_user", list)) app_log("config.ini: exploit_user=%s", list);
    else app_log("ERROR config.ini exploit_user not written: %s", vfs_last_error());
}

bool users_guard_follows(void) { return g_follows; }

bool users_guard_differs(const char *hex) { return g_follows && strcmp(g_image_guard, hex); }

bool users_protected(const Uid *u) {
    char h[33];
    uid_hex(u, h);
    return refusal(h) != NULL;
}

bool users_exploit_unknown(void) { return !g_exploit[0] && !g_loader[0] && (!g_guard[0] || guard_nobody()); }

static bool user_label(const Uid *u, const char *hex, char *out, size_t n) {
    if (user_name(u, out, n)) return true;
    snprintf(out, n, "User %.8s", hex);
    return false;
}

void users_load(Library *L) {
    char *cfg = read_text(VFS_SD, CONFIG, 4096);
    char user[48] = {0};
    g_kept[0] = g_exploit[0] = 0;
    if (cfg) {
        ini_get(cfg, "protected_users", g_kept, sizeof(g_kept));
        lower(g_kept);
        if (ini_get(cfg, "exploit_user", g_exploit, sizeof(g_exploit)) && strlen(g_exploit) < 32) g_exploit[0] = 0;
        lower(g_exploit);
    }
    read_loader_list();
    find_exploit();
    g_follows = g_exploit[0] && hostimg_guard_user(INSTALL_DIR, g_image_guard);
    if (!g_follows) g_image_guard[0] = 0;
    snprintf(g_guard, sizeof(g_guard), "%s", guard_user(L));
    lower(g_guard);
    g_guard_name[0] = 0;
    Uid gu;
    if (guard_nobody()) snprintf(g_guard_name, sizeof(g_guard_name), "nobody yet");
    else if (g_guard[0] && uid_parse(g_guard, &gu)) user_label(&gu, g_guard, g_guard_name, sizeof(g_guard_name));

    // the play user: config.ini, else the save guard's user; the first one that is allowed
    L->play_user_set = false;
    L->play_user_source = PLAY_USER_NONE;
    const char *cand[2] = {cfg && ini_get(cfg, "play_user", user, sizeof(user)) ? user : "", guard_nobody() ? "" : g_guard};
    for (int i = 0; i < 2 && !L->play_user_set; i++) {
        Uid u;
        if (!cand[i][0] || !uid_parse(cand[i], &u)) continue;
        char h[33];
        uid_hex(&u, h);
        const char *why = refusal(h);
        if (why) {
            app_log("play user %s (%s) not used: %s", h, i == 0 ? "config.ini" : "save guard", why);
            continue;
        }
        L->play_user = u;
        L->play_user_set = true;
        L->play_user_source = i == 0 ? PLAY_USER_CONFIG : PLAY_USER_GUARD;
    }
    free(cfg);
    if (L->play_user_set) {
        uid_hex(&L->play_user, L->play_user_hex);
        user_label(&L->play_user, L->play_user_hex, L->play_user_name, sizeof(L->play_user_name));
        save_allow_only(&L->play_user);
    } else {
        memset(&L->play_user, 0, sizeof(L->play_user));
        L->play_user_hex[0] = 0;
        snprintf(L->play_user_name, sizeof(L->play_user_name), "the play user (not chosen yet)");
        save_allow_only(NULL);
    }
}

int users_rows(const Library *L, UserRow *out, int max) {
    Uid users[USERS_MAX * 2];
    int host_n = save_list_users(users, USERS_MAX);
    if (host_n < 0) host_n = 0;
    int n = host_n, k = 0;
    if (save_title() == SEEDSOW_TITLE) {
        int demo_n = save_list_title_users(SEEDSOW_DEMO_TITLE, users + host_n, USERS_MAX);
        if (demo_n > 0) n += demo_n;
    }
    for (int i = 0; i < n && k < max; i++) {
        bool demo_only = i >= host_n;
        if (demo_only) {
            int j = 0;
            while (j < i && memcmp(users[j].b, users[i].b, sizeof(users[i].b))) j++;
            if (j < i) continue;
        }
        UserRow *r = &out[k++];
        memset(r, 0, sizeof(*r));
        r->uid = users[i];
        uid_hex(&r->uid, r->hex);
        user_label(&r->uid, r->hex, r->name, sizeof(r->name));
        r->exploit = listed(g_exploit, r->hex);
        r->loader = listed(g_loader, r->hex);
        r->kept_out = listed(g_kept, r->hex);
        r->guard = g_guard[0] && !strcmp(g_guard, r->hex);
        r->not_guard = g_guard[0] && !r->guard && !g_follows;
        r->play = L->play_user_set && !memcmp(L->play_user.b, r->uid.b, 16);
        r->demo_only = demo_only;
    }
    return k;
}

const char *users_change_blocker(const Library *L, const UserRow *row) {
    static char why[400];
    if (row->demo_only) return "This user has a Seedsow Lullaby Demo save. The selected host needs its own save.";
    if (row->play && !users_guard_differs(row->hex)) return "This is the play user already.";
    if (row->exploit) return "This user has the exploit save, so its save can't be used.";
    if (row->loader) return "The loader protects this user, so its save can't be used.";
    if (row->kept_out) return "This user is kept out. Press Y to allow it.";
    if (row->not_guard) {
        if (guard_nobody())
            return "SeedSwap couldn't find the user with the exploit save, so it can't set up the loader. Add "
                   "exploit_user=<that user's ID> to SD:/seedswap/config.ini.";
        snprintf(why, sizeof(why), "Only %s can play games with this loader.", g_guard_name);
        return why;
    }
    if (L->journal_pending) return "The last switch didn't finish. Check the Log tab first.";
    if (row->play) return NULL;   // only the save guard changes
    // a game in any host's folder has its save on the play user
    for (int i = 0; L->play_user_set && i < L->count; i++) {
        const Game *g = &L->games[i];
        const Host *h = lib_game_host(L, g);
        if (g->builtin || !h) continue;
        snprintf(why, sizeof(why), "%s is in %s and its save is on %s. Switch %s back to itself first.", g->name, h->name,
                 L->play_user_name, h->name);
        return why;
    }
    return NULL;
}

// config.ini: set a key (value NULL removes it), keeping the other lines
static bool config_set(const char *key, const char *value) {
    char *t = read_text(VFS_SD, CONFIG, 4096);
    const char *src = t ? t : "";
    size_t kl = strlen(key), cap = strlen(src) + (value ? strlen(value) : 0) + kl + 8;
    char *out = malloc(cap), *o = out;
    if (!out) { free(t); return false; }
    bool done = false;
    for (const char *line = src; *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        const char *k = line;
        while (k < line + len && (*k == ' ' || *k == '\t')) k++;
        bool match = (size_t)(line + len - k) > kl && !strncmp(k, key, kl) && (k[kl] == '=' || k[kl] == ' ');
        if (!match) {
            memcpy(o, line, len);
            o += len;
            *o++ = '\n';
        } else if (value && !done) {
            o += sprintf(o, "%s=%s\n", key, value);
            done = true;
        }
        line += len + (nl ? 1 : 0);
    }
    if (value && !done) o += sprintf(o, "%s=%s\n", key, value);
    *o = 0;
    mkdirs(VFS_SD, SS_ROOT);
    bool ok = write_text(VFS_SD, CONFIG, out);
    free(out);
    free(t);
    return ok;
}

bool users_set_play(Library *L, const UserRow *row, char *err, size_t errn) {
    const char *why = users_change_blocker(L, row);
    if (why) {
        snprintf(err, errn, "%s", why);
        return false;
    }
    if (users_guard_differs(row->hex)) {   // the save guard follows: the loader lets this user play after a restart
        char stamp[24], backup[96];
        plat_now(stamp, sizeof(stamp), true);
        snprintf(backup, sizeof(backup), SS_ROOT "/install-backups/%s", stamp);
        if (!hostimg_set_guard(INSTALL_DIR, row->hex, backup, err, errn)) {
            app_log("ERROR save guard for %s: %s", row->hex, err);
            return false;
        }
    }
    if (!config_set("play_user", row->hex)) {
        snprintf(err, errn, "SD:/seedswap/config.ini could not be written: %s", vfs_last_error());
        return false;
    }
    app_log("play user: %s (%s), was %s", row->hex, row->name, L->play_user_set ? L->play_user_hex : "none");
    return true;
}

bool users_toggle_kept_out(Library *L, const UserRow *row, char *err, size_t errn) {
    (void)L;
    if (row->exploit || row->loader) {
        snprintf(err, errn, "This user is always kept out.");
        return false;
    }
    if (row->play) {
        snprintf(err, errn, "This is the play user. Pick another one first.");
        return false;
    }
    char list[400] = "";
    if (row->kept_out) {   // take it off the list
        char copy[400];
        snprintf(copy, sizeof(copy), "%s", g_kept);
        for (char *tok = strtok(copy, ", "); tok; tok = strtok(NULL, ", "))
            if (strcmp(tok, row->hex))
                snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", list[0] ? "," : "", tok);
    } else {
        if (strlen(g_kept) + 34 >= sizeof(list)) {
            snprintf(err, errn, "Too many users are kept out already.");
            return false;
        }
        snprintf(list, sizeof(list), "%s%s%s", g_kept, g_kept[0] ? "," : "", row->hex);
    }
    if (!config_set("protected_users", list[0] ? list : NULL)) {
        snprintf(err, errn, "SD:/seedswap/config.ini could not be written: %s", vfs_last_error());
        return false;
    }
    app_log("%s %s (%s)", row->kept_out ? "let in again:" : "kept out:", row->hex, row->name);
    return true;
}
