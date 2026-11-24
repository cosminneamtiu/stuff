// SPDX-License-Identifier: GPL-2.0-only
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "convert.h"
#include "gfx.h"
#include "model.h"
#include "nsp.h"
#include "users.h"
#include "util.h"

// ---- theme
#define C_BG_TOP HEX(0x1b2133)
#define C_BG_BOT HEX(0x0c0f16)
#define C_PANEL RGBA(255, 255, 255, 13)
#define C_PANEL2 RGBA(255, 255, 255, 22)
#define C_LINE RGBA(255, 255, 255, 24)
#define C_SEL HEX(0x28324c)
#define C_ACCENT HEX(0x7bd389)
#define C_ACCENT_DK HEX(0x10301c)
#define C_TEXT HEX(0xedf0f7)
#define C_DIM HEX(0xa3abc0)
#define C_FAINT HEX(0x6b7591)
#define C_WARN HEX(0xf2c14e)
#define C_BAD HEX(0xef6f6c)
#define C_INFO HEX(0x7ab8f5)
#define C_DARK HEX(0x111522)

enum { TAB_LIBRARY, TAB_CONVERT, TAB_SAVES, TAB_LOG, TAB_COUNT };
static const char *TAB_NAMES[TAB_COUNT] = {"Library", "Convert", "Saves", "Log"};
typedef enum { DLG_NONE, DLG_CONFIRM, DLG_MESSAGE, DLG_PROGRESS, DLG_CHOOSE, DLG_USERS, DLG_HOSTS } DlgKind;
typedef enum {
    ACT_NONE, ACT_SWITCH, ACT_BACKUP, ACT_RESTORE, ACT_COMPAT, ACT_COMPAT_REVERT, ACT_DLC_ADD, ACT_DLC_REMOVE, ACT_CONVERT,
    ACT_DELETE_DUMPS, ACT_SET_USER, ACT_ADD_HOST, ACT_MOVE
} Action;

typedef struct { bool app; int i; } HostRow;   // a row of the host list: a host, or an installed game

#define LIST_Y 100
#define LIST_H 552
#define LEFT_X 32
#define LEFT_W 536
#define RIGHT_X 588
#define RIGHT_W 660

static struct {
    Library lib;
    NspInfo nsps[MAX_NSPS];
    int nsp_count;
    int tab, sel[TAB_COUNT], scroll[TAB_COUNT];
    DlgKind dlg;
    Action act;
    int act_arg;
    char act_path[256];  // the dump an action works on
    char dlg_title[128], dlg_body[1400];
    Color dlg_color;
    float prog;
    char prog_text[200];
    uint64_t prog_last;
    char *log;
    int log_lines, log_scroll;
    char keys_path[48];
    bool auto_compat;   // look at the active game's Nintendo patches once, at start
    bool compat_auto_run;  // the running ACT_COMPAT was started by SeedSwap, not by a button
    ConvertPlan plan;      // the dumps being converted
    int plan_prog, choose_sel;
    bool allow_older;      // the user confirmed converting an older version
    char del_paths[2][320];  // the dumps a finished conversion offers to delete
    int64_t del_bytes;
    UserRow users[USERS_MAX];  // the play-user list (users.h)
    int user_count, user_sel;
    // the host list (-): the hosts, then the installed games that could become one
    HostRow host_rows[HOSTS_MAX + 64];
    int host_row_count, host_sel, host_top;
    AppEntry apps[64];         // installed games that could become a host
    int app_count;
} U;

// ---- small helpers
static int text_h(int font, int w, const char *s) {  // measure a wrapped paragraph
    gfx_clip((Rect){0, 0, 0, 0});
    int h = gfx_text_wrap(font, 0, 0, w, 0, 0, s);
    gfx_unclip();
    return h;
}

static void text_center(int font, Rect r, Color c, const char *s) {
    gfx_text(font, r.x + (r.w - gfx_text_width(font, s)) / 2, r.y + (r.h - gfx_line_height(font)) / 2, c, s);
}

static int glyph_w(const char *btn) { return strlen(btn) == 1 && btn[0] != 'L' && btn[0] != 'R' ? 28 : 16 + gfx_text_width(FONT_SMALL, btn); }

static int button_glyph(int x, int y, const char *btn, Color bg) {
    int w = glyph_w(btn);
    Rect r = {x, y, w, 28};
    gfx_round(r, 14, bg);
    text_center(FONT_SMALL, (Rect){r.x, r.y - 1, r.w, r.h}, C_DARK, btn);
    return w;
}

static int hint_w(const char *btn, const char *label) { return glyph_w(btn) + 8 + gfx_text_width(FONT_SMALL, label); }

static int hint(int x, int y, const char *btn, const char *label) {
    int w = button_glyph(x, y, btn, C_TEXT);
    gfx_text(FONT_SMALL, x + w + 8, y + 2, C_DIM, label);
    return hint_w(btn, label);
}

static int pill(int x, int y, const char *text, Color bg, Color fg) {
    int w = gfx_text_width(FONT_SMALL, text) + 22;
    gfx_round((Rect){x, y, w, 28}, 14, bg);
    gfx_text(FONT_SMALL, x + 11, y + 2, fg, text);
    return w;
}

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void initials(const char *name, char out[4]) {
    int n = 0;
    bool start = true;
    for (const char *p = name; *p && n < 2; p++) {
        bool alnum = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        if (alnum && start) out[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        start = !alnum;
    }
    out[n] = 0;
}

static void game_icon(const Game *g, Rect r, bool big) {
    const Image *img = big ? &g->icon_big : &g->icon_small;
    int rad = big ? 22 : 12;
    if (img->px) { gfx_image(img, r, rad, 255); return; }
    static const Color tones[] = {HEX(0x3f6b55), HEX(0x4d5b8a), HEX(0x7a4e6b), HEX(0x80643a), HEX(0x3d6f7a), HEX(0x6a4f8c)};
    Color c = g->builtin ? HEX(0x2f6b45) : tones[hash_str(g->id) % (sizeof(tones) / sizeof(*tones))];
    gfx_round(r, rad, c);
    char ini[4];
    initials(g->name, ini);
    text_center(big ? FONT_HERO : FONT_BODY_BOLD, r, RGBA(255, 255, 255, 230), ini);
}

static void status_style(const Game *g, const char **label, Color *c) {
    if (g->builtin) { *label = "Host game"; *c = C_ACCENT; return; }
    if (g->unknown) { *label = "Unrecognised"; *c = C_WARN; return; }
    const char *s = g->status;
    if (!strcmp(s, "plays")) { *label = "Plays"; *c = C_ACCENT; }
    else if (!strcmp(s, "partial")) { *label = "Partly works"; *c = C_WARN; }
    else if (!strcmp(s, "crashes") || !strcmp(s, "broken")) { *label = "Does not work"; *c = C_BAD; }
    else { *label = "Not tested yet"; *c = C_INFO; }
}

static void ensure_visible(int tab, int count, int per_page) {
    int *sel = &U.sel[tab], *top = &U.scroll[tab];
    if (*sel >= count) *sel = count - 1;
    if (*sel < 0) *sel = 0;
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + per_page) *top = *sel - per_page + 1;
    if (*top < 0) *top = 0;
}

static void scrollbar(Rect area, int top, int count, int per_page) {
    if (count <= per_page) return;
    int h = area.h * per_page / count, y = area.y + (area.h - h) * top / (count - per_page);
    gfx_round((Rect){area.x + area.w + 6, area.y, 4, area.h}, 2, C_PANEL);
    gfx_round((Rect){area.x + area.w + 6, y, 4, h}, 2, C_FAINT);
}

static void panel(Rect r) { gfx_round(r, 18, C_PANEL); }

static void info_row(int x, int y, int w, const char *key, const char *val, Color vc) {
    gfx_text(FONT_SMALL, x, y + 2, C_FAINT, key);
    gfx_text_ellipsis(FONT_BODY, x + 150, y, w - 150, vc, val);
}

static void notice(Rect r, Color accent, const char *title, const char *body) {
    gfx_round(r, 14, RGBA(accent & 0xff, (accent >> 8) & 0xff, (accent >> 16) & 0xff, 30));
    gfx_round((Rect){r.x, r.y + 12, 4, r.h - 24}, 2, accent);
    int y = r.y + 12;
    if (title) {
        gfx_text(FONT_BODY_BOLD, r.x + 20, y, accent, title);
        y += gfx_line_height(FONT_BODY_BOLD) + 2;
    }
    int lines = (r.y + r.h - y - 8) / gfx_line_height(FONT_SMALL);
    gfx_text_wrap(FONT_SMALL, r.x + 20, y, r.w - 36, lines < 1 ? 1 : lines, C_DIM, body);
}

// a notice sized to its text, sitting on `bottom`
static int notice_height(int w, const char *title, const char *body) {
    return 26 + (title ? gfx_line_height(FONT_BODY_BOLD) + 2 : 0) + text_h(FONT_SMALL, w - 36, body);
}
static void notice_bottom(int x, int bottom, int w, Color accent, const char *title, const char *body) {
    int h = notice_height(w, title, body);
    notice((Rect){x, bottom - h, w, h}, accent, title, body);
}

// ---- data refresh
// One line on Nintendo's compatibility patches for a game.
static void compat_text(const Game *g, char *out, size_t n, Color *c) {
    const CompatInfo *ci = &g->compat;
    switch (ci->state) {
    case COMPAT_ST_APPLIED:
        snprintf(out, n, "%d applied (system %s)", ci->changes, ci->version);
        *c = C_ACCENT;
        break;
    case COMPAT_ST_NONE: snprintf(out, n, "None needed"); *c = C_TEXT; break;
    case COMPAT_ST_PENDING:
        snprintf(out, n, "%d not applied yet: press X", ci->changes);
        *c = C_WARN;
        break;
    case COMPAT_ST_NO_PATCHER: snprintf(out, n, "Patcher file missing: press X"); *c = C_WARN; break;
    case COMPAT_ST_FAILED: snprintf(out, n, "Failed: %s", ci->note); *c = C_BAD; break;
    default: snprintf(out, n, "Not checked yet: press X"); *c = C_DIM; break;
    }
}

static int find_game(const char *id) {
    for (int i = 0; i < U.lib.count; i++)
        if (!strcmp(U.lib.games[i].id, id)) return i;
    return -1;
}

static void reload(void) {
    char keep[24] = {0};
    if (U.lib.count) snprintf(keep, sizeof(keep), "%s", U.lib.games[U.sel[TAB_LIBRARY]].id);
    lib_load(&U.lib);
    int i = keep[0] ? find_game(keep) : -1;
    U.sel[TAB_LIBRARY] = i >= 0 ? i : (lib_active(&U.lib) >= 0 ? lib_active(&U.lib) : 0);
    U.nsp_count = nsp_scan(U.nsps, MAX_NSPS);
    static const char *KEYS[] = {"/switch/prod.keys", SS_ROOT "/prod.keys"};
    U.keys_path[0] = 0;
    for (size_t k = 0; k < sizeof(KEYS) / sizeof(*KEYS); k++) {
        bool d;
        if (vfs_stat(VFS_SD, KEYS[k], &d, NULL) == 0 && !d) { snprintf(U.keys_path, sizeof(U.keys_path), "%s", KEYS[k]); break; }
    }
}

static void load_log(void) {
    free(U.log);
    U.log = NULL;
    VfsFile *f = vfs_open(VFS_SD, SS_ROOT "/log.txt", false);
    int64_t size = f ? vfs_size(f) : -1;
    if (f && size > 0) {
        int64_t want = size > 24576 ? 24576 : size;
        U.log = malloc((size_t)want + 1);
        size_t got = 0;
        if (U.log && vfs_read(f, size - want, U.log, (size_t)want, &got) == 0) {
            U.log[got] = 0;
            if (want < size) {  // start at a whole line
                char *nl = strchr(U.log, '\n');
                if (nl) memmove(U.log, nl + 1, strlen(nl + 1) + 1);
            }
        }
    }
    vfs_close(f);
    if (!U.log) U.log = strdup(app_log_tail());
    U.log_lines = 0;
    for (char *p = U.log; *p; p++)
        if (*p == '\n') U.log_lines++;
    U.log_scroll = 0;  // 0 = newest at the bottom
}

static void open_users(void);
static void open_hosts(void);

void ui_init(void) {
    memset(&U, 0, sizeof(U));
    reload();
    U.auto_compat = true;
    if (!U.lib.play_user_set) {   // first start (or the play user is gone): ask who plays
        U.tab = TAB_SAVES;
        open_users();
    } else if (!U.lib.host_chosen) open_hosts();   // no host picked yet: ask which
}

void ui_exit(void) {
    lib_free(&U.lib);
    free(U.log);
}

// ---- dialogs
static void message(Color c, const char *title, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void message(Color c, const char *title, const char *fmt, ...) {
    U.dlg = DLG_MESSAGE;
    U.dlg_color = c;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "%s", title);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.dlg_body, sizeof(U.dlg_body), fmt, ap);
    va_end(ap);
}

static int user_pitch(void) { return U.user_count > 5 ? 50 : 58; }  // up to 8 users fit on the screen

// ---- hosts (-): the hosts, then every installed game that could become one
#define HOST_ROWS 6

// what a host holds, for its row
static void host_line(int i, char *out, size_t n) {
    const Library *L = &U.lib;
    const Host *h = &L->hs.list[i];
    const char *game = "Itself";
    for (int k = 0; k < L->count; k++)
        if (!L->games[k].builtin && L->games[k].host == i) game = L->games[k].name;
    snprintf(out, n, "%s%s%s", game,
             !lib_host_served(L, h) ? "  \xe2\x80\xa2  restart and run the launcher first"
             : !h->npdm ? "  \xe2\x80\xa2  start it once first" : "",
             L->play_user_set && !h->user_save ? "  \xe2\x80\xa2  no save for the play user yet" : "");
}

static void draw_dialog(void) {
    if (U.dlg == DLG_NONE) return;
    gfx_fill((Rect){0, 0, SCREEN_W, SCREEN_H}, RGBA(4, 6, 12, 170));
    int w = 760, pad = 36;
    int body_h = U.dlg == DLG_PROGRESS ? 96 : U.dlg == DLG_CHOOSE ? 58 * U.plan.count + 40
               : U.dlg == DLG_USERS ? user_pitch() * U.user_count + 40 + 66
               : U.dlg == DLG_HOSTS ? 58 * (U.host_row_count < HOST_ROWS ? U.host_row_count : HOST_ROWS) + 40
               : text_h(FONT_BODY, w - pad * 2, U.dlg_body);
    if (body_h > 400 && U.dlg != DLG_USERS) body_h = 400;
    int h = pad + 44 + body_h + (U.dlg == DLG_PROGRESS ? 24 : 86);
    Rect r = {(SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h};
    gfx_round((Rect){r.x - 1, r.y - 1, r.w + 2, r.h + 2}, 24, C_LINE);
    gfx_round(r, 24, HEX(0x1e2538));
    gfx_round((Rect){r.x + pad, r.y + pad - 4, 40, 6}, 3, U.dlg_color);
    gfx_text_ellipsis(FONT_TITLE, r.x + pad, r.y + pad + 8, w - pad * 2, C_TEXT, U.dlg_title);
    int y = r.y + pad + 52;
    if (U.dlg == DLG_PROGRESS) {
        Rect bar = {r.x + pad, y + 8, w - pad * 2, 14};
        gfx_round(bar, 7, C_PANEL2);
        if (U.prog >= 0) {
            int fw = (int)((float)bar.w * (U.prog > 1 ? 1 : U.prog));
            if (fw > 14) gfx_round((Rect){bar.x, bar.y, fw, bar.h}, 7, U.dlg_color);
            char pct[16];
            snprintf(pct, sizeof(pct), "%d%%", (int)(U.prog * 100 + 0.5f));
            gfx_text(FONT_BODY_BOLD, bar.x + bar.w - gfx_text_width(FONT_BODY_BOLD, pct), y + 34, C_TEXT, pct);
        }
        gfx_text_ellipsis(FONT_SMALL, bar.x, y + 38, bar.w - 90, C_DIM, U.prog_text);
        gfx_text(FONT_SMALL, bar.x, y + 70, C_FAINT, "Don't turn off the console or close SeedSwap.");
        return;
    }
    int by = r.y + h - pad - 30, bx = r.x + w - pad;
    if (U.dlg == DLG_CHOOSE) {
        gfx_text(FONT_SMALL, r.x + pad, y, C_DIM, "This dump holds several games. Pick one.");
        for (int i = 0; i < U.plan.count; i++) {
            const ConvertProgram *p = &U.plan.programs[i];
            Rect row = {r.x + pad, y + 32 + 58 * i, w - pad * 2, 52};
            gfx_round(row, 12, i == U.choose_sel ? C_SEL : C_PANEL2);
            if (i == U.choose_sel) gfx_round_outline(row, 12, 2, C_ACCENT);
            gfx_text_ellipsis(FONT_BODY_BOLD, row.x + 16, row.y + 4, row.w - 32, C_TEXT, p->name);
            char sub[200];
            snprintf(sub, sizeof(sub), "%016llX%s%s", (unsigned long long)p->program_id,
                     p->note[0] ? "  \xe2\x80\xa2  " : "", p->note);
            gfx_text_ellipsis(FONT_SMALL, row.x + 16, row.y + 28, row.w - 32, C_DIM, sub);
        }
        bx -= hint_w("B", "Cancel");
        hint(bx, by, "B", "Cancel");
        bx -= 28 + hint_w("A", "Choose");
        hint(bx, by, "A", "Choose");
        return;
    }
    if (U.dlg == DLG_HOSTS) {
        const Library *L = &U.lib;
        gfx_text(FONT_SMALL, r.x + pad, y, C_DIM, "Your games run as the host. Pick one.");
        for (int k = 0; k < HOST_ROWS && U.host_top + k < U.host_row_count; k++) {
            int i = U.host_top + k;
            Rect row = {r.x + pad, y + 32 + 58 * k, w - pad * 2, 52};
            gfx_round(row, 12, i == U.host_sel ? C_SEL : C_PANEL2);
            if (i == U.host_sel) gfx_round_outline(row, 12, 2, C_ACCENT);
            char sub[200], hex[17];
            if (U.host_rows[i].app) {
                const AppEntry *a = &U.apps[U.host_rows[i].i];
                hosts_hex(a->id, hex);
                gfx_text_ellipsis(FONT_BODY_BOLD, row.x + 16, row.y + 4, row.w - 32, C_TEXT, a->name[0] ? a->name : hex);
                gfx_text_ellipsis(FONT_SMALL, row.x + 16, row.y + 28, row.w - 32, C_FAINT,
                                  L->hs.spare_count ? "Not set up yet" : "Not set up yet  \xe2\x80\xa2  no free slot");
                continue;
            }
            int hi = U.host_rows[i].i;
            const Host *h = &L->hs.list[hi];
            gfx_text_ellipsis(FONT_BODY_BOLD, row.x + 16, row.y + 4, row.w - 200, C_TEXT, h->name);
            host_line(hi, sub, sizeof(sub));
            gfx_text_ellipsis(FONT_SMALL, row.x + 16, row.y + 28, row.w - 32, C_DIM, sub);
            if (hi == L->host && L->host_chosen) {
                int tw = gfx_text_width(FONT_SMALL, "Shown") + 22;
                pill(row.x + row.w - tw - 12, row.y + 12, "Shown", RGBA(0x3f, 0xb9, 0x7a, 40), C_ACCENT);
            }
        }
        bx -= hint_w("B", "Close");
        hint(bx, by, "B", "Close");
        bx -= 28 + hint_w("A", "Choose");
        hint(bx, by, "A", "Choose");
        return;
    }
    if (U.dlg == DLG_USERS) {
        gfx_text(FONT_SMALL, r.x + pad, y, C_DIM, "Games run on this user, each with its own save.");
        for (int i = 0; i < U.user_count; i++) {
            const UserRow *u = &U.users[i];
            Rect row = {r.x + pad, y + 32 + user_pitch() * i, w - pad * 2, user_pitch() - 6};
            bool off = u->demo_only || u->exploit || u->loader || u->kept_out || u->not_guard;
            gfx_round(row, 12, i == U.user_sel ? C_SEL : C_PANEL2);
            if (i == U.user_sel) gfx_round_outline(row, 12, 2, off ? C_FAINT : C_ACCENT);
            gfx_text_ellipsis(FONT_BODY_BOLD, row.x + 16, row.y + 4, row.w - 200, off ? C_FAINT : C_TEXT, u->name);
            char sub[200];
            snprintf(sub, sizeof(sub), "%.8s\xe2\x80\xa6%s%s", u->hex, u->hex + 24,
                     u->exploit ? "  \xe2\x80\xa2  has the exploit save" : u->loader ? "  \xe2\x80\xa2  protected by the loader" : "");
            gfx_text_ellipsis(FONT_SMALL, row.x + 16, row.y + row.h - 24, row.w - 32, C_DIM, sub);
            const char *tag = u->demo_only ? "Demo save" : u->play ? "Play user" : u->exploit || u->loader ? "Never"
                            : u->kept_out ? "Kept out" : NULL;
            if (tag) {
                Color tc = u->play ? C_ACCENT : C_FAINT;
                int tw = gfx_text_width(FONT_SMALL, tag) + 22;
                pill(row.x + row.w - tw - 12, row.y + (row.h - 28) / 2, tag, RGBA(tc & 0xff, (tc >> 8) & 0xff, (tc >> 16) & 0xff, 40), tc);
            }
        }
        const char *why = U.user_count ? users_change_blocker(&U.lib, &U.users[U.user_sel]) : NULL;
        int wy = y + 32 + user_pitch() * U.user_count + 6;
        if (why && (!U.users[U.user_sel].play || U.users[U.user_sel].demo_only))
            gfx_text_wrap(FONT_SMALL, r.x + pad, wy, w - pad * 2, 3, C_WARN, why);
        else if (U.user_count && U.users[U.user_sel].play && users_guard_differs(U.users[U.user_sel].hex))
            gfx_text_wrap(FONT_SMALL, r.x + pad, wy, w - pad * 2, 3, C_WARN,
                          "The loader isn't set up for this user yet. Press A to set it up.");
        else if (users_exploit_unknown())
            gfx_text_wrap(FONT_SMALL, r.x + pad, wy, w - pad * 2, 3, C_WARN,
                          "Don't pick the user whose Seedsow save has the exploit: SeedSwap writes to the play user's save.");
        else if (!U.lib.play_user_set)
            gfx_text_wrap(FONT_SMALL, r.x + pad, wy, w - pad * 2, 3, C_DIM, "Other users are never touched.");
        bx -= hint_w("B", "Close");
        hint(bx, by, "B", "Close");
        const UserRow *cur = U.user_count ? &U.users[U.user_sel] : NULL;
        if (cur && !cur->demo_only && !cur->exploit && !cur->loader && !cur->play) {
            const char *yl = cur->kept_out ? "Let in" : "Keep out";
            bx -= 28 + hint_w("Y", yl);
            hint(bx, by, "Y", yl);
        }
        if (cur && !why) {
            bx -= 28 + hint_w("A", "Choose");
            hint(bx, by, "A", "Choose");
        }
        return;
    }
    gfx_text_wrap(FONT_BODY, r.x + pad, y, w - pad * 2, body_h / gfx_line_height(FONT_BODY), C_DIM, U.dlg_body);
    if (U.dlg == DLG_CONFIRM) {
        const char *ok = U.act == ACT_SWITCH ? "Switch" : U.act == ACT_RESTORE ? "Restore" : U.act == ACT_DLC_ADD ? "Add"
                       : U.act == ACT_DLC_REMOVE ? "Remove" : U.act == ACT_COMPAT_REVERT ? "Remove"
                       : U.act == ACT_CONVERT ? (U.plan.programs[U.plan_prog].library >= 0 ? "Update" : "Convert")
                       : U.act == ACT_DELETE_DUMPS ? "Delete" : U.act == ACT_SET_USER ? "Choose"
                       : U.act == ACT_ADD_HOST ? "Set up" : U.act == ACT_MOVE ? "Move" : "Back up";
        const char *no = U.act == ACT_DELETE_DUMPS ? "Keep" : "Cancel";
        bx -= hint_w("B", no);
        hint(bx, by, "B", no);
        bx -= 28 + hint_w("A", ok);
        hint(bx, by, "A", ok);
    } else {
        bx -= hint_w("A", "OK");
        hint(bx, by, "A", "OK");
    }
}

static void redraw(void);
static const char *when_text(void);

static bool progress_cb(float frac, const char *text) {
    uint64_t now = plat_ms();
    U.prog = frac;
    snprintf(U.prog_text, sizeof(U.prog_text), "%s", text ? text : "");
    if (now - U.prog_last < 60 && frac < 1.f && U.prog_last) return true;
    U.prog_last = now;
    redraw();
    return true;
}

static void begin_progress(const char *title) {
    U.dlg = DLG_PROGRESS;
    U.dlg_color = C_ACCENT;
    U.prog = -1;
    U.prog_last = 0;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "%s", title);
    snprintf(U.prog_text, sizeof(U.prog_text), "Starting…");
    redraw();
}

static void run_action(void) {
    char err[400] = {0};
    Library *L = &U.lib;
    switch (U.act) {
    case ACT_SWITCH: {
        Game *to = &L->games[U.act_arg];
        char name[128];
        snprintf(name, sizeof(name), "%s", to->name);
        begin_progress("Switching games");
        bool ok = lib_switch(L, U.act_arg, progress_cb, err, sizeof(err));
        char note[260];  // taken before the reload, which starts the library afresh
        snprintf(note, sizeof(note), "%s%s", L->switch_note[0] ? "\n\n" : "", L->switch_note);
        bool backed_up = L->switch_backed_up;
        reload();
        if (ok)
            message(C_ACCENT, "Switched", "%s is ready. %s\n\nThe other game's save is kept for when you switch "
                    "back.%s", name, when_text(), note);
        else
            message(C_BAD, "Switch failed", "%s\n\n%s", err,
                    backed_up ? "Your save was backed up first (Saves tab)." : "Nothing was changed.");
        break;
    }
    case ACT_BACKUP: {
        begin_progress("Backing up the save");
        bool ok = saves_backup_now(L, "by hand", progress_cb, err, sizeof(err));
        reload();
        if (ok) message(C_ACCENT, "Backed up", "Saved to SD:/seedswap/backups.");
        else message(C_BAD, "Backup failed", "%s", err);
        break;
    }
    case ACT_RESTORE: {
        begin_progress("Restoring the save");
        bool ok = saves_restore(L, U.act_arg, progress_cb, err, sizeof(err));
        reload();
        if (ok) message(C_ACCENT, "Restored", "Backup restored. The save it replaced was backed up too.");
        else message(C_BAD, "Restore failed", "%s\n\nYour save is unchanged.", err);
        break;
    }
    case ACT_COMPAT: {
        char name[128];
        bool automatic = U.compat_auto_run;
        U.compat_auto_run = false;
        snprintf(name, sizeof(name), "%s", L->games[U.act_arg].name);
        begin_progress("Applying Nintendo's patches");
        bool ok = lib_compat(L, U.act_arg, true, progress_cb, err, sizeof(err));
        CompatInfo info = L->games[U.act_arg].compat;
        reload();
        if (!ok)
            message(C_BAD, "Patches not applied", "%s\n\n%s is unchanged.", err, name);
        else if (info.state == COMPAT_ST_APPLIED)
            message(C_ACCENT, "Patches applied", "Applied Nintendo's %d patch%s for %s (system %s).%s", info.changes,
                    info.changes == 1 ? "" : "es", name, info.version,
                    automatic ? " This is done once for every new game." : "");
        else if (!automatic)
            message(C_INFO, "Nothing to patch", "Nintendo has no patches for %s (system %s).", name, info.version);
        else
            U.dlg = DLG_NONE;
        break;
    }
    case ACT_COMPAT_REVERT: {
        char name[128];
        snprintf(name, sizeof(name), "%s", L->games[U.act_arg].name);
        bool ok = lib_compat_revert(L, U.act_arg, err, sizeof(err));
        reload();
        if (ok) message(C_ACCENT, "Patches removed", "%s is back to its unpatched files. Press X to patch it again.", name);
        else message(C_BAD, "Not removed", "%s", err);
        break;
    }
    case ACT_DLC_ADD: {
        char name[128];
        snprintf(name, sizeof(name), "%s", L->games[U.act_arg].name);
        begin_progress("Adding DLC");
        DlcResult res;
        const char *siblings[MAX_NSPS];   // the game's other DLC dumps on the card
        int nsib = 0;
        uint64_t game = 0;
        for (int i = 0; i < U.nsp_count; i++)
            if (!strcmp(U.nsps[i].path, U.act_path)) game = nsp_base_id(U.nsps[i].title_id);
        for (int i = 0; game && i < U.nsp_count; i++)
            if (U.nsps[i].kind == NSP_DLC && nsp_base_id(U.nsps[i].title_id) == game && strcmp(U.nsps[i].path, U.act_path))
                siblings[nsib++] = U.nsps[i].path;
        bool ok = lib_dlc_add(L, U.act_arg, U.act_path, U.keys_path, siblings, nsib, progress_cb, &res, err, sizeof(err));
        bool active = L->games[U.act_arg].active;
        reload();
        if (ok && res.warning[0])
            message(C_WARN, "Added, but check it", "Added %s to %s.\n\n%s", res.name, name, res.warning);
        else if (ok)
            message(C_ACCENT, "DLC added", "Added %s to %s. %s", res.name, name,
                    active ? when_text() : "It's there next time you switch to the game.");
        else
            message(C_BAD, "Not added", "%s\n\n%s wasn't changed.", err, name);
        break;
    }
    case ACT_DLC_REMOVE: {
        char name[128];
        snprintf(name, sizeof(name), "%s", L->games[U.act_arg].name);
        bool ok = lib_dlc_remove(L, U.act_arg, err, sizeof(err));
        reload();
        if (ok) message(C_ACCENT, "DLC removed", "%s no longer has its DLC.", name);
        else message(C_BAD, "Not removed", "%s", err);
        break;
    }
    case ACT_CONVERT: {
        const ConvertProgram *p = &U.plan.programs[U.plan_prog];
        char title[160];
        snprintf(title, sizeof(title), "Converting %s", p->name);
        begin_progress(title);
        ConvertResult res;
        plat_keep_awake(true);
        bool ok = convert_run(L, &U.plan, U.plan_prog, U.allow_older, U.keys_path, progress_cb, &res, err, sizeof(err));
        plat_keep_awake(false);
        reload();
        if (!ok) {
            message(C_BAD, "Not converted", "%s\n\n%s", err,
                    p->library >= 0 ? "The game in the library wasn't changed." : "Nothing was added to the library.");
            break;
        }
        char size[32], extra[1000] = "", place[64] = "";
        fmt_bytes((int64_t)res.romfs_size, size, sizeof(size));
        int gi = find_game(res.id);
        bool active = gi >= 0 && U.lib.games[gi].active;
        snprintf(extra, sizeof(extra), "%s%s%sNintendo's patches: %s.%s%s%s%s", res.fixes[0] ? "Fixes: " : "", res.fixes,
                 res.fixes[0] ? ".\n" : "", res.compat, res.dlc[0] ? "\nDLC: " : "", res.dlc,
                 res.warnings[0] ? "\n\n" : "", res.warnings);
        if (res.replaced) snprintf(place, sizeof(place), ", replacing %s (save kept)", res.replaced_version);
        U.del_bytes = 0;
        int ndel = 0;
        memset(U.del_paths, 0, sizeof(U.del_paths));
        const char *paths[2] = {U.plan.base_path, strcmp(U.plan.update_path, U.plan.base_path) ? U.plan.update_path : ""};
        for (int k = 0; k < 2; k++) {
            int64_t sz = 0;
            bool d = false;
            if (paths[k][0] && vfs_stat(VFS_SD, paths[k], &d, &sz) == 0) {
                snprintf(U.del_paths[ndel++], sizeof(U.del_paths[0]), "%s", paths[k]);
                U.del_bytes += sz;
            }
        }
        char freed[32], head[600];
        fmt_bytes(U.del_bytes, freed, sizeof(freed));
        snprintf(head, sizeof(head), "%s %s is in the library%s (%s, took %d:%02d). %s", res.name, res.version, place,
                 size, res.seconds / 60, res.seconds % 60, active ? when_text() : "Switch to it in the Library.");
        snprintf(U.dlg_title, sizeof(U.dlg_title), "%s %s", res.replaced ? "Updated:" : "Converted:", res.name);
        if (!ndel) {
            message(C_ACCENT, U.dlg_title, "%s\n\n%s", head, extra);
            break;
        }
        U.dlg = DLG_CONFIRM;
        U.act = ACT_DELETE_DUMPS;
        U.dlg_color = C_ACCENT;
        snprintf(U.dlg_body, sizeof(U.dlg_body), "%s\n\n%s\n\nDelete the dump%s to free %s?", head, extra,
                 ndel == 1 ? "" : "s", freed);
        return;
    }
    case ACT_DELETE_DUMPS: {
        int done = 0;
        for (int k = 0; k < 2; k++)
            if (U.del_paths[k][0] && vfs_delete(VFS_SD, U.del_paths[k]) == 0) {
                app_log("deleted the dump %s after converting it", U.del_paths[k]);
                done++;
            }
        reload();
        if (U.sel[TAB_CONVERT] >= U.nsp_count) U.sel[TAB_CONVERT] = U.nsp_count ? U.nsp_count - 1 : 0;
        message(C_ACCENT, "Dumps deleted", "Deleted %d file%s. The game stays in the library.", done, done == 1 ? "" : "s");
        break;
    }
    case ACT_ADD_HOST: {
        AppEntry app = U.apps[U.act_arg];
        char name[64];
        snprintf(name, sizeof(name), "%s", app.name[0] ? app.name : "The game");
        bool ok = lib_host_add(L, app.id, app.name, err, sizeof(err));
        reload();
        if (ok)
            message(C_ACCENT, "Almost ready", "Restart and run the launcher (A). Then start %s once from the HOME menu "
                    "and open SeedSwap again.", name);
        else message(C_BAD, "Not set up", "%s", err);
        break;
    }
    case ACT_MOVE: {
        char name[128], host[64];
        snprintf(name, sizeof(name), "%s", L->games[U.act_arg].name);
        snprintf(host, sizeof(host), "%s", lib_host(L)->name);
        begin_progress("Moving the game");
        bool ok = lib_move(L, U.act_arg, progress_cb, err, sizeof(err));
        char note[260];
        snprintf(note, sizeof(note), "%s%s", L->switch_note[0] ? "\n\n" : "", L->switch_note);
        reload();
        if (ok) message(C_ACCENT, "Moved", "%s is in %s now, with its save. %s%s", name, host, when_text(), note);
        else message(C_BAD, "Move failed", "%s", err);
        break;
    }
    case ACT_SET_USER: {
        UserRow row = U.users[U.act_arg];
        bool ok = users_set_play(L, &row, err, sizeof(err));
        reload();
        const char *running = U.lib.loader != LOADER_OFF && U.lib.loader_guard ? U.lib.loader_guard_user : "";
        bool restart = users_guard_follows() && running[0] && strcmp(running, row.hex);
        if (ok)
            message(C_ACCENT, "Play user set", "Games now run on %s.%s%s", row.name,
                    restart ? "\n\nRestart and run the launcher (A) before switching games." : "",
                    U.lib.play_user_has_save ? "" : "\n\nStart the host game on this user once before switching games.");
        else message(C_BAD, "Not changed", "%s", err);
        break;
    }
    default: U.dlg = DLG_NONE;
    }
    U.act = ACT_NONE;
}

// The play-user list (users.h). keep: the user to select again ("" = the play user).
static void open_users_at(const char *keep) {
    U.user_count = users_rows(&U.lib, U.users, USERS_MAX);
    if (!U.user_count) {
        message(C_WARN, "No users to pick from", "Start %s once on the user you want to play on, then come back here.",
                lib_host(&U.lib)->name);
        return;
    }
    U.user_sel = -1;
    for (int i = 0; i < U.user_count && U.user_sel < 0; i++)
        if (keep[0] ? !strcmp(U.users[i].hex, keep) : U.users[i].play) U.user_sel = i;
    for (int i = 0; i < U.user_count && U.user_sel < 0; i++)  // else the first one that can be chosen
        if (!users_change_blocker(&U.lib, &U.users[i])) U.user_sel = i;
    if (U.user_sel < 0) U.user_sel = 0;
    U.dlg = DLG_USERS;
    U.dlg_color = C_ACCENT;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "Play user");
}

static void open_users(void) { open_users_at(""); }

static void users_choose(void) {
    const UserRow *u = &U.users[U.user_sel];
    if (users_change_blocker(&U.lib, u)) return;   // the list says why
    U.act = ACT_SET_USER;
    U.act_arg = U.user_sel;
    U.dlg = DLG_CONFIRM;
    U.dlg_color = C_ACCENT;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "Play on %s?", u->name);
    const char *guard = U.lib.loader != LOADER_OFF && U.lib.loader_guard ? U.lib.loader_guard_user : U.lib.image_guard_user;
    char note[400] = "";
    if (users_guard_follows() && guard[0] && strcmp(guard, u->hex))
        snprintf(note, sizeof(note), "\n\nThe loader will be set up for %s. Restart and run the launcher (A) before "
                 "switching games.", u->name);
    else if (guard[0] && !u->guard)
        snprintf(note, sizeof(note), "\n\nThe loader is set up for another user (%.8s\xe2\x80\xa6), so switching won't "
                 "work until it's set up for %s.", guard, u->name);
    if (u->play) snprintf(U.dlg_title, sizeof(U.dlg_title), "Set up the loader for %s?", u->name);
    if (u->play)
        snprintf(U.dlg_body, sizeof(U.dlg_body), "The loader will be set up for %s again. Restart and run the "
                 "launcher (A) before switching games.", u->name);
    else
        snprintf(U.dlg_body, sizeof(U.dlg_body), "Games will run on %s, each with its own save. Its save is backed up "
                 "before every switch.%s", u->name, note);
}

static void users_keep_out(void) {
    char err[400], keep[33];
    UserRow row = U.users[U.user_sel];
    if (row.demo_only) return;
    snprintf(keep, sizeof(keep), "%s", row.hex);
    if (!users_toggle_kept_out(&U.lib, &row, err, sizeof(err))) {
        message(C_WARN, "Not changed", "%s", err);
        return;
    }
    reload();
    open_users_at(keep);
}

// ---- header and footer
static void draw_header(void) {
    // logo: a seed
    gfx_circle(54, 46, 16, C_ACCENT);
    gfx_circle(49, 41, 6, RGBA(255, 255, 255, 90));
    int x = 82;
    x += gfx_text(FONT_TITLE, x, 28, C_TEXT, "SeedSwap");
    gfx_text(FONT_SMALL, x + 10, 36, C_FAINT, "v" SEEDSWAP_VERSION);

    // tabs
    int tw = 0;
    for (int i = 0; i < TAB_COUNT; i++) tw += gfx_text_width(FONT_BODY_BOLD, TAB_NAMES[i]) + 44;
    int tx = 640 - tw / 2;
    button_glyph(tx - 46, 32, "L", C_FAINT);
    for (int i = 0; i < TAB_COUNT; i++) {
        int w = gfx_text_width(FONT_BODY_BOLD, TAB_NAMES[i]) + 44;
        Rect r = {tx, 24, w, 44};
        if (i == U.tab) {
            gfx_round(r, 22, C_SEL);
            gfx_round((Rect){r.x + w / 2 - 14, r.y + r.h - 5, 28, 4}, 2, C_ACCENT);
        }
        text_center(FONT_BODY_BOLD, r, i == U.tab ? C_TEXT : C_FAINT, TAB_NAMES[i]);
        tx += w;
    }
    button_glyph(tx + 18, 32, "R", C_FAINT);

    // play user + SD
    Library *L = &U.lib;
    char free_s[32], line[64];
    fmt_bytes(L->sd_free, free_s, sizeof(free_s));
    snprintf(line, sizeof(line), "SD: %s free", free_s);
    const char *who = L->play_user_set ? L->play_user_name : "No play user";
    int nw = gfx_text_width(FONT_BODY_BOLD, who), lw = gfx_text_width(FONT_SMALL, line);
    int rw = nw > lw ? nw : lw, rx = SCREEN_W - 40 - rw;
    gfx_text(FONT_BODY_BOLD, SCREEN_W - 40 - nw, 20, L->play_user_set ? C_TEXT : C_WARN, who);
    gfx_text(FONT_SMALL, SCREEN_W - 40 - lw, 48, C_FAINT, line);
    Rect av = {rx - 52, 24, 40, 40};
    gfx_round(av, 20, L->play_user_has_save ? HEX(0x3d6f7a) : C_FAINT);
    char ini[4];
    initials(L->play_user_set ? who : "?", ini);
    text_center(FONT_BODY_BOLD, av, C_TEXT, ini);
    gfx_fill((Rect){32, 86, SCREEN_W - 64, 1}, C_LINE);
}

typedef struct { const char *btn, *label; } Hint;

static void draw_footer(const Hint *h, int n, const char *status, Color sc) {
    gfx_fill((Rect){32, 664, SCREEN_W - 64, 1}, C_LINE);
    if (status) {
        gfx_circle(44, 692, 5, sc);
        gfx_text_ellipsis(FONT_SMALL, 58, 680, 560, C_DIM, status);
    }
    int x = SCREEN_W - 40;
    for (int i = n - 1; i >= 0; i--) {
        x -= hint_w(h[i].btn, h[i].label);
        hint(x, 678, h[i].btn, h[i].label);
        x -= 26;
    }
}

static void loader_status(const char **text, Color *c) {
    const Library *L = &U.lib;
    // nothing to say when all is well; only what needs doing
    *text = NULL;
    *c = C_DIM;
    if (L->journal_pending) { *text = "The last switch didn't finish. See the Log tab."; *c = C_BAD; }
    else if (L->loader == LOADER_FIXED) { *text = "Reboot before switching games."; *c = C_WARN; }
    else if (L->loader == LOADER_OFF && !L->live_sizes) { *text = "This loader can't switch games."; *c = C_WARN; }
}

// when the switched game starts, for the dialogs
static const char *when_text(void) {
    static char buf[200];
    const Library *L = &U.lib;
    if (!L->play_user_set)
        snprintf(buf, sizeof(buf), "Choose who plays first: Saves tab, Y.");
    else if (L->loader == LOADER_LIVE)
        snprintf(buf, sizeof(buf), "Start %s on %s to play.", lib_host(L)->name, L->play_user_name);
    else
        snprintf(buf, sizeof(buf), "Run the launcher (A), then start %s on %s to play.", lib_host(L)->name,
                 L->play_user_name);
    return buf;
}

// ---- Library
static void draw_library(void) {
    Library *L = &U.lib;
    const int rowh = 92, per = LIST_H / rowh;
    ensure_visible(TAB_LIBRARY, L->count, per);
    Rect area = {LEFT_X, LIST_Y, LEFT_W, per * rowh};
    for (int k = 0; k < per && U.scroll[TAB_LIBRARY] + k < L->count; k++) {
        int i = U.scroll[TAB_LIBRARY] + k;
        const Game *g = &L->games[i];
        Rect r = {area.x, area.y + k * rowh, area.w, rowh - 8};
        bool sel = i == U.sel[TAB_LIBRARY];
        gfx_round(r, 16, sel ? C_SEL : C_PANEL);
        if (sel) gfx_round_outline(r, 16, 2, C_ACCENT);
        game_icon(g, (Rect){r.x + 8, r.y + 6, 72, 72}, false);
        int bx = r.x + r.w - 16;
        if (g->active) {
            int w = gfx_text_width(FONT_SMALL, "ACTIVE") + 22;
            bx -= w;
            pill(bx, r.y + 14, "ACTIVE", C_ACCENT, C_ACCENT_DK);
            bx -= 8;
        } else if (!g->builtin && g->host >= 0) {   // in another host's folder
            int w = gfx_text_width(FONT_SMALL, "OTHER HOST") + 22;
            bx -= w;
            pill(bx, r.y + 14, "OTHER HOST", C_PANEL2, C_DIM);
            bx -= 8;
        } else if (!g->builtin && !g->parked) {
            int w = gfx_text_width(FONT_SMALL, "NO FILES") + 22;
            bx -= w;
            pill(bx, r.y + 14, "NO FILES", RGBA(239, 111, 108, 50), C_BAD);
            bx -= 8;
        }
        gfx_text_ellipsis(FONT_BODY_BOLD, r.x + 96, r.y + 14, bx - (r.x + 96) - 8, C_TEXT, g->name);
        char sub[200];
        const char *st;
        Color sc;
        status_style(g, &st, &sc);
        snprintf(sub, sizeof(sub), "%s%s%s", g->publisher[0] ? g->publisher : (g->builtin ? "" : "Unknown publisher"),
                 g->version[0] ? "  \xe2\x80\xa2  " : "", g->version);
        gfx_circle(r.x + 102, r.y + 58, 5, sc);
        gfx_text_ellipsis(FONT_SMALL, r.x + 114, r.y + 46, r.w - 130, C_DIM, sub[0] ? sub : st);
    }
    scrollbar(area, U.scroll[TAB_LIBRARY], L->count, per);

    // detail
    Rect d = {RIGHT_X, LIST_Y, RIGHT_W, LIST_H - 8};
    panel(d);
    if (!L->count) {
        if (!L->host_chosen) {
            gfx_text(FONT_BODY_BOLD, d.x + 28, d.y + 28, C_TEXT, "No host yet");
            gfx_text_wrap(FONT_SMALL, d.x + 28, d.y + 64, d.w - 56, 6, C_DIM,
                          "Your games run as an installed game, the host. Press - to pick one.");
        }
        return;
    }
    const Game *g = &L->games[U.sel[TAB_LIBRARY]];
    game_icon(g, (Rect){d.x + 24, d.y + 24, 176, 176}, true);
    int tx = d.x + 224, tw = d.w - 248;
    int y = d.y + 26;
    y += gfx_text_wrap(FONT_TITLE, tx, y, tw, 2, C_TEXT, g->name);
    gfx_text_ellipsis(FONT_BODY, tx, y + 4, tw, C_DIM, g->publisher[0] ? g->publisher : "Publisher unknown");
    y += gfx_line_height(FONT_BODY) + 16;
    const char *st;
    Color sc;
    status_style(g, &st, &sc);
    int px = tx;
    px += pill(px, y, st, RGBA(sc & 0xff, (sc >> 8) & 0xff, (sc >> 16) & 0xff, 45), sc) + 8;
    const Host *gh = lib_game_host(L, g);
    if (g->active) pill(px, y, "Active", C_ACCENT, C_ACCENT_DK);
    else if (gh && !g->builtin) {
        char in[80];
        snprintf(in, sizeof(in), "In %s", gh->name);
        pill(px, y, in, C_PANEL2, C_DIM);
    } else if (g->parked) pill(px, y, "On SD card", C_PANEL2, C_DIM);

    // facts
    char v[160], a[32], b[32];
    int fy = d.y + 224, fx = d.x + 28, fw = d.w - 56;
    if (g->builtin) snprintf(v, sizeof(v), "Installed on the console");
    else if (g->romfs_size >= 0) {
        fmt_bytes(g->romfs_size + (g->exefs_size > 0 ? g->exefs_size : 0), a, sizeof(a));
        snprintf(v, sizeof(v), "%s", a);
    } else snprintf(v, sizeof(v), "Files not on the SD card");
    info_row(fx, fy, fw, "Game files", v, g->builtin || gh || g->parked ? C_TEXT : C_BAD);
    fy += 34;
    if (g->active || (gh && !g->builtin)) snprintf(v, sizeof(v), "In use by %s", L->play_user_name);
    else if (g->slot) {
        fmt_bytes(g->slot_stats.bytes, b, sizeof(b));
        snprintf(v, sizeof(v), "Kept (%s)", b);
    } else snprintf(v, sizeof(v), "None yet");
    info_row(fx, fy, fw, "Save", v, g->slot || g->active || gh ? C_TEXT : C_DIM);
    fy += 34;
    if (g->title_id) {
        // "v4.0.1 (1507328)": the display version and, when it says more, the package's version number
        char num[40] = "";
        if (g->version_code > 0 && version_code_from_text(g->version) != g->version_code)
            snprintf(num, sizeof(num), " (%lld)", (long long)g->version_code);
        snprintf(v, sizeof(v), "%016llX%s%s%s", (unsigned long long)g->title_id, g->version[0] ? "  \xe2\x80\xa2  " : "",
                 g->version, g->version[0] ? num : "");
        info_row(fx, fy, fw, "Title ID", v, C_TEXT);
        fy += 34;
    }
    if (g->added[0]) {
        // an entry that an update replaced says so, and what it was before
        if (g->prev_version[0])
            snprintf(v, sizeof(v), "%s, was %s%s%s", g->added, g->prev_version, g->source[0] ? ", from " : "", g->source);
        else snprintf(v, sizeof(v), "%s%s%s", g->added, g->source[0] ? " from " : "", g->source);
        info_row(fx, fy, fw, g->prev_version[0] ? "Updated" : "Added", v, C_TEXT);
        fy += 34;
    }
    if (!g->builtin && !g->unknown && (gh || g->parked)) {
        Color cc = C_DIM;
        compat_text(g, v, sizeof(v), &cc);
        info_row(fx, fy, fw, "Patches", v, cc);
        fy += 34;
    }
    if (g->dlc[0]) {
        snprintf(v, sizeof(v), "%s built in", g->dlc_names[0] ? g->dlc_names : g->dlc);
        info_row(fx, fy, fw, "DLC", v, C_ACCENT);
        fy += 34;
    }
    if (g->has_compat_words) {  // the words themselves are in game.ini and the log
        info_row(fx, fy, fw, "Settings", "Nintendo's for this game", C_TEXT);
        fy += 34;
    }
    // action (sized first, so the notes above it get only the room that is left)
    Rect act = {d.x + 24, d.y + d.h - 96, d.w - 48, 72};
    const char *why = lib_switch_blocker(L, U.sel[TAB_LIBRARY]);
    const char *ntitle = NULL, *nbody = NULL;
    Color ncol = C_ACCENT;
    char nb[200], mt[96];
    bool elsewhere = gh && !g->builtin && L->host_chosen && g->host != L->host;
    const char *mwhy = elsewhere ? lib_move_blocker(L, U.sel[TAB_LIBRARY]) : NULL;
    if (g->active) {
        snprintf(nb, sizeof(nb), "%s", when_text());
        ntitle = "Active game", nbody = nb;
    } else if (elsewhere) {
        snprintf(mt, sizeof(mt), "In %s", gh->name);
        snprintf(nb, sizeof(nb), "%s", mwhy ? mwhy : "Press A to move it here, with its save.");
        ntitle = mt, nbody = nb, ncol = mwhy ? C_WARN : C_INFO;
    } else if (why) {
        ntitle = "Can't switch yet", nbody = why, ncol = C_WARN;
    }
    int act_top = nbody ? act.y + act.h - notice_height(act.w, ntitle, nbody) : act.y;
    int note_lines = (act_top - 10 - (fy + 6)) / gfx_line_height(FONT_SMALL);
    if (g->notes[0] && note_lines > 0) gfx_text_wrap(FONT_SMALL, fx, fy + 6, fw, note_lines > 2 ? 2 : note_lines, C_DIM, g->notes);
    if (nbody) {
        notice_bottom(act.x, act.y + act.h, act.w, ncol, ntitle, nbody);
    } else {
        gfx_round(act, 18, C_ACCENT);
        snprintf(v, sizeof(v), "Switch to %s", g->name);
        int w = glyph_w("A") + 12 + gfx_text_width(FONT_BODY_BOLD, v);
        if (w > act.w - 40) w = act.w - 40;
        int x = act.x + (act.w - w) / 2;
        button_glyph(x, act.y + 22, "A", C_ACCENT_DK);
        gfx_text_ellipsis(FONT_BODY_BOLD, x + glyph_w("A") + 12, act.y + 22, w - glyph_w("A") - 12, C_ACCENT_DK, v);
        // the glyph's letter is dark-on-dark; redraw it light
        text_center(FONT_SMALL, (Rect){x, act.y + 21, glyph_w("A"), 28}, C_ACCENT, "A");
    }
}

// X on a game: look at Nintendo's patches for it, apply them, or offer to take them off.
static void library_compat(void) {
    Library *L = &U.lib;
    if (!L->count) return;
    int i = U.sel[TAB_LIBRARY];
    Game *g = &L->games[i];
    if (g->builtin) { message(C_INFO, g->name, "The host game doesn't need patches."); return; }
    if (g->unknown || !(g->host >= 0 || g->parked)) {
        message(C_WARN, "No files to patch", "%s's files aren't on the SD card.", g->name);
        return;
    }
    if (g->compat.state == COMPAT_ST_APPLIED && g->compat.have_original) {
        U.dlg = DLG_CONFIRM;
        U.act = ACT_COMPAT_REVERT;
        U.act_arg = i;
        U.dlg_color = C_WARN;
        snprintf(U.dlg_title, sizeof(U.dlg_title), "Remove Nintendo's patches from %s?", g->name);
        snprintf(U.dlg_body, sizeof(U.dlg_body), "This puts back %s's files without its %d patch%s. Only useful for "
                 "testing.", g->name, g->compat.changes, g->compat.changes == 1 ? "" : "es");
        return;
    }
    if (!L->patcher_present) {
        message(C_WARN, "Patcher file missing", "Nintendo's patcher isn't on the SD card (SD:" COMPAT_PATCHER "). "
                "Run compat_sd.py --provision on the PC to copy it from your system dump.");
        return;
    }
    U.act = ACT_COMPAT;
    U.act_arg = i;
    run_action();
}

static void open_hosts(void) {
    Library *L = &U.lib;
    U.host_row_count = 0;
    for (int i = 0; i < L->hs.count; i++) U.host_rows[U.host_row_count++] = (HostRow){false, i};
    U.app_count = L->hs.ini ? lib_host_candidates(L, U.apps, 64) : 0;   // an image without host slots: Seedsow only
    for (int i = 0; i < U.app_count; i++) U.host_rows[U.host_row_count++] = (HostRow){true, i};
    U.host_sel = L->host_chosen ? L->host : 0;
    U.host_top = U.host_sel >= HOST_ROWS ? U.host_sel - HOST_ROWS + 1 : 0;
    U.dlg = DLG_HOSTS;
    U.dlg_color = C_ACCENT;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "Hosts");
}

static void add_host_confirm(int app) {
    const AppEntry *a = &U.apps[app];
    char hex[17];
    hosts_hex(a->id, hex);
    if (!U.lib.hs.spare_count) {
        message(C_WARN, "No free slot", "Every slot of the install image is in use. Build one with more slots on the PC "
                "(build_host_image.py).");
        return;
    }
    U.act = ACT_ADD_HOST;
    U.act_arg = app;
    U.dlg = DLG_CONFIRM;
    U.dlg_color = C_ACCENT;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "Use %s as a host?", a->name[0] ? a->name : hex);
    snprintf(U.dlg_body, sizeof(U.dlg_body), "It takes a free slot (%d left) and still plays as itself until you put a "
             "game in it.\n\nAfterwards: restart, run the launcher (A), start it once and open SeedSwap again.",
             U.lib.hs.spare_count);
}

static void hosts_choose(void) {
    Library *L = &U.lib;
    if (!U.host_row_count) return;
    int i = U.host_rows[U.host_sel].i;
    if (U.host_rows[U.host_sel].app) { add_host_confirm(i); return; }
    U.dlg = DLG_NONE;
    if (i != L->host || !L->host_chosen) {
        lib_set_host(L, i);
        reload();
        U.sel[TAB_LIBRARY] = lib_active(L) >= 0 ? lib_active(L) : 0;
    }
}

// a host whose own Nintendo settings the game would get too
static void settings_note(const Game *g, const Host *H, const char *extra, char *out, size_t n) {
    snprintf(out, n, "%s has Nintendo settings that %s doesn't (%s). %s gets them too. If it acts up, try another "
             "host.\n\n", H->name, g->name, extra, g->name);
}

static void library_activate(void) {
    Library *L = &U.lib;
    if (!L->count) return;
    int i = U.sel[TAB_LIBRARY];
    Game *g = &L->games[i];
    const char *why = lib_switch_blocker(L, i);
    if (g->active) { message(C_ACCENT, g->name, "%s", when_text()); return; }
    const Host *other = !g->builtin ? lib_game_host(L, g) : NULL;
    if (other && L->host_chosen && g->host != L->host) {   // in another host: move it here, with its save
        const char *mwhy = lib_move_blocker(L, i);
        if (mwhy) { message(C_WARN, "Can't move it yet", "%s", mwhy); return; }
        U.dlg = DLG_CONFIRM;
        U.act = ACT_MOVE;
        U.act_arg = i;
        char extra[120], note[300] = "";
        if (lib_host_adds_settings(g, lib_host(L), extra, sizeof(extra))) settings_note(g, lib_host(L), extra, note, sizeof(note));
        U.dlg_color = note[0] ? C_WARN : C_ACCENT;
        snprintf(U.dlg_title, sizeof(U.dlg_title), "Move %s to %s?", g->name, lib_host(L)->name);
        snprintf(U.dlg_body, sizeof(U.dlg_body), "%sIts save moves with it. %s plays as itself again. Close both games "
                 "first.", note, other->name);
        return;
    }
    if (why) { message(C_WARN, "Can't switch yet", "%s", why); return; }
    int a = lib_active(L);
    const char *from = a >= 0 ? L->games[a].name : "the current game";
    char warn[820] = "", extra[120];
    if (!strcmp(g->status, "crashes") || !strcmp(g->status, "broken") || !strcmp(g->status, "partial")) {
        size_t nl = strlen(g->notes);
        bool dot = nl && (g->notes[nl - 1] == '.' || g->notes[nl - 1] == '!');
        snprintf(warn, sizeof(warn), "%s %s on Seedsow. %s%s\n\n", g->name,
                 g->status[0] == 'p' ? "only partly works" : "doesn't work", g->notes, nl && !dot ? "." : "");
    }
    if (lib_host_adds_settings(g, lib_host(L), extra, sizeof(extra)))
        settings_note(g, lib_host(L), extra, warn + strlen(warn), sizeof(warn) - strlen(warn));
    U.dlg = DLG_CONFIRM;
    U.act = ACT_SWITCH;
    U.act_arg = i;
    U.dlg_color = warn[0] ? C_WARN : C_ACCENT;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "Switch to %s?", g->name);
    snprintf(U.dlg_body, sizeof(U.dlg_body), "%sYour save for %s is kept, and %s %s. %s", warn, from, g->name,
             g->slot ? "picks up where you left off" : "starts with a new save", when_text());
}

// ---- Convert
static int find_by_title(uint64_t tid) {
    for (int i = 0; i < U.lib.count; i++)
        if (U.lib.games[i].title_id && U.lib.games[i].title_id == tid) return i;
    return -1;
}

static const char *dump_kind(const NspInfo *n) {
    return n->with_update ? "Base game + update" : nsp_kind_name(n->kind);
}

static void draw_convert(void) {
    const int rowh = 84, per = LIST_H / rowh;
    ensure_visible(TAB_CONVERT, U.nsp_count, per);
    Rect area = {LEFT_X, LIST_Y, LEFT_W, per * rowh};
    if (!U.nsp_count) {
        panel((Rect){area.x, area.y, area.w, LIST_H - 8});
        gfx_text(FONT_BODY_BOLD, area.x + 28, area.y + 28, C_TEXT, "No game dumps found");
        gfx_text_wrap(FONT_SMALL, area.x + 28, area.y + 64, area.w - 56, 6, C_DIM,
                      "Put your dumps (.nsp, .nsz, .xci or .xcz) in SD:/seedswap/nsp or the root of the SD card, then "
                      "press Y.");
    }
    for (int k = 0; k < per && U.scroll[TAB_CONVERT] + k < U.nsp_count; k++) {
        int i = U.scroll[TAB_CONVERT] + k;
        NspInfo *n = &U.nsps[i];
        Rect r = {area.x, area.y + k * rowh, area.w, rowh - 8};
        bool sel = i == U.sel[TAB_CONVERT];
        gfx_round(r, 16, sel ? C_SEL : C_PANEL);
        if (sel) gfx_round_outline(r, 16, 2, C_ACCENT);
        Color kc = n->kind == NSP_BASE ? C_ACCENT : n->kind == NSP_UPDATE ? C_INFO : n->kind == NSP_DLC ? C_WARN : C_FAINT;
        gfx_round((Rect){r.x + 14, r.y + 14, 48, 48}, 12, RGBA(kc & 0xff, (kc >> 8) & 0xff, (kc >> 16) & 0xff, 45));
        text_center(FONT_BODY_BOLD, (Rect){r.x + 14, r.y + 14, 48, 48}, kc,
                    n->kind == NSP_BASE ? "G" : n->kind == NSP_UPDATE ? "U" : n->kind == NSP_DLC ? "D" : "?");
        gfx_text_ellipsis(FONT_BODY_BOLD, r.x + 78, r.y + 10, r.w - 96, C_TEXT, n->name[0] ? n->name : n->file);
        char sub[96], sz[32];
        fmt_bytes(n->size, sz, sizeof(sz));
        snprintf(sub, sizeof(sub), "%s%s%s  \xe2\x80\xa2  %s", dump_kind(n), n->version[0] ? "  \xe2\x80\xa2  " : "",
                 n->version, sz);
        gfx_text_ellipsis(FONT_SMALL, r.x + 78, r.y + 42, r.w - 96, C_DIM, sub);
    }
    scrollbar(area, U.scroll[TAB_CONVERT], U.nsp_count, per);

    Rect d = {RIGHT_X, LIST_Y, RIGHT_W, LIST_H - 8};
    panel(d);
    int x = d.x + 28, w = d.w - 56, y = d.y + 24;
    char v[200], sz[32];
    if (U.nsp_count) {
        NspInfo *n = &U.nsps[U.sel[TAB_CONVERT]];
        if (!n->inspected) nsp_inspect(n);
        y += gfx_text_wrap(FONT_TITLE, x, y, w, 2, C_TEXT, n->name[0] ? n->name : n->file) + 6;
        gfx_text_ellipsis(FONT_SMALL, x, y, w, C_FAINT, n->path);
        y += 40;
        snprintf(v, sizeof(v), "%s (%s)", dump_kind(n), n->container);
        info_row(x, y, w, "Content", v, C_TEXT);
        y += 36;
        if (n->title_id) snprintf(v, sizeof(v), "%016llX", (unsigned long long)n->title_id);
        else snprintf(v, sizeof(v), "unknown");
        info_row(x, y, w, "Title ID", v, C_TEXT);
        y += 36;
        fmt_bytes(n->size, sz, sizeof(sz));
        if (n->ok && !n->files) snprintf(v, sizeof(v), "%s, cartridge image", sz);
        else if (n->ok) snprintf(v, sizeof(v), "%s, %d NCA%s%s%s", sz, n->ncas, n->ncas == 1 ? "" : "s", n->ticket ? ", ticket" : ", no ticket",
                            n->cnmt ? "" : ", no CNMT");
        else snprintf(v, sizeof(v), "%s: %s", sz, n->err[0] ? n->err : "not read");
        info_row(x, y, w, "Package", v, n->ok ? C_TEXT : C_BAD);
        y += 36;
        int gi = n->title_id ? find_by_title(nsp_base_id(n->title_id)) : -1;
        Color lc = gi >= 0 ? C_ACCENT : C_DIM;
        if (gi >= 0 && n->kind != NSP_DLC) {
            // a base game or an update of a game the library holds: newer, the same, or older?
            const Game *lg = &U.lib.games[gi];
            const char *have = lg->version[0] ? lg->version : "version unknown";
            switch (lib_version_relation(lg, version_code_from_text(n->version))) {
            case VER_NEWER: snprintf(v, sizeof(v), "Newer than your %s", have); break;
            case VER_SAME: snprintf(v, sizeof(v), "Same as your %s", have); break;
            case VER_OLDER:
                snprintf(v, sizeof(v), "Older than your %s", have);
                lc = C_WARN;
                break;
            default: snprintf(v, sizeof(v), "In the library (%s)", have); break;
            }
        } else if (gi >= 0) {
            const Game *lg = &U.lib.games[gi];
            bool in = lib_dlc_built_in(lg, n->title_id);
            snprintf(v, sizeof(v), "%s: %s", lg->name, in ? "added" : "not added yet");
        } else if (n->kind == NSP_BASE) snprintf(v, sizeof(v), "Not in the library yet");
        else snprintf(v, sizeof(v), "Its game isn't in the library");
        info_row(x, y, w, "Library", v, lc);
        y += 36;
    } else {
        gfx_text(FONT_TITLE, x, y, C_TEXT, "Add games");
        y += 60;
    }
    snprintf(v, sizeof(v), "%s", U.keys_path[0] ? U.keys_path : "not found (SD:/switch/prod.keys)");
    info_row(x, y, w, "prod.keys", v, U.keys_path[0] ? C_ACCENT : C_WARN);
    const NspInfo *sel = U.nsp_count ? &U.nsps[U.sel[TAB_CONVERT]] : NULL;
    if (sel && sel->kind == NSP_DLC)
        notice_bottom(d.x + 24, d.y + d.h - 24, d.w - 48, C_ACCENT, "DLC",
               "Press A to add this DLC to its game. Press A again to remove it.");
    else if (sel && sel->kind == NSP_UPDATE)
        notice_bottom(d.x + 24, d.y + d.h - 24, d.w - 48, C_INFO, "Update",
               "Press A to update the game. The base game's dump has to be on the SD card too. Your save is kept.");
    else
        notice_bottom(d.x + 24, d.y + d.h - 24, d.w - 48, C_ACCENT, "Game",
               "Press A to add this game to the library. An update for it on the SD card is included.");
}

// A on a dump in the Convert tab: add-on content is built into (or taken out of) its game.
static void convert_confirm(int k);

// The dump of a base game and the newest update for it on the SD card, from the selected dump.
static bool pick_dumps(const NspInfo *n, const NspInfo **base, const NspInfo **update) {
    *base = *update = NULL;
    uint64_t game = n->title_id ? nsp_base_id(n->title_id) : 0;
    if (n->kind == NSP_UPDATE) *update = n;
    else *base = n;
    // a base game's dump that holds its update too: another update dump is used only when newer
    int64_t best = n->with_update ? version_code_from_text(n->version) : -1;
    for (int i = 0; game && i < U.nsp_count; i++) {
        const NspInfo *o = &U.nsps[i];
        if (!o->title_id || nsp_base_id(o->title_id) != game) continue;
        if (o->kind == NSP_BASE && !*base) *base = o;
        if (o->kind == NSP_UPDATE && n->kind != NSP_UPDATE && version_code_from_text(o->version) > best) {
            best = version_code_from_text(o->version);
            *update = o;
        }
        if (o->kind == NSP_UPDATE && n->kind != NSP_UPDATE && !*update && !n->with_update) *update = o;
    }
    return *base != NULL;
}

// A dump whose title the list does not know: a cartridge image (or a file named without its ID) tells only when it
// is opened with the keys.
static bool title_unknown(const NspInfo *o) { return o->ok && !o->title_id && o->kind != NSP_UPDATE && o->kind != NSP_DLC; }

// After planning a base game: an update dump of the same game on the card that is newer than the plan's update
// (or the update a cartridge carries) takes its place. Update dumps whose version the list does not know are
// opened to find out.
static void plan_newer_update(const NspInfo *base) {
    static ConvertPlan alt;
    for (int i = 0; i < U.nsp_count; i++) {
        NspInfo *o = &U.nsps[i];
        if (o != base && !o->inspected) nsp_inspect(o);   // the list reads a dump's header only when it is selected
        if (o == base || !o->ok || o->kind != NSP_UPDATE || !o->title_id || nsp_base_id(o->title_id) != U.plan.app_id)
            continue;
        int64_t v = version_code_from_text(o->version);
        if (U.plan.has_update && v >= 0 && v <= (int64_t)U.plan.update_version) continue;
        char e[400];
        progress_cb(-1, "Looking at another update on the card");
        if (convert_plan(&U.lib, base->path, o->path, U.keys_path, &alt, e, sizeof(e)) && alt.has_update &&
            (!U.plan.has_update || alt.update_version > U.plan.update_version))
            U.plan = alt;
    }
}

static void convert_dumps(const NspInfo *n) {
    if (!U.keys_path[0]) {
        message(C_WARN, "prod.keys is missing", "Put your console's prod.keys at SD:/switch/prod.keys.");
        return;
    }
    const NspInfo *base, *update;
    bool found_base = false;
    char err[400];
    if (!pick_dumps(n, &base, &update) && n->cnmts >= 2) {
        base = n;   // it may hold the base game too (read with the keys below: the plan says for sure)
        update = NULL;
    } else if (!base) {
        // an update whose base game's dump the list cannot name: try the dumps of unknown title (cartridge images)
        begin_progress("Looking for the base game");
        for (int i = 0; i < U.nsp_count && !found_base; i++) {
            NspInfo *o = &U.nsps[i];
            if (o != n && !o->inspected) nsp_inspect(o);
            if (!title_unknown(o)) continue;
            progress_cb(-1, o->name[0] ? o->name : o->file);
            if (convert_plan(&U.lib, o->path, NULL, U.keys_path, &U.plan, err, sizeof(err)) &&
                U.plan.app_id == nsp_base_id(n->title_id)) {
                base = o;
                found_base = true;
            }
        }
        U.dlg = DLG_NONE;
        if (!found_base) {
            message(C_WARN, "Base game needed", "%s is an update. Put the base game's dump (%016llX) on the SD card "
                    "too.", n->name, (unsigned long long)nsp_base_id(n->title_id));
            return;
        }
        update = n;
    }
    begin_progress("Reading the dumps");
    progress_cb(-1, update ? "The base game and its update" : "The base game");
    bool ok = convert_plan(&U.lib, base->path, update ? update->path : NULL, U.keys_path, &U.plan, err, sizeof(err));
    // a base dump the list could not match by title (a cartridge image), or one that carries its own update:
    // a newer update dump on the card is used instead
    if (ok && (!base->title_id || !strcmp(U.plan.update_path, U.plan.base_path))) plan_newer_update(base);
    U.dlg = DLG_NONE;
    if (!ok) {
        message(C_BAD, "The dump cannot be used", "%s", err);
        return;
    }
    U.allow_older = false;
    if (U.plan.count > 1) {
        U.dlg = DLG_CHOOSE;
        U.dlg_color = C_ACCENT;
        U.choose_sel = 0;
        snprintf(U.dlg_title, sizeof(U.dlg_title), "Which game?");
        return;
    }
    convert_confirm(0);
}

static void convert_confirm(int k) {
    const ConvertProgram *p = &U.plan.programs[k];
    U.plan_prog = k;
    const char *why = convert_blocker(&U.lib, &U.plan, k, true);
    if (why) {
        message(C_WARN, "Not possible right now", "%s", why);
        return;
    }
    char size[32], ver[96], lib[560] = "";
    fmt_bytes((int64_t)p->romfs_size, size, sizeof(size));
    if (U.plan.has_update && p->with_update)
        snprintf(ver, sizeof(ver), "%s%s (update v%u)", U.plan.display_version[0] ? "v" : "", U.plan.display_version,
                 U.plan.update_version);
    else snprintf(ver, sizeof(ver), "%s%s (base game v%u)", U.plan.display_version[0] ? "v" : "", U.plan.display_version,
                  U.plan.base_version);
    bool older = false;
    if (p->library >= 0) {
        const Game *g = &U.lib.games[p->library];
        older = p->relation == VER_OLDER;
        const char *have = g->version[0] ? g->version : "(version unknown)";
        if (older)
            snprintf(lib, sizeof(lib), "\n\nThis is older than your %s. Your save might not work with it.", have);
        else if (p->relation == VER_SAME)
            snprintf(lib, sizeof(lib), "\n\nYou already have %s. It will be rebuilt.", have);
        else snprintf(lib, sizeof(lib), "\n\nThis replaces your %s. Your save is kept.", have);
        if (g->active) snprintf(lib + strlen(lib), sizeof(lib) - strlen(lib), " It's backed up first.");
        if (g->dlc[0]) snprintf(lib + strlen(lib), sizeof(lib) - strlen(lib), " Its DLC is added again.");
    }
    U.allow_older = older;
    U.act = ACT_CONVERT;
    U.dlg = DLG_CONFIRM;
    U.dlg_color = older ? C_WARN : C_ACCENT;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "%s %s?", p->library >= 0 ? "Update" : "Convert", p->name);
    snprintf(U.dlg_body, sizeof(U.dlg_body), "%s, from %s%s (%s). This takes a few minutes for a big game.%s%s%s", ver,
             U.plan.base_file, U.plan.has_update && strcmp(U.plan.update_path, U.plan.base_path) ? " and its update" : "",
             size, p->note[0] ? "\n\nNote: " : "", p->note, lib);
}

static void convert_activate(void) {
    NspInfo *n = &U.nsps[U.sel[TAB_CONVERT]];
    if (!n->inspected) nsp_inspect(n);
    if (n->kind != NSP_DLC) {
        if (!n->ok) { message(C_BAD, "The dump cannot be used", "%s: %s", n->file, n->err); return; }
        convert_dumps(n);
        return;
    }
    if (!n->inspected) nsp_inspect(n);
    if (!n->ok) { message(C_BAD, "The dump cannot be used", "%s: %s", n->file, n->err); return; }
    int gi = n->title_id ? find_by_title(nsp_base_id(n->title_id)) : -1;
    if (gi < 0) {
        message(C_WARN, "Game not in the library", "Add the game (%016llX) to the library first.",
                (unsigned long long)nsp_base_id(n->title_id));
        return;
    }
    Game *g = &U.lib.games[gi];
    const char *why = lib_dlc_blocker(&U.lib, gi);
    if (why) { message(C_WARN, "Not possible right now", "%s", why); return; }
    U.act_arg = gi;
    snprintf(U.act_path, sizeof(U.act_path), "%s", n->path);
    U.dlg = DLG_CONFIRM;
    if (lib_dlc_built_in(g, n->title_id)) {
        if (!g->dlc_removable) {
            message(C_INFO, "Already added", "%s is part of %s and can't be removed here: there's no copy of the game "
                    "without it on the SD card.", n->name, g->name);
            return;
        }
        U.act = ACT_DLC_REMOVE;
        U.dlg_color = C_WARN;
        snprintf(U.dlg_title, sizeof(U.dlg_title), "Remove DLC from %s?", g->name);
        snprintf(U.dlg_body, sizeof(U.dlg_body), "This removes all of %s's DLC (%s). Your save isn't touched.", g->name,
                 g->dlc_names[0] ? g->dlc_names : n->name);
        return;
    }
    if (!U.keys_path[0]) {
        U.dlg = DLG_NONE;
        message(C_WARN, "prod.keys is missing", "Put your console's prod.keys at SD:/switch/prod.keys.");
        return;
    }
    U.act = ACT_DLC_ADD;
    U.dlg_color = C_ACCENT;
    snprintf(U.dlg_title, sizeof(U.dlg_title), "Add DLC to %s?", g->name);
    snprintf(U.dlg_body, sizeof(U.dlg_body), "%s will be added to %s. You can remove it again later.%s", n->name, g->name,
             g->host >= 0 ? "\n\nDon't do this while its host game is running." : "");
}

// ---- Saves
static void draw_saves(void) {
    Library *L = &U.lib;
    Rect c = {LEFT_X, LIST_Y, LEFT_W, LIST_H - 8};
    panel(c);
    int x = c.x + 28, w = c.w - 56, y = c.y + 24;
    char v[160], b[32];
    snprintf(v, sizeof(v), "%s save", lib_host(L)->name);
    gfx_text_ellipsis(FONT_TITLE, x, y, w, C_TEXT, v);
    y += 50;
    info_row(x, y, w, "Play user", L->play_user_set ? L->play_user_name : "Not chosen (press Y)",
             L->play_user_set ? C_TEXT : C_WARN);
    y += 34;
    if (L->play_user_set)
        snprintf(v, sizeof(v), "%.8s\xe2\x80\xa6%s", L->play_user_hex, L->play_user_hex + 24);
    else snprintf(v, sizeof(v), "-");
    info_row(x, y, w, "User ID", v, C_DIM);
    y += 34;
    if (!L->play_user_has_save) snprintf(v, sizeof(v), "No save yet");
    else if (!L->current_save_ok) snprintf(v, sizeof(v), "Could not read it");
    else {
        fmt_bytes(L->current_save.bytes, b, sizeof(b));
        snprintf(v, sizeof(v), "%d file%s, %s", L->current_save.files, L->current_save.files == 1 ? "" : "s", b);
    }
    info_row(x, y, w, "Now holds", v, L->current_save_ok ? C_TEXT : C_WARN);
    y += 34;
    int a = lib_active(L);
    info_row(x, y, w, "Belongs to", a >= 0 ? L->games[a].name : "-", C_TEXT);
    y += 50;
    gfx_text(FONT_BODY_BOLD, x, y, C_TEXT, "Kept game saves");
    y += 36;
    int shown = 0;
    for (int i = 0; i < L->count && y < c.y + c.h - 120; i++) {
        const Game *g = &L->games[i];
        if (!g->slot) continue;
        fmt_bytes(g->slot_stats.bytes, b, sizeof(b));
        snprintf(v, sizeof(v), "%d file%s, %s", g->slot_stats.files, g->slot_stats.files == 1 ? "" : "s", b);
        gfx_circle(x + 5, y + 13, 4, C_ACCENT);
        gfx_text_ellipsis(FONT_BODY, x + 20, y, w - 200, C_TEXT, g->name);
        gfx_text(FONT_SMALL, x + w - gfx_text_width(FONT_SMALL, v), y + 2, C_DIM, v);
        y += 32;
        shown++;
    }
    if (!shown) gfx_text(FONT_SMALL, x, y, C_DIM, "None yet.");
    if (!L->play_user_set)
        notice_bottom(c.x + 24, c.y + c.h - 24, c.w - 48, C_WARN, NULL, "Press Y to choose who plays.");

    // backups
    Rect d = {RIGHT_X, LIST_Y, RIGHT_W, LIST_H - 8};
    panel(d);
    snprintf(v, sizeof(v), "Backups (%d)", L->backup_count);
    gfx_text(FONT_TITLE, d.x + 28, d.y + 24, C_TEXT, v);
    gfx_text(FONT_SMALL, d.x + 28, d.y + 62, C_FAINT, "Made before every switch. The last 30 are kept.");
    const int rowh = 64, per = (d.h - 112) / rowh;
    ensure_visible(TAB_SAVES, L->backup_count, per);
    Rect area = {d.x + 20, d.y + 96, d.w - 40 - 10, per * rowh};
    if (!L->backup_count) gfx_text_wrap(FONT_BODY, area.x + 8, area.y + 8, area.w - 16, 3, C_DIM, "No backups yet. Press X to make one now.");
    for (int k = 0; k < per && U.scroll[TAB_SAVES] + k < L->backup_count; k++) {
        int i = U.scroll[TAB_SAVES] + k;
        const Backup *bk = &L->backups[i];
        Rect r = {area.x, area.y + k * rowh, area.w, rowh - 8};
        bool sel = i == U.sel[TAB_SAVES];
        gfx_round(r, 14, sel ? C_SEL : C_PANEL);
        if (sel) gfx_round_outline(r, 14, 2, C_ACCENT);
        int gi = find_game(bk->game);
        gfx_text(FONT_BODY_BOLD, r.x + 18, r.y + 14, C_TEXT, bk->when);
        fmt_bytes(bk->st.bytes, b, sizeof(b));
        snprintf(v, sizeof(v), "%s  \xe2\x80\xa2  %d file%s, %s", gi >= 0 ? L->games[gi].name : bk->game, bk->st.files,
                 bk->st.files == 1 ? "" : "s", b);
        gfx_text_ellipsis(FONT_SMALL, r.x + 220, r.y + 16, r.w - 236, C_DIM, v);
    }
    scrollbar(area, U.scroll[TAB_SAVES], L->backup_count, per);
}

// ---- Log
static void draw_log(void) {
    Rect d = {LEFT_X, LIST_Y, SCREEN_W - 64, LIST_H - 8};
    panel(d);
    if (!U.log) load_log();
    int lh = gfx_line_height(FONT_SMALL), per = (d.h - 40) / lh;
    int max_scroll = U.log_lines > per ? U.log_lines - per : 0;
    if (U.log_scroll > max_scroll) U.log_scroll = max_scroll;
    int first = U.log_lines - per - U.log_scroll;
    if (first < 0) first = 0;
    int line = 0, y = d.y + 20;
    for (char *p = U.log; *p && y < d.y + d.h - lh;) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (line >= first) {
            char buf[300];
            snprintf(buf, sizeof(buf), "%.*s", (int)(len < 299 ? len : 299), p);
            Color c = strstr(buf, "ERROR") ? C_BAD : strstr(buf, "] switch") ? C_ACCENT : C_DIM;
            gfx_text_ellipsis(FONT_SMALL, d.x + 24, y, d.w - 48, c, buf);
            y += lh;
        }
        line++;
        if (!nl) break;
        p = nl + 1;
    }
    if (!U.log_lines) gfx_text(FONT_BODY, d.x + 24, d.y + 24, C_DIM, "Nothing logged yet.");
    scrollbar((Rect){d.x + d.w - 20, d.y + 20, 0, d.h - 40}, max_scroll - U.log_scroll, U.log_lines, per);
}

// ---- frame
static void redraw(void) {
    Canvas *c = plat_frame_begin();
    gfx_begin(c);
    ui_draw();
    plat_frame_end();
}

void ui_draw(void) {
    gfx_clear_gradient(C_BG_TOP, C_BG_BOT);
    draw_header();
    const char *st;
    Color sc;
    loader_status(&st, &sc);
    switch (U.tab) {
    case TAB_LIBRARY: {
        draw_library();
        static char host_hint[96];
        snprintf(host_hint, sizeof(host_hint), "Host: %s", U.lib.host_chosen ? lib_host(&U.lib)->name : "none");
        Hint h[] = {{"A", "Switch"}, {"X", "Patches"}, {"-", host_hint}, {"Y", "Refresh"}, {"+", "Exit"}};
        draw_footer(h, 5, st, sc);
        break;
    }
    case TAB_CONVERT: {
        draw_convert();
        bool dlc = U.nsp_count && U.nsps[U.sel[TAB_CONVERT]].kind == NSP_DLC;
        Hint h[] = {{"A", dlc ? "Add / remove DLC" : "Convert"}, {"Y", "Refresh"}, {"+", "Exit"}};
        draw_footer(U.nsp_count ? h : h + 1, U.nsp_count ? 3 : 2, st, sc);
        break;
    }
    case TAB_SAVES: {
        draw_saves();
        Hint h[] = {{"A", "Restore"}, {"X", "Back up now"}, {"Y", "Play user"}, {"+", "Exit"}};
        draw_footer(h, 4, st, sc);
        break;
    }
    case TAB_LOG: {
        draw_log();
        Hint h[] = {{"Y", "Reload"}, {"+", "Exit"}};
        draw_footer(h, 2, SS_ROOT "/log.txt", C_FAINT);
        break;
    }
    }
    draw_dialog();
}

bool ui_input(uint64_t b) {
    if (U.dlg == DLG_MESSAGE) {
        if (b & (BTN_A | BTN_B)) U.dlg = DLG_NONE;
        return true;
    }
    if (U.dlg == DLG_CHOOSE) {
        int dy = (b & BTN_DOWN ? 1 : 0) - (b & BTN_UP ? 1 : 0);
        if (dy && U.plan.count) U.choose_sel = (U.choose_sel + dy + U.plan.count) % U.plan.count;
        if (b & BTN_A) {
            U.dlg = DLG_NONE;
            convert_confirm(U.choose_sel);
        } else if (b & BTN_B) U.dlg = DLG_NONE;
        return true;
    }
    if (U.dlg == DLG_HOSTS) {
        int dy = (b & BTN_DOWN ? 1 : 0) - (b & BTN_UP ? 1 : 0), rows = U.host_row_count;
        if (dy && rows) U.host_sel = (U.host_sel + dy + rows) % rows;
        if (U.host_sel < U.host_top) U.host_top = U.host_sel;
        if (U.host_sel >= U.host_top + HOST_ROWS) U.host_top = U.host_sel - HOST_ROWS + 1;
        if (b & BTN_A) hosts_choose();
        else if (b & BTN_B) U.dlg = DLG_NONE;
        return true;
    }
    if (U.dlg == DLG_USERS) {
        int dy = (b & BTN_DOWN ? 1 : 0) - (b & BTN_UP ? 1 : 0);
        if (dy && U.user_count) U.user_sel = (U.user_sel + dy + U.user_count) % U.user_count;
        if (b & BTN_A) users_choose();
        else if (b & BTN_Y) users_keep_out();
        else if (b & BTN_B) U.dlg = DLG_NONE;
        return true;
    }
    if (U.dlg == DLG_CONFIRM) {
        if (b & BTN_A) run_action();
        else if (b & BTN_B) {
            bool back = U.act == ACT_SET_USER, back_hosts = U.act == ACT_ADD_HOST;   // back to the list
            int sel = U.host_sel, top = U.host_top;
            U.dlg = DLG_NONE;
            U.act = ACT_NONE;
            if (back) open_users_at(U.users[U.user_sel].hex);
            if (back_hosts) { open_hosts(); U.host_sel = sel; U.host_top = top; }
        }
        return true;
    }
    if (U.auto_compat) {
        // Once, at start: the active game gets its look (and its patches) without being asked.
        U.auto_compat = false;
        int a = lib_active(&U.lib);
        if (a >= 0 && lib_compat_wanted(&U.lib, a)) {
            U.act = ACT_COMPAT;
            U.act_arg = a;
            U.compat_auto_run = true;
            run_action();
            return true;
        }
    }
    if (b & BTN_PLUS) return false;
    if (b & (BTN_L | BTN_ZL)) { U.tab = (U.tab + TAB_COUNT - 1) % TAB_COUNT; if (U.tab == TAB_LOG) load_log(); }
    if (b & (BTN_R | BTN_ZR)) { U.tab = (U.tab + 1) % TAB_COUNT; if (U.tab == TAB_LOG) load_log(); }
    int dy = (b & BTN_DOWN ? 1 : 0) - (b & BTN_UP ? 1 : 0);
    if (U.tab == TAB_LOG) {
        U.log_scroll -= dy * 3;
        if (U.log_scroll < 0) U.log_scroll = 0;
        if (b & BTN_Y) load_log();
        return true;
    }
    int count = U.tab == TAB_LIBRARY ? U.lib.count : U.tab == TAB_CONVERT ? U.nsp_count : U.lib.backup_count;
    if (dy && count) U.sel[U.tab] = (U.sel[U.tab] + dy + count) % count;
    if ((b & BTN_Y) && U.tab == TAB_SAVES) {
        reload();
        open_users();
        return true;
    }
    if (b & BTN_Y) reload();
    if (U.tab == TAB_LIBRARY && (b & BTN_A)) library_activate();
    else if (U.tab == TAB_LIBRARY && (b & BTN_X)) library_compat();
    else if (U.tab == TAB_LIBRARY && (b & BTN_MINUS)) open_hosts();
    if (U.tab == TAB_CONVERT && (b & BTN_A) && U.nsp_count) convert_activate();
    if (U.tab == TAB_SAVES) {
        if ((b & BTN_X) && U.lib.play_user_has_save) {
            U.act = ACT_BACKUP;
            run_action();
        } else if ((b & BTN_X) && !U.lib.play_user_set) {
            message(C_WARN, "No play user", "Press Y to choose who plays first.");
        } else if ((b & BTN_X)) {
            message(C_WARN, "Nothing to back up", "%s has no %s save yet.", U.lib.play_user_name, lib_host(&U.lib)->name);
        } else if ((b & BTN_A) && U.lib.backup_count && !U.lib.play_user_set) {
            message(C_WARN, "No play user", "Press Y to choose who plays first.");
        } else if ((b & BTN_A) && U.lib.backup_count) {
            const Backup *bk = &U.lib.backups[U.sel[TAB_SAVES]];
            U.dlg = DLG_CONFIRM;
            U.act = ACT_RESTORE;
            U.act_arg = U.sel[TAB_SAVES];
            U.dlg_color = C_WARN;
            snprintf(U.dlg_title, sizeof(U.dlg_title), "Restore the backup from %s?", bk->when);
            snprintf(U.dlg_body, sizeof(U.dlg_body), "This replaces the current save with the backup (%d file%s). The "
                     "current save is backed up first.\n\nOnly restore a backup of the game that's active now.",
                     bk->st.files, bk->st.files == 1 ? "" : "s");
        }
    }
    return true;
}
