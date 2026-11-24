// SPDX-License-Identifier: GPL-2.0-only
#include "fixes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// ---- symbols, relocations and PLT stubs of a module (64-bit and 32-bit)

typedef struct {
    const NsoImage *nso;
    bool wide;
    uint64_t strtab, symtab, strsz;
    uint32_t count;
    uint64_t rela, rela_size, jmprel, jmprel_size;
} Mod;

static bool mod_open(const NsoImage *nso, Mod *m) {
    memset(m, 0, sizeof(*m));
    m->nso = nso;
    m->wide = !nso_is32(nso);
    const uint8_t *img = nso->image;
    uint64_t size = nso->image_size;
    if (size < 8) return false;
    uint32_t mod0 = rd32(img + 4);
    if ((uint64_t)mod0 + 8 > size || memcmp(img + mod0, "MOD0", 4)) return false;
    uint64_t dyn = (uint64_t)mod0 + (uint64_t)(int64_t)(int32_t)rd32(img + mod0 + 4);
    uint64_t hash = 0;
    size_t step = m->wide ? 16 : 8;
    for (int i = 0; i < 128 && dyn + step * (uint64_t)(i + 1) <= size; i++) {
        const uint8_t *e = img + dyn + step * i;
        uint64_t tag = m->wide ? rd64(e) : rd32(e), val = m->wide ? rd64(e + 8) : rd32(e + 4);
        if (!tag) break;
        switch (tag) {
        case 5: m->strtab = val; break;
        case 6: m->symtab = val; break;
        case 10: m->strsz = val; break;
        case 4: hash = val; break;
        case 7: m->rela = val; break;
        case 8: m->rela_size = val; break;
        case 23: m->jmprel = val; break;
        case 2: m->jmprel_size = val; break;
        }
    }
    if (!m->strtab || !m->symtab || m->strtab >= size || m->symtab >= size) return false;
    size_t entry = m->wide ? 24 : 16;
    if (hash && hash + 8 <= size) m->count = rd32(img + hash + 4);
    else if (m->strtab > m->symtab) m->count = (uint32_t)((m->strtab - m->symtab) / entry);
    return (uint64_t)m->count * entry + m->symtab <= size;
}

static const char *sym_name(const Mod *m, uint32_t i) {
    size_t entry = m->wide ? 24 : 16;
    uint64_t off = m->strtab + rd32(m->nso->image + m->symtab + entry * i);
    if (off >= m->nso->image_size) return "";
    const char *p = (const char *)m->nso->image + off;
    return memchr(p, 0, m->nso->image_size - off) ? p : "";
}

static uint64_t sym_value(const Mod *m, uint32_t i) {
    const uint8_t *s = m->nso->image + m->symtab + (m->wide ? 24 : 16) * (uint64_t)i;
    return m->wide ? rd64(s + 8) : rd32(s + 4);
}

static long sym_defined(const Mod *m, const char *name) {
    for (uint32_t i = 0; i < m->count; i++)
        if (sym_value(m, i) && !strcmp(sym_name(m, i), name)) return (long)i;
    return -1;
}

long long fixes_defined(const NsoImage *nso, const char *name) {
    Mod m;
    if (!mod_open(nso, &m)) return -1;
    long i = sym_defined(&m, name);
    return i < 0 ? -1 : (long long)sym_value(&m, (uint32_t)i);
}

bool fixes_imports(const NsoImage *nso, const char *name) {
    Mod m;
    if (!mod_open(nso, &m)) return false;
    for (uint32_t i = 0; i < m.count; i++)
        if (!sym_value(&m, i) && !strcmp(sym_name(&m, i), name)) return true;
    return false;
}

typedef struct {
    uint64_t offset, addend;
    uint32_t kind, symbol;
} Reloc;

// RELA first, then JMPREL (the order the PC tools read them). 64-bit modules only. malloc'd.
static Reloc *relocs(const Mod *m, int *count) {
    uint64_t starts[2] = {m->rela, m->jmprel}, sizes[2] = {m->rela_size, m->jmprel_size};
    uint64_t total = (sizes[0] + sizes[1]) / 24 + 1;
    Reloc *out = total < 4000000 ? malloc(sizeof(Reloc) * (size_t)total) : NULL;
    int n = 0;
    for (int t = 0; out && t < 2; t++)
        for (uint64_t at = starts[t]; sizes[t] && at + 24 <= starts[t] + sizes[t] && at + 24 <= m->nso->image_size &&
             (uint64_t)n < total; at += 24) {
            const uint8_t *r = m->nso->image + at;
            uint64_t info = rd64(r + 8);
            out[n++] = (Reloc){rd64(r), rd64(r + 16), (uint32_t)info, (uint32_t)(info >> 32)};
        }
    *count = n;
    return out;
}

// PLT stub of an imported symbol name (64-bit): adrp x16 ; ldr x17, [x16, #off] ; add x16 ; br x17.
// With prefix, the first import whose name starts with `name` that has a stub is also offered by calls.
static long plt64(const Mod *m, uint64_t slot) {
    const NsoImage *nso = m->nso;
    for (uint32_t at = 0; at + 16 <= nso->size[0]; at += 4) {
        uint32_t w0 = rd32(nso->image + at), w1 = rd32(nso->image + at + 4), w3 = rd32(nso->image + at + 12);
        if ((w0 & 0x9F00001Fu) != 0x90000010u || (w1 & 0xFFC003FFu) != 0xF9400211u || w3 != 0xD61F0220u) continue;
        int64_t page = (int64_t)((((w0 >> 5) & 0x7FFFF) << 2) | ((w0 >> 29) & 3));
        if (page & (1 << 20)) page -= 1 << 21;
        if ((uint64_t)((int64_t)(at & ~0xFFFu) + page * 4096) + ((w1 >> 10) & 0xFFF) * 8 == slot) return (long)at;
    }
    return -1;
}

static uint32_t arm_imm(uint32_t w) {
    uint32_t imm = w & 0xFF, rot = ((w >> 8) & 0xF) * 2;
    return rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
}

static long plt32(const Mod *m, uint32_t slot) {
    const NsoImage *nso = m->nso;
    for (uint32_t at = 0; at + 12 <= nso->size[0]; at += 4) {
        uint32_t w0 = rd32(nso->image + at), w1 = rd32(nso->image + at + 4), w2 = rd32(nso->image + at + 8);
        if ((w0 & 0xFFFFF000u) == 0xE28FC000u && (w1 & 0xFFFFF000u) == 0xE28CC000u && (w2 & 0xFFFFF000u) == 0xE5BCF000u &&
            ((at + 8 + arm_imm(w0) + arm_imm(w1) + (w2 & 0xFFF)) & 0xFFFFFFFFu) == slot)
            return (long)at;
    }
    return -1;
}

// Stubs of every import whose name equals `name` (or starts with it when prefix): offsets into out.
static int stubs_of(const Mod *m, const char *name, bool prefix, long *out, int max) {
    int n = 0;
    size_t len = strlen(name);
    if (m->wide) {
        int nr = 0;
        Reloc *r = relocs(m, &nr);
        for (int i = 0; i < nr && n < max; i++) {
            if (!r[i].symbol || r[i].symbol >= m->count || sym_value(m, r[i].symbol)) continue;
            const char *s = sym_name(m, r[i].symbol);
            if (prefix ? strncmp(s, name, len) : strcmp(s, name)) continue;
            long stub = plt64(m, r[i].offset);
            if (stub >= 0) out[n++] = stub;
        }
        free(r);
    } else {
        for (uint64_t at = m->jmprel; m->jmprel_size && at + 8 <= m->jmprel + m->jmprel_size && n < max; at += 8) {
            uint32_t off = rd32(m->nso->image + at), sym = rd32(m->nso->image + at + 4) >> 8;
            if (!sym || sym >= m->count) continue;
            const char *s = sym_name(m, sym);
            if (prefix ? strncmp(s, name, len) : strcmp(s, name)) continue;
            long stub = plt32(m, off);
            if (stub >= 0) out[n++] = stub;
        }
    }
    return n;
}

static int64_t branch_target64(uint32_t word, uint32_t at) {
    if ((word & 0x7C000000u) != 0x14000000u) return -1;
    int64_t off = word & 0x3FFFFFF;
    if (off & 0x2000000) off -= 1 << 26;
    return (int64_t)at + off * 4;
}

static bool is_bl64(uint32_t w) { return w >> 26 == 0x25; }

static int calls64(const NsoImage *nso, long target, uint32_t from, uint32_t to, uint32_t *out, int max) {
    int n = 0;
    for (uint32_t at = from; at + 4 <= to && at + 4 <= nso->size[0]; at += 4) {
        uint32_t w = rd32(nso->image + at);
        if (is_bl64(w) && branch_target64(w, at) == target && n < max) out[n++] = at;
    }
    return n;
}

static int calls32(const NsoImage *nso, long target, uint32_t *out, int max) {
    int n = 0;
    for (uint32_t at = 0; at + 4 < nso->size[0]; at += 4) {
        uint32_t w = rd32(nso->image + at);
        if (w >> 24 != 0xEB) continue;
        int32_t imm = (int32_t)(w & 0xFFFFFF);
        if (imm & 0x800000) imm -= 1 << 24;
        if ((int64_t)at + 8 + (int64_t)imm * 4 == target && n < max) out[n++] = at;
    }
    return n;
}

static uint32_t branch64(uint32_t source, uint32_t target, bool link) {
    int64_t off = (int64_t)target - (int64_t)source;
    return (link ? 0x94000000u : 0x14000000u) | (uint32_t)((off >> 2) & 0x3FFFFFF);
}

static uint64_t adrp_target(uint32_t word, uint32_t at) {
    int64_t pages = (int64_t)((((word >> 5) & 0x7FFFF) << 2) | ((word >> 29) & 3));
    if (pages & (1 << 20)) pages -= 1 << 21;
    return (uint64_t)((int64_t)(at & ~0xFFFu) + pages * 4096);
}

static uint32_t set_adrp(uint32_t word, uint32_t at, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(at >> 12);
    uint32_t p = (uint32_t)pages & ((1u << 21) - 1);
    return (word & 0x9F00001Fu) | (p & 3) << 29 | (p >> 2) << 5;
}

static uint32_t set_imm12(uint32_t word, uint32_t value) { return (word & ~(0xFFFu << 10)) | (value & 0xFFF) << 10; }
static uint32_t set_imm16(uint32_t word, uint32_t value) { return (word & ~(0xFFFFu << 5)) | (value & 0xFFFF) << 5; }

static bool cstring(const NsoImage *nso, uint64_t address, char *out, size_t n) {
    if (address >= nso->image_size) return false;
    const uint8_t *p = nso->image + address;
    const uint8_t *end = memchr(p, 0, nso->image_size - address);
    if (!end || (size_t)(end - p) > 64 || (size_t)(end - p) >= n) return false;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = 0;
    return true;
}

typedef struct {
    uint32_t adrp, add, reg;
    uint64_t address;
    char text[72];
} StringArg;

// a64.string_argument: adrp xR + add xR, xR, #lo before the call, xR being x0 or moved into it.
static bool string_arg64(const NsoImage *nso, uint32_t call, StringArg *out) {
    const int reach = 10;
    if (call < 4 * reach) return false;
    unsigned moved = 1;
    for (uint32_t at = call - 4; at + 4 * reach >= call; at -= 4) {
        uint32_t w = rd32(nso->image + at);
        if ((w & 0xFFE0FFFFu) == 0xAA0003E0u) moved |= 1u << ((w >> 16) & 31);
        unsigned rd = w & 31;
        if ((w & 0xFFC00000u) == 0x91000000u && rd == ((w >> 5) & 31) && (moved >> rd & 1)) {
            for (uint32_t back = at - 4; back + 4 * (reach + 1) > at && back < at; back -= 4) {
                uint32_t b = rd32(nso->image + back);
                if ((b & 0x9F000000u) == 0x90000000u && (b & 31) == rd) {
                    out->adrp = back;
                    out->add = at;
                    out->reg = rd;
                    out->address = adrp_target(b, back) + ((w >> 10) & 0xFFF);
                    return cstring(nso, out->address, out->text, sizeof(out->text));
                }
                if (back < 4) break;
            }
            return false;
        }
        if (at < 4) break;
    }
    return false;
}

typedef struct {
    uint32_t literal, value, address;
    char text[72];
} StringArg32;

// mk8_devicesave.string_argument32: `ldr r0, [pc, #x]` and, within three instructions after it,
// `add rN, pc, r0`, in the eight instructions before the call.
static bool string_arg32(const NsoImage *nso, uint32_t call, StringArg32 *out) {
    const int reach = 8;
    if (call < 4 * reach) return false;
    for (int i = reach - 1; i >= 0; i--) {
        uint32_t at = call - 4 * reach + 4 * (uint32_t)i, w = rd32(nso->image + at);
        if ((w & 0xFFFF0FFFu) != 0xE08F0000u) continue;  // add rN, pc, r0
        for (int j = i - 1; j >= 0 && j >= i - 3; j--) {
            uint32_t lat = call - 4 * reach + 4 * (uint32_t)j, l = rd32(nso->image + lat);
            if ((l & 0xFF7FF000u) != 0xE51F0000u) continue;  // ldr r0, [pc, #+/-imm]
            int64_t imm = (int64_t)(l & 0xFFF) * ((l >> 23) & 1 ? 1 : -1);
            out->literal = (uint32_t)((int64_t)lat + 8 + imm);
            if ((uint64_t)out->literal + 4 > nso->image_size) return false;
            out->value = rd32(nso->image + out->literal);
            out->address = (at + 8 + out->value) & 0xFFFFFFFFu;
            return cstring(nso, out->address, out->text, sizeof(out->text));
        }
        return false;
    }
    return false;
}

// ---- the fixes

static void hex(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void change(FixRecord *r, const char *module, uint32_t addr, uint32_t old_word, uint32_t new_word,
                   const char *what) {
    if (r->count >= FIX_MAX_CHANGES) return;
    FixChange *c = &r->changes[r->count++];
    snprintf(c->module, sizeof(c->module), "%s", module);
    c->addr = addr;
    c->old_word = old_word;
    c->new_word = new_word;
    snprintf(c->what, sizeof(c->what), "%s", what);
}

static bool not_placed(FixRecord *r, const char *why) {
    r->status = FIX_NOT_PLACED;
    r->count = 0;
    snprintf(r->reason, sizeof(r->reason), "%s", why);
    return false;
}

#define DEVICE "_ZN2nn2fs19MountDeviceSaveDataEPKc"
#define ACCOUNT_PREFIX "_ZN2nn2fs13MountSaveDataEPKc"

static bool mk8_64(NsoImage *main, FixRecord *r) {
    Mod m;
    if (!mod_open(main, &m)) return not_placed(r, "main's symbols cannot be read");
    long dev[4];
    if (stubs_of(&m, DEVICE, false, dev, 4) < 1)
        return not_placed(r, "main does not import nn::fs::MountDeviceSaveData, so there is nothing to redirect");
    uint32_t calls[4];
    if (calls64(main, dev[0], 0, main->size[0], calls, 4) != 1)
        return not_placed(r, "expected one call to MountDeviceSaveData in main");
    uint32_t call = calls[0];
    StringArg device, account;
    if (!string_arg64(main, call, &device))
        return not_placed(r, "the code before the device mount does not form its name the expected way");
    long acc[16];
    int na = stubs_of(&m, ACCOUNT_PREFIX, true, acc, 16);
    uint32_t first = UINT32_MAX;
    uint32_t end = call + 0x100 < main->size[0] ? call + 0x100 : main->size[0];
    for (int i = 0; i < na; i++) {
        uint32_t c[8];
        int n = calls64(main, acc[i], call, end, c, 8);
        for (int k = 0; k < n; k++)
            if (c[k] < first) first = c[k];
    }
    if (first == UINT32_MAX) return not_placed(r, "no MountSaveData call follows the device mount");
    if (!string_arg64(main, first, &account))
        return not_placed(r, "the code before the account mount does not form its name the expected way");
    if (strcmp(device.text, "CommonSave") || strcmp(account.text, "UserSave"))
        return not_placed(r, "the mount names are not CommonSave and UserSave");
    char what[120];
    if (device.address >> 12 != account.address >> 12) {
        uint32_t old = rd32(main->image + device.adrp);
        snprintf(what, sizeof(what), "adrp x%u: the page of \"CommonSave\" -> the page of \"UserSave\"", device.reg);
        change(r, "main", device.adrp, old, set_adrp(old, device.adrp, account.address), what);
    }
    uint32_t old = rd32(main->image + device.add);
    snprintf(what, sizeof(what), "add x%u, x%u, #%#x (\"CommonSave\") -> #%#x (\"UserSave\")", device.reg, device.reg,
             (unsigned)(device.address & 0xFFF), (unsigned)(account.address & 0xFFF));
    change(r, "main", device.add, old, set_imm12(old, (uint32_t)(account.address & 0xFFF)), what);
    change(r, "main", call, rd32(main->image + call), 0x2A1F03E0u, "bl nn::fs::MountDeviceSaveData -> mov w0, wzr");
    return true;
}

static bool mk8_32(NsoImage *main, FixRecord *r) {
    Mod m;
    if (!mod_open(main, &m)) return not_placed(r, "main's symbols cannot be read");
    long dev[4];
    if (stubs_of(&m, DEVICE, false, dev, 4) < 1)
        return not_placed(r, "main does not import nn::fs::MountDeviceSaveData, so there is nothing to redirect");
    uint32_t calls[4];
    if (calls32(main, dev[0], calls, 4) != 1) return not_placed(r, "expected one call to MountDeviceSaveData in main");
    uint32_t call = calls[0];
    StringArg32 device, account;
    if (!string_arg32(main, call, &device))
        return not_placed(r, "the code before the device mount does not form its name the expected way");
    long acc[16];
    int na = stubs_of(&m, ACCOUNT_PREFIX, true, acc, 16);
    uint32_t first = UINT32_MAX;
    for (int i = 0; i < na; i++) {
        uint32_t c[64];
        int n = calls32(main, acc[i], c, 64);
        for (int k = 0; k < n; k++)
            if (c[k] > call && c[k] < call + 0x100 && c[k] < first) first = c[k];
    }
    if (first == UINT32_MAX) return not_placed(r, "no MountSaveData call follows the device mount");
    if (!string_arg32(main, first, &account))
        return not_placed(r, "the code before the account mount does not form its name the expected way");
    if (strcmp(device.text, "CommonSave") || strcmp(account.text, "UserSave"))
        return not_placed(r, "the mount names are not CommonSave and UserSave");
    change(r, "main", call, rd32(main->image + call), 0xE3A00000u, "bl nn::fs::MountDeviceSaveData -> mov r0, #0");
    change(r, "main", device.literal, device.value, device.value + account.address - device.address,
           "the literal that forms the address of \"CommonSave\" now forms \"UserSave\"");
    return true;
}

// ---- Super Mario Odyssey: heap pre-touch (smo_pretouch.py; the code is smo_pretouch.S as words)

#define SET_HEAP "_ZN2nn2os17SetMemoryHeapSizeEm"
#define ALLOCATE "_ZN2nn2os19AllocateMemoryBlockEPmm"
#define OS_STORAGE "_ZN2nn2os6detail26g_OsResourceManagerStorageE"
#define BLOCK 0x2400000u

static const uint32_t SMO_WORDS[] = {
    0xa9be7bfd, 0x910003fd, 0xf9000bf3, 0x528002a1, 0x128fffc2, 0xaa1f03e3, 0xd4000521, 0x350001c0, 0xaa0103f3,
    0x528002c1, 0x128fffc2, 0xaa1f03e3, 0xd4000521, 0x35000100, 0xcb010260, 0xd1600000, 0x926ba800, 0xd2b9000b,
    0xeb0b001f, 0x9a8b9000, 0x14000002, 0x52b90000, 0xf9400bf3, 0xa8c27bfd, 0xd65f03c0, 0xd503201f, 0xd503201f,
    0xd503201f, 0xa9be7bfd, 0x910003fd, 0xf9000bf3, 0xaa0003f3, 0xaa1303e0, 0x97fff719, 0x34000100, 0xd2a48009,
    0xeb09027f, 0x54000083, 0xd2a08009, 0xcb090273, 0x17fffff8, 0xaa1f03f3, 0xd000cdaa, 0x913c014a, 0xf9000153,
    0xf9400bf3, 0xa8c27bfd, 0xd65f03c0, 0xa9be7bfd, 0x910003fd, 0xf9000bf3, 0xaa0003f3, 0xf900027f, 0x97fff709,
    0x35000260, 0xf9400260, 0xb4000220, 0x94000027, 0xb4000100, 0xd29ff70c, 0xf2a0122c, 0xf86c6809, 0x910dc129,
    0xa9400520, 0x94000018, 0x14000004, 0xf9400260, 0xd2a04801, 0x94000014, 0x2a1f03e0, 0xf9400bf3, 0xa8c27bfd,
    0xd65f03c0, 0xaa1f03e9, 0xf900013f, 0xd503201f, 0xd000cda9, 0x913c0129, 0xf9400128, 0xd2b7f80a, 0xb40000c8,
    0xd2a0480b, 0xcb0b0108, 0xeb0a011f, 0x9a8a9108, 0xd65f03c0, 0xaa0a03e8, 0xd65f03c0, 0x8b010001, 0xeb01001f,
    0x540000a2, 0xf9400009, 0xf9000009, 0x91400400, 0x17fffffb, 0xd65f03c0, 0xa9be7bfd, 0x910003fd, 0xb00082e9,
    0xf9403129, 0xd290fd0c, 0xf2a0120c, 0xcb0c012b, 0xf9000beb, 0xf2402d7f, 0x54000221, 0xd292920c, 0xf2a0010c,
    0x8b0c0160, 0xf9000fe0, 0x52800021, 0x94000011, 0x34000140, 0xf9400fe9, 0xb940012d, 0x528f7fae, 0x72b537ee,
    0x6b0e01bf, 0x54000081, 0xf9400be0, 0xa8c27bfd, 0xd65f03c0, 0xaa1f03e0, 0xa8c27bfd, 0xd65f03c0, 0xd503201f,
    0xd503201f, 0xd503201f, 0xd10103ff, 0xb90033e1, 0xaa0003e2, 0x910003e0, 0xd40000c1, 0x35000160, 0xb94013e9,
    0x34000129, 0xb9401be9, 0xb94033ea, 0x0a0a0129, 0x6b0a013f, 0x54000081, 0x52800020, 0x910103ff, 0xd65f03c0,
    0x2a1f03e0, 0x910103ff, 0xd65f03c0};
#define SMO_NWORDS (sizeof(SMO_WORDS) / sizeof(SMO_WORDS[0]))
enum { E_HEAP_SIZE = 0x000, E_SET_HEAP = 0x070, E_AMB_HOOK = 0x0C0, E_ARENA_SIZE = 0x130 };

typedef struct {
    uint32_t size_site, set_heap_site, allocate_site, skip_from, skip_to, arena_site;
    uint32_t heap_max, recording, arena_max;
    long plt_set_heap, plt_allocate;
    uint32_t cave;
    uint64_t slot;
    uint64_t sdk_ref, sdk_set_heap, sdk_os_got, sdk_heap_manager, main_sdk_ref;
    char sdk_ref_symbol[96];
} SmoPlan;

static bool movz_w_shifted(uint32_t w, uint32_t *reg, uint32_t *value) {
    if ((w & 0xFFE00000u) != 0x52A00000u) return false;
    *reg = w & 31;
    *value = ((w >> 5) & 0xFFFF) << 16;
    return true;
}

// The value of a 32-bit logical immediate (N = 0): `imms` gives the element size and the run of ones,
// `immr` the rotation.
static bool decode_bitmask32(uint32_t immr, uint32_t imms, uint32_t *out) {
    uint32_t inverted = ~imms & 0x3F;
    int len = -1;
    for (int b = 5; b >= 0; b--)
        if (inverted >> b & 1) {
            len = b;
            break;
        }
    if (len < 1) return false;
    uint32_t size = 1u << len, levels = size - 1, s = imms & levels, r = immr & levels;
    if (s == levels) return false;
    uint64_t mask = size == 32 ? 0xFFFFFFFFull : ((1ull << size) - 1);
    uint64_t elem = (1ull << (s + 1)) - 1;
    if (r) elem = ((elem >> r) | (elem << (size - r))) & mask;
    uint32_t v = 0;
    for (uint32_t at = 0; at < 32; at += size) v |= (uint32_t)(elem << at);
    *out = v;
    return true;
}

// Whether `target` is the PLT stub of some import (adrp x16 / ldr x17 / add x16 / br x17 whose slot
// a relocation with a symbol fills).
static bool is_import_stub(const Mod *m, uint32_t target) {
    const NsoImage *nso = m->nso;
    if ((uint64_t)target + 16 > nso->size[0]) return false;
    uint32_t w0 = rd32(nso->image + target), w1 = rd32(nso->image + target + 4), w3 = rd32(nso->image + target + 12);
    if ((w0 & 0x9F00001Fu) != 0x90000010u || (w1 & 0xFFC003FFu) != 0xF9400211u || w3 != 0xD61F0220u) return false;
    uint64_t slot = adrp_target(w0, target) + ((w1 >> 10) & 0xFFF) * 8;
    int n = 0;
    Reloc *r = relocs(m, &n);
    bool found = false;
    for (int i = 0; i < n && !found; i++) found = r[i].offset == slot && r[i].symbol;
    free(r);
    return found;
}

// `mov w1, #imm` in any of its encodings (movz, movn, orr with wzr)
static bool mov_w1(uint32_t w, uint32_t *value) {
    if ((w & 0xFF80001Fu) == 0x52800001u) {  // movz w1, #imm16, lsl #hw*16
        *value = ((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3));
        return true;
    }
    if ((w & 0xFF80001Fu) == 0x12800001u) {  // movn
        *value = ~(((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3)));
        return true;
    }
    if ((w & 0xFFC003FFu) == 0x320003E1u)  // orr w1, wzr, #bitmask
        return decode_bitmask32((w >> 16) & 0x3F, (w >> 10) & 0x3F, value);
    return false;
}

static bool smo_sdk(FixModule *mods, int count, NsoImage *main, NsoImage *sdk, SmoPlan *p, FixRecord *r) {
    Mod sm, mm;
    if (!mod_open(sdk, &sm) || !mod_open(main, &mm)) return not_placed(r, "the modules' symbols cannot be read");
    int nrel = 0;
    Reloc *rel = relocs(&mm, &nrel);
    if (!rel) return not_placed(r, "not enough memory");
    bool found = false;
    for (int i = 0; i < nrel && !found; i++) {
        if ((rel[i].kind != 257 && rel[i].kind != 1025) || !rel[i].symbol || rel[i].symbol >= mm.count ||
            sym_value(&mm, rel[i].symbol))
            continue;
        const char *name = sym_name(&mm, rel[i].symbol);
        long def = sym_defined(&sm, name);
        if (def < 0) continue;
        bool elsewhere = false;
        for (int k = 0; k < count && !elsewhere; k++) {
            if (mods[k].nso == sdk) continue;
            Mod o;
            if (mod_open(mods[k].nso, &o) && sym_defined(&o, name) >= 0) elsewhere = true;
        }
        if (elsewhere) continue;
        p->main_sdk_ref = rel[i].offset;
        p->sdk_ref = sym_value(&sm, (uint32_t)def) + rel[i].addend;
        snprintf(p->sdk_ref_symbol, sizeof(p->sdk_ref_symbol), "%s", name);
        found = true;
    }
    if (!found) {
        free(rel);
        return not_placed(r, "main holds no load-time pointer into the sdk");
    }
    if (p->main_sdk_ref & 7) {
        free(rel);
        return not_placed(r, "main's pointer into the sdk is not 8-byte aligned");
    }
    long sh = sym_defined(&sm, SET_HEAP);
    p->sdk_set_heap = sh >= 0 ? sym_value(&sm, (uint32_t)sh) : 0;
    if (sh < 0 || p->sdk_set_heap + 0x20 > sdk->size[0] || rd32(sdk->image + p->sdk_set_heap) != 0xA9BF7BFDu) {
        free(rel);
        return not_placed(r, "the sdk's SetMemoryHeapSize does not start the expected way");
    }
    const uint8_t *code = sdk->image + p->sdk_set_heap;
    uint32_t a = rd32(code + 8), l = rd32(code + 12), add = rd32(code + 20);
    if ((a & 0x9F000000u) != 0x90000000u || (l & 0xFFC00000u) != 0xF9400000u || (add & 0xFFC00000u) != 0x91000000u) {
        free(rel);
        return not_placed(r, "the sdk's SetMemoryHeapSize does not find its heap manager the expected way");
    }
    p->sdk_os_got = adrp_target(a, (uint32_t)p->sdk_set_heap + 8) + ((l >> 10) & 0xFFF) * 8;
    p->sdk_heap_manager = (add >> 10) & 0xFFF;
    free(rel);
    int nsr = 0;
    rel = relocs(&sm, &nsr);
    bool storage = false;
    for (int i = 0; rel && i < nsr; i++)
        if (rel[i].offset == p->sdk_os_got)
            storage = rel[i].kind == 1025 && rel[i].symbol < sm.count && !strcmp(sym_name(&sm, rel[i].symbol), OS_STORAGE);
    free(rel);
    if (!storage) return not_placed(r, "the sdk's SetMemoryHeapSize does not use the OS resource manager storage");
    long al = sym_defined(&sm, ALLOCATE);
    bool same = false;
    if (al >= 0) {
        uint64_t at = sym_value(&sm, (uint32_t)al);
        for (int i = 0; i < 8 && at + 4 * (uint64_t)i + 4 <= sdk->size[0]; i++) {
            uint32_t w = rd32(sdk->image + at + 4 * i);
            if ((w & 0xFFC00000u) == 0x91000000u && ((w >> 10) & 0xFFF) == p->sdk_heap_manager) same = true;
            if ((w & 0xFFC00000u) == 0xF9400000u && ((w >> 10) & 0xFFF) * 8 == p->sdk_heap_manager) same = true;
        }
    }
    if (!same) return not_placed(r, "the sdk's AllocateMemoryBlock does not use the same heap manager");
    if (p->sdk_ref >> 32 || p->sdk_set_heap >> 32 || p->sdk_os_got >> 32 || p->main_sdk_ref >> 32)
        return not_placed(r, "an sdk offset does not fit in 32 bits");
    return true;
}

static bool smo_find(FixModule *mods, int count, NsoImage *main, NsoImage *sdk, SmoPlan *p, FixRecord *r) {
    memset(p, 0, sizeof(*p));
    Mod m;
    if (!mod_open(main, &m)) return not_placed(r, "main's symbols cannot be read");
    long s1[4], s2[4];
    if (stubs_of(&m, SET_HEAP, false, s1, 4) < 1) return not_placed(r, "main does not import nn::os::SetMemoryHeapSize");
    if (stubs_of(&m, ALLOCATE, false, s2, 4) < 1) return not_placed(r, "main does not import nn::os::AllocateMemoryBlock");
    p->plt_set_heap = s1[0];
    p->plt_allocate = s2[0];
    long start_sym = sym_defined(&m, "nninitStartup");
    if (start_sym < 0) return not_placed(r, "main does not define nninitStartup (the game leaves heap set-up to the sdk)");
    uint32_t start = (uint32_t)sym_value(&m, (uint32_t)start_sym), end = 0;
    for (uint32_t at = start; at < start + 0x200 && at + 4 <= main->size[0]; at += 4)
        if (rd32(main->image + at) == 0xD65F03C0u) {
            end = at + 4;
            break;
        }
    if (!end) return not_placed(r, "nninitStartup is longer than expected");
    // every bl in the body, and whether it goes to an import stub
    uint32_t calls[64], targets[64];
    int nc = 0;
    for (uint32_t at = start; at < end; at += 4) {
        uint32_t w = rd32(main->image + at);
        if (is_bl64(w) && nc < 64) {
            calls[nc] = at;
            targets[nc++] = (uint32_t)branch_target64(w, at);
        }
    }
    int heap_calls = 0;
    for (int i = 0; i < nc; i++)
        if (targets[i] == p->plt_set_heap) {
            heap_calls++;
            p->set_heap_site = calls[i];
        }
    if (heap_calls != 1) return not_placed(r, "nninitStartup should call SetMemoryHeapSize once");
    p->size_site = p->set_heap_site - 4;
    uint32_t reg, value;
    if (!movz_w_shifted(rd32(main->image + p->size_site), &reg, &value) || reg != 0)
        return not_placed(r, "the heap size is not a constant loaded right before SetMemoryHeapSize");
    p->heap_max = value;
    bool saves = false;
    for (uint32_t at = start; at < p->size_site; at += 4) saves |= (rd32(main->image + at) & 0xFFC07FFFu) == 0xA9007BFDu;
    if (!saves) return not_placed(r, "nninitStartup does not save its return address before setting the heap");
    uint32_t allocs[4];
    int na = 0;
    for (int i = 0; i < nc; i++)
        if (targets[i] == p->plt_allocate && calls[i] > p->set_heap_site && na < 4) allocs[na++] = calls[i];
    if (!na) return not_placed(r, "nninitStartup does not call AllocateMemoryBlock after setting the heap");
    p->allocate_site = allocs[0];
    int blocks = 0;
    bool block_ok = true;
    for (uint32_t at = p->set_heap_site + 4; at < p->allocate_site; at += 4)
        if (movz_w_shifted(rd32(main->image + at), &reg, &value)) {
            blocks++;
            block_ok &= value == BLOCK;
        }
    if (blocks != 1 || !block_ok) return not_placed(r, "the block nninitStartup gives to malloc is not the expected 36 MiB");
    if (na > 2) return not_placed(r, "nninitStartup allocates more blocks than expected");
    if (na > 1) {
        uint32_t second = allocs[1], before = 0;
        int after = -1;
        for (int i = 0; i < nc; i++) {
            if (calls[i] < second) before = calls[i];
            if (calls[i] > second && after < 0) after = i;
        }
        if (after < 0) return not_placed(r, "the code after the second AllocateMemoryBlock is not the expected recording set-up");
        if (!is_import_stub(&m, targets[after]))
            return not_placed(r, "the code after the second AllocateMemoryBlock is not the expected recording set-up");
        p->skip_from = before + 4;
        p->skip_to = calls[after] + 4;
        int inside = 0;
        for (int i = 0; i < nc; i++) inside += calls[i] >= p->skip_from && calls[i] < p->skip_to;
        if (!(p->skip_from <= second && second < p->skip_to) || inside != 2)
            return not_placed(r, "the gameplay-recording set-up in nninitStartup is not the expected two calls");
        bool got = false;
        for (uint32_t at = p->skip_from; at < p->skip_to && !got; at += 4) got = mov_w1(rd32(main->image + at), &p->recording);
        if (!got) return not_placed(r, "the size of the gameplay-recording block could not be read");
    }
    int64_t arena = (int64_t)p->heap_max - BLOCK - p->recording;
    if (arena <= 0 || arena & 0xFFFF) return not_placed(r, "the arena size cannot be derived from the heap size");
    p->arena_max = (uint32_t)arena;
    uint32_t wanted = 0x52A00008u | (((uint32_t)arena >> 16) & 0xFFFF) << 5, sites = 0;
    for (uint32_t at = 4; at + 12 < main->size[0]; at += 4) {
        if (rd32(main->image + at) != wanted) continue;
        bool stored = (rd32(main->image + at + 4) & 0xFFC003FFu) == 0xF90003E8u ||
                      (rd32(main->image + at + 8) & 0xFFC003FFu) == 0xF90003E8u;
        if (stored && is_bl64(rd32(main->image + at - 4))) {
            sites++;
            p->arena_site = at;
        }
    }
    if (sites != 1) return not_placed(r, "expected one place that sets the arena size");
    p->cave = (main->size[0] + 15) & ~15u;
    if (p->cave + 4 * SMO_NWORDS > main->mem[1]) return not_placed(r, "main has too few free bytes after its code for the fix");
    uint64_t data_end = (uint64_t)main->mem[2] + main->size[2] + main->bss_size;
    p->slot = data_end - 0x100;
    uint32_t mod0 = rd32(main->image + 4);
    uint64_t bss_end = (uint64_t)mod0 + (uint64_t)(int64_t)(int32_t)rd32(main->image + mod0 + 12);
    if (data_end & 0xFFF || bss_end > p->slot) return not_placed(r, "main has no unused bytes after its bss for the fix's variable");
    return smo_sdk(mods, count, main, sdk, p, r);
}

static bool smo_build(const SmoPlan *p, uint32_t *w, FixRecord *r) {
    memcpy(w, SMO_WORDS, sizeof(SMO_WORDS));
    uint32_t base = p->cave;
#define AT(i) (base + 4u * (uint32_t)(i))
    w[17] = set_imm16(w[17], p->heap_max >> 16);
    w[21] = set_imm16(w[21], p->heap_max >> 16);
    w[33] = branch64(AT(33), (uint32_t)p->plt_set_heap, true);
    w[53] = branch64(AT(53), (uint32_t)p->plt_allocate, true);
    w[42] = set_adrp(w[42], AT(42), p->slot);
    w[43] = set_imm12(w[43], (uint32_t)(p->slot & 0xFFF));
    w[76] = set_adrp(w[76], AT(76), p->slot);
    w[77] = set_imm12(w[77], (uint32_t)(p->slot & 0xFFF));
    w[59] = set_imm16(w[59], (uint32_t)(p->sdk_os_got & 0xFFFF));
    w[60] = set_imm16(w[60], (uint32_t)(p->sdk_os_got >> 16));
    if (p->sdk_heap_manager >= 0x1000) return not_placed(r, "the sdk's heap manager offset does not fit");
    w[62] = set_imm12(w[62], (uint32_t)p->sdk_heap_manager);
    w[79] = set_imm16(w[79], p->arena_max >> 16);
    w[98] = set_adrp(w[98], AT(98), p->main_sdk_ref);
    w[99] = set_imm12(w[99], (uint32_t)((p->main_sdk_ref & 0xFFF) >> 3));
    w[100] = set_imm16(w[100], (uint32_t)(p->sdk_ref & 0xFFFF));
    w[101] = set_imm16(w[101], (uint32_t)(p->sdk_ref >> 16));
    w[106] = set_imm16(w[106], (uint32_t)(p->sdk_set_heap & 0xFFFF));
    w[107] = set_imm16(w[107], (uint32_t)(p->sdk_set_heap >> 16));
    // check_cave: branches stay inside or go to the two imports; adrp only to the two pages; no
    // indirect branch; svc 0x29 and 0x06 only; exactly one deliberate null store
    int null_stores = 0;
    uint64_t end = base + 4 * SMO_NWORDS;
    for (uint32_t i = 0; i < SMO_NWORDS; i++) {
        uint32_t x = w[i], at = AT(i);
        int64_t target = -1;
        if ((x & 0x7C000000u) == 0x14000000u) target = branch_target64(x, at);
        else if ((x & 0xFF000010u) == 0x54000000u || (x & 0x7E000000u) == 0x34000000u) {
            int64_t imm = (x >> 5) & 0x7FFFF;
            if (imm & 0x40000) imm -= 1 << 19;
            target = at + imm * 4;
        } else if ((x & 0x9F000000u) == 0x10000000u) {
            int64_t imm = (int64_t)(((x >> 5) & 0x7FFFF) << 2 | ((x >> 29) & 3));
            if (imm & (1 << 20)) imm -= 1 << 21;
            target = at + imm;
        }
        if (target >= 0 && !(target >= base && (uint64_t)target < end) && target != p->plt_set_heap &&
            target != p->plt_allocate)
            return not_placed(r, "the added code would branch outside itself");
        if ((x & 0x9F000000u) == 0x90000000u) {
            uint64_t page = adrp_target(x, at);
            if (page != (p->slot & ~0xFFFull) && page != (p->main_sdk_ref & ~0xFFFull))
                return not_placed(r, "the added code would address an unexpected page");
        }
        if ((x & 0xFFDFFC1Fu) == 0xD61F0000u) return not_placed(r, "the added code has an indirect branch");
        if ((x & 0xFFE0001Fu) == 0xD4000001u) {
            uint32_t n = (x >> 5) & 0xFFFF;
            if (n != 0x29 && n != 6) return not_placed(r, "the added code would make an unexpected system call");
        }
        if (x == 0xF900013Fu) {
            if (!i || w[i - 1] != 0xAA1F03E9u) return not_placed(r, "unexpected store in the added code");
            null_stores++;
        }
    }
    if (null_stores != 1) return not_placed(r, "the added code should hold exactly one deliberate crash");
    return true;
#undef AT
}

static bool smo_apply(FixModule *mods, int count, NsoImage *main, NsoImage *sdk, FixRecord *r) {
    if (nso_is32(main)) return not_placed(r, "this fix is for the 64-bit game");
    SmoPlan p;
    uint32_t words[SMO_NWORDS];
    if (!smo_find(mods, count, main, sdk, &p, r) || !smo_build(&p, words, r)) return false;
    char what[120];
    uint32_t cave = p.cave;
    snprintf(what, sizeof(what), "mov w0, #%#x (heap size) -> bl heap_size (free memory - 8 MiB, at most that)", p.heap_max);
    change(r, "main", p.size_site, rd32(main->image + p.size_site), branch64(p.size_site, cave + E_HEAP_SIZE, true), what);
    change(r, "main", p.set_heap_site, rd32(main->image + p.set_heap_site), branch64(p.set_heap_site, cave + E_SET_HEAP, true),
           "bl nn::os::SetMemoryHeapSize -> bl set_heap (retries with less)");
    change(r, "main", p.allocate_site, rd32(main->image + p.allocate_site),
           branch64(p.allocate_site, cave + E_AMB_HOOK, true),
           "bl nn::os::AllocateMemoryBlock -> bl amb_hook (then every heap page is written once)");
    if (p.recording) {
        snprintf(what, sizeof(what), "the %u MiB gameplay-recording block is skipped (b %#x)", p.recording >> 20, p.skip_to);
        change(r, "main", p.skip_from, rd32(main->image + p.skip_from), branch64(p.skip_from, p.skip_to, false), what);
    }
    snprintf(what, sizeof(what), "mov w8, #%#x (arena size) -> bl arena_size (heap - 36 MiB, at most that)", p.arena_max);
    change(r, "main", p.arena_site, rd32(main->image + p.arena_site), branch64(p.arena_site, cave + E_ARENA_SIZE, true), what);
    // the code goes after the text, inside its last page; the text grows to hold it
    memset(main->image + main->size[0], 0, cave - main->size[0]);
    for (uint32_t i = 0; i < SMO_NWORDS; i++) wr32(main->image + cave + 4 * i, words[i]);
    main->size[0] = cave + 4 * (uint32_t)SMO_NWORDS;
    r->cave_start = cave;
    r->cave_size = 4 * (uint32_t)SMO_NWORDS;
    snprintf(r->note, sizeof(r->note), "sdk offsets read from this game's sdk: heap manager at [sdk+%#llx]+%#llx, base "
             "found through main+%#llx", (unsigned long long)p.sdk_os_got, (unsigned long long)p.sdk_heap_manager,
             (unsigned long long)p.main_sdk_ref);
    return true;
}

// ---- the table

typedef struct {
    const char *id, *title;
    uint64_t program;
    const char *builds[3][2];  // main build ID (32 hex digits) -> what was seen
} FixDef;

static const FixDef FIXES[] = {
    {"mk8dx-device-save", "Mario Kart 8 Deluxe: device save redirected to the account save", 0x0100152000022000ull,
     {{"b5c39b5b7c62a88ec25fa897858c874e", "v0 (32-bit): runs on the console with this fix (2026-11-20)"},
      {"158c0c7399aabd7bf83795c2ef3484c7", "v1507328 (64-bit): runs on the console with this fix, Grand Prix included "
                                           "(2026-11-21)"}}},
    {"smo-heap-pretouch", "Super Mario Odyssey: heap sized from free memory and written once before the allocator starts",
     0x0100000000010000ull, {{"3ca12dfaaf9c82da064d1698df79cda1", "v0 (1.0.0): runs on the console with this fix (2026-11-21)"}}},
};

int fixes_apply(uint64_t program_id, FixModule *modules, int count, FixRecord *out, int max, unsigned *changed) {
    *changed = 0;
    NsoImage *main = NULL, *sdk = NULL;
    int main_i = -1;
    for (int i = 0; i < count; i++) {
        if (!strcmp(modules[i].name, "main")) {
            main = modules[i].nso;
            main_i = i;
        }
        if (!strcmp(modules[i].name, "sdk")) sdk = modules[i].nso;
    }
    int n = 0;
    for (size_t k = 0; k < sizeof(FIXES) / sizeof(FIXES[0]) && n < max; k++) {
        const FixDef *d = &FIXES[k];
        if (d->program != program_id) continue;
        FixRecord *r = &out[n++];
        memset(r, 0, sizeof(*r));
        snprintf(r->id, sizeof(r->id), "%s", d->id);
        snprintf(r->title, sizeof(r->title), "%s", d->title);
        if (!main) {
            not_placed(r, "the game has no main module");
            continue;
        }
        hex(main->build_id, 16, r->build);
        for (int b = 0; b < 3 && d->builds[b][0]; b++)
            if (!strcmp(d->builds[b][0], r->build)) snprintf(r->build_note, sizeof(r->build_note), "%s", d->builds[b][1]);
        // work on a copy of main's image, so a fix that does not fit leaves nothing behind
        uint8_t *saved = malloc((size_t)main->image_size);
        uint32_t saved_text = main->size[0];
        if (!saved) {
            not_placed(r, "not enough memory");
            continue;
        }
        memcpy(saved, main->image, (size_t)main->image_size);
        bool ok;
        if (!strcmp(d->id, "mk8dx-device-save")) ok = nso_is32(main) ? mk8_32(main, r) : mk8_64(main, r);
        else ok = sdk ? smo_apply(modules, count, main, sdk, r) : not_placed(r, "the game has no sdk module");
        if (ok) {
            for (int c = 0; c < r->count; c++) wr32(main->image + r->changes[c].addr, r->changes[c].new_word);
            r->status = FIX_APPLIED;
            *changed |= 1u << main_i;
        } else {
            memcpy(main->image, saved, (size_t)main->image_size);
            main->size[0] = saved_text;
        }
        free(saved);
    }
    return n;
}
