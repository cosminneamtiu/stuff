/* SPDX-License-Identifier: GPL-2.0-only */
/* PC command line for the patch engine that SeedSwap runs on the console. The tests use it to
 * compare the engine with the reference (compat/cmptsim.py).
 *
 *   compat_tool run --cmptldr FILE --main NSO --id HEX --flag 0|1 [--second TEXT RO DATA]
 *                   [--poke OFFSET WORD]...   (hex; tests only)
 *       prints one JSON object: status, routine, result, steps, changes
 *   compat_tool exefs --cmptldr FILE --exefs IN --id HEX [--out OUT]
 *       inspects an exefs.nsp, and with --out writes the patched image
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmpt.h"
#include "compat.h"
#include "nsofile.h"

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(length > 0 ? (size_t)length : 1);
    if (!data || fread(data, 1, (size_t)length, f) != (size_t)length) {
        fprintf(stderr, "cannot read %s\n", path);
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return data;
}

static const char *option(int argc, char **argv, const char *name) {
    for (int i = 2; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return NULL;
}

static int run(int argc, char **argv) {
    const char *cmptldr = option(argc, argv, "--cmptldr"), *main_path = option(argc, argv, "--main");
    const char *id = option(argc, argv, "--id"), *flag = option(argc, argv, "--flag");
    if (!cmptldr || !main_path || !id || !flag)
        return 2;
    size_t patcher_size, main_size;
    uint8_t *patcher = read_file(cmptldr, &patcher_size), *main_file = read_file(main_path, &main_size);
    if (!patcher || !main_file)
        return 2;
    NsoImage nso;
    char error[96];
    if (nso_load(main_file, main_size, &nso, error, sizeof(error)) < 0) {
        fprintf(stderr, "%s: %s\n", main_path, error);
        return 2;
    }
    CmptModule game = {nso.image, nso.image_size, nso.mem[0], nso.size[0], nso.mem[1], nso.size[1],
                       nso.mem[2], nso.size[2], nso.api_off, nso.api_size};
    CmptModule second, *second_pointer = NULL;
    for (int i = 2; i + 3 < argc; i++) {
        if (strcmp(argv[i], "--second"))
            continue;
        size_t sizes[3];
        uint8_t *parts[3];
        for (int k = 0; k < 3; k++)
            if (!(parts[k] = read_file(argv[i + 1 + k], &sizes[k])))
                return 2;
        memset(&second, 0, sizeof(second));
        second.image_size = (sizes[0] + sizes[1] + sizes[2] + 0xfff) & ~(uint64_t)0xfff;
        second.image = calloc(1, (size_t)second.image_size);
        memcpy(second.image, parts[0], sizes[0]);
        memcpy(second.image + sizes[0], parts[1], sizes[1]);
        memcpy(second.image + sizes[0] + sizes[1], parts[2], sizes[2]);
        second.text_size = (uint32_t)sizes[0];
        second.ro_off = (uint32_t)sizes[0];
        second.ro_size = (uint32_t)sizes[1];
        second.data_off = (uint32_t)(sizes[0] + sizes[1]);
        second.data_size = (uint32_t)sizes[2];
        second_pointer = &second;
    }
    uint32_t pokes[32];
    size_t poke_count = 0;
    for (int i = 2; i + 2 < argc && poke_count < 16; i++) {
        if (strcmp(argv[i], "--poke"))
            continue;
        pokes[2 * poke_count] = (uint32_t)strtoul(argv[i + 1], NULL, 16);
        pokes[2 * poke_count + 1] = (uint32_t)strtoul(argv[i + 2], NULL, 16);
        poke_count++;
    }
    CmptResult result;
    int status = cmpt_run_poked(patcher, patcher_size, strtoull(id, NULL, 16), atoi(flag), &game, second_pointer,
                                pokes, poke_count, &result);
    printf("{\"status\": %d, \"message\": \"%s\", \"version\": \"%s\", \"result\": %u, \"routine\": %u, "
           "\"steps\": %llu, \"changes\": [",
           status, result.message, result.version, result.patcher_result, result.routine,
           (unsigned long long)result.steps);
    for (size_t i = 0; i < result.change_count; i++)
        printf("%s[%u, %u, %u, %u]", i ? ", " : "", result.changes[i].module, result.changes[i].offset,
               result.changes[i].old_word, result.changes[i].new_word);
    printf("]}\n");
    cmpt_free(&result);
    return 0;
}

static int exefs(int argc, char **argv) {
    const char *cmptldr = option(argc, argv, "--cmptldr"), *in = option(argc, argv, "--exefs");
    const char *id = option(argc, argv, "--id"), *out_path = option(argc, argv, "--out");
    if (!cmptldr || !in || !id)
        return 2;
    size_t patcher_size, image_size;
    uint8_t *patcher = read_file(cmptldr, &patcher_size), *image = read_file(in, &image_size);
    if (!patcher || !image)
        return 2;
    uint8_t *out = NULL;
    CompatReport report;
    int status = compat_patch_exefs(patcher, patcher_size, image, image_size, strtoull(id, NULL, 16),
                                    out_path != NULL, &out, &report);
    printf("{\"status\": %d, \"message\": \"%s\", \"version\": \"%s\", \"width\": %d, \"routine\": %u, "
           "\"changes\": %zu, \"steps\": %llu, \"written\": %s}\n",
           status, report.message, report.version, report.width, report.routine, report.change_count,
           (unsigned long long)report.steps, out ? "true" : "false");
    if (out) {
        FILE *f = fopen(out_path, "wb");
        if (!f || fwrite(out, 1, image_size, f) != image_size) {
            fprintf(stderr, "cannot write %s\n", out_path);
            return 2;
        }
        fclose(f);
    }
    return status == CMPT_OK ? 0 : 1;
}

/* Single instructions for test_a64.py. Input: cases of {u32 instruction, u32 nzcv, u64 x[32], 4096
 * bytes of memory}. Each runs for exactly one instruction with the instruction at FUZZ_CODE and the
 * memory at FUZZ_DATA. Output: {u32 status, u32 nzcv, u64 pc, u64 x[32], 4096 bytes}. */
#include "a64.h"
#define FUZZ_CODE 0x200000ull
#define FUZZ_DATA 0x100000ull
#define FUZZ_CASE (8 + 32 * 8 + 4096)

static int no_svc(A64 *cpu, uint32_t number, void *user) {
    (void)cpu;
    (void)number;
    (void)user;
    return A64_HALT;
}

static int fuzz(int argc, char **argv) {
    if (argc < 4)
        return 2;
    size_t size;
    uint8_t *in = read_file(argv[2], &size);
    if (!in)
        return 2;
    FILE *out = fopen(argv[3], "wb");
    if (!out)
        return 2;
    static uint8_t code[0x1000], data[0x1000];
    static A64 cpu;
    for (size_t at = 0; at + FUZZ_CASE <= size; at += FUZZ_CASE) {
        const uint8_t *c = in + at;
        a64_init(&cpu);
        memset(code, 0, sizeof(code));
        memcpy(code + 0x800, c, 4);
        memcpy(&cpu.nzcv, c + 4, 4);
        memcpy(cpu.x, c + 8, 32 * 8);
        memcpy(data, c + 8 + 32 * 8, 4096);
        a64_map(&cpu, FUZZ_CODE, sizeof(code), code, 0, 0, 0);
        a64_map(&cpu, FUZZ_DATA, sizeof(data), data, 1, 0, 0);
        cpu.pc = FUZZ_CODE + 0x800;
        cpu.svc = no_svc;
        cpu.max_steps = 1;
        cpu.tpidrro_el0 = 0x1234500000ull;
        uint32_t status = (uint32_t)a64_run(&cpu);
        fwrite(&status, 4, 1, out);
        fwrite(&cpu.nzcv, 4, 1, out);
        fwrite(&cpu.pc, 8, 1, out);
        fwrite(cpu.x, 8, 32, out);
        fwrite(data, 1, 4096, out);
    }
    fclose(out);
    free(in);
    return 0;
}

int main(int argc, char **argv) {
    int rc = 2;
    if (argc >= 2 && !strcmp(argv[1], "fuzz"))
        return fuzz(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "run"))
        rc = run(argc, argv);
    else if (argc >= 2 && !strcmp(argv[1], "exefs"))
        rc = exefs(argc, argv);
    if (rc == 2)
        fprintf(stderr, "usage: compat_tool run --cmptldr F --main NSO --id HEX --flag 0|1 [--second T R D]\n"
                        "       compat_tool exefs --cmptldr F --exefs IN --id HEX [--out OUT]\n");
    return rc;
}
