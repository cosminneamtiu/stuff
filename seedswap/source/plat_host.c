// SPDX-License-Identifier: GPL-2.0-only
// SeedSwap PC preview backend (Linux/WSL). Not part of the NRO.
//   SEEDSWAP_SD     directory that stands in for the SD card root
//   SEEDSWAP_SAVES  directory with one subdirectory per user uid (hex) = that user's Seedsow save;
//                   another host's saves are in SEEDSWAP_SAVES/<title id hex>/<uid hex>
//   SEEDSWAP_APPS   optional installed games "id=name,id=name" (apps_list)
//   SEEDSWAP_SWITCH2 optional IDs of SEEDSWAP_APPS that are Switch 2 games, "id,id"
//   SEEDSWAP_SAVE_LOCKED optional user IDs whose Seedsow save can't be opened (save_find_files), "uid,uid"
//   SEEDSWAP_USERS  optional "uid=name,uid=name" nicknames
//   SEEDSWAP_SCRIPT button script, one step per frame group: e.g. "R,_,DOWN,A,shot:lib" where
//                   "_" is an idle frame and "shot:<name>" writes <SEEDSWAP_OUT>/<name>.ppm
//   SEEDSWAP_OUT    directory for screenshots
//   SEEDSWAP_TIME   fixed clock text "2026-11-20 19:00" (default) for reproducible frames
#define _GNU_SOURCE
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static uint32_t g_px[SCREEN_W * SCREEN_H];
static Canvas g_canvas = {g_px, SCREEN_W, SCREEN_H, SCREEN_W};
static char g_sd[512], g_saves[512], g_out[512], g_err[256];
static char *g_script;
static char *g_tok;
static uint64_t g_down, g_frames;
static char g_save_dir[600];
static bool g_save_open, g_quit;
static char g_shot[128];
static Uid g_mounted;       // the user whose save is open
static int64_t g_pending;   // bytes written to the save since the last commit

struct VfsFile { int fd; };

const char *vfs_last_error(void) { return g_err; }

static void host_path(VfsRoot r, const char *path, char *out, size_t n) {
    snprintf(out, n, "%s%s", r == VFS_SAVE ? g_save_dir : g_sd, path);
}

bool plat_init(void) {
    const char *sd = getenv("SEEDSWAP_SD"), *sv = getenv("SEEDSWAP_SAVES"), *out = getenv("SEEDSWAP_OUT");
    if (!sd || !sv) { fprintf(stderr, "set SEEDSWAP_SD and SEEDSWAP_SAVES\n"); return false; }
    snprintf(g_sd, sizeof(g_sd), "%s", sd);
    snprintf(g_saves, sizeof(g_saves), "%s", sv);
    snprintf(g_out, sizeof(g_out), "%s", out ? out : ".");
    const char *s = getenv("SEEDSWAP_SCRIPT");
    g_script = strdup(s ? s : "shot:main");
    return true;
}

void plat_exit(void) { free(g_script); }

static uint64_t parse_btn(const char *t) {
    static const struct { const char *n; uint64_t b; } map[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L", BTN_L}, {"R", BTN_R}, {"ZL", BTN_ZL},
        {"ZR", BTN_ZR}, {"PLUS", BTN_PLUS}, {"MINUS", BTN_MINUS}, {"UP", BTN_UP}, {"DOWN", BTN_DOWN},
        {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT}};
    for (size_t i = 0; i < sizeof(map) / sizeof(*map); i++)
        if (!strcmp(t, map[i].n)) return map[i].b;
    return 0;
}

bool plat_loop(void) {
    if (g_quit) return false;
    g_down = 0;
    g_shot[0] = 0;
    char *tok = strtok_r(g_frames++ ? NULL : g_script, ",", &g_tok);
    if (!tok) return false;
    if (!strncmp(tok, "shot:", 5)) snprintf(g_shot, sizeof(g_shot), "%s", tok + 5);
    else if (strcmp(tok, "_")) g_down = parse_btn(tok);
    return true;
}

uint64_t plat_buttons_down(void) { return g_down; }
uint64_t plat_buttons_held(void) { return 0; }
uint64_t plat_ms(void) { return g_frames * 16; }
void plat_sleep_ms(int ms) { (void)ms; }
void plat_keep_awake(bool on) { (void)on; }

bool plat_now(char *out, size_t n, bool compact) {
    const char *fixed = getenv("SEEDSWAP_TIME");
    if (!fixed) fixed = "2026-11-20 19:00";
    if (!compact) { snprintf(out, n, "%s", fixed); return true; }
    // "2026-11-20 19:00" -> "20261120T190000" (+ frame counter so names stay unique)
    snprintf(out, n, "%.4s%.2s%.2sT%.2s%.2s%02llu", fixed, fixed + 5, fixed + 8, fixed + 11, fixed + 14,
             (unsigned long long)(g_frames % 60));
    return true;
}

Canvas *plat_frame_begin(void) { return &g_canvas; }

void plat_frame_end(void) {
    if (!g_shot[0]) return;
    char path[700];
    snprintf(path, sizeof(path), "%s/%s.ppm", g_out, g_shot);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t p = g_px[i];
        unsigned char rgb[3] = {(unsigned char)(p & 0xff), (unsigned char)((p >> 8) & 0xff), (unsigned char)((p >> 16) & 0xff)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s\n", path);
}

// ---- files
int vfs_stat(VfsRoot r, const char *path, bool *is_dir, int64_t *size) {
    if (r == VFS_SAVE && !g_save_open) return -1;
    char p[1024];
    host_path(r, path, p, sizeof(p));
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    if (size) *size = S_ISDIR(st.st_mode) ? 0 : st.st_size;
    return 0;
}

int vfs_list(VfsRoot r, const char *dir, VfsEntry **out, int *count) {
    *out = NULL;
    *count = 0;
    if (r == VFS_SAVE && !g_save_open) return -1;
    char p[1024];
    host_path(r, dir, p, sizeof(p));
    DIR *d = opendir(p);
    if (!d) { snprintf(g_err, sizeof(g_err), "opendir %s: %s", p, strerror(errno)); return -1; }
    int cap = 16, n = 0;
    VfsEntry *e = malloc(sizeof(*e) * (size_t)cap);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) e = realloc(e, sizeof(*e) * (size_t)(cap *= 2));
        char q[1400];
        snprintf(q, sizeof(q), "%s/%s", p, de->d_name);
        struct stat st;
        stat(q, &st);
        snprintf(e[n].name, sizeof(e[n].name), "%s", de->d_name);
        e[n].is_dir = S_ISDIR(st.st_mode);
        e[n].size = e[n].is_dir ? 0 : st.st_size;
        n++;
    }
    closedir(d);
    *out = e;
    *count = n;
    return 0;
}

static int save_room_for(int64_t size);

int vfs_create(VfsRoot r, const char *path, int64_t size) {
    if (r == VFS_SAVE && (!g_save_open || save_room_for(size) != 0)) return -1;
    char p[1024];
    host_path(r, path, p, sizeof(p));
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0 || ftruncate(fd, size) != 0) { snprintf(g_err, sizeof(g_err), "create %s: %s", p, strerror(errno)); if (fd >= 0) close(fd); return -1; }
    close(fd);
    return 0;
}

// SEEDSWAP_LOCKED: comma-separated SD paths that refuse to open (a resident loader's report file)
static bool locked(VfsRoot r, const char *path) {
    const char *l = getenv("SEEDSWAP_LOCKED");
    if (r != VFS_SD || !l) return false;
    size_t n = strlen(path);
    for (const char *p = l; (p = strstr(p, path)); p += n)
        if ((p == l || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    return false;
}

VfsFile *vfs_open(VfsRoot r, const char *path, bool write) {
    if (locked(r, path)) { snprintf(g_err, sizeof(g_err), "open %s: locked", path); return NULL; }
    char p[1024];
    host_path(r, path, p, sizeof(p));
    int fd = open(p, write ? O_RDWR : O_RDONLY);
    if (fd < 0) { snprintf(g_err, sizeof(g_err), "open %s: %s", p, strerror(errno)); return NULL; }
    VfsFile *f = malloc(sizeof(*f));
    f->fd = fd;
    return f;
}

int vfs_read(VfsFile *f, int64_t off, void *buf, size_t n, size_t *got) {
    ssize_t k = pread(f->fd, buf, n, off);
    if (got) *got = k > 0 ? (size_t)k : 0;
    return k < 0 ? -1 : 0;
}
int vfs_write(VfsFile *f, int64_t off, const void *buf, size_t n) { return pwrite(f->fd, buf, n, off) == (ssize_t)n ? 0 : -1; }
int vfs_flush(VfsFile *f) { return fsync(f->fd); }
int64_t vfs_size(VfsFile *f) { struct stat st; return fstat(f->fd, &st) == 0 ? st.st_size : -1; }
void vfs_close(VfsFile *f) { if (f) { close(f->fd); free(f); } }

int vfs_mkdir(VfsRoot r, const char *path) {
    char p[1024];
    host_path(r, path, p, sizeof(p));
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}
int vfs_delete(VfsRoot r, const char *path) { char p[1024]; host_path(r, path, p, sizeof(p)); return unlink(p); }

int vfs_rmtree(VfsRoot r, const char *path) {
    VfsEntry *e;
    int n;
    if (vfs_list(r, path, &e, &n) != 0) return -1;
    for (int i = 0; i < n; i++) {
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, e[i].name);
        if (e[i].is_dir) vfs_rmtree(r, sub);
        else vfs_delete(r, sub);
    }
    free(e);
    char p[1024];
    host_path(r, path, p, sizeof(p));
    return rmdir(p);
}

int vfs_rename(VfsRoot r, const char *from, const char *to) {
    char a[1024], b[1024];
    host_path(r, from, a, sizeof(a));
    host_path(r, to, b, sizeof(b));
    if (access(b, F_OK) == 0) { snprintf(g_err, sizeof(g_err), "rename target exists: %s", b); return -1; }
    return rename(a, b);
}

int64_t vfs_free_space(VfsRoot r) {
    (void)r;
    const char *fake = getenv("SEEDSWAP_FREE");
    if (fake) return strtoll(fake, NULL, 10);
    struct statvfs s;
    return statvfs(g_sd, &s) == 0 ? (int64_t)s.f_bavail * (int64_t)s.f_frsize : -1;
}

// ---- saves: SEEDSWAP_SAVES/<uid hex>/ (Seedsow), SEEDSWAP_SAVES/<title id>/<uid hex>/ (other hosts)
static uint64_t g_title = SEEDSOW_TITLE;
static char g_title_dir[600];

static const char *saves_root(uint64_t title) {
    if (title == SEEDSOW_TITLE) return g_saves;
    snprintf(g_title_dir, sizeof(g_title_dir), "%s/%016llx", g_saves, (unsigned long long)title);
    return g_title_dir;
}

void save_set_title(uint64_t title) {
    if (title != g_title) save_unmount();
    g_title = title ? title : SEEDSOW_TITLE;
}

uint64_t save_title(void) { return g_title; }

int save_list_users(Uid *out, int max) {
    return save_list_title_users(g_title, out, max);
}

int save_list_title_users(uint64_t title, Uid *out, int max) {
    DIR *d = opendir(saves_root(title));
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (strlen(de->d_name) != 32) continue;
        for (int i = 0; i < 16; i++) {
            unsigned v;
            sscanf(de->d_name + i * 2, "%2x", &v);
            out[n].b[i] = (uint8_t)v;
        }
        n++;
    }
    closedir(d);
    return n;
}

// the same second line of defence as on the console
static Uid g_allowed;
static bool g_allowed_set;

void save_allow_only(const Uid *uid) {
    g_allowed_set = uid != NULL;
    if (uid) g_allowed = *uid;
}

int save_find_files(const Uid *uid, const char *const *names, int n, char *found, size_t fn) {
    char h[33], path[800];
    for (int i = 0; i < 16; i++) snprintf(h + i * 2, 3, "%02x", uid->b[i]);
    const char *locked = getenv("SEEDSWAP_SAVE_LOCKED");
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", g_saves, h);
    if ((locked && strstr(locked, h)) || stat(path, &st) != 0) {
        snprintf(g_err, sizeof(g_err), "open save (read-only check) %s: can't", h);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", g_saves, h, names[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (found) snprintf(found, fn, "%s", names[i]);
            return 1;
        }
    }
    return 0;
}

int save_mount(const Uid *uid, bool write) {
    (void)write;
    char h[33];
    for (int i = 0; i < 16; i++) snprintf(h + i * 2, 3, "%02x", uid->b[i]);
    if (!g_allowed_set || memcmp(g_allowed.b, uid->b, 16)) {
        snprintf(g_err, sizeof(g_err), "refused: not the play user's save (%s)", h);
        return -1;
    }
    snprintf(g_save_dir, sizeof(g_save_dir), "%s/%s", saves_root(g_title), h);
    struct stat st;
    if (stat(g_save_dir, &st) != 0) { snprintf(g_err, sizeof(g_err), "no save for %s", h); return -1; }
    g_save_open = true;
    g_mounted = *uid;
    g_pending = 0;
    return 0;
}
// Save capacity, as on the console: SEEDSWAP_SAVES/<uid hex>.size holds "<data> <journal>" in bytes
// (no file: Seedsow's 32 + 32 MiB). Files in the save take whole 16 KiB blocks; the save as a whole
// must fit its data size, and what is written between two commits must fit its journal.
// SEEDSWAP_NO_EXTEND=1: growing fails (a console without room).
static void size_file(const Uid *uid, char *out, size_t n) {
    char h[33];
    for (int i = 0; i < 16; i++) snprintf(h + i * 2, 3, "%02x", uid->b[i]);
    snprintf(out, n, "%s/%s.size", saves_root(g_title), h);
}

static int64_t blocks(int64_t n) { return (n + 0x3FFF) & ~(int64_t)0x3FFF; }

static int64_t tree_used(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int64_t sum = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char q[1400];
        snprintf(q, sizeof(q), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(q, &st) != 0) continue;
        sum += S_ISDIR(st.st_mode) ? tree_used(q) : blocks(st.st_size);
    }
    closedir(d);
    return sum;
}

int save_commit(void) {
    if (!g_save_open) return -1;
    g_pending = 0;
    return 0;
}
void save_unmount(void) { g_save_open = false; g_pending = 0; }

int save_get_size(const Uid *uid, int64_t *data, int64_t *journal) {
    if (!g_allowed_set || memcmp(g_allowed.b, uid->b, 16)) { snprintf(g_err, sizeof(g_err), "refused: not the play user's save"); return -1; }
    char p[600];
    size_file(uid, p, sizeof(p));
    *data = *journal = 32LL << 20;
    FILE *f = fopen(p, "r");
    if (f) {
        long long d = 0, j = 0;
        if (fscanf(f, "%lld %lld", &d, &j) == 2) { *data = d; *journal = j; }
        fclose(f);
    }
    return 0;
}

int save_extend(const Uid *uid, int64_t data, int64_t journal) {
    int64_t now_d, now_j;
    if (save_get_size(uid, &now_d, &now_j) != 0) return -1;
    if (g_save_open) { snprintf(g_err, sizeof(g_err), "the save is open"); return -1; }
    if (data < now_d || journal < now_j || (data | journal) & 0xFFFFF) {
        snprintf(g_err, sizeof(g_err), "refused: %lld + %lld is not a growth of %lld + %lld", (long long)data,
                 (long long)journal, (long long)now_d, (long long)now_j);
        return -1;
    }
    const char *no = getenv("SEEDSWAP_NO_EXTEND");
    if (no && *no == '1') { snprintf(g_err, sizeof(g_err), "ExtendSaveData failed (0x4602)"); return -1; }
    char p[600];
    size_file(uid, p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) { snprintf(g_err, sizeof(g_err), "write %s: %s", p, strerror(errno)); return -1; }
    fprintf(f, "%lld %lld\n", (long long)data, (long long)journal);
    fclose(f);
    return 0;
}

// called before a file of this size is created in the open save
static int save_room_for(int64_t size) {
    int64_t d, j;
    if (save_get_size(&g_mounted, &d, &j) != 0) return 0;
    if (tree_used(g_save_dir) + blocks(size) > d) { snprintf(g_err, sizeof(g_err), "write the save: 2002-0034 (the save is full)"); return -1; }
    if (g_pending + blocks(size) > j) { snprintf(g_err, sizeof(g_err), "write the save: 2002-0035 (the journal is full; commit first)"); return -1; }
    g_pending += blocks(size);
    return 0;
}

bool user_name(const Uid *uid, char *out, size_t n) {
    const char *u = getenv("SEEDSWAP_USERS");
    if (!u) return false;
    char h[33];
    for (int i = 0; i < 16; i++) snprintf(h + i * 2, 3, "%02x", uid->b[i]);
    const char *p = strstr(u, h);
    if (!p || p[32] != '=') return false;
    p += 33;
    size_t k = strcspn(p, ",");
    snprintf(out, n, "%.*s", (int)k, p);
    return true;
}

int apps_list(AppEntry *out, int max) {
    const char *a = getenv("SEEDSWAP_APPS");
    if (!a) return -1;
    int n = 0;
    for (const char *p = a; *p && n < max;) {
        size_t k = strcspn(p, ",");
        char id[17] = "";
        const char *eq = memchr(p, '=', k);
        if (eq && eq - p == 16) {
            memcpy(id, p, 16);
            out[n].id = strtoull(id, NULL, 16);
            snprintf(out[n].name, sizeof(out[n].name), "%.*s", (int)(k - 17), eq + 1);
            const char *s2 = getenv("SEEDSWAP_SWITCH2");
            out[n].platform = s2 && strstr(s2, id) ? 1 : 0;
            n++;
        }
        p += k;
        if (*p == ',') p++;
    }
    return n;
}
