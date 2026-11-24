/* SPDX-License-Identifier: GPL-2.0-only */
/* Nintendo's Switch 1 compatibility patches for a hosted game.
 *
 * On Switch 2 the system program cmptldr patches every Switch 1 game when its
 * process is created, choosing among 65 routines by the game's program ID (or
 * its Unity / Unreal version). A game hosted under Seedsow's ID is treated as
 * Seedsow and gets nothing. cmpt_run() runs the user's own copy of cmptldr's
 * `main` in the interpreter (a64.c) on the game's image with the game's real
 * ID, so the image ends up exactly as the system would have made it. No code
 * of cmptldr is part of SeedSwap; without the file there are no patches. */
#ifndef SEEDSWAP_CMPT_H
#define SEEDSWAP_CMPT_H

#include <stddef.h>
#include <stdint.h>

/* One module of the game process, as the loader would have laid it out. */
typedef struct CmptModule {
    uint8_t *image;
    uint64_t image_size;
    uint32_t text_off, text_size;
    uint32_t ro_off, ro_size;
    uint32_t data_off, data_size;
    uint32_t api_off, api_size;   /* SDK version strings, relative to rodata */
} CmptModule;

/* One changed 32-bit word. */
typedef struct CmptChange {
    uint32_t module;              /* 0 = main, 1 = the second module */
    uint32_t offset;              /* from the module's start, a multiple of 4 */
    uint32_t old_word, new_word;
} CmptChange;

enum {
    CMPT_OK = 0,
    CMPT_UNKNOWN_PATCHER,   /* not a cmptldr build this code knows */
    CMPT_BAD_PATCHER,       /* the file does not load */
    CMPT_NO_MEMORY,
    CMPT_ENGINE_ERROR,      /* the interpreter stopped; see message */
    CMPT_PATCHER_FAILED,    /* cmptldr itself returned an error */
    CMPT_TOO_MANY_CHANGES
};

typedef struct CmptResult {
    int status;
    uint32_t patcher_result;  /* cmptldr's own result code */
    uint32_t routine;         /* offset of the patch routine that ran, 0 for none */
    uint64_t steps;           /* instructions interpreted */
    CmptChange *changes;      /* sorted by module and offset; free with cmpt_free */
    size_t change_count;
    char version[16];         /* system version of the patcher, e.g. "23.0.1" */
    char message[192];
} CmptResult;

/* Applies the system's patches for program_id to main (and second, which may be NULL), in place.
 * On any status other than CMPT_OK the images are restored to what they were. */
int cmpt_run(const uint8_t *patcher_file, size_t patcher_size, uint64_t program_id, int is64, CmptModule *main,
             CmptModule *second, CmptResult *result);
/* The same, after writing (offset, word) pairs into the patcher's code. For the tests only: it lets
 * them force a particular patch routine. */
int cmpt_run_poked(const uint8_t *patcher_file, size_t patcher_size, uint64_t program_id, int is64, CmptModule *main,
                   CmptModule *second, const uint32_t *pokes, size_t poke_count, CmptResult *result);
void cmpt_free(CmptResult *result);
const char *cmpt_status_text(int status);

#endif
