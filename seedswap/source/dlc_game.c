// SPDX-License-Identifier: GPL-2.0-only
#include "dlc_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aoc.h"
#include "compat/nsofile.h"
#include "compat_game.h"
#include "dlcpkg.h"
#include "dumpcheck.h"

#define CHUNK (1024 * 1024)
#define HEADER_MAX 0x8000

static const struct { uint64_t id; const char *name; } KNOWN[] = {
    {0x0100152000023001ull, "Booster Course Pass"},  // Mario Kart 8 Deluxe
};

const char *dlc_known_name(uint64_t title_id) {
    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(*KNOWN); i++)
        if (KNOWN[i].id == title_id) return KNOWN[i].name;
    return "";
}

static int vfs_cb(void *user, uint64_t off, void *buf, size_t n) {
    size_t got = 0;
    return vfs_read((VfsFile *)user, (int64_t)off, buf, n, &got) != 0 || got != n;
}

static bool read_at(VfsFile *f, int64_t off, void *buf, size_t n) { return vfs_cb(f, (uint64_t)off, buf, n) == 0; }

static void hexs(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static bool unhex(const char *s, uint8_t *out, size_t n) {
    if (strlen(s) < 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

// game.ini: set a key (value NULL removes it). *text is replaced.
static void ini_set(char **text, const char *key, const char *value) {
    const char *t = *text ? *text : "";
    size_t kl = strlen(key), cap = strlen(t) + (value ? strlen(value) : 0) + kl + 4;
    char *out = malloc(cap), *o = out;
    if (!out) return;
    bool done = false;
    for (const char *line = t; *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        bool match = len > kl && !strncmp(line, key, kl) && line[kl] == '=';
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
    free(*text);
    *text = out;
}

static bool exists(const char *path, int64_t *size) {
    bool dir = false;
    int64_t s = 0;
    bool ok = vfs_stat(VFS_SD, path, &dir, &s) == 0 && !dir;
    if (size) *size = s;
    return ok;
}

// ---- one code image: the same image with sdk's four bodies replaced, written to `part`
typedef struct {
    uint8_t header[HEADER_MAX];
    size_t header_size;
    Pfs0Entry entries[64];
    int count;
    int64_t size;
} Image;

static bool load_entry(VfsFile *f, const Image *x, const char *name, uint8_t **data, size_t *size, int *index) {
    for (int i = 0; i < x->count; i++)
        if (!strcmp(x->entries[i].name, name)) {
            *size = (size_t)x->entries[i].size;
            *data = malloc(*size ? *size : 1);
            *index = i;
            return *data && read_at(f, (int64_t)x->entries[i].offset, *data, *size);
        }
    return false;
}

static bool patch_image(const char *src, const char *part, const int *indices, int count, uint8_t digest[32],
                        DlcResult *res, ProgressFn cb, float from, float to, char *err, size_t errn) {
    bool ok = false;
    Image *x = calloc(1, sizeof(Image));
    VfsFile *f = vfs_open(VFS_SD, src, false), *o = NULL;
    uint8_t *main_file = NULL, *sdk_file = NULL, *new_sdk = NULL, *buf = NULL;
    size_t main_size = 0, sdk_size = 0, new_size = 0;
    int mi = -1, si = -1;
    NsoImage main_nso = {0}, sdk_nso = {0};
    char why[200];
    if (!x || !f) { snprintf(err, errn, "%s could not be opened.", src); goto out; }
    x->size = vfs_size(f);
    size_t want = x->size < HEADER_MAX ? (size_t)x->size : HEADER_MAX;
    if (x->size < 0x10 || !read_at(f, 0, x->header, want) ||
        (x->count = pfs0_parse(x->header, want, (uint64_t)x->size, x->entries, 64, &x->header_size)) < 0 ||
        !load_entry(f, x, "main", &main_file, &main_size, &mi) || !load_entry(f, x, "sdk", &sdk_file, &sdk_size, &si)) {
        snprintf(err, errn, "%s is not a game image SeedSwap can read (no main or sdk).", src);
        goto out;
    }
    if (cb) cb(from + 0.1f * (to - from), "Reading the game's code…");
    if (nso_load(main_file, main_size, &main_nso, why, sizeof(why)) || nso_load(sdk_file, sdk_size, &sdk_nso, why, sizeof(why))) {
        snprintf(err, errn, "The game's code could not be loaded: %s", why);
        goto out;
    }
    AocSites s;
    if (!aoc_locate(&sdk_nso, &main_nso, &s, err, errn)) goto out;
    int changed = aoc_apply(&sdk_nso, &s, indices, count, err, errn);
    if (changed < 0) goto out;
    if (cb) cb(from + 0.3f * (to - from), "Checking the new code in the interpreter…");
    if (!aoc_check(&sdk_nso, &s, indices, count, why, sizeof(why))) {
        snprintf(err, errn, "The new sdk code does not behave as intended: %s Nothing was changed.", why);
        goto out;
    }
    res->changed = changed;
    if (nso_build(&sdk_nso, 1, &new_sdk, &new_size)) { snprintf(err, errn, "The new sdk could not be built."); goto out; }
    {   // check: the rebuilt sdk loads, and its code is what was intended
        NsoImage back = {0};
        bool same = !nso_load(new_sdk, new_size, &back, why, sizeof(why)) && back.size[0] == sdk_nso.size[0] &&
                    !memcmp(back.image, sdk_nso.image, back.image_size < sdk_nso.image_size ? back.image_size : sdk_nso.image_size);
        nso_free(&back);
        if (!same) { snprintf(err, errn, "The rebuilt sdk does not read back as intended."); goto out; }
    }
    Pfs0Entry entries[64];
    uint64_t total = x->header_size;
    for (int i = 0; i < x->count; i++) {
        entries[i] = x->entries[i];
        if (i == si) entries[i].size = new_size;
        total += entries[i].size;
    }
    if (total > (uint64_t)x->size) { snprintf(err, errn, "The changed game no longer fits in its %lld-byte image.", (long long)x->size); goto out; }
    buf = malloc(CHUNK);
    vfs_delete(VFS_SD, part);
    o = buf && vfs_create(VFS_SD, part, x->size) == 0 ? vfs_open(VFS_SD, part, true) : NULL;
    if (!o) { snprintf(err, errn, "%s could not be created: %s", part, vfs_last_error()); goto out; }
    Sha256 ctx;
    sha256_init(&ctx);
    size_t hs = pfs0_header(x->header, entries, x->count, buf);
    bool w = vfs_write(o, 0, buf, hs) == 0;
    sha256_update(&ctx, buf, hs);
    int64_t at = (int64_t)hs;
    for (int i = 0; w && i < x->count; i++) {
        for (uint64_t done = 0; w && done < entries[i].size;) {
            size_t n = entries[i].size - done > CHUNK ? CHUNK : (size_t)(entries[i].size - done);
            const uint8_t *data = buf;
            if (i == si) data = new_sdk + done;
            else w = read_at(f, (int64_t)(x->entries[i].offset + done), buf, n);
            w = w && vfs_write(o, at, data, n) == 0;
            sha256_update(&ctx, data, n);
            at += (int64_t)n;
            done += n;
        }
    }
    memset(buf, 0, CHUNK);
    while (w && at < x->size) {
        size_t n = x->size - at > CHUNK ? CHUNK : (size_t)(x->size - at);
        w = vfs_write(o, at, buf, n) == 0;
        sha256_update(&ctx, buf, n);
        at += (int64_t)n;
        if (cb) cb(from + (0.4f + 0.3f * (float)at / (float)x->size) * (to - from), "Writing the new game image…");
    }
    w = w && vfs_flush(o) == 0;
    vfs_close(o);
    o = NULL;
    sha256_final(&ctx, digest);
    if (!w) { snprintf(err, errn, "Writing %s failed: %s", part, vfs_last_error()); goto out; }
    // read back
    {
        VfsFile *r = vfs_open(VFS_SD, part, false);
        uint8_t back[32];
        sha256_init(&ctx);
        bool rb = r && vfs_size(r) == x->size;
        for (int64_t p = 0; rb && p < x->size;) {
            size_t n = x->size - p > CHUNK ? CHUNK : (size_t)(x->size - p);
            rb = read_at(r, p, buf, n);
            sha256_update(&ctx, buf, n);
            p += (int64_t)n;
            if (cb) cb(from + (0.7f + 0.3f * (float)p / (float)x->size) * (to - from), "Reading it back…");
        }
        sha256_final(&ctx, back);
        if (r) vfs_close(r);
        if (!rb || memcmp(back, digest, 32)) { snprintf(err, errn, "%s did not read back from the SD card as written.", part); goto out; }
    }
    ok = true;
out:
    if (!ok) vfs_delete(VFS_SD, part);
    if (o) vfs_close(o);
    if (f) vfs_close(f);
    nso_free(&main_nso);
    nso_free(&sdk_nso);
    free(main_file);
    free(sdk_file);
    free(new_sdk);
    free(buf);
    free(x);
    return ok;
}

// The DLC images already built into the game (to build them in again next to a new one).
typedef struct {
    int index;
    uint8_t *image;
    size_t size;
    char name[16];
    int is_signed;          // from game.ini dlc_signed: 1 / 0; -1 when game.ini does not say (older record)
    char label[40];
} Built;

static bool id_listed(const char *list, uint64_t id) {
    char want[20];
    snprintf(want, sizeof(want), "%016llx", (unsigned long long)id);
    for (const char *p = list; (p = strstr(p, want)); p += 16)
        if ((p == list || p[-1] == ',') && (p[16] == 0 || p[16] == ',')) return true;
    return false;
}

// Whether the new dump may go in (dumpcheck.h): it is compared with the packs built in already whose
// signature is known, and with the other dumps of the same game on the card (read with the keys).
static DlcVerdict judge_new(const DlcPackage *pkg, const char *label, const Built *built, int nbuilt,
                            const char *const *siblings, int nsiblings, const char *nsp_path, const char *keys,
                            char *msg, size_t msgn) {
    DlcPack me = {label, pkg->image, pkg->image_size, pkg->is_signed ? 1 : 0};
    if (pkg->is_signed) { msg[0] = 0; return DLC_OK; }
    DlcPack others[AOC_MAX_ADD_ONS + 8];
    DlcPackage read[8];
    int n = 0, nread = 0;
    for (int i = 0; i < nbuilt; i++) others[n++] = (DlcPack){built[i].label, built[i].image, built[i].size, built[i].is_signed};
    for (int i = 0; i < nsiblings && nread < 8; i++) {
        if (!siblings[i] || !strcmp(siblings[i], nsp_path)) continue;
        VfsFile *f = vfs_open(VFS_SD, siblings[i], false);
        if (!f) continue;
        char e[200];
        DlcPackage *p = &read[nread];
        bool ok = dlcpkg_read(vfs_cb, f, (uint64_t)vfs_size(f), keys, p, e, sizeof(e));
        vfs_close(f);
        if (!ok) continue;
        if (p->application_id != pkg->application_id || p->title_id == pkg->title_id) { dlcpkg_free(p); continue; }
        others[n++] = (DlcPack){"another pack", p->image, p->image_size, p->is_signed ? 1 : 0};
        nread++;
    }
    DlcVerdict v = dlc_judge(&me, others, n, msg, msgn);
    for (int i = 0; i < nread; i++) dlcpkg_free(&read[i]);
    return v;
}

static int parse_ids(const char *list, uint64_t *ids, int max) {
    int n = 0;
    for (const char *p = list; p && *p && n < max;) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        ids[n++] = strtoull(p, NULL, 16);
        p = strchr(p, ',');
    }
    return n;
}

static bool sha_region(VfsFile *f, int64_t off, int64_t size, uint8_t out[32]) {
    uint8_t *buf = malloc(CHUNK);
    Sha256 ctx;
    sha256_init(&ctx);
    bool ok = buf != NULL;
    for (int64_t p = 0; ok && p < size;) {
        size_t n = size - p > CHUNK ? CHUNK : (size_t)(size - p);
        ok = read_at(f, off + p, buf, n);
        sha256_update(&ctx, buf, n);
        p += (int64_t)n;
    }
    sha256_final(&ctx, out);
    free(buf);
    return ok;
}

bool dlc_add(const char *nsp_path, const char *keys_path, const char *folder, const char *game_dir, uint64_t program_id,
             int64_t version_code, const char *const *siblings, int nsiblings, ProgressFn cb, DlcResult *out, char *err,
             size_t errn) {
    memset(out, 0, sizeof(*out));
    bool ok = false, header_written = false;
    char *keys = NULL, *ini = NULL;
    DlcPackage pkg = {0};
    Built built[AOC_MAX_ADD_ONS];
    int nbuilt = 0;
    RomfsPlan plan = {0};
    VfsFile *rf = NULL;
    char exefs[300], romfs[300], ini_path[300], part[300], nocompat[300], nocompat_part[300], kept[300], hex[200];
    snprintf(exefs, sizeof(exefs), "%s/exefs.nsp", folder);
    snprintf(romfs, sizeof(romfs), "%s/romfs.bin", folder);
    snprintf(ini_path, sizeof(ini_path), "%s/game.ini", game_dir);
    snprintf(part, sizeof(part), "%s/exefs.nsp.part", folder);
    snprintf(nocompat, sizeof(nocompat), "%s/" COMPAT_ORIGINAL, game_dir);
    snprintf(nocompat_part, sizeof(nocompat_part), "%s/" COMPAT_ORIGINAL ".part", game_dir);
    snprintf(kept, sizeof(kept), "%s/" DLC_KEPT_IMAGE, game_dir);
    uint8_t old_header[0x50], base_header[0x50];

    // 1. the dump
    if (cb) cb(0.01f, "Reading the add-on content…");
    keys = read_text(VFS_SD, keys_path, 256 * 1024);
    if (!keys) { snprintf(err, errn, "The keys file %s could not be read.", keys_path); goto out; }
    VfsFile *nf = vfs_open(VFS_SD, nsp_path, false);
    if (!nf) { snprintf(err, errn, "%s could not be opened.", nsp_path); goto out; }
    bool read_ok = dlcpkg_read(vfs_cb, nf, (uint64_t)vfs_size(nf), keys, &pkg, err, errn);
    vfs_close(nf);
    if (!read_ok) goto out;
    snprintf(out->title_id, sizeof(out->title_id), "%016llx", (unsigned long long)pkg.title_id);
    out->index = (int)((int64_t)pkg.title_id - (int64_t)(pkg.application_id + 0x1000));
    out->version = pkg.version;
    out->required_version = pkg.required_version;
    const char *known = dlc_known_name(pkg.title_id);
    if (known[0]) snprintf(out->name, sizeof(out->name), "%s", known);
    else snprintf(out->name, sizeof(out->name), "add-on content %d", out->index);
    if (pkg.application_id != (program_id & ~0xFFFull)) {
        snprintf(err, errn, "This add-on content is for the game %016llx, not for this one (%016llx).",
                 (unsigned long long)pkg.application_id, (unsigned long long)program_id);
        goto out;
    }
    if (out->index < 0 || out->index > AOC_MAX_INDEX) {
        snprintf(err, errn, "The add-on content's ID %s is not one of this game's add-on content IDs.", out->title_id);
        goto out;
    }
    if (version_code >= 0 && pkg.required_version > version_code) {
        snprintf(err, errn, "%s needs version %u or later of the game; the library holds version %lld. Update the game "
                 "first (prepare_game.py with the update, or the console converter once it handles updates).",
                 out->name, pkg.required_version, (long long)version_code);
        goto out;
    }

    // 2. what is built in already
    ini = read_text(VFS_SD, ini_path, 16384);
    if (!ini) ini = strdup("");
    char list[200] = "", base_hex[200] = "", signed_list[200] = "";
    ini_get(ini, "dlc", list, sizeof(list));
    bool signed_known = ini_get(ini, "dlc_signed", signed_list, sizeof(signed_list));
    uint64_t ids[AOC_MAX_ADD_ONS];
    int nids = parse_ids(list, ids, AOC_MAX_ADD_ONS - 1);
    for (int i = 0; i < nids; i++)
        if (ids[i] == pkg.title_id) {
            snprintf(err, errn, "%s is already built into this game.", out->name);
            goto out;
        }
    rf = vfs_open(VFS_SD, romfs, true);
    int64_t romfs_size = rf ? vfs_size(rf) : -1;
    if (!rf || romfs_size < 0x50 || !read_at(rf, 0, old_header, 0x50)) {
        snprintf(err, errn, "The game's romfs.bin could not be opened for writing: %s", vfs_last_error());
        goto out;
    }
    memcpy(base_header, old_header, 0x50);
    if (nids) {
        if (!ini_get(ini, "romfs_base_header", base_hex, sizeof(base_hex)) || !unhex(base_hex, base_header, 0x50)) {
            snprintf(err, errn, "game.ini does not record romfs.bin's header from before its add-on content, so another one "
                     "cannot be added here. Add them together on the PC (prepare_game.py).");
            goto out;
        }
        for (int i = 0; i < nids; i++) {
            Built *b = &built[nbuilt];
            b->index = (int)((int64_t)ids[i] - (int64_t)(pkg.application_id + 0x1000));
            aoc_image_name(b->index, b->name);
            char path[64];
            snprintf(path, sizeof(path), "/" AOC_DIRECTORY "/%s", b->name);
            uint64_t at = 0, len = 0;
            if (!romfs_find(vfs_cb, rf, old_header, path, &at, &len) || len > DLCPKG_MAX_IMAGE ||
                !(b->image = malloc(len ? (size_t)len : 1)) || !read_at(rf, (int64_t)at, b->image, (size_t)len)) {
                snprintf(err, errn, "The add-on content already built in (%s) could not be read back from romfs.bin.", path);
                goto out;
            }
            b->size = (size_t)len;
            b->is_signed = signed_known ? id_listed(signed_list, ids[i]) : -1;
            const char *kn = dlc_known_name(ids[i]);
            if (kn[0]) snprintf(b->label, sizeof(b->label), "%s", kn);
            else snprintf(b->label, sizeof(b->label), "add-on content %d", b->index);
            nbuilt++;
        }
    }
    // a dump that is not an original: refused when it lacks what the game's original packs hold
    {
        if (cb) cb(0.03f, "Checking that the dump is an original…");
        char msg[sizeof(out->warning)];
        DlcVerdict v = judge_new(&pkg, out->name, built, nbuilt, siblings, nsiblings, nsp_path, keys, msg, sizeof(msg));
        if (v == DLC_REFUSE) {
            snprintf(err, errn, "%s", msg);
            goto out;
        }
        if (v == DLC_WARN) {
            snprintf(out->warning, sizeof(out->warning), "%s", msg);
            app_log("DLC: %s", msg);
        }
    }
    memset(keys, 0, strlen(keys));
    out->first = nids == 0;
    int indices[AOC_MAX_ADD_ONS];
    RomfsNewFile files[AOC_MAX_ADD_ONS];
    for (int i = 0; i < nbuilt; i++) {
        indices[i] = built[i].index;
        files[i] = (RomfsNewFile){built[i].name, built[i].image, built[i].size};
    }
    char new_name[16];
    aoc_image_name(out->index, new_name);
    indices[nbuilt] = out->index;
    files[nbuilt] = (RomfsNewFile){new_name, pkg.image, pkg.image_size};
    int count = nbuilt + 1;
    // sort by index, as the PC tools do
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && indices[j] < indices[j - 1]; j--) {
            int t = indices[j]; indices[j] = indices[j - 1]; indices[j - 1] = t;
            RomfsNewFile tf = files[j]; files[j] = files[j - 1]; files[j - 1] = tf;
        }

    // 3. the RomFS plan (from the tables of the game without DLC)
    if (!romfs_plan(vfs_cb, rf, base_header, (uint64_t)romfs_size, AOC_DIRECTORY, files, count, &plan, err, errn)) goto out;
    out->tail_bytes = (int64_t)plan.tail_size;
    int64_t free_space = vfs_free_space(VFS_SD);
    if (free_space >= 0 && free_space < (int64_t)plan.tail_size + 2 * 33 * 1024 * 1024 + 16 * 1024 * 1024) {
        snprintf(err, errn, "Not enough free space on the SD card (needs about %lld MB).",
                 (long long)((plan.tail_size + 82 * 1024 * 1024) >> 20));
        goto out;
    }

    // 4. the code images, written beside the old ones and read back
    uint8_t digest[32], digest_nc[32];
    if (!patch_image(exefs, part, indices, count, digest, out, cb, 0.05f, 0.45f, err, errn)) goto out;
    bool have_nocompat = exists(nocompat, NULL);
    if (have_nocompat && !patch_image(nocompat, nocompat_part, indices, count, digest_nc, out, cb, 0.45f, 0.8f, err, errn)) {
        vfs_delete(VFS_SD, part);
        goto out;
    }

    // 5. romfs.bin: the tail, then the header; both read back
    if (cb) cb(0.82f, "Appending the add-on content to romfs.bin…");
    {
        uint8_t want[32], got[32], head[0x50];
        bool w = vfs_write(rf, romfs_size, plan.tail, plan.tail_size) == 0 && vfs_flush(rf) == 0;
        sha256(plan.tail, plan.tail_size, want);
        w = w && vfs_size(rf) == (int64_t)plan.new_size && sha_region(rf, romfs_size, (int64_t)plan.tail_size, got) &&
            !memcmp(want, got, 32);
        if (!w) {
            snprintf(err, errn, "The add-on content could not be appended to romfs.bin (or did not read back); the game "
                     "is unchanged.");
            goto undo;
        }
        if (cb) cb(0.9f, "Pointing romfs.bin at the new file tables…");
        header_written = true;
        w = vfs_write(rf, 8, plan.new_header + 8, 0x48) == 0 && vfs_flush(rf) == 0 && read_at(rf, 0, head, 0x50) &&
            !memcmp(head, plan.new_header, 0x50);
        for (int i = 0; w && i < count; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/" AOC_DIRECTORY "/%s", files[i].name);
            uint64_t at = 0, len = 0;
            uint8_t *back = NULL;
            w = romfs_find(vfs_cb, rf, plan.new_header, path, &at, &len) && len == files[i].size &&
                (back = malloc(len ? (size_t)len : 1)) && read_at(rf, (int64_t)at, back, (size_t)len) &&
                !memcmp(back, files[i].data, (size_t)len);
            free(back);
        }
        // the new tables hold the game's own files and the new ones
        w = w && romfs_count_files(vfs_cb, rf, plan.new_header) == romfs_count_files(vfs_cb, rf, base_header) + count;
        if (!w) {
            snprintf(err, errn, "romfs.bin does not read back as intended after the header was rewritten; the old "
                     "header was put back and the game is unchanged.");
            goto undo;
        }
    }

    // 6. swap the images in; keep the ones without DLC the first time
    if (cb) cb(0.95f, "Putting the new game image in place…");
    {
        char old_key[17] = "", new_key[17] = "";
        compat_image_key(exefs, old_key);
        mkdirs(VFS_SD, game_dir);
        bool moved = out->first && !exists(kept, NULL) ? vfs_rename(VFS_SD, exefs, kept) == 0 : vfs_delete(VFS_SD, exefs) == 0;
        if (!moved || vfs_rename(VFS_SD, part, exefs) != 0) {
            if (!exists(exefs, NULL) && out->first) vfs_rename(VFS_SD, kept, exefs);
            snprintf(err, errn, "The new game image could not be moved into place.");
            goto undo;
        }
        if (have_nocompat) {
            char kept_nc[300];
            snprintf(kept_nc, sizeof(kept_nc), "%s/exefs-nocompat-nodlc.nsp", game_dir);
            if (out->first && !exists(kept_nc, NULL)) vfs_rename(VFS_SD, nocompat, kept_nc);
            else vfs_delete(VFS_SD, nocompat);
            vfs_rename(VFS_SD, nocompat_part, nocompat);
        }
        if (out->first) {  // Nintendo's patch records of the image without DLC go with it
            static const char *RECORDS[][2] = {{"compat.ini", "compat-nodlc.ini"}, {"compat.json", "compat-nodlc.json"}};
            for (int i = 0; i < 2; i++) {
                char from[300], to[300];
                snprintf(from, sizeof(from), "%s/%s", game_dir, RECORDS[i][0]);
                snprintf(to, sizeof(to), "%s/%s", game_dir, RECORDS[i][1]);
                char *t = exists(to, NULL) ? NULL : read_text(VFS_SD, from, 64 * 1024);
                if (t) write_text(VFS_SD, to, t);
                free(t);
            }
        }
        if (compat_image_key(exefs, new_key) && old_key[0]) compat_rekey(game_dir, old_key, new_key);
    }

    // 7. game.ini
    {
        char ids_text[200] = "", names[200] = "", v[64];
        for (int i = 0; i < count; i++) {
            uint64_t id = pkg.application_id + 0x1000 + (uint64_t)indices[i];
            const char *n = dlc_known_name(id);
            char fallback[40];
            snprintf(fallback, sizeof(fallback), "add-on content %d", indices[i]);
            snprintf(ids_text + strlen(ids_text), sizeof(ids_text) - strlen(ids_text), "%s%016llx", i ? "," : "",
                     (unsigned long long)id);
            snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", i ? ", " : "", n[0] ? n : fallback);
        }
        ini_set(&ini, "dlc", ids_text);
        ini_set(&ini, "dlc_names", names);
        // which of them are original dumps (signed by Nintendo), for the next one's check
        char signed_ids[200] = "";
        for (int i = 0; i < count; i++) {
            uint64_t id = pkg.application_id + 0x1000 + (uint64_t)indices[i];
            bool s = id == pkg.title_id ? pkg.is_signed : signed_known && id_listed(signed_list, id);
            if (s)
                snprintf(signed_ids + strlen(signed_ids), sizeof(signed_ids) - strlen(signed_ids), "%s%016llx",
                         signed_ids[0] ? "," : "", (unsigned long long)id);
        }
        if (signed_known || nids == 0) ini_set(&ini, "dlc_signed", signed_ids);
        snprintf(v, sizeof(v), "%lld", (long long)plan.new_size);
        ini_set(&ini, "romfs_size", v);
        char have[200] = "";
        if (!ini_get(ini, "romfs_base_header", have, sizeof(have)) || !have[0]) {
            hexs(old_header, 0x50, hex);
            ini_set(&ini, "romfs_base_header", hex);
            snprintf(v, sizeof(v), "%lld", (long long)romfs_size);
            ini_set(&ini, "romfs_base_size", v);
        }
        ini_set(&ini, "romfs_sha256", NULL);  // the whole file was not hashed here
        hexs(digest, 32, hex);
        ini_set(&ini, "exefs_sha256", hex);
        if (!write_text(VFS_SD, ini_path, ini)) app_log("DLC: game.ini could not be written");
    }
    app_log("DLC: %s (%s, number %d) built into %016llx: %d sdk instructions, %lld bytes appended to romfs.bin",
            out->name, out->title_id, out->index, (unsigned long long)program_id, out->changed, (long long)out->tail_bytes);
    if (cb) cb(1.f, "Done");
    ok = true;
    goto out;

undo:
    if (header_written && rf) {
        vfs_write(rf, 8, old_header + 8, 0x48);
        vfs_flush(rf);
    }
    vfs_delete(VFS_SD, part);
    vfs_delete(VFS_SD, nocompat_part);
out:
    if (rf) vfs_close(rf);
    for (int i = 0; i < nbuilt; i++) free(built[i].image);
    romfs_plan_free(&plan);
    dlcpkg_free(&pkg);
    if (keys) {
        memset(keys, 0, strlen(keys));
        free(keys);
    }
    free(ini);
    if (!ok) app_log("DLC: not built in: %s", err);
    return ok;
}

bool dlc_remove(const char *folder, const char *game_dir, char *err, size_t errn) {
    char exefs[300], kept[300], aside[300], ini_path[300], romfs[300];
    snprintf(exefs, sizeof(exefs), "%s/exefs.nsp", folder);
    snprintf(kept, sizeof(kept), "%s/" DLC_KEPT_IMAGE, game_dir);
    snprintf(aside, sizeof(aside), "%s/exefs.nsp.undo", folder);
    snprintf(ini_path, sizeof(ini_path), "%s/game.ini", game_dir);
    snprintf(romfs, sizeof(romfs), "%s/romfs.bin", folder);
    int64_t ks = 0, cs = 0;
    if (!exists(kept, &ks) || !exists(exefs, &cs) || ks != cs) {
        snprintf(err, errn, "The game image without add-on content (%s) is not on the SD card, so there is nothing to "
                 "go back to.", kept);
        return false;
    }
    char *ini = read_text(VFS_SD, ini_path, 16384);
    char base_hex[200] = "";
    uint8_t base[0x50];
    bool have_header = ini && ini_get(ini, "romfs_base_header", base_hex, sizeof(base_hex)) && unhex(base_hex, base, 0x50);
    vfs_delete(VFS_SD, aside);
    if (vfs_rename(VFS_SD, exefs, aside) != 0 || vfs_rename(VFS_SD, kept, exefs) != 0) {
        vfs_rename(VFS_SD, aside, exefs);
        free(ini);
        snprintf(err, errn, "The files could not be moved.");
        return false;
    }
    vfs_delete(VFS_SD, aside);
    static const char *BACK[][2] = {{"compat-nodlc.ini", "compat.ini"}, {"compat-nodlc.json", "compat.json"},
                                    {"exefs-nocompat-nodlc.nsp", COMPAT_ORIGINAL}};
    for (int i = 0; i < 3; i++) {
        char from[300], to[300];
        snprintf(from, sizeof(from), "%s/%s", game_dir, BACK[i][0]);
        snprintf(to, sizeof(to), "%s/%s", game_dir, BACK[i][1]);
        if (!exists(from, NULL)) continue;
        vfs_delete(VFS_SD, to);
        vfs_rename(VFS_SD, from, to);
    }
    if (have_header) {  // romfs.bin: its old header back; the tail stays, unused
        VfsFile *rf = vfs_open(VFS_SD, romfs, true);
        uint8_t head[0x50];
        bool w = rf && vfs_write(rf, 8, base + 8, 0x48) == 0 && vfs_flush(rf) == 0 && read_at(rf, 0, head, 0x50) &&
                 !memcmp(head, base, 0x50);
        if (rf) vfs_close(rf);
        if (!w) app_log("DLC removed, but romfs.bin's old header could not be written back (the game still works)");
    }
    if (ini) {
        ini_set(&ini, "dlc", NULL);
        ini_set(&ini, "dlc_names", NULL);
        ini_set(&ini, "dlc_signed", NULL);
        ini_set(&ini, "romfs_sha256", NULL);
        ini_set(&ini, "exefs_sha256", NULL);
        write_text(VFS_SD, ini_path, ini);
        free(ini);
    }
    app_log("DLC removed: %s is the image without add-on content again", exefs);
    return true;
}
