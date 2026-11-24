// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#define SS_ROOT "/seedswap"
#define OVERLAY_DIR "/ns2jb-modloader/contents/0100ee5021c9e000"

// progress callback: frac 0..1 (negative = indeterminate), short status text; return false to abort
typedef bool (*ProgressFn)(float frac, const char *text);

bool ini_get(const char *text, const char *key, char *out, size_t n);
int64_t ini_get_i64(const char *text, const char *key, int64_t def);

char *read_text(VfsRoot r, const char *path, size_t max);           // malloc'd, NUL-terminated
bool write_text(VfsRoot r, const char *path, const char *text);     // replace file
bool read_blob(VfsRoot r, const char *path, size_t max, void **out, size_t *len);

void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *app_log_tail(void);  // in-memory copy of this session's log

void uid_hex(const Uid *u, char out[33]);
bool uid_parse(const char *hex, Uid *u);
void fmt_bytes(int64_t n, char *out, size_t sz);
bool mkdirs(VfsRoot r, const char *path);

typedef struct { int files, dirs; int64_t bytes; } TreeStats;
bool tree_stats(VfsRoot r, const char *path, TreeStats *st);
// copy a directory tree between roots (dst created); progress over total bytes
bool copy_tree(VfsRoot sr, const char *src, VfsRoot dr, const char *dst, int64_t total, int64_t *done, ProgressFn cb);
bool clear_dir(VfsRoot r, const char *path);  // delete everything inside, keep the directory
// copy_tree into a save, calling commit() whenever the next file would take what was written since the
// last commit past budget bytes (files counted in whole 16 KiB blocks). budget <= 0: no commits.
bool copy_tree_commit(VfsRoot sr, const char *src, VfsRoot dr, const char *dst, int64_t total, int64_t *done,
                      ProgressFn cb, int64_t budget, int (*commit)(void));
// What a tree takes in a save: all files in 16 KiB blocks (+ one per directory), and the largest file.
// Adds to *blocks and raises *largest.
bool tree_save_room(VfsRoot r, const char *path, int64_t *blocks, int64_t *largest);
