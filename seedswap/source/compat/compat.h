/* SPDX-License-Identifier: GPL-2.0-only */
/* Nintendo's compatibility patches for a game's ExeFS image (exefs.nsp), end to end. */
#ifndef SEEDSWAP_COMPAT_H
#define SEEDSWAP_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include "cmpt.h"

typedef struct CompatReport {
    int status;              /* a CMPT_ status, or one of the COMPAT_ ones below */
    int width;               /* 32 or 64 */
    uint32_t routine;        /* the patch routine the system chose, 0 for none */
    size_t change_count;     /* instructions changed in main */
    uint64_t steps;
    char version[16];
    char message[192];
} CompatReport;

enum {
    COMPAT_BAD_EXEFS = 100,  /* not a PFS0 image with a main NSO */
    COMPAT_OUTSIDE_TEXT,     /* the patcher changed something that is not main's code */
    COMPAT_NO_ROOM,          /* the patched image does not fit in the old size */
    COMPAT_VERIFY_FAILED     /* the rebuilt image does not read back as intended */
};

/* Runs the system's patcher on a game's main NSO. With build set and something to change, *out is
 * the new main NSO (to free()); otherwise *out stays NULL. Returns the status. */
int compat_patch_main(const uint8_t *patcher, size_t patcher_size, const uint8_t *main_file, size_t main_size,
                      uint64_t program_id, int build, uint8_t **out, size_t *out_size, CompatReport *report);

/* The same for a whole exefs.nsp held in memory: inspects the game and returns what the system's
 * patcher would do to it, without building anything (*out == NULL), or builds the patched image of
 * the same size (*out, to free()).
 * Returns the status. With CMPT_OK and change_count == 0 there is nothing to do and *out stays NULL. */
int compat_patch_exefs(const uint8_t *patcher, size_t patcher_size, const uint8_t *exefs, size_t exefs_size,
                       uint64_t program_id, int build, uint8_t **out, CompatReport *report);
const char *compat_status_text(int status);

#endif
