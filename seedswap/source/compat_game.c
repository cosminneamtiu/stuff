// SPDX-License-Identifier: GPL-2.0-only
#include "compat_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/compat.h"
#include "compat/nsofile.h"

#define CHUNK (1024 * 1024)
#define HEADER_MAX 0x8000

static const char *STATE_NAMES[] = {"unknown", "no-patcher", "none", "pending", "applied", "failed"};

bool compat_patcher_present(void) {
    bool dir = false;
    int64_t size = 0;
    return vfs_stat(VFS_SD, COMPAT_PATCHER, &dir, &size) == 0 && !dir && size > 0x100;
}

static void hex16(const uint8_t *digest, char out[17]) {
    for (int i = 0; i < 8; i++)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

static bool read_at(VfsFile *f, int64_t off, void *buf, size_t n) {
    size_t got = 0;
    return vfs_read(f, off, buf, n, &got) == 0 && got == n;
}

// The table of an exefs.nsp: its header bytes, the entries, and which one is main.
typedef struct {
    uint8_t header[HEADER_MAX];
    size_t header_size;
    Pfs0Entry entries[64];
    int count, main_index;
    int64_t file_size;
} Exefs;

static bool exefs_open(VfsFile *f, Exefs *x) {
    x->file_size = vfs_size(f);
    size_t want = x->file_size < HEADER_MAX ? (size_t)x->file_size : HEADER_MAX;
    if (x->file_size < 0x10 || !read_at(f, 0, x->header, want)) return false;
    x->count = pfs0_parse(x->header, want, (uint64_t)x->file_size, x->entries, 64, &x->header_size);
    if (x->count < 0) return false;
    x->main_index = -1;
    for (int i = 0; i < x->count; i++)
        if (!strcmp(x->entries[i].name, "main")) x->main_index = i;
    return x->main_index >= 0 && x->entries[x->main_index].size >= 0x100;
}

// Identifies one build of a game image: its size, its file table, and main's NSO header (which
// holds the hashes of main's code and data).
static bool exefs_key(VfsFile *f, const Exefs *x, char out[17]) {
    uint8_t nso[0x100], digest[32];
    if (!read_at(f, (int64_t)x->entries[x->main_index].offset, nso, sizeof(nso))) return false;
    Sha256 ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)&x->file_size, sizeof(x->file_size));
    sha256_update(&ctx, x->header, x->header_size);
    sha256_update(&ctx, nso, sizeof(nso));
    sha256_final(&ctx, digest);
    hex16(digest, out);
    return true;
}

static bool path_key(const char *exefs_path, char out[17]) {
    VfsFile *f = vfs_open(VFS_SD, exefs_path, false);
    if (!f) return false;
    Exefs *x = malloc(sizeof(*x));
    bool ok = x && exefs_open(f, x) && exefs_key(f, x, out);
    free(x);
    vfs_close(f);
    return ok;
}

static void ini_path(const char *game_dir, char *out, size_t n) { snprintf(out, n, "%s/compat.ini", game_dir); }

bool compat_image_key(const char *exefs_path, char out[17]) { return path_key(exefs_path, out); }

bool compat_rekey(const char *game_dir, const char *old_key, const char *new_key) {
    char path[300], saved[24] = "";
    ini_path(game_dir, path, sizeof(path));
    char *ini = read_text(VFS_SD, path, 4096);
    if (!ini || !ini_get(ini, "key", saved, sizeof(saved)) || strcmp(saved, old_key)) {
        free(ini);
        return false;
    }
    char *at = strstr(ini, "key=");
    while (at && at != ini && at[-1] != '\n') at = strstr(at + 1, "key=");
    bool ok = false;
    if (at && strlen(at + 4) >= 16) {
        memcpy(at + 4, new_key, 16);
        ok = write_text(VFS_SD, path, ini);
    }
    free(ini);
    return ok;
}

static void save_info(const char *game_dir, const CompatInfo *info, const char *key, uint32_t routine) {
    char path[300], text[700], when[32] = "";
    mkdirs(VFS_SD, game_dir);
    ini_path(game_dir, path, sizeof(path));
    plat_now(when, sizeof(when), false);
    snprintf(text, sizeof(text), "state=%s\nchanges=%d\nwidth=%d\npatcher=%s\nroutine=0x%x\nkey=%s\nchecked=%s\nnote=%s\n",
             STATE_NAMES[info->state], info->changes, info->width, info->version, routine, key, when, info->note);
    write_text(VFS_SD, path, text);
}

static bool original_exists(const char *game_dir) {
    char path[300];
    bool dir = false;
    snprintf(path, sizeof(path), "%s/" COMPAT_ORIGINAL, game_dir);
    return vfs_stat(VFS_SD, path, &dir, NULL) == 0 && !dir;
}

void compat_load(const char *exefs_path, const char *game_dir, CompatInfo *out) {
    memset(out, 0, sizeof(*out));
    out->have_original = original_exists(game_dir);
    char path[300], key[17] = "", saved[24] = "", state[24] = "";
    ini_path(game_dir, path, sizeof(path));
    char *ini = read_text(VFS_SD, path, 4096);
    if (ini && path_key(exefs_path, key) && ini_get(ini, "key", saved, sizeof(saved)) && !strcmp(saved, key) &&
        ini_get(ini, "state", state, sizeof(state))) {
        for (int i = 0; i < (int)(sizeof(STATE_NAMES) / sizeof(*STATE_NAMES)); i++)
            if (!strcmp(state, STATE_NAMES[i])) out->state = (CompatState)i;
        out->changes = (int)ini_get_i64(ini, "changes", 0);
        out->width = (int)ini_get_i64(ini, "width", 0);
        ini_get(ini, "patcher", out->version, sizeof(out->version));
        ini_get(ini, "note", out->note, sizeof(out->note));
    }
    free(ini);
    // A game that still needs a look, or that failed for want of the patcher, depends on the file.
    if (!compat_patcher_present() && out->state != COMPAT_ST_APPLIED && out->state != COMPAT_ST_NONE)
        out->state = COMPAT_ST_NO_PATCHER;
    else if (out->state == COMPAT_ST_NO_PATCHER)
        out->state = COMPAT_ST_UNKNOWN;
}

static bool stop(CompatInfo *out, const char *game_dir, const char *key, char *err, size_t errn, const char *what) {
    out->state = COMPAT_ST_FAILED;
    snprintf(out->note, sizeof(out->note), "%s", what);
    snprintf(err, errn, "%s", what);
    if (key && key[0]) save_info(game_dir, out, key, 0);
    app_log("Nintendo patches: %s: %s", game_dir, what);
    return false;
}

// Writes the new image to `part`: the old one with main replaced, at the old size. The hash of what
// was written goes to digest.
static bool write_image(VfsFile *old, const Exefs *x, const uint8_t *new_main, size_t new_main_size, const char *part,
                        uint8_t digest[32], ProgressFn cb) {
    Pfs0Entry entries[64];
    uint64_t total = x->header_size;
    for (int i = 0; i < x->count; i++) {
        entries[i] = x->entries[i];
        if (i == x->main_index) entries[i].size = new_main_size;
        total += entries[i].size;
    }
    if (total > (uint64_t)x->file_size) return false;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return false;
    vfs_delete(VFS_SD, part);
    VfsFile *f = vfs_create(VFS_SD, part, x->file_size) == 0 ? vfs_open(VFS_SD, part, true) : NULL;
    bool ok = f != NULL;
    Sha256 ctx;
    sha256_init(&ctx);
    int64_t at = 0;
    if (ok) {
        size_t header = pfs0_header(x->header, entries, x->count, buf);
        ok = vfs_write(f, 0, buf, header) == 0;
        sha256_update(&ctx, buf, header);
        at = (int64_t)header;
    }
    for (int i = 0; ok && i < x->count; i++) {
        for (uint64_t done = 0; ok && done < entries[i].size;) {
            size_t n = entries[i].size - done > CHUNK ? CHUNK : (size_t)(entries[i].size - done);
            const uint8_t *src = buf;
            if (i == x->main_index) src = new_main + done;
            else ok = read_at(old, (int64_t)(x->entries[i].offset + done), buf, n);
            ok = ok && vfs_write(f, at, src, n) == 0;
            sha256_update(&ctx, src, n);
            at += (int64_t)n;
            done += n;
            if (cb) cb(0.35f + 0.35f * (float)at / (float)x->file_size, "Writing the patched game…");
        }
    }
    memset(buf, 0, CHUNK);
    while (ok && at < x->file_size) {  // the rest is zeros, as in the image it replaces
        size_t n = x->file_size - at > CHUNK ? CHUNK : (size_t)(x->file_size - at);
        ok = vfs_write(f, at, buf, n) == 0;
        sha256_update(&ctx, buf, n);
        at += (int64_t)n;
        if (cb) cb(0.35f + 0.35f * (float)at / (float)x->file_size, "Writing the patched game…");
    }
    ok = ok && vfs_flush(f) == 0;
    if (f) vfs_close(f);
    sha256_final(&ctx, digest);
    free(buf);
    return ok;
}

static bool hash_file(const char *path, int64_t expect_size, uint8_t digest[32], ProgressFn cb) {
    VfsFile *f = vfs_open(VFS_SD, path, false);
    uint8_t *buf = malloc(CHUNK);
    bool ok = f && buf && vfs_size(f) == expect_size;
    Sha256 ctx;
    sha256_init(&ctx);
    for (int64_t at = 0; ok && at < expect_size;) {
        size_t n = expect_size - at > CHUNK ? CHUNK : (size_t)(expect_size - at);
        ok = read_at(f, at, buf, n);
        sha256_update(&ctx, buf, n);
        at += (int64_t)n;
        if (cb) cb(0.7f + 0.28f * (float)at / (float)expect_size, "Reading it back…");
    }
    sha256_final(&ctx, digest);
    free(buf);
    if (f) vfs_close(f);
    return ok;
}

bool compat_run(const char *exefs_path, const char *game_dir, uint64_t program_id, bool apply, CompatInfo *out,
                ProgressFn cb, char *err, size_t errn) {
    memset(out, 0, sizeof(*out));
    out->have_original = original_exists(game_dir);
    void *patcher = NULL;
    size_t patcher_size = 0;
    if (!read_blob(VFS_SD, COMPAT_PATCHER, 8 * 1024 * 1024, &patcher, &patcher_size)) {
        out->state = COMPAT_ST_NO_PATCHER;
        snprintf(err, errn, "The console's patcher is not on the SD card (%s).", COMPAT_PATCHER);
        return false;
    }

    bool ok = false;
    char key[17] = "", old_state[24] = "", old_key[24] = "";
    uint8_t *main_file = NULL, *new_main = NULL;
    size_t new_main_size = 0;
    Exefs *x = malloc(sizeof(*x));
    VfsFile *f = vfs_open(VFS_SD, exefs_path, false);
    if (!x || !f || !exefs_open(f, x) || !exefs_key(f, x, key)) {
        stop(out, game_dir, NULL, err, errn, "The game's exefs.nsp could not be read.");
        goto done;
    }
    {  // what was recorded for this very image, if anything
        char path[300];
        ini_path(game_dir, path, sizeof(path));
        char *ini = read_text(VFS_SD, path, 4096);
        if (ini) {
            ini_get(ini, "state", old_state, sizeof(old_state));
            ini_get(ini, "key", old_key, sizeof(old_key));
            free(ini);
        }
    }
    size_t main_size = (size_t)x->entries[x->main_index].size;
    main_file = malloc(main_size);
    if (!main_file) {
        stop(out, game_dir, key, err, errn, "Not enough memory to load the game's code.");
        goto done;
    }
    for (size_t done = 0; done < main_size;) {
        size_t n = main_size - done > CHUNK ? CHUNK : main_size - done;
        if (!read_at(f, (int64_t)(x->entries[x->main_index].offset + done), main_file + done, n)) {
            stop(out, game_dir, key, err, errn, "The game's code could not be read from the SD card.");
            goto done;
        }
        done += n;
        if (cb) cb(0.1f * (float)done / (float)main_size, "Reading the game's code…");
    }

    if (cb) cb(0.12f, "Running the console's patcher on the game (this can take a minute)…");
    CompatReport report;
    int status = compat_patch_main(patcher, patcher_size, main_file, main_size, program_id, apply, &new_main,
                                   &new_main_size, &report);
    out->width = report.width;
    snprintf(out->version, sizeof(out->version), "%s", report.version);
    if (status != CMPT_OK) {
        char what[260];
        snprintf(what, sizeof(what), "%s%s%s", compat_status_text(status), report.message[0] ? ": " : "", report.message);
        what[0] = (char)(what[0] >= 'a' && what[0] <= 'z' ? what[0] - 32 : what[0]);
        stop(out, game_dir, key, err, errn, what);
        goto done;
    }
    out->changes = (int)report.change_count;
    app_log("Nintendo patches: program %016llx, %d-bit, patcher %s, routine 0x%x: %d instruction(s) to change",
            (unsigned long long)program_id, report.width, report.version, report.routine, out->changes);
    if (!report.change_count) {
        // Nothing left to do: either the game needs nothing, or this image already has the patches.
        bool was_applied = !strcmp(old_state, "applied") && !strcmp(old_key, key);
        out->state = was_applied ? COMPAT_ST_APPLIED : COMPAT_ST_NONE;
        if (was_applied) {
            char path[300];
            ini_path(game_dir, path, sizeof(path));
            char *ini = read_text(VFS_SD, path, 4096);
            if (ini) out->changes = (int)ini_get_i64(ini, "changes", 0);
            free(ini);
        }
        save_info(game_dir, out, key, report.routine);
        ok = true;
        goto done;
    }
    if (!apply) {
        out->state = COMPAT_ST_PENDING;
        save_info(game_dir, out, key, report.routine);
        ok = true;
        goto done;
    }

    {
        char part[300], kept[300];
        uint8_t written[32], read_back[32];
        snprintf(part, sizeof(part), "%s.part", exefs_path);
        snprintf(kept, sizeof(kept), "%s/" COMPAT_ORIGINAL, game_dir);
        if (vfs_free_space(VFS_SD) >= 0 && vfs_free_space(VFS_SD) < x->file_size + 16 * 1024 * 1024) {
            stop(out, game_dir, key, err, errn, "Not enough free space on the SD card for the patched game.");
            goto done;
        }
        if (!write_image(f, x, new_main, new_main_size, part, written, cb)) {
            vfs_delete(VFS_SD, part);
            stop(out, game_dir, key, err, errn, "The patched game could not be written (or no longer fits in its exefs.nsp).");
            goto done;
        }
        if (!hash_file(part, x->file_size, read_back, cb) || memcmp(written, read_back, 32)) {
            vfs_delete(VFS_SD, part);
            stop(out, game_dir, key, err, errn, "The patched game did not read back from the SD card as written.");
            goto done;
        }
        vfs_close(f);
        f = NULL;
        // Swap: the image without the patches is kept once; a later patched image replaces the last one.
        mkdirs(VFS_SD, game_dir);
        bool moved = out->have_original ? vfs_delete(VFS_SD, exefs_path) == 0 : vfs_rename(VFS_SD, exefs_path, kept) == 0;
        if (!moved || vfs_rename(VFS_SD, part, exefs_path) != 0) {
            // Put things back as they were, as far as possible.
            bool dir = false;
            if (vfs_stat(VFS_SD, exefs_path, &dir, NULL) != 0 && !out->have_original) vfs_rename(VFS_SD, kept, exefs_path);
            stop(out, game_dir, key, err, errn, "The patched game could not be moved into place.");
            goto done;
        }
        out->have_original = true;
        out->state = COMPAT_ST_APPLIED;
        char new_key[17] = "";
        path_key(exefs_path, new_key);
        save_info(game_dir, out, new_key, report.routine);
        app_log("Nintendo patches applied: %s (%d instructions); the image before is %s", exefs_path, out->changes, kept);
        if (cb) cb(1.f, "Done");
        ok = true;
    }

done:
    if (f) vfs_close(f);
    free(x);
    free(main_file);
    free(new_main);
    free(patcher);
    return ok;
}

bool compat_revert(const char *exefs_path, const char *game_dir, CompatInfo *out, char *err, size_t errn) {
    char kept[300], aside[300];
    bool dir = false;
    int64_t kept_size = 0, cur_size = 0;
    snprintf(kept, sizeof(kept), "%s/" COMPAT_ORIGINAL, game_dir);
    snprintf(aside, sizeof(aside), "%s.undo", exefs_path);
    if (vfs_stat(VFS_SD, kept, &dir, &kept_size) != 0 || dir) {
        snprintf(err, errn, "The image from before the patches is not on the SD card.");
        return false;
    }
    if (vfs_stat(VFS_SD, exefs_path, &dir, &cur_size) != 0 || cur_size != kept_size) {
        snprintf(err, errn, "The kept image has another size than the current one; left alone.");
        return false;
    }
    vfs_delete(VFS_SD, aside);
    if (vfs_rename(VFS_SD, exefs_path, aside) != 0 || vfs_rename(VFS_SD, kept, exefs_path) != 0) {
        vfs_rename(VFS_SD, aside, exefs_path);
        snprintf(err, errn, "The files could not be moved.");
        return false;
    }
    vfs_delete(VFS_SD, aside);
    memset(out, 0, sizeof(*out));
    char key[17] = "";
    path_key(exefs_path, key);
    out->state = COMPAT_ST_PENDING;
    char path[300];
    ini_path(game_dir, path, sizeof(path));
    char *ini = read_text(VFS_SD, path, 4096);
    if (ini) {
        out->changes = (int)ini_get_i64(ini, "changes", 0);
        out->width = (int)ini_get_i64(ini, "width", 0);
        ini_get(ini, "patcher", out->version, sizeof(out->version));
        free(ini);
    }
    save_info(game_dir, out, key, 0);
    app_log("Nintendo patches removed: %s is the image from before again", exefs_path);
    return true;
}
