// SPDX-License-Identifier: GPL-2.0-only
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ini_get(const char *text, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = len - kl - 1;
            if (vl && p[kl + 1 + vl - 1] == '\r') vl--;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return true;
        }
        p = eol ? eol + 1 : NULL;
    }
    if (n) out[0] = 0;
    return false;
}

int64_t ini_get_i64(const char *text, const char *key, int64_t def) {
    char v[32];
    return ini_get(text, key, v, sizeof(v)) && v[0] ? strtoll(v, NULL, 10) : def;
}

bool read_blob(VfsRoot r, const char *path, size_t max, void **out, size_t *len) {
    *out = NULL;
    VfsFile *f = vfs_open(r, path, false);
    if (!f) return false;
    int64_t size = vfs_size(f);
    if (size < 0 || (size_t)size > max) { vfs_close(f); return false; }
    char *buf = malloc((size_t)size + 1);
    size_t got = 0;
    bool ok = buf && vfs_read(f, 0, buf, (size_t)size, &got) == 0 && got == (size_t)size;
    vfs_close(f);
    if (!ok) { free(buf); return false; }
    buf[size] = 0;
    *out = buf;
    if (len) *len = (size_t)size;
    return true;
}

char *read_text(VfsRoot r, const char *path, size_t max) {
    void *b = NULL;
    return read_blob(r, path, max, &b, NULL) ? b : NULL;
}

bool write_text(VfsRoot r, const char *path, const char *text) {
    size_t n = strlen(text);
    bool dir = false;
    if (vfs_stat(r, path, &dir, NULL) == 0) vfs_delete(r, path);
    if (vfs_create(r, path, (int64_t)n) != 0) return false;
    VfsFile *f = vfs_open(r, path, true);
    if (!f) return false;
    bool ok = vfs_write(f, 0, text, n) == 0 && vfs_flush(f) == 0;
    vfs_close(f);
    return ok;
}

static char g_log[16384];
static size_t g_log_len;

void app_log(const char *fmt, ...) {
    char line[512], stamp[32];
    plat_now(stamp, sizeof(stamp), false);
    int k = snprintf(line, sizeof(line), "[%s] ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + k, sizeof(line) - (size_t)k, fmt, ap);
    va_end(ap);
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);
    size_t n = strlen(line);
    if (g_log_len + n >= sizeof(g_log)) {  // keep the newest half
        size_t drop = g_log_len / 2;
        memmove(g_log, g_log + drop, g_log_len - drop);
        g_log_len -= drop;
    }
    memcpy(g_log + g_log_len, line, n + 1);
    g_log_len += n;
    // append to SD:/seedswap/log.txt
    const char *path = SS_ROOT "/log.txt";
    bool dir = false;
    int64_t size = 0;
    if (vfs_stat(VFS_SD, path, &dir, &size) != 0) {
        vfs_mkdir(VFS_SD, SS_ROOT);
        if (vfs_create(VFS_SD, path, 0) != 0) return;
        size = 0;
    } else if (size > 512 * 1024) {
        vfs_delete(VFS_SD, path);
        if (vfs_create(VFS_SD, path, 0) != 0) return;
        size = 0;
    }
    VfsFile *f = vfs_open(VFS_SD, path, true);
    if (f) {
        vfs_write(f, size, line, n);
        vfs_flush(f);
        vfs_close(f);
    }
}

const char *app_log_tail(void) { return g_log; }

void uid_hex(const Uid *u, char out[33]) {
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", u->b[i]);
}

bool uid_parse(const char *hex, Uid *u) {
    if (!hex || strlen(hex) < 32) return false;
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        u->b[i] = (uint8_t)v;
    }
    return true;
}

void fmt_bytes(int64_t n, char *out, size_t sz) {
    if (n < 0) snprintf(out, sz, "?");
    else if (n >= 1000LL * 1000 * 1000) snprintf(out, sz, "%.2f GB", (double)n / 1e9);
    else if (n >= 1000 * 1000) snprintf(out, sz, "%.1f MB", (double)n / 1e6);
    else if (n >= 1000) snprintf(out, sz, "%.1f KB", (double)n / 1e3);
    else snprintf(out, sz, "%lld B", (long long)n);
}

bool mkdirs(VfsRoot r, const char *path) {
    char cur[512];
    size_t n = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && n > 0) {
            cur[n] = 0;
            if (vfs_mkdir(r, cur) != 0) return false;
        }
        if (n + 1 >= sizeof(cur)) return false;
        cur[n++] = *p;
    }
    cur[n] = 0;
    return n <= 1 || vfs_mkdir(r, cur) == 0;
}

bool tree_stats(VfsRoot r, const char *path, TreeStats *st) {
    VfsEntry *e;
    int n;
    if (vfs_list(r, path, &e, &n) != 0) return false;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char sub[768];
        snprintf(sub, sizeof(sub), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e[i].name);
        if (e[i].is_dir) {
            st->dirs++;
            ok = tree_stats(r, sub, st);
        } else {
            st->files++;
            st->bytes += e[i].size;
        }
    }
    free(e);
    return ok;
}

static bool copy_file(VfsRoot sr, const char *src, VfsRoot dr, const char *dst, int64_t size, int64_t total,
                      int64_t *done, ProgressFn cb) {
    if (vfs_create(dr, dst, size) != 0) return false;
    VfsFile *in = vfs_open(sr, src, false), *out = vfs_open(dr, dst, true);
    bool ok = in && out;
    static uint8_t buf[1 << 20];
    for (int64_t at = 0; ok && at < size;) {
        size_t want = size - at < (int64_t)sizeof(buf) ? (size_t)(size - at) : sizeof(buf), got = 0;
        ok = vfs_read(in, at, buf, want, &got) == 0 && got == want && vfs_write(out, at, buf, got) == 0;
        at += (int64_t)got;
        *done += (int64_t)got;
        if (ok && cb && !cb(total > 0 ? (float)((double)*done / (double)total) : -1.f, dst)) ok = false;
    }
    if (ok) ok = vfs_flush(out) == 0;
    vfs_close(in);
    vfs_close(out);
    if (!ok) app_log("copy %s -> %s failed: %s", src, dst, vfs_last_error());
    return ok;
}

typedef struct { int64_t budget, pending; int (*commit)(void); } CommitPlan;

static int64_t save_blocks(int64_t n) { return (n + 0x3FFF) & ~(int64_t)0x3FFF; }

static bool copy_tree_in(VfsRoot sr, const char *src, VfsRoot dr, const char *dst, int64_t total, int64_t *done,
                         ProgressFn cb, CommitPlan *plan) {
    if (dst[0] && strcmp(dst, "/") && !mkdirs(dr, dst)) return false;
    VfsEntry *e;
    int n;
    if (vfs_list(sr, src, &e, &n) != 0) return false;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char s[768], d[768];
        snprintf(s, sizeof(s), "%s%s%s", src, src[strlen(src) - 1] == '/' ? "" : "/", e[i].name);
        snprintf(d, sizeof(d), "%s%s%s", dst, dst[strlen(dst) - 1] == '/' ? "" : "/", e[i].name);
        if (!e[i].is_dir && plan && plan->budget > 0) {
            int64_t need = save_blocks(e[i].size);
            if (plan->pending > 0 && plan->pending + need > plan->budget) {
                if (plan->commit() != 0) { ok = false; break; }
                plan->pending = 0;
            }
            plan->pending += need;
        }
        ok = e[i].is_dir ? copy_tree_in(sr, s, dr, d, total, done, cb, plan)
                         : copy_file(sr, s, dr, d, e[i].size, total, done, cb);
    }
    free(e);
    return ok;
}

bool copy_tree(VfsRoot sr, const char *src, VfsRoot dr, const char *dst, int64_t total, int64_t *done, ProgressFn cb) {
    return copy_tree_in(sr, src, dr, dst, total, done, cb, NULL);
}

bool copy_tree_commit(VfsRoot sr, const char *src, VfsRoot dr, const char *dst, int64_t total, int64_t *done,
                      ProgressFn cb, int64_t budget, int (*commit)(void)) {
    CommitPlan plan = {budget, 0, commit};
    return copy_tree_in(sr, src, dr, dst, total, done, cb, &plan);
}

bool tree_save_room(VfsRoot r, const char *path, int64_t *blocks, int64_t *largest) {
    VfsEntry *e;
    int n;
    if (vfs_list(r, path, &e, &n) != 0) return false;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char sub[768];
        snprintf(sub, sizeof(sub), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e[i].name);
        if (e[i].is_dir) {
            *blocks += 0x4000;
            ok = tree_save_room(r, sub, blocks, largest);
        } else {
            int64_t b = save_blocks(e[i].size);
            *blocks += b;
            if (b > *largest) *largest = b;
        }
    }
    free(e);
    return ok;
}

bool clear_dir(VfsRoot r, const char *path) {
    VfsEntry *e;
    int n;
    if (vfs_list(r, path, &e, &n) != 0) return false;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char s[768];
        snprintf(s, sizeof(s), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e[i].name);
        ok = (e[i].is_dir ? vfs_rmtree(r, s) : vfs_delete(r, s)) == 0;
    }
    free(e);
    return ok;
}
