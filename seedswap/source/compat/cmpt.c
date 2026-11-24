/* SPDX-License-Identifier: GPL-2.0-only */
#include "cmpt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a64.h"
#include "nsofile.h"

/* What is known about one build of cmptldr: the function behind PatchProcess, its two wrappers
 * around svcMapProcessMemory / svcUnmapProcessMemory (replaced here), and the table of branches
 * to the patch routines (only to report which routine ran). */
typedef struct Patcher {
    const char *version;
    uint8_t sha256[32];
    uint32_t worker, map_helper, unmap_helper, islands, islands_end;
} Patcher;

static const Patcher PATCHERS[] = {
    {"23.0.1",
     {0x36, 0x37, 0xfe, 0xaf, 0x1b, 0xcd, 0x9e, 0x1e, 0x1d, 0xc2, 0xcd, 0x7e, 0x15, 0x53, 0x28, 0x3f,
      0x7b, 0x8c, 0x9b, 0xec, 0x2b, 0xc4, 0x13, 0xf8, 0xc7, 0xff, 0x6f, 0x9a, 0x21, 0x02, 0xd9, 0xd3},
     0x8560, 0x2bae0, 0x2ba90, 0x15b8, 0x16bc},
};

/* The guest's view of memory. */
#define CODE_BASE 0x08000000ull
#define STACK_BASE 0x0c000000ull
#define STACK_SIZE 0x100000ull
#define TLS_BASE 0x0d000000ull
#define STOP_BASE 0x0e000000ull
#define LOCAL_BASE 0x2000000000ull
#define MAIN_BASE 0x4657c05000ull

#define SVC_MAP 0x7f1
#define SVC_UNMAP 0x7f2
#define SVC_STOP 0x7ff
#define SVC_ISLAND 0x600 /* + index: a branch to a patch routine */
#define MAX_ISLANDS 128
#define MAX_WRITES 262144

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

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

const char *cmpt_status_text(int status) {
    switch (status) {
    case CMPT_OK: return "ok";
    case CMPT_UNKNOWN_PATCHER: return "the system's patcher is a version SeedSwap does not know";
    case CMPT_BAD_PATCHER: return "the system's patcher file is damaged";
    case CMPT_NO_MEMORY: return "not enough memory";
    case CMPT_ENGINE_ERROR: return "the patcher could not be run";
    case CMPT_PATCHER_FAILED: return "the patcher reported an error";
    case CMPT_TOO_MANY_CHANGES: return "the patcher changed more than SeedSwap can record";
    default: return "unknown";
    }
}

void cmpt_free(CmptResult *result) {
    free(result->changes);
    result->changes = NULL;
    result->change_count = 0;
}

/* Applies the module's relative relocations, as its own start-up code would. */
static int relocate(uint8_t *image, uint64_t size) {
    if (size < 8)
        return -1;
    uint32_t mod0 = rd32(image + 4);
    if ((uint64_t)mod0 + 8 > size || memcmp(image + mod0, "MOD0", 4))
        return -1;
    uint64_t dynamic = (uint64_t)mod0 + (int32_t)rd32(image + mod0 + 4);
    uint64_t rela = 0, rela_size = 0, relr = 0, relr_size = 0;
    int have_rela = 0, have_relr = 0;
    for (uint64_t at = dynamic; at + 16 <= size; at += 16) {
        uint64_t tag = rd64(image + at), value = rd64(image + at + 8);
        if (!tag)
            break;
        if (tag == 7) { rela = value; have_rela = 1; }
        else if (tag == 8) rela_size = value;
        else if (tag == 36) { relr = value; have_relr = 1; }
        else if (tag == 35) relr_size = value;
    }
    if (have_rela) {
        if (rela > size || rela_size > size - rela)
            return -1;
        for (uint64_t i = 0; i + 24 <= rela_size; i += 24) {
            uint64_t where = rd64(image + rela + i), info = rd64(image + rela + i + 8);
            uint64_t addend = rd64(image + rela + i + 16);
            if ((uint32_t)info == 0)
                continue;
            if ((uint32_t)info != 1027 || where > size - 8)
                return -1;
            wr64(image + where, CODE_BASE + addend);
        }
    }
    if (have_relr) {
        if (relr > size || relr_size > size - relr)
            return -1;
        uint64_t where = 0;
        for (uint64_t i = 0; i + 8 <= relr_size; i += 8) {
            uint64_t entry = rd64(image + relr + i);
            if (!(entry & 1)) {
                if (entry > size - 8)
                    return -1;
                wr64(image + entry, rd64(image + entry) + CODE_BASE);
                where = entry + 8;
            } else {
                for (unsigned bit = 0; bit < 63; bit++) {
                    if ((entry >> (bit + 1)) & 1) {
                        uint64_t at = where + 8 * bit;
                        if (at > size - 8)
                            return -1;
                        wr64(image + at, rd64(image + at) + CODE_BASE);
                    }
                }
                where += 8 * 63;
            }
        }
    }
    return 0;
}

typedef struct Run {
    CmptModule *modules[2];
    uint64_t bases[2];
    uint64_t next_local;
    uint32_t routine;
    const Patcher *patcher;
    uint32_t island_target[MAX_ISLANDS];
    char note[96];
} Run;

static int on_svc(A64 *cpu, uint32_t number, void *user) {
    Run *run = user;
    if (number >= SVC_ISLAND && number < SVC_ISLAND + MAX_ISLANDS && run->island_target[number - SVC_ISLAND]) {
        /* A branch into the routine table: note which routine, then take the branch. */
        uint32_t target = run->island_target[number - SVC_ISLAND];
        if (!run->routine)
            run->routine = target;
        cpu->pc = CODE_BASE + target - 4;
        return A64_OK;
    }
    switch (number) {
    case SVC_STOP:
        return A64_HALT;
    case SVC_MAP: { /* (out pointer, handle, remote address, size) */
        uint64_t remote = cpu->x[2], size = cpu->x[3];
        for (int i = 0; i < 2; i++) {
            CmptModule *m = run->modules[i];
            if (!m || !size || remote < run->bases[i])
                continue;
            uint64_t offset = remote - run->bases[i];
            if (offset > m->image_size || size > m->image_size - offset)
                continue;
            uint64_t local = run->next_local;
            run->next_local += ((size + 0x1fffff) & ~0x1fffffull) + 0x200000;
            if (a64_map(cpu, local, size, m->image + offset, 1, 1, i) < 0)
                return A64_ERR_REGIONS;
            uint8_t *out = a64_pointer(cpu, cpu->x[0], 8, 1);
            if (!out)
                return A64_ERR_MEMORY;
            wr64(out, local);
            cpu->x[0] = 0;
            return A64_OK;
        }
        snprintf(run->note, sizeof(run->note), "map of unknown memory 0x%llx+0x%llx", (unsigned long long)remote,
                 (unsigned long long)size);
        cpu->x[0] = 0xd401;
        return A64_OK;
    }
    case SVC_UNMAP:
    case 0x75: /* the mapping aliases the image, so there is nothing to copy back */
    case 0x16: /* CloseHandle on the process handle */
        cpu->x[0] = 0;
        return A64_OK;
    default:
        snprintf(run->note, sizeof(run->note), "system call 0x%x", number);
        return A64_ERR_SVC;
    }
}

static int compare_changes(const void *a, const void *b) {
    const CmptChange *x = a, *y = b;
    if (x->module != y->module)
        return x->module < y->module ? -1 : 1;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/* Turns the store log into a sorted list of changed words, and can undo the stores. */
static int collect(A64 *cpu, Run *run, CmptResult *result, int undo_only) {
    size_t count = cpu->write_count;
    CmptChange *words = NULL;
    size_t word_count = 0;
    if (!undo_only && count) {
        words = malloc(sizeof(*words) * count * 3);
        if (!words)
            return CMPT_NO_MEMORY;
    }
    /* First pass: which words were touched, with the values they have now. */
    for (size_t i = 0; i < count && !undo_only; i++) {
        A64Write *w = &cpu->writes[i];
        for (int r = 0; r < cpu->region_count; r++) {
            A64Region *region = &cpu->regions[r];
            if (!region->track || w->address - region->base >= region->size)
                continue;
            CmptModule *m = run->modules[region->tag];
            uint64_t first = (uint64_t)(region->host - m->image) + (w->address - region->base);
            for (uint64_t at = first & ~3ull; at < first + w->size; at += 4) {
                words[word_count].module = (uint32_t)region->tag;
                words[word_count].offset = (uint32_t)at;
                words[word_count].new_word = at + 4 <= m->image_size ? rd32(m->image + at) : 0;
                words[word_count].old_word = 0;
                word_count++;
            }
            break;
        }
    }
    /* Undo every store, newest first: the images are back to what they were. */
    for (size_t i = count; i-- > 0;) {
        A64Write *w = &cpu->writes[i];
        uint8_t *p = a64_pointer(cpu, w->address, w->size, 1);
        if (p)
            for (unsigned b = 0; b < w->size; b++)
                p[b] = (uint8_t)(w->old_value >> (8 * b));
    }
    if (undo_only)
        return CMPT_OK;
    /* Second pass: the old values, then put the new ones back and drop words that did not change. */
    qsort(words, word_count, sizeof(*words), compare_changes);
    size_t kept = 0;
    for (size_t i = 0; i < word_count; i++) {
        if (kept && words[kept - 1].module == words[i].module && words[kept - 1].offset == words[i].offset)
            continue;
        CmptModule *m = run->modules[words[i].module];
        if ((uint64_t)words[i].offset + 4 > m->image_size)
            continue;
        words[i].old_word = rd32(m->image + words[i].offset);
        if (words[i].old_word != words[i].new_word)
            words[kept++] = words[i];
    }
    for (size_t i = 0; i < kept; i++)
        wr32(run->modules[words[i].module]->image + words[i].offset, words[i].new_word);
    result->changes = words;
    result->change_count = kept;
    if (!kept) {
        free(words);
        result->changes = NULL;
    }
    return CMPT_OK;
}

int cmpt_run(const uint8_t *patcher_file, size_t patcher_size, uint64_t program_id, int is64, CmptModule *main,
             CmptModule *second, CmptResult *result) {
    return cmpt_run_poked(patcher_file, patcher_size, program_id, is64, main, second, NULL, 0, result);
}

int cmpt_run_poked(const uint8_t *patcher_file, size_t patcher_size, uint64_t program_id, int is64, CmptModule *main,
                   CmptModule *second, const uint32_t *pokes, size_t poke_count, CmptResult *result) {
    memset(result, 0, sizeof(*result));
    uint8_t digest[32];
    sha256(patcher_file, patcher_size, digest);
    const Patcher *patcher = NULL;
    for (size_t i = 0; i < sizeof(PATCHERS) / sizeof(PATCHERS[0]); i++)
        if (!memcmp(digest, PATCHERS[i].sha256, 32))
            patcher = &PATCHERS[i];
    if (!patcher) {
        snprintf(result->message, sizeof(result->message), "%s", cmpt_status_text(CMPT_UNKNOWN_PATCHER));
        return result->status = CMPT_UNKNOWN_PATCHER;
    }
    snprintf(result->version, sizeof(result->version), "%s", patcher->version);

    NsoImage nso;
    char error[96];
    if (nso_load(patcher_file, patcher_size, &nso, error, sizeof(error)) < 0) {
        snprintf(result->message, sizeof(result->message), "%s", error);
        return result->status = CMPT_BAD_PATCHER;
    }
    uint8_t *stack = calloc(1, STACK_SIZE);
    uint8_t *tls = calloc(1, 0x1000);
    A64Write *writes = malloc(sizeof(A64Write) * MAX_WRITES);
    uint8_t stop[8];
    int status = CMPT_NO_MEMORY;
    A64 *cpu = malloc(sizeof(A64));
    if (!stack || !tls || !writes || !cpu)
        goto done;
    if (relocate(nso.image, nso.image_size) < 0 || patcher->map_helper + 8 > nso.image_size ||
        patcher->unmap_helper + 8 > nso.image_size || patcher->worker + 4 > nso.image_size) {
        status = CMPT_BAD_PATCHER;
        snprintf(result->message, sizeof(result->message), "%s", cmpt_status_text(status));
        goto done;
    }
    wr32(nso.image + patcher->map_helper, 0xd4000001u | SVC_MAP << 5);
    wr32(nso.image + patcher->map_helper + 4, 0xd65f03c0u);
    wr32(nso.image + patcher->unmap_helper, 0xd4000001u | SVC_UNMAP << 5);
    wr32(nso.image + patcher->unmap_helper + 4, 0xd65f03c0u);
    wr32(stop, 0xd4000001u | SVC_STOP << 5);
    wr32(stop + 4, 0x14000000u);

    for (size_t i = 0; i < poke_count; i++)
        if ((uint64_t)pokes[2 * i] + 4 <= nso.image_size)
            wr32(nso.image + pokes[2 * i], pokes[2 * i + 1]);

    Run run;
    memset(&run, 0, sizeof(run));
    run.patcher = patcher;
    /* Each entry of the routine table is a plain branch; route it through on_svc. */
    for (uint32_t at = patcher->islands, i = 0; at < patcher->islands_end && i < MAX_ISLANDS; at += 4, i++) {
        uint32_t word = rd32(nso.image + at);
        if (at + 4 > nso.image_size || word >> 26 != 5)
            continue;
        int32_t offset = (int32_t)(word << 6) >> 6;
        run.island_target[i] = at + (uint32_t)(offset * 4);
        wr32(nso.image + at, 0xd4000001u | (SVC_ISLAND + i) << 5);
    }
    run.modules[0] = main;
    run.modules[1] = second;
    run.bases[0] = MAIN_BASE;
    run.bases[1] = MAIN_BASE + ((main->image_size + 0xfff) & ~0xfffull);
    run.next_local = LOCAL_BASE;

    a64_init(cpu);
    cpu->svc = on_svc;
    cpu->user = &run;
    cpu->writes = writes;
    cpu->write_capacity = MAX_WRITES;
    cpu->tpidrro_el0 = TLS_BASE;
    a64_map(cpu, CODE_BASE, nso.image_size, nso.image, 1, 0, 0);
    a64_map(cpu, STACK_BASE, STACK_SIZE, stack, 1, 0, 0);
    a64_map(cpu, TLS_BASE, 0x1000, tls, 1, 0, 0);
    a64_map(cpu, STOP_BASE, sizeof(stop), stop, 0, 0, 0);

    /* The arguments, in the order the PatchProcess handler unpacks its request. */
    uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
    uint8_t *args = stack + (sp - STACK_BASE);
    wr64(args + 0x00, main->ro_size);
    wr64(args + 0x08, main->data_off);
    wr64(args + 0x10, main->data_size);
    wr64(args + 0x18, main->api_off);
    wr64(args + 0x20, main->api_size);
    if (second) {
        wr64(args + 0x28, run.bases[1]);
        wr64(args + 0x30, second->text_off);
        wr64(args + 0x38, second->text_size);
        wr64(args + 0x40, second->ro_off);
        wr64(args + 0x48, second->ro_size);
        wr64(args + 0x50, second->data_off);
        wr64(args + 0x58, second->data_size);
    }
    wr64(args + 0x60, is64 ? 1 : 0);
    cpu->x[0] = 0;
    cpu->x[1] = 0xbeef; /* the process handle */
    cpu->x[2] = program_id;
    cpu->x[3] = 0;
    cpu->x[4] = run.bases[0];
    cpu->x[5] = main->text_off;
    cpu->x[6] = main->text_size;
    cpu->x[7] = main->ro_off;
    cpu->x[31] = sp;
    cpu->x[30] = STOP_BASE;
    cpu->pc = CODE_BASE + patcher->worker;

    int rc = a64_run(cpu);
    result->steps = cpu->steps;
    result->routine = run.routine;
    result->patcher_result = (uint32_t)cpu->x[0];

    if (rc != A64_HALT) {
        status = CMPT_ENGINE_ERROR;
        snprintf(result->message, sizeof(result->message), "%s at cmptldr+0x%llx (instruction %08x, address 0x%llx)%s%s",
                 a64_error_text(rc), (unsigned long long)(cpu->fault_pc - CODE_BASE), cpu->fault_instruction,
                 (unsigned long long)cpu->fault_address, run.note[0] ? "; " : "", run.note);
        collect(cpu, &run, result, 1);
    } else if (cpu->write_overflow) {
        status = CMPT_TOO_MANY_CHANGES;
        snprintf(result->message, sizeof(result->message), "%s", cmpt_status_text(status));
        collect(cpu, &run, result, 1);
    } else if (result->patcher_result) {
        status = CMPT_PATCHER_FAILED;
        snprintf(result->message, sizeof(result->message), "cmptldr returned 0x%x%s%s", result->patcher_result,
                 run.note[0] ? "; " : "", run.note);
        collect(cpu, &run, result, 1);
    } else {
        status = collect(cpu, &run, result, 0);
        if (status != CMPT_OK)
            snprintf(result->message, sizeof(result->message), "%s", cmpt_status_text(status));
    }

done:
    free(cpu);
    free(writes);
    free(tls);
    free(stack);
    nso_free(&nso);
    return result->status = status;
}
