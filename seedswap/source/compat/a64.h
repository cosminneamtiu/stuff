/* SPDX-License-Identifier: GPL-2.0-only */
/* A small AArch64 interpreter: the integer instruction set, enough to run the
 * system's Switch 1 compatibility patcher on a game image (see cmpt.h). Guest
 * memory is a short list of regions; every access is bounds-checked, and
 * anything the interpreter does not know stops the run with an error. */
#ifndef SEEDSWAP_A64_H
#define SEEDSWAP_A64_H

#include <stddef.h>
#include <stdint.h>

#define A64_MAX_REGIONS 24

enum {
    A64_OK = 0,          /* still running (internal) */
    A64_HALT = 1,        /* the svc handler asked to stop */
    A64_ERR_UNDEFINED,   /* an instruction the interpreter does not implement */
    A64_ERR_MEMORY,      /* an access outside every region, or a write to a read-only one */
    A64_ERR_STEPS,       /* more instructions than max_steps */
    A64_ERR_SVC,         /* the svc handler refused the call */
    A64_ERR_REGIONS      /* no room for another region */
};

typedef struct A64Region {
    uint64_t base;
    uint64_t size;
    uint8_t *host;
    int writable;
    int track;           /* log every store into this region */
    int tag;             /* free for the user of the region */
} A64Region;

/* One logged store: where, and what was there before. */
typedef struct A64Write {
    uint64_t address;
    uint64_t old_value;
    uint8_t size;
} A64Write;

struct A64;
/* Returns A64_OK to continue, A64_HALT to stop, or an error. */
typedef int (*A64Svc)(struct A64 *cpu, uint32_t number, void *user);

typedef struct A64 {
    uint64_t x[32];      /* x[31] is the stack pointer */
    uint64_t pc;
    uint32_t nzcv;       /* N=8 Z=4 C=2 V=1 */
    uint64_t tpidrro_el0;
    uint64_t tpidr_el0;

    A64Region regions[A64_MAX_REGIONS];
    int region_count;
    int last_data;       /* region that served the last data access */
    int last_code;

    A64Write *writes;    /* store log for tracked regions */
    size_t write_count, write_capacity;
    int write_overflow;

    A64Svc svc;
    void *user;

    uint64_t steps, max_steps;
    uint64_t fault_address;  /* for A64_ERR_MEMORY */
    uint64_t fault_pc;
    uint32_t fault_instruction;
} A64;

void a64_init(A64 *cpu);
/* Adds a region and returns its index, or -1. */
int a64_map(A64 *cpu, uint64_t base, uint64_t size, uint8_t *host, int writable, int track, int tag);
/* Runs from cpu->pc until the svc handler halts or an error occurs. */
int a64_run(A64 *cpu);
/* Host pointer for size bytes at a guest address, or NULL. */
uint8_t *a64_pointer(A64 *cpu, uint64_t address, uint64_t size, int write);
const char *a64_error_text(int code);

#endif
