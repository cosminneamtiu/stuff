// SPDX-License-Identifier: GPL-2.0-only
#include "dlcpkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat/nsofile.h"
#include "crypto.h"
#include "dumpcheck.h"

#define MAX_ENTRIES 64
#define MAX_TICKETS 8

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcasecmp(s + a - b, suffix);
}

typedef struct {
    uint8_t rights_id[16];
    uint8_t title_key[16];        // still encrypted with the title KEK
    bool common;
    int key_generation;
    uint64_t device_id;
    bool bad;
} Ticket;

typedef struct {
    DlcReadFn read;
    void *user;
    uint64_t size;
    const char *keys;
    Pfs0Entry entries[MAX_ENTRIES];
    int count;
    Ticket tickets[MAX_TICKETS];
    int ticket_count;
    uint8_t header_key[32];
    char *err;
    size_t errn;
} Ctx;

static bool fail(Ctx *c, const char *fmt, const char *arg) {
    snprintf(c->err, c->errn, fmt, arg ? arg : "");
    return false;
}

static bool read_at(Ctx *c, uint64_t off, void *buf, size_t n) {
    return off + n <= c->size && c->read(c->user, off, buf, n) == 0;
}

static void parse_ticket(Ctx *c, const uint8_t *data, size_t n) {
    if (c->ticket_count >= MAX_TICKETS) return;
    Ticket *t = &c->tickets[c->ticket_count++];
    memset(t, 0, sizeof(*t));
    uint32_t type = n >= 4 ? rd32(data) : 0;
    size_t sig = type == 0x10000 || type == 0x10003 ? 0x240 : type == 0x10001 || type == 0x10004 ? 0x140
               : type == 0x10002 || type == 0x10005 ? 0x80 : 0;
    if (!sig || n < sig + 0x180) { t->bad = true; return; }
    const uint8_t *body = data + sig;
    memcpy(t->rights_id, body + 0x160, 16);
    memcpy(t->title_key, body + 0x40, 16);
    t->common = body[0x141] == 0;
    t->key_generation = body[0x145];
    t->device_id = rd64(body + 0x158);
}

// The NCA header, decrypted.
static bool nca_header(Ctx *c, const Pfs0Entry *e, uint8_t header[0xc00]) {
    uint8_t raw[0xc00];
    if (e->size < 0xc00 || !read_at(c, e->offset, raw, sizeof(raw))) return fail(c, "%s is cut short.", e->name);
    aes128_xts_decrypt(c->header_key, 0, raw, header, sizeof(raw));
    if (memcmp(header + 0x200, "NCA3", 4))
        return fail(c, "%s does not decrypt with the header_key in prod.keys: the keys file is wrong or the dump is "
                       "damaged.", e->name);
    return true;
}

static int key_generation(const uint8_t *header) {
    int g = header[0x206] > header[0x220] ? header[0x206] : header[0x220];
    return g > 0 ? g - 1 : 0;
}

// The key for the NCA's sections: from the ticket (rights ID set) or from the key area.
static bool section_key(Ctx *c, const Pfs0Entry *e, const uint8_t *header, uint8_t key[16]) {
    char name[48];
    int gen = key_generation(header);
    static const uint8_t zero[16];
    if (memcmp(header + 0x230, zero, 16)) {
        const Ticket *t = NULL;
        for (int i = 0; i < c->ticket_count; i++)
            if (!c->tickets[i].bad && !memcmp(c->tickets[i].rights_id, header + 0x230, 16)) t = &c->tickets[i];
        if (!t)
            return fail(c, "The dump holds no ticket for %s, so it cannot be decrypted. Dump it again with its ticket "
                           "(a \"standard crypto\" or common-ticket dump).", e->name);
        if (!t->common) {
            snprintf(c->err, c->errn,
                     "The dump's ticket is personalised (it belongs to the console with device ID %016llx): its key "
                     "can only be read on that console's own system. Dump it with a common ticket instead.",
                     (unsigned long long)t->device_id);
            return false;
        }
        snprintf(name, sizeof(name), "titlekek_%02x", gen);
        uint8_t kek[16];
        if (!keys_get(c->keys, name, kek, 16))
            return fail(c, "prod.keys has no %s, which this dump needs. Use a newer keys file from your console.", name);
        aes128_ecb_decrypt(kek, t->title_key, key, 16);
        memset(kek, 0, sizeof(kek));
        return true;
    }
    static const char *AREAS[] = {"application", "ocean", "system"};
    if (header[0x207] > 2) return fail(c, "%s uses a key area this tool does not know.", e->name);
    snprintf(name, sizeof(name), "key_area_key_%s_%02x", AREAS[header[0x207]], gen);
    uint8_t kak[16], area[0x40];
    if (!keys_get(c->keys, name, kak, 16))
        return fail(c, "prod.keys has no %s, which this dump needs. Use a newer keys file from your console.", name);
    aes128_ecb_decrypt(kak, header + 0x300, area, sizeof(area));
    memcpy(key, area + 0x20, 16);
    memset(kak, 0, sizeof(kak));
    memset(area, 0, sizeof(area));
    return true;
}

typedef struct {
    uint64_t start, end;       // within the NCA
    const uint8_t *fs;         // its 0x200-byte header
    uint8_t upper[8];          // the counter's upper half
} Section;

static bool find_section(Ctx *c, const Pfs0Entry *e, const uint8_t *header, int fs_type, Section *s) {
    for (int i = 0; i < 4; i++) {
        uint64_t start = (uint64_t)rd32(header + 0x240 + 16 * i) * 0x200, end = (uint64_t)rd32(header + 0x244 + 16 * i) * 0x200;
        const uint8_t *fs = header + 0x400 + 0x200 * i;
        if (!(start < end) || !start || fs[2] != fs_type) continue;
        if (end > e->size) return fail(c, "%s: a section lies outside the file; the dump is damaged.", e->name);
        uint8_t digest[32];
        sha256(fs, 0x200, digest);
        if (memcmp(digest, header + 0x280 + 0x20 * i, 32)) return fail(c, "%s: a section header is damaged.", e->name);
        if (fs[4] != 3) return fail(c, "%s: a section uses an encryption this tool does not handle.", e->name);
        s->start = start;
        s->end = end;
        s->fs = fs;
        for (int k = 0; k < 8; k++) s->upper[k] = fs[0x147 - k];  // little-endian u64 -> big-endian bytes
        return true;
    }
    return fail(c, "%s has no section of the expected kind.", e->name);
}

// Reads and decrypts size bytes at off within a section (off relative to the section start).
static bool section_read(Ctx *c, const Pfs0Entry *e, const Section *s, const Aes128 *aes, uint64_t off, uint8_t *out,
                         size_t size) {
    uint64_t abs = s->start + off;
    if (off + size > s->end - s->start || !read_at(c, e->offset + abs, out, size)) return false;
    uint8_t counter[16];
    memcpy(counter, s->upper, 8);
    uint64_t block = abs >> 4;
    for (int k = 0; k < 8; k++) counter[15 - k] = (uint8_t)(block >> (8 * k));
    aes128_ctr(aes, counter, (size_t)(abs & 15), out, out, size);
    return true;
}

// Checks data against a table of SHA-256 hashes, one per block (the last block may be hashed
// zero-padded or as it is).
static bool blocks_ok(const uint8_t *table, size_t table_size, const uint8_t *data, size_t size, size_t block) {
    uint8_t digest[32];
    for (size_t i = 0, at = 0; at < size; i++, at += block) {
        if ((i + 1) * 32 > table_size) return false;
        size_t n = size - at < block ? size - at : block;
        sha256(data + at, n, digest);
        if (!memcmp(digest, table + 32 * i, 32)) continue;
        if (n == block) return false;
        Sha256 ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, data + at, n);
        uint8_t zero[256] = {0};
        for (size_t left = block - n; left;) {
            size_t k = left < sizeof(zero) ? left : sizeof(zero);
            sha256_update(&ctx, zero, k);
            left -= k;
        }
        sha256_final(&ctx, digest);
        if (memcmp(digest, table + 32 * i, 32)) return false;
    }
    return true;
}

static int find_entry(Ctx *c, const char *name) {
    for (int i = 0; i < c->count; i++)
        if (!strcasecmp(c->entries[i].name, name)) return i;
    return -1;
}

static bool read_meta(Ctx *c, DlcPackage *out, char data_name[40]) {
    for (int i = 0; i < c->count; i++) {
        const Pfs0Entry *e = &c->entries[i];
        if (!ends_with(e->name, ".nca")) continue;
        uint8_t header[0xc00];
        if (!nca_header(c, e, header)) return false;
        if (header[0x205] != 1) continue;  // not the Meta NCA
        out->is_signed = nca_header_signed(header);
        uint8_t key[16];
        Section s;
        if (!section_key(c, e, header, key) || !find_section(c, e, header, 1, &s)) return false;
        Aes128 aes;
        aes128_init(&aes, key);
        memset(key, 0, sizeof(key));
        uint64_t pfs_off = rd64(s.fs + 0x40), pfs_size = rd64(s.fs + 0x48);
        if (pfs_size < 0x10 || pfs_size > 1024 * 1024) return fail(c, "%s: unexpected content list size.", e->name);
        uint8_t *pfs = malloc((size_t)pfs_size);
        if (!pfs || !section_read(c, e, &s, &aes, pfs_off, pfs, (size_t)pfs_size) || memcmp(pfs, "PFS0", 4)) {
            free(pfs);
            return fail(c, "%s does not decrypt to a content list: the keys do not fit this dump.", e->name);
        }
        Pfs0Entry inner[8];
        int n = pfs0_list(pfs, (size_t)pfs_size, inner, 8);
        const uint8_t *cnmt = NULL;
        size_t cnmt_size = 0;
        for (int k = 0; k < n; k++)
            if (ends_with(inner[k].name, ".cnmt") && inner[k].offset + inner[k].size <= pfs_size) {
                cnmt = pfs + inner[k].offset;
                cnmt_size = (size_t)inner[k].size;
            }
        bool ok = cnmt && cnmt_size >= 0x20;
        if (ok) {
            uint8_t type = cnmt[0xc];
            uint16_t extended = (uint16_t)(cnmt[0xe] | cnmt[0xf] << 8), records = (uint16_t)(cnmt[0x10] | cnmt[0x11] << 8);
            out->title_id = rd64(cnmt);
            out->version = rd32(cnmt + 8);
            if (type != 0x82) {
                free(pfs);
                return fail(c, "This dump is not add-on content (DLC): it is %s.",
                            type == 0x80 ? "a base game" : type == 0x81 ? "an update" : "something else");
            }
            if (extended >= 0x10 && 0x20u + extended <= cnmt_size) {
                out->application_id = rd64(cnmt + 0x20);
                out->required_version = rd32(cnmt + 0x28);
            }
            data_name[0] = 0;
            for (unsigned r = 0; r < records; r++) {
                size_t at = 0x20 + extended + 0x38 * r;
                if (at + 0x38 > cnmt_size) break;
                if (cnmt[at + 0x36] != 2) continue;  // data
                if (data_name[0]) { ok = false; break; }
                for (int b = 0; b < 16; b++) snprintf(data_name + 2 * b, 3, "%02x", cnmt[at + 0x20 + b]);
                strcat(data_name, ".nca");
            }
            ok = ok && data_name[0] && out->application_id;
        }
        free(pfs);
        if (!ok) return fail(c, "The content list in %s is not the usual one for add-on content (one data file).", e->name);
        return true;
    }
    return fail(c, "The dump has no content list (Meta NCA); it is incomplete.", NULL);
}

static bool read_image(Ctx *c, const char *data_name, DlcPackage *out) {
    int i = find_entry(c, data_name);
    if (i < 0) return fail(c, "The dump lacks %s, the add-on content's data; it is incomplete.", data_name);
    const Pfs0Entry *e = &c->entries[i];
    uint8_t header[0xc00], key[16];
    Section s;
    if (!nca_header(c, e, header) || !section_key(c, e, header, key) || !find_section(c, e, header, 0, &s)) return false;
    out->is_signed = out->is_signed && nca_header_signed(header);
    out->key_generation = key_generation(header) + 1;
    Aes128 aes;
    aes128_init(&aes, key);
    memset(key, 0, sizeof(key));
    const uint8_t *fs = s.fs;
    if (memcmp(fs + 8, "IVFC", 4)) return fail(c, "%s: the data has no hash tree.", e->name);
    uint32_t layers = rd32(fs + 0x14), master_size = rd32(fs + 0x10);
    if (layers < 2 || layers > 7 || master_size > 0x20 * 4) return fail(c, "%s: unexpected hash tree.", e->name);
    int levels = (int)layers - 1;
    uint64_t off[6], size[6];
    uint32_t log2[6];
    for (int l = 0; l < levels; l++) {
        off[l] = rd64(fs + 0x18 + 0x18 * l);
        size[l] = rd64(fs + 0x20 + 0x18 * l);
        log2[l] = rd32(fs + 0x28 + 0x18 * l);
        if (log2[l] < 9 || log2[l] > 24 || size[l] > DLCPKG_MAX_IMAGE) {
            snprintf(c->err, c->errn, "%s holds more than %u MiB of data. SeedSwap builds in small (unlock-style) add-on "
                     "content so far; use prepare_game.py on the PC for this one.", e->name, DLCPKG_MAX_IMAGE >> 20);
            return false;
        }
    }
    uint8_t *data[6] = {0};
    bool ok = true;
    for (int l = 0; ok && l < levels; l++) {
        data[l] = malloc(size[l] ? (size_t)size[l] : 1);
        ok = data[l] && section_read(c, e, &s, &aes, off[l], data[l], (size_t)size[l]);
    }
    if (!ok) {
        for (int l = 0; l < levels; l++) free(data[l]);
        return fail(c, "%s could not be read; the dump is cut short or damaged.", e->name);
    }
    uint8_t digest[32];
    sha256(data[0], (size_t)size[0], digest);
    ok = !memcmp(digest, fs + 0xc8, 32) || blocks_ok(fs + 0xc8, master_size, data[0], (size_t)size[0], (size_t)1 << log2[0]);
    for (int l = 1; ok && l < levels; l++)
        ok = blocks_ok(data[l - 1], (size_t)size[l - 1], data[l], (size_t)size[l], (size_t)1 << log2[l]);
    for (int l = 0; l < levels - 1; l++) free(data[l]);
    if (!ok) {
        free(data[levels - 1]);
        return fail(c, "%s fails its own checksums: the dump is damaged, or its ticket and the keys file do not "
                       "match. Dump or download it again.", e->name);
    }
    out->image = data[levels - 1];
    out->image_size = (size_t)size[levels - 1];
    snprintf(out->data_nca, sizeof(out->data_nca), "%s", data_name);
    if (out->image_size < 0x50 || rd64(out->image) != 0x50) {
        dlcpkg_free(out);
        return fail(c, "%s does not decrypt to a RomFS image.", e->name);
    }
    return true;
}

bool dlcpkg_read(DlcReadFn read, void *user, uint64_t size, const char *keys_text, DlcPackage *out, char *err,
                 size_t errn) {
    memset(out, 0, sizeof(*out));
    Ctx *c = calloc(1, sizeof(Ctx));
    if (!c) { snprintf(err, errn, "Not enough memory."); return false; }
    c->read = read;
    c->user = user;
    c->size = size;
    c->keys = keys_text ? keys_text : "";
    c->err = err;
    c->errn = errn;
    bool ok = false;
    uint8_t *head = malloc(0x8000);
    size_t want = size < 0x8000 ? (size_t)size : 0x8000, needed = 0;
    if (!head || !read_at(c, 0, head, want) || memcmp(head, "PFS0", 4)) {
        fail(c, "This file is not an .nsp (PFS0) package.", NULL);
        goto done;
    }
    c->count = pfs0_parse(head, want, size, c->entries, MAX_ENTRIES, &needed);
    if (c->count < 0) {
        fail(c, "The package's file table cannot be read; the file is damaged or cut short.", NULL);
        goto done;
    }
    for (int i = 0; i < c->count; i++)
        if (c->entries[i].offset + c->entries[i].size > size) {
            fail(c, "The file is cut short: %s does not fit in it.", c->entries[i].name);
            goto done;
        }
    if (!keys_get(c->keys, "header_key", c->header_key, 32)) {
        fail(c, "prod.keys has no header_key, or it could not be read.", NULL);
        goto done;
    }
    for (int i = 0; i < c->count; i++) {
        if (!ends_with(c->entries[i].name, ".tik")) continue;
        uint8_t t[0x400];
        size_t n = c->entries[i].size < sizeof(t) ? (size_t)c->entries[i].size : sizeof(t);
        if (read_at(c, c->entries[i].offset, t, n)) parse_ticket(c, t, n);
    }
    char data_name[40];
    ok = read_meta(c, out, data_name) && read_image(c, data_name, out);
done:
    memset(c->header_key, 0, sizeof(c->header_key));
    free(c);
    free(head);
    return ok;
}

void dlcpkg_free(DlcPackage *p) {
    free(p->image);
    p->image = NULL;
    p->image_size = 0;
}
