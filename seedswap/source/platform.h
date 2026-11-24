// SPDX-License-Identifier: GPL-2.0-only
// SeedSwap platform layer: display, input, files (SD card and the Seedsow
// save), users, time. Two backends: plat_switch.c (kaiHEN, libounce) and
// plat_host.c (PC preview: PNG frames, scripted input, plain directories).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREEN_W 1280
#define SCREEN_H 720

typedef struct {
    uint32_t *px;   // RGBA8888 in memory order R,G,B,A (u32 little endian: A<<24|B<<16|G<<8|R)
    int w, h, stride;  // stride in pixels
} Canvas;

// ---- lifecycle / display / input
bool plat_init(void);
void plat_exit(void);
bool plat_loop(void);              // false when the app should quit (applet exit)
Canvas *plat_frame_begin(void);
void plat_frame_end(void);
uint64_t plat_buttons_down(void);  // edge-triggered, with auto-repeat for directions
uint64_t plat_buttons_held(void);
uint64_t plat_ms(void);            // monotonic milliseconds
void plat_sleep_ms(int ms);
void plat_keep_awake(bool on);      // no auto-sleep while a long job (a conversion) runs
bool plat_now(char *out, size_t n, bool compact);  // local time "2026-11-20 18:40" / "20261120T184000"

enum {
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3,
    BTN_L = 1 << 4, BTN_R = 1 << 5, BTN_ZL = 1 << 6, BTN_ZR = 1 << 7,
    BTN_PLUS = 1 << 8, BTN_MINUS = 1 << 9,
    BTN_UP = 1 << 10, BTN_DOWN = 1 << 11, BTN_LEFT = 1 << 12, BTN_RIGHT = 1 << 13,
};

// ---- files
typedef enum { VFS_SD = 0, VFS_SAVE = 1 } VfsRoot;
typedef struct { char name[256]; bool is_dir; int64_t size; } VfsEntry;
typedef struct VfsFile VfsFile;

int vfs_stat(VfsRoot r, const char *path, bool *is_dir, int64_t *size);   // 0 = exists
int vfs_list(VfsRoot r, const char *dir, VfsEntry **out, int *count);      // caller frees *out
int vfs_create(VfsRoot r, const char *path, int64_t size);
VfsFile *vfs_open(VfsRoot r, const char *path, bool write);
int vfs_read(VfsFile *f, int64_t off, void *buf, size_t n, size_t *got);
int vfs_write(VfsFile *f, int64_t off, const void *buf, size_t n);
int vfs_flush(VfsFile *f);
int64_t vfs_size(VfsFile *f);
void vfs_close(VfsFile *f);
int vfs_mkdir(VfsRoot r, const char *path);              // 0 if it exists already
int vfs_delete(VfsRoot r, const char *path);             // file
int vfs_rmtree(VfsRoot r, const char *path);             // directory, recursively
int vfs_rename(VfsRoot r, const char *from, const char *to);  // file or directory, same root
int64_t vfs_free_space(VfsRoot r);
const char *vfs_last_error(void);

// ---- the account save of one user for the host title (Seedsow 0100ee5021c9e000 unless
// save_set_title chose another host), mounted as VFS_SAVE
#define SEEDSOW_TITLE 0x0100ee5021c9e000ULL
#define SEEDSOW_DEMO_TITLE 0x010042d022aea000ULL
// The title whose saves the calls below open (unmounts a save that is open). Seedsow until set.
void save_set_title(uint64_t title);
uint64_t save_title(void);
typedef struct { uint8_t b[16]; } Uid;
int save_list_users(Uid *out, int max);       // users that have an account save for the title
int save_list_title_users(uint64_t title, Uid *out, int max); // lookup without changing the current title
// The only user whose Seedsow save save_mount opens (NULL: none).
void save_allow_only(const Uid *uid);
// Whether a user's Seedsow save (always Seedsow's, whatever save_set_title chose) has one of these
// files at its root: opened read-only, nothing read but whether the names exist, closed again. The one
// look into a save that isn't the play user's: it finds the user with the exploit save (users.c).
// 1 = found (found = the name), 0 = none of them, -1 = the save couldn't be opened.
int save_find_files(const Uid *uid, const char *const *names, int n, char *found, size_t fn);
int save_mount(const Uid *uid, bool write);
int save_commit(void);
void save_unmount(void);
// The save's size: data and journal in bytes (the journal bounds what one commit may write). Only the
// allowed user's; the save must not be mounted.
int save_get_size(const Uid *uid, int64_t *data, int64_t *journal);
// Grow it (fsp-srv ExtendSaveDataFileSystem). Sizes are multiples of 1 MiB and never smaller than now:
// a save can't shrink.
int save_extend(const Uid *uid, int64_t data, int64_t journal);
bool user_name(const Uid *uid, char *out, size_t n);  // profile nickname, false if unknown

// ---- installed games: the console's application records with their names (the PC preview reads
// SEEDSWAP_APPS, "id=name,id=name"). -1 when the console can't tell.
// platform: the game's content platform from ns (ApplicationDeliveryInfo, 20.0.0+): 0 = Switch 1 (Nx),
// above 0 = another (Switch 2), -1 = not known (the PC preview: SEEDSWAP_SWITCH2 lists Switch 2 IDs).
typedef struct { uint64_t id; char name[64]; int platform; } AppEntry;
int apps_list(AppEntry *out, int max);
