/* SPDX-License-Identifier: GPL-2.0-only */
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nsofile.h"

const char *compat_status_text(int status) {
    switch (status) {
    case COMPAT_BAD_EXEFS: return "the game's exefs.nsp could not be read";
    case COMPAT_OUTSIDE_TEXT: return "the patcher changed something outside the game's code";
    case COMPAT_NO_ROOM: return "the patched game does not fit in its exefs.nsp";
    case COMPAT_VERIFY_FAILED: return "the patched game did not read back correctly";
    default: return cmpt_status_text(status);
    }
}

static int stop(CompatReport *report, int status, const char *detail) {
    report->status = status;
    if (detail && detail[0])
        snprintf(report->message, sizeof(report->message), "%s", detail);
    else if (!report->message[0])
        snprintf(report->message, sizeof(report->message), "%s", compat_status_text(status));
    return status;
}

static void module_of(const NsoImage *nso, CmptModule *m) {
    m->image = nso->image;
    m->image_size = nso->image_size;
    m->text_off = nso->mem[0];
    m->text_size = nso->size[0];
    m->ro_off = nso->mem[1];
    m->ro_size = nso->size[1];
    m->data_off = nso->mem[2];
    m->data_size = nso->size[2];
    m->api_off = nso->api_off;
    m->api_size = nso->api_size;
}

int compat_patch_main(const uint8_t *patcher, size_t patcher_size, const uint8_t *main_file, size_t main_size,
                      uint64_t program_id, int build, uint8_t **out, size_t *out_size, CompatReport *report) {
    memset(report, 0, sizeof(*report));
    *out = NULL;
    *out_size = 0;

    NsoImage nso;
    char error[96];
    if (nso_load(main_file, main_size, &nso, error, sizeof(error)) < 0)
        return stop(report, COMPAT_BAD_EXEFS, error);
    report->width = nso_is32(&nso) ? 32 : 64;

    CmptModule module;
    module_of(&nso, &module);
    CmptResult result;
    int status = cmpt_run(patcher, patcher_size, program_id, report->width == 64, &module, NULL, &result);
    report->routine = result.routine;
    report->steps = result.steps;
    report->change_count = result.change_count;
    snprintf(report->version, sizeof(report->version), "%s", result.version);
    if (status != CMPT_OK) {
        stop(report, status, result.message);
        goto done;
    }
    for (size_t i = 0; i < result.change_count; i++) {
        const CmptChange *c = &result.changes[i];
        if (c->module != 0 || c->offset < nso.mem[0] || (uint64_t)c->offset + 4 > (uint64_t)nso.mem[0] + nso.size[0]) {
            stop(report, COMPAT_OUTSIDE_TEXT, NULL);
            goto done;
        }
    }
    report->status = CMPT_OK;
    if (!result.change_count || !build)
        goto done;

    {
        uint8_t *new_main = NULL;
        size_t new_main_size = 0;
        int good = 0;
        if (nso_build(&nso, 1u, &new_main, &new_main_size) == 0) {
            /* The new main must load as the image that was just patched, hashes included. */
            NsoImage check;
            if (nso_load(new_main, new_main_size, &check, error, sizeof(error)) == 0) {
                good = check.image_size == nso.image_size && !memcmp(check.image, nso.image, (size_t)nso.image_size);
                nso_free(&check);
            }
        }
        if (!good) {
            free(new_main);
            stop(report, COMPAT_VERIFY_FAILED, NULL);
            goto done;
        }
        *out = new_main;
        *out_size = new_main_size;
    }

done:
    cmpt_free(&result);
    nso_free(&nso);
    return report->status;
}

int compat_patch_exefs(const uint8_t *patcher, size_t patcher_size, const uint8_t *exefs, size_t exefs_size,
                       uint64_t program_id, int build, uint8_t **out, CompatReport *report) {
    *out = NULL;
    Pfs0Entry entries[64];
    int count = pfs0_list(exefs, exefs_size, entries, 64);
    const Pfs0Entry *main_entry = NULL;
    for (int i = 0; i < count; i++)
        if (!strcmp(entries[i].name, "main"))
            main_entry = &entries[i];
    if (!main_entry) {
        memset(report, 0, sizeof(*report));
        return stop(report, COMPAT_BAD_EXEFS, NULL);
    }

    uint8_t *new_main = NULL;
    size_t new_main_size = 0;
    int status = compat_patch_main(patcher, patcher_size, exefs + main_entry->offset, (size_t)main_entry->size,
                                   program_id, build, &new_main, &new_main_size, report);
    if (status != CMPT_OK || !new_main)
        return status;

    uint8_t *image = NULL;
    int rc = pfs0_replace(exefs, exefs_size, "main", new_main, new_main_size, &image);
    free(new_main);
    if (rc != 0)
        return stop(report, rc == -2 ? COMPAT_NO_ROOM : COMPAT_BAD_EXEFS, NULL);
    /* Every other file must be byte-identical in the new image. */
    Pfs0Entry after[64];
    int after_count = pfs0_list(image, exefs_size, after, 64);
    int good = after_count == count;
    for (int i = 0; good && i < count; i++) {
        good = !strcmp(entries[i].name, after[i].name);
        if (good && strcmp(entries[i].name, "main"))
            good = entries[i].size == after[i].size &&
                   !memcmp(exefs + entries[i].offset, image + after[i].offset, (size_t)entries[i].size);
    }
    if (!good) {
        free(image);
        return stop(report, COMPAT_VERIFY_FAILED, NULL);
    }
    *out = image;
    return report->status = CMPT_OK;
}
