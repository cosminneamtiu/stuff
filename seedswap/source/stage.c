// SPDX-License-Identifier: GPL-2.0-only
#include "stage.h"

#include "hosts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

#define CHUNK (4u * 1024 * 1024)
#define NODE 0x4000u
#define MAX_HASH_AREA (512u * 1024 * 1024)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static bool fail(char *err, size_t errn, const char *text) {
    snprintf(err, errn, "%s", text);
    return false;
}

// ---- ExeFS

bool stage_exefs(Nca *program, uint64_t program_id, StageExefs *out, char *err, size_t errn) {
    memset(out, 0, sizeof(*out));
    NcaSection s;
    if (!nca_section(program, 1, 3, &s)) return fail(err, errn, "The game's program has no code section (ExeFS).");
    uint64_t pfs_off = rd64(s.fs + 0x40), pfs_size = rd64(s.fs + 0x48);
    if (pfs_off + pfs_size > s.end - s.start || pfs_size > 96u * 1024 * 1024)
        return fail(err, errn, "The game's code section lies outside its NCA; the dump is damaged.");
    uint8_t h[16];
    if (!nca_section_read(program, &s, pfs_off, h, 16) || memcmp(h, "PFS0", 4))
        return fail(err, errn, "The game's code does not decrypt: the title key or the keys file does not fit the dump.");
    uint32_t count = rd32(h + 4), strsize = rd32(h + 8);
    if (!count || count > STAGE_MAX_FILES || strsize > 0x10000)
        return fail(err, errn, "The game's code section has an unexpected file table.");
    size_t table = 16 + 0x18 * (size_t)count + strsize;
    uint8_t *t = malloc(table + 1);
    if (!t || !nca_section_read(program, &s, pfs_off, t, table)) {
        free(t);
        return fail(err, errn, "The game's code section could not be read.");
    }
    t[table] = 0;
    bool ok = true, have[4] = {0};
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *r = t + 16 + 0x18 * i;
        uint64_t off = rd64(r), size = rd64(r + 8);
        uint32_t name_at = rd32(r + 16);
        const char *name = name_at < strsize ? (const char *)t + 16 + 0x18 * count + name_at : "";
        if (!name[0] || strlen(name) >= 32 || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") ||
            !strcmp(name, "..") || table + off + size > pfs_size) {
            ok = false;
            break;
        }
        uint8_t *data = malloc(size ? (size_t)size : 1);
        if (!data || !nca_section_read(program, &s, pfs_off + table + off, data, (size_t)size)) {
            free(data);
            ok = false;
            break;
        }
        if (!strcmp(name, "main.npdm")) {
            out->game_npdm = data;
            out->game_npdm_size = (size_t)size;
            have[0] = true;
            continue;
        }
        StageFile *f = &out->files[out->count++];
        snprintf(f->name, sizeof(f->name), "%s", name);
        f->data = data;
        f->size = (size_t)size;
        have[1] |= !strcmp(name, "main");
        have[2] |= !strcmp(name, "rtld");
        have[3] |= !strcmp(name, "sdk");
    }
    free(t);
    if (!ok) {
        stage_exefs_free(out);
        return fail(err, errn, "A file in the game's code section is damaged or could not be read.");
    }
    if (!have[0] || !have[1] || !have[2] || !have[3]) {
        stage_exefs_free(out);
        return fail(err, errn, "The game's code section lacks main.npdm, main, rtld or sdk.");
    }
    const uint8_t *n = out->game_npdm;
    size_t nn = out->game_npdm_size;
    uint32_t aci = nn >= 0x80 ? rd32(n + 0x70) : 0, aci_size = nn >= 0x80 ? rd32(n + 0x74) : 0;
    if (nn < 0x80 || memcmp(n, "META", 4) || (uint64_t)aci + aci_size > nn || aci_size < 0x40 || memcmp(n + aci, "ACI0", 4) ||
        rd64(n + aci + 0x10) != program_id) {
        stage_exefs_free(out);
        return fail(err, errn, "The game's main.npdm does not name this program; the dump is damaged.");
    }
    return true;
}

void stage_exefs_free(StageExefs *x) {
    for (int i = 0; i < x->count; i++) free(x->files[i].data);
    free(x->game_npdm);
    memset(x, 0, sizeof(*x));
}

StageFile *stage_file(StageExefs *x, const char *name) {
    for (int i = 0; i < x->count; i++)
        if (!strcmp(x->files[i].name, name)) return &x->files[i];
    return NULL;
}

// sha256 of Seedsow's main.npdm with the transplanted fields zeroed (1604 bytes)
static const uint8_t HOST_MASKED[32] = {0x3b, 0xc1, 0x52, 0x4a, 0xb3, 0xfe, 0xe8, 0xaa, 0x35, 0x67, 0xb8,
                                        0xae, 0xcc, 0x28, 0x06, 0x91, 0x9d, 0x6e, 0x65, 0xf1, 0x4a, 0xe4,
                                        0xf7, 0x91, 0xb4, 0x8e, 0x4a, 0xd3, 0x74, 0xbf, 0x97, 0x69};
static const uint8_t META_FIELDS[][2] = {{0x0C, 0x0D}, {0x0E, 0x10}, {0x14, 0x18}, {0x1C, 0x20}};

bool host_npdm_valid(const uint8_t *npdm, size_t n) {
    if (n != 1604) return false;
    uint8_t *copy = malloc(n), digest[32];
    if (!copy) return false;
    memcpy(copy, npdm, n);
    for (int i = 0; i < 4; i++) memset(copy + META_FIELDS[i][0], 0, META_FIELDS[i][1] - META_FIELDS[i][0]);
    fsha256(copy, n, digest);
    free(copy);
    return !memcmp(digest, HOST_MASKED, 32);
}

bool stage_npdm(const uint8_t *host, size_t host_size, const uint8_t *game, size_t game_size, uint8_t **out, char *err,
                size_t errn) {
    if (!host_npdm_valid(host, host_size) && !npdm_acid_signed(host, host_size))
        return fail(err, errn, "The host's NPDM SeedSwap found is not signed by Nintendo.");
    if (game_size < 0x20 || memcmp(game, "META", 4)) return fail(err, errn, "The game's main.npdm is damaged.");
    uint8_t *n = malloc(host_size);
    if (!n) return fail(err, errn, "Not enough memory.");
    memcpy(n, host, host_size);
    for (int i = 0; i < 4; i++)
        memcpy(n + META_FIELDS[i][0], game + META_FIELDS[i][0], META_FIELDS[i][1] - META_FIELDS[i][0]);
    *out = n;
    return true;
}

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t size;
} ImageFile;

static int cmp_file(const void *a, const void *b) { return strcmp(((const ImageFile *)a)->name, ((const ImageFile *)b)->name); }

size_t stage_image_size(size_t unpadded) {
    if (unpadded <= STAGE_EXEFS_PAD) return STAGE_EXEFS_PAD;
    size_t mib = (size_t)1 << 20;
    return (unpadded + mib - 1) / mib * mib + STAGE_EXEFS_ROOM;
}

bool stage_image(const StageExefs *x, const uint8_t *npdm, size_t npdm_size, uint8_t **image, size_t *unpadded,
                 char *err, size_t errn) {
    ImageFile f[STAGE_MAX_FILES + 1];
    int n = 0;
    for (int i = 0; i < x->count; i++) f[n++] = (ImageFile){x->files[i].name, x->files[i].data, x->files[i].size};
    f[n++] = (ImageFile){"main.npdm", npdm, npdm_size};
    qsort(f, (size_t)n, sizeof(f[0]), cmp_file);
    size_t names = 0, data = 0;
    for (int i = 0; i < n; i++) {
        names += strlen(f[i].name) + 1;
        data += f[i].size;
    }
    size_t header = 16 + 0x18 * (size_t)n + names, total = header + data;
    if (total > STAGE_EXEFS_MAX) {
        snprintf(err, errn, "The game's code is %zu bytes, more than the %u MiB a SeedSwap game image can hold.", total,
                 (unsigned)(STAGE_EXEFS_MAX >> 20));
        return false;
    }
    uint8_t *img = calloc(1, stage_image_size(total));
    if (!img) return fail(err, errn, "Not enough memory.");
    memcpy(img, "PFS0", 4);
    wr32(img + 4, (uint32_t)n);
    wr32(img + 8, (uint32_t)names);
    size_t name_at = 0, at = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *r = img + 16 + 0x18 * i;
        wr64(r, at);
        wr64(r + 8, f[i].size);
        wr32(r + 16, (uint32_t)name_at);
        memcpy(img + 16 + 0x18 * n + name_at, f[i].name, strlen(f[i].name) + 1);
        name_at += strlen(f[i].name) + 1;
        memcpy(img + header + at, f[i].data, f[i].size);
        at += f[i].size;
    }
    *image = img;
    *unpadded = total;
    return true;
}

// ---- permissions

typedef struct {
    const uint8_t *aci, *acid;
    uint32_t aci_size, acid_size;
    const uint8_t *data;
    size_t size;
} Npdm;

static bool npdm_open(const uint8_t *d, size_t n, Npdm *x) {
    if (n < 0x80 || memcmp(d, "META", 4)) return false;
    uint32_t a = rd32(d + 0x70), as = rd32(d + 0x74), b = rd32(d + 0x78), bs = rd32(d + 0x7c);
    if ((uint64_t)a + as > n || (uint64_t)b + bs > n || as < 0x40 || bs < 0x240) return false;
    x->aci = d + a;
    x->aci_size = as;
    x->acid = d + b;
    x->acid_size = bs;
    return !memcmp(x->aci, "ACI0", 4) && !memcmp(x->acid + 0x200, "ACID", 4);
}

static const uint8_t *region(const uint8_t *block, uint32_t block_size, uint32_t at, uint32_t *size) {
    uint32_t off = rd32(block + at), sz = rd32(block + at + 4);
    if ((uint64_t)off + sz > block_size) { *size = 0; return block; }
    *size = sz;
    return block + off;
}

static uint64_t fs_flags(const uint8_t *block, uint32_t block_size, uint32_t at) {
    uint32_t n;
    const uint8_t *r = region(block, block_size, at, &n);
    return n >= 12 ? rd64(r + 4) : 0;
}

typedef struct {
    uint32_t syscalls[8];      // bitmap of 256
    bool thread;
    int lowest, highest, min_core, max_core;
    int handle_table, program_type, debug_flags;
    char kernel_version[12];
    uint32_t other[32];
    int other_count;
} Kcaps;

static void kcaps(const uint8_t *block, uint32_t block_size, uint32_t at, Kcaps *k) {
    memset(k, 0, sizeof(*k));
    k->handle_table = k->program_type = k->debug_flags = -1;
    uint32_t n;
    const uint8_t *r = region(block, block_size, at, &n);
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        uint32_t v = rd32(r + i);
        int ones = 0;
        while (ones < 32 && (v >> ones & 1)) ones++;
        if (ones == 3) {
            k->thread = true;
            k->lowest = (v >> 4) & 0x3f;
            k->highest = (v >> 10) & 0x3f;
            k->min_core = (v >> 16) & 0xff;
            k->max_core = (v >> 24) & 0xff;
        } else if (ones == 4) {
            uint32_t index = v >> 29, mask = (v >> 5) & 0xffffff;
            for (int b = 0; b < 24; b++)
                if (mask >> b & 1 && index * 24 + b < 256) k->syscalls[(index * 24 + b) / 32] |= 1u << ((index * 24 + b) % 32);
        } else if (ones == 13) k->program_type = (v >> 14) & 7;
        else if (ones == 14) snprintf(k->kernel_version, sizeof(k->kernel_version), "%u.%u", v >> 19, (v >> 15) & 0xf);
        else if (ones == 15) k->handle_table = (v >> 16) & 0x3ff;
        else if (ones == 16) k->debug_flags = (v >> 17) & 7;
        else if (k->other_count < 32) k->other[k->other_count++] = v;
    }
}

static const char *FS_BITS[64] = {
    [0] = "ApplicationInfo", [1] = "BootModeControl", [2] = "Calibration", [3] = "SystemSaveData", [4] = "GameCard",
    [5] = "SaveDataBackUp", [6] = "SaveDataManagement", [7] = "BisAllRaw", [8] = "GameCardRaw",
    [9] = "GameCardPrivate", [10] = "SetTime", [11] = "ContentManager", [12] = "ImageManager", [13] = "CreateSaveData",
    [14] = "SystemSaveDataManagement", [15] = "BisFileSystem", [16] = "SystemUpdate", [17] = "SaveDataMeta",
    [18] = "DeviceSaveData", [19] = "SettingsControl", [20] = "SystemData", [21] = "SdCard", [22] = "Host",
    [23] = "FillBis", [24] = "CorruptSaveData", [25] = "SaveDataForDebug", [26] = "FormatSdCard", [27] = "GetRightsId",
    [28] = "RegisterExternalKey", [29] = "RegisterUpdatePartition", [30] = "SaveDataTransfer",
    [31] = "DeviceDetection", [32] = "AccessFailureResolution", [33] = "SaveDataTransferVersion2",
    [34] = "RegisterProgramIndexMapInfo", [35] = "CreateOwnSaveData", [36] = "MoveCacheStorage", [62] = "Debug",
    [63] = "FullPermission"};

static bool has_service(const uint8_t *r, uint32_t n, const uint8_t *entry) {
    for (uint32_t at = 0; at < n;) {
        uint32_t len = (r[at] & 7) + 1;
        if (at + 1 + len > n) break;
        if (r[at] == entry[0] && !memcmp(r + at + 1, entry + 1, len)) return true;
        at += 1 + len;
    }
    return false;
}

static int add_line(PolicyLine *out, int n, int max, int level, const char *text) {
    if (n >= max) return n;
    out[n].level = level;
    snprintf(out[n].text, sizeof(out[n].text), "%s", text);
    return n + 1;
}

int stage_policy(const uint8_t *game, size_t game_size, const uint8_t *host, size_t host_size, int device_save,
                 const char *host_name, PolicyLine *out, int max) {
    const char *H = host_name && host_name[0] ? host_name : "the host";
    Npdm g, h;
    int n = 0;
    char text[320];
    if (!npdm_open(game, game_size, &g) || !npdm_open(host, host_size, &h))
        return add_line(out, n, max, POLICY_WARNING, "The game's permissions could not be read.");
    bool gaps = false;
    // services
    uint32_t gs, hs;
    const uint8_t *gr = region(g.aci, g.aci_size, 0x28, &gs), *hr = region(h.aci, h.aci_size, 0x28, &hs);
    char missing[256] = "";
    bool jit = false;
    for (uint32_t at = 0; at < gs;) {
        uint32_t len = (gr[at] & 7) + 1;
        if (at + 1 + len > gs) break;
        if (!has_service(hr, hs, gr + at)) {
            char name[12];
            snprintf(name, sizeof(name), "%.*s", (int)len, (const char *)gr + at + 1);
            if (!strncmp(name, "jit", 3)) jit = true;
            else if (strlen(missing) + strlen(name) + 3 < sizeof(missing))
                snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s%s", missing[0] ? ", " : "", name);
        }
        at += 1 + len;
    }
    if (jit) {
        gaps = true;
        snprintf(text, sizeof(text), "The game asks for the JIT service (jit:u), which %s may not use and which Switch 2 "
                 "does not have. It will close at start.", H);
        n = add_line(out, n, max, POLICY_BLOCKER, text);
    }
    if (missing[0]) {
        gaps = true;
        snprintf(text, sizeof(text), "The game asks for system services that %s is not allowed to open: %s. Opening "
                 "them fails; many games only use such services optionally and run anyway.", H, missing);
        n = add_line(out, n, max, POLICY_WARNING, text);
    }
    // file-system rights
    uint64_t host_fs = fs_flags(h.aci, h.aci_size, 0x20) & fs_flags(h.acid, h.acid_size, 0x220);
    uint64_t game_fs = fs_flags(g.aci, g.aci_size, 0x20);
    for (int bit = 0; bit < 64; bit++) {
        if (!((game_fs & ~host_fs) >> bit & 1)) continue;
        gaps = true;
        if (bit == 18) {
            snprintf(text, sizeof(text), "The game declares device save data (a save shared by all users); %s has "
                     "no right to it.%s", H, device_save == 2 ? " The fix applied here redirects it to the account save."
                     : device_save == 1 ? " Its code mounts it (nn::fs::MountDeviceSaveData), and the refused mount "
                     "usually closes the game at start. There is no fix for this game yet."
                     : " Its main program does not mount it directly, so this may not matter.");
            n = add_line(out, n, max, device_save == 2 ? POLICY_NOTE : POLICY_WARNING, text);
        } else {
            char unknown[12];
            snprintf(unknown, sizeof(unknown), "bit%d", bit);
            snprintf(text, sizeof(text), "The game declares the file-system right %s, which %s lacks. A call that "
                     "needs it is refused.", FS_BITS[bit] ? FS_BITS[bit] : unknown, H);
            n = add_line(out, n, max, POLICY_WARNING, text);
        }
    }
    Kcaps gk, hk;
    kcaps(g.aci, g.aci_size, 0x30, &gk);
    kcaps(h.aci, h.aci_size, 0x30, &hk);
    char calls[160] = "";
    bool jit_calls = false;
    for (int s = 0; s < 256; s++)
        if ((gk.syscalls[s / 32] >> (s % 32) & 1) && !(hk.syscalls[s / 32] >> (s % 32) & 1)) {
            if (s == 0x4b || s == 0x4c) jit_calls = true;
            if (strlen(calls) + 32 < sizeof(calls))
                snprintf(calls + strlen(calls), sizeof(calls) - strlen(calls), "%s%#04x%s", calls[0] ? ", " : "", s,
                         s == 0x4b ? " CreateCodeMemory (JIT)" : s == 0x4c ? " ControlCodeMemory (JIT)" : "");
        }
    if (calls[0]) {
        gaps = true;
        snprintf(text, sizeof(text), "The game may use system calls that %s's signed permissions do not include: %s. "
                 "The kernel refuses them%s.", H, calls, jit_calls ? "; a game that needs JIT closes at start" : "");
        n = add_line(out, n, max, jit_calls ? POLICY_BLOCKER : POLICY_WARNING, text);
    }
    if (!(gk.thread && hk.thread && hk.highest <= gk.highest && gk.lowest <= hk.lowest && hk.min_core <= gk.min_core &&
          gk.max_core <= hk.max_core)) {
        gaps = true;
        snprintf(text, sizeof(text), "The game wants thread priorities or CPU cores outside what %s may use. Creating "
                 "such a thread fails.", H);
        n = add_line(out, n, max, POLICY_WARNING, text);
    }
    if (gk.handle_table > (hk.handle_table < 0 ? 0 : hk.handle_table)) {
        gaps = true;
        snprintf(text, sizeof(text), "The game asks for a larger handle table (%d) than %s has (%d); a game that opens "
                 "very many handles could run out.", gk.handle_table, H, hk.handle_table);
        n = add_line(out, n, max, POLICY_WARNING, text);
    }
    if (gk.program_type != hk.program_type || gk.debug_flags != hk.debug_flags) {
        gaps = true;
        snprintf(text, sizeof(text), "The game's kernel settings (program type or debug flags) differ from %s's.", H);
        n = add_line(out, n, max, POLICY_WARNING, text);
    }
    for (int i = 0; i < gk.other_count; i++) {
        bool found = false;
        for (int j = 0; j < hk.other_count; j++) found |= gk.other[i] == hk.other[j];
        if (!found) {
            gaps = true;
            snprintf(text, sizeof(text), "The game declares kernel capabilities (memory maps or interrupts) that %s's do "
                     "not include.", H);
            n = add_line(out, n, max, POLICY_WARNING, text);
            break;
        }
    }
    if (!gaps) {
        snprintf(text, sizeof(text), "Everything the game declares (services, file-system rights, system calls, threads) is "
                 "covered by %s's signed permissions.", H);
        n = add_line(out, n, max, POLICY_OK, text);
    }
    if (strcmp(gk.kernel_version, hk.kernel_version)) {
        snprintf(text, sizeof(text), "The game was built for kernel %s and runs with %s's declared %s (harmless).",
                 gk.kernel_version, H, hk.kernel_version);
        n = add_line(out, n, max, POLICY_NOTE, text);
    }
    return n;
}

// ---- RomFS

typedef struct {
    uint64_t virt, len, phys, upper;
    uint8_t src;       // 0 base, 1 patch
    uint8_t raw;       // stored unencrypted
    uint8_t sparse;    // base, stored packed at `pos` (NCA offset) but encrypted for its section offset `phys`
    uint64_t pos;
} Extent;

typedef struct {
    Nca *base, *patch;
    NcaSection bs, ps;
    const uint8_t *fs;          // the IVFC header's section (patch when patched)
    int levels;
    uint64_t off[6], size[6];
    uint32_t log2[6];
    uint64_t vsize;             // the (patched) section's size
    Extent *ext;
    size_t next, cap;
    // a sparse base section (cartridge base with a bundled update): its map, 20-byte rows (virtual offset,
    // packed offset, storage 0 = held / 1 = absent); sp_count 0 with sparse set = nothing held at all
    bool sparse;
    uint8_t *sp_rows;
    uint32_t sp_count;
    uint64_t sp_phys, sp_table, sp_upper;
    char *err;
    size_t errn;
} Romfs;

static bool push(Romfs *r, Extent e) {
    if (r->next == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 1024;
        Extent *x = realloc(r->ext, cap * sizeof(Extent));
        if (!x) return false;
        r->ext = x;
        r->cap = cap;
    }
    r->ext[r->next++] = e;
    return true;
}

typedef bool (*TreeRead)(Romfs *r, uint64_t off, void *buf, size_t n);
static bool patch_meta_read(Romfs *r, uint64_t off, void *buf, size_t n) {
    return nca_section_read(r->patch, &r->ps, off, buf, n);
}
// The sparse map lies after the packed data, encrypted with the section's counter whose low 32 bits are the
// sparse generation << 16.
static bool sparse_meta_read(Romfs *r, uint64_t off, void *buf, size_t n) {
    return nca_read_ctr(r->base, r->sp_phys + r->sp_table + off, buf, n, r->sp_upper);
}

// A bucket tree (the patch's tables, or a sparse map): its rows, entry_size bytes each.
static bool bucket_tree_from(Romfs *r, TreeRead read, uint64_t table_off, uint64_t table_size, const uint8_t *hdr,
                             size_t entry_size, uint8_t **rows_out, uint32_t *count_out, uint64_t *end_out);
static bool bucket_tree(Romfs *r, uint64_t table_off, uint64_t table_size, const uint8_t *hdr, size_t entry_size,
                        uint8_t **rows_out, uint32_t *count_out, uint64_t *end_out) {
    return bucket_tree_from(r, patch_meta_read, table_off, table_size, hdr, entry_size, rows_out, count_out, end_out);
}
static bool bucket_tree_from(Romfs *r, TreeRead read, uint64_t table_off, uint64_t table_size, const uint8_t *hdr,
                             size_t entry_size, uint8_t **rows_out, uint32_t *count_out, uint64_t *end_out) {
    if (memcmp(hdr, "BKTR", 4)) return fail(r->err, r->errn, "The update's patch tables are damaged (no BKTR header).");
    int32_t count = (int32_t)rd32(hdr + 8);
    if (count <= 0 || count > 10000000) return fail(r->err, r->errn, "The update's patch tables are damaged.");
    uint64_t per_set = (NODE - 0x10) / entry_size, per_node = (NODE - 0x10) / 8;
    uint64_t sets = ((uint64_t)count + per_set - 1) / per_set, l2 = 0;
    if (sets > per_node) {
        l2 = (sets + per_node - 1) / per_node;
        l2 = (sets - (per_node - (l2 - 1)) + per_node - 1) / per_node;
    }
    uint64_t entries_start = table_off + (1 + l2) * NODE;
    if ((1 + l2 + sets) * NODE > table_size) return fail(r->err, r->errn, "The update's patch tables do not fit their area.");
    uint8_t *node = malloc(NODE), *rows = malloc((size_t)count * entry_size);
    if (!node || !rows) {
        free(node);
        free(rows);
        return fail(r->err, r->errn, "Not enough memory.");
    }
    bool ok = read(r, table_off, node, 16) && rd32(node) == 0 && (int32_t)rd32(node + 4) > 0;
    *end_out = ok ? rd64(node + 8) : 0;
    uint32_t got = 0;
    for (uint64_t i = 0; ok && i < sets; i++) {
        ok = read(r, entries_start + i * NODE, node, NODE);
        uint32_t index = rd32(node), n = rd32(node + 4);
        ok = ok && index == i && n > 0 && n <= per_set && got + n <= (uint32_t)count;
        if (ok) {
            memcpy(rows + (size_t)got * entry_size, node + 16, (size_t)n * entry_size);
            got += n;
        }
    }
    free(node);
    if (!ok || got != (uint32_t)count) {
        free(rows);
        return fail(r->err, r->errn, "The update's patch tables do not decrypt or are damaged: the update's ticket or the "
                    "keys file does not fit.");
    }
    *rows_out = rows;
    *count_out = got;
    return true;
}

static const char SPARSE_HOLE[] =
    "This update needs parts of the base game that the cartridge image does not hold: a cartridge that carries an "
    "update keeps only what that update reads. Convert the cartridge image without another update (its own update is "
    "used then), or use a full dump of the base game.";

// The map of a sparse base section (sw2_sparse.py on the PC is the same reader).
static bool sparse_load(Romfs *r) {
    const uint8_t *fs = r->bs.fs;
    r->sparse = true;
    int32_t count = (int32_t)rd32(fs + 0x160);
    if (count == 0) return true;   // the cartridge holds nothing of this section
    r->sp_table = rd64(fs + 0x148);
    uint64_t table_size = rd64(fs + 0x150);
    r->sp_phys = rd64(fs + 0x168);
    uint64_t generation = (uint64_t)fs[0x170] | (uint64_t)fs[0x171] << 8;
    r->sp_upper = (r->bs.upper & 0xFFFFFFFF00000000ull) | generation << 16;
    uint64_t end = 0, size = r->bs.end - r->bs.start;
    if (count < 0 || !bucket_tree_from(r, sparse_meta_read, 0, table_size, fs + 0x158, 20, &r->sp_rows, &r->sp_count, &end))
        return fail(r->err, r->errn, "The cartridge's base game is stored sparse, and its map does not decrypt or is "
                    "damaged: the keys file does not fit, or the cartridge image is damaged.");
    bool ok = end == size && rd64(r->sp_rows) == 0;
    for (uint32_t i = 0; ok && i < r->sp_count; i++) {
        const uint8_t *e = r->sp_rows + 20 * (size_t)i;
        uint64_t v = rd64(e), p = rd64(e + 8), stop = i + 1 < r->sp_count ? rd64(e + 20) : size;
        uint32_t storage = rd32(e + 16);
        ok = v < stop && storage <= 1 && (storage || (p + (stop - v) <= r->sp_table && ((p - v) & 15) == 0));
    }
    if (!ok) return fail(r->err, r->errn, "The cartridge's base game is stored sparse, and its map is damaged.");
    return true;
}

// A piece of the base section [p, p+len) placed at virt: through the sparse map when there is one.
static bool push_base(Romfs *r, uint64_t virt, uint64_t len, uint64_t p) {
    if (!r->sparse)
        return push(r, (Extent){virt, len, p, r->bs.upper, 0, 0, 0, 0}) || fail(r->err, r->errn, "Not enough memory.");
    uint64_t size = r->bs.end - r->bs.start;
    while (len) {
        if (!r->sp_count) return fail(r->err, r->errn, SPARSE_HOLE);
        uint32_t lo = 0, hi = r->sp_count;
        while (hi - lo > 1) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (rd64(r->sp_rows + 20 * (size_t)mid) <= p) lo = mid;
            else hi = mid;
        }
        const uint8_t *e = r->sp_rows + 20 * (size_t)lo;
        uint64_t rv = rd64(e), rp = rd64(e + 8), stop = lo + 1 < r->sp_count ? rd64(e + 20) : size;
        if (rd32(e + 16) != 0 || p < rv || p >= stop) return fail(r->err, r->errn, SPARSE_HOLE);
        uint64_t k = stop - p < len ? stop - p : len;
        if (!push(r, (Extent){virt, k, p, r->bs.upper, 0, 0, 1, r->sp_phys + rp + (p - rv)}))
            return fail(r->err, r->errn, "Not enough memory.");
        virt += k;
        p += k;
        len -= k;
    }
    return true;
}

static bool plan(Romfs *r) {
    if (!nca_section(r->base, 0, 3, &r->bs))
        return fail(r->err, r->errn, "The base game's program has no game data (RomFS) section that SeedSwap can read.");
    if (r->patch && !nca_section(r->patch, 0, 4, &r->ps))
        return fail(r->err, r->errn, "The update's program has no patch for the game data (BKTR section).");
    r->fs = r->patch ? r->ps.fs : r->bs.fs;
    if (memcmp(r->fs + 8, "IVFC", 4)) return fail(r->err, r->errn, "The game data has no hash tree (IVFC).");
    uint32_t layers = rd32(r->fs + 0x14);
    if (layers < 2 || layers > 7 || rd32(r->fs + 0x10) > 0x40)
        return fail(r->err, r->errn, "The game data's hash tree has an unexpected shape.");
    r->levels = (int)layers - 1;
    for (int l = 0; l < r->levels; l++) {
        r->off[l] = rd64(r->fs + 0x18 + 0x18 * l);
        r->size[l] = rd64(r->fs + 0x20 + 0x18 * l);
        r->log2[l] = rd32(r->fs + 0x28 + 0x18 * l);
        if (r->log2[l] < 9 || r->log2[l] > 24) return fail(r->err, r->errn, "The game data's hash tree has an unexpected block size.");
    }
    int d = r->levels - 1;
    for (int l = 0; l < d; l++)
        if (r->off[l] + r->size[l] > r->off[d])
            return fail(r->err, r->errn, "The game data's hash tree is laid out in a way SeedSwap does not handle.");
    if (r->off[d] > MAX_HASH_AREA) return fail(r->err, r->errn, "The game data's hash tree is larger than expected.");
    for (int i = 0x178; i < 0x1a0; i++)
        if (r->bs.fs[i]) return fail(r->err, r->errn, "This game stores its data in a compressed layout that SeedSwap "
                                     "does not read yet.");
    if (!memcmp(r->bs.fs + 0x158, "BKTR", 4)) {
        if (!r->patch)
            return fail(r->err, r->errn, "This base game is stored sparse (a cartridge that carries an update keeps only "
                        "what that update reads), so it can only be converted together with an update.");
        if (!sparse_load(r)) return false;
    } else {
        for (int i = 0x148; i < 0x178; i++)
            if (r->bs.fs[i]) return fail(r->err, r->errn, "This game stores its data in a sparse layout that SeedSwap does "
                                         "not read.");
    }
    if (!r->patch) {
        r->vsize = r->bs.end - r->bs.start;
        if (r->off[d] + r->size[d] > r->vsize) return fail(r->err, r->errn, "The game data runs past its section; the dump is damaged.");
        return push_base(r, 0, r->vsize, 0);
    }
    const uint8_t *fs = r->ps.fs;
    for (int i = 0x148; i < 0x1a0; i++)
        if (fs[i]) return fail(r->err, r->errn, "This update stores its data in a sparse or compressed layout that SeedSwap "
                               "does not read yet.");
    uint8_t *ind = NULL, *ctr = NULL;
    uint32_t nind = 0, nctr = 0;
    uint64_t ctr_end = 0;
    if (!bucket_tree(r, rd64(fs + 0x100), rd64(fs + 0x108), fs + 0x110, 20, &ind, &nind, &r->vsize)) return false;
    if (!bucket_tree(r, rd64(fs + 0x120), rd64(fs + 0x128), fs + 0x130, 16, &ctr, &nctr, &ctr_end)) {
        free(ind);
        return false;
    }
    bool ok = rd64(ind) == 0 && rd64(ctr) == 0 && r->off[d] + r->size[d] <= r->vsize;
    uint64_t base_size = r->bs.end - r->bs.start;
    uint32_t j = 0;
    for (uint32_t i = 0; ok && i < nind; i++) {
        const uint8_t *e = ind + 20 * (size_t)i;
        uint64_t v = rd64(e), p = rd64(e + 8), end = i + 1 < nind ? rd64(ind + 20 * (size_t)(i + 1)) : r->vsize;
        uint32_t storage = rd32(e + 16);
        if (!(v < end) || storage > 1 || p + (end - v) > (storage ? ctr_end : base_size) || (int64_t)p < 0) {
            ok = false;
            break;
        }
        if (storage == 0) {
            if (!push_base(r, v, end - v, p)) {   // its own sentence (memory, or a piece a sparse base lacks)
                free(ind);
                free(ctr);
                return false;
            }
            continue;
        }
        // in the patch: split where the encryption entries change
        uint64_t at = p, left = end - v, virt = v;
        while (ok && left) {
            while (j + 1 < nctr && rd64(ctr + 16 * (size_t)(j + 1)) <= at) j++;
            while (j > 0 && rd64(ctr + 16 * (size_t)j) > at) j--;
            const uint8_t *c = ctr + 16 * (size_t)j;
            uint64_t c_end = j + 1 < nctr ? rd64(ctr + 16 * (size_t)(j + 1)) : ctr_end;
            if (c_end <= at || c[8] > 1) { ok = false; break; }
            uint64_t k = c_end - at < left ? c_end - at : left;
            uint64_t upper = (r->ps.upper & 0xFFFFFFFF00000000ull) | rd32(c + 12);
            ok = push(r, (Extent){virt, k, at, upper, 1, c[8] == 1, 0, 0});
            at += k;
            virt += k;
            left -= k;
        }
    }
    free(ind);
    free(ctr);
    if (!ok) return fail(r->err, r->errn, "The update's patch tables point outside the game data: the update does not "
                         "belong to this base game, or one of the files is damaged.");
    return true;
}

static int cmp_extent(const void *a, const void *b) {
    const Extent *x = a, *y = b;
    if (x->src != y->src) return x->src < y->src ? -1 : 1;
    return x->phys < y->phys ? -1 : x->phys > y->phys ? 1 : 0;
}

static bool extent_read(Romfs *r, const Extent *e, uint64_t skip, uint8_t *buf, size_t n) {
    if (e->src == 0 && e->sparse)
        return nca_read_ctr_at(r->base, e->pos + skip, r->bs.start + e->phys + skip, buf, n, e->upper);
    if (e->src == 0) return nca_read_ctr(r->base, r->bs.start + e->phys + skip, buf, n, e->upper);
    if (e->raw) return nca_read_raw(r->patch, r->ps.start + e->phys + skip, buf, n);
    return nca_read_ctr(r->patch, r->ps.start + e->phys + skip, buf, n, e->upper);
}

static bool blocks_ok(const uint8_t *table, uint64_t table_size, uint64_t first, const uint8_t *data, size_t size,
                      size_t block) {
    uint8_t digest[32];
    for (size_t at = 0, i = (size_t)first; at < size; at += block, i++) {
        if ((i + 1) * 32 > table_size) return false;
        size_t n = size - at < block ? size - at : block;
        fsha256(data + at, n, digest);
        if (!memcmp(digest, table + 32 * i, 32)) continue;
        if (n == block) return false;
        FastSha256 ctx;
        fsha256_init(&ctx);
        fsha256_update(&ctx, data + at, n);
        static const uint8_t zero[4096];
        for (size_t left = block - n; left;) {
            size_t k = left < sizeof(zero) ? left : sizeof(zero);
            fsha256_update(&ctx, zero, k);
            left -= k;
        }
        fsha256_final(&ctx, digest);
        if (memcmp(digest, table + 32 * i, 32)) return false;
    }
    return true;
}

typedef struct {
    ProgressFn cb;
    uint64_t done, total;
    uint64_t last_ms;
} Progress;

static bool tick(Progress *p, uint64_t add, const char *phase, uint64_t of_done, uint64_t of_total) {
    p->done += add;
    uint64_t now = plat_ms();
    if (!p->cb || (now - p->last_ms < 250 && p->done < p->total)) return true;
    p->last_ms = now;
    char text[160];
    snprintf(text, sizeof(text), "%s: %.2f of %.2f GB", phase, (double)of_done / 1e9, (double)of_total / 1e9);
    return p->cb(p->total ? (float)((double)p->done / (double)p->total) : -1.0f, text);
}

bool stage_romfs_size(Nca *base, Nca *patch, uint64_t *size, char *err, size_t errn) {
    NcaSection s;
    if (!nca_section(patch ? patch : base, 0, patch ? 4 : 3, &s) || memcmp(s.fs + 8, "IVFC", 4)) {
        snprintf(err, errn, "The game data section cannot be found.");
        return false;
    }
    uint32_t layers = rd32(s.fs + 0x14);
    if (layers < 2 || layers > 7) {
        snprintf(err, errn, "The game data's hash tree has an unexpected shape.");
        return false;
    }
    *size = rd64(s.fs + 0x20 + 0x18 * (layers - 2));
    return true;
}

bool stage_romfs(Nca *base, Nca *patch, const char *path, ProgressFn cb, RomfsResult *out, char *err, size_t errn) {
    memset(out, 0, sizeof(*out));
    Romfs r = {.base = base, .patch = patch, .err = err, .errn = errn};
    uint8_t *hashes = NULL, *buf = NULL;
    VfsFile *f = NULL;
    bool ok = false, created = false;
    if (!plan(&r)) goto done;
    int d = r.levels - 1;
    if (d < 1) {
        fail(err, errn, "The game data's hash tree has only one level.");
        goto done;
    }
    uint64_t data_off = r.off[d], data_size = r.size[d], hash_end = data_off;
    out->size = data_size;
    out->levels = r.levels;
    out->patched = patch != NULL;
    hashes = malloc(hash_end ? (size_t)hash_end : 1);
    buf = malloc(CHUNK);
    if (!hashes || !buf) {
        fail(err, errn, "Not enough memory.");
        goto done;
    }
    qsort(r.ext, r.next, sizeof(Extent), cmp_extent);
    Progress pr = {.cb = cb, .total = hash_end + 2 * data_size};

    // 1. the hash levels, checked against the master hash before anything is written
    for (size_t i = 0; i < r.next; i++) {
        const Extent *e = &r.ext[i];
        if (e->virt >= hash_end) continue;
        uint64_t len = e->virt + e->len > hash_end ? hash_end - e->virt : e->len;
        for (uint64_t at = 0; at < len;) {
            size_t k = len - at < CHUNK ? (size_t)(len - at) : CHUNK;
            if (!extent_read(&r, e, at, hashes + e->virt + at, k)) {
                fail(err, errn, "The game data could not be read; the dump is cut short or damaged.");
                goto done;
            }
            at += k;
            if (!tick(&pr, k, "Reading the hash tree", pr.done, hash_end)) {
                fail(err, errn, "Stopped.");
                goto done;
            }
        }
    }
    const uint8_t *master = r.fs + 0xc8;
    uint32_t master_size = rd32(r.fs + 0x10);
    uint8_t digest[32];
    fsha256(hashes + r.off[0], (size_t)r.size[0], digest);
    bool chain = !memcmp(digest, master, 32) ||
                 blocks_ok(master, master_size, 0, hashes + r.off[0], (size_t)r.size[0], (size_t)1 << r.log2[0]);
    for (int l = 1; chain && l < d; l++)
        chain = blocks_ok(hashes + r.off[l - 1], r.size[l - 1], 0, hashes + r.off[l], (size_t)r.size[l], (size_t)1 << r.log2[l]);
    if (!chain) {
        fail(err, errn, patch ? "The update does not fit this base game: after combining the two, the game data's hash "
                                "tree is wrong. The update was made for another base dump (another region or revision), "
                                "or one of the two files is damaged, or a ticket does not fit."
                              : "The game data fails its own checksums: the dump is damaged, or its ticket and the keys "
                                "file do not fit. Dump or download it again.");
        goto done;
    }
    const uint8_t *table = hashes + r.off[d - 1];
    uint64_t table_size = r.size[d - 1];

    // 2. the data, in the order it lies in the dumps, each piece written where it belongs
    vfs_delete(VFS_SD, path);
    if (vfs_create(VFS_SD, path, (int64_t)data_size) != 0) {
        snprintf(err, errn, "SeedSwap could not create a %.2f GB file on the SD card (%s). A FAT32 card holds files up to "
                 "4 GB only; exFAT is needed.", (double)data_size / 1e9, vfs_last_error());
        goto done;
    }
    created = true;
    f = vfs_open(VFS_SD, path, true);
    if (!f) {
        snprintf(err, errn, "SeedSwap could not open %s for writing (%s).", path, vfs_last_error());
        goto done;
    }
    uint64_t copied = 0;
    for (size_t i = 0; i < r.next; i++) {
        const Extent *e = &r.ext[i];
        uint64_t from = e->virt < data_off ? data_off : e->virt;
        uint64_t to = e->virt + e->len < data_off + data_size ? e->virt + e->len : data_off + data_size;
        if (from >= to) continue;
        for (uint64_t at = from; at < to;) {
            size_t k = to - at < CHUNK ? (size_t)(to - at) : CHUNK;
            if (!extent_read(&r, e, at - e->virt, buf, k)) {
                fail(err, errn, "The game data could not be read; the dump is cut short or damaged.");
                goto done;
            }
            if (vfs_write(f, (int64_t)(at - data_off), buf, k) != 0) {
                snprintf(err, errn, "Writing to the SD card failed (%s).", vfs_last_error());
                goto done;
            }
            if (e->src == 0) out->from_base += k;
            else out->from_patch += k;
            at += k;
            copied += k;
            if (!tick(&pr, k, "Copying the game data", copied, data_size)) {
                fail(err, errn, "Stopped.");
                goto done;
            }
        }
    }
    if (vfs_flush(f) != 0) {
        snprintf(err, errn, "Writing to the SD card failed (%s).", vfs_last_error());
        goto done;
    }
    vfs_close(f);

    // 3. read back: every block against the hash tree, and the whole file's SHA-256
    f = vfs_open(VFS_SD, path, false);
    if (!f || vfs_size(f) != (int64_t)data_size) {
        fail(err, errn, "The game data file on the SD card does not have the size it was written with.");
        goto done;
    }
    size_t block = (size_t)1 << r.log2[d], step = CHUNK / block * block;
    if (!step) step = block;
    FastSha256 whole;
    fsha256_init(&whole);
    for (uint64_t at = 0; at < data_size;) {
        size_t k = data_size - at < step ? (size_t)(data_size - at) : step, got = 0;
        if (k > CHUNK) {
            fail(err, errn, "The game data uses blocks larger than SeedSwap reads at once.");
            goto done;
        }
        if (vfs_read(f, (int64_t)at, buf, k, &got) != 0 || got != k) {
            snprintf(err, errn, "Reading the game data back from the SD card failed (%s).", vfs_last_error());
            goto done;
        }
        if (at == 0 && (k < 8 || rd64(buf) != 0x50)) {
            fail(err, errn, "The game data does not start with a RomFS header.");
            goto done;
        }
        if (!blocks_ok(table, table_size, at / block, buf, k, block)) {
            snprintf(err, errn, "The game data read back from the SD card fails its checksum near %.2f GB: %s",
                     (double)at / 1e9, patch ? "the update does not fit this base game, or a dump or the SD card is damaged."
                                             : "the dump or the SD card is damaged.");
            goto done;
        }
        fsha256_update(&whole, buf, k);
        at += k;
        if (!tick(&pr, k, "Checking what was written", at, data_size)) {
            fail(err, errn, "Stopped.");
            goto done;
        }
    }
    fsha256_final(&whole, out->sha256);
    out->restarts = (base->ncz ? ncz_restarts(base->ncz) : 0) + (patch && patch->ncz ? ncz_restarts(patch->ncz) : 0);
    ok = true;
done:
    if (f) vfs_close(f);
    if (!ok && created) vfs_delete(VFS_SD, path);
    free(hashes);
    free(buf);
    free(r.ext);
    free(r.sp_rows);
    return ok;
}
