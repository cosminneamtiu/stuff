/* SPDX-License-Identifier: GPL-2.0-only */
/* AArch64 integer interpreter. See a64.h. Written from the Arm ARM's
 * instruction descriptions; checked against the Unicorn emulator by
 * compat/test_engine.py (whole runs) and compat/test_a64.py (single
 * instructions). */
#include "a64.h"

#include <string.h>

#define N_FLAG 8u
#define Z_FLAG 4u
#define C_FLAG 2u
#define V_FLAG 1u

void a64_init(A64 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->max_steps = (uint64_t)1 << 36;
}

int a64_map(A64 *cpu, uint64_t base, uint64_t size, uint8_t *host, int writable, int track, int tag) {
    if (cpu->region_count >= A64_MAX_REGIONS || !size || base + size < base)
        return -1;
    A64Region *r = &cpu->regions[cpu->region_count];
    r->base = base;
    r->size = size;
    r->host = host;
    r->writable = writable;
    r->track = track;
    r->tag = tag;
    return cpu->region_count++;
}

const char *a64_error_text(int code) {
    switch (code) {
    case A64_OK: return "running";
    case A64_HALT: return "finished";
    case A64_ERR_UNDEFINED: return "unsupported instruction";
    case A64_ERR_MEMORY: return "memory access outside the image";
    case A64_ERR_STEPS: return "too many instructions";
    case A64_ERR_SVC: return "unexpected system call";
    case A64_ERR_REGIONS: return "too many memory regions";
    default: return "unknown error";
    }
}

static A64Region *find_region(A64 *cpu, uint64_t address, uint64_t size, int *cache) {
    A64Region *r = &cpu->regions[*cache];
    if (*cache < cpu->region_count && address - r->base < r->size && size <= r->size - (address - r->base))
        return r;
    for (int i = 0; i < cpu->region_count; i++) {
        r = &cpu->regions[i];
        if (address - r->base < r->size && size <= r->size - (address - r->base)) {
            *cache = i;
            return r;
        }
    }
    return NULL;
}

uint8_t *a64_pointer(A64 *cpu, uint64_t address, uint64_t size, int write) {
    A64Region *r = find_region(cpu, address, size, &cpu->last_data);
    if (!r || (write && !r->writable))
        return NULL;
    return r->host + (address - r->base);
}

/* --- memory ---------------------------------------------------------------- */

static int load(A64 *cpu, uint64_t address, unsigned size, uint64_t *out) {
    A64Region *r = find_region(cpu, address, size, &cpu->last_data);
    if (!r) {
        cpu->fault_address = address;
        return A64_ERR_MEMORY;
    }
    const uint8_t *p = r->host + (address - r->base);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)p[i] << (8 * i);
    *out = v;
    return A64_OK;
}

static int store(A64 *cpu, uint64_t address, unsigned size, uint64_t value) {
    A64Region *r = find_region(cpu, address, size, &cpu->last_data);
    if (!r || !r->writable) {
        cpu->fault_address = address;
        return A64_ERR_MEMORY;
    }
    uint8_t *p = r->host + (address - r->base);
    if (r->track) {
        if (cpu->write_count < cpu->write_capacity) {
            A64Write *w = &cpu->writes[cpu->write_count++];
            uint64_t old = 0;
            for (unsigned i = 0; i < size; i++)
                old |= (uint64_t)p[i] << (8 * i);
            w->address = address;
            w->old_value = old;
            w->size = (uint8_t)size;
        } else {
            cpu->write_overflow = 1;
        }
    }
    for (unsigned i = 0; i < size; i++)
        p[i] = (uint8_t)(value >> (8 * i));
    return A64_OK;
}

/* --- helpers ---------------------------------------------------------------- */

static inline uint64_t reg(const A64 *cpu, unsigned n) { /* register or zero */
    return n == 31 ? 0 : cpu->x[n];
}

static inline void set_reg(A64 *cpu, unsigned n, int sf, uint64_t v) {
    if (n != 31)
        cpu->x[n] = sf ? v : (uint32_t)v;
}

static inline void set_reg_sp(A64 *cpu, unsigned n, int sf, uint64_t v) {
    cpu->x[n] = sf ? v : (uint32_t)v;
}

static inline uint64_t sign_extend(uint64_t v, unsigned bits) {
    uint64_t m = (uint64_t)1 << (bits - 1);
    v &= (bits == 64) ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
    return (v ^ m) - m;
}

static inline uint64_t ror(uint64_t v, unsigned amount, unsigned width) {
    amount %= width;
    if (width == 64)
        return amount ? (v >> amount) | (v << (64 - amount)) : v;
    v &= 0xffffffffu;
    return amount ? ((v >> amount) | (v << (32 - amount))) & 0xffffffffu : v;
}

static uint64_t add_with_carry(int sf, uint64_t a, uint64_t b, unsigned carry, uint32_t *flags) {
    uint64_t result;
    unsigned n, z, c, v;
    if (sf) {
        result = a + b + carry;
        c = (result < a) || (carry && result == a);
        v = (unsigned)(((~(a ^ b)) & (a ^ result)) >> 63);
        n = (unsigned)(result >> 63);
    } else {
        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        uint64_t wide = (uint64_t)a32 + b32 + carry;
        result = (uint32_t)wide;
        c = (unsigned)(wide >> 32);
        v = (unsigned)(((~(a32 ^ b32)) & (a32 ^ (uint32_t)result)) >> 31);
        n = (unsigned)((uint32_t)result >> 31);
    }
    z = result == 0;
    *flags = (n ? N_FLAG : 0) | (z ? Z_FLAG : 0) | (c ? C_FLAG : 0) | (v ? V_FLAG : 0);
    return result;
}

static inline uint32_t logic_flags(int sf, uint64_t result) {
    if (!sf)
        result = (uint32_t)result;
    unsigned n = (unsigned)(result >> (sf ? 63 : 31)) & 1;
    return (n ? N_FLAG : 0) | (result == 0 ? Z_FLAG : 0);
}

static int condition(uint32_t nzcv, unsigned cond) {
    unsigned n = !!(nzcv & N_FLAG), z = !!(nzcv & Z_FLAG), c = !!(nzcv & C_FLAG), v = !!(nzcv & V_FLAG);
    int r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = c; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: return 1;
    }
    return (cond & 1) ? !r : r;
}

/* DecodeBitMasks from the Arm ARM. Returns 0 for a reserved encoding. */
static int bit_masks(unsigned n, unsigned imms, unsigned immr, int immediate, unsigned width, uint64_t *wmask,
                     uint64_t *tmask) {
    unsigned combined = (n << 6) | (~imms & 0x3f);
    int len = -1;
    for (int i = 6; i >= 0; i--) {
        if (combined & (1u << i)) {
            len = i;
            break;
        }
    }
    if (len < 1)
        return 0;
    unsigned esize = 1u << len;
    if (esize > width)
        return 0;
    unsigned levels = esize - 1;
    unsigned s = imms & levels, r = immr & levels;
    if (immediate && s == levels)
        return 0;
    unsigned diff = (s - r) & levels;
    uint64_t welem = (s + 1 >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << (s + 1)) - 1);
    uint64_t telem = (diff + 1 >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << (diff + 1)) - 1);
    uint64_t emask = esize == 64 ? ~(uint64_t)0 : (((uint64_t)1 << esize) - 1);
    /* rotate welem right by r within esize */
    uint64_t rotated = r ? ((welem >> r) | (welem << (esize - r))) & emask : welem;
    uint64_t w = 0, t = 0;
    for (unsigned i = 0; i < 64; i += esize) {
        w |= rotated << i;
        t |= telem << i;
    }
    if (width == 32) {
        w &= 0xffffffffu;
        t &= 0xffffffffu;
    }
    *wmask = w;
    *tmask = t;
    return 1;
}

static uint64_t shift_value(uint64_t v, unsigned type, unsigned amount, int sf) {
    unsigned width = sf ? 64 : 32;
    if (!sf)
        v = (uint32_t)v;
    if (amount == 0)
        return v;
    switch (type) {
    case 0: v = v << amount; break;
    case 1: v = v >> amount; break;
    case 2:
        if (sf)
            v = (uint64_t)((int64_t)v >> amount);
        else
            v = (uint32_t)((int32_t)(uint32_t)v >> amount);
        break;
    default: v = ror(v, amount, width); break;
    }
    return sf ? v : (uint32_t)v;
}

static uint64_t extend_value(uint64_t v, unsigned option, unsigned shift) {
    switch (option) {
    case 0: v = (uint8_t)v; break;
    case 1: v = (uint16_t)v; break;
    case 2: v = (uint32_t)v; break;
    case 3: break;
    case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
    case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
    case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
    default: break;
    }
    return v << shift;
}

static uint64_t umulh(uint64_t a, uint64_t b) {
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

static uint64_t smulh(int64_t a, int64_t b) {
    uint64_t high = umulh((uint64_t)a, (uint64_t)b);
    if (a < 0)
        high -= (uint64_t)b;
    if (b < 0)
        high -= (uint64_t)a;
    return high;
}

static uint64_t reverse_bits(uint64_t v, unsigned width) {
    uint64_t r = 0;
    for (unsigned i = 0; i < width; i++)
        if (v & ((uint64_t)1 << i))
            r |= (uint64_t)1 << (width - 1 - i);
    return r;
}

static uint64_t reverse_bytes(uint64_t v, unsigned total, unsigned group) { /* sizes in bytes */
    uint64_t r = 0;
    for (unsigned i = 0; i < total; i += group)
        for (unsigned j = 0; j < group; j++)
            r |= ((v >> (8 * (i + j))) & 0xff) << (8 * (i + group - 1 - j));
    return r;
}

static unsigned count_leading(uint64_t v, unsigned width, int sign) {
    unsigned n = 0;
    if (sign) { /* CLS: bits equal to the sign bit, after it */
        unsigned top = (unsigned)(v >> (width - 1)) & 1;
        for (int i = (int)width - 2; i >= 0; i--) {
            if (((v >> i) & 1) != top)
                break;
            n++;
        }
        return n;
    }
    for (int i = (int)width - 1; i >= 0; i--) {
        if ((v >> i) & 1)
            break;
        n++;
    }
    return n;
}

/* --- instruction groups ----------------------------------------------------- */

static int data_processing_immediate(A64 *cpu, uint32_t in) {
    unsigned sf = in >> 31, rd = in & 31, rn = (in >> 5) & 31;
    switch ((in >> 23) & 7) {
    case 0:
    case 1: { /* ADR, ADRP */
        uint64_t imm = sign_extend(((uint64_t)((in >> 5) & 0x7ffff) << 2) | ((in >> 29) & 3), 21);
        if (in >> 31)
            set_reg(cpu, rd, 1, (cpu->pc & ~(uint64_t)0xfff) + (imm << 12));
        else
            set_reg(cpu, rd, 1, cpu->pc + imm);
        return A64_OK;
    }
    case 2: { /* ADD, SUB immediate */
        uint64_t imm = (in >> 10) & 0xfff;
        if (in & (1u << 22))
            imm <<= 12;
        unsigned op = (in >> 30) & 1, s = (in >> 29) & 1;
        uint64_t a = cpu->x[rn];
        uint32_t flags;
        uint64_t result = op ? add_with_carry(sf, a, ~imm, 1, &flags) : add_with_carry(sf, a, imm, 0, &flags);
        if (s) {
            cpu->nzcv = flags;
            set_reg(cpu, rd, sf, result);
        } else {
            set_reg_sp(cpu, rd, sf, result);
        }
        return A64_OK;
    }
    case 4: { /* logical immediate */
        uint64_t wmask, tmask;
        unsigned n = (in >> 22) & 1;
        if ((!sf && n) || !bit_masks(n, (in >> 10) & 0x3f, (in >> 16) & 0x3f, 1, sf ? 64 : 32, &wmask, &tmask))
            return A64_ERR_UNDEFINED;
        uint64_t a = reg(cpu, rn), result;
        unsigned opc = (in >> 29) & 3;
        if (opc == 1)
            result = a | wmask;
        else if (opc == 2)
            result = a ^ wmask;
        else
            result = a & wmask;
        if (opc == 3) {
            cpu->nzcv = logic_flags(sf, result);
            set_reg(cpu, rd, sf, result);
        } else {
            set_reg_sp(cpu, rd, sf, result);
        }
        return A64_OK;
    }
    case 5: { /* MOVN, MOVZ, MOVK */
        unsigned opc = (in >> 29) & 3, hw = (in >> 21) & 3;
        uint64_t imm = (uint64_t)((in >> 5) & 0xffff) << (16 * hw);
        if (opc == 1 || (!sf && hw > 1))
            return A64_ERR_UNDEFINED;
        if (opc == 0)
            set_reg(cpu, rd, sf, ~imm);
        else if (opc == 2)
            set_reg(cpu, rd, sf, imm);
        else
            set_reg(cpu, rd, sf, (reg(cpu, rd) & ~((uint64_t)0xffff << (16 * hw))) | imm);
        return A64_OK;
    }
    case 6: { /* SBFM, BFM, UBFM */
        unsigned opc = (in >> 29) & 3, n = (in >> 22) & 1, immr = (in >> 16) & 0x3f, imms = (in >> 10) & 0x3f;
        unsigned width = sf ? 64 : 32;
        uint64_t wmask, tmask;
        if (opc == 3 || n != sf || (!sf && (immr > 31 || imms > 31)) ||
            !bit_masks(n, imms, immr, 0, width, &wmask, &tmask))
            return A64_ERR_UNDEFINED;
        uint64_t src = reg(cpu, rn);
        if (!sf)
            src = (uint32_t)src;
        uint64_t dst = opc == 1 ? reg(cpu, rd) : 0;
        if (!sf)
            dst = (uint32_t)dst;
        uint64_t bot = (dst & ~wmask) | (ror(src, immr, width) & wmask);
        uint64_t result;
        if (opc == 0) { /* SBFM: replicate bit imms above the field */
            uint64_t top = ((src >> imms) & 1) ? ~(uint64_t)0 : 0;
            result = (top & ~tmask) | (bot & tmask);
        } else if (opc == 1) {
            result = (dst & ~tmask) | (bot & tmask);
        } else {
            result = bot & tmask;
        }
        set_reg(cpu, rd, sf, result);
        return A64_OK;
    }
    case 7: { /* EXTR */
        unsigned n = (in >> 22) & 1, imms = (in >> 10) & 0x3f, rm = (in >> 16) & 31;
        if (((in >> 29) & 3) || (in & (1u << 21)) || n != sf || (!sf && imms > 31))
            return A64_ERR_UNDEFINED;
        uint64_t hi = reg(cpu, rn), lo = reg(cpu, rm), result;
        if (sf) {
            result = imms ? (lo >> imms) | (hi << (64 - imms)) : lo;
        } else {
            uint32_t h = (uint32_t)hi, l = (uint32_t)lo;
            result = imms ? (uint32_t)((l >> imms) | (h << (32 - imms))) : l;
        }
        set_reg(cpu, rd, sf, result);
        return A64_OK;
    }
    default:
        return A64_ERR_UNDEFINED;
    }
}

static int system_instruction(A64 *cpu, uint32_t in) {
    /* 1101 0101 00 L op0 op1 CRn CRm op2 Rt */
    unsigned l = (in >> 21) & 1, op0 = (in >> 19) & 3, op1 = (in >> 16) & 7;
    unsigned crn = (in >> 12) & 15, crm = (in >> 8) & 15, op2 = (in >> 5) & 7, rt = in & 31;
    if (op0 == 0 && crn == 2 && rt == 31) /* hints: NOP, PAC*, AUT*, XPACLRI, BTI */
        return A64_OK;
    if (op0 == 0 && crn == 3) /* barriers, CLREX */
        return A64_OK;
    if (op0 == 3 && op1 == 3 && crn == 13 && crm == 0 && (op2 == 2 || op2 == 3)) {
        uint64_t *where = op2 == 3 ? &cpu->tpidrro_el0 : &cpu->tpidr_el0;
        if (l)
            set_reg(cpu, rt, 1, *where);
        else if (op2 == 2)
            *where = reg(cpu, rt);
        else
            return A64_ERR_UNDEFINED;
        return A64_OK;
    }
    if (l && op0 == 3 && op1 == 3 && crn == 14 && crm == 0 && (op2 == 1 || op2 == 2)) { /* CNTPCT/CNTVCT */
        set_reg(cpu, rt, 1, cpu->steps);
        return A64_OK;
    }
    return A64_ERR_UNDEFINED;
}

/* Returns A64_OK with *branched set when the pc was changed. */
static int branches(A64 *cpu, uint32_t in, int *branched) {
    if ((in & 0x7c000000u) == 0x14000000u) { /* B, BL */
        uint64_t offset = sign_extend(in & 0x3ffffff, 26) << 2;
        if (in >> 31)
            cpu->x[30] = cpu->pc + 4;
        cpu->pc += offset;
        *branched = 1;
        return A64_OK;
    }
    if ((in & 0x7e000000u) == 0x34000000u) { /* CBZ, CBNZ */
        unsigned sf = in >> 31, rt = in & 31;
        uint64_t v = reg(cpu, rt);
        if (!sf)
            v = (uint32_t)v;
        if ((v == 0) != ((in >> 24) & 1)) {
            cpu->pc += sign_extend((in >> 5) & 0x7ffff, 19) << 2;
            *branched = 1;
        }
        return A64_OK;
    }
    if ((in & 0x7e000000u) == 0x36000000u) { /* TBZ, TBNZ */
        unsigned bit = ((in >> 31) << 5) | ((in >> 19) & 31), rt = in & 31;
        unsigned set = (unsigned)(reg(cpu, rt) >> bit) & 1;
        if (set == ((in >> 24) & 1)) {
            cpu->pc += sign_extend((in >> 5) & 0x3fff, 14) << 2;
            *branched = 1;
        }
        return A64_OK;
    }
    if ((in & 0xff000010u) == 0x54000000u) { /* B.cond */
        if (condition(cpu->nzcv, in & 15)) {
            cpu->pc += sign_extend((in >> 5) & 0x7ffff, 19) << 2;
            *branched = 1;
        }
        return A64_OK;
    }
    if ((in & 0xffe0001fu) == 0xd4000001u) { /* SVC */
        if (!cpu->svc)
            return A64_ERR_SVC;
        return cpu->svc(cpu, (in >> 5) & 0xffff, cpu->user);
    }
    if ((in & 0xffc00000u) == 0xd5000000u)
        return system_instruction(cpu, in);
    if ((in & 0xfe000000u) == 0xd6000000u) {
        /* BR, BLR, RET and their pointer-authentication forms; authentication is off. */
        unsigned opc = (in >> 21) & 15, op2 = (in >> 16) & 31, op3 = (in >> 10) & 63, rn = (in >> 5) & 31;
        unsigned kind = opc & 3;
        if (op2 != 31 || kind > 2 || (opc & 4))
            return A64_ERR_UNDEFINED;
        if (!(op3 == 0 || op3 == 2 || op3 == 3))
            return A64_ERR_UNDEFINED;
        uint64_t target = reg(cpu, rn);
        if (kind == 2 && op3 != 0) /* RETAA, RETAB */
            target = cpu->x[30];
        if (kind == 1)
            cpu->x[30] = cpu->pc + 4;
        cpu->pc = target;
        *branched = 1;
        return A64_OK;
    }
    return A64_ERR_UNDEFINED;
}

static int load_store_exclusive(A64 *cpu, uint32_t in) {
    /* size 001000 o2 L o1 Rs o0 Rt2 Rn Rt */
    unsigned size = in >> 30, o2 = (in >> 23) & 1, l = (in >> 22) & 1, o1 = (in >> 21) & 1;
    unsigned rs = (in >> 16) & 31, rt2 = (in >> 10) & 31, rn = (in >> 5) & 31, rt = in & 31;
    unsigned bytes = 1u << size;
    uint64_t address = cpu->x[rn], v;
    int rc;
    if (o2 && o1) { /* CAS, CASA, CASL, CASAL */
        if (rt2 != 31)
            return A64_ERR_UNDEFINED;
        if ((rc = load(cpu, address, bytes, &v)) != A64_OK)
            return rc;
        uint64_t expected = reg(cpu, rs);
        if (bytes < 8)
            expected &= ((uint64_t)1 << (8 * bytes)) - 1;
        if (v == expected && (rc = store(cpu, address, bytes, reg(cpu, rt))) != A64_OK)
            return rc;
        set_reg(cpu, rs, 1, v);
        return A64_OK;
    }
    if (o1) /* pairs */
        return A64_ERR_UNDEFINED;
    if (l) { /* LDXR, LDAXR, LDAR, LDLAR */
        if ((rc = load(cpu, address, bytes, &v)) != A64_OK)
            return rc;
        set_reg(cpu, rt, 1, v);
        return A64_OK;
    }
    if ((rc = store(cpu, address, bytes, reg(cpu, rt))) != A64_OK)
        return rc;
    if (!o2) /* STXR, STLXR: single-threaded, so it always succeeds */
        set_reg(cpu, rs, 0, 0);
    return A64_OK;
}

static int load_store(A64 *cpu, uint32_t in) {
    unsigned rt = in & 31, rn = (in >> 5) & 31;
    int rc;
    uint64_t v;

    if ((in & 0x3f000000u) == 0x08000000u)
        return load_store_exclusive(cpu, in);

    if (in & (1u << 26)) /* SIMD and floating point */
        return A64_ERR_UNDEFINED;

    if ((in & 0x3b000000u) == 0x18000000u) { /* load literal */
        unsigned opc = in >> 30;
        uint64_t address = cpu->pc + (sign_extend((in >> 5) & 0x7ffff, 19) << 2);
        if (opc == 3)
            return A64_OK; /* PRFM */
        if ((rc = load(cpu, address, opc == 1 ? 8 : 4, &v)) != A64_OK)
            return rc;
        if (opc == 2)
            v = sign_extend(v, 32);
        set_reg(cpu, rt, 1, v);
        return A64_OK;
    }

    if ((in & 0x3a000000u) == 0x28000000u) { /* LDP, STP */
        unsigned opc = in >> 30, mode = (in >> 23) & 3, l = (in >> 22) & 1, rt2 = (in >> 10) & 31;
        if (opc == 3 || (opc == 1 && !l))
            return A64_ERR_UNDEFINED;
        unsigned bytes = opc == 2 ? 8 : 4;
        uint64_t offset = sign_extend((in >> 15) & 0x7f, 7) * bytes;
        uint64_t base = cpu->x[rn], address = mode == 1 ? base : base + offset;
        if (l) {
            uint64_t a, b;
            if ((rc = load(cpu, address, bytes, &a)) != A64_OK || (rc = load(cpu, address + bytes, bytes, &b)) != A64_OK)
                return rc;
            if (opc == 1) {
                a = sign_extend(a, 32);
                b = sign_extend(b, 32);
            }
            set_reg(cpu, rt, 1, a);
            set_reg(cpu, rt2, 1, b);
        } else {
            if ((rc = store(cpu, address, bytes, reg(cpu, rt))) != A64_OK ||
                (rc = store(cpu, address + bytes, bytes, reg(cpu, rt2))) != A64_OK)
                return rc;
        }
        if (mode == 1 || mode == 3)
            cpu->x[rn] = base + offset;
        return A64_OK;
    }

    if ((in & 0x3a000000u) == 0x38000000u) { /* single register */
        unsigned size = in >> 30, opc = (in >> 22) & 3, bytes = 1u << size;
        uint64_t base = cpu->x[rn], address;
        int writeback = 0;
        uint64_t new_base = 0;
        if (in & (1u << 24)) { /* unsigned scaled offset */
            address = base + (uint64_t)((in >> 10) & 0xfff) * bytes;
        } else if (!(in & (1u << 21))) {
            uint64_t imm9 = sign_extend((in >> 12) & 0x1ff, 9);
            switch ((in >> 10) & 3) {
            case 0: address = base + imm9; break;                       /* unscaled */
            case 1: address = base; writeback = 1; new_base = base + imm9; break; /* post-index */
            case 3: address = base + imm9; writeback = 1; new_base = address; break; /* pre-index */
            default: return A64_ERR_UNDEFINED;
            }
        } else {
            unsigned low = (in >> 10) & 3;
            if (low == 2) { /* register offset */
                unsigned option = (in >> 13) & 7, s = (in >> 12) & 1, rm = (in >> 16) & 31;
                if (!(option & 2))
                    return A64_ERR_UNDEFINED;
                address = base + extend_value(reg(cpu, rm), option, s ? size : 0);
            } else if (low == 0 && ((in >> 12) & 0xf) == 0xc && ((in >> 16) & 31) == 31 && opc == 2) {
                /* LDAPR, LDAPRB, LDAPRH */
                if ((rc = load(cpu, base, bytes, &v)) != A64_OK)
                    return rc;
                set_reg(cpu, rt, 1, v);
                return A64_OK;
            } else {
                return A64_ERR_UNDEFINED;
            }
        }
        if (opc == 0) {
            if ((rc = store(cpu, address, bytes, reg(cpu, rt))) != A64_OK)
                return rc;
        } else {
            if (size == 3 && opc == 2) { /* PRFM */
                return A64_OK;
            }
            if ((size == 3 && opc == 3) || (size == 2 && opc == 3))
                return A64_ERR_UNDEFINED;
            if ((rc = load(cpu, address, bytes, &v)) != A64_OK)
                return rc;
            if (opc == 2)
                v = sign_extend(v, 8 * bytes);
            else if (opc == 3)
                v = (uint32_t)sign_extend(v, 8 * bytes);
            set_reg(cpu, rt, 1, v);
        }
        if (writeback)
            cpu->x[rn] = new_base;
        return A64_OK;
    }
    return A64_ERR_UNDEFINED;
}

static int data_processing_register(A64 *cpu, uint32_t in) {
    unsigned sf = in >> 31, rd = in & 31, rn = (in >> 5) & 31, rm = (in >> 16) & 31;
    unsigned width = sf ? 64 : 32;

    if (!(in & (1u << 28))) {
        if (!(in & (1u << 24))) { /* logical, shifted register */
            unsigned opc = (in >> 29) & 3, imm6 = (in >> 10) & 0x3f;
            if (!sf && imm6 > 31)
                return A64_ERR_UNDEFINED;
            uint64_t b = shift_value(reg(cpu, rm), (in >> 22) & 3, imm6, sf);
            if (in & (1u << 21))
                b = ~b;
            uint64_t a = reg(cpu, rn), result;
            if (opc == 1)
                result = a | b;
            else if (opc == 2)
                result = a ^ b;
            else
                result = a & b;
            if (opc == 3)
                cpu->nzcv = logic_flags(sf, result);
            set_reg(cpu, rd, sf, result);
            return A64_OK;
        }
        /* ADD, SUB: shifted or extended register */
        unsigned op = (in >> 30) & 1, s = (in >> 29) & 1;
        uint64_t a, b;
        int extended = (in >> 21) & 1;
        if (extended) {
            if ((in >> 22) & 3)
                return A64_ERR_UNDEFINED;
            unsigned imm3 = (in >> 10) & 7;
            if (imm3 > 4)
                return A64_ERR_UNDEFINED;
            a = cpu->x[rn];
            b = extend_value(reg(cpu, rm), (in >> 13) & 7, imm3);
        } else {
            unsigned imm6 = (in >> 10) & 0x3f, type = (in >> 22) & 3;
            if (type == 3 || (!sf && imm6 > 31))
                return A64_ERR_UNDEFINED;
            a = reg(cpu, rn);
            b = shift_value(reg(cpu, rm), type, imm6, sf);
        }
        uint32_t flags;
        uint64_t result = op ? add_with_carry(sf, a, ~b, 1, &flags) : add_with_carry(sf, a, b, 0, &flags);
        if (s)
            cpu->nzcv = flags;
        if (extended && !s)
            set_reg_sp(cpu, rd, sf, result);
        else
            set_reg(cpu, rd, sf, result);
        return A64_OK;
    }

    if (in & (1u << 24)) { /* three sources */
        unsigned op31 = (in >> 21) & 7, o0 = (in >> 15) & 1, ra = (in >> 10) & 31;
        if ((in >> 29) & 3)
            return A64_ERR_UNDEFINED;
        uint64_t a = reg(cpu, rn), b = reg(cpu, rm), c = reg(cpu, ra), result;
        switch (op31) {
        case 0:
            result = o0 ? c - a * b : c + a * b;
            set_reg(cpu, rd, sf, result);
            return A64_OK;
        case 1: /* SMADDL, SMSUBL */
        case 5: /* UMADDL, UMSUBL */
            if (!sf)
                return A64_ERR_UNDEFINED;
            if (op31 == 1)
                result = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
            else
                result = (uint64_t)(uint32_t)a * (uint32_t)b;
            set_reg(cpu, rd, 1, o0 ? c - result : c + result);
            return A64_OK;
        case 2:
        case 6:
            if (!sf || o0)
                return A64_ERR_UNDEFINED;
            set_reg(cpu, rd, 1, op31 == 2 ? smulh((int64_t)a, (int64_t)b) : umulh(a, b));
            return A64_OK;
        default:
            return A64_ERR_UNDEFINED;
        }
    }

    switch ((in >> 21) & 15) {
    case 0: { /* ADC, SBC */
        if ((in >> 10) & 0x3f)
            return A64_ERR_UNDEFINED;
        unsigned op = (in >> 30) & 1, s = (in >> 29) & 1, carry = !!(cpu->nzcv & C_FLAG);
        uint64_t b = reg(cpu, rm);
        uint32_t flags;
        uint64_t result = add_with_carry(sf, reg(cpu, rn), op ? ~b : b, carry, &flags);
        if (s)
            cpu->nzcv = flags;
        set_reg(cpu, rd, sf, result);
        return A64_OK;
    }
    case 2: { /* CCMN, CCMP */
        if (!((in >> 29) & 1) || (in & (1u << 10)) || (in & (1u << 4)))
            return A64_ERR_UNDEFINED;
        unsigned op = (in >> 30) & 1;
        uint64_t b = (in & (1u << 11)) ? rm : reg(cpu, rm);
        if (condition(cpu->nzcv, (in >> 12) & 15)) {
            uint32_t flags;
            if (op)
                add_with_carry(sf, reg(cpu, rn), ~b, 1, &flags);
            else
                add_with_carry(sf, reg(cpu, rn), b, 0, &flags);
            cpu->nzcv = flags;
        } else {
            cpu->nzcv = in & 15;
        }
        return A64_OK;
    }
    case 4: { /* CSEL, CSINC, CSINV, CSNEG */
        unsigned op = (in >> 30) & 1, op2 = (in >> 10) & 3;
        if (((in >> 29) & 1) || op2 > 1)
            return A64_ERR_UNDEFINED;
        uint64_t result;
        if (condition(cpu->nzcv, (in >> 12) & 15)) {
            result = reg(cpu, rn);
        } else {
            result = reg(cpu, rm);
            if (op)
                result = ~result;
            if (op2)
                result += 1;
        }
        set_reg(cpu, rd, sf, result);
        return A64_OK;
    }
    case 6: {
        unsigned opcode = (in >> 10) & 0x3f;
        if ((in >> 29) & 1)
            return A64_ERR_UNDEFINED;
        if (in & (1u << 30)) { /* one source */
            uint64_t a = reg(cpu, rn), result;
            if (rm == 1) { /* pointer authentication: keys are off, so these change nothing */
                if (!sf)
                    return A64_ERR_UNDEFINED;
                return A64_OK;
            }
            if (rm != 0)
                return A64_ERR_UNDEFINED;
            if (!sf)
                a = (uint32_t)a;
            switch (opcode) {
            case 0: result = reverse_bits(a, width); break;
            case 1: result = reverse_bytes(a, width / 8, 2); break;
            case 2: result = sf ? reverse_bytes(a, 8, 4) : reverse_bytes(a, 4, 4); break;
            case 3:
                if (!sf)
                    return A64_ERR_UNDEFINED;
                result = reverse_bytes(a, 8, 8);
                break;
            case 4: result = count_leading(a, width, 0); break;
            case 5: result = count_leading(a, width, 1); break;
            default: return A64_ERR_UNDEFINED;
            }
            set_reg(cpu, rd, sf, result);
            return A64_OK;
        }
        uint64_t a = reg(cpu, rn), b = reg(cpu, rm), result;
        if (!sf) {
            a = (uint32_t)a;
            b = (uint32_t)b;
        }
        switch (opcode) {
        case 2: result = b ? a / b : 0; break;
        case 3:
            if (!b)
                result = 0;
            else if (sf)
                result = (a == (uint64_t)1 << 63 && b == ~(uint64_t)0) ? a : (uint64_t)((int64_t)a / (int64_t)b);
            else
                result = ((uint32_t)a == 0x80000000u && (uint32_t)b == 0xffffffffu)
                             ? a : (uint32_t)((int32_t)(uint32_t)a / (int32_t)(uint32_t)b);
            break;
        case 8:
        case 9:
        case 10:
        case 11: result = shift_value(a, opcode - 8, (unsigned)(b % width), sf); break;
        default: return A64_ERR_UNDEFINED;
        }
        set_reg(cpu, rd, sf, result);
        return A64_OK;
    }
    default:
        return A64_ERR_UNDEFINED;
    }
}

int a64_run(A64 *cpu) {
    for (;;) {
        A64Region *code = find_region(cpu, cpu->pc, 4, &cpu->last_code);
        if (!code || (cpu->pc & 3)) {
            cpu->fault_address = cpu->pc;
            cpu->fault_pc = cpu->pc;
            return A64_ERR_MEMORY;
        }
        const uint8_t *p = code->host + (cpu->pc - code->base);
        uint32_t in = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        int rc, branched = 0;

        if (++cpu->steps > cpu->max_steps) {
            cpu->fault_pc = cpu->pc;
            return A64_ERR_STEPS;
        }
        switch ((in >> 25) & 15) {
        case 8:
        case 9: rc = data_processing_immediate(cpu, in); break;
        case 10:
        case 11: rc = branches(cpu, in, &branched); break;
        case 4:
        case 6:
        case 12:
        case 14: rc = load_store(cpu, in); break;
        case 5:
        case 13: rc = data_processing_register(cpu, in); break;
        default: rc = A64_ERR_UNDEFINED; break;
        }
        if (rc != A64_OK) {
            cpu->fault_pc = cpu->pc;
            cpu->fault_instruction = in;
            if (rc == A64_HALT)
                cpu->pc += 4;
            return rc;
        }
        if (!branched)
            cpu->pc += 4;
    }
}
