// SPDX-License-Identifier: GPL-2.0-only
#include "nsp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcasecmp(s + a - b, suffix);
}

static bool parse_hex16(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int c = tolower((unsigned char)s[i]);
        if (c >= '0' && c <= '9') v = v << 4 | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v << 4 | (uint64_t)(c - 'a' + 10);
        else return false;
    }
    *out = v;
    return true;
}

static int kind_of(uint64_t tid) {
    if (!tid) return NSP_UNKNOWN;
    unsigned low = (unsigned)(tid & 0xfff);
    return low == 0 ? NSP_BASE : low == 0x800 ? NSP_UPDATE : NSP_DLC;
}

uint64_t nsp_base_id(uint64_t tid) {
    unsigned low = (unsigned)(tid & 0xfff);
    if (low == 0) return tid;
    if (low == 0x800) return tid & ~0xfffULL;
    return (tid & ~0xfffULL) - 0x1000;  // DLC: base + 0x1000 + index
}

const char *nsp_kind_name(int kind) {
    switch (kind) {
    case NSP_BASE: return "Base game";
    case NSP_UPDATE: return "Update";
    case NSP_DLC: return "DLC";
    default: return "Unknown";
    }
}

// "Super Mario Odyssey [0100000000010000][v0].nsp" -> name, title id, version
static void parse_file_name(NspInfo *n) {
    const char *f = n->file;
    const char *br = strchr(f, '[');
    size_t nl = br ? (size_t)(br - f) : (strlen(f) > 4 ? strlen(f) - 4 : strlen(f));
    while (nl && f[nl - 1] == ' ') nl--;
    if (nl >= sizeof(n->name)) nl = sizeof(n->name) - 1;
    memcpy(n->name, f, nl);
    n->name[nl] = 0;
    for (const char *p = f; (p = strchr(p, '[')); p++) {
        const char *q = strchr(p, ']');
        if (!q) break;
        size_t len = (size_t)(q - p - 1);
        if (len == 16 && !n->title_id) parse_hex16(p + 1, &n->title_id);
        else if (len < sizeof(n->version) && p[1] == 'v' && isdigit((unsigned char)p[2]))
            snprintf(n->version, sizeof(n->version), "%.*s", (int)len, p + 1);
    }
    n->kind = kind_of(n->title_id);
}

bool nsp_is_dump(const char *f) {
    return ends_with(f, ".nsp") || ends_with(f, ".nsz") || ends_with(f, ".xci") || ends_with(f, ".xcz");
}

static void scan_dir(const char *dir, NspInfo *out, int max, int *count) {
    VfsEntry *e;
    int k;
    if (vfs_list(VFS_SD, dir, &e, &k) != 0) return;
    for (int i = 0; i < k && *count < max; i++) {
        if (e[i].is_dir || !nsp_is_dump(e[i].name)) continue;
        NspInfo *n = &out[(*count)++];
        memset(n, 0, sizeof(*n));
        snprintf(n->file, sizeof(n->file), "%s", e[i].name);
        snprintf(n->container, sizeof(n->container), "%s", e[i].name + strlen(e[i].name) - 4);
        for (char *c = n->container; *c; c++) *c = (char)tolower((unsigned char)*c);
        snprintf(n->path, sizeof(n->path), "%s/%s", strcmp(dir, "/") ? dir : "", e[i].name);
        n->size = e[i].size;
        parse_file_name(n);
    }
    free(e);
}

static int cmp_nsp(const void *a, const void *b) {
    const NspInfo *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : x->kind - y->kind;
}

int nsp_scan(NspInfo *out, int max) {
    int count = 0;
    scan_dir(NSP_DIR, out, max, &count);
    scan_dir("/", out, max, &count);
    qsort(out, (size_t)count, sizeof(NspInfo), cmp_nsp);
    return count;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

bool nsp_inspect(NspInfo *n) {
    n->inspected = true;
    VfsFile *f = vfs_open(VFS_SD, n->path, false);
    if (!f) { snprintf(n->err, sizeof(n->err), "cannot open: %s", vfs_last_error()); return false; }
    uint8_t h[0x200];
    size_t got = 0;
    bool ok = vfs_read(f, 0, h, sizeof(h), &got) == 0 && got == sizeof(h) && !memcmp(h, "PFS0", 4);
    if (!ok && got == sizeof(h)) {
        // a cartridge image: its header at 0x100 (or after a 0x1000-byte key area); the contents are read
        // with the keys when it is converted
        bool xci = !memcmp(h + 0x100, "HEAD", 4) ||
                   (vfs_read(f, 0x1000, h, sizeof(h), &got) == 0 && got == sizeof(h) && !memcmp(h + 0x100, "HEAD", 4));
        vfs_close(f);
        if (!xci) { snprintf(n->err, sizeof(n->err), "not a game dump (no PFS0 table, no cartridge header)"); return false; }
        if (!n->title_id) n->kind = NSP_BASE;
        n->ok = true;
        return true;
    }
    uint32_t count = ok ? rd32(h + 4) : 0, strsz = ok ? rd32(h + 8) : 0;
    if (!ok || count == 0 || count > 4096 || strsz > 512 * 1024) {
        snprintf(n->err, sizeof(n->err), "not a PFS0 container");
        vfs_close(f);
        return false;
    }
    size_t hsz = 0x10 + (size_t)count * 0x18 + strsz;
    uint8_t *buf = malloc(hsz + 1);
    ok = buf && vfs_read(f, 0, buf, hsz, &got) == 0 && got == hsz;
    vfs_close(f);
    if (!ok) { free(buf); snprintf(n->err, sizeof(n->err), "short read"); return false; }
    buf[hsz] = 0;
    const char *names = (const char *)buf + 0x10 + count * 0x18;
    int64_t data = (int64_t)hsz, end = 0;
    uint64_t best = 0, name_tid = n->title_id;
    bool has_base = false, has_update = false;
    n->cnmts = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *ent = buf + 0x10 + i * 0x18;
        uint64_t off = rd64(ent), size = rd64(ent + 8);
        uint32_t no = rd32(ent + 16);
        if (no >= strsz) continue;
        const char *nm = names + no;
        if ((int64_t)(off + size) + data > end) end = (int64_t)(off + size) + data;
        n->files++;
        if (ends_with(nm, ".cnmt.nca") || ends_with(nm, ".cnmt.ncz")) {
            n->cnmt = true;
            n->cnmts++;
        }
        if (ends_with(nm, ".nca") || ends_with(nm, ".ncz")) n->ncas++;
        if (ends_with(nm, ".tik")) {
            n->ticket = true;
            uint64_t tid;
            // a dump can hold several titles (a game with its update): the game says what the dump is
            if (strlen(nm) >= 36 && parse_hex16(nm, &tid)) {
                if (!best || kind_of(tid) < kind_of(best)) best = tid;
                has_base |= kind_of(tid) == NSP_BASE;
                has_update |= kind_of(tid) == NSP_UPDATE;
            }
        }
    }
    free(buf);
    if (best) n->title_id = best;
    // Only what is encrypted with a title key has a ticket, so a game with its update in one dump may
    // show the update's ticket alone; two content lists and a file name that names the game say more.
    if (has_update && !has_base && n->cnmts >= 2 && kind_of(name_tid) == NSP_BASE && nsp_base_id(best) == name_tid) {
        n->title_id = name_tid;
        has_base = true;
    }
    n->with_update = has_base && has_update;
    n->kind = kind_of(n->title_id);
    if (end > n->size) { snprintf(n->err, sizeof(n->err), "file is cut short (%lld of %lld bytes)", (long long)n->size, (long long)end); return false; }
    n->ok = true;
    return true;
}
