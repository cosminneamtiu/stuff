// SPDX-License-Identifier: GPL-2.0-only
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat/nsofile.h"
#include "crypto.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcasecmp(s + a - b, suffix);
}

static bool rd(const Pkg *p, uint64_t off, void *buf, size_t n) {
    return off + n <= p->size && off + n >= off && p->read(p->user, off, buf, n) == 0;
}

static void gb(uint64_t n, char *out, size_t sz) {
    if (n >= 100000000ull) snprintf(out, sz, "%.2f GB", (double)n / 1e9);
    else snprintf(out, sz, "%llu bytes", (unsigned long long)n);
}

const char *pkg_kind_name(int kind) {
    static const char *N[] = {".nsp", ".nsz", ".xci", ".xcz"};
    return kind >= 0 && kind < 4 ? N[kind] : "dump";
}

// ---- containers

static bool add_entry(Pkg *p, const char *name, uint64_t offset, uint64_t size) {
    if (p->count >= PKG_MAX_ENTRIES) return false;
    PkgEntry *e = &p->entries[p->count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->offset = offset;
    e->size = size;
    e->ncz = ends_with(name, ".ncz");
    return true;
}

static bool read_pfs0(Pkg *p, char *err, size_t errn) {
    size_t want = p->size < 0x8000 ? (size_t)p->size : 0x8000, needed = 0;
    uint8_t *head = malloc(want);
    if (!head || !rd(p, 0, head, want)) {
        free(head);
        snprintf(err, errn, "The file could not be read.");
        return false;
    }
    Pfs0Entry *list = malloc(sizeof(Pfs0Entry) * PKG_MAX_ENTRIES);
    int n = list ? pfs0_parse(head, want, UINT64_MAX, list, PKG_MAX_ENTRIES, &needed) : -1;
    if (n == -2 && needed <= 4u * 1024 * 1024 && needed <= p->size) {
        free(head);
        head = malloc(needed);
        want = needed;
        n = head && rd(p, 0, head, want) ? pfs0_parse(head, want, UINT64_MAX, list, PKG_MAX_ENTRIES, &needed) : -1;
    }
    free(head);
    if (n <= 0) {
        free(list);
        snprintf(err, errn, "The file's table of contents is damaged, or it holds more than %d files.", PKG_MAX_ENTRIES);
        return false;
    }
    for (int i = 0; i < n; i++) add_entry(p, list[i].name, list[i].offset, list[i].size);
    free(list);
    return true;
}

// HFS0: the partitions of a cartridge image. Returns the number of entries, -1 on damage.
static int read_hfs0(Pkg *p, uint64_t at, char names[][64], uint64_t *offs, uint64_t *sizes, int max) {
    uint8_t h[16];
    if (!rd(p, at, h, 16) || memcmp(h, "HFS0", 4)) return -1;
    uint32_t count = rd32(h + 4), strsize = rd32(h + 8);
    if (count > (uint32_t)max || strsize > 0x100000) return -1;
    size_t table = 0x40 * (size_t)count;
    uint8_t *t = malloc(table + strsize + 1);
    if (!t || !rd(p, at + 16, t, table + strsize)) {
        free(t);
        return -1;
    }
    t[table + strsize] = 0;
    uint64_t data = at + 16 + table + strsize;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = t + 0x40 * i;
        uint32_t name_at = rd32(r + 16);
        if (name_at >= strsize) {
            free(t);
            return -1;
        }
        snprintf(names[i], 64, "%s", (const char *)t + table + name_at);
        offs[i] = data + rd64(r);
        sizes[i] = rd64(r + 8);
    }
    free(t);
    return (int)count;
}

static bool read_xci(Pkg *p, uint64_t base, char *err, size_t errn) {
    uint8_t h[0x200];
    if (!rd(p, base, h, sizeof(h))) return false;
    uint64_t root = base + rd64(h + 0x130);
    static char names[PKG_MAX_ENTRIES][64];
    static uint64_t offs[PKG_MAX_ENTRIES], sizes[PKG_MAX_ENTRIES];
    int n = read_hfs0(p, root, names, offs, sizes, 8);
    int secure = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], "secure")) secure = i;
    if (secure < 0) {
        snprintf(err, errn, "The cartridge image has no readable \"secure\" partition; it is damaged or trimmed too far.");
        return false;
    }
    n = read_hfs0(p, offs[secure], names, offs, sizes, PKG_MAX_ENTRIES);
    if (n <= 0) {
        snprintf(err, errn, "The cartridge image's game partition is damaged.");
        return false;
    }
    for (int i = 0; i < n; i++) add_entry(p, names[i], offs[i], sizes[i]);
    return true;
}

static void parse_ticket(Pkg *p, const uint8_t *data, size_t n) {
    if (p->ticket_count >= PKG_MAX_TICKETS) return;
    PkgTicket *t = &p->tickets[p->ticket_count++];
    memset(t, 0, sizeof(*t));
    uint32_t type = n >= 4 ? rd32(data) : 0;
    size_t sig = type == 0x10000 || type == 0x10003 ? 0x240 : type == 0x10001 || type == 0x10004 ? 0x140
               : type == 0x10002 || type == 0x10005 ? 0x80 : 0;
    if (!sig || n < sig + 0x180) { t->bad = true; return; }
    const uint8_t *body = data + sig;
    memcpy(t->rights_id, body + 0x160, 16);
    memcpy(t->title_key, body + 0x40, 16);
    t->common = body[0x141] == 0;
    t->device_id = rd64(body + 0x158);
}

// ---- NCAs

static int nca_entry_read(void *user, uint64_t off, void *buf, size_t n) {
    Pkg *p = user;
    return rd(p, off, buf, n) ? 0 : -1;
}

static bool header_of(Pkg *p, const PkgEntry *e, uint8_t header[0xc00]) {
    uint8_t raw[0xc00];
    if (e->size < 0xc00 || !rd(p, e->offset, raw, sizeof(raw))) return false;
    aes128_xts_decrypt(p->header_key, 0, raw, header, sizeof(raw));
    return !memcmp(header + 0x200, "NCA3", 4);
}

static int generation(const uint8_t *header) {
    int g = header[0x206] > header[0x220] ? header[0x206] : header[0x220];
    return g > 0 ? g - 1 : 0;
}

static bool section_key(Nca *n) {
    Pkg *p = n->pkg;
    const uint8_t *h = n->header;
    char name[48];
    int gen = generation(h);
    static const uint8_t zero[16];
    if (memcmp(h + 0x230, zero, 16)) {
        const PkgTicket *t = NULL;
        for (int i = 0; i < p->ticket_count; i++)
            if (!p->tickets[i].bad && !memcmp(p->tickets[i].rights_id, h + 0x230, 16)) t = &p->tickets[i];
        if (!t) {
            snprintf(n->err, sizeof(n->err),
                     "The game data (%s) is encrypted with a title key, and the ticket that holds it is not in the "
                     "dump. Dump the game again with its ticket included, or use a cartridge dump.", n->e->name);
            return false;
        }
        if (!t->common) {
            snprintf(n->err, sizeof(n->err),
                     "The dump's ticket is personalised (issued to console %016llx): its title key can only be "
                     "opened on that console. Dump the game again with a common ticket.", (unsigned long long)t->device_id);
            return false;
        }
        snprintf(name, sizeof(name), "titlekek_%02x", gen);
        uint8_t kek[16];
        if (!keys_get(p->keys, name, kek, 16)) {
            snprintf(n->err, sizeof(n->err), "prod.keys has no %s, which this dump needs. Dump the keys again from a "
                     "console on a newer system version.", name);
            return false;
        }
        aes128_ecb_decrypt(kek, t->title_key, n->key, 16);
        memset(kek, 0, sizeof(kek));
        return n->keyed = true;
    }
    static const char *AREAS[] = {"application", "ocean", "system"};
    if (h[0x207] > 2) {
        snprintf(n->err, sizeof(n->err), "%s uses a key area this tool does not know.", n->e->name);
        return false;
    }
    snprintf(name, sizeof(name), "key_area_key_%s_%02x", AREAS[h[0x207]], gen);
    uint8_t kak[16], area[0x40];
    if (!keys_get(p->keys, name, kak, 16)) {
        snprintf(n->err, sizeof(n->err), "prod.keys has no %s, which this dump needs. Dump the keys again from a "
                 "console on a newer system version.", name);
        return false;
    }
    aes128_ecb_decrypt(kak, h + 0x300, area, sizeof(area));
    memcpy(n->key, area + 0x20, 16);
    memset(kak, 0, sizeof(kak));
    memset(area, 0, sizeof(area));
    return n->keyed = true;
}

bool nca_open(Pkg *p, int entry, Nca *n, char *err, size_t errn) {
    memset(n, 0, sizeof(*n));
    if (entry < 0 || entry >= p->count) {
        snprintf(err, errn, "A file the dump's content list names is missing; the dump is incomplete.");
        return false;
    }
    n->pkg = p;
    n->e = &p->entries[entry];
    if (!header_of(p, n->e, n->header)) {
        snprintf(err, errn, "%s does not decrypt with the header_key in prod.keys: the keys file is wrong or the dump "
                 "is damaged.", n->e->name);
        return false;
    }
    n->size = n->e->size;
    if (n->e->ncz) {
        n->ncz = ncz_open(nca_entry_read, p, n->e->offset, n->e->size, err, errn);
        if (!n->ncz) return false;
        n->size = ncz_nca_size(n->ncz);
    }
    if (!section_key(n)) {
        snprintf(err, errn, "%s", n->err);
        nca_close(n);
        return false;
    }
    return true;
}

void nca_close(Nca *n) {
    if (n->ncz) ncz_close(n->ncz);
    memset(n, 0, sizeof(*n));
}

bool nca_section(Nca *n, int fs_type, int encryption, NcaSection *s) {
    for (int i = 0; i < 4; i++) {
        uint64_t start = (uint64_t)rd32(n->header + 0x240 + 16 * i) * 0x200;
        uint64_t end = (uint64_t)rd32(n->header + 0x244 + 16 * i) * 0x200;
        const uint8_t *fs = n->header + 0x400 + 0x200 * i;
        if (!start || start >= end || fs[2] != fs_type || (encryption && fs[4] != encryption)) continue;
        uint8_t digest[32];
        fsha256(fs, 0x200, digest);
        // a sparse section keeps only part of its data, packed: its packed bytes must lie in the file
        bool sparse = !memcmp(fs + 0x158, "BKTR", 4);
        int32_t sparse_count = (int32_t)rd32(fs + 0x160);
        bool inside = sparse ? sparse_count >= 0 && (!sparse_count || rd64(fs + 0x168) + rd64(fs + 0x148) + rd64(fs + 0x150) <= n->size)
                             : end <= n->size;
        if (memcmp(digest, n->header + 0x280 + 0x20 * i, 32) || !inside) {
            snprintf(n->err, sizeof(n->err), "%s: a section header is damaged or lies outside the file.", n->e->name);
            return false;
        }
        s->index = i;
        s->start = start;
        s->end = end;
        s->fs = fs;
        s->fs_type = fs[2];
        s->hash_type = fs[3];
        s->encryption = fs[4];
        s->upper = rd64(fs + 0x140);
        s->sparse = sparse;
        return true;
    }
    snprintf(n->err, sizeof(n->err), "%s has no section of the expected kind.", n->e->name);
    return false;
}

static bool raw_plain_part(Nca *n, uint64_t off, uint8_t *buf, size_t len) {
    // the first 0x4000 bytes of an .ncz are the NCA's own, as stored
    return n->pkg->read(n->pkg->user, n->e->offset + off, buf, len) == 0;
}

bool nca_read_raw(Nca *n, uint64_t off, void *buf, size_t len) {
    uint8_t *out = buf;
    if (off + len > n->size) return false;
    if (!n->ncz) return rd(n->pkg, n->e->offset + off, buf, len);
    if (off < 0x4000) {
        size_t k = off + len > 0x4000 ? (size_t)(0x4000 - off) : len;
        if (!raw_plain_part(n, off, out, k)) return false;
        out += k;
        off += k;
        len -= k;
    }
    while (len) {
        const NczSection *s = ncz_section(n->ncz, off);
        if (!s) return false;
        size_t k = s->offset + s->size - off < len ? (size_t)(s->offset + s->size - off) : len;
        if (!ncz_read(n->ncz, off, out, k)) return false;
        if (s->crypto != 1) {  // encrypted again with the key and counter the .ncz lists
            uint8_t c[16];
            nca_counter(0, off, c);
            memcpy(c, s->counter, 8);
            ctr_crypt(s->key, c, (size_t)(off & 15), out, out, k);
        }
        out += k;
        off += k;
        len -= k;
    }
    return true;
}

bool nca_read_ctr(Nca *n, uint64_t off, void *buf, size_t len, uint64_t upper) {
    uint8_t *out = buf;
    uint8_t ours[16];
    if (off + len > n->size) return false;
    if (!n->ncz) {
        if (!rd(n->pkg, n->e->offset + off, buf, len)) return false;
        nca_counter(upper, off, ours);
        ctr_crypt(n->key, ours, (size_t)(off & 15), out, out, len);
        return true;
    }
    if (off < 0x4000) {
        size_t k = off + len > 0x4000 ? (size_t)(0x4000 - off) : len;
        if (!raw_plain_part(n, off, out, k)) return false;
        nca_counter(upper, off, ours);
        ctr_crypt(n->key, ours, (size_t)(off & 15), out, out, k);
        out += k;
        off += k;
        len -= k;
    }
    while (len) {
        const NczSection *s = ncz_section(n->ncz, off);
        if (!s) return false;
        size_t k = s->offset + s->size - off < len ? (size_t)(s->offset + s->size - off) : len;
        if (!ncz_read(n->ncz, off, out, k)) return false;
        nca_counter(upper, off, ours);
        bool same = s->crypto != 1 && !memcmp(s->key, n->key, 16) && !memcmp(s->counter, ours, 8);
        if (!same) {  // back to the stored bytes, then decrypted the way the caller asked
            if (s->crypto != 1) {
                uint8_t c[16];
                nca_counter(0, off, c);
                memcpy(c, s->counter, 8);
                ctr_crypt(s->key, c, (size_t)(off & 15), out, out, k);
            }
            ctr_crypt(n->key, ours, (size_t)(off & 15), out, out, k);
        }
        out += k;
        off += k;
        len -= k;
    }
    return true;
}

bool nca_read_ctr_at(Nca *n, uint64_t pos, uint64_t ctr_off, void *buf, size_t len, uint64_t upper) {
    if ((pos ^ ctr_off) & 15) return false;
    if (!nca_read_raw(n, pos, buf, len)) return false;   // the stored bytes (an .ncz gives them back as stored too)
    uint8_t c[16];
    nca_counter(upper, ctr_off, c);
    ctr_crypt(n->key, c, (size_t)(ctr_off & 15), buf, buf, len);
    return true;
}

bool nca_section_read(Nca *n, const NcaSection *s, uint64_t off, void *buf, size_t len) {
    if (s->sparse || off + len > s->end - s->start) return false;   // a sparse section is read through its map
    if (s->encryption == 1) return nca_read_raw(n, s->start + off, buf, len);
    return nca_read_ctr(n, s->start + off, buf, len, s->upper);
}

// ---- content lists

static bool parse_cnmt(const uint8_t *c, size_t size, PkgMeta *m) {
    if (size < 0x20) return false;
    memset(m, 0, sizeof(*m));
    m->title_id = rd64(c);
    m->version = rd32(c + 8);
    m->type = c[0xc];
    uint16_t extended = (uint16_t)(c[0xe] | c[0xf] << 8), records = (uint16_t)(c[0x10] | c[0x11] << 8);
    if (extended >= 0x10 && 0x20u + extended <= size) {
        m->other_id = rd64(c + 0x20);
        m->required = rd32(c + 0x28);
    }
    for (unsigned r = 0; r < records && m->count < PKG_MAX_CONTENTS; r++) {
        size_t at = 0x20 + extended + 0x38 * (size_t)r;
        if (at + 0x38 > size) return false;
        PkgContent *x = &m->contents[m->count++];
        memcpy(x->id, c + at + 0x20, 16);
        x->size = rd32(c + at + 0x30) | (uint64_t)(c[at + 0x34] | c[at + 0x35] << 8) << 32;
        x->type = c[at + 0x36];
        x->index = c[at + 0x37];
    }
    return true;
}

static bool read_meta_nca(Pkg *p, int entry, char *err, size_t errn) {
    Nca n;
    if (!nca_open(p, entry, &n, err, errn)) return false;
    NcaSection s;
    bool ok = nca_section(&n, 1, 0, &s);
    uint64_t pfs_off = ok ? rd64(s.fs + 0x40) : 0, pfs_size = ok ? rd64(s.fs + 0x48) : 0;
    uint8_t *pfs = NULL;
    if (ok && (pfs_size < 0x10 || pfs_size > 1024 * 1024)) ok = false;
    if (ok) {
        pfs = malloc((size_t)pfs_size);
        ok = pfs && nca_section_read(&n, &s, pfs_off, pfs, (size_t)pfs_size) && !memcmp(pfs, "PFS0", 4);
    }
    if (ok) {
        Pfs0Entry inner[8];
        int count = pfs0_list(pfs, (size_t)pfs_size, inner, 8);
        ok = false;
        for (int k = 0; k < count; k++)
            if (ends_with(inner[k].name, ".cnmt") && inner[k].offset + inner[k].size <= pfs_size &&
                p->meta_count < PKG_MAX_METAS)
                ok = parse_cnmt(pfs + inner[k].offset, (size_t)inner[k].size, &p->metas[p->meta_count]) &&
                     (p->meta_count++, true);
    }
    free(pfs);
    if (!ok) snprintf(err, errn, "The content list in %s cannot be read: the keys do not fit this dump, or it is "
                      "damaged.", p->entries[entry].name);
    nca_close(&n);
    return ok;
}

Pkg *pkg_open(PkgReadFn read, void *user, uint64_t size, const char *keys_text, char *err, size_t errn) {
    Pkg *p = calloc(1, sizeof(Pkg));
    if (!p) { snprintf(err, errn, "Not enough memory."); return NULL; }
    p->read = read;
    p->user = user;
    p->size = size;
    p->keys = strdup(keys_text ? keys_text : "");
    uint8_t head[0x200];
    if (size < 0x200 || !rd(p, 0, head, sizeof(head))) {
        snprintf(err, errn, "The file is only %llu bytes long; that is not a dump.", (unsigned long long)size);
        goto fail;
    }
    if (!memcmp(head, "PFS0", 4)) {
        if (!read_pfs0(p, err, errn)) goto fail;
        p->kind = PKG_NSP;
    } else {
        uint8_t h2[0x200];
        uint64_t base = !memcmp(head + 0x100, "HEAD", 4) ? 0
                      : rd(p, 0x1000, h2, sizeof(h2)) && !memcmp(h2 + 0x100, "HEAD", 4) ? 0x1000 : UINT64_MAX;
        if (base == UINT64_MAX) {
            if (!memcmp(head, "PK", 2) || !memcmp(head, "Rar!", 4) || !memcmp(head, "7z\xbc\xaf\x27\x1c", 6))
                snprintf(err, errn, "This is an archive (zip, rar or 7z). Unpack it and use the .nsp, .nsz, .xci or "
                         ".xcz inside.");
            else snprintf(err, errn, "This is not a game dump (.nsp, .nsz, .xci or .xcz).");
            goto fail;
        }
        if (!read_xci(p, base, err, errn)) goto fail;
        p->kind = PKG_XCI;
    }
    uint64_t declared = 0;
    for (int i = 0; i < p->count; i++) {
        const PkgEntry *e = &p->entries[i];
        if (e->offset + e->size > declared) declared = e->offset + e->size;
        if (e->ncz) p->kind = p->kind == PKG_XCI || p->kind == PKG_XCZ ? PKG_XCZ : PKG_NSZ;
    }
    if (declared > size) {
        char a[32], b[32];
        gb(declared, a, sizeof(a));
        gb(size, b, sizeof(b));
        if (size < 1024 * 1024)
            snprintf(err, errn, "This is a stub, not a dump: its table lists %d files adding up to %s, but the file is "
                     "%s (the table and nothing else).", p->count, a, b);
        else
            snprintf(err, errn, "The dump is incomplete: its table lists %s of content, the file holds %s. The "
                     "download or copy was probably cut short.", a, b);
        goto fail;
    }
    for (int i = 0; i < p->count; i++) {
        if (!ends_with(p->entries[i].name, ".tik")) continue;
        uint8_t t[0x400];
        size_t n = p->entries[i].size < sizeof(t) ? (size_t)p->entries[i].size : sizeof(t);
        if (rd(p, p->entries[i].offset, t, n)) parse_ticket(p, t, n);
    }
    if (!keys_get(p->keys, "header_key", p->header_key, 32)) {
        snprintf(err, errn, "prod.keys has no header_key, or it could not be read.");
        goto fail;
    }
    int ncas = 0, opened = 0;
    for (int i = 0; i < p->count; i++) {
        PkgEntry *e = &p->entries[i];
        if (!ends_with(e->name, ".nca") && !e->ncz) continue;
        ncas++;
        uint8_t h[0xc00];
        if (!header_of(p, e, h)) continue;
        opened++;
        e->nca = true;
        e->content_type = h[0x205];
        e->program_id = rd64(h + 0x210);
        memcpy(e->rights_id, h + 0x230, 16);
        e->key_generation = generation(h);
    }
    if (!ncas) {
        snprintf(err, errn, "The dump holds no game data (no NCA files).");
        goto fail;
    }
    if (!opened) {
        snprintf(err, errn, "No file of the dump decrypts with the header_key in prod.keys: the keys file is wrong, or "
                 "the dump is damaged.");
        goto fail;
    }
    for (int i = 0; i < p->count; i++)
        if (p->entries[i].nca && p->entries[i].content_type == NCA_META && !read_meta_nca(p, i, err, errn)) goto fail;
    if (!p->meta_count) {
        snprintf(err, errn, "The dump has no content list (Meta NCA); it is incomplete.");
        goto fail;
    }
    return p;
fail:
    pkg_close(p);
    return NULL;
}

void pkg_close(Pkg *p) {
    if (!p) return;
    if (p->keys) {
        memset(p->keys, 0, strlen(p->keys));
        free(p->keys);
    }
    memset(p, 0, sizeof(*p));
    free(p);
}

void pkg_describe(const Pkg *p, char *out, size_t n) {
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < p->meta_count && at + 1 < n; i++) {
        const PkgMeta *m = &p->metas[i];
        int programs = 0;
        for (int k = 0; k < m->count; k++) programs += m->contents[k].type == CONTENT_PROGRAM;
        const char *sep = at ? "; " : "";
        if (m->type == META_APPLICATION)
            at += (size_t)snprintf(out + at, n - at, "%sgame %016llx v%u%s", sep, (unsigned long long)m->title_id,
                                   m->version, programs > 1 ? " (several programs)" : "");
        else if (m->type == META_PATCH)
            at += (size_t)snprintf(out + at, n - at, "%supdate v%u for game %016llx", sep, m->version,
                                   (unsigned long long)m->other_id);
        else if (m->type == META_AOC)
            at += (size_t)snprintf(out + at, n - at, "%sadd-on content %016llx for game %016llx", sep,
                                   (unsigned long long)m->title_id, (unsigned long long)m->other_id);
        else
            at += (size_t)snprintf(out + at, n - at, "%stype %#x %016llx", sep, m->type, (unsigned long long)m->title_id);
    }
}

const PkgMeta *pkg_meta(const Pkg *p, int type, int nth) {
    for (int i = 0; i < p->meta_count; i++)
        if (p->metas[i].type == type && nth-- == 0) return &p->metas[i];
    return NULL;
}

int pkg_content_entry(const Pkg *p, const PkgContent *c) {
    char hex[33];
    for (int b = 0; b < 16; b++) snprintf(hex + 2 * b, 3, "%02x", c->id[b]);
    for (int i = 0; i < p->count; i++)
        if (!strncasecmp(p->entries[i].name, hex, 32) && p->entries[i].name[32] == '.' &&
            (ends_with(p->entries[i].name, ".nca") || p->entries[i].ncz))
            return i;
    return -1;
}

const PkgContent *pkg_meta_content(const PkgMeta *m, int type, int index) {
    for (int i = 0; i < m->count; i++)
        if (m->contents[i].type == type && m->contents[i].index == index) return &m->contents[i];
    return NULL;
}

int pkg_programs(Pkg *base, Pkg *update, PkgProgram *out, int max) {
    const PkgMeta *app = pkg_meta(base, META_APPLICATION, 0);
    if (!app) return 0;
    const PkgMeta *patch = NULL;
    for (int k = 0; update && pkg_meta(update, META_PATCH, k); k++)
        if (pkg_meta(update, META_PATCH, k)->other_id == app->title_id) patch = pkg_meta(update, META_PATCH, k);
    int n = 0;
    for (int index = 0; index < 256 && n < max; index++) {
        const PkgContent *c = pkg_meta_content(app, CONTENT_PROGRAM, index);
        if (!c) continue;
        int e = pkg_content_entry(base, c);
        if (e < 0 || !base->entries[e].nca) continue;
        PkgProgram *x = &out[n++];
        x->index = index;
        x->program_id = base->entries[e].program_id;
        x->base_entry = e;
        x->patch_entry = -1;
        const PkgContent *pc = patch ? pkg_meta_content(patch, CONTENT_PROGRAM, index) : NULL;
        int pe = pc ? pkg_content_entry(update, pc) : -1;
        if (pe >= 0 && update->entries[pe].nca) x->patch_entry = pe;
    }
    return n;
}

// ---- control data

static const char *LANGS[] = {"AmericanEnglish", "BritishEnglish", "Japanese", "French", "German",
                              "LatinAmericanSpanish", "Spanish", "Italian", "Dutch", "CanadianFrench",
                              "Portuguese", "Russian", "Korean", "TraditionalChinese", "SimplifiedChinese",
                              "BrazilianPortuguese"};

// A root file of a small RomFS image in memory.
static const uint8_t *romfs_file(const uint8_t *img, size_t size, const char *name, size_t *len) {
    if (size < 0x50 || rd64(img) != 0x50) return NULL;
    uint64_t fmo = rd64(img + 0x38), fms = rd64(img + 0x40), fdo = rd64(img + 0x48);
    if (fmo + fms > size || fdo > size) return NULL;
    for (uint64_t pos = 0; pos + 0x20 <= fms;) {
        const uint8_t *m = img + fmo + pos;
        uint64_t off = rd64(m + 8), sz = rd64(m + 16);
        uint32_t nlen = rd32(m + 0x1c);
        if (pos + 0x20 + nlen > fms) break;
        if (nlen == strlen(name) && !memcmp(m + 0x20, name, nlen) && fdo + off + sz <= size) {
            *len = (size_t)sz;
            return img + fdo + off;
        }
        pos += 0x20 + ((nlen + 3) & ~3u);
    }
    return NULL;
}

static bool read_control(Pkg *p, int entry, PkgControl *out) {
    char err[200];
    Nca n;
    if (!nca_open(p, entry, &n, err, sizeof(err))) return false;
    NcaSection s;
    bool ok = nca_section(&n, 0, 0, &s) && !memcmp(s.fs + 8, "IVFC", 4);
    uint32_t layers = ok ? rd32(s.fs + 0x14) : 0;
    ok = ok && layers >= 2 && layers <= 7;
    uint64_t off = 0, size = 0;
    if (ok) {
        const uint8_t *l = s.fs + 0x18 + 0x18 * (layers - 2);
        off = rd64(l);
        size = rd64(l + 8);
        ok = size >= 0x50 && size <= 64u * 1024 * 1024 && off + size <= s.end - s.start;
    }
    uint8_t *img = ok ? malloc((size_t)size) : NULL;
    ok = img && nca_section_read(&n, &s, off, img, (size_t)size);
    nca_close(&n);
    size_t nacp_len = 0;
    const uint8_t *nacp = ok ? romfs_file(img, (size_t)size, "control.nacp", &nacp_len) : NULL;
    if (!nacp || nacp_len < 0x4000) {
        free(img);
        return false;
    }
    memset(out, 0, sizeof(*out));
    int lang = -1;
    for (int i = 0; i < 16; i++)
        if (nacp[i * 0x300] && (lang < 0 || i == 0)) lang = i;
    if (lang < 0) {
        free(img);
        return false;
    }
    snprintf(out->name, sizeof(out->name), "%.*s", 0x200, (const char *)nacp + lang * 0x300);
    snprintf(out->publisher, sizeof(out->publisher), "%.*s", 0x100, (const char *)nacp + lang * 0x300 + 0x200);
    snprintf(out->display_version, sizeof(out->display_version), "%.*s", 0x10, (const char *)nacp + 0x3060);
    out->add_on_base_id = rd64(nacp + 0x3070);
    out->save_user = rd64(nacp + 0x3080);
    out->save_user_journal = rd64(nacp + 0x3088);
    out->save_device = rd64(nacp + 0x3090);
    out->save_device_journal = rd64(nacp + 0x3098);
    out->save_user_max = rd64(nacp + 0x3148);
    out->save_user_journal_max = rd64(nacp + 0x3150);
    out->save_device_max = rd64(nacp + 0x3158);
    out->save_device_journal_max = rd64(nacp + 0x3160);
    char icon_name[48];
    snprintf(icon_name, sizeof(icon_name), "icon_%s.dat", LANGS[lang]);
    size_t icon_len = 0;
    const uint8_t *icon = romfs_file(img, (size_t)size, icon_name, &icon_len);
    for (int i = 0; !icon && i < 16; i++) {
        snprintf(icon_name, sizeof(icon_name), "icon_%s.dat", LANGS[i]);
        icon = romfs_file(img, (size_t)size, icon_name, &icon_len);
    }
    if (icon && icon_len > 2 && icon[0] == 0xff && icon[1] == 0xd8 && (out->icon = malloc(icon_len))) {
        memcpy(out->icon, icon, icon_len);
        out->icon_size = icon_len;
    }
    free(img);
    return true;
}

bool pkg_control(Pkg *base, Pkg *update, int index, PkgControl *out) {
    const PkgMeta *app = pkg_meta(base, META_APPLICATION, 0);
    if (!app) return false;
    for (int round = 0; round < 2; round++) {
        Pkg *p = round == 0 ? update : base;
        if (!p) continue;
        for (int k = 0; k < PKG_MAX_METAS; k++) {
            const PkgMeta *m = pkg_meta(p, round == 0 ? META_PATCH : META_APPLICATION, k);
            if (!m) break;
            if (round == 0 && m->other_id != app->title_id) continue;
            for (int pass = 0; pass < 2; pass++) {
                const PkgContent *c = pkg_meta_content(m, CONTENT_CONTROL, pass ? 0 : index);
                int e = c ? pkg_content_entry(p, c) : -1;
                if (e >= 0 && read_control(p, e, out)) return true;
            }
        }
    }
    return false;
}

void pkg_control_free(PkgControl *c) {
    free(c->icon);
    c->icon = NULL;
}
