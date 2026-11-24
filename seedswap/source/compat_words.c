// SPDX-License-Identifier: GPL-2.0-only
// Nintendo's per-game settings for hosted games (see compat_words.h and pc/fixes/compat_words.py).
#include "compat_words.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#define NAME "ApplicationCompatibilityInfo.json"
#define ANY_VERSION 0xFFFFFFFFull
#define PAGE_LIMIT (0x1000u - 0xD8u)   // the block's words up to +0xd0 stay inside the argument page
#define MAX_DATA (16u << 20)
#define TITLE "Nintendo's Switch 2 settings for this game"

static const char *const FIELD_NAMES[CW_FIELDS] = {"graphics_config", "movie_config", "audio_config", "misc_config"};
static const uint32_t FIELD_OFFSETS[CW_FIELDS] = {0xB8, 0xC0, 0xC8, 0xD0};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// ---- JSON: only what the records need; anything else is skipped (or refused when malformed)

typedef struct {
    const char *p, *end;
    int depth;
} Js;

typedef struct {
    bool ok;        // a number (JSON integer, or a string in the form the PC accepts)
    bool negative;
    bool overflow;  // more than 64 bits
    uint64_t value; // the value mod 2^64 (two's complement when negative)
} Num;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool in_set(char c, const char *set) { return c && strchr(set, c); }

static void js_ws(Js *j) {
    while (j->p < j->end && is_space(*j->p)) j->p++;
}

static bool js_lit(Js *j, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n)) return false;
    j->p += n;
    return true;
}

// A string; its bytes go to out when given (escapes decoded; \u below 0x80 as that character, else '?').
static bool js_string(Js *j, char *out, size_t n, size_t *len) {
    size_t k = 0;
    if (j->p >= j->end || *j->p != '"') return false;
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if ((unsigned char)c < 0x20) return false;
        if (c == '\\') {
            if (j->p >= j->end) return false;
            char e = *j->p++;
            switch (e) {
            case '"': case '\\': case '/': c = e; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                if (j->end - j->p < 4) return false;
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = j->p[i];
                    if (!in_set(h, "0123456789abcdefABCDEF")) return false;
                    code = code * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                }
                j->p += 4;
                c = code && code < 0x80 ? (char)code : '?';
                break;
            }
            default: return false;
            }
        }
        if (out && k + 1 < n) out[k] = c;
        k++;
    }
    if (j->p >= j->end) return false;
    j->p++;
    if (out && n) out[k < n ? k : n - 1] = 0;
    if (len) *len = k;
    return true;
}

// Digits with wraparound (the value mod 2^64), as Python's `value & 0xffffffffffffffff` sees it.
static bool digits(const char *s, size_t n, int base, Num *out) {
    if (!n) return false;
    uint64_t v = 0;
    bool over = false;
    for (size_t i = 0; i < n; i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        uint64_t next = v * (uint64_t)base + (uint64_t)d;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) over = true;
        v = next;
    }
    out->value = v;
    out->overflow = over;
    return true;
}

// The forms pc/fixes/compat_words.py NUMBER_TEXT accepts: [+-] then 0x<hex>, 0, or [1-9][0-9]*
static Num number_text(const char *s, size_t n, bool json_integer) {
    Num r = {0};
    while (!json_integer && n && is_space(s[0])) s++, n--;
    while (!json_integer && n && is_space(s[n - 1])) n--;
    bool neg = false;
    if (n && (s[0] == '-' || (!json_integer && s[0] == '+'))) {
        neg = s[0] == '-';
        s++, n--;
    }
    bool ok;
    if (!json_integer && n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ok = digits(s + 2, n - 2, 16, &r);
    else if (n == 1 && s[0] == '0') ok = digits(s, n, 10, &r);
    else ok = n && s[0] != '0' && digits(s, n, 10, &r);
    if (!ok) return (Num){0};
    r.ok = true;
    r.negative = neg && r.value != 0;
    if (neg) r.value = (uint64_t)0 - r.value;
    return r;
}

static bool js_skip(Js *j);

// A value; a number (or a string holding one) is returned in *num, anything else is skipped.
static bool js_value(Js *j, Num *num) {
    *num = (Num){0};
    js_ws(j);
    if (j->p >= j->end) return false;
    char c = *j->p;
    if (c == '"') {
        char text[80];
        size_t len = 0;
        if (!js_string(j, text, sizeof(text), &len)) return false;
        if (len < sizeof(text)) *num = number_text(text, len, false);   // longer: not a number anyone writes
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = j->p;
        while (j->p < j->end && in_set(*j->p, "+-0123456789.eE")) j->p++;
        size_t n = (size_t)(j->p - start);
        bool integer = true;
        for (size_t i = 0; i < n; i++)
            if (in_set(start[i], ".eE+") || (i && start[i] == '-')) integer = false;
        if (integer) *num = number_text(start, n, true);   // JSON integers only; 1.0 and 1e3 are not numbers
        return n > 0;
    }
    return js_skip(j);
}

static bool js_skip(Js *j) {
    js_ws(j);
    if (j->p >= j->end || ++j->depth > 64) return false;
    bool ok = false;
    char c = *j->p;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        js_ws(j);
        if (j->p < j->end && *j->p == close) {
            j->p++;
            j->depth--;
            return true;
        }
        for (;;) {
            Num unused;
            if (c == '{') {
                js_ws(j);
                if (!js_string(j, NULL, 0, NULL)) break;
                js_ws(j);
                if (j->p >= j->end || *j->p++ != ':') break;
            }
            if (!js_value(j, &unused)) break;
            js_ws(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->end && *j->p == close) {
                j->p++;
                ok = true;
            }
            break;
        }
    } else if (c == '"') ok = js_string(j, NULL, 0, NULL);
    else if (js_lit(j, "true") || js_lit(j, "false") || js_lit(j, "null")) ok = true;
    else {
        Num unused;
        ok = js_value(j, &unused);
    }
    j->depth--;
    return ok;
}

// Walks an object: for each key, fn is called with the parser at the value, and must consume it.
typedef bool (*KeyFn)(Js *j, const char *key, void *user);

static bool js_object(Js *j, KeyFn fn, void *user) {
    js_ws(j);
    if (j->p >= j->end || *j->p != '{') return false;
    j->p++;
    js_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        return true;
    }
    for (;;) {
        char key[64];
        js_ws(j);
        if (!js_string(j, key, sizeof(key), NULL)) return false;
        js_ws(j);
        if (j->p >= j->end || *j->p++ != ':') return false;
        if (!fn(j, key, user)) return false;
        js_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            return true;
        }
        return false;
    }
}

static bool layer_key(Js *j, const char *key, void *user) {
    CwRecord *r = user;
    Num n;
    if (!js_value(j, &n)) return false;
    for (int i = 0; i < CW_FIELDS; i++)
        if (!strcmp(key, FIELD_NAMES[i])) r->words[i] = n.ok ? n.value : 0;
    return true;
}

static bool record_key(Js *j, const char *key, void *user) {
    CwRecord *r = user;
    if (!strcmp(key, "application_layer_info")) {
        memset(r->words, 0, sizeof(r->words));
        js_ws(j);
        if (j->p < j->end && *j->p == '{') return js_object(j, layer_key, r);
        return js_skip(j);   // not an object: no words
    }
    Num n;
    if (!js_value(j, &n)) return false;
    if (!strcmp(key, "application_id")) {
        r->id_ok = n.ok && !n.negative && !n.overflow;
        r->id = n.value;
    } else if (!strcmp(key, "target_max_version")) {
        // not a number: applies to every version (as when it is missing)
        r->limit = !n.ok ? ANY_VERSION : n.overflow && !n.negative ? UINT64_MAX : n.value;
        r->limit_ok = !n.ok || !n.negative;
    }
    return true;
}

typedef struct {
    CwInfo *info;
    int capacity;
    bool seen;
} Doc;

static bool records_array(Js *j, Doc *d) {
    js_ws(j);
    if (j->p >= j->end || *j->p != '[') return false;
    d->info->count = 0;   // a later "records" replaces an earlier one, as in Python's json
    j->p++;
    js_ws(j);
    if (j->p < j->end && *j->p == ']') {
        j->p++;
        return true;
    }
    for (;;) {
        js_ws(j);
        if (j->p < j->end && *j->p == '{') {
            if (d->info->count == d->capacity) {
                int cap = d->capacity ? d->capacity * 2 : 1024;
                CwRecord *more = realloc(d->info->records, sizeof(CwRecord) * (size_t)cap);
                if (!more) return false;
                d->info->records = more;
                d->capacity = cap;
            }
            CwRecord *r = &d->info->records[d->info->count];
            memset(r, 0, sizeof(*r));
            r->limit = ANY_VERSION;
            r->limit_ok = true;
            if (!js_object(j, record_key, r)) return false;
            d->info->count++;
        } else if (!js_skip(j)) return false;   // not an object: not a record
        js_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == ']') {
            j->p++;
            return true;
        }
        return false;
    }
}

static bool top_key(Js *j, const char *key, void *user) {
    Doc *d = user;
    if (!strcmp(key, "records")) {
        js_ws(j);
        if (j->p < j->end && *j->p == '[') {
            d->seen = true;
            return records_array(j, d);
        }
        d->seen = false;
    }
    return js_skip(j);
}

// ---- RomFS: the file table only (the JSON lies in the root of system data 0400000000000848)

static bool romfs_find(const uint8_t *data, size_t size, const char *name, const uint8_t **out, size_t *out_n) {
    if (size < 0x50 || rd64(data) != 0x50) return false;
    uint64_t meta = rd64(data + 0x38), meta_size = rd64(data + 0x40), fdata = rd64(data + 0x48);
    if (meta > size || meta_size > size - meta || fdata > size) return false;
    bool found = false;
    size_t nl = strlen(name);
    for (uint64_t at = meta; at + 0x20 <= meta + meta_size;) {
        uint64_t off = rd64(data + at + 8), len = rd64(data + at + 16);
        uint32_t name_len = rd32(data + at + 28);
        if (at + 0x20 + name_len > meta + meta_size || off > size - fdata || len > size - fdata - off) return false;
        if (name_len == nl && !memcmp(data + at + 0x20, name, nl)) {   // the last one, as the PC's dict keeps it
            *out = data + fdata + off;
            *out_n = (size_t)len;
            found = true;
        }
        at += 0x20 + ((name_len + 3u) & ~3u);
    }
    return found;
}

bool cw_parse(const uint8_t *data, size_t size, CwInfo *out, char *err, size_t errn) {
    memset(out, 0, sizeof(*out));
    if (size >= 8 && rd64(data) == 0x50) {
        const uint8_t *json;
        size_t n;
        if (!romfs_find(data, size, NAME, &json, &n)) {
            snprintf(err, errn, "the system data holds no " NAME);
            return false;
        }
        data = json;
        size = n;
    }
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) data += 3, size -= 3;
    Js j = {(const char *)data, (const char *)data + size, 0};
    Doc d = {out, 0, false};
    bool ok = js_object(&j, top_key, &d);
    js_ws(&j);
    if (!ok || j.p != j.end) {
        cw_free(out);
        snprintf(err, errn, NAME " is not valid JSON");
        return false;
    }
    if (!d.seen) {
        cw_free(out);
        snprintf(err, errn, NAME " has no list of records");
        return false;
    }
    return true;
}

void cw_free(CwInfo *info) {
    free(info->records);
    info->records = NULL;
    info->count = 0;
}

bool cw_lookup(const CwInfo *info, uint64_t program_id, uint64_t app_id, uint64_t version, CompatWords *out) {
    memset(out, 0, sizeof(*out));
    uint64_t wanted[2] = {program_id, app_id};
    for (int w = 0; w < 2; w++) {
        if (w == 1 && (!app_id || app_id == program_id)) break;
        const CwRecord *best = NULL;
        for (int i = 0; i < info->count; i++) {
            const CwRecord *r = &info->records[i];
            if (!r->id_ok || r->id != wanted[w] || !r->limit_ok || version > r->limit) continue;
            if (!best || r->limit < best->limit) best = r;   // the narrowest; the first of equals
        }
        if (!best) continue;
        out->record_id = wanted[w];
        bool any = false;
        for (int k = 0; k < CW_FIELDS; k++) {
            out->words[k] = best->words[k];
            any |= best->words[k] != 0;
        }
        return any;
    }
    return false;
}

// ---- where the data comes from

static uint8_t *read_source(size_t *n, int *state, char *why, size_t whyn) {
    uint8_t *buf = NULL;
#if defined(__SWITCH__)
    FsStorage st;
    Result rc = fsOpenDataStorageByDataId(&st, CW_DATA_ID, NcmStorageId_BuiltInSystem);
    if (R_FAILED(rc)) {
        snprintf(why, whyn, "system data 0400000000000848 could not be opened (%08x)", (unsigned)rc);
        *state = CW_UNREADABLE;
        return NULL;
    }
    s64 size = 0;
    rc = fsStorageGetSize(&st, &size);
    if (R_SUCCEEDED(rc) && (size <= 0 || (uint64_t)size > MAX_DATA)) rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(rc) && !(buf = malloc((size_t)size))) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    if (R_SUCCEEDED(rc)) rc = fsStorageRead(&st, 0, buf, (u64)size);
    fsStorageClose(&st);
    if (R_FAILED(rc)) {
        free(buf);
        snprintf(why, whyn, "system data 0400000000000848 could not be read (%08x)", (unsigned)rc);
        *state = CW_UNREADABLE;
        return NULL;
    }
    *n = (size_t)size;
#else
    const char *path = getenv("SEEDSWAP_COMPAT_INFO");   // the PC preview: a dump of the system data, or the JSON
    if (!path || !path[0]) {
        *state = CW_UNAVAILABLE;
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (f && size > 0 && (unsigned long)size <= MAX_DATA && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)size)) &&
        fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    if (f) fclose(f);
    if (!buf) {
        snprintf(why, whyn, "%s could not be read", path);
        *state = CW_UNREADABLE;
        return NULL;
    }
    *n = (size_t)size;
#endif
    return buf;
}

int cw_for_game(uint64_t program_id, uint64_t app_id, uint64_t version, CompatWords *out, char *why, size_t whyn) {
    static CwInfo info;
    static int state = -2;   // not read yet
    static char reason[160];
    memset(out, 0, sizeof(*out));
    if (state == -2) {
        size_t n = 0;
        uint8_t *data = read_source(&n, &state, reason, sizeof(reason));
        if (data) {
            state = cw_parse(data, n, &info, reason, sizeof(reason)) ? CW_FOUND : CW_UNREADABLE;
            free(data);
        }
    }
    if (state != CW_FOUND) {
        snprintf(why, whyn, "%s", reason);
        return state;
    }
    if (cw_lookup(&info, program_id, app_id, version, out)) return CW_FOUND;
    snprintf(why, whyn, out->record_id ? "the record has no application-layer settings" : "no record for this game");
    return CW_NONE;
}

// ---- the code (pc/fixes/compat_words.py build_cave)

enum { X1 = 1, X9 = 9, X10 = 10, X11 = 11, HEAD_WORDS = 10, MAX_WORDS = HEAD_WORDS + 4 * 7 + 2 };

static uint32_t ldr64(int rt, int rn, uint32_t off) { return 0xF9400000u | (off / 8) << 10 | (uint32_t)rn << 5 | (uint32_t)rt; }
static uint32_t str64(int rt, int rn, uint32_t off) { return 0xF9000000u | (off / 8) << 10 | (uint32_t)rn << 5 | (uint32_t)rt; }

static int mov_wide(int rd, uint64_t value, uint32_t *out) {
    int n = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t half = (uint32_t)(value >> (16 * k)) & 0xFFFF;
        if (!half) continue;
        out[n] = (n ? 0xF2800000u : 0xD2800000u) | (uint32_t)k << 21 | half << 5 | (uint32_t)rd;
        n++;
    }
    return n;
}

static uint32_t cond_branch(uint32_t at, uint32_t target, uint32_t cond) {
    return 0x54000000u | (((target - at) >> 2) & 0x7FFFF) << 5 | cond;
}

static bool branch_ok(int64_t from, int64_t to) {
    int64_t d = to - from;
    return !(d & 3) && d >= -(1ll << 27) && d < (1ll << 27);
}

static uint32_t branch(uint32_t from, uint32_t to, bool link) {
    return (link ? 0x94000000u : 0x14000000u) | (((to - from) >> 2) & 0x3FFFFFF);
}

// The first instruction of nnosInitialize, re-encoded to run at `to`; false when it reads the PC in a
// way that is not re-encoded (as compat_words.py relocate()).
static bool relocate(uint32_t w, uint32_t from, uint32_t to, uint32_t *out) {
    if ((w & 0x7C000000u) == 0x14000000u) {   // b, bl
        int64_t off = w & 0x3FFFFFF;
        if (off & 0x2000000) off -= 1 << 26;
        int64_t target = (int64_t)from + off * 4;
        if (!branch_ok(to, target)) return false;
        *out = branch(to, (uint32_t)target, w & 0x80000000u);
        return true;
    }
    if ((w & 0x9F000000u) == 0x90000000u) {   // adrp
        int64_t pages = (int64_t)(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3));
        if (pages & (1 << 20)) pages -= 1 << 21;
        int64_t target = (int64_t)(from & ~0xFFFu) + pages * 4096;
        int64_t delta = (target >> 12) - (int64_t)(to >> 12);
        if (delta < -(1 << 20) || delta >= (1 << 20)) return false;
        uint32_t p = (uint32_t)delta & 0x1FFFFF;
        *out = (w & 0x9F00001Fu) | (p & 3) << 29 | (p >> 2) << 5;
        return true;
    }
    if ((w & 0x9F000000u) == 0x10000000u || (w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u ||
        (w & 0x7E000000u) == 0x36000000u || (w & 0x3B000000u) == 0x18000000u)
        return false;
    *out = w;
    return true;
}

static int build_cave(uint32_t cave, uint32_t hook, uint32_t displaced, const uint64_t words[CW_FIELDS], uint32_t *w,
                      bool *ok) {
    uint32_t body[4 * 7];
    int nb = 0;
    for (int i = 0; i < CW_FIELDS; i++) {
        if (!words[i]) continue;
        body[nb++] = ldr64(X10, X9, FIELD_OFFSETS[i]);
        nb += mov_wide(X11, words[i], body + nb);
        body[nb++] = 0xAA000000u | X11 << 16 | X10 << 5 | X10;   // orr x10, x10, x11
        body[nb++] = str64(X10, X9, FIELD_OFFSETS[i]);
    }
    uint32_t out_at = cave + 4u * (uint32_t)(HEAD_WORDS + nb);
    int n = 0;
    w[n++] = 0xB4000000u | (((out_at - cave) >> 2) & 0x7FFFF) << 5 | X1;   // cbz  x1, out
    w[n++] = ldr64(X9, X1, 0);                                             // ldr  x9, [x1]
    n += mov_wide(X10, 0x1000ull | 1ull << 32, w + n);                      // mov  x10, #0x100001000
    w[n++] = 0xEB000000u | X10 << 16 | X9 << 5 | 31;                        // cmp  x9, x10
    w[n++] = cond_branch(cave + 20, out_at, 0x1);                          // b.ne out
    w[n++] = ldr64(X9, X1, 8);                                             // ldr  x9, [x1, #8]
    w[n++] = 0xCB000000u | X1 << 16 | X9 << 5 | X10;                        // sub  x10, x9, x1
    w[n++] = 0xF100001Fu | PAGE_LIMIT << 10 | X10 << 5;                     // cmp  x10, #0xf28
    w[n++] = cond_branch(cave + 36, out_at, 0x8);                          // b.hi out
    memcpy(w + n, body, sizeof(uint32_t) * (size_t)nb);
    n += nb;
    *ok = relocate(displaced, hook, out_at, &w[n]);
    n++;
    w[n++] = branch(out_at + 4, hook + 4, false);
    return n;
}

static bool not_placed(FixRecord *r, const char *why) {
    r->status = FIX_NOT_PLACED;
    r->count = 0;
    snprintf(r->reason, sizeof(r->reason), "%s", why);
    return false;
}

void cw_describe(const uint64_t words[CW_FIELDS], char *out, size_t n) {
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < CW_FIELDS && k < n; i++) {
        if (!words[i]) continue;
        int len = strchr(FIELD_NAMES[i], '_') - FIELD_NAMES[i];
        int m = snprintf(out + k, n - k, "%s%.*s 0x%llx", k ? ", " : "", len, FIELD_NAMES[i], (unsigned long long)words[i]);
        if (m < 0) break;
        k += (size_t)m;
    }
}

void cw_ini_value(const uint64_t words[CW_FIELDS], char *out, size_t n) {
    snprintf(out, n, "0x%llx,0x%llx,0x%llx,0x%llx", (unsigned long long)words[0], (unsigned long long)words[1],
             (unsigned long long)words[2], (unsigned long long)words[3]);
}

bool cw_ini_parse(const char *text, uint64_t words[CW_FIELDS]) {
    memset(words, 0, sizeof(uint64_t) * CW_FIELDS);
    if (!text || !text[0]) return false;
    bool any = false;
    for (int i = 0; i < CW_FIELDS; i++) {
        char *end;
        words[i] = strtoull(text, &end, 16);
        if (end == text) return false;
        any |= words[i] != 0;
        text = end;
        if (i < CW_FIELDS - 1) {
            if (*text != ',') return false;
            text++;
        }
    }
    return any;
}

bool cw_apply(FixModule *mods, int count, const CompatWords *cw, FixRecord *r, unsigned *changed) {
    memset(r, 0, sizeof(*r));
    snprintf(r->id, sizeof(r->id), CW_ID);
    snprintf(r->title, sizeof(r->title), TITLE);
    snprintf(r->build_note, sizeof(r->build_note), "record %016llx", (unsigned long long)cw->record_id);
    NsoImage *main = NULL, *sdk = NULL;
    int sdk_i = -1;
    bool elsewhere = false;
    for (int i = 0; i < count; i++) {
        bool defined = fixes_defined(mods[i].nso, "nnosInitialize") > 0;
        if (!strcmp(mods[i].name, "main")) main = mods[i].nso;
        if (!strcmp(mods[i].name, "sdk")) {
            sdk = mods[i].nso;
            sdk_i = i;
        } else if (defined) elsewhere = true;
    }
    if (main) {
        for (int i = 0; i < 16; i++) snprintf(r->build + 2 * i, 3, "%02x", main->build_id[i]);
    }
    if (!main) return not_placed(r, "the game has no main module");
    if (nso_is32(main)) return not_placed(r, "not done for 32-bit games yet");
    bool any = false;
    for (int i = 0; i < CW_FIELDS; i++) any |= cw->words[i] != 0;
    if (!any) return not_placed(r, "the record has no application-layer settings");
    if (!sdk) return not_placed(r, "the game has no sdk module");
    long long hook = fixes_defined(sdk, "nnosInitialize");
    if (hook <= 0 || elsewhere) return not_placed(r, "nnosInitialize is not defined only in the sdk");
    if ((hook & 3) || (uint64_t)hook + 8 > sdk->size[0]) return not_placed(r, "the sdk's nnosInitialize is not in its code");
    uint32_t at = (uint32_t)hook, displaced = rd32(sdk->image + at);
    uint32_t cave = (sdk->size[0] + 15) & ~15u, words[MAX_WORDS];
    bool moved;
    int n = build_cave(cave, at, displaced, cw->words, words, &moved);
    if (!moved) return not_placed(r, "nnosInitialize starts with a PC-relative instruction that the fix does not move");
    if ((uint64_t)cave + 4u * (uint64_t)n > sdk->mem[1] || !branch_ok(at, cave))
        return not_placed(r, "the sdk has too few free bytes after its code for the fix");
    uint32_t jump = branch(at, cave, false);
    char what[120], list[100];
    cw_describe(cw->words, list, sizeof(list));
    snprintf(what, sizeof(what), "nnosInitialize: first instruction -> b compat_words (%s)", list);
    r->changes[0] = (FixChange){"sdk", at, displaced, jump, ""};
    snprintf(r->changes[0].what, sizeof(r->changes[0].what), "%s", what);
    r->count = 1;
    memset(sdk->image + sdk->size[0], 0, cave - sdk->size[0]);
    for (int i = 0; i < n; i++) wr32(sdk->image + cave + 4 * (uint32_t)i, words[i]);
    sdk->size[0] = cave + 4u * (uint32_t)n;
    wr32(sdk->image + at, jump);
    r->cave_start = cave;
    r->cave_size = 4u * (uint32_t)n;
    r->status = FIX_APPLIED;
    snprintf(r->note, sizeof(r->note), "%s (record %016llx), written at the start of the sdk's nnosInitialize", list,
             (unsigned long long)cw->record_id);
    *changed |= 1u << sdk_i;
    return true;
}
