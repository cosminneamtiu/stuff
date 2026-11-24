// SPDX-License-Identifier: GPL-2.0-only
#include "aoc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/a64.h"

static const char *REPLACED[AOC_TARGETS] = {
    "_ZN2nn3aoc17CountAddOnContentEv",
    "_ZN2nn3aoc16ListAddOnContentEPiii",
    "_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi",
    "_ZN2nn2fs17MountAddOnContentEPKciPvm",
};
static const char *CALLED[AOC_CALLEES] = {
    "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
    "_ZN2nn2fs9CloseFileENS0_10FileHandleE",
    "_ZN2nn2fs14MountRomOnFileEPKcNS0_10FileHandleEPvm",
    "_ZN2nn2fs28QueryMountRomOnFileCacheSizeEPmNS0_10FileHandleE",
};
static const char *MOUNT_ROM = "_ZN2nn2fs8MountRomEPKcPvm";
static const char *NAMES[AOC_TARGETS] = {"CountAddOnContent", "ListAddOnContent", "QueryMountAddOnContentCacheSize",
                                         "MountAddOnContent"};

#define PATH_ROOM 48
#define RET 0xD65F03C0u

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

void aoc_image_name(int index, char out[16]) { snprintf(out, 16, "%04d.romfs", index); }

// ---- the module's dynamic symbols (64-bit modules only)
typedef struct {
    const uint8_t *image;
    uint64_t size;
    uint64_t strtab, symtab, strsz;
    uint32_t count;
    uint64_t rel[2][2];  // JMPREL/PLTRELSZ, RELA/RELASZ
} Syms;

static bool syms_open(const NsoImage *nso, Syms *s) {
    memset(s, 0, sizeof(*s));
    s->image = nso->image;
    s->size = nso->image_size;
    if (nso->image_size < 8) return false;
    uint32_t mod0 = rd32(nso->image + 4);
    if ((uint64_t)mod0 + 8 > nso->image_size || memcmp(nso->image + mod0, "MOD0", 4)) return false;
    uint64_t dyn = (uint64_t)mod0 + (uint64_t)(int64_t)(int32_t)rd32(nso->image + mod0 + 4);
    uint64_t hash = 0;
    bool has_hash = false;
    for (int i = 0; i < 128 && dyn + 16 * (uint64_t)(i + 1) <= s->size; i++) {
        uint64_t tag = rd64(nso->image + dyn + 16 * i), val = rd64(nso->image + dyn + 16 * i + 8);
        if (!tag) break;
        switch (tag) {
        case 5: s->strtab = val; break;
        case 6: s->symtab = val; break;
        case 10: s->strsz = val; break;
        case 4: hash = val; has_hash = true; break;
        case 23: s->rel[0][0] = val; break;
        case 2: s->rel[0][1] = val; break;
        case 7: s->rel[1][0] = val; break;
        case 8: s->rel[1][1] = val; break;
        }
    }
    if (!s->strtab || !s->symtab || s->strtab >= s->size || s->symtab >= s->size) return false;
    if (has_hash && hash + 8 <= s->size) s->count = rd32(nso->image + hash + 4);
    else if (s->strtab > s->symtab) s->count = (uint32_t)((s->strtab - s->symtab) / 24);
    if ((uint64_t)s->count * 24 + s->symtab > s->size) return false;
    return true;
}

static const char *sym_name(const Syms *s, uint32_t i) {
    uint64_t off = s->strtab + rd32(s->image + s->symtab + 24 * (uint64_t)i);
    if (off >= s->size) return "";
    const char *p = (const char *)s->image + off;
    return memchr(p, 0, s->size - off) ? p : "";
}
static uint64_t sym_value(const Syms *s, uint32_t i) { return rd64(s->image + s->symtab + 24 * (uint64_t)i + 8); }
static uint64_t sym_size(const Syms *s, uint32_t i) { return rd64(s->image + s->symtab + 24 * (uint64_t)i + 16); }
static uint16_t sym_shndx(const Syms *s, uint32_t i) {
    const uint8_t *p = s->image + s->symtab + 24 * (uint64_t)i + 6;
    return (uint16_t)(p[0] | p[1] << 8);
}

static long sym_find(const Syms *s, const char *name, bool defined) {
    for (uint32_t i = 0; i < s->count; i++)
        if (!strcmp(sym_name(s, i), name) && (!defined || (sym_value(s, i) && sym_shndx(s, i)))) return (long)i;
    return -1;
}

static bool imports(const Syms *s, const char *name) {
    for (uint32_t i = 0; i < s->count; i++)
        if (!sym_value(s, i) && !strcmp(sym_name(s, i), name)) return true;
    return false;
}

// The PLT stub of an imported symbol: adrp x16 ; ldr x17, [x16, #off] ; add x16, x16, #off ; br x17
static long plt_stub(const NsoImage *nso, const Syms *s, const char *name) {
    long index = sym_find(s, name, false);
    if (index < 0) return -1;
    uint64_t slots[4];
    int nslots = 0;
    for (int r = 0; r < 2; r++)
        for (uint64_t at = s->rel[r][0]; s->rel[r][1] && at + 24 <= s->rel[r][0] + s->rel[r][1] && at + 24 <= s->size; at += 24)
            if ((rd64(s->image + at + 8) >> 32) == (uint64_t)index && nslots < 4) slots[nslots++] = rd64(s->image + at);
    uint32_t text = nso->size[0];
    for (uint32_t at = 0; nslots && at + 16 <= text; at += 4) {
        uint32_t w0 = rd32(nso->image + at), w1 = rd32(nso->image + at + 4), w3 = rd32(nso->image + at + 12);
        if ((w0 & 0x9F00001Fu) != 0x90000010u || (w1 & 0xFFC003FFu) != 0xF9400211u || w3 != 0xD61F0220u) continue;
        int64_t page = (int64_t)((((w0 >> 5) & 0x7FFFF) << 2) | ((w0 >> 29) & 3));
        if (page & (1 << 20)) page -= 1 << 21;
        uint64_t slot = (uint64_t)((int64_t)(at & ~0xFFFu) + page * 4096) + ((w1 >> 10) & 0xFFF) * 8;
        for (int k = 0; k < nslots; k++)
            if (slots[k] == slot) return (long)at;
    }
    return -1;
}

static int64_t branch_target(uint32_t word, uint32_t at) {
    if ((word & 0x7C000000u) != 0x14000000u) return -1;
    int64_t off = word & 0x3FFFFFF;
    if (off & 0x2000000) off -= 1 << 26;
    return (int64_t)at + off * 4;
}

static uint64_t adrp_target(uint32_t word, uint32_t at) {
    int64_t pages = (int64_t)((((word >> 5) & 0x7FFFF) << 2) | ((word >> 29) & 3));
    if (pages & (1 << 20)) pages -= 1 << 21;
    return (uint64_t)((int64_t)(at & ~0xFFFu) + pages * 4096);
}

// The string a call passes in x0, formed by adrp + add (with up to `reach` instructions between).
static bool string_argument(const NsoImage *nso, uint32_t call, char *out, size_t n) {
    const int reach = 10;
    if (call < 4 * reach) return false;
    unsigned moved = 1;  // bit set: registers that end up in x0
    for (uint32_t at = call - 4; at + 4 * reach >= call; at -= 4) {
        uint32_t w = rd32(nso->image + at);
        if ((w & 0xFFE0FFFFu) == 0xAA0003E0u) moved |= 1u << ((w >> 16) & 31);
        unsigned rd = w & 31;
        if ((w & 0xFFC00000u) == 0x91000000u && rd == ((w >> 5) & 31) && (moved >> rd & 1)) {
            for (uint32_t back = at - 4; back + 4 * (reach + 1) > at && back < at; back -= 4) {
                uint32_t b = rd32(nso->image + back);
                if ((b & 0x9F000000u) == 0x90000000u && (b & 31) == rd) {
                    uint64_t address = adrp_target(b, back) + ((w >> 10) & 0xFFF);
                    if (address >= nso->image_size) return false;
                    const uint8_t *p = nso->image + address;
                    const uint8_t *end = memchr(p, 0, nso->image_size - address);
                    if (!end || (size_t)(end - p) >= n || end == p) return false;
                    memcpy(out, p, (size_t)(end - p));
                    out[end - p] = 0;
                    return true;
                }
                if (back < 4) break;
            }
            return false;
        }
        if (at < 4) break;
    }
    return false;
}

bool aoc_main_uses(const NsoImage *main) {
    Syms s;
    if (!syms_open(main, &s)) return false;
    for (int k = 0; k < AOC_TARGETS; k++)
        if (imports(&s, REPLACED[k])) return true;
    return false;
}

bool aoc_locate(const NsoImage *sdk, const NsoImage *main, AocSites *out, char *err, size_t errn) {
    memset(out, 0, sizeof(*out));
    if (nso_is32(sdk) || nso_is32(main)) {
        snprintf(err, errn, "Add-on content is only built into 64-bit games so far.");
        return false;
    }
    Syms ss, ms;
    if (!syms_open(sdk, &ss) || !syms_open(main, &ms)) {
        snprintf(err, errn, "The game's code has no symbol table SeedSwap can read.");
        return false;
    }
    if (!aoc_main_uses(main)) {
        snprintf(err, errn, "This version of the game never asks for add-on content, so it would have no effect.");
        return false;
    }
    for (int k = 0; k < AOC_TARGETS; k++) {
        long i = sym_find(&ss, REPLACED[k], true);
        if (i < 0 || !sym_size(&ss, (uint32_t)i) || sym_value(&ss, (uint32_t)i) + sym_size(&ss, (uint32_t)i) > sdk->size[0]) {
            snprintf(err, errn, "The game's sdk has no usable %s.", NAMES[k]);
            return false;
        }
        out->target[k] = (uint32_t)sym_value(&ss, (uint32_t)i);
        out->room[k] = (uint32_t)sym_size(&ss, (uint32_t)i);
    }
    for (int k = 0; k < AOC_CALLEES; k++) {
        long at = plt_stub(sdk, &ss, CALLED[k]);
        if (at < 0) {
            long i = sym_find(&ss, CALLED[k], true);
            at = i >= 0 ? (long)sym_value(&ss, (uint32_t)i) : -1;
        }
        if (at < 0) {
            snprintf(err, errn, "The game's sdk is too old for this way of mounting add-on content (no %s).", CALLED[k]);
            return false;
        }
        out->callee[k] = (uint32_t)at;
    }
    long stub = plt_stub(main, &ms, MOUNT_ROM);
    if (stub < 0) {
        snprintf(err, errn, "The game does not mount its RomFS the usual way (no nn::fs::MountRom call).");
        return false;
    }
    int found = 0;
    for (uint32_t at = 0; at + 4 <= main->size[0]; at += 4) {
        uint32_t w = rd32(main->image + at);
        if (w >> 26 != 0x25 || branch_target(w, at) != stub) continue;
        char name[32];
        if (!string_argument(main, at, name, sizeof(name))) {
            snprintf(err, errn, "The game's RomFS mount at main+0x%x does not form its name the expected way.", at);
            return false;
        }
        if (found && strcmp(name, out->mount_name)) {
            snprintf(err, errn, "The game mounts its RomFS under two names (%s, %s).", out->mount_name, name);
            return false;
        }
        snprintf(out->mount_name, sizeof(out->mount_name), "%s", name);
        found++;
    }
    if (!found) {
        snprintf(err, errn, "The game never calls nn::fs::MountRom.");
        return false;
    }
    return true;
}

// ---- the bodies
enum { L_DONE, L_TABLE, L_NEXT, L_OUT, L_BAD, L_TEMPLATE, L_DIGIT, L_COUNT };
enum { F_B, F_BL, F_CBZ, F_CBNZ, F_TBNZ31, F_BNE, F_BHI, F_BGE, F_BLT, F_ADR };

typedef struct {
    uint32_t base;
    uint32_t words[160];
    int n;
    int64_t label[L_COUNT];
    struct { int at, kind; int64_t target; int label, reg; } fix[32];
    int nfix;
    bool overflow;
} Asm;

static void emit(Asm *a, uint32_t w) {
    if (a->n < (int)(sizeof(a->words) / 4)) a->words[a->n++] = w;
    else a->overflow = true;
}
static void label(Asm *a, int l) { a->label[l] = a->base + 4 * (uint32_t)a->n; }
static void branch_to(Asm *a, int kind, int l, int64_t target, int reg) {
    if (a->nfix < 32) {
        a->fix[a->nfix].at = a->n;
        a->fix[a->nfix].kind = kind;
        a->fix[a->nfix].label = l;
        a->fix[a->nfix].target = target;
        a->fix[a->nfix].reg = reg;
        a->nfix++;
    } else a->overflow = true;
    emit(a, 0);
}
#define SP 31
#define FP 29
#define LR 30
static void stp_pre(Asm *a, int t, int t2, int imm) { emit(a, 0xA9800000u | (uint32_t)((imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | SP << 5 | (uint32_t)t); }
static void ldp_post(Asm *a, int t, int t2, int imm) { emit(a, 0xA8C00000u | (uint32_t)((imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | SP << 5 | (uint32_t)t); }
static void stp(Asm *a, int t, int t2, int n, int imm) { emit(a, 0xA9000000u | (uint32_t)((imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void ldp(Asm *a, int t, int t2, int n, int imm) { emit(a, 0xA9400000u | (uint32_t)((imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void str_x(Asm *a, int t, int n, int imm) { emit(a, 0xF9000000u | (uint32_t)(imm / 8) << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void ldr_x(Asm *a, int t, int n, int imm) { emit(a, 0xF9400000u | (uint32_t)(imm / 8) << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void add_x(Asm *a, int d, int n, int imm) { emit(a, 0x91000000u | (uint32_t)imm << 10 | (uint32_t)n << 5 | (uint32_t)d); }
static void add_w(Asm *a, int d, int n, int imm) { emit(a, 0x11000000u | (uint32_t)imm << 10 | (uint32_t)n << 5 | (uint32_t)d); }
static void subs_w(Asm *a, int d, int n, int imm) { emit(a, 0x71000000u | (uint32_t)imm << 10 | (uint32_t)n << 5 | (uint32_t)d); }
static void cmp_w_imm(Asm *a, int n, int imm) { emit(a, 0x7100001Fu | (uint32_t)imm << 10 | (uint32_t)n << 5); }
static void cmp_w(Asm *a, int n, int m) { emit(a, 0x6B00001Fu | (uint32_t)m << 16 | (uint32_t)n << 5); }
static void mov_x(Asm *a, int d, int m) { emit(a, 0xAA0003E0u | (uint32_t)m << 16 | (uint32_t)d); }
static void mov_w(Asm *a, int d, int m) { emit(a, 0x2A0003E0u | (uint32_t)m << 16 | (uint32_t)d); }
static void movz_w(Asm *a, int d, uint32_t imm, int shift) { emit(a, 0x52800000u | (uint32_t)(shift / 16) << 21 | imm << 5 | (uint32_t)d); }
static void movk_w(Asm *a, int d, uint32_t imm, int shift) { emit(a, 0x72800000u | (uint32_t)(shift / 16) << 21 | imm << 5 | (uint32_t)d); }
static void udiv_w(Asm *a, int d, int n, int m) { emit(a, 0x1AC00800u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d); }
static void msub_w(Asm *a, int d, int n, int m, int ra) { emit(a, 0x1B008000u | (uint32_t)m << 16 | (uint32_t)ra << 10 | (uint32_t)n << 5 | (uint32_t)d); }
static void strb_post(Asm *a, int t, int n, int imm) { emit(a, 0x38000400u | (uint32_t)(imm & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t); }
static void ldr_w_index(Asm *a, int t, int n, int m) { emit(a, 0xB8605800u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)t); }
static void str_w_index(Asm *a, int t, int n, int m) { emit(a, 0xB8205800u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)t); }

static bool finish(Asm *a) {
    if (a->overflow) return false;
    static const uint32_t COND[] = {[F_BNE] = 1, [F_BHI] = 8, [F_BGE] = 10, [F_BLT] = 11};
    for (int i = 0; i < a->nfix; i++) {
        int64_t at = a->base + 4 * a->fix[i].at;
        int64_t dest = a->fix[i].label >= 0 ? a->label[a->fix[i].label] : a->fix[i].target;
        if (dest < 0) return false;
        int64_t d = dest - at;
        uint32_t w, reg = (uint32_t)a->fix[i].reg;
        switch (a->fix[i].kind) {
        case F_B: case F_BL:
            if (d % 4 || d < -(1 << 27) || d >= (1 << 27)) return false;
            w = (a->fix[i].kind == F_BL ? 0x94000000u : 0x14000000u) | (uint32_t)((d >> 2) & 0x3FFFFFF);
            break;
        case F_CBZ: case F_CBNZ:
            w = (a->fix[i].kind == F_CBZ ? 0x34000000u : 0x35000000u) | (uint32_t)((d / 4) & 0x7FFFF) << 5 | reg;
            break;
        case F_TBNZ31: w = 0x37F80000u | (uint32_t)((d / 4) & 0x3FFF) << 5 | reg; break;
        case F_ADR: w = 0x10000000u | (uint32_t)(d & 3) << 29 | (uint32_t)((d >> 2) & 0x7FFFF) << 5 | reg; break;
        default: w = 0x54000000u | (uint32_t)((d / 4) & 0x7FFFF) << 5 | COND[a->fix[i].kind]; break;
        }
        a->words[a->fix[i].at] = w;
    }
    return true;
}

static void asm_init(Asm *a, uint32_t base) {
    memset(a, 0, sizeof(*a));
    a->base = base;
    for (int i = 0; i < L_COUNT; i++) a->label[i] = -1;
}

// w1 = index. Writes "<rom>:/__aoc/<iiii>.romfs" to [sp + slot]; branches to L_BAD for an index above
// AOC_MAX_INDEX (and every negative one). Uses x9-x14; w1 is consumed.
static void path_onto_stack(Asm *a, int slot, int digits_at) {
    movz_w(a, 9, AOC_MAX_INDEX, 0);
    cmp_w(a, 1, 9);
    branch_to(a, F_BHI, L_BAD, 0, 0);
    branch_to(a, F_ADR, L_TEMPLATE, 0, 9);
    for (int at = 0; at < PATH_ROOM; at += 16) {
        ldp(a, 10, 11, 9, at);
        stp(a, 10, 11, SP, slot + at);
    }
    add_x(a, 11, SP, slot + digits_at + 3);
    movz_w(a, 10, 10, 0);
    movz_w(a, 12, 4, 0);
    label(a, L_DIGIT);
    udiv_w(a, 13, 1, 10);
    msub_w(a, 14, 13, 10, 1);
    add_w(a, 14, 14, 0x30);
    strb_post(a, 14, 11, -1);
    mov_w(a, 1, 13);
    subs_w(a, 12, 12, 1);
    branch_to(a, F_BNE, L_DIGIT, 0, 0);
}

typedef struct {
    uint8_t blob[AOC_TARGETS][1024];
    size_t size[AOC_TARGETS];
} Bodies;

static bool put(Bodies *b, int k, Asm *a, const void *data, size_t data_size) {
    if (!finish(a)) return false;
    size_t code = 4 * (size_t)a->n;
    if (code + data_size > sizeof(b->blob[k])) return false;
    for (int i = 0; i < a->n; i++) wr32(b->blob[k] + 4 * i, a->words[i]);
    memcpy(b->blob[k] + code, data, data_size);
    b->size[k] = code + data_size;
    return true;
}

static bool build_bodies(const AocSites *s, const int *indices, int count, Bodies *b, char *err, size_t errn) {
    char template[PATH_ROOM] = {0};
    int len = snprintf(template, sizeof(template), "%s:/" AOC_DIRECTORY "/0000.romfs", s->mount_name);
    if (len + 1 > PATH_ROOM) {
        snprintf(err, errn, "The game's RomFS mount name (%s) is too long.", s->mount_name);
        return false;
    }
    int digits_at = (int)(strstr(template, "/0000.") - template) + 1;
    int32_t table[AOC_MAX_ADD_ONS];
    for (int i = 0; i < count; i++) table[i] = indices[i];
    Asm a;
    bool ok = true;

    asm_init(&a, s->target[AOC_COUNT]);
    movz_w(&a, 0, (uint32_t)count, 0);
    emit(&a, RET);
    ok = ok && put(b, AOC_COUNT, &a, NULL, 0);

    asm_init(&a, s->target[AOC_LIST]);  // x0 = int *out, w1 = offset, w2 = count
    mov_x(&a, 12, 0);
    movz_w(&a, 0, 0, 0);
    branch_to(&a, F_TBNZ31, L_DONE, 0, 1);
    branch_to(&a, F_ADR, L_TABLE, 0, 9);
    label(&a, L_NEXT);
    cmp_w_imm(&a, 1, count);
    branch_to(&a, F_BGE, L_DONE, 0, 0);
    cmp_w(&a, 0, 2);
    branch_to(&a, F_BGE, L_DONE, 0, 0);
    ldr_w_index(&a, 11, 9, 1);
    str_w_index(&a, 11, 12, 0);
    add_w(&a, 1, 1, 1);
    add_w(&a, 0, 0, 1);
    branch_to(&a, F_B, L_NEXT, 0, 0);
    label(&a, L_DONE);
    emit(&a, RET);
    label(&a, L_TABLE);
    ok = ok && put(b, AOC_LIST, &a, table, 4 * (size_t)count);

    asm_init(&a, s->target[AOC_QUERY]);  // x0 = size_t *out, w1 = index
    stp_pre(&a, FP, LR, -0x60);
    add_x(&a, FP, SP, 0);
    str_x(&a, 19, SP, 0x10);
    mov_x(&a, 19, 0);
    path_onto_stack(&a, 0x30, digits_at);
    add_x(&a, 0, SP, 0x20);
    add_x(&a, 1, SP, 0x30);
    movz_w(&a, 2, 1, 0);
    branch_to(&a, F_BL, -1, s->callee[AOC_OPEN], 0);
    branch_to(&a, F_CBNZ, L_OUT, 0, 0);
    mov_x(&a, 0, 19);
    ldr_x(&a, 1, SP, 0x20);
    branch_to(&a, F_BL, -1, s->callee[AOC_QUERY_FILE], 0);
    mov_w(&a, 19, 0);
    ldr_x(&a, 0, SP, 0x20);
    branch_to(&a, F_BL, -1, s->callee[AOC_CLOSE], 0);
    mov_w(&a, 0, 19);
    label(&a, L_OUT);
    ldr_x(&a, 19, SP, 0x10);
    ldp_post(&a, FP, LR, 0x60);
    emit(&a, RET);
    label(&a, L_BAD);
    movz_w(&a, 0, AOC_NOT_FOUND & 0xFFFF, 0);
    movk_w(&a, 0, AOC_NOT_FOUND >> 16, 16);
    branch_to(&a, F_B, L_OUT, 0, 0);
    label(&a, L_TEMPLATE);
    ok = ok && put(b, AOC_QUERY, &a, template, PATH_ROOM);

    asm_init(&a, s->target[AOC_MOUNT]);  // x0 = name, w1 = index, x2 = cache, x3 = cache size
    stp_pre(&a, FP, LR, -0x70);
    add_x(&a, FP, SP, 0);
    stp(&a, 19, 20, SP, 0x10);
    str_x(&a, 21, SP, 0x20);
    mov_x(&a, 19, 0);
    mov_x(&a, 20, 2);
    mov_x(&a, 21, 3);
    path_onto_stack(&a, 0x40, digits_at);
    add_x(&a, 0, SP, 0x30);
    add_x(&a, 1, SP, 0x40);
    movz_w(&a, 2, 1, 0);
    branch_to(&a, F_BL, -1, s->callee[AOC_OPEN], 0);
    branch_to(&a, F_CBNZ, L_OUT, 0, 0);
    mov_x(&a, 0, 19);
    ldr_x(&a, 1, SP, 0x30);
    mov_x(&a, 2, 20);
    mov_x(&a, 3, 21);
    branch_to(&a, F_BL, -1, s->callee[AOC_MOUNT_FILE], 0);
    branch_to(&a, F_CBZ, L_OUT, 0, 0);
    mov_w(&a, 19, 0);
    ldr_x(&a, 0, SP, 0x30);
    branch_to(&a, F_BL, -1, s->callee[AOC_CLOSE], 0);
    mov_w(&a, 0, 19);
    label(&a, L_OUT);
    ldr_x(&a, 21, SP, 0x20);
    ldp(&a, 19, 20, SP, 0x10);
    ldp_post(&a, FP, LR, 0x70);
    emit(&a, RET);
    label(&a, L_BAD);
    movz_w(&a, 0, AOC_NOT_FOUND & 0xFFFF, 0);
    movk_w(&a, 0, AOC_NOT_FOUND >> 16, 16);
    branch_to(&a, F_B, L_OUT, 0, 0);
    label(&a, L_TEMPLATE);
    ok = ok && put(b, AOC_MOUNT, &a, template, PATH_ROOM);

    if (!ok) snprintf(err, errn, "The new sdk code could not be assembled.");
    return ok;
}

static int cmp_int(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }

int aoc_apply(NsoImage *sdk, const AocSites *s, const int *indices_in, int count, char *err, size_t errn) {
    int indices[AOC_MAX_ADD_ONS];
    if (count < 1 || count > AOC_MAX_ADD_ONS) {
        snprintf(err, errn, "Between 1 and %d add-on contents can be built in.", AOC_MAX_ADD_ONS);
        return -1;
    }
    memcpy(indices, indices_in, sizeof(int) * (size_t)count);
    qsort(indices, (size_t)count, sizeof(int), cmp_int);
    for (int i = 0; i < count; i++)
        if (indices[i] < 0 || indices[i] > AOC_MAX_INDEX || (i && indices[i] == indices[i - 1])) {
            snprintf(err, errn, "Add-on content numbers must be different numbers from 0 to %d.", AOC_MAX_INDEX);
            return -1;
        }
    Bodies *b = malloc(sizeof(Bodies));
    if (!b || !build_bodies(s, indices, count, b, err, errn)) {
        free(b);
        return -1;
    }
    int changed = 0;
    for (int k = 0; k < AOC_TARGETS; k++) {
        if (b->size[k] > s->room[k]) {
            snprintf(err, errn, "The new %s is %u bytes, the function it replaces only %u.", NAMES[k], (unsigned)b->size[k],
                     s->room[k]);
            free(b);
            return -1;
        }
    }
    for (int k = 0; k < AOC_TARGETS; k++) {
        uint8_t *at = sdk->image + sdk->mem[0] + s->target[k];
        for (size_t i = 0; i < b->size[k]; i += 4) changed += rd32(at + i) != rd32(b->blob[k] + i);
        memcpy(at, b->blob[k], b->size[k]);
    }
    free(b);
    return changed;
}

// ---- running the bodies in the interpreter
#define BASE 0x10000000ull
#define STACK 0x7000000ull
#define HEAP 0x6000000ull
#define STOP 0x5000000ull
#define REGION 0x10000

typedef struct {
    const AocSites *s;
    const int *indices;
    int count;
    uint32_t mount_result;
    struct { int what; char path[64]; uint64_t a, b, c, d; } calls[16];
    int ncalls;
    uint8_t *heap;
} Sim;

enum { SVC_STOP = 0xff };

static bool built(const Sim *m, const char *path) {
    for (int i = 0; i < m->count; i++) {
        char want[64], name[16];
        aoc_image_name(m->indices[i], name);
        snprintf(want, sizeof(want), "%s:/" AOC_DIRECTORY "/%s", m->s->mount_name, name);
        if (!strcmp(want, path)) return true;
    }
    return false;
}

static int svc(A64 *cpu, uint32_t number, void *user) {
    Sim *m = user;
    if (number == SVC_STOP) return A64_HALT;
    if (number >= AOC_CALLEES || m->ncalls >= 16) return A64_ERR_SVC;
    typeof(m->calls[0]) *c = &m->calls[m->ncalls++];
    memset(c, 0, sizeof(*c));
    c->what = (int)number;
    c->a = cpu->x[0];
    c->b = cpu->x[1];
    c->c = cpu->x[2];
    c->d = cpu->x[3];
    switch (number) {
    case AOC_OPEN: {
        uint8_t *p = a64_pointer(cpu, cpu->x[1], 64, 0);
        if (!p) return A64_ERR_SVC;
        memcpy(c->path, p, 63);
        c->path[63] = 0;
        bool found = built(m, c->path);
        uint8_t *handle = a64_pointer(cpu, cpu->x[0], 8, 1);
        if (!handle) return A64_ERR_SVC;
        if (found) wr64(handle, 0xF11E0000ull + (uint64_t)m->ncalls);
        cpu->x[0] = found ? 0 : 0x202;
        break;
    }
    case AOC_QUERY_FILE: {
        uint8_t *out = a64_pointer(cpu, cpu->x[0], 8, 1);
        if (!out) return A64_ERR_SVC;
        wr64(out, 0x1234);
        cpu->x[0] = 0;
        break;
    }
    case AOC_MOUNT_FILE: {
        uint8_t *p = a64_pointer(cpu, cpu->x[0], 32, 0);
        if (!p) return A64_ERR_SVC;
        memcpy(c->path, p, 31);
        c->path[31] = 0;
        cpu->x[0] = m->mount_result;
        break;
    }
    case AOC_CLOSE: cpu->x[0] = 0; break;
    }
    return A64_OK;
}

// Runs one function. Returns false when it did not return cleanly; *w0 is its result.
static bool run(Sim *m, const uint8_t *code, uint64_t code_size, int key, uint64_t x0, uint64_t x1, uint64_t x2,
                uint64_t x3, uint32_t *w0, uint8_t *stack, uint8_t *stop) {
    A64 cpu;
    a64_init(&cpu);
    cpu.svc = svc;
    cpu.user = m;
    cpu.max_steps = 20000;
    m->ncalls = 0;
    memset(stack, 0, REGION);
    if (a64_map(&cpu, BASE, code_size, (uint8_t *)code, 0, 0, 0) < 0 || a64_map(&cpu, STACK, REGION, stack, 1, 0, 0) < 0 ||
        a64_map(&cpu, HEAP, REGION, m->heap, 1, 0, 0) < 0 || a64_map(&cpu, STOP, 0x1000, stop, 0, 0, 0) < 0)
        return false;
    cpu.x[0] = x0;
    cpu.x[1] = x1;
    cpu.x[2] = x2;
    cpu.x[3] = x3;
    cpu.x[19] = 0x1919;
    cpu.x[20] = 0x2020;
    cpu.x[21] = 0x2121;
    cpu.x[29] = 0x2929;
    cpu.x[30] = STOP;
    cpu.x[31] = STACK + 0x8000;
    cpu.pc = BASE + m->s->target[key];
    int rc = a64_run(&cpu);
    *w0 = (uint32_t)cpu.x[0];
    return rc == A64_HALT && cpu.pc == STOP + 4 && cpu.x[31] == STACK + 0x8000 && cpu.x[19] == 0x1919 &&
           cpu.x[20] == 0x2020 && cpu.x[21] == 0x2121 && cpu.x[29] == 0x2929;
}

bool aoc_check(const NsoImage *sdk, const AocSites *s, const int *indices_in, int count, char *err, size_t errn) {
    int indices[AOC_MAX_ADD_ONS];
    if (count < 1 || count > AOC_MAX_ADD_ONS) return false;
    memcpy(indices, indices_in, sizeof(int) * (size_t)count);
    qsort(indices, (size_t)count, sizeof(int), cmp_int);
    // a copy of the module's text with each called function replaced by "svc #k; ret"
    uint64_t code_size = (sdk->size[0] + 0xFFF) & ~0xFFFull;
    uint8_t *code = calloc(1, code_size), *stack = malloc(REGION), *heap = malloc(REGION), *stop = calloc(1, 0x1000);
    bool ok = code && stack && heap && stop;
    Sim m = {.s = s, .indices = indices, .count = count, .heap = heap};
    if (!ok) goto out;
    memcpy(code, sdk->image + sdk->mem[0], sdk->size[0]);
    for (int k = 0; k < AOC_CALLEES; k++) {
        wr32(code + s->callee[k], 0xD4000001u | (uint32_t)k << 5);
        wr32(code + s->callee[k] + 4, RET);
    }
    wr32(stop, 0xD4000001u | SVC_STOP << 5);
    uint32_t r;
#define FAIL(...)                              \
    do {                                       \
        snprintf(err, errn, __VA_ARGS__);      \
        ok = false;                            \
        goto out;                              \
    } while (0)

    memset(heap, 0, REGION);
    if (!run(&m, code, code_size, AOC_COUNT, 0, 0, 0, 0, &r, stack, stop) || r != (uint32_t)count)
        FAIL("CountAddOnContent does not return %d.", count);
    static const int TRIES[][2] = {{0, 16}, {0, 1}, {1, 16}, {-1, 16}, {0, 0}, {0, 2000}, {AOC_MAX_ADD_ONS, 16}};
    for (size_t t = 0; t < sizeof(TRIES) / sizeof(*TRIES); t++) {
        int off = TRIES[t][0], want = TRIES[t][1];
        memset(heap, 0, REGION);
        int expect = off < 0 || off >= count ? 0 : (count - off < want ? count - off : want);
        if (!run(&m, code, code_size, AOC_LIST, HEAP, (uint64_t)(int64_t)off, (uint64_t)want, 0, &r, stack, stop) ||
            r != (uint32_t)expect || rd32(heap + 4 * expect) != 0)
            FAIL("ListAddOnContent(offset %d, count %d) does not give %d entries.", off, want, expect);
        for (int i = 0; i < expect; i++)
            if ((int)rd32(heap + 4 * i) != indices[off + i]) FAIL("ListAddOnContent gives the wrong numbers.");
    }
    for (int i = 0; i < count; i++) {
        char path[64], name[16];
        aoc_image_name(indices[i], name);
        snprintf(path, sizeof(path), "%s:/" AOC_DIRECTORY "/%s", s->mount_name, name);
        memset(heap, 0, REGION);
        if (!run(&m, code, code_size, AOC_QUERY, HEAP + 0x400, (uint64_t)indices[i], 0, 0, &r, stack, stop) || r ||
            rd64(heap + 0x400) != 0x1234 || m.ncalls != 3 || m.calls[0].what != AOC_OPEN || strcmp(m.calls[0].path, path) ||
            m.calls[0].c != 1 || m.calls[1].what != AOC_QUERY_FILE || m.calls[2].what != AOC_CLOSE ||
            m.calls[1].b != m.calls[2].a)
            FAIL("QueryMountAddOnContentCacheSize(%d) does not open %s and ask its cache size.", indices[i], path);
        memset(heap, 0, REGION);
        memcpy(heap + 0x800, "aoc", 4);
        if (!run(&m, code, code_size, AOC_MOUNT, HEAP + 0x800, (uint64_t)indices[i], 0xCAC4E000, 0x4000, &r, stack, stop) ||
            r || m.ncalls != 2 || m.calls[0].what != AOC_OPEN || strcmp(m.calls[0].path, path) ||
            m.calls[1].what != AOC_MOUNT_FILE || strcmp(m.calls[1].path, "aoc") || m.calls[1].c != 0xCAC4E000 ||
            m.calls[1].d != 0x4000)
            FAIL("MountAddOnContent(%d) does not mount %s.", indices[i], path);
    }
    int missing = 0;  // the smallest number that was not built in
    for (int i = 0; i < count; i++)
        if (indices[i] == missing) missing++;
    const int64_t BAD[][2] = {{missing, 0x202}, {AOC_MAX_INDEX + 1, AOC_NOT_FOUND}, {-1, AOC_NOT_FOUND}, {1 << 20, AOC_NOT_FOUND}};
    for (size_t t = 0; t < sizeof(BAD) / sizeof(*BAD); t++) {
        memset(heap, 0, REGION);
        memcpy(heap + 0x800, "aoc", 4);
        if (!run(&m, code, code_size, AOC_MOUNT, HEAP + 0x800, (uint64_t)BAD[t][0], 0xCAC4E000, 0x4000, &r, stack, stop) ||
            r != (uint32_t)BAD[t][1])
            FAIL("MountAddOnContent(%lld) does not fail as it should.", (long long)BAD[t][0]);
        for (int c = 0; c < m.ncalls; c++)
            if (m.calls[c].what == AOC_MOUNT_FILE || (BAD[t][1] == AOC_NOT_FOUND && m.calls[c].what == AOC_OPEN))
                FAIL("MountAddOnContent(%lld) mounts or opens something it should not.", (long long)BAD[t][0]);
        memset(heap, 0, REGION);
        if (!run(&m, code, code_size, AOC_QUERY, HEAP + 0x400, (uint64_t)BAD[t][0], 0, 0, &r, stack, stop) ||
            r != (uint32_t)BAD[t][1])
            FAIL("QueryMountAddOnContentCacheSize(%lld) does not fail as it should.", (long long)BAD[t][0]);
    }
    m.mount_result = 0x177202;
    memset(heap, 0, REGION);
    memcpy(heap + 0x800, "aoc", 4);
    if (!run(&m, code, code_size, AOC_MOUNT, HEAP + 0x800, (uint64_t)indices[0], 0xCAC4E000, 0x4000, &r, stack, stop) ||
        r != 0x177202 || m.ncalls != 3 || m.calls[2].what != AOC_CLOSE || m.calls[1].b != m.calls[2].a)
        FAIL("A failed mount does not close the file again.");
#undef FAIL
out:
    free(code);
    free(stack);
    free(heap);
    free(stop);
    return ok;
}

// ---- the RomFS append
#define NONE 0xFFFFFFFFu
#define DIR_FIXED 0x18
#define FILE_FIXED 0x20

static uint64_t align_up(uint64_t v, uint64_t to) { return (v + to - 1) & ~(to - 1); }

static uint32_t name_hash(uint32_t parent, const char *name, size_t n) {
    uint32_t v = parent ^ 123456789u;
    for (size_t i = 0; i < n; i++) {
        v = v >> 5 | v << 27;
        v ^= (uint8_t)name[i];
    }
    return v;
}

typedef struct {
    uint64_t h[10];
    uint8_t *dir_hash, *dir_meta, *file_hash, *file_meta;
} Tables;

static void tables_free(Tables *t) {
    free(t->dir_hash);
    free(t->dir_meta);
    free(t->file_hash);
    free(t->file_meta);
}

static bool tables_read(RomfsReadFn read, void *user, const uint8_t header[0x50], Tables *t) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 10; i++) t->h[i] = rd64(header + 8 * i);
    uint64_t *h = t->h;
    if (h[0] != 0x50 || !h[2] || h[2] % 4 || !h[6] || h[6] % 4 || h[4] < DIR_FIXED || h[4] % 4 || h[8] % 4) return false;
    for (int i = 2; i <= 8; i += 2)
        if (h[i] > 64 * 1024 * 1024) return false;
    uint8_t **dst[4] = {&t->dir_hash, &t->dir_meta, &t->file_hash, &t->file_meta};
    for (int i = 0; i < 4; i++) {
        *dst[i] = malloc(h[2 + 2 * i] ? (size_t)h[2 + 2 * i] : 1);
        if (!*dst[i] || (h[2 + 2 * i] && read(user, h[1 + 2 * i], *dst[i], (size_t)h[2 + 2 * i]))) {
            tables_free(t);
            return false;
        }
    }
    return true;
}

static int64_t lookup(const uint8_t *table, uint64_t table_size, const uint8_t *meta, uint64_t meta_size, int fixed,
                      uint32_t parent, const char *name, size_t n) {
    uint32_t at = rd32(table + 4 * (name_hash(parent, name, n) % (table_size / 4)));
    for (uint64_t guard = meta_size / (uint64_t)fixed + 1; at != NONE && guard; guard--) {
        if ((uint64_t)at + (uint64_t)fixed > meta_size) return -1;
        uint32_t length = rd32(meta + at + fixed - 4);
        if (rd32(meta + at) == parent && length == n && (uint64_t)at + (uint64_t)fixed + n <= meta_size &&
            !memcmp(meta + at + fixed, name, n))
            return at;
        at = rd32(meta + at + fixed - 8);
    }
    return -1;
}

static int64_t find_dir(const Tables *t, const char *path) {
    uint32_t at = 0;
    while (*path == '/') path++;
    while (*path) {
        const char *slash = strchr(path, '/');
        size_t n = slash ? (size_t)(slash - path) : strlen(path);
        int64_t next = lookup(t->dir_hash, t->h[2], t->dir_meta, t->h[4], DIR_FIXED, at, path, n);
        if (next < 0) return -1;
        at = (uint32_t)next;
        path += n;
        while (*path == '/') path++;
    }
    return at;
}

bool romfs_find(RomfsReadFn read, void *user, const uint8_t header[0x50], const char *path, uint64_t *offset,
                uint64_t *size) {
    Tables t;
    if (!tables_read(read, user, header, &t)) return false;
    const char *slash = strrchr(path, '/');
    char dir[256];
    size_t dn = slash ? (size_t)(slash - path) : 0;
    bool ok = false;
    if (dn < sizeof(dir)) {
        memcpy(dir, path, dn);
        dir[dn] = 0;
        int64_t parent = find_dir(&t, dir);
        const char *name = slash ? slash + 1 : path;
        int64_t at = parent < 0 ? -1 : lookup(t.file_hash, t.h[6], t.file_meta, t.h[8], FILE_FIXED, (uint32_t)parent, name, strlen(name));
        if (at >= 0) {
            *offset = t.h[9] + rd64(t.file_meta + at + 8);
            *size = rd64(t.file_meta + at + 16);
            ok = true;
        }
    }
    tables_free(&t);
    return ok;
}

long romfs_count_files(RomfsReadFn read, void *user, const uint8_t header[0x50]) {
    Tables t;
    if (!tables_read(read, user, header, &t)) return -1;
    long n = 0;
    for (uint64_t at = 0; at + FILE_FIXED <= t.h[8]; n++) at += align_up(FILE_FIXED + rd32(t.file_meta + at + 0x1c), 4);
    tables_free(&t);
    return n;
}

bool romfs_plan(RomfsReadFn read, void *user, const uint8_t header[0x50], uint64_t append_at, const char *directory,
                const RomfsNewFile *files, int count, RomfsPlan *out, char *err, size_t errn) {
    memset(out, 0, sizeof(*out));
    Tables t;
    if (count < 1 || count > AOC_MAX_ADD_ONS || !tables_read(read, user, header, &t)) {
        snprintf(err, errn, "The game's romfs.bin does not have readable RomFS tables.");
        return false;
    }
    bool ok = false;
    size_t name_len = strlen(directory);
    uint64_t tables_end = 0;
    for (int i = 1; i <= 7; i += 2)
        if (t.h[i] + t.h[i + 1] > tables_end) tables_end = t.h[i] + t.h[i + 1];
    if (append_at < tables_end || !name_len || strchr(directory, '/')) {
        snprintf(err, errn, "The append does not fit the image.");
        goto done;
    }
    if (find_dir(&t, directory) >= 0) {
        snprintf(err, errn, "The game's romfs.bin already holds /%s.", directory);
        goto done;
    }
    {
        // the tail: each file 16-byte aligned, then the four tables
        uint64_t tail = 0;
        for (int i = 0; i < count; i++) {
            tail = align_up(append_at + tail, 16) - append_at;
            out->file_offset[i] = append_at + tail;
            tail += files[i].size;
        }
        uint64_t files_end = align_up(append_at + tail, 16) - append_at;
        uint32_t new_dir = (uint32_t)t.h[4];
        uint64_t dir_meta = t.h[4] + align_up(DIR_FIXED + name_len, 4), file_meta = t.h[8];
        uint32_t records[AOC_MAX_ADD_ONS];
        for (int i = 0; i < count; i++) {
            records[i] = (uint32_t)file_meta;
            file_meta += align_up(FILE_FIXED + strlen(files[i].name), 4);
        }
        uint64_t total = files_end + t.h[2] + dir_meta + t.h[6] + file_meta;
        uint8_t *buf = calloc(1, (size_t)total);
        if (!buf) {
            snprintf(err, errn, "Not enough memory for the new RomFS tables.");
            goto done;
        }
        for (int i = 0; i < count; i++) memcpy(buf + (out->file_offset[i] - append_at), files[i].data, files[i].size);
        uint8_t *dh = buf + files_end, *dm = dh + t.h[2], *fh = dm + dir_meta, *fm = fh + t.h[6];
        memcpy(dh, t.dir_hash, (size_t)t.h[2]);
        memcpy(dm, t.dir_meta, (size_t)t.h[4]);
        memcpy(fh, t.file_hash, (size_t)t.h[6]);
        memcpy(fm, t.file_meta, (size_t)t.h[8]);
        // the directory: a child of the root, first in the root's list of children
        uint32_t bucket = 4 * (name_hash(0, directory, name_len) % (uint32_t)(t.h[2] / 4));
        uint8_t *d = dm + new_dir;
        wr32(d, 0);
        wr32(d + 4, rd32(dm + 8));
        wr32(d + 8, NONE);
        wr32(d + 12, records[0]);
        wr32(d + 16, rd32(dh + bucket));
        wr32(d + 20, (uint32_t)name_len);
        memcpy(d + DIR_FIXED, directory, name_len);
        wr32(dh + bucket, new_dir);
        wr32(dm + 8, new_dir);
        for (int i = 0; i < count; i++) {
            size_t n = strlen(files[i].name);
            uint32_t fb = 4 * (name_hash(new_dir, files[i].name, n) % (uint32_t)(t.h[6] / 4));
            uint8_t *f = fm + records[i];
            wr32(f, new_dir);
            wr32(f + 4, i + 1 < count ? records[i + 1] : NONE);
            wr64(f + 8, out->file_offset[i] - t.h[9]);
            wr64(f + 16, files[i].size);
            wr32(f + 24, rd32(fh + fb));
            wr32(f + 28, (uint32_t)n);
            memcpy(f + FILE_FIXED, files[i].name, n);
            wr32(fh + fb, records[i]);
        }
        uint64_t at = append_at + files_end;
        memcpy(out->old_header, header, 0x50);
        uint64_t h[10] = {0x50, at, t.h[2], at + t.h[2], dir_meta, at + t.h[2] + dir_meta, t.h[6],
                          at + t.h[2] + dir_meta + t.h[6], file_meta, t.h[9]};
        for (int i = 0; i < 10; i++) wr64(out->new_header + 8 * i, h[i]);
        out->base_size = append_at;
        out->append_at = append_at;
        out->tables_at = at;
        out->tail = buf;
        out->tail_size = (size_t)total;
        out->new_size = append_at + total;
        ok = true;
    }
done:
    tables_free(&t);
    return ok;
}

void romfs_plan_free(RomfsPlan *p) {
    free(p->tail);
    p->tail = NULL;
}
