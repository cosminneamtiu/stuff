// SPDX-License-Identifier: GPL-2.0-only
// SeedSwap platform backend for kaiHEN (libounce). Display: linear RGBA8888
// framebuffer, double buffered. Files: the SD card and the Seedsow account
// save through fsp-srv (HEN's FS proxy root, as mxdump/mxwrite use it).
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <switch.h>

static Framebuffer g_fb;
static Canvas g_canvas;
static PadState g_pad;
static FsFileSystem g_sd, g_save;
static bool g_save_open, g_acc_ok;
static char g_err[160];
static uint64_t g_down, g_held, g_repeat_at;

struct VfsFile { FsFile f; };

static void set_err(const char *what, Result rc) { snprintf(g_err, sizeof(g_err), "%s failed (0x%x)", what, rc); }
const char *vfs_last_error(void) { return g_err; }

bool plat_init(void) {
    if (R_FAILED(framebufferCreate(&g_fb, nwindowGetDefault(), SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2)))
        return false;
    framebufferMakeLinear(&g_fb);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    Result rc = fsOpenSdCardFileSystem(&g_sd);
    if (R_FAILED(rc)) { set_err("open SD card", rc); return false; }
    g_acc_ok = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));
    timeInitialize();
    return true;
}

void plat_exit(void) {
    save_unmount();
    fsFsClose(&g_sd);
    if (g_acc_ok) accountExit();
    timeExit();
    framebufferClose(&g_fb);
}

static uint64_t map_buttons(uint64_t b) {
    uint64_t o = 0;
    if (b & HidNpadButton_A) o |= BTN_A;
    if (b & HidNpadButton_B) o |= BTN_B;
    if (b & HidNpadButton_X) o |= BTN_X;
    if (b & HidNpadButton_Y) o |= BTN_Y;
    if (b & HidNpadButton_L) o |= BTN_L;
    if (b & HidNpadButton_R) o |= BTN_R;
    if (b & HidNpadButton_ZL) o |= BTN_ZL;
    if (b & HidNpadButton_ZR) o |= BTN_ZR;
    if (b & HidNpadButton_Plus) o |= BTN_PLUS;
    if (b & HidNpadButton_Minus) o |= BTN_MINUS;
    if (b & HidNpadButton_AnyUp) o |= BTN_UP;
    if (b & HidNpadButton_AnyDown) o |= BTN_DOWN;
    if (b & HidNpadButton_AnyLeft) o |= BTN_LEFT;
    if (b & HidNpadButton_AnyRight) o |= BTN_RIGHT;
    return o;
}

bool plat_loop(void) {
    if (!appletMainLoop()) return false;
    padUpdate(&g_pad);
    g_down = map_buttons(padGetButtonsDown(&g_pad));
    g_held = map_buttons(padGetButtons(&g_pad));
    const uint64_t dirs = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
    uint64_t now = plat_ms();
    if (g_down & dirs) g_repeat_at = now + 380;
    else if ((g_held & dirs) && now >= g_repeat_at) { g_down |= g_held & dirs; g_repeat_at = now + 70; }
    return true;
}

uint64_t plat_buttons_down(void) { return g_down; }
uint64_t plat_buttons_held(void) { return g_held; }
uint64_t plat_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }
void plat_sleep_ms(int ms) { svcSleepThread((s64)ms * 1000000LL); }
void plat_keep_awake(bool on) { (void)appletSetAutoSleepDisabled(on); }  // best effort: an error changes nothing

bool plat_now(char *out, size_t n, bool compact) {
    time_t t = time(NULL);
    if (t < 1600000000) { snprintf(out, n, compact ? "boot%llu" : "time unknown", (unsigned long long)plat_ms()); return false; }
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, compact ? "%Y%m%dT%H%M%S" : "%Y-%m-%d %H:%M", &tm);
    return true;
}

Canvas *plat_frame_begin(void) {
    u32 stride = 0;
    g_canvas.px = framebufferBegin(&g_fb, &stride);
    g_canvas.w = SCREEN_W;
    g_canvas.h = SCREEN_H;
    g_canvas.stride = (int)(stride / 4);
    return &g_canvas;
}
void plat_frame_end(void) { framebufferEnd(&g_fb); }

// ---- files
static FsFileSystem *fsys(VfsRoot r) { return r == VFS_SAVE ? (g_save_open ? &g_save : NULL) : &g_sd; }

int vfs_stat(VfsRoot r, const char *path, bool *is_dir, int64_t *size) {
    FsFileSystem *fs = fsys(r);
    if (!fs) return -1;
    FsDirEntryType type;
    if (R_FAILED(fsFsGetEntryType(fs, path, &type))) return -1;
    if (is_dir) *is_dir = type == FsDirEntryType_Dir;
    if (size) {
        *size = 0;
        if (type == FsDirEntryType_File) {
            FsFile f;
            if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Read, &f))) {
                s64 s = 0;
                fsFileGetSize(&f, &s);
                *size = s;
                fsFileClose(&f);
            }
        }
    }
    return 0;
}

int vfs_list(VfsRoot r, const char *dir, VfsEntry **out, int *count) {
    *out = NULL;
    *count = 0;
    FsFileSystem *fs = fsys(r);
    if (!fs) return -1;
    FsDir d;
    Result rc = fsFsOpenDirectory(fs, dir, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d);
    if (R_FAILED(rc)) { set_err("open directory", rc); return -1; }
    s64 n = 0;
    fsDirGetEntryCount(&d, &n);
    if (n <= 0) { fsDirClose(&d); return 0; }
    FsDirectoryEntry *raw = calloc((size_t)n, sizeof(*raw));
    VfsEntry *e = calloc((size_t)n, sizeof(*e));
    s64 got = 0;
    rc = raw && e ? fsDirRead(&d, &got, (size_t)n, raw) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    fsDirClose(&d);
    if (R_FAILED(rc)) { free(raw); free(e); set_err("read directory", rc); return -1; }
    for (s64 i = 0; i < got; i++) {
        snprintf(e[i].name, sizeof(e[i].name), "%s", raw[i].name);
        e[i].is_dir = raw[i].type == FsDirEntryType_Dir;
        e[i].size = raw[i].file_size;
    }
    free(raw);
    *out = e;
    *count = (int)got;
    return 0;
}

int vfs_create(VfsRoot r, const char *path, int64_t size) {
    FsFileSystem *fs = fsys(r);
    Result rc = fs ? fsFsCreateFile(fs, path, size, 0) : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (R_FAILED(rc)) { set_err("create file", rc); return -1; }
    return 0;
}

VfsFile *vfs_open(VfsRoot r, const char *path, bool write) {
    FsFileSystem *fs = fsys(r);
    if (!fs) return NULL;
    VfsFile *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    Result rc = fsFsOpenFile(fs, path, write ? (FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append) : FsOpenMode_Read, &f->f);
    if (R_FAILED(rc)) { set_err("open file", rc); free(f); return NULL; }
    return f;
}

int vfs_read(VfsFile *f, int64_t off, void *buf, size_t n, size_t *got) {
    u64 g = 0;
    Result rc = fsFileRead(&f->f, off, buf, n, FsReadOption_None, &g);
    if (got) *got = g;
    if (R_FAILED(rc)) { set_err("read", rc); return -1; }
    return 0;
}

int vfs_write(VfsFile *f, int64_t off, const void *buf, size_t n) {
    Result rc = fsFileWrite(&f->f, off, buf, n, FsWriteOption_None);
    if (R_FAILED(rc)) { set_err("write", rc); return -1; }
    return 0;
}

int vfs_flush(VfsFile *f) { return R_SUCCEEDED(fsFileFlush(&f->f)) ? 0 : -1; }
int64_t vfs_size(VfsFile *f) { s64 s = -1; fsFileGetSize(&f->f, &s); return s; }
void vfs_close(VfsFile *f) { if (f) { fsFileClose(&f->f); free(f); } }

int vfs_mkdir(VfsRoot r, const char *path) {
    bool dir = false;
    if (vfs_stat(r, path, &dir, NULL) == 0) return dir ? 0 : -1;
    FsFileSystem *fs = fsys(r);
    Result rc = fs ? fsFsCreateDirectory(fs, path) : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (R_FAILED(rc)) { set_err("create directory", rc); return -1; }
    return 0;
}

int vfs_delete(VfsRoot r, const char *path) {
    FsFileSystem *fs = fsys(r);
    Result rc = fs ? fsFsDeleteFile(fs, path) : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (R_FAILED(rc)) { set_err("delete file", rc); return -1; }
    return 0;
}

int vfs_rmtree(VfsRoot r, const char *path) {
    FsFileSystem *fs = fsys(r);
    Result rc = fs ? fsFsDeleteDirectoryRecursively(fs, path) : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (R_FAILED(rc)) { set_err("delete directory", rc); return -1; }
    return 0;
}

int vfs_rename(VfsRoot r, const char *from, const char *to) {
    FsFileSystem *fs = fsys(r);
    bool dir = false;
    if (!fs || vfs_stat(r, from, &dir, NULL) != 0) { snprintf(g_err, sizeof(g_err), "rename: %s missing", from); return -1; }
    Result rc = dir ? fsFsRenameDirectory(fs, from, to) : fsFsRenameFile(fs, from, to);
    if (R_FAILED(rc)) { set_err("rename", rc); return -1; }
    return 0;
}

int64_t vfs_free_space(VfsRoot r) {
    FsFileSystem *fs = fsys(r);
    s64 s = -1;
    if (fs) fsFsGetFreeSpace(fs, "/", &s);
    return s;
}

// ---- account saves of the host title (Seedsow unless save_set_title chose another)
static uint64_t g_title = SEEDSOW_TITLE;

void save_set_title(uint64_t title) {
    if (title != g_title) save_unmount();
    g_title = title ? title : SEEDSOW_TITLE;
}

uint64_t save_title(void) { return g_title; }

int save_list_users(Uid *out, int max) {
    return save_list_title_users(g_title, out, max);
}

int save_list_title_users(uint64_t title, Uid *out, int max) {
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User))) return -1;
    int n = 0;
    FsSaveDataInfo infos[16];
    s64 total = 0;
    while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, infos, 16, &total)) && total > 0) {
        for (s64 i = 0; i < total && n < max; i++) {
            if (infos[i].application_id != title || infos[i].save_data_type != FsSaveDataType_Account) continue;
            memcpy(out[n++].b, &infos[i].uid, 16);
        }
    }
    fsSaveDataInfoReaderClose(&reader);
    return n;
}

// Second line of defence (model.c and users.c refuse these too): only the play user's save is ever
// mounted; save_find_files only looks whether files exist in another one, read-only. (Which user has the
// exploit save is not asked of the account service: under kaiHEN its preselected and last opened users
// are whoever last started a game.)
static Uid g_allowed;
static bool g_allowed_set;

void save_allow_only(const Uid *uid) {
    g_allowed_set = uid != NULL;
    if (uid) g_allowed = *uid;
}

static bool never_open(const Uid *uid) {
    return !g_allowed_set || memcmp(g_allowed.b, uid->b, 16) != 0;
}

int save_find_files(const Uid *uid, const char *const *names, int n, char *found, size_t fn) {
    FsSaveDataAttribute attr = {0};
    attr.application_id = SEEDSOW_TITLE;
    memcpy(&attr.uid, uid->b, 16);
    attr.save_data_type = FsSaveDataType_Account;
    FsFileSystem fs;
    Result rc = fsOpenReadOnlySaveDataFileSystem(&fs, FsSaveDataSpaceId_User, &attr);
    if (R_FAILED(rc)) { set_err("open save (read-only check)", rc); return -1; }
    int hit = 0;
    for (int i = 0; i < n && !hit; i++) {
        char path[96];
        FsDirEntryType type;
        snprintf(path, sizeof(path), "/%s", names[i]);
        if (R_SUCCEEDED(fsFsGetEntryType(&fs, path, &type)) && type == FsDirEntryType_File) {
            hit = 1;
            if (found) snprintf(found, fn, "%s", names[i]);
        }
    }
    fsFsClose(&fs);
    return hit;
}

int save_mount(const Uid *uid, bool write) {
    save_unmount();
    if (never_open(uid)) { snprintf(g_err, sizeof(g_err), "refused: not the play user's save"); return -1; }
    FsSaveDataAttribute attr = {0};
    attr.application_id = g_title;
    memcpy(&attr.uid, uid->b, 16);
    attr.save_data_type = FsSaveDataType_Account;
    Result rc = write ? fsOpenSaveDataFileSystem(&g_save, FsSaveDataSpaceId_User, &attr)
                      : fsOpenReadOnlySaveDataFileSystem(&g_save, FsSaveDataSpaceId_User, &attr);
    if (R_FAILED(rc)) { set_err(write ? "open save (write)" : "open save", rc); return -1; }
    g_save_open = true;
    return 0;
}

int save_commit(void) {
    if (!g_save_open) return -1;
    Result rc = fsFsCommit(&g_save);
    if (R_FAILED(rc)) { set_err("commit save", rc); return -1; }
    return 0;
}

void save_unmount(void) {
    if (g_save_open) fsFsClose(&g_save);
    g_save_open = false;
}

// the save data ID of the allowed user's account save for the title (0: none)
static u64 save_id_of(const Uid *uid) {
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User))) return 0;
    u64 id = 0;
    FsSaveDataInfo infos[16];
    s64 total = 0;
    while (!id && R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, infos, 16, &total)) && total > 0) {
        for (s64 i = 0; i < total; i++)
            if (infos[i].application_id == g_title && infos[i].save_data_type == FsSaveDataType_Account &&
                !memcmp(&infos[i].uid, uid->b, 16)) {
                id = infos[i].save_data_id;
                break;
            }
    }
    fsSaveDataInfoReaderClose(&reader);
    return id;
}

int save_get_size(const Uid *uid, int64_t *data, int64_t *journal) {
    if (never_open(uid)) { snprintf(g_err, sizeof(g_err), "refused: not the play user's save"); return -1; }
    u64 id = save_id_of(uid);
    if (!id) { snprintf(g_err, sizeof(g_err), "the play user has no save for this host"); return -1; }
    FsSaveDataExtraData extra;
    memset(&extra, 0, sizeof(extra));
    Result rc = fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), FsSaveDataSpaceId_User, id);
    if (R_FAILED(rc)) { set_err("read the save's size", rc); return -1; }
    *data = extra.data_size;
    *journal = extra.journal_size;
    return 0;
}

int save_extend(const Uid *uid, int64_t data, int64_t journal) {
    if (never_open(uid)) { snprintf(g_err, sizeof(g_err), "refused: not the play user's save"); return -1; }
    if (g_save_open) { snprintf(g_err, sizeof(g_err), "the save is open"); return -1; }
    int64_t now_d = 0, now_j = 0;
    if (save_get_size(uid, &now_d, &now_j) != 0) return -1;
    if (data < now_d || journal < now_j || (data | journal) & 0xFFFFF) {
        snprintf(g_err, sizeof(g_err), "refused: %lld + %lld is not a growth of %lld + %lld", (long long)data,
                 (long long)journal, (long long)now_d, (long long)now_j);
        return -1;
    }
    u64 id = save_id_of(uid);
    Result rc = fsExtendSaveDataFileSystem(FsSaveDataSpaceId_User, id, data, journal);
    if (R_FAILED(rc)) { set_err("ExtendSaveData", rc); return -1; }
    return 0;
}

bool user_name(const Uid *uid, char *out, size_t n) {
    if (!g_acc_ok) return false;
    AccountUid au;
    memcpy(&au, uid->b, 16);
    AccountProfile prof;
    if (R_FAILED(accountGetProfile(&prof, au))) return false;
    AccountProfileBase base;
    bool ok = R_SUCCEEDED(accountProfileGet(&prof, NULL, &base));
    accountProfileClose(&prof);
    if (ok) snprintf(out, n, "%s", base.nickname);
    return ok && out[0];
}

int apps_list(AppEntry *out, int max) {
    if (R_FAILED(nsInitialize())) return -1;
    NsApplicationControlData *cd = malloc(sizeof(*cd));
    NsApplicationRecord recs[32];
    int n = 0;
    for (s32 offset = 0; cd && n < max;) {
        s32 got = 0;
        if (R_FAILED(nsListApplicationRecord(recs, 32, offset, &got)) || got <= 0) break;
        for (s32 i = 0; i < got && n < max; i++) {
            AppEntry *e = &out[n++];
            e->id = recs[i].application_id;
            e->name[0] = 0;
            e->platform = -1;
            NsApplicationDeliveryInfo di;   // its platform: Switch 1 (0) or Switch 2
            s32 total = 0;
            memset(&di, 0, sizeof(di));
            if (R_SUCCEEDED(nsGetApplicationDeliveryInfo(&di, 1, e->id, 0, &total)) && total >= 1)
                e->platform = di.data.platform;
            u64 size = 0;
            if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, e->id, cd, sizeof(*cd), &size)))
                continue;
            NacpLanguageEntry *le = NULL;   // the system language's, else another that has a name
            if (R_SUCCEEDED(nacpGetLanguageEntry(&cd->nacp, &le)) && le)
                snprintf(e->name, sizeof(e->name), "%.*s", (int)sizeof(le->name), le->name);
        }
        offset += got;
        if (got < 32) break;
    }
    free(cd);
    nsExit();
    return n;
}
