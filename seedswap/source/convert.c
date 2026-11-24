// SPDX-License-Identifier: GPL-2.0-only
#include "convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat/nsofile.h"
#include "compat_words.h"
#include "crypto.h"
#include "fixes.h"
#include "nsp.h"
#include "stage.h"
#include "ui.h"

#define DEVICE_SAVE "_ZN2nn2fs19MountDeviceSaveDataEPKc"

// What is known about the programs of titles that hold several (pc/fixes PROGRAM_NOTES)
static const struct {
    uint64_t id;
    const char *name, *note;
} PROGRAM_NOTES[] = {
    {0x010049900f546000ull, "Super Mario 3D All-Stars (menu)", "the launcher menu: on Seedsow it closes about 10 s in"},
    {0x010049900f546001ull, "Super Mario 64 (3D All-Stars)", "needs JIT, which Seedsow cannot give; closes at start"},
    {0x010049900f546002ull, "Super Mario Sunshine (3D All-Stars)", "plays; its code module needs kaiHEN's RO"},
    {0x010049900f546003ull, "Super Mario Galaxy (3D All-Stars)", "32-bit; closes at start, cause unknown"},
};

static int vfs_cb(void *user, uint64_t off, void *buf, size_t n) {
    size_t got = 0;
    return vfs_read((VfsFile *)user, (int64_t)off, buf, n, &got) == 0 && got == n ? 0 : -1;
}

typedef struct {
    VfsFile *f[2];
    Pkg *base, *update;   // update may be == base
} Dumps;

static void dumps_close(Dumps *d) {
    if (d->update && d->update != d->base) pkg_close(d->update);
    pkg_close(d->base);
    for (int i = 0; i < 2; i++)
        if (d->f[i]) vfs_close(d->f[i]);
    memset(d, 0, sizeof(*d));
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool dumps_open(const char *base_path, const char *update_path, const char *keys_path, Dumps *d, char *err,
                       size_t errn) {
    memset(d, 0, sizeof(*d));
    char *keys = read_text(VFS_SD, keys_path, 256 * 1024);
    if (!keys) {
        snprintf(err, errn, "prod.keys is missing: the dumps are encrypted. Put your console's keys file at "
                 "SD:/switch/prod.keys.");
        return false;
    }
    const char *paths[2] = {base_path, update_path && update_path[0] && strcmp(update_path, base_path) ? update_path : NULL};
    char e2[300];
    bool ok = true;
    for (int i = 0; ok && i < 2; i++) {
        if (!paths[i]) continue;
        d->f[i] = vfs_open(VFS_SD, paths[i], false);
        if (!d->f[i]) {
            snprintf(err, errn, "%s cannot be opened (%s).", base_name(paths[i]), vfs_last_error());
            ok = false;
            break;
        }
        Pkg *p = pkg_open(vfs_cb, d->f[i], (uint64_t)vfs_size(d->f[i]), keys, e2, sizeof(e2));
        if (!p) {
            snprintf(err, errn, "%s: %s", base_name(paths[i]), e2);
            ok = false;
            break;
        }
        if (i == 0) d->base = p;
        else d->update = p;
    }
    memset(keys, 0, strlen(keys));
    free(keys);
    if (!ok) {
        dumps_close(d);
        return false;
    }
    if (!d->update && pkg_meta(d->base, META_PATCH, 0) && update_path && update_path[0]) d->update = d->base;
    if (!d->update && pkg_meta(d->base, META_PATCH, 0) && pkg_meta(d->base, META_APPLICATION, 0)) d->update = d->base;
    // roles: a base game, and an update that belongs to it
    char what[300];
    const PkgMeta *app = pkg_meta(d->base, META_APPLICATION, 0);
    if (!app) {
        pkg_describe(d->base, what, sizeof(what));
        snprintf(err, errn, "%s does not contain a base game (it holds: %s).%s", base_name(base_path), what,
                 pkg_meta(d->base, META_PATCH, 0) ? " It is an update: an update alone cannot be converted, it only holds "
                 "the differences. Put the base game's dump on the SD card too."
                 : pkg_meta(d->base, META_AOC, 0) ? " It is add-on content (DLC): add it to its game instead." : "");
        dumps_close(d);
        return false;
    }
    if (d->update) {
        bool belongs = false;
        for (int k = 0; pkg_meta(d->update, META_PATCH, k); k++) belongs |= pkg_meta(d->update, META_PATCH, k)->other_id == app->title_id;
        if (!belongs) {
            pkg_describe(d->update, what, sizeof(what));
            snprintf(err, errn, "The update does not belong to the base game: %s holds %s, but the game is %016llx.",
                     base_name(update_path && update_path[0] ? update_path : base_path), what,
                     (unsigned long long)app->title_id);
            dumps_close(d);
            return false;
        }
    }
    return true;
}

static const PkgMeta *patch_of(const Dumps *d) {
    const PkgMeta *app = pkg_meta(d->base, META_APPLICATION, 0);
    for (int k = 0; d->update && app && pkg_meta(d->update, META_PATCH, k); k++)
        if (pkg_meta(d->update, META_PATCH, k)->other_id == app->title_id) return pkg_meta(d->update, META_PATCH, k);
    return NULL;
}

static int library_index(const Library *L, uint64_t program_id) {
    char id[24];
    snprintf(id, sizeof(id), "%016llx", (unsigned long long)program_id);
    for (int i = 0; i < L->count; i++)
        if (!L->games[i].builtin && !L->games[i].unknown && !strcasecmp(L->games[i].id, id)) return i;
    return -1;
}

bool convert_plan(const Library *L, const char *base_path, const char *update_path, const char *keys_path,
                  ConvertPlan *plan, char *err, size_t errn) {
    memset(plan, 0, sizeof(*plan));
    Dumps d;
    if (!dumps_open(base_path, update_path, keys_path, &d, err, errn)) return false;
    snprintf(plan->base_path, sizeof(plan->base_path), "%s", base_path);
    snprintf(plan->base_file, sizeof(plan->base_file), "%s", base_name(base_path));
    if (d.update) snprintf(plan->update_path, sizeof(plan->update_path), "%s", d.update == d.base ? base_path : update_path);
    const PkgMeta *app = pkg_meta(d.base, META_APPLICATION, 0), *patch = patch_of(&d);
    plan->app_id = app->title_id;
    plan->base_version = app->version;
    plan->has_update = patch != NULL;
    plan->update_version = patch ? patch->version : 0;
    snprintf(plan->base_kind, sizeof(plan->base_kind), "%s", pkg_kind_name(d.base->kind));
    if (d.update) snprintf(plan->update_kind, sizeof(plan->update_kind), "%s", pkg_kind_name(d.update->kind));
    PkgProgram progs[CONVERT_MAX_PROGRAMS];
    int n = pkg_programs(d.base, d.update, progs, CONVERT_MAX_PROGRAMS);
    if (!n) {
        snprintf(err, errn, "The base game's content list names no program; the dump is incomplete.");
        dumps_close(&d);
        return false;
    }
    for (int i = 0; i < n; i++) {
        ConvertProgram *c = &plan->programs[plan->count++];
        c->index = progs[i].index;
        c->program_id = progs[i].program_id;
        c->with_update = progs[i].patch_entry >= 0;
        PkgControl ctl;
        bool have = pkg_control(d.base, c->with_update ? d.update : NULL, c->index, &ctl);
        if (have) {
            if (!plan->display_version[0]) snprintf(plan->display_version, sizeof(plan->display_version), "%s", ctl.display_version);
            snprintf(c->name, sizeof(c->name), "%s", ctl.name);
            pkg_control_free(&ctl);
        } else snprintf(c->name, sizeof(c->name), "%016llx", (unsigned long long)c->program_id);
        for (size_t k = 0; k < sizeof(PROGRAM_NOTES) / sizeof(PROGRAM_NOTES[0]); k++)
            if (PROGRAM_NOTES[k].id == c->program_id) {
                snprintf(c->name, sizeof(c->name), "%s", PROGRAM_NOTES[k].name);
                snprintf(c->note, sizeof(c->note), "%s", PROGRAM_NOTES[k].note);
            }
        if (n > 1 && !c->note[0] && strlen(c->name) + 16 < sizeof(c->name))
            snprintf(c->name + strlen(c->name), sizeof(c->name) - strlen(c->name), " (program %d)", c->index);
        Nca base, upd;
        char e2[200];
        if (nca_open(d.base, progs[i].base_entry, &base, e2, sizeof(e2))) {
            bool up = c->with_update && nca_open(d.update, progs[i].patch_entry, &upd, e2, sizeof(e2));
            stage_romfs_size(&base, up ? &upd : NULL, &c->romfs_size, e2, sizeof(e2));
            if (up) nca_close(&upd);
            nca_close(&base);
        }
        c->library = library_index(L, c->program_id);
        int64_t code = c->with_update ? (int64_t)plan->update_version : (int64_t)plan->base_version;
        c->relation = c->library >= 0 ? lib_version_relation(&L->games[c->library], code) : VER_UNKNOWN;
    }
    plan->sd_free = vfs_free_space(VFS_SD);
    dumps_close(&d);
    return true;
}

const char *convert_blocker(const Library *L, const ConvertPlan *plan, int program, bool allow_older) {
    static char why[400];
    const ConvertProgram *c = &plan->programs[program];
    if (L->journal_pending) return "An unfinished switch needs attention first (see the Log tab).";
    if (c->library >= 0) {
        const Game *g = &L->games[c->library];
        if (c->relation == VER_OLDER && !allow_older)
            return "The library holds a newer version of this game. Its save may not load in an older one.";
        if (!g->active && !g->builtin && g->host >= 0) {
            snprintf(why, sizeof(why), "This game is in %s now. Choose that host (-) first.", lib_game_host(L, g)->name);
            return why;
        }
        if (g->active && L->loader == LOADER_FIXED)
            return "This game is the active one, and the loader in memory has its file sizes fixed. Reboot first (or "
                   "switch to another game, convert, and switch back).";
        if (g->active && L->loader == LOADER_OFF && !L->live_sizes)
            return "This game is the active one, and the install image on the SD card is fixed to its file sizes. Switch "
                   "to another game first.";
    }
    // room: the new game data and code image beside whatever is there
    uint64_t need = c->romfs_size + STAGE_EXEFS_PAD + 64ull * 1024 * 1024;
    if (plan->sd_free >= 0 && (uint64_t)plan->sd_free < need) {
        snprintf(why, sizeof(why), "Not enough room on the SD card: the game needs %.2f GB while it is built, %.2f GB are "
                 "free.%s", (double)need / 1e9, (double)plan->sd_free / 1e9,
                 c->library >= 0 ? " The old version stays until the new one is checked, so both have to fit." : "");
        return why;
    }
    return NULL;
}

// ---- progress over the whole run

static ProgressFn g_cb;
static float g_from, g_to;
static bool scaled(float frac, const char *text) {
    if (!g_cb) return true;
    return g_cb(frac < 0 ? -1.0f : g_from + (g_to - g_from) * frac, text);
}
static void phase(ProgressFn cb, float from, float to) {
    g_cb = cb;
    g_from = from;
    g_to = to;
}

// ---- the host's NPDM (hosts.h): the shown host's, or the one whose folder holds the game

static bool host_npdm(const Library *L, const Game *old, uint8_t **out, size_t *n, char *err, size_t errn) {
    const Host *h = old && old->host >= 0 ? lib_game_host(L, old) : lib_host(L);
    return hosts_npdm(h, out, n, err, errn);
}

// ---- files

static bool exists(const char *path, int64_t *size) {
    bool dir = false;
    return vfs_stat(VFS_SD, path, &dir, size) == 0 && !dir;
}

// SHA-256 of a file on the card, read back in 1 MiB pieces; *size gets its size.
static bool file_sha(const char *path, uint8_t out[32], int64_t *size) {
    VfsFile *f = vfs_open(VFS_SD, path, false);
    uint8_t *buf = malloc(1u << 20);
    int64_t n = f ? vfs_size(f) : -1;
    bool ok = f && buf && n >= 0;
    FastSha256 c;
    fsha256_init(&c);
    for (int64_t at = 0; ok && at < n;) {
        size_t k = n - at < (1 << 20) ? (size_t)(n - at) : (1u << 20), got = 0;
        ok = vfs_read(f, at, buf, k, &got) == 0 && got == k;
        if (ok) fsha256_update(&c, buf, k);
        at += (int64_t)k;
    }
    fsha256_final(&c, out);
    free(buf);
    if (f) vfs_close(f);
    if (size) *size = n;
    return ok;
}

static bool write_checked(const char *path, const uint8_t *data, size_t n, char *err, size_t errn) {
    vfs_delete(VFS_SD, path);
    VfsFile *f = NULL;
    if (vfs_create(VFS_SD, path, (int64_t)n) != 0 || !(f = vfs_open(VFS_SD, path, true)) || vfs_write(f, 0, data, n) != 0 ||
        vfs_flush(f) != 0) {
        snprintf(err, errn, "Writing %s failed (%s).", path, vfs_last_error());
        if (f) vfs_close(f);
        vfs_delete(VFS_SD, path);
        return false;
    }
    vfs_close(f);
    // read back and compare
    uint8_t want[32], got_digest[32];
    int64_t got_size = -1;
    fsha256(data, n, want);
    bool ok = file_sha(path, got_digest, &got_size) && got_size == (int64_t)n;
    if (!ok || memcmp(want, got_digest, 32)) {
        snprintf(err, errn, "%s did not read back as it was written; the SD card may be failing.", path);
        vfs_delete(VFS_SD, path);
        return false;
    }
    return true;
}

static void hexs(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void append(char *dst, size_t n, const char *sep, const char *text) {
    size_t at = strlen(dst);
    if (at + 2 >= n) return;
    snprintf(dst + at, n - at, "%s%s", at ? sep : "", text);
}

// The records that describe one image or the game data, removed with the old version.
static const char *OLD_RECORDS[] = {"compat.ini", "compat.json", "exefs-nocompat.nsp", "exefs-plain.nsp", "exefs-nodlc.nsp",
                                    "exefs-nocompat-nodlc.nsp", "compat-nodlc.ini", "compat-nodlc.json"};

static bool replace_file(const char *part, const char *target, char *err, size_t errn) {
    char old[300];
    snprintf(old, sizeof(old), "%s.old", target);
    vfs_delete(VFS_SD, old);
    bool had = exists(target, NULL);
    if (had && vfs_rename(VFS_SD, target, old) != 0) {
        snprintf(err, errn, "%s could not be moved aside (%s).", target, vfs_last_error());
        return false;
    }
    if (vfs_rename(VFS_SD, part, target) != 0) {
        snprintf(err, errn, "%s could not be put in place (%s).", target, vfs_last_error());
        if (had) vfs_rename(VFS_SD, old, target);
        return false;
    }
    if (had) vfs_delete(VFS_SD, old);
    return true;
}

typedef struct {
    char key[32];
    char value[320];
} IniField;

static void ini_add(IniField *f, int *n, const char *key, const char *value) {
    if (!value || !value[0]) return;
    snprintf(f[*n].key, sizeof(f[*n].key), "%s", key);
    snprintf(f[*n].value, sizeof(f[*n].value), "%s", value);
    for (char *p = f[*n].value; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    (*n)++;
}

static bool ini_write(const char *path, const IniField *f, int nf) {
    size_t cap = 8192, at = 0;
    char *text = malloc(cap);
    if (!text) return false;
    text[0] = 0;
    for (int i = 0; i < nf && at < cap; i++) at += (size_t)snprintf(text + at, cap - at, "%s=%s\n", f[i].key, f[i].value);
    bool ok = write_text(VFS_SD, path, text);
    free(text);
    return ok;
}

// add-on content the game had: built in again from its dumps on the card
static void dlc_again(const char *old_ids, const char *keys_path, const char *folder, const char *gdir, uint64_t program_id,
                      int64_t version_code, ConvertResult *res) {
    if (!old_ids[0]) return;
    NspInfo *list = calloc(MAX_NSPS, sizeof(NspInfo));
    int n = list ? nsp_scan(list, MAX_NSPS) : 0;
    char ids[200];
    snprintf(ids, sizeof(ids), "%s", old_ids);
    const char *siblings[MAX_NSPS];   // the game's DLC dumps on the card: compared with when one is not an original
    int nsib = 0;
    for (int i = 0; i < n; i++)
        if (list[i].kind == NSP_DLC && nsp_base_id(list[i].title_id) == (program_id & ~0xFFFull)) siblings[nsib++] = list[i].path;
    for (char *tok = strtok(ids, ","); tok; tok = strtok(NULL, ",")) {
        uint64_t want = strtoull(tok, NULL, 16);
        int found = -1;
        for (int i = 0; i < n; i++)
            if (list[i].title_id == want && list[i].kind == NSP_DLC) found = i;
        char line[200];
        if (found < 0) {
            snprintf(line, sizeof(line), "%s not added: its dump isn't on the SD card",
                     dlc_known_name(want)[0] ? dlc_known_name(want) : tok);
            append(res->dlc, sizeof(res->dlc), "; ", line);
            continue;
        }
        DlcResult dr;
        char e2[300];
        if (dlc_add(list[found].path, keys_path, folder, gdir, program_id, version_code, siblings, nsib, NULL, &dr, e2,
                    sizeof(e2)))
            snprintf(line, sizeof(line), "%s added again%s", dr.name, dr.warning[0] ? " (not signed by Nintendo)" : "");
        else snprintf(line, sizeof(line), "%s not added: %s", dlc_known_name(want)[0] ? dlc_known_name(want) : tok, e2);
        append(res->dlc, sizeof(res->dlc), "; ", line);
    }
    free(list);
}

bool convert_run(Library *L, const ConvertPlan *plan, int program, bool allow_older, const char *keys_path, ProgressFn cb,
                 ConvertResult *res, char *err, size_t errn) {
    memset(res, 0, sizeof(*res));
    uint64_t started = plat_ms();
    const ConvertProgram *c = &plan->programs[program];
    const char *why = convert_blocker(L, plan, program, allow_older);
    if (why) {
        snprintf(err, errn, "%s", why);
        return false;
    }
    Dumps d;
    if (!dumps_open(plan->base_path, plan->update_path, keys_path, &d, err, errn)) return false;
    PkgProgram progs[CONVERT_MAX_PROGRAMS];
    int np = pkg_programs(d.base, d.update, progs, CONVERT_MAX_PROGRAMS), pi = -1;
    for (int i = 0; i < np; i++)
        if (progs[i].index == c->index) pi = i;
    char id[24], gdir[160], folder[160], path[300], part_romfs[300], part_exefs[300];
    snprintf(id, sizeof(id), "%016llx", (unsigned long long)c->program_id);
    snprintf(gdir, sizeof(gdir), SS_ROOT "/games/%s", id);
    int lib = library_index(L, c->program_id);
    Game *old = lib >= 0 ? &L->games[lib] : NULL;
    snprintf(folder, sizeof(folder), "%s", old && old->active ? lib_host(L)->dir : gdir);
    snprintf(part_romfs, sizeof(part_romfs), "%s/romfs.bin.part", gdir);
    snprintf(part_exefs, sizeof(part_exefs), "%s/exefs.nsp.part", gdir);
    bool new_dir = false, ok = false;
    Nca base = {0}, patch = {0};
    bool have_patch = false;
    StageExefs x = {0};
    FixModule mods[STAGE_MAX_FILES];
    NsoImage nsos[STAGE_MAX_FILES];
    int nmods = 0;
    uint8_t *host = NULL, *npdm = NULL, *image = NULL;
    size_t host_n = 0, unpadded = 0;
    PkgControl ctl = {0};
    bool have_ctl = false;
    char *old_ini = NULL;
    FixRecord fixes[4];
    int nfix = 0;
    if (pi < 0) {
        snprintf(err, errn, "The program is no longer in the dump.");
        goto done;
    }
    if (!nca_open(d.base, progs[pi].base_entry, &base, err, errn)) goto done;
    if (progs[pi].patch_entry >= 0) {
        if (!nca_open(d.update, progs[pi].patch_entry, &patch, err, errn)) goto done;
        have_patch = true;
    }
    if (!host_npdm(L, old, &host, &host_n, err, errn)) goto done;
    if (cb && !cb(0.0f, "Reading the game's code")) {
        snprintf(err, errn, "Stopped.");
        goto done;
    }

    // 1. code: the update's when there is one
    if (!stage_exefs(have_patch ? &patch : &base, c->program_id, &x, err, errn)) goto done;
    for (int i = 0; i < x.count; i++) {
        char e2[160];
        if (nso_load(x.files[i].data, x.files[i].size, &nsos[nmods], e2, sizeof(e2)) != 0) continue;  // not an NSO
        snprintf(mods[nmods].name, sizeof(mods[nmods].name), "%s", x.files[i].name);
        mods[nmods].nso = &nsos[nmods];
        nmods++;
    }
    const NsoImage *main_nso = NULL;
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].name, "main")) main_nso = mods[i].nso;
    if (!main_nso) {
        snprintf(err, errn, "The game's main program is not a readable code module.");
        goto done;
    }
    int bits = nso_is32(main_nso) ? 32 : 64;
    unsigned changed = 0;
    uint32_t text_before[STAGE_MAX_FILES];
    for (int i = 0; i < nmods; i++) text_before[i] = nsos[i].size[0];
    nfix = fixes_apply(c->program_id, mods, nmods, fixes, 4, &changed);
    // Nintendo's settings for the game, from the console's own list (compat_words.c)
    CompatWords words;
    char words_why[200] = "";
    int words_state = cw_for_game(c->program_id, plan->app_id,
                                  have_patch ? (uint64_t)plan->update_version : (uint64_t)plan->base_version, &words,
                                  words_why, sizeof(words_why));
    if (words_state == CW_FOUND && nfix < 4) cw_apply(mods, nmods, &words, &fixes[nfix++], &changed);
    else if (words_state == CW_UNREADABLE) {
        char line[260];
        app_log("convert %s: Nintendo's settings not read: %s", id, words_why);
        snprintf(line, sizeof(line), "Nintendo's settings for this game couldn't be read (%s).", words_why);
        append(res->warnings, sizeof(res->warnings), " ", line);
    } else if (words_state == CW_NONE) app_log("convert %s: Nintendo's settings: %s", id, words_why);
    for (int i = 0; i < nmods; i++) {
        if (!(changed >> i & 1)) continue;
        unsigned segments = nsos[i].size[0] != text_before[i] ? 1u : 0u;  // which segments the fixes touched
        for (int k = 0; k < nfix; k++)
            for (int j = 0; fixes[k].status == FIX_APPLIED && j < fixes[k].count; j++) {
                const FixChange *fc = &fixes[k].changes[j];
                if (strcmp(fc->module, mods[i].name)) continue;
                segments |= fc->addr < nsos[i].mem[1] ? 1u : fc->addr < nsos[i].mem[2] ? 2u : 4u;
            }
        uint8_t *built = NULL;
        size_t built_n = 0;
        if (nso_build(&nsos[i], segments, &built, &built_n) != 0) {
            snprintf(err, errn, "The fixed %s could not be packed again.", mods[i].name);
            goto done;
        }
        if (nsos[i].size[0] != text_before[i]) {  // the text grew (added code): its size field too
            for (int b = 0; b < 4; b++) built[0x18 + b] = (uint8_t)(nsos[i].size[0] >> (8 * b));
        }
        StageFile *sf = stage_file(&x, mods[i].name);
        free(sf->data);
        sf->data = built;
        sf->size = built_n;
    }
    bool device_fix = false;
    for (int i = 0; i < nfix; i++) {
        char line[200];
        if (fixes[i].status == FIX_APPLIED) {
            res->fixes_applied++;
            device_fix |= !strcmp(fixes[i].id, "mk8dx-device-save");
            snprintf(line, sizeof(line), "%s (%d changes%s)", fixes[i].title, fixes[i].count,
                     fixes[i].build_note[0] ? "" : "; this build is new to the fix");
            app_log("convert %s: fix %s applied, %d changes, build %s%s", id, fixes[i].id, fixes[i].count, fixes[i].build,
                    fixes[i].cave_size ? ", code added" : "");
            for (int k = 0; k < fixes[i].count; k++)
                app_log("  %s+%#x: %08x -> %08x %s", fixes[i].changes[k].module, fixes[i].changes[k].addr,
                        fixes[i].changes[k].old_word, fixes[i].changes[k].new_word, fixes[i].changes[k].what);
        } else {
            res->fixes_failed++;
            snprintf(line, sizeof(line), "NOT placed: %s (%s)", fixes[i].title, fixes[i].reason);
            app_log("convert %s: fix %s not placed: %s", id, fixes[i].id, fixes[i].reason);
        }
        append(res->fixes, sizeof(res->fixes), "; ", line);
    }
    if (!stage_npdm(host, host_n, x.game_npdm, x.game_npdm_size, &npdm, err, errn)) goto done;
    if (!stage_image(&x, npdm, host_n, &image, &unpadded, err, errn)) goto done;
    PolicyLine pol[12];
    const Host *ph = old && old->host >= 0 ? lib_game_host(L, old) : lib_host(L);
    int npol = stage_policy(x.game_npdm, x.game_npdm_size, host, host_n,
                            device_fix ? 2 : fixes_imports(main_nso, DEVICE_SAVE) ? 1 : 0, ph->name, pol, 12);
    for (int i = 0; i < npol; i++) {
        app_log("convert %s: %s%s", id, pol[i].level == POLICY_BLOCKER ? "PROBLEM: " : pol[i].level == POLICY_WARNING ? "WARNING: " : "",
                pol[i].text);
        if (pol[i].level >= POLICY_WARNING) append(res->warnings, sizeof(res->warnings), " ", pol[i].text);
    }
    for (int i = 0; i < nfix; i++)
        if (fixes[i].status != FIX_APPLIED) {
            char line[300];
            snprintf(line, sizeof(line), "Fix not applied: %s (%s).", fixes[i].title, fixes[i].reason);
            append(res->warnings, sizeof(res->warnings), " ", line);
        }
    have_ctl = pkg_control(d.base, have_patch ? d.update : NULL, c->index, &ctl);
    // the account save the game may grow to; SeedSwap makes the play user's save this big when the game
    // is switched in (with the device-save fix the device save lives in the account save too)
    uint64_t save_data = 0, save_journal = 0;
    if (have_ctl) {
        save_data = ctl.save_user_max > ctl.save_user ? ctl.save_user_max : ctl.save_user;
        save_journal = ctl.save_user_journal_max > ctl.save_user_journal ? ctl.save_user_journal_max : ctl.save_user_journal;
        if (device_fix) {
            save_data += ctl.save_device_max > ctl.save_device ? ctl.save_device_max : ctl.save_device;
            save_journal += ctl.save_device_journal_max > ctl.save_device_journal ? ctl.save_device_journal_max
                                                                                   : ctl.save_device_journal;
        }
        app_log("convert %s: save %llu + %llu MiB", id, (unsigned long long)(save_data >> 20),
                (unsigned long long)(save_journal >> 20));
    }

    // 2. the game data, then the code image, both as .part beside what is there
    if (!exists(gdir, NULL)) {
        bool dir = false;
        new_dir = vfs_stat(VFS_SD, gdir, &dir, NULL) != 0;
    }
    if (!mkdirs(VFS_SD, gdir)) {
        snprintf(err, errn, "%s could not be created (%s).", gdir, vfs_last_error());
        goto done;
    }
    RomfsResult rr;
    phase(cb, 0.02f, 0.97f);
    if (!stage_romfs(&base, have_patch ? &patch : NULL, part_romfs, scaled, &rr, err, errn)) goto done;
    app_log("convert %s: romfs.bin %llu bytes, %d levels checked, %s, %llu restarts of compressed reads", id,
            (unsigned long long)rr.size, rr.levels, rr.patched ? "base + update" : "base", (unsigned long long)rr.restarts);
    if (cb) cb(0.97f, "Writing the code image");
    if (!write_checked(part_exefs, image, stage_image_size(unpadded), err, errn)) {
        vfs_delete(VFS_SD, part_romfs);
        goto done;
    }

    // 3. in place: the active game's files are in the host's folder; records stay in the game's folder
    if (old && old->active) {
        char what[96];
        if (!saves_backup_now(L, "before its update", NULL, err, errn)) {
            snprintf(what, sizeof(what), "%s", err);
            snprintf(err, errn, "The save could not be backed up before the update, so nothing was changed: %s", what);
            vfs_delete(VFS_SD, part_romfs);
            vfs_delete(VFS_SD, part_exefs);
            goto done;
        }
    }
    snprintf(path, sizeof(path), "%s/game.ini", gdir);
    old_ini = read_text(VFS_SD, path, 16384);
    char old_dlc[200] = "";
    if (old_ini) ini_get(old_ini, "dlc", old_dlc, sizeof(old_dlc));
    snprintf(path, sizeof(path), "%s/romfs.bin", folder);
    if (!replace_file(part_romfs, path, err, errn)) goto done;
    snprintf(path, sizeof(path), "%s/exefs.nsp", folder);
    if (!replace_file(part_exefs, path, err, errn)) goto done;
    for (size_t k = 0; k < sizeof(OLD_RECORDS) / sizeof(OLD_RECORDS[0]); k++) {
        snprintf(path, sizeof(path), "%s/%s", gdir, OLD_RECORDS[k]);
        vfs_delete(VFS_SD, path);
    }
    if (have_ctl && ctl.icon) {
        snprintf(path, sizeof(path), "%s/icon.jpg", gdir);
        vfs_delete(VFS_SD, path);
        VfsFile *f;
        if (vfs_create(VFS_SD, path, (int64_t)ctl.icon_size) == 0 && (f = vfs_open(VFS_SD, path, true))) {
            vfs_write(f, 0, ctl.icon, ctl.icon_size);
            vfs_close(f);
        }
    }

    // 4. game.ini
    int64_t version_code = have_patch ? (int64_t)plan->update_version : (int64_t)plan->base_version;
    char version[40], when[32], num[32], name[128], sha_r[65], sha_e[65], notes[320], source[200], fixes_ids[120] = "";
    char words_ini[96] = "", words_record[20] = "";
    const char *display = have_ctl ? ctl.display_version : "";
    if (display[0]) snprintf(version, sizeof(version), "%s%s", display[0] == 'v' ? "" : "v", display);
    else snprintf(version, sizeof(version), "v%lld", (long long)version_code);
    snprintf(name, sizeof(name), "%s", c->name);
    plat_now(when, sizeof(when), false);
    uint8_t ed[32];
    fsha256(image, stage_image_size(unpadded), ed);
    hexs(rr.sha256, 32, sha_r);
    hexs(ed, 32, sha_e);
    for (int i = 0; i < nfix; i++)
        if (fixes[i].status == FIX_APPLIED) append(fixes_ids, sizeof(fixes_ids), ",", fixes[i].id);
    for (int i = 0; i < nfix; i++)
        if (fixes[i].status == FIX_APPLIED && !strcmp(fixes[i].id, CW_ID)) {
            cw_ini_value(words.words, words_ini, sizeof(words_ini));
            snprintf(words_record, sizeof(words_record), "%016llx", (unsigned long long)words.record_id);
        }
    snprintf(notes, sizeof(notes), "%s%s%s", c->note, c->note[0] && res->warnings[0] ? " " : "", res->warnings);
    snprintf(source, sizeof(source), "SeedSwap %s on the console: %s%s", SEEDSWAP_VERSION, plan->base_file,
             have_patch ? " + update" : "");
    if (have_patch) snprintf(source + strlen(source), sizeof(source) - strlen(source), " v%u", plan->update_version);
    IniField f[32];
    int nf = 0;
    char tid[20], status[24] = "", prev[40] = "", prev_code[24] = "", old_version[32] = "";
    snprintf(tid, sizeof(tid), "%016llX", (unsigned long long)c->program_id);
    int64_t old_code = -1;
    if (old_ini) {
        ini_get(old_ini, "version", old_version, sizeof(old_version));
        old_code = ini_get_i64(old_ini, "version_code", version_code_from_text(old_version));
        if (old_code == version_code) {  // the same version again: its verdict, notes and history stay
            char old_notes[320] = "";
            ini_get(old_ini, "status", status, sizeof(status));
            ini_get(old_ini, "previous_version", prev, sizeof(prev));
            ini_get(old_ini, "previous_version_code", prev_code, sizeof(prev_code));
            if (ini_get(old_ini, "notes", old_notes, sizeof(old_notes)) && old_notes[0])
                snprintf(notes, sizeof(notes), "%s", old_notes);
        } else if (old_version[0]) {
            snprintf(prev, sizeof(prev), "%s", old_version);
            if (old_code >= 0) snprintf(prev_code, sizeof(prev_code), "%lld", (long long)old_code);
            res->replaced = true;
            snprintf(res->replaced_version, sizeof(res->replaced_version), "%s", old_version);
        }
    }
    ini_add(f, &nf, "id", id);
    ini_add(f, &nf, "name", name);
    ini_add(f, &nf, "publisher", have_ctl ? ctl.publisher : "");
    ini_add(f, &nf, "version", version);
    snprintf(num, sizeof(num), "%lld", (long long)version_code);
    ini_add(f, &nf, "version_code", num);
    snprintf(num, sizeof(num), "%u", plan->base_version);
    ini_add(f, &nf, "base_version_code", num);
    ini_add(f, &nf, "title_id", tid);
    snprintf(num, sizeof(num), "%zu", stage_image_size(unpadded));
    ini_add(f, &nf, "exefs_size", num);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)rr.size);
    ini_add(f, &nf, "romfs_size", num);
    snprintf(num, sizeof(num), "%d", bits);
    ini_add(f, &nf, "bits", num);
    if (save_data) {
        snprintf(num, sizeof(num), "%llu", (unsigned long long)save_data);
        ini_add(f, &nf, "save_size", num);
        snprintf(num, sizeof(num), "%llu", (unsigned long long)save_journal);
        ini_add(f, &nf, "save_journal", num);
    }
    ini_add(f, &nf, "source", source);
    ini_add(f, &nf, "added", when);
    ini_add(f, &nf, "fixes", fixes_ids);
    ini_add(f, &nf, "compat_words", words_ini);
    ini_add(f, &nf, "compat_record", words_record);
    ini_add(f, &nf, "status", status);
    ini_add(f, &nf, "notes", notes);
    ini_add(f, &nf, "romfs_sha256", sha_r);
    ini_add(f, &nf, "exefs_sha256", sha_e);
    ini_add(f, &nf, "previous_version", prev);
    ini_add(f, &nf, "previous_version_code", prev_code);
    char ini_path[300];
    snprintf(ini_path, sizeof(ini_path), "%s/game.ini", gdir);
    if (!ini_write(ini_path, f, nf)) {
        snprintf(err, errn, "game.ini could not be written (%s); the game's files are in place.", vfs_last_error());
        goto done;
    }

    // 5. Nintendo's patches (when the patcher is on the card), then add-on content it had
    snprintf(path, sizeof(path), "%s/exefs.nsp", folder);
    if (compat_patcher_present()) {
        CompatInfo info;
        char e2[300];
        phase(cb, 0.97f, 0.99f);
        if (compat_run(path, gdir, c->program_id, true, &info, scaled, e2, sizeof(e2))) {
            if (info.state == COMPAT_ST_APPLIED)
                snprintf(res->compat, sizeof(res->compat), "%d applied", info.changes);
            else snprintf(res->compat, sizeof(res->compat), "none needed");
            // exefs_sha256 describes the image the game runs with, as push_prepared.py records it
            int64_t n = 0;
            if (info.state == COMPAT_ST_APPLIED && file_sha(path, ed, &n)) {
                for (int i = 0; i < nf; i++)
                    if (!strcmp(f[i].key, "exefs_sha256")) hexs(ed, 32, f[i].value);
                if (!ini_write(ini_path, f, nf)) app_log("convert %s: game.ini could not be written again", id);
            }
        } else snprintf(res->compat, sizeof(res->compat), "not applied: %s", e2);
    } else snprintf(res->compat, sizeof(res->compat), "not applied, the patcher file isn't on the SD card");
    dlc_again(old_dlc, keys_path, folder, gdir, c->program_id, version_code, res);

    snprintf(res->id, sizeof(res->id), "%s", id);
    snprintf(res->name, sizeof(res->name), "%s", name);
    snprintf(res->version, sizeof(res->version), "%s", version);
    res->version_code = version_code;
    res->romfs_size = rr.size;
    res->seconds = (int)((plat_ms() - started) / 1000);
    app_log("convert %s: done in %d s: %s %s (version number %lld)%s", id, res->seconds, name, version,
            (long long)version_code, res->replaced ? " (replaced the earlier version)" : "");
    ok = true;
done:
    if (!ok && new_dir) vfs_rmtree(VFS_SD, gdir);
    free(old_ini);
    if (have_ctl) pkg_control_free(&ctl);
    free(image);
    free(npdm);
    free(host);
    for (int i = 0; i < nmods; i++) nso_free(&nsos[i]);
    stage_exefs_free(&x);
    if (have_patch) nca_close(&patch);
    if (base.pkg) nca_close(&base);
    dumps_close(&d);
    phase(NULL, 0, 1);
    return ok;
}
