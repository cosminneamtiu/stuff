// SPDX-License-Identifier: GPL-2.0-only
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat_words.h"
#include "stage.h"
#include "users.h"

static bool compat_paths(const Library *L, const Game *g, char *exefs, size_t en, char *gdir, size_t gn);
static bool fit_host(Library *L, int host, const char *game_id, const char *exefs, ProgressFn cb, char *err, size_t errn);

bool uid_protected(const Uid *u) { return users_protected(u); }

static void load_icon(Game *g, const char *dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/icon.jpg", dir);
    void *blob;
    size_t len;
    if (!read_blob(VFS_SD, path, 512 * 1024, &blob, &len)) return;
    Image full;
    if (img_decode_jpeg(blob, (int)len, &full)) {
        img_scale(&full, 72, 72, &g->icon_small);
        img_scale(&full, 176, 176, &g->icon_big);
        img_free(&full);
    }
    free(blob);
}

static void slot_path(const Library *L, const char *id, char *out, size_t n) {
    snprintf(out, n, SS_ROOT "/saves/%s/%s", id, L->play_user_hex);
}

static void load_slot(Library *L, Game *g) {
    if (!L->play_user_set) return;
    char p[256];
    slot_path(L, g->id, p, sizeof(p));
    bool dir = false;
    if (vfs_stat(VFS_SD, p, &dir, NULL) == 0 && dir) {
        g->slot = true;
        tree_stats(VFS_SD, p, &g->slot_stats);
    }
}

static int cmp_games(const void *a, const void *b) {
    const Game *x = a, *y = b;
    if (x->builtin != y->builtin) return x->builtin ? 1 : -1;
    return strcmp(x->name, y->name);
}

int64_t version_code_from_text(const char *text) {
    const char *last = strrchr(text, ' ');  // "update v1507328": the number is the last word
    if (last) text = last + 1;
    if (*text == 'v' || *text == 'V') text++;
    if (!*text) return -1;
    int64_t value = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9' || value > INT64_MAX / 16) return -1;  // "1.0.0" is a display version
        value = value * 10 + (*p - '0');
    }
    return value;
}

VersionRelation lib_version_relation(const Game *g, int64_t candidate) {
    if (g->version_code < 0 || candidate < 0) return VER_UNKNOWN;
    return candidate > g->version_code ? VER_NEWER : candidate == g->version_code ? VER_SAME : VER_OLDER;
}

static int cmp_backups(const void *a, const void *b) { return strcmp(((const Backup *)b)->dir, ((const Backup *)a)->dir); }

void lib_free(Library *L) {
    for (int i = 0; i < L->count; i++) {
        img_free(&L->games[i].icon_small);
        img_free(&L->games[i].icon_big);
    }
    memset(L, 0, sizeof(*L));
}

int lib_active(const Library *L) {
    for (int i = 0; i < L->count; i++)
        if (L->games[i].active) return i;
    return -1;
}

static void load_backups(Library *L) {
    L->backup_count = 0;
    VfsEntry *e;
    int n;
    if (vfs_list(VFS_SD, SS_ROOT "/backups", &e, &n) != 0) return;
    for (int i = 0; i < n && L->backup_count < MAX_BACKUPS; i++) {
        if (!e[i].is_dir) continue;
        Backup *b = &L->backups[L->backup_count++];
        memset(b, 0, sizeof(*b));
        snprintf(b->dir, sizeof(b->dir), "%s", e[i].name);
        // <yyyymmddThhmmss>-<game id>-<uid 8>
        char stamp[24] = {0};
        sscanf(e[i].name, "%15[^-]-%23[^-]", stamp, b->game);
        if (strlen(stamp) == 15 && stamp[8] == 'T')
            snprintf(b->when, sizeof(b->when), "%.4s-%.2s-%.2s %.2s:%.2s", stamp, stamp + 4, stamp + 6, stamp + 9, stamp + 11);
        else snprintf(b->when, sizeof(b->when), "%s", stamp);
        char p[256];
        snprintf(p, sizeof(p), SS_ROOT "/backups/%s", e[i].name);
        tree_stats(VFS_SD, p, &b->st);
    }
    free(e);
    qsort(L->backups, (size_t)L->backup_count, sizeof(Backup), cmp_backups);
}

// Which loader is in memory now? The launcher's A run (s2_ml.lua) appends to seedsow_run.log; the
// last successful run names its report file, which the resident loader keeps open exclusively until
// reboot. Report present but not openable = that run's loader is in memory now.
static const char *rfind(const char *hay, const char *end, const char *needle) {
    const char *last = NULL;
    for (const char *p = hay; (p = strstr(p, needle)) && p < end; p++) last = p;
    return last;
}

static void load_loader_state(Library *L) {
    L->loader = LOADER_OFF;
    L->loader_guard = false;
    L->loader_guard_user[0] = L->loader_report[0] = 0;
    char *log = read_text(VFS_SD, "/kaihen/seedsow_run.log", 4 * 1024 * 1024);
    if (!log) return;
    const char *end = log + strlen(log);
    const char *m3 = rfind(log, end, "milestone 3: modloader resident");
    const char *start = m3 ? rfind(log, m3, "ML: image ") : NULL;
    const char *rep = m3 ? strstr(m3, "loader report: ") : NULL;
    if (m3 && rep) {
        rep += strlen("loader report: ");
        size_t n = strcspn(rep, "\r\n");
        if (n < sizeof(L->loader_report)) {
            memcpy(L->loader_report, rep, n);
            L->loader_report[n] = 0;
        }
    }
    bool live = start && rfind(start, m3, "live SD sizes:");
    const char *g = start ? rfind(start, m3, "save guard: account saves") : NULL;
    const char *ge = g ? strstr(g, "every user except ") : NULL;
    const char *ts = start ? strstr(start, ": titles ") : NULL;
    L->loader_titles[0] = L->loader_guard_program[0] = 0;
    if (ts && ts < m3) {
        ts += strlen(": titles ");
        size_t n = strcspn(ts, ";\r\n");
        snprintf(L->loader_titles, sizeof(L->loader_titles), "%.*s", (int)n, ts);
    }
    const char *gp = g ? strstr(g, "of program ") : NULL;
    if (gp && gp < m3) snprintf(L->loader_guard_program, sizeof(L->loader_guard_program), "%.16s", gp + strlen("of program "));
    if (ge && ge < m3) {
        L->loader_guard = true;
        snprintf(L->loader_guard_user, sizeof(L->loader_guard_user), "%.32s", ge + strlen("every user except "));
    }
    free(log);
    if (!L->loader_report[0]) return;
    bool dir = false;
    if (vfs_stat(VFS_SD, L->loader_report, &dir, NULL) != 0 || dir) return;
    VfsFile *f = vfs_open(VFS_SD, L->loader_report, false);
    if (f) { vfs_close(f); return; }  // readable: that loader is gone (rebooted since)
    L->loader = live ? LOADER_LIVE : LOADER_FIXED;
}

static void builtin_id(const Host *h, char *out, size_t n) {
    if (h->id == SEEDSOW_TITLE) snprintf(out, n, SEEDSOW_ID);
    else snprintf(out, n, HOST_ID_PREFIX "%s", h->hex);
}

// state.ini: host=<the host shown>, active=<Seedsow's game>, active.<host>=<another host's game>
static void read_state(Library *L) {
    char *state = read_text(VFS_SD, SS_ROOT "/state.ini", 8192);
    char v[24], key[32];
    L->host = 0;
    L->host_chosen = false;
    if (state && ini_get(state, "host", v, sizeof(v)) && strlen(v) == 16) {
        int i = hosts_find(&L->hs, strtoull(v, NULL, 16));
        if (i >= 0) { L->host = i; L->host_chosen = true; }
    }
    for (int i = 0; state && i < L->hs.count; i++) {
        Host *h = &L->hs.list[i];
        if (h->id == SEEDSOW_TITLE) snprintf(key, sizeof(key), "active");
        else snprintf(key, sizeof(key), "active.%s", h->hex);
        ini_get(state, key, h->active_id, sizeof(h->active_id));
    }
    free(state);
    // no host picked yet: one that holds a game counts as picked; an image without host slots has
    // only Seedsow; otherwise the user picks (Hosts, -)
    for (int i = 0; !L->host_chosen && i < L->hs.count; i++)
        if (L->hs.list[i].active_id[0]) { L->host = i; L->host_chosen = true; }
    if (!L->hs.ini) L->host_chosen = true;
}

static bool write_state(const Library *L) {
    char st[1600], when[32];
    plat_now(when, sizeof(when), false);
    int si = hosts_find(&L->hs, SEEDSOW_TITLE);
    size_t k = (size_t)snprintf(st, sizeof(st), "active=%s\n", si >= 0 ? L->hs.list[si].active_id : "");
    for (int i = 0; i < L->hs.count && k < sizeof(st); i++)
        if (L->hs.list[i].id != SEEDSOW_TITLE && L->hs.list[i].active_id[0])
            k += (size_t)snprintf(st + k, sizeof(st) - k, "active.%s=%s\n", L->hs.list[i].hex, L->hs.list[i].active_id);
    if (k < sizeof(st) && L->host_chosen) k += (size_t)snprintf(st + k, sizeof(st) - k, "host=%s\n", L->hs.list[L->host].hex);
    if (k < sizeof(st)) snprintf(st + k, sizeof(st) - k, "changed=%s\nby=SeedSwap\n", when);
    return write_text(VFS_SD, SS_ROOT "/state.ini", st);
}

const Host *lib_host(const Library *L) { return &L->hs.list[L->host]; }

const Host *lib_game_host(const Library *L, const Game *g) {
    return g->host >= 0 && g->host < L->hs.count ? &L->hs.list[g->host] : NULL;
}

static bool title_in(const char *list, const char *hex) {
    for (const char *p = list; (p = strstr(p, hex)); p += 16)
        if ((p == list || p[-1] == ' ' || p[-1] == ',') && (p[16] == 0 || p[16] == ',' || p[16] == ' ')) return true;
    return false;
}

void lib_load(Library *L) {
    lib_free(L);
    mkdirs(VFS_SD, SS_ROOT "/games");
    // loader capability: an install image built for switching on the console says so
    char *img = read_text(VFS_SD, INSTALL_DIR "/image.lua", 256 * 1024);
    L->live_sizes = img && strstr(img, "live_sizes = true");
    L->image_guard_user[0] = L->image_guard_program[0] = 0;
    const char *gb = img ? strstr(img, "guard = {") : NULL;
    const char *gu = gb ? strstr(gb, "user = \"") : NULL;
    const char *gp = gb ? strstr(gb, "program = \"") : NULL;
    if (gu && strlen(gu + 8) >= 32) snprintf(L->image_guard_user, sizeof(L->image_guard_user), "%.32s", gu + 8);
    if (gp && strlen(gp + 11) >= 16) snprintf(L->image_guard_program, sizeof(L->image_guard_program), "%.16s", gp + 11);
    free(img);
    load_loader_state(L);
    // the hosts; the NPDMs the loader's reports hold for hosts that lack one
    hosts_load(&L->hs);
    hosts_import(&L->hs);
    for (int i = 0; i < L->hs.count; i++) hosts_request(&L->hs.list[i]);   // the loader fills it when that game starts
    read_state(L);
    save_set_title(L->hs.list[L->host].id);
    // the play user: config.ini, else the loader guard's user; never the user SeedSwap runs on
    users_load(L);

    // games parked or described under /seedswap/games/<id>/
    VfsEntry *e;
    int n;
    if (vfs_list(VFS_SD, SS_ROOT "/games", &e, &n) == 0) {
        for (int i = 0; i < n && L->count < MAX_GAMES - 1 - HOSTS_MAX; i++) {
            if (!e[i].is_dir) continue;
            Game *g = &L->games[L->count++];
            memset(g, 0, sizeof(*g));
            g->host = -1;
            char dir[256], p[300];
            snprintf(dir, sizeof(dir), SS_ROOT "/games/%s", e[i].name);
            snprintf(g->id, sizeof(g->id), "%s", e[i].name);
            snprintf(p, sizeof(p), "%s/game.ini", dir);
            char *ini = read_text(VFS_SD, p, 16384);
            if (ini) {
                ini_get(ini, "name", g->name, sizeof(g->name));
                ini_get(ini, "publisher", g->publisher, sizeof(g->publisher));
                ini_get(ini, "version", g->version, sizeof(g->version));
                ini_get(ini, "status", g->status, sizeof(g->status));
                ini_get(ini, "notes", g->notes, sizeof(g->notes));
                ini_get(ini, "source", g->source, sizeof(g->source));
                ini_get(ini, "added", g->added, sizeof(g->added));
                char tid[24];
                if (ini_get(ini, "title_id", tid, sizeof(tid)) && tid[0]) g->title_id = strtoull(tid, NULL, 16);
                g->romfs_size = ini_get_i64(ini, "romfs_size", -1);
                g->exefs_size = ini_get_i64(ini, "exefs_size", -1);
                // version_code is written by prepare_game.py; older entries only have "v<number>"
                g->version_code = ini_get_i64(ini, "version_code", version_code_from_text(g->version));
                ini_get(ini, "previous_version", g->prev_version, sizeof(g->prev_version));
                ini_get(ini, "dlc", g->dlc, sizeof(g->dlc));
                ini_get(ini, "dlc_names", g->dlc_names, sizeof(g->dlc_names));
                char words[96] = "";
                ini_get(ini, "compat_words", words, sizeof(words));
                g->has_compat_words = cw_ini_parse(words, g->compat_words);
                g->save_need = ini_get_i64(ini, "save_size", 0);
                g->journal_need = ini_get_i64(ini, "save_journal", 0);
                free(ini);
            } else g->version_code = -1;
            if (!g->name[0]) memcpy(g->name, g->id, strlen(g->id) + 1);  // id[24] fits name[128]
            if (!g->title_id && strlen(g->id) == 16) g->title_id = strtoull(g->id, NULL, 16);  // ids are program ids
            int64_t rs = -1, es = -1;
            bool d = false;
            snprintf(p, sizeof(p), "%s/romfs.bin", dir);
            bool has_r = vfs_stat(VFS_SD, p, &d, &rs) == 0;
            snprintf(p, sizeof(p), "%s/exefs.nsp", dir);
            bool has_e = vfs_stat(VFS_SD, p, &d, &es) == 0;
            g->parked = has_r && has_e;
            if (g->parked) { g->romfs_size = rs; g->exefs_size = es; }
            snprintf(p, sizeof(p), "%s/" DLC_KEPT_IMAGE, dir);
            g->dlc_removable = g->dlc[0] && vfs_stat(VFS_SD, p, &d, NULL) == 0;
            load_icon(g, dir);
            load_slot(L, g);
        }
        free(e);
    }

    // the hosts' folders
    bool d = false;
    for (int hi = 0; hi < L->hs.count; hi++) {
        Host *h = &L->hs.list[hi];
        char pr[96], pe[96];
        snprintf(pr, sizeof(pr), "%s/romfs.bin", h->dir);
        snprintf(pe, sizeof(pe), "%s/exefs.nsp", h->dir);
        h->overlay_present = vfs_stat(VFS_SD, pr, &d, &h->overlay_romfs) == 0 && vfs_stat(VFS_SD, pe, &d, &h->overlay_exefs) == 0;
        if (!h->overlay_present) continue;
        Game *act = NULL;
        for (int i = 0; i < L->count; i++)
            if (!strcmp(L->games[i].id, h->active_id) && !L->games[i].parked && L->games[i].host < 0 && !L->games[i].unknown)
                act = &L->games[i];
        if (act && (act->romfs_size < 0 || act->romfs_size == h->overlay_romfs)) {
            act->host = hi;
            act->romfs_size = h->overlay_romfs;
            act->exefs_size = h->overlay_exefs;
        } else if (L->count < MAX_GAMES - 1) {
            Game *g = &L->games[L->count++];
            memset(g, 0, sizeof(*g));
            snprintf(g->id, sizeof(g->id), "unknown");
            g->version_code = -1;
            snprintf(g->name, sizeof(g->name), "Unrecognised game");
            snprintf(g->notes, sizeof(g->notes),
                     "SeedSwap doesn't know the game in %s's folder (state.ini says \"%s\"). Push it again from the PC.",
                     h->name, h->active_id[0] ? h->active_id : "nothing");
            g->romfs_size = h->overlay_romfs;
            g->exefs_size = h->overlay_exefs;
            g->unknown = true;
            g->host = hi;
        }
    }
    Host *H = &L->hs.list[L->host];
    L->overlay_present = H->overlay_present;
    L->overlay_romfs = H->overlay_romfs;
    L->overlay_exefs = H->overlay_exefs;
    snprintf(L->active_id, sizeof(L->active_id), "%s", H->active_id);
    for (int i = 0; i < L->count; i++) L->games[i].active = L->games[i].host == L->host;

    // Nintendo's compatibility patches: what is recorded for each game that has its files here
    L->patcher_present = compat_patcher_present();
    for (int i = 0; i < L->count; i++) {
        Game *g = &L->games[i];
        char exefs[256], gdir[160];
        if (compat_paths(L, g, exefs, sizeof(exefs), gdir, sizeof(gdir))) compat_load(exefs, gdir, &g->compat);
    }

    // the host itself (none until a host is picked)
    if (L->host_chosen) {
        Game *s = &L->games[L->count++];
        Host self = *H;   // a copy: the compiler can't tell the host and the entry apart
        memset(s, 0, sizeof(*s));
        builtin_id(&self, s->id, sizeof(s->id));
        s->version_code = -1;
        snprintf(s->name, sizeof(s->name), "%s", self.name);
        snprintf(s->publisher, sizeof(s->publisher), "the host game");
        snprintf(s->notes, sizeof(s->notes), "The host game. Switch to it to play %s itself.",
                 self.id == SEEDSOW_TITLE ? "Seedsow" : self.name);
        s->builtin = true;
        s->host = L->host;
        s->active = !L->overlay_present;
        load_slot(L, s);
    }

    qsort(L->games, (size_t)L->count, sizeof(Game), cmp_games);

    L->journal_pending = vfs_stat(VFS_SD, SS_ROOT "/journal.ini", &d, NULL) == 0;
    L->sd_free = vfs_free_space(VFS_SD);

    // which hosts the play user has a save for; the shown host's save
    Uid users[16];
    for (int hi = 0; hi < L->hs.count; hi++) {
        save_set_title(L->hs.list[hi].id);
        int nu = save_list_users(users, 16);
        for (int i = 0; L->play_user_set && i < nu; i++)
            if (!memcmp(users[i].b, L->play_user.b, 16)) L->hs.list[hi].user_save = true;
    }
    save_set_title(H->id);
    L->play_user_has_save = H->user_save;
    if (L->play_user_has_save && save_mount(&L->play_user, false) == 0) {
        L->current_save_ok = tree_stats(VFS_SAVE, "/", &L->current_save);
        save_unmount();
    }
    load_backups(L);
}

// Where a game's exefs.nsp is now (a host's folder or its own) and where SeedSwap keeps its records.
// False for entries that have no files or no program ID of their own.
static bool compat_paths(const Library *L, const Game *g, char *exefs, size_t en, char *gdir, size_t gn) {
    const Host *h = lib_game_host(L, g);
    if (g->builtin || g->unknown || !g->title_id || !(h || g->parked)) return false;
    snprintf(gdir, gn, SS_ROOT "/games/%s", g->id);
    if (h) snprintf(exefs, en, "%s/exefs.nsp", h->dir);
    else snprintf(exefs, en, "%s/exefs.nsp", gdir);
    return true;
}

// The image in a host's folder must carry that host's own signed NPDM: Loader refuses another's.
// Rewrites main.npdm when it is not the host's (hosts.h exefs_rehost); Nintendo's-patch records follow.
static bool fit_host(Library *L, int host, const char *game_id, const char *exefs, ProgressFn cb, char *err, size_t errn) {
    const Host *h = &L->hs.list[host];
    uint8_t *cur = NULL, *npdm = NULL;
    size_t cn = 0, n = 0;
    if (h->id == SEEDSOW_TITLE) {
        // as before hosts: an image whose NPDM can't be read is moved as it is; one built for Seedsow
        // needs nothing (also when Seedsow's NPDM is nowhere else on the card)
        if (!exefs_read_npdm(exefs, &cur, &cn)) {
            app_log("%s: the code image's main.npdm can't be read; moved to Seedsow as it is", game_id);
            return true;
        }
        bool seedsow = host_npdm_valid(cur, cn);
        free(cur);
        if (seedsow) return true;
    }
    if (!hosts_npdm(h, &npdm, &n, err, errn)) return false;
    char gdir[160], old_key[17] = "", new_key[17] = "";
    snprintf(gdir, sizeof(gdir), SS_ROOT "/games/%s", game_id);
    bool have_key = compat_image_key(exefs, old_key), changed = false;
    bool ok = exefs_rehost(exefs, npdm, n, &changed, cb, err, errn);
    free(npdm);
    if (ok && changed) {
        app_log("%s: code image moved to host %s (%s)", game_id, h->hex, h->name);
        if (have_key && compat_image_key(exefs, new_key)) compat_rekey(gdir, old_key, new_key);
    }
    if (!ok) app_log("ERROR %s", err);
    return ok;
}

bool lib_compat_wanted(const Library *L, int idx) {
    const Game *g = &L->games[idx];
    char exefs[256], gdir[160];
    return L->patcher_present && compat_paths(L, g, exefs, sizeof(exefs), gdir, sizeof(gdir)) &&
           (g->compat.state == COMPAT_ST_UNKNOWN || g->compat.state == COMPAT_ST_PENDING);
}

bool lib_compat(Library *L, int idx, bool apply, ProgressFn cb, char *err, size_t errn) {
    Game *g = &L->games[idx];
    char exefs[256], gdir[160];
    if (!compat_paths(L, g, exefs, sizeof(exefs), gdir, sizeof(gdir))) {
        snprintf(err, errn, "This entry has no game files of its own on the SD card.");
        return false;
    }
    return compat_run(exefs, gdir, g->title_id, apply, &g->compat, cb, err, errn);
}

bool lib_compat_revert(Library *L, int idx, char *err, size_t errn) {
    Game *g = &L->games[idx];
    char exefs[256], gdir[160];
    if (!compat_paths(L, g, exefs, sizeof(exefs), gdir, sizeof(gdir))) {
        snprintf(err, errn, "This entry has no game files of its own on the SD card.");
        return false;
    }
    if (!compat_revert(exefs, gdir, &g->compat, err, errn)) return false;
    // the image put back may have been made for another host
    return g->host < 0 || fit_host(L, g->host, g->id, exefs, NULL, err, errn);
}

const char *lib_dlc_blocker(const Library *L, int idx) {
    const Game *g = &L->games[idx];
    char exefs[256], gdir[160];
    if (!compat_paths(L, g, exefs, sizeof(exefs), gdir, sizeof(gdir))) return "This entry has no game files of its own on the SD card.";
    if (L->journal_pending) return "The last switch didn't finish. Check the Log tab first.";
    if (g->host >= 0 && L->loader == LOADER_FIXED)
        return "Reboot first, or switch to another game, add it, and switch back.";
    if (g->host >= 0 && L->loader == LOADER_OFF && !L->live_sizes)
        return "This loader can't handle the game changing size. Use the PC tools for this game.";
    return NULL;
}

bool lib_dlc_built_in(const Game *g, uint64_t dlc_title_id) {
    char want[20];
    snprintf(want, sizeof(want), "%016llx", (unsigned long long)dlc_title_id);
    for (const char *p = g->dlc; (p = strstr(p, want)); p += 16)
        if ((p == g->dlc || p[-1] == ',') && (p[16] == 0 || p[16] == ',')) return true;
    return false;
}

bool lib_dlc_add(Library *L, int idx, const char *nsp_path, const char *keys_path, const char *const *siblings,
                 int nsiblings, ProgressFn cb, DlcResult *res, char *err, size_t errn) {
    Game *g = &L->games[idx];
    const char *why = lib_dlc_blocker(L, idx);
    if (why) {
        snprintf(err, errn, "%s", why);
        return false;
    }
    char gdir[160], folder[160];
    const Host *h = lib_game_host(L, g);
    snprintf(gdir, sizeof(gdir), SS_ROOT "/games/%s", g->id);
    snprintf(folder, sizeof(folder), "%s", h ? h->dir : gdir);
    return dlc_add(nsp_path, keys_path, folder, gdir, g->title_id, g->version_code, siblings, nsiblings, cb, res, err,
                   errn);
}

bool lib_dlc_remove(Library *L, int idx, char *err, size_t errn) {
    Game *g = &L->games[idx];
    char exefs[256], gdir[160];
    if (!compat_paths(L, g, exefs, sizeof(exefs), gdir, sizeof(gdir))) {
        snprintf(err, errn, "This entry has no game files of its own on the SD card.");
        return false;
    }
    const Host *h = lib_game_host(L, g);
    if (!dlc_remove(h ? h->dir : gdir, gdir, err, errn)) return false;
    // the image without DLC may have been made for another host
    return !h || fit_host(L, g->host, g->id, exefs, NULL, err, errn);
}

// moving: game idx is in another host and would come here with lib_move (its files aren't parked)
static const char *switch_blocker(const Library *L, int idx, bool moving) {
    static char why[320];
    const Game *g = &L->games[idx];
    const Host *H = lib_host(L);
    if (!L->host_chosen) return "Choose a host first: press -.";
    if (g->active) return "This game is already active.";
    if (!moving && !g->builtin && g->host >= 0) {
        snprintf(why, sizeof(why), "It's in %s now. Choose that host (-) and switch it to another game first.",
                 L->hs.list[g->host].name);
        return why;
    }
    if (L->journal_pending) return "The last switch didn't finish. Check the Log tab first.";
    if (L->loader == LOADER_FIXED) return "Reboot and run the launcher (A) first.";
    if (L->loader == LOADER_OFF && !L->live_sizes) return "This loader can't switch games. Install the newer loader.";
    if (L->loader == LOADER_LIVE && L->loader_titles[0] && !title_in(L->loader_titles, H->hex))
        return "Restart and run the launcher (A) first.";
    const char *gprog = L->loader == LOADER_LIVE ? L->loader_guard_program : L->image_guard_program;
    if (!g->builtin && gprog[0] && strcmp(gprog, "ffffffffffffffff") && strcmp(gprog, H->hex)) {
        snprintf(why, sizeof(why), "The loader's save guard doesn't cover %s. Install the image with host slots.", H->name);
        return why;
    }
    if (!L->play_user_set) return "Choose who plays first: Saves tab, Y.";
    const char *guard = L->loader == LOADER_LIVE ? (L->loader_guard ? L->loader_guard_user : "") : L->image_guard_user;
    if (users_guard_follows() && users_guard_differs(L->play_user_hex)) {
        snprintf(why, sizeof(why), "The loader isn't set up for %s yet. Choose the play user again in Saves (Y).",
                 L->play_user_name);
        return why;
    }
    if (guard[0] && strcmp(guard, L->play_user_hex)) {
        if (!users_guard_follows())
            return "The loader is set up for a different user than the play user. Pick that user in Saves (Y).";
        snprintf(why, sizeof(why), "Restart and run the launcher (A) so the loader is set up for %s.", L->play_user_name);
        return why;
    }
    if (lib_active(L) >= 0 && L->games[lib_active(L)].unknown)
        return "The active game isn't known to SeedSwap. Push it again from the PC first so it isn't lost.";
    if (!g->builtin && !g->parked && !moving) return "This game's files are missing from the SD card.";
    if (uid_protected(&L->play_user))
        return "The play user can't be used. Pick another in Saves (Y).";
    if (!L->play_user_has_save) {
        snprintf(why, sizeof(why), "Start %s once on the play user first.", H->name);
        return why;
    }
    if (!g->builtin && !H->npdm) {
        snprintf(why, sizeof(why), "Start %s once from the HOME menu, then open SeedSwap again. If it still says this, "
                 "restart and run the launcher (A) first.", H->name);
        return why;
    }
    return NULL;
}

const char *lib_switch_blocker(const Library *L, int idx) { return switch_blocker(L, idx, false); }

bool lib_host_served(const Library *L, const Host *h) {
    return L->loader != LOADER_LIVE || !L->loader_titles[0] || title_in(L->loader_titles, h->hex);
}

const char *lib_move_blocker(const Library *L, int idx) {
    static char why[320];
    const Game *g = &L->games[idx];
    if (g->builtin || g->unknown || g->host < 0 || g->host == L->host || g->host >= L->hs.count)
        return "This game can't be moved.";
    const Host *from = &L->hs.list[g->host];
    if (L->loader == LOADER_LIVE && L->loader_titles[0] && !title_in(L->loader_titles, from->hex))
        return "Restart and run the launcher (A) first.";
    if (!from->user_save) {
        snprintf(why, sizeof(why), "The play user has no %s save, so its save can't be taken out.", from->name);
        return why;
    }
    return switch_blocker(L, idx, true);
}

static int builtin_index(const Library *L) {
    for (int i = 0; i < L->count; i++)
        if (L->games[i].builtin) return i;
    return -1;
}

static int game_index(const Library *L, const char *id) {
    for (int i = 0; i < L->count; i++)
        if (!L->games[i].builtin && !strcmp(L->games[i].id, id)) return i;
    return -1;
}

bool lib_move(Library *L, int idx, ProgressFn cb, char *err, size_t errn) {
    const char *why = lib_move_blocker(L, idx);
    if (why) { snprintf(err, errn, "%s", why); return false; }
    char id[24], name[128], from_name[64], to_name[64];
    snprintf(id, sizeof(id), "%s", L->games[idx].id);
    snprintf(name, sizeof(name), "%s", L->games[idx].name);
    int from = L->games[idx].host, to = L->host;
    snprintf(from_name, sizeof(from_name), "%s", L->hs.list[from].name);
    snprintf(to_name, sizeof(to_name), "%s", L->hs.list[to].name);
    app_log("move %s from %s to %s", id, L->hs.list[from].hex, L->hs.list[to].hex);
    char extra[120];
    if (lib_host_adds_settings(&L->games[idx], &L->hs.list[to], extra, sizeof(extra)))
        app_log("%s adds Nintendo's settings %s", L->hs.list[to].hex, extra);
    // 1. its host back to itself: the game's files go to SeedSwap's folder, its save to its slot
    lib_set_host(L, from);
    int b = builtin_index(L);
    char e1[300] = "";
    bool ok = b >= 0 && lib_switch(L, b, cb, e1, sizeof(e1));
    if (!ok) {
        lib_set_host(L, to);
        snprintf(err, errn, "%s couldn't leave %s: %s", name, from_name, e1[0] ? e1 : "its host has no entry of its own.");
        app_log("ERROR %s", err);
        return false;
    }
    // 2. into the host shown
    lib_set_host(L, to);
    int k = game_index(L, id);
    char e2[300] = "";
    ok = k >= 0 && lib_switch(L, k, cb, e2, sizeof(e2));
    if (!ok) {
        snprintf(err, errn, "%s left %s but didn't go into %s: %s\n\nIts files and save are kept; switch to it again "
                 "when that is sorted.", name, from_name, to_name, e2[0] ? e2 : "it's no longer in the library.");
        app_log("ERROR %s", err);
        return false;
    }
    app_log("moved %s to %s", id, L->hs.list[L->host].hex);
    return true;
}

static void fail(char *err, size_t n, const char *what) {
    snprintf(err, n, "%s: %s", what, vfs_last_error());
    app_log("ERROR %s", err);
}

// Copy the play user's save to SD:/seedswap/backups/<stamp>-<game>-<uid8>/
static bool backup_save(Library *L, char *out_dir, size_t on, ProgressFn cb, char *err, size_t errn) {
    if (!L->play_user_set) { snprintf(err, errn, "No play user is chosen (Saves tab, Y)."); return false; }
    if (uid_protected(&L->play_user)) { snprintf(err, errn, "refused: protected user"); return false; }
    char stamp[24];
    plat_now(stamp, sizeof(stamp), true);
    int a = lib_active(L);
    snprintf(out_dir, on, SS_ROOT "/backups/%s-%s-%.8s", stamp, a >= 0 ? L->games[a].id : "unknown", L->play_user_hex);
    if (save_mount(&L->play_user, false) != 0) { fail(err, errn, "open the save"); return false; }
    TreeStats st = {0};
    tree_stats(VFS_SAVE, "/", &st);
    int64_t done = 0;
    bool ok = mkdirs(VFS_SD, out_dir) && copy_tree(VFS_SAVE, "/", VFS_SD, out_dir, st.bytes, &done, cb);
    save_unmount();
    TreeStats chk = {0};
    if (ok) ok = tree_stats(VFS_SD, out_dir, &chk) && chk.files == st.files && chk.bytes == st.bytes;
    if (!ok) { fail(err, errn, "back up the save"); return false; }
    app_log("backup %s: %d files, %lld bytes", out_dir, st.files, (long long)st.bytes);
    return true;
}

static int64_t mib_up(int64_t n) { return (n + 0xFFFFF) & ~(int64_t)0xFFFFF; }

// The play user's save must hold what game g may grow to (game.ini save_size, save_journal) and the
// tree at src; its journal must hold the largest file in it, since a file is written within one commit.
// A smaller save is made bigger (it can't be made smaller again). *journal: its journal size afterwards
// (0 when the console can't tell; then the save is written in one commit, as before).
static bool ensure_save_room(Library *L, const Game *g, const char *src, int64_t *journal, char *err, size_t errn) {
    *journal = 0;
    int64_t blocks = 0, largest = 0, data = 0, jour = 0;
    if (src && !tree_save_room(VFS_SD, src, &blocks, &largest)) { fail(err, errn, "read the saved slot"); return false; }
    int64_t need_d = mib_up(blocks + (1 << 20)), need_j = mib_up(largest + (1 << 20));
    if (g && g->save_need > need_d) need_d = mib_up(g->save_need);
    if (g && g->journal_need > need_j) need_j = mib_up(g->journal_need);
    if (save_get_size(&L->play_user, &data, &jour) != 0) {
        app_log("the save's size can't be read (%s); writing it as before", vfs_last_error());
        return true;
    }
    if (need_d <= data && need_j <= jour) {
        *journal = jour;
        return true;
    }
    int64_t to_d = need_d > data ? need_d : data, to_j = need_j > jour ? need_j : jour;
    if (save_extend(&L->play_user, to_d, to_j) != 0 || save_get_size(&L->play_user, &data, &jour) != 0 ||
        data < to_d || jour < to_j) {
        snprintf(err, errn, "This game needs a bigger save (%lld + %lld MiB) and the play user's %s save couldn't be "
                 "made bigger: %s. If the console's storage is full, free some space and try again.",
                 (long long)(to_d >> 20), (long long)(to_j >> 20), lib_host(L)->name, vfs_last_error());
        app_log("ERROR %s", err);
        return false;
    }
    app_log("save of %.8s made bigger for %s: now %lld + %lld MiB", L->play_user_hex, g ? g->id : "a backup",
            (long long)(data >> 20), (long long)(jour >> 20));
    *journal = jour;
    return true;
}

// Replace the play user's save with the tree at src (NULL = empty), for game g (NULL: not known);
// verify. Written in parts that each fit the save's journal, each committed.
static bool restore_save(Library *L, const Game *g, const char *src, ProgressFn cb, char *err, size_t errn) {
    if (!L->play_user_set) { snprintf(err, errn, "No play user is chosen (Saves tab, Y)."); return false; }
    if (uid_protected(&L->play_user)) { snprintf(err, errn, "refused: protected user"); return false; }
    TreeStats want = {0};
    if (src && !tree_stats(VFS_SD, src, &want)) { fail(err, errn, "read the saved slot"); return false; }
    int64_t journal = 0;
    if (!ensure_save_room(L, g, src, &journal, err, errn)) return false;
    if (save_mount(&L->play_user, true) != 0) { fail(err, errn, "open the save for writing"); return false; }
    int64_t done = 0, budget = journal > (2 << 20) ? journal - (1 << 20) : 0;
    bool ok = clear_dir(VFS_SAVE, "/") && (budget <= 0 || save_commit() == 0) &&
              (!src || copy_tree_commit(VFS_SD, src, VFS_SAVE, "/", want.bytes, &done, cb, budget, save_commit)) &&
              save_commit() == 0;
    save_unmount();
    if (!ok) { fail(err, errn, "write the save"); return false; }
    TreeStats got = {0};
    if (save_mount(&L->play_user, false) == 0) {
        tree_stats(VFS_SAVE, "/", &got);
        save_unmount();
    }
    if (got.files != want.files || got.bytes != want.bytes) {
        snprintf(err, errn, "save check failed: %d files/%lld B, expected %d/%lld", got.files, (long long)got.bytes,
                 want.files, (long long)want.bytes);
        app_log("ERROR %s", err);
        return false;
    }
    return true;
}

static void prune_backups(Library *L, int keep) {
    load_backups(L);
    for (int i = keep; i < L->backup_count; i++) {
        char p[256];
        snprintf(p, sizeof(p), SS_ROOT "/backups/%s", L->backups[i].dir);
        vfs_rmtree(VFS_SD, p);
    }
    load_backups(L);
}

bool saves_backup_now(Library *L, const char *why, ProgressFn cb, char *err, size_t errn) {
    char dir[256];
    if (!L->play_user_set) { snprintf(err, errn, "No play user is chosen (Saves tab, Y)."); return false; }
    if (uid_protected(&L->play_user)) { snprintf(err, errn, "refused: protected user"); return false; }
    if (!L->play_user_has_save) { snprintf(err, errn, "The play user has no %s save.", lib_host(L)->name); return false; }
    bool ok = backup_save(L, dir, sizeof(dir), cb, err, errn);
    if (ok) app_log("backup (%s) done", why);
    prune_backups(L, 30);
    return ok;
}

bool saves_restore(Library *L, int bi, ProgressFn cb, char *err, size_t errn) {
    if (bi < 0 || bi >= L->backup_count) return false;
    char src[256], safety[256];
    snprintf(src, sizeof(src), SS_ROOT "/backups/%s", L->backups[bi].dir);
    app_log("restore %s into the save of %s", src, L->play_user_hex);
    if (!backup_save(L, safety, sizeof(safety), cb, err, errn)) return false;
    int a = lib_active(L);
    const Game *g = a >= 0 ? &L->games[a] : NULL;
    if (!restore_save(L, g, src, cb, err, errn)) {
        char e2[160];
        restore_save(L, g, safety, cb, e2, sizeof(e2));
        app_log("restore failed; put back %s", safety);
        return false;
    }
    app_log("restore done (safety backup %s)", safety);
    return true;
}

bool lib_host_adds_settings(const Game *g, const Host *H, char *desc, size_t n) {
    CompatWords host, own;
    char why[200];
    desc[0] = 0;
    if (g->builtin || !g->title_id || cw_for_game(H->id, H->id, 0, &host, why, sizeof(why)) != CW_FOUND) return false;
    uint64_t game[CW_FIELDS] = {0}, extra[CW_FIELDS];
    if (g->has_compat_words) memcpy(game, g->compat_words, sizeof(game));
    else if (cw_for_game(g->title_id, g->title_id, g->version_code > 0 ? (uint64_t)g->version_code : 0, &own, why,
                         sizeof(why)) == CW_FOUND)
        memcpy(game, own.words, sizeof(game));
    bool any = false;
    for (int i = 0; i < CW_FIELDS; i++) {
        extra[i] = host.words[i] & ~game[i];
        any = any || extra[i];
    }
    if (any) cw_describe(extra, desc, n);
    return any;
}

bool lib_switch(Library *L, int idx, ProgressFn cb, char *err, size_t errn) {
    const char *why = lib_switch_blocker(L, idx);
    if (why) { snprintf(err, errn, "%s", why); return false; }
    int ai = lib_active(L);
    Game *from = ai >= 0 ? &L->games[ai] : NULL, *to = &L->games[idx];
    Host *H = &L->hs.list[L->host];
    app_log("switch %s -> %s in %s (user %s)", from ? from->id : "-", to->id, H->hex, L->play_user_hex);
    char extra[120];
    if (lib_host_adds_settings(to, H, extra, sizeof(extra))) app_log("%s adds Nintendo's settings %s", H->hex, extra);
    L->switch_note[0] = 0;
    L->switch_backed_up = false;

    // 0. the incoming game's code: the host's own patches must not touch it, and it must carry the host's NPDM
    if (!to->builtin) {
        char gdir[160], exefs[200];
        snprintf(gdir, sizeof(gdir), SS_ROOT "/games/%s", to->id);
        snprintf(exefs, sizeof(exefs), "%s/exefs.nsp", gdir);
        int changes = 0;
        char e2[300] = "";
        if (!hosts_patch_check(exefs, gdir, H->id, &changes, cb, e2, sizeof(e2))) {
            app_log("host patch check: %s", e2);
            snprintf(L->switch_note, sizeof(L->switch_note), "%s", e2);
        } else if (changes) {
            snprintf(err, errn, "%s has Nintendo patches of its own that would change this game's code. Use another host "
                     "for it.", H->name);
            app_log("ERROR %s", err);
            return false;
        }
        if (!fit_host(L, L->host, to->id, exefs, cb, err, errn)) return false;
    }

    // 1. saves: back up, keep the outgoing game's slot, load the incoming game's slot
    char backup[256], slot_from[256], slot_to[256], tmp[300];
    if (!backup_save(L, backup, sizeof(backup), cb, err, errn)) return false;
    L->switch_backed_up = true;
    if (from) {
        slot_path(L, from->id, slot_from, sizeof(slot_from));
        snprintf(tmp, sizeof(tmp), "%s.new", slot_from);
        vfs_rmtree(VFS_SD, tmp);
        int64_t done = 0;
        TreeStats st = {0};
        tree_stats(VFS_SD, backup, &st);
        if (!copy_tree(VFS_SD, backup, VFS_SD, tmp, st.bytes, &done, cb)) { fail(err, errn, "keep the outgoing save"); return false; }
        vfs_rmtree(VFS_SD, slot_from);
        if (vfs_rename(VFS_SD, tmp, slot_from) != 0) { fail(err, errn, "store the outgoing save"); return false; }
    }
    slot_path(L, to->id, slot_to, sizeof(slot_to));
    bool dir = false;
    bool has_slot = vfs_stat(VFS_SD, slot_to, &dir, NULL) == 0 && dir;
    if (!restore_save(L, to, has_slot ? slot_to : NULL, cb, err, errn)) {
        char e2[160];
        restore_save(L, from, backup, cb, e2, sizeof(e2));
        return false;
    }

    // 2. files, with a journal in case the console stops half way
    char j[512];
    snprintf(j, sizeof(j), "from=%s\nto=%s\nbackup=%s\n", from ? from->id : "", to->id, backup);
    write_text(VFS_SD, SS_ROOT "/journal.ini", j);
    static const char *FILES[] = {"exefs.nsp", "romfs.bin"};
    bool ok = true;
    if (from && !from->builtin) {
        char gdir[160];
        snprintf(gdir, sizeof(gdir), SS_ROOT "/games/%s", from->id);
        mkdirs(VFS_SD, gdir);
        for (int i = 0; i < 2 && ok; i++) {
            char a[256], b[256];
            snprintf(a, sizeof(a), "%s/%s", H->dir, FILES[i]);
            snprintf(b, sizeof(b), "%s/%s", gdir, FILES[i]);
            ok = vfs_rename(VFS_SD, a, b) == 0;
        }
    }
    if (ok && !to->builtin) {
        mkdirs(VFS_SD, H->dir);
        for (int i = 0; i < 2 && ok; i++) {
            char a[256], b[256];
            snprintf(a, sizeof(a), SS_ROOT "/games/%s/%s", to->id, FILES[i]);
            snprintf(b, sizeof(b), "%s/%s", H->dir, FILES[i]);
            ok = vfs_rename(VFS_SD, a, b) == 0;
        }
    }
    if (!ok) { fail(err, errn, "move the game files (journal kept)"); return false; }
    snprintf(H->active_id, sizeof(H->active_id), "%s", to->id);
    write_state(L);
    vfs_delete(VFS_SD, SS_ROOT "/journal.ini");
    prune_backups(L, 30);
    app_log("switch done: %s is active in %s", to->id, H->hex);

    // The incoming game gets Nintendo's patches now if it has not had its look yet. A failure here
    // does not undo the switch: the game is as playable as it was before.
    if (!to->builtin && to->title_id && compat_patcher_present()) {
        char gdir[160], e2[300] = "", exefs[96];
        CompatInfo info;
        snprintf(gdir, sizeof(gdir), SS_ROOT "/games/%s", to->id);
        snprintf(exefs, sizeof(exefs), "%s/exefs.nsp", H->dir);
        compat_load(exefs, gdir, &info);
        if (info.state == COMPAT_ST_UNKNOWN || info.state == COMPAT_ST_PENDING) {
            if (compat_run(exefs, gdir, to->title_id, true, &info, cb, e2, sizeof(e2))) {
                if (info.state == COMPAT_ST_APPLIED)
                    snprintf(L->switch_note, sizeof(L->switch_note),
                             "Nintendo's compatibility patches were applied to it (%d instruction%s).", info.changes,
                             info.changes == 1 ? "" : "s");
            } else {
                snprintf(L->switch_note, sizeof(L->switch_note), "Nintendo's compatibility patches could not be applied: %s", e2);
            }
        }
    }
    return true;
}

// ---- hosts

void lib_set_host(Library *L, int host) {
    if (host < 0 || host >= L->hs.count || (host == L->host && L->host_chosen)) return;
    L->host = host;
    L->host_chosen = true;
    write_state(L);
    app_log("showing host %s (%s)", L->hs.list[host].hex, L->hs.list[host].name);
    lib_load(L);
}

int lib_host_candidates(const Library *L, AppEntry *out, int max) {
    AppEntry *all = malloc(256 * sizeof(AppEntry));
    if (!all) return -1;
    int n = apps_list(all, 256), k = 0, s2 = 0, unknown = 0;
    char left[200] = "";
    for (int i = 0; i < n && k < max; i++) {
        uint64_t id = all[i].id;
        if (hosts_find(&L->hs, id) >= 0 || (id >> 56) != 0x01 || (id & 0xfff)) continue;   // applications only
        if (all[i].platform > 0) {   // a Switch 2 game: no Switch 1 compatibility layer to host a game in
            if (s2++ < 8) snprintf(left + strlen(left), sizeof(left) - strlen(left), " %016llx", (unsigned long long)id);
            continue;
        }
        if (all[i].platform < 0) unknown++;
        out[k++] = all[i];
    }
    free(all);
    static bool logged;
    if (n >= 0 && !logged && (s2 || unknown)) {
        logged = true;
        app_log("host list: %d Switch 2 game(s) left out%s%s; %d game(s) whose platform the console didn't say", s2,
                s2 ? ":" : "", left, unknown);
    }
    return n < 0 ? -1 : k;
}

bool lib_host_add(Library *L, uint64_t id, const char *name, char *err, size_t errn) {
    if (!L->hs.ini || !L->hs.spare_count) {
        snprintf(err, errn, "The install image has no free host slot. Build one with more slots on the PC "
                 "(build_host_image.py).");
        return false;
    }
    if (hosts_find(&L->hs, id) >= 0) {
        snprintf(err, errn, "That game is a host already.");
        return false;
    }
    char stamp[24], backup[96], hex[17], path[96], ini[160];
    plat_now(stamp, sizeof(stamp), true);
    snprintf(backup, sizeof(backup), SS_ROOT "/install-backups/%s", stamp);
    if (!hostimg_retitle(INSTALL_DIR, L->hs.spares[0], id, backup, err, errn)) {
        app_log("ERROR add host %016llx: %s", (unsigned long long)id, err);
        return false;
    }
    hosts_hex(id, hex);
    snprintf(path, sizeof(path), HOSTS_DIR "/%s", hex);
    mkdirs(VFS_SD, path);
    snprintf(path, sizeof(path), HOSTS_DIR "/%s/host.ini", hex);
    snprintf(ini, sizeof(ini), "name=%s\n", name && name[0] ? name : hex);
    write_text(VFS_SD, path, ini);
    app_log("host added: %s (%s)", hex, name ? name : "");
    lib_load(L);
    int i = hosts_find(&L->hs, id);
    if (i >= 0) lib_set_host(L, i);
    return true;
}
