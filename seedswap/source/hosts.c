// SPDX-License-Identifier: GPL-2.0-only
// Host titles: see hosts.h and DESIGN.md "Hosts".
#include "hosts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/compat.h"
#include "compat/nsofile.h"
#include "compat_game.h"
#include "crypto.h"
#include "dumpcheck.h"
#include "stage.h"

// Nintendo's public keys for ACID signatures (the same table as Sphaira's nca.cpp).
static const uint8_t ACID_MODULI[2][256] = {
    {
     0xdd, 0xc8, 0xdd, 0xf2, 0x4e, 0x6d, 0xf0, 0xca, 0x9e, 0xc7, 0x5d, 0xc7, 0x7b, 0xad, 0xfe, 0x7d,
     0x23, 0x89, 0x69, 0xb6, 0xf2, 0x06, 0xa2, 0x02, 0x88, 0xe1, 0x55, 0x91, 0xab, 0xcb, 0x4d, 0x50,
     0x2e, 0xfc, 0x9d, 0x94, 0x76, 0xd6, 0x4c, 0xd8, 0xff, 0x10, 0xfa, 0x5e, 0x93, 0x0a, 0xb4, 0x57,
     0xac, 0x51, 0xc7, 0x16, 0x66, 0xf4, 0x1a, 0x54, 0xc2, 0xc5, 0x04, 0x3d, 0x1b, 0xfe, 0x30, 0x20,
     0x8a, 0xac, 0x6f, 0x6f, 0xf5, 0xc7, 0xb6, 0x68, 0xb8, 0xc9, 0x40, 0x6b, 0x42, 0xad, 0x11, 0x21,
     0xe7, 0x8b, 0xe9, 0x75, 0x01, 0x86, 0xe4, 0x48, 0x9b, 0x0a, 0x0a, 0xf8, 0x7f, 0xe8, 0x87, 0xf2,
     0x82, 0x01, 0xe6, 0xa3, 0x0f, 0xe4, 0x66, 0xae, 0x83, 0x3f, 0x4e, 0x9f, 0x5e, 0x01, 0x30, 0xa4,
     0x00, 0xb9, 0x9a, 0xae, 0x5f, 0x03, 0xcc, 0x18, 0x60, 0xe5, 0xef, 0x3b, 0x5e, 0x15, 0x16, 0xfe,
     0x1c, 0x82, 0x78, 0xb5, 0x2f, 0x47, 0x7c, 0x06, 0x66, 0x88, 0x5d, 0x35, 0xa2, 0x67, 0x20, 0x10,
     0xe7, 0x6c, 0x43, 0x68, 0xd3, 0xe4, 0x5a, 0x68, 0x2a, 0x5a, 0xe2, 0x6d, 0x73, 0xb0, 0x31, 0x53,
     0x1c, 0x20, 0x09, 0x44, 0xf5, 0x1a, 0x9d, 0x22, 0xbe, 0x12, 0xa1, 0x77, 0x11, 0xe2, 0xa1, 0xcd,
     0x40, 0x9a, 0xa2, 0x8b, 0x60, 0x9b, 0xef, 0xa0, 0xd3, 0x48, 0x63, 0xa2, 0xf8, 0xa3, 0x2c, 0x08,
     0x56, 0x52, 0x2e, 0x60, 0x19, 0x67, 0x5a, 0xa7, 0x9f, 0xdc, 0x3f, 0x3f, 0x69, 0x2b, 0x31, 0x6a,
     0xb7, 0x88, 0x4a, 0x14, 0x84, 0x80, 0x33, 0x3c, 0x9d, 0x44, 0xb7, 0x3f, 0x4c, 0xe1, 0x75, 0xea,
     0x37, 0xea, 0xe8, 0x1e, 0x7c, 0x77, 0xb7, 0xc6, 0x1a, 0xa2, 0xf0, 0x9f, 0x10, 0x61, 0xcd, 0x7b,
     0x5b, 0x32, 0x4c, 0x37, 0xef, 0xb1, 0x71, 0x68, 0x53, 0x0a, 0xed, 0x51, 0x7d, 0x35, 0x22, 0xfd,
    },
    {
     0xe7, 0xaa, 0x25, 0xc8, 0x01, 0xa5, 0x14, 0x6b, 0x01, 0x60, 0x3e, 0xd9, 0x96, 0x5a, 0xbf, 0x90,
     0xac, 0xa7, 0xfd, 0x9b, 0x5b, 0xbd, 0x8a, 0x26, 0xb0, 0xcb, 0x20, 0x28, 0x9a, 0x72, 0x12, 0xf5,
     0x20, 0x65, 0xb3, 0xb9, 0x84, 0x58, 0x1f, 0x27, 0xbc, 0x7c, 0xa2, 0xc9, 0x9e, 0x18, 0x95, 0xcf,
     0xc2, 0x73, 0x2e, 0x74, 0x8c, 0x66, 0xe5, 0x9e, 0x79, 0x2b, 0xb8, 0x07, 0x0c, 0xb0, 0x4e, 0x8e,
     0xab, 0x85, 0x21, 0x42, 0xc4, 0xc5, 0x6d, 0x88, 0x9c, 0xdb, 0x15, 0x95, 0x3f, 0x80, 0xdb, 0x7a,
     0x9a, 0x7d, 0x41, 0x56, 0x25, 0x17, 0x18, 0x42, 0x4d, 0x8c, 0xac, 0xa5, 0x7b, 0xdb, 0x42, 0x5d,
     0x59, 0x35, 0x45, 0x5d, 0x8a, 0x02, 0xb5, 0x70, 0xc0, 0x72, 0x35, 0x46, 0xd0, 0x1d, 0x60, 0x01,
     0x4a, 0xcc, 0x1c, 0x46, 0xd3, 0xd6, 0x35, 0x52, 0xd6, 0xe1, 0xf8, 0x3b, 0x5d, 0xea, 0xdd, 0xb8,
     0xfe, 0x7d, 0x50, 0xcb, 0x35, 0x23, 0x67, 0x8b, 0xb6, 0xe4, 0x74, 0xd2, 0x60, 0xfc, 0xfd, 0x43,
     0xbf, 0x91, 0x08, 0x81, 0xc5, 0x4f, 0x5d, 0x16, 0x9a, 0xc4, 0x9a, 0xc6, 0xf6, 0xf3, 0xe1, 0xf6,
     0x5c, 0x07, 0xaa, 0x71, 0x6c, 0x13, 0xa4, 0xb1, 0xb3, 0x66, 0xbf, 0x90, 0x4c, 0x3d, 0xa2, 0xc4,
     0x0b, 0xb8, 0x3d, 0x7a, 0x8c, 0x19, 0xfa, 0xff, 0x6b, 0xb9, 0x1f, 0x02, 0xcc, 0xb6, 0xd3, 0x0c,
     0x7d, 0x19, 0x1f, 0x47, 0xf9, 0xc7, 0x40, 0x01, 0xfa, 0x46, 0xea, 0x0b, 0xd4, 0x02, 0xe0, 0x3d,
     0x30, 0x9a, 0x1a, 0x0f, 0xea, 0xa7, 0x66, 0x55, 0xf7, 0xcb, 0x28, 0xe2, 0xbb, 0x99, 0xe4, 0x83,
     0xc3, 0x43, 0x03, 0xee, 0xdc, 0x1f, 0x02, 0x23, 0xdd, 0xd1, 0x2d, 0x39, 0xa4, 0x65, 0x75, 0x03,
     0xef, 0x37, 0x9c, 0x06, 0xd6, 0xfa, 0xa1, 0x15, 0xf0, 0xdb, 0x17, 0x47, 0x26, 0x4f, 0x49, 0x03,
    },
};

static const uint8_t META_FIELDS[][2] = {{0x0C, 0x0D}, {0x0E, 0x10}, {0x14, 0x18}, {0x1C, 0x20}};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static uint32_t adler32_of(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

static bool say(char *err, size_t errn, const char *text) {
    if (err && errn) snprintf(err, errn, "%s", text);
    return false;
}

void hosts_hex(uint64_t id, char out[17]) { snprintf(out, 17, "%016llx", (unsigned long long)id); }

// ---- NPDM

typedef struct {
    const uint8_t *aci, *acid;
    uint32_t aci_size, acid_size;
} NpdmView;

static bool npdm_view(const uint8_t *d, size_t n, NpdmView *v) {
    if (!d || n < 0x80 || memcmp(d, "META", 4)) return false;
    uint32_t a = rd32(d + 0x70), as = rd32(d + 0x74), b = rd32(d + 0x78), bs = rd32(d + 0x7c);
    if ((uint64_t)a + as > n || (uint64_t)b + bs > n || as < 0x40 || bs < 0x240) return false;
    v->aci = d + a;
    v->aci_size = as;
    v->acid = d + b;
    v->acid_size = bs;
    return !memcmp(v->aci, "ACI0", 4) && !memcmp(v->acid + 0x200, "ACID", 4);
}

uint64_t npdm_program(const uint8_t *npdm, size_t n) {
    NpdmView v;
    return npdm_view(npdm, n, &v) ? rd64(v.aci + 0x10) : 0;
}

bool npdm_acid_signed(const uint8_t *npdm, size_t n) {
    NpdmView v;
    if (!npdm_view(npdm, n, &v)) return false;
    uint32_t gen = rd32(npdm + 4), signed_size = rd32(v.acid + 0x204);
    if (gen >= 2 || signed_size < 0x100 || signed_size > v.acid_size - 0x100) return false;
    return rsa2048_pss_sha256_verify(ACID_MODULI[gen], v.acid, v.acid + 0x100, signed_size);
}

bool npdm_code_info_ok(const uint8_t *npdm, size_t n, const uint8_t *info, size_t in) {
    NpdmView v;
    if (!npdm_view(npdm, n, &v) || !info || in < 0x1a1 || info[0x1a0] != 1) return false;
    return rsa2048_pss_sha256_verify_digest(v.acid + 0x100, info, info + 0x180);
}

bool npdm_host_ok(const uint8_t *npdm, size_t n, uint64_t id, const uint8_t *info, size_t in, char *why, size_t whyn) {
    NpdmView v;
    char text[160];
    if (!npdm_view(npdm, n, &v)) return say(why, whyn, "not an NPDM");
    if (rd64(v.aci + 0x10) != id) {
        snprintf(text, sizeof(text), "it names %016llx", (unsigned long long)rd64(v.aci + 0x10));
        return say(why, whyn, text);
    }
    if (!npdm_acid_signed(npdm, n)) return say(why, whyn, "its ACID signature does not check out");
    if (id < rd64(v.acid + 0x210) || id > rd64(v.acid + 0x218)) return say(why, whyn, "its ACID does not allow it");
    if (info && !npdm_code_info_ok(npdm, n, info, in)) return say(why, whyn, "its code data does not verify under it");
    return true;
}

bool npdm_for_game(const uint8_t *host, size_t hn, const uint8_t *game, size_t gn, uint8_t **out) {
    if (hn < 0x80 || gn < 0x20 || memcmp(game, "META", 4)) return false;
    uint8_t *d = malloc(hn);
    if (!d) return false;
    memcpy(d, host, hn);
    for (int i = 0; i < 4; i++) memcpy(d + META_FIELDS[i][0], game + META_FIELDS[i][0], META_FIELDS[i][1] - META_FIELDS[i][0]);
    *out = d;
    return true;
}

// ---- files

static bool write_file(const char *path, const void *data, size_t n) {
    bool dir = false;
    if (vfs_stat(VFS_SD, path, &dir, NULL) == 0) vfs_delete(VFS_SD, path);
    if (vfs_create(VFS_SD, path, (int64_t)n) != 0) return false;
    VfsFile *f = vfs_open(VFS_SD, path, true);
    if (!f) return false;
    bool ok = (!n || vfs_write(f, 0, data, n) == 0) && vfs_flush(f) == 0;
    vfs_close(f);
    void *back = NULL;
    size_t got = 0;
    ok = ok && read_blob(VFS_SD, path, n, &back, &got) && got == n && !memcmp(back, data, n);
    free(back);
    return ok;
}

static bool exists(const char *path) {
    bool dir = false;
    return vfs_stat(VFS_SD, path, &dir, NULL) == 0 && !dir;
}

// ---- the host list

static void host_init(Host *h, uint64_t id) {
    memset(h, 0, sizeof(*h));
    h->id = id;
    char hex[17];
    hosts_hex(id, hex);
    memcpy(h->hex, hex, sizeof(hex));
    snprintf(h->dir, sizeof(h->dir), "/ns2jb-modloader/contents/%s", hex);
}

static bool parse_id(const char *s, uint64_t *id) {
    if (strlen(s) != 16) return false;
    for (int i = 0; i < 16; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    *id = strtoull(s, NULL, 16);
    return *id != 0;
}

int hosts_find(const HostSet *s, uint64_t id) {
    for (int i = 0; i < s->count; i++)
        if (s->list[i].id == id) return i;
    return -1;
}

static bool seedsow_npdm(uint8_t **out, size_t *n);

static void host_name(Host *h, AppEntry *apps, int *napps) {
    char p[96];
    snprintf(p, sizeof(p), HOSTS_DIR "/%s/host.ini", h->hex);
    char *ini = read_text(VFS_SD, p, 4096);
    if (ini) ini_get(ini, "name", h->name, sizeof(h->name));
    free(ini);
    if (h->name[0]) return;
    if (h->id == SEEDSOW_TITLE) { snprintf(h->name, sizeof(h->name), "Seedsow Lullaby"); return; }
    if (*napps < 0) *napps = apps_list(apps, 256);
    for (int i = 0; i < *napps; i++)
        if (apps[i].id == h->id && apps[i].name[0]) {
            snprintf(h->name, sizeof(h->name), "%s", apps[i].name);
            char text[160];
            snprintf(text, sizeof(text), "name=%s\n", h->name);
            snprintf(p, sizeof(p), HOSTS_DIR "/%s", h->hex);
            mkdirs(VFS_SD, p);
            snprintf(p, sizeof(p), HOSTS_DIR "/%s/host.ini", h->hex);
            write_text(VFS_SD, p, text);
            return;
        }
    snprintf(h->name, sizeof(h->name), "%s", h->hex);
}

void hosts_load(HostSet *s) {
    memset(s, 0, sizeof(*s));
    char *ini = read_text(VFS_SD, HOSTS_INI, 16384);
    if (ini) {
        s->ini = true;
        for (const char *p = ini; p && *p;) {
            const char *eol = strchr(p, '\n');
            char line[64];
            size_t len = eol ? (size_t)(eol - p) : strlen(p);
            if (len < sizeof(line)) {
                memcpy(line, p, len);
                line[len] = 0;
                if (len && line[len - 1] == '\r') line[len - 1] = 0;
                uint64_t id;
                if (!strncmp(line, "host=", 5) && parse_id(line + 5, &id) && s->count < HOSTS_MAX && hosts_find(s, id) < 0)
                    host_init(&s->list[s->count++], id);
                else if (!strncmp(line, "spare=", 6) && parse_id(line + 6, &id) && s->spare_count < HOST_SPARES_MAX)
                    s->spares[s->spare_count++] = id;
            }
            p = eol ? eol + 1 : NULL;
        }
        free(ini);
    }
    if (!s->count) host_init(&s->list[s->count++], SEEDSOW_TITLE);
    AppEntry *apps = malloc(256 * sizeof(AppEntry));
    int napps = -1;
    char p[96];
    for (int i = 0; i < s->count; i++) {
        Host *h = &s->list[i];
        if (apps) host_name(h, apps, &napps);
        else snprintf(h->name, sizeof(h->name), "%s", h->id == SEEDSOW_TITLE ? "Seedsow Lullaby" : h->hex);
        char q[96];
        snprintf(p, sizeof(p), HOSTS_DIR "/%s/main.npdm", h->hex);
        snprintf(q, sizeof(q), HOSTS_DIR "/%s/codeinfo.bin", h->hex);
        h->npdm = (exists(p) && exists(q)) || (h->id == SEEDSOW_TITLE && (exists(p) || seedsow_npdm(NULL, NULL)));
    }
    free(apps);
}

// ---- a host's NPDM

static bool npdm_in_image(const char *path, uint8_t **out, size_t *n) {
    uint8_t *d = NULL;
    size_t dn = 0;
    if (!exefs_read_npdm(path, &d, &dn)) return false;
    if (host_npdm_valid(d, dn)) {
        *out = d;
        *n = dn;
        return true;
    }
    free(d);
    return false;
}

bool hosts_npdm(const Host *h, uint8_t **out, size_t *n, char *err, size_t errn) {
    char p[128], why[120], text[320];
    void *npdm = NULL, *info = NULL;
    size_t nn = 0, in = 0;
    snprintf(p, sizeof(p), HOSTS_DIR "/%s/main.npdm", h->hex);
    if (read_blob(VFS_SD, p, 0x10000, &npdm, &nn)) {
        snprintf(p, sizeof(p), HOSTS_DIR "/%s/codeinfo.bin", h->hex);
        bool have_info = read_blob(VFS_SD, p, 0x1000, &info, &in);
        bool ok = npdm_host_ok(npdm, nn, h->id, have_info ? info : NULL, in, why, sizeof(why)) &&
                  (have_info || (h->id == SEEDSOW_TITLE && host_npdm_valid(npdm, nn)));
        free(info);
        if (ok) {
            *out = npdm;
            *n = nn;
            return true;
        }
        free(npdm);
        app_log("host %s: the NPDM in " HOSTS_DIR " is not used: %s", h->hex, have_info ? why : "no code data with it");
    }
    if (h->id == SEEDSOW_TITLE && seedsow_npdm(out, n)) return true;
    // Seedsow too: a card without the PC's files learns it from the loader like any other host
    snprintf(text, sizeof(text), "%s's permissions aren't known yet. Start it once with this loader, restart, run the "
             "launcher (A) and open SeedSwap again.", h->name);
    return say(err, errn, text);
}

// Seedsow's NPDM from before hosts had folders: the file older versions kept, else any game image
// built for it (then kept in that file). out NULL: only whether there is one.
static bool seedsow_npdm(uint8_t **out, size_t *n) {
    void *legacy = NULL;
    uint8_t *npdm = NULL;
    size_t nn = 0;
    if (read_blob(VFS_SD, LEGACY_HOST_NPDM, 0x10000, &legacy, &nn) && host_npdm_valid(legacy, nn)) {
        npdm = legacy;
    } else {
        free(legacy);
        bool found = npdm_in_image("/ns2jb-modloader/contents/0100ee5021c9e000/exefs.nsp", &npdm, &nn);
        VfsEntry *e = NULL;
        int count = 0;
        static const char *NAMES[] = {"exefs.nsp", "exefs-nocompat.nsp", "exefs-nodlc.nsp"};
        char p[320];
        if (!found && vfs_list(VFS_SD, SS_ROOT "/games", &e, &count) == 0) {
            for (int i = 0; !found && i < count; i++)
                for (int k = 0; !found && e[i].is_dir && k < 3; k++) {
                    snprintf(p, sizeof(p), SS_ROOT "/games/%s/%s", e[i].name, NAMES[k]);
                    found = npdm_in_image(p, &npdm, &nn);
                }
            free(e);
        }
        if (!found) return false;
        mkdirs(VFS_SD, SS_ROOT "/system");
        write_file(LEGACY_HOST_NPDM, npdm, nn);
    }
    if (out) {
        *out = npdm;
        *n = nn;
    } else {
        free(npdm);
    }
    return true;
}

// ---- loader reports

#define PROBE_NPDM_MAX 0x2000
#define PROBE_INFO_MAX 0x400
#define PROBE_ROW 0x80
#define PROBES_MAX 16

typedef struct {
    uint32_t total;
    uint8_t have[PROBE_NPDM_MAX / PROBE_ROW];
    uint8_t data[PROBE_NPDM_MAX];
} Part;

typedef struct {
    uint64_t program;
    Part npdm, info;
    bool bad;
} Probe;

struct HostProbe {
    Probe p[PROBES_MAX];
    int count;
};

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool hex_u64(const char *s, int digits, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < digits; i++) {
        int d = hexval(s[i]);
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return true;
}

// "<program 16> <offset<<32|total 16> <result 8> <hex>" after the event name
static void add_row(struct HostProbe *hp, const char *s, size_t len, bool npdm) {
    uint64_t program, value, result;
    if (len < 16 + 1 + 16 + 1 + 8 + 1 || s[16] != ' ' || s[33] != ' ' || s[42] != ' ') return;
    if (!hex_u64(s, 16, &program) || !hex_u64(s + 17, 16, &value) || !hex_u64(s + 34, 8, &result) || result) return;
    const char *hex = s + 43;
    size_t hl = len - 43;
    uint32_t offset = (uint32_t)(value >> 32), total = (uint32_t)value;
    uint32_t max = npdm ? PROBE_NPDM_MAX : PROBE_INFO_MAX;
    Probe *p = NULL;
    for (int i = 0; i < hp->count; i++)
        if (hp->p[i].program == program) p = &hp->p[i];
    if (!p) {
        if (hp->count >= PROBES_MAX) return;
        p = &hp->p[hp->count++];
        memset(p, 0, sizeof(*p));
        p->program = program;
    }
    Part *part = npdm ? &p->npdm : &p->info;
    uint32_t n = total - offset < PROBE_ROW ? total - offset : PROBE_ROW;
    if (!total || total > max || offset >= total || offset % PROBE_ROW || hl != (size_t)n * 2 ||
        (part->total && part->total != total)) {
        p->bad = true;
        return;
    }
    part->total = total;
    uint8_t row[PROBE_ROW];
    for (uint32_t i = 0; i < n; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) { p->bad = true; return; }
        row[i] = (uint8_t)(a << 4 | b);
    }
    uint32_t k = offset / PROBE_ROW;
    if (part->have[k] && memcmp(part->data + offset, row, n)) p->bad = true;   // launches disagree
    memcpy(part->data + offset, row, n);
    part->have[k] = 1;
}

static bool part_done(const Part *part) {
    if (!part->total) return false;
    for (uint32_t k = 0; k * PROBE_ROW < part->total; k++)
        if (!part->have[k]) return false;
    return true;
}

HostProbe *probe_parse(const char *text, int *count) {
    struct HostProbe *hp = calloc(1, sizeof(*hp));
    if (!hp) return NULL;
    static const char NP[] = "Host NPDM ", CI[] = "Host code info ";
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        if (len > sizeof(NP) - 1 && !strncmp(p, NP, sizeof(NP) - 1))
            add_row(hp, p + sizeof(NP) - 1, len - (sizeof(NP) - 1), true);
        else if (len > sizeof(CI) - 1 && !strncmp(p, CI, sizeof(CI) - 1))
            add_row(hp, p + sizeof(CI) - 1, len - (sizeof(CI) - 1), false);
        p = eol ? eol + 1 : NULL;
    }
    if (count) *count = hp->count;
    return hp;
}

bool probe_get(const HostProbe *hp, int i, uint64_t *program, const uint8_t **npdm, size_t *n, const uint8_t **info,
               size_t *in) {
    if (!hp || i < 0 || i >= hp->count) return false;
    const Probe *p = &hp->p[i];
    *program = p->program;
    if (p->bad || !part_done(&p->npdm) || !part_done(&p->info)) return false;
    *npdm = p->npdm.data;
    *n = p->npdm.total;
    *info = p->info.data;
    *in = p->info.total;
    return true;
}

void probe_free(HostProbe *p) { free(p); }

static int cmp_names_desc(const void *a, const void *b) {
    return strcmp(((const VfsEntry *)b)->name, ((const VfsEntry *)a)->name);
}

static bool store_host(Host *host, const uint8_t *npdm, size_t nn, const uint8_t *info, size_t in, const char *from) {
    char dir[96], p[128], ini[200];
    snprintf(dir, sizeof(dir), HOSTS_DIR "/%s", host->hex);
    mkdirs(VFS_SD, dir);
    snprintf(p, sizeof(p), "%s/codeinfo.bin", dir);
    bool ok = write_file(p, info, in);
    snprintf(p, sizeof(p), "%s/main.npdm", dir);
    ok = ok && write_file(p, npdm, nn);
    if (!ok) {
        app_log("host %s: storing its NPDM failed: %s", host->hex, vfs_last_error());
        return false;
    }
    snprintf(p, sizeof(p), "%s/host.ini", dir);
    snprintf(ini, sizeof(ini), "name=%s\nnpdm_from=%s\n", host->name, from);
    write_text(VFS_SD, p, ini);
    host->npdm = true;
    app_log("host %s (%s): NPDM from %s stored (%zu bytes)", host->hex, host->name, from, nn);
    return true;
}

static void host_file_path(const Host *h, char *out, size_t n) { snprintf(out, n, HOST_FILES_DIR "/%s.bin", h->hex); }

bool hosts_request(const Host *h) {
    char p[96];
    host_file_path(h, p, sizeof(p));
    if (h->npdm) {
        if (exists(p)) vfs_delete(VFS_SD, p);   // known another way since it was made (Seedsow's old sources)
        return true;
    }
    if (exists(p)) return true;
    mkdirs(VFS_SD, HOST_FILES_DIR);
    uint8_t *zero = calloc(1, HOST_FILE_BYTES);
    bool ok = zero && write_file(p, zero, HOST_FILE_BYTES);
    free(zero);
    if (ok) app_log("host %s: waiting for its data in %s", h->hex, p);
    else app_log("host %s: can't make %s: %s", h->hex, p, vfs_last_error());
    return ok;
}

// The loader's answer for one host, written when it started (hosts.h). False while it is still empty.
static bool import_host_file(Host *h) {
    char p[96];
    host_file_path(h, p, sizeof(p));
    void *d = NULL;
    size_t n = 0;
    if (!read_blob(VFS_SD, p, HOST_FILE_BYTES, &d, &n)) return false;
    const uint8_t *b = d;
    bool done = false;
    if (n >= 0x20 && !memcmp(b, "HPRB", 4)) {
        uint32_t nn = rd32(b + 16), in = rd32(b + 20);
        char why[120] = "";
        if (rd32(b + 4) != 1 || rd64(b + 8) != h->id || nn > 0x2000 || in > 0x400 || 0x20 + (size_t)nn + in > n)
            snprintf(why, sizeof(why), "its header doesn't fit");
        else if (npdm_host_ok(b + 0x20, nn, h->id, b + 0x20 + nn, in, why, sizeof(why)))
            done = store_host(h, b + 0x20, nn, b + 0x20 + nn, in, p);
        if (why[0]) app_log("host %s: the data in %s is not used: %s", h->hex, p, why);
        if (done) vfs_delete(VFS_SD, p);
    }
    free(d);
    return done;
}

int hosts_import(HostSet *s) {
    int wanted = 0, stored = 0;
    for (int i = 0; i < s->count; i++)
        if (!s->list[i].npdm && import_host_file(&s->list[i])) stored++;
    for (int i = 0; i < s->count; i++) wanted += !s->list[i].npdm;
    if (!wanted) return stored;
    VfsEntry *e = NULL;
    int count = 0;
    if (vfs_list(VFS_SD, LOADER_LOGS, &e, &count) != 0) return stored;
    qsort(e, (size_t)count, sizeof(VfsEntry), cmp_names_desc);
    for (int i = 0, from_reports = 0; i < count && from_reports < wanted; i++) {
        size_t nl = strlen(e[i].name);
        if (e[i].is_dir || strncmp(e[i].name, "onconsole-", 10) || nl < 5 || strcmp(e[i].name + nl - 4, ".log")) continue;
        char path[300];
        snprintf(path, sizeof(path), LOADER_LOGS "/%s", e[i].name);
        char *text = read_text(VFS_SD, path, 16u << 20);   // the newest one is locked while its loader runs
        if (!text) continue;
        int n = 0;
        HostProbe *hp = strstr(text, "Host NPDM ") ? probe_parse(text, &n) : NULL;
        free(text);
        for (int k = 0; k < n; k++) {
            uint64_t program;
            const uint8_t *npdm, *info;
            size_t nn, in;
            bool complete = probe_get(hp, k, &program, &npdm, &nn, &info, &in);
            int h = hosts_find(s, program);
            if (h < 0 || s->list[h].npdm) continue;
            Host *host = &s->list[h];
            char why[120];
            if (!complete) {
                app_log("host %s: the rows in %s are incomplete or disagree", host->hex, e[i].name);
                continue;
            }
            if (!npdm_host_ok(npdm, nn, host->id, info, in, why, sizeof(why))) {
                app_log("host %s: the NPDM in %s is not used: %s", host->hex, e[i].name, why);
                continue;
            }
            if (!store_host(host, npdm, nn, info, in, e[i].name)) continue;
            stored++;
            from_reports++;
            char p[96];
            host_file_path(host, p, sizeof(p));
            if (exists(p)) vfs_delete(VFS_SD, p);
        }
        probe_free(hp);
    }
    free(e);
    return stored;
}

// ---- ExeFS images

typedef struct {
    Pfs0Entry e[32];
    int count;
    uint64_t end;           // end of the last file's data
    int64_t size;
} Pfs;

static bool pfs_open(VfsFile *f, Pfs *x) {
    memset(x, 0, sizeof(*x));
    x->size = vfs_size(f);
    uint8_t *head = malloc(0x10000);
    size_t got = 0, needed = 0;
    bool ok = head && x->size > 16 && vfs_read(f, 0, head, 0x10000, &got) == 0 && got >= 16;
    x->count = ok ? pfs0_parse(head, got, (uint64_t)x->size, x->e, 32, &needed) : -1;
    free(head);
    if (x->count <= 0) return false;
    for (int i = 0; i < x->count; i++)
        if (x->e[i].offset + x->e[i].size > x->end) x->end = x->e[i].offset + x->e[i].size;
    return true;
}

static int pfs_find(const Pfs *x, const char *name) {
    for (int i = 0; i < x->count; i++)
        if (!strcmp(x->e[i].name, name)) return i;
    return -1;
}

static uint8_t *pfs_read(VfsFile *f, const Pfs0Entry *e, size_t max) {
    if (e->size > max) return NULL;
    uint8_t *d = malloc(e->size ? (size_t)e->size : 1);
    size_t got = 0;
    if (d && vfs_read(f, (int64_t)e->offset, d, (size_t)e->size, &got) == 0 && got == e->size) return d;
    free(d);
    return NULL;
}

bool exefs_read_npdm(const char *path, uint8_t **npdm, size_t *n) {
    VfsFile *f = vfs_open(VFS_SD, path, false);
    if (!f) return false;
    Pfs x;
    int i = pfs_open(f, &x) ? pfs_find(&x, "main.npdm") : -1;
    uint8_t *d = i >= 0 ? pfs_read(f, &x.e[i], 0x10000) : NULL;
    vfs_close(f);
    if (!d) return false;
    *npdm = d;
    *n = (size_t)x.e[i].size;
    return true;
}

static bool zero_tail(VfsFile *f, uint64_t from, uint64_t to) {
    uint8_t buf[1 << 16];
    for (uint64_t at = from; at < to;) {
        size_t n = to - at < sizeof(buf) ? (size_t)(to - at) : sizeof(buf), got = 0;
        if (vfs_read(f, (int64_t)at, buf, n, &got) != 0 || got != n) return false;
        for (size_t i = 0; i < n; i++)
            if (buf[i]) return false;
        at += n;
    }
    return true;
}

bool exefs_rehost(const char *path, const uint8_t *host_npdm, size_t hn, bool *changed, ProgressFn cb, char *err,
                  size_t errn) {
    *changed = false;
    char text[400], tmp[300];
    VfsFile *f = vfs_open(VFS_SD, path, false);
    if (!f) {
        snprintf(text, sizeof(text), "The game's code image can't be opened: %s", vfs_last_error());
        return say(err, errn, text);
    }
    Pfs x;
    int ni = pfs_open(f, &x) ? pfs_find(&x, "main.npdm") : -1;
    uint8_t *cur = ni >= 0 ? pfs_read(f, &x.e[ni], 0x10000) : NULL, *npdm = NULL, *buf = NULL;
    VfsFile *o = NULL;
    bool ok = false;
    if (!cur) { say(err, errn, "The game's code image has no readable main.npdm."); goto done; }
    if (!npdm_for_game(host_npdm, hn, cur, (size_t)x.e[ni].size, &npdm)) {
        say(err, errn, "The game's main.npdm is damaged.");
        goto done;
    }
    if (x.e[ni].size == hn && !memcmp(cur, npdm, hn)) { ok = true; goto done; }   // already the host's
    // the layout pc/rehost.py writes: header, entries, names back to back, then the files in order
    size_t names = 0;
    uint64_t data = 0;
    for (int i = 0; i < x.count; i++) {
        names += strlen(x.e[i].name) + 1;
        data += i == ni ? hn : x.e[i].size;
    }
    size_t head = 16 + (size_t)x.count * 0x18 + names;
    uint64_t total = head + data, pad_to = 0;
    if ((uint64_t)x.size > x.end && zero_tail(f, x.end, (uint64_t)x.size)) pad_to = (uint64_t)x.size;
    if (pad_to > total) total = pad_to;
    uint8_t *h = calloc(1, head);
    if (!h) { say(err, errn, "Not enough memory."); goto done; }
    memcpy(h, "PFS0", 4);
    wr32(h + 4, (uint32_t)x.count);
    wr32(h + 8, (uint32_t)names);
    uint64_t at = 0;
    size_t name_at = 0;
    for (int i = 0; i < x.count; i++) {
        uint64_t sz = i == ni ? hn : x.e[i].size;
        uint8_t *row = h + 16 + i * 0x18;
        wr64(row, at);
        wr64(row + 8, sz);
        wr32(row + 16, (uint32_t)name_at);
        size_t l = strlen(x.e[i].name) + 1;
        memcpy(h + 16 + x.count * 0x18 + name_at, x.e[i].name, l);
        name_at += l;
        at += sz;
    }
    snprintf(tmp, sizeof(tmp), "%s.host", path);
    vfs_delete(VFS_SD, tmp);
    buf = malloc(1 << 20);
    ok = buf && vfs_create(VFS_SD, tmp, (int64_t)total) == 0 && (o = vfs_open(VFS_SD, tmp, true)) &&
         vfs_write(o, 0, h, head) == 0;
    free(h);
    uint64_t out = head;
    for (int i = 0; ok && i < x.count; i++) {
        if (i == ni) {
            ok = vfs_write(o, (int64_t)out, npdm, hn) == 0;
            out += hn;
            continue;
        }
        for (uint64_t done = 0; ok && done < x.e[i].size;) {
            size_t n = x.e[i].size - done < (1 << 20) ? (size_t)(x.e[i].size - done) : (1 << 20), got = 0;
            ok = vfs_read(f, (int64_t)(x.e[i].offset + done), buf, n, &got) == 0 && got == n &&
                 vfs_write(o, (int64_t)out, buf, n) == 0;
            done += n;
            out += n;
            if (cb && ok && !cb(0.9f * (float)out / (float)total, "Moving the game to its host…")) ok = false;
        }
    }
    if (ok && out < total) {   // padding
        memset(buf, 0, 1 << 20);
        while (ok && out < total) {
            size_t n = total - out < (1 << 20) ? (size_t)(total - out) : (1 << 20);
            ok = vfs_write(o, (int64_t)out, buf, n) == 0;
            out += n;
        }
    }
    ok = ok && vfs_flush(o) == 0;
    if (o) vfs_close(o);
    o = NULL;
    vfs_close(f);
    f = NULL;
    if (ok) {   // read back: the table and main.npdm
        VfsFile *r = vfs_open(VFS_SD, tmp, false);
        Pfs y;
        int k = r && pfs_open(r, &y) ? pfs_find(&y, "main.npdm") : -1;
        uint8_t *back = k >= 0 ? pfs_read(r, &y.e[k], 0x10000) : NULL;
        ok = back && y.count == x.count && y.size == (int64_t)total && y.e[k].size == hn && !memcmp(back, npdm, hn);
        for (int i = 0; ok && i < x.count; i++) ok = !strcmp(y.e[i].name, x.e[i].name) && (i == k || y.e[i].size == x.e[i].size);
        free(back);
        if (r) vfs_close(r);
    }
    if (!ok) {
        snprintf(text, sizeof(text), "The game's code image couldn't be written for the host: %s", vfs_last_error());
        say(err, errn, text);
        vfs_delete(VFS_SD, tmp);
        goto done;
    }
    if (vfs_delete(VFS_SD, path) != 0 || vfs_rename(VFS_SD, tmp, path) != 0) {
        snprintf(text, sizeof(text), "The game's code image couldn't be swapped in (%s); the new one is %s.", vfs_last_error(), tmp);
        ok = say(err, errn, text);
        goto done;
    }
    *changed = true;
    app_log("%s: main.npdm now for %016llx (%llu bytes)", path, (unsigned long long)npdm_program(npdm, hn),
            (unsigned long long)total);
done:
    if (f) vfs_close(f);
    if (o) vfs_close(o);
    free(cur);
    free(npdm);
    free(buf);
    return ok;
}

// ---- the install image's slots

static int count_in(const uint8_t *hay, size_t n, const void *needle, size_t nl) {
    int c = 0;
    for (size_t i = 0; i + nl <= n;) {
        if (!memcmp(hay + i, needle, nl)) { c++; i += nl; }
        else i++;
    }
    return c;
}

static void replace_in(uint8_t *hay, size_t n, const void *from, const void *to, size_t nl) {
    for (size_t i = 0; i + nl <= n;) {
        if (!memcmp(hay + i, from, nl)) { memcpy(hay + i, to, nl); i += nl; }
        else i++;
    }
}

// image.lua's entry for context.bin: where its sha256 and adler32 values are
static bool context_entry(const char *lua, size_t *sha_at, size_t *adler_at, size_t *adler_len, uint64_t *bytes) {
    const char *c = strstr(lua, "context = {");
    const char *end = c ? strchr(c, '}') : NULL;
    if (!end) return false;
    const char *f = strstr(c, "file = \"context.bin\""), *b = strstr(c, "bytes = "), *s = strstr(c, "sha256 = \""),
               *a = strstr(c, "adler32 = ");
    if (!f || !b || !s || !a || f > end || b > end || s > end || a > end) return false;
    *bytes = strtoull(b + 8, NULL, 10);
    *sha_at = (size_t)(s + 10 - lua);
    a += 10;
    size_t l = 0;
    while (a[l] >= '0' && a[l] <= '9') l++;
    if (!l) return false;
    *adler_at = (size_t)(a - lua);
    *adler_len = l;
    return true;
}

static bool copy_file(const char *from, const char *to) {
    void *d = NULL;
    size_t n = 0;
    bool ok = read_blob(VFS_SD, from, 64u << 20, &d, &n) && write_file(to, d, n);
    free(d);
    return ok;
}

// image.lua's sha256 and adler32 of context.bin agree with ctx; where they are
static bool context_matches(const char *lua, const uint8_t *ctx, size_t cn, size_t *sha_at, size_t *adler_at,
                            size_t *adler_len) {
    uint64_t bytes;
    uint8_t digest[32];
    char hex[65];
    fsha256(ctx, cn, digest);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return context_entry(lua, sha_at, adler_at, adler_len, &bytes) && bytes == cn && !strncmp(lua + *sha_at, hex, 64) &&
           strtoull(lua + *adler_at, NULL, 10) == adler32_of(ctx, cn);
}

// a copy of image.lua with ctx's sha256 and adler32 in it (the caller frees it); NULL without memory
static char *lua_for_context(const char *lua, size_t sha_at, size_t adler_at, size_t adler_len, const uint8_t *ctx,
                             size_t cn) {
    uint8_t digest[32];
    char hex[65], adler[16];
    fsha256(ctx, cn, digest);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    snprintf(adler, sizeof(adler), "%lu", (unsigned long)adler32_of(ctx, cn));
    size_t ln = strlen(lua);
    char *out = malloc(ln + 32);
    if (!out) return NULL;
    memcpy(out, lua, sha_at);
    memcpy(out + sha_at, hex, 64);
    memcpy(out + sha_at + 64, lua + sha_at + 64, adler_at - sha_at - 64);
    size_t k = adler_at;
    memcpy(out + k, adler, strlen(adler));
    k += strlen(adler);
    memcpy(out + k, lua + adler_at + adler_len, ln - adler_at - adler_len + 1);
    return out;
}

// The old files go to backup_dir first; then each new file is written beside its old one and swapped in.
static bool swap_in(const char *install_dir, const char *const *names, const void *const *data, const size_t *len,
                    int n, const char *backup_dir, char *err, size_t errn) {
    char p[3][160], tmp[3][170], text[300];
    if (n > 3) return say(err, errn, "Too many files.");
    if (!mkdirs(VFS_SD, backup_dir)) return say(err, errn, "The backup folder can't be made.");
    for (int i = 0; i < n; i++) {
        char b[200];
        snprintf(p[i], sizeof(p[i]), "%s/%s", install_dir, names[i]);
        snprintf(b, sizeof(b), "%s/%s", backup_dir, names[i]);
        if (!copy_file(p[i], b)) {
            snprintf(text, sizeof(text), "The install image couldn't be backed up: %s", vfs_last_error());
            return say(err, errn, text);
        }
    }
    for (int i = 0; i < n; i++) {
        snprintf(tmp[i], sizeof(tmp[i]), "%s.new", p[i]);
        if (!write_file(tmp[i], data[i], len[i])) {
            snprintf(text, sizeof(text), "The install image couldn't be written: %s", vfs_last_error());
            for (int j = 0; j <= i; j++) vfs_delete(VFS_SD, tmp[j]);
            return say(err, errn, text);
        }
    }
    for (int i = 0; i < n; i++)
        if (vfs_delete(VFS_SD, p[i]) != 0 || vfs_rename(VFS_SD, tmp[i], p[i]) != 0) {
            snprintf(text, sizeof(text), "The install image was only partly changed (%s). Copy the files in %s back.",
                     vfs_last_error(), backup_dir);
            return say(err, errn, text);
        }
    return true;
}

bool hostimg_retitle(const char *install_dir, uint64_t spare, uint64_t to, const char *backup_dir, char *err,
                     size_t errn) {
    char p[3][160], old[17], new_[17], line[32];
    static const char *NAMES[3] = {"context.bin", "image.lua", "hosts.ini"};
    for (int i = 0; i < 3; i++) snprintf(p[i], sizeof(p[i]), "%s/%s", install_dir, NAMES[i]);
    hosts_hex(spare, old);
    hosts_hex(to, new_);
    char *ini = read_text(VFS_SD, p[2], 16384), *lua = NULL, *ini2 = NULL, *lua2 = NULL;
    uint8_t *ctx = NULL;
    size_t cn = 0;
    bool ok = false;
    if (!ini) { say(err, errn, "The install image has no host slots (hosts.ini). Build it with build_host_image.py."); goto done; }
    snprintf(line, sizeof(line), "spare=%s", old);
    char *at = strstr(ini, line);
    if (!at || (at != ini && at[-1] != '\n') || (at[strlen(line)] != '\n' && at[strlen(line)] != '\r' && at[strlen(line)])) {
        say(err, errn, "That slot is not free.");
        goto done;
    }
    char host_line[32], spare_line[32];
    snprintf(host_line, sizeof(host_line), "host=%s", new_);
    snprintf(spare_line, sizeof(spare_line), "spare=%s", new_);
    if (strstr(ini, host_line) || strstr(ini, spare_line)) { say(err, errn, "That game is a host already."); goto done; }
    int64_t cids = ini_get_i64(ini, "context_ids", -1), lids = ini_get_i64(ini, "image_ids", -1);
    lua = read_text(VFS_SD, p[1], 1u << 20);
    if (!read_blob(VFS_SD, p[0], 1u << 20, (void **)&ctx, &cn) || !lua) {
        say(err, errn, "The install image can't be read.");
        goto done;
    }
    uint8_t oldq[8], newq[8];
    wr64(oldq, spare);
    wr64(newq, to);
    if (count_in(ctx, cn, old, 16) != cids || count_in((const uint8_t *)lua, strlen(lua), old, 16) != lids ||
        count_in(ctx, cn, oldq, 8) != 1) {
        say(err, errn, "The install image does not look as hosts.ini says. Build it again on the PC.");
        goto done;
    }
    size_t sha_at, adler_at, adler_len;
    if (!context_matches(lua, ctx, cn, &sha_at, &adler_at, &adler_len)) {
        say(err, errn, "image.lua does not match context.bin. Build the image again on the PC.");
        goto done;
    }
    replace_in(ctx, cn, old, new_, 16);
    replace_in(ctx, cn, oldq, newq, 8);
    lua2 = lua_for_context(lua, sha_at, adler_at, adler_len, ctx, cn);
    if (!lua2) { say(err, errn, "Not enough memory."); goto done; }
    replace_in((uint8_t *)lua2, strlen(lua2), old, new_, 16);
    ini2 = malloc(strlen(ini) + 8);
    if (!ini2) { say(err, errn, "Not enough memory."); goto done; }
    size_t pre = (size_t)(at - ini);
    memcpy(ini2, ini, pre);
    snprintf(ini2 + pre, strlen(ini) + 8 - pre, "host=%s%s", new_, at + strlen(line));
    const void *data[3] = {ctx, lua2, ini2};
    size_t len[3] = {cn, strlen(lua2), strlen(ini2)};
    if (!swap_in(install_dir, NAMES, data, len, 3, backup_dir, err, errn)) goto done;
    ok = true;
    app_log("install image: slot %s is now %s (backup %s)", old, new_, backup_dir);
done:
    free(ini);
    free(lua);
    free(ctx);
    free(lua2);
    free(ini2);
    return ok;
}

// ---- the install image's save-guard user

static bool parse_user(const char *hex, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

// where the 32 digits of `user = "..."` start inside image.lua's one guard = { } block; NULL if not so
static const char *guard_user_in(const char *lua) {
    const char *g = strstr(lua, "guard = {");
    if (!g || strstr(g + 1, "guard = {")) return NULL;
    const char *end = strchr(g, '}'), *u = strstr(g, "user = \"");
    if (!end || !u || u > end) return NULL;
    u += 8;
    uint8_t b[16];
    return parse_user(u, b) && u[32] == '"' ? u : NULL;
}

// The image files the guard lives in, checked: image.lua's guard user is in context.bin exactly once
// and image.lua's checksums of context.bin hold.
typedef struct {
    char *lua, *ini;
    uint8_t *ctx;
    size_t cn, user_at, sha_at, adler_at, adler_len;
    uint8_t user[16];
} GuardImage;

static void guard_free(GuardImage *g) {
    free(g->lua);
    free(g->ini);
    free(g->ctx);
}

static bool guard_read(const char *install_dir, GuardImage *g, char *err, size_t errn) {
    char p[160];
    memset(g, 0, sizeof(*g));
    snprintf(p, sizeof(p), "%s/image.lua", install_dir);
    g->lua = read_text(VFS_SD, p, 1u << 20);
    snprintf(p, sizeof(p), "%s/context.bin", install_dir);
    if (!g->lua || !read_blob(VFS_SD, p, 1u << 20, (void **)&g->ctx, &g->cn))
        return say(err, errn, "The install image can't be read.");
    const char *u = guard_user_in(g->lua);
    if (!u) return say(err, errn, "The install image has no save guard.");
    g->user_at = (size_t)(u - g->lua);
    parse_user(u, g->user);
    if (count_in(g->ctx, g->cn, g->user, 16) != 1)
        return say(err, errn, "The install image does not look as expected. Build it again on the PC.");
    if (!context_matches(g->lua, g->ctx, g->cn, &g->sha_at, &g->adler_at, &g->adler_len))
        return say(err, errn, "image.lua does not match context.bin. Build the image again on the PC.");
    snprintf(p, sizeof(p), "%s/hosts.ini", install_dir);
    g->ini = read_text(VFS_SD, p, 16384);   // only images with host slots have it
    return true;
}

bool hostimg_guard_user(const char *install_dir, char out[33]) {
    GuardImage g;
    char err[200];
    bool ok = guard_read(install_dir, &g, err, sizeof(err));
    if (ok) snprintf(out, 33, "%.32s", g.lua + g.user_at);
    else out[0] = 0;
    guard_free(&g);
    return ok;
}

bool hostimg_set_guard(const char *install_dir, const char *user_hex, const char *backup_dir, char *err, size_t errn) {
    GuardImage g;
    char *lua2 = NULL, *ini2 = NULL, old[33], new_[33];
    uint8_t nu[16];
    bool ok = false;
    if (!guard_read(install_dir, &g, err, errn)) goto done;
    snprintf(old, sizeof(old), "%.32s", g.lua + g.user_at);
    snprintf(new_, sizeof(new_), "%.32s", user_hex);
    for (char *c = new_; *c; c++)
        if (*c >= 'A' && *c <= 'F') *c = (char)(*c + 32);
    static const uint8_t ZERO[16];
    if (strlen(new_) != 32 || !parse_user(new_, nu) || !memcmp(nu, ZERO, 16)) {
        say(err, errn, "That user ID is not valid.");
        goto done;
    }
    if (!strcmp(old, new_)) { ok = true; goto done; }
    if (count_in(g.ctx, g.cn, nu, 16)) {
        say(err, errn, "The install image does not look as expected. Build it again on the PC.");
        goto done;
    }
    replace_in(g.ctx, g.cn, g.user, nu, 16);
    memcpy(g.lua + g.user_at, new_, 32);
    lua2 = lua_for_context(g.lua, g.sha_at, g.adler_at, g.adler_len, g.ctx, g.cn);
    if (!lua2) { say(err, errn, "Not enough memory."); goto done; }
    static const char *NAMES[3] = {"context.bin", "image.lua", "hosts.ini"};
    const void *data[3] = {g.ctx, lua2, NULL};
    size_t len[3] = {g.cn, strlen(lua2), 0};
    int n = 2;
    char *gl = g.ini ? strstr(g.ini, "guard_user=") : NULL;
    if (gl && (gl == g.ini || gl[-1] == '\n') && strlen(gl + 11) >= 32) {   // hosts.ini names it too
        ini2 = strdup(g.ini);
        if (!ini2) { say(err, errn, "Not enough memory."); goto done; }
        memcpy(ini2 + (gl - g.ini) + 11, new_, 32);
        data[2] = ini2;
        len[2] = strlen(ini2);
        n = 3;
    }
    if (!swap_in(install_dir, NAMES, data, len, n, backup_dir, err, errn)) goto done;
    ok = true;
    app_log("install image: the save guard lets %s play now, was %s (backup %s)", new_, old, backup_dir);
done:
    guard_free(&g);
    free(lua2);
    free(ini2);
    return ok;
}

// ---- Nintendo's patches under the host's ID

bool hosts_patch_check(const char *exefs_path, const char *game_dir, uint64_t host, int *changes, ProgressFn cb,
                       char *err, size_t errn) {
    *changes = 0;
    if (host == SEEDSOW_TITLE) return true;   // Seedsow has none
    VfsFile *f = vfs_open(VFS_SD, exefs_path, false);
    if (!f) return say(err, errn, "The game's code image can't be opened.");
    Pfs x;
    int mi = pfs_open(f, &x) ? pfs_find(&x, "main") : -1;
    uint8_t *main_file = mi >= 0 ? pfs_read(f, &x.e[mi], 256u << 20) : NULL;
    vfs_close(f);
    if (!main_file) return say(err, errn, "The game's code image has no readable main.");
    size_t main_size = (size_t)x.e[mi].size;
    uint8_t digest[32];
    fsha256(main_file, main_size, digest);
    char key[17], want[64], hex[17], path[200], got[64];
    for (int i = 0; i < 8; i++) snprintf(key + 2 * i, 3, "%02x", digest[i]);
    hosts_hex(host, hex);
    snprintf(path, sizeof(path), "%s/hostcheck.ini", game_dir);
    char *ini = read_text(VFS_SD, path, 16384);
    if (ini && ini_get(ini, hex, got, sizeof(got)) && !strncmp(got, key, 16) && got[16] == ' ') {
        *changes = atoi(got + 17);
        free(ini);
        free(main_file);
        return true;
    }
    void *patcher = NULL;
    size_t patcher_size = 0;
    if (!read_blob(VFS_SD, COMPAT_PATCHER, 64u << 20, &patcher, &patcher_size)) {
        free(ini);
        free(main_file);
        return say(err, errn, "Nintendo's patcher isn't on the SD card, so SeedSwap can't tell whether this host's own "
                   "patches would hit the game. Run compat_sd.py --provision on the PC.");
    }
    if (cb) cb(0.05f, "Checking the host's patches against the game (this can take a minute)…");
    CompatReport report;
    uint8_t *out = NULL;
    size_t out_size = 0;
    int status = compat_patch_main(patcher, patcher_size, main_file, main_size, host, 0, &out, &out_size, &report);
    free(out);
    free(patcher);
    free(main_file);
    if (status != CMPT_OK) {
        char text[300];
        snprintf(text, sizeof(text), "The host's patches couldn't be checked: %s", compat_status_text(status));
        free(ini);
        return say(err, errn, text);
    }
    *changes = (int)report.change_count;
    // remember it: replace this host's line
    size_t cap = (ini ? strlen(ini) : 0) + 64;
    char *next = malloc(cap);
    if (next) {
        next[0] = 0;
        for (const char *p = ini; p && *p;) {
            const char *eol = strchr(p, '\n');
            size_t len = eol ? (size_t)(eol - p + 1) : strlen(p);
            if (strncmp(p, hex, 16) || p[16] != '=') strncat(next, p, len);
            p = eol ? eol + 1 : NULL;
        }
        snprintf(want, sizeof(want), "%s=%s %d\n", hex, key, *changes);
        strncat(next, want, cap - strlen(next) - 1);
        write_text(VFS_SD, path, next);
        free(next);
    }
    free(ini);
    app_log("host %s: its patches would change %d instruction%s of %s (routine %#x)", hex, *changes,
            *changes == 1 ? "" : "s", exefs_path, report.routine);
    return true;
}
