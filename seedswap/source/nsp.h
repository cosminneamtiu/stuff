// SPDX-License-Identifier: GPL-2.0-only
// Game dumps on the SD card (.nsp, .nsz, .xci, .xcz): find them and read their file list (no keys needed).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAX_NSPS 64
#define NSP_DIR "/seedswap/nsp"

enum { NSP_BASE, NSP_UPDATE, NSP_DLC, NSP_UNKNOWN };

typedef struct {
    char path[320], file[256];
    char name[128], version[16];  // from the file name: "Game Name [titleid][v0].nsp"
    int64_t size;
    uint64_t title_id;            // from the ticket name, else from the file name
    int kind;                     // NSP_*
    int ncas, files, cnmts;       // cnmts: content lists (one per title in the dump)
    bool ticket, cnmt, inspected, ok;
    bool with_update;             // a base game's dump that holds its update too
    char err[96];
    char container[8];            // ".nsp", ".nsz", ".xci", ".xcz"
} NspInfo;

int nsp_scan(NspInfo *out, int max);   // SD:/seedswap/nsp and the SD root
bool nsp_is_dump(const char *file_name);  // .nsp, .nsz, .xci or .xcz
bool nsp_inspect(NspInfo *n);          // read the PFS0 header; fills title_id/kind/ncas/ticket/cnmt
const char *nsp_kind_name(int kind);
uint64_t nsp_base_id(uint64_t title_id);  // the base game's title id for an update or DLC
