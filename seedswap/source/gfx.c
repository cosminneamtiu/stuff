// SPDX-License-Identifier: GPL-2.0-only
#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_cfg.h"

static Canvas *C;
static Rect g_clip;

void gfx_begin(Canvas *c) {
    C = c;
    g_clip = (Rect){0, 0, c->w, c->h};
}

void gfx_clip(Rect r) {
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > C->w ? C->w : r.x + r.w, y1 = r.y + r.h > C->h ? C->h : r.y + r.h;
    g_clip = (Rect){x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}
void gfx_unclip(void) { g_clip = (Rect){0, 0, C->w, C->h}; }

static inline void blend(uint32_t *d, Color c, unsigned a) {
    if (a >= 255) { *d = c | 0xff000000u; return; }
    if (!a) return;
    uint32_t s = *d;
    unsigned ia = 255 - a;
    uint32_t rb = (((c & 0x00ff00ffu) * a + (s & 0x00ff00ffu) * ia) >> 8) & 0x00ff00ffu;
    uint32_t g = (((c & 0x0000ff00u) * a + (s & 0x0000ff00u) * ia) >> 8) & 0x0000ff00u;
    *d = rb | g | 0xff000000u;
}

static inline bool in_clip_y(int y) { return y >= g_clip.y && y < g_clip.y + g_clip.h; }

// coverage-scaled pixel
static inline void plot(int x, int y, Color c, float cov) {
    if (cov <= 0.f || x < g_clip.x || x >= g_clip.x + g_clip.w || !in_clip_y(y)) return;
    if (cov > 1.f) cov = 1.f;
    blend(&C->px[y * C->stride + x], c, (unsigned)((float)(c >> 24) * cov + 0.5f));
}

static void hspan(int x0, int x1, int y, Color c) {
    if (!in_clip_y(y)) return;
    if (x0 < g_clip.x) x0 = g_clip.x;
    if (x1 > g_clip.x + g_clip.w) x1 = g_clip.x + g_clip.w;
    if (x1 <= x0) return;
    uint32_t *row = &C->px[y * C->stride];
    unsigned a = c >> 24;
    if (a == 255) {
        for (int x = x0; x < x1; x++) row[x] = c;
    } else {
        for (int x = x0; x < x1; x++) blend(&row[x], c, a);
    }
}

void gfx_clear_gradient(Color top, Color bottom) {
    for (int y = 0; y < C->h; y++) {
        unsigned t = (unsigned)(y * 256 / C->h), it = 256 - t;
        Color c = RGB(((top & 0xff) * it + (bottom & 0xff) * t) >> 8, (((top >> 8) & 0xff) * it + ((bottom >> 8) & 0xff) * t) >> 8,
                      (((top >> 16) & 0xff) * it + ((bottom >> 16) & 0xff) * t) >> 8);
        uint32_t *row = &C->px[y * C->stride];
        for (int x = 0; x < C->w; x++) row[x] = c;
    }
}

void gfx_fill(Rect r, Color c) {
    for (int y = r.y; y < r.y + r.h; y++) hspan(r.x, r.x + r.w, y, c);
}

static float corner_cov(float fx, float fy, float rad) {
    float d = sqrtf(fx * fx + fy * fy);
    float cov = rad - d + 0.5f;
    return cov < 0.f ? 0.f : cov > 1.f ? 1.f : cov;
}

void gfx_round(Rect r, int rad, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.w) rad = r.w / 2;
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad <= 0) { gfx_fill(r, c); return; }
    for (int y = r.y; y < r.y + r.h; y++) {
        if (!in_clip_y(y)) continue;
        float fy;
        if (y < r.y + rad) fy = (float)(r.y + rad) - ((float)y + 0.5f);
        else if (y >= r.y + r.h - rad) fy = ((float)y + 0.5f) - (float)(r.y + r.h - rad);
        else { hspan(r.x, r.x + r.w, y, c); continue; }
        for (int i = 0; i < rad; i++) {
            float fx = (float)rad - ((float)i + 0.5f);
            float cov = corner_cov(fx, fy, (float)rad);
            plot(r.x + i, y, c, cov);
            plot(r.x + r.w - 1 - i, y, c, cov);
        }
        hspan(r.x + rad, r.x + r.w - rad, y, c);
    }
}

void gfx_round_outline(Rect r, int rad, int t, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.w) rad = r.w / 2;
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad < t) rad = t;
    gfx_fill((Rect){r.x + rad, r.y, r.w - 2 * rad, t}, c);
    gfx_fill((Rect){r.x + rad, r.y + r.h - t, r.w - 2 * rad, t}, c);
    gfx_fill((Rect){r.x, r.y + rad, t, r.h - 2 * rad}, c);
    gfx_fill((Rect){r.x + r.w - t, r.y + rad, t, r.h - 2 * rad}, c);
    for (int j = 0; j < rad; j++) {
        float fy = (float)rad - ((float)j + 0.5f);
        for (int i = 0; i < rad; i++) {
            float fx = (float)rad - ((float)i + 0.5f);
            float cov = corner_cov(fx, fy, (float)rad) - corner_cov(fx, fy, (float)(rad - t));
            if (cov <= 0.f) continue;
            plot(r.x + i, r.y + j, c, cov);
            plot(r.x + r.w - 1 - i, r.y + j, c, cov);
            plot(r.x + i, r.y + r.h - 1 - j, c, cov);
            plot(r.x + r.w - 1 - i, r.y + r.h - 1 - j, c, cov);
        }
    }
}

void gfx_circle(int cx, int cy, int radius, Color c) { gfx_round((Rect){cx - radius, cy - radius, radius * 2, radius * 2}, radius, c); }

void gfx_image(const Image *img, Rect dst, int rad, uint8_t alpha) {
    if (!img || !img->px || dst.w <= 0 || dst.h <= 0) return;
    if (rad * 2 > dst.w) rad = dst.w / 2;
    for (int y = 0; y < dst.h; y++) {
        int py = dst.y + y;
        if (!in_clip_y(py)) continue;
        int sy = y * img->h / dst.h;
        float fy = -1.f;
        if (y < rad) fy = (float)rad - ((float)y + 0.5f);
        else if (y >= dst.h - rad) fy = ((float)y + 0.5f) - (float)(dst.h - rad);
        for (int x = 0; x < dst.w; x++) {
            int px = dst.x + x;
            if (px < g_clip.x || px >= g_clip.x + g_clip.w) continue;
            uint32_t s = img->px[sy * img->w + x * img->w / dst.w];
            float cov = 1.f;
            if (fy >= 0.f) {
                if (x < rad) cov = corner_cov((float)rad - ((float)x + 0.5f), fy, (float)rad);
                else if (x >= dst.w - rad) cov = corner_cov(((float)x + 0.5f) - (float)(dst.w - rad), fy, (float)rad);
            }
            unsigned a = (unsigned)((float)((s >> 24) * alpha / 255) * cov + 0.5f);
            blend(&C->px[py * C->stride + px], s, a);
        }
    }
}

// ---- text
static const char *utf8_next(const char *s, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return s + 1; }
    if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) { *cp = ((u[0] & 0x1fu) << 6) | (u[1] & 0x3fu); return s + 2; }
    if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
        *cp = ((u[0] & 0x0fu) << 12) | ((u[1] & 0x3fu) << 6) | (u[2] & 0x3fu);
        return s + 3;
    }
    if ((u[0] & 0xf8) == 0xf0 && u[1] && u[2] && u[3]) {
        *cp = ((u[0] & 0x07u) << 18) | ((u[1] & 0x3fu) << 12) | ((u[2] & 0x3fu) << 6) | (u[3] & 0x3fu);
        return s + 4;
    }
    *cp = '?';
    return s + 1;
}

static const FontGlyph *glyph(const FontFace *f, uint32_t cp) {
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return cp == '?' ? NULL : glyph(f, '?');
}

int gfx_line_height(int font) { return font_faces[font].line; }

int gfx_text_width(int font, const char *s) {
    const FontFace *f = &font_faces[font];
    int w = 0;
    while (s && *s) {
        uint32_t cp;
        s = utf8_next(s, &cp);
        const FontGlyph *g = glyph(f, cp);
        if (g) w += g->adv;
    }
    return w;
}

int gfx_text(int font, int x, int y, Color c, const char *s) {
    const FontFace *f = &font_faces[font];
    int x0 = x;
    unsigned ca = c >> 24;
    while (s && *s) {
        uint32_t cp;
        s = utf8_next(s, &cp);
        const FontGlyph *g = glyph(f, cp);
        if (!g) continue;
        for (int j = 0; j < g->h; j++) {
            int py = y + g->yoff + j;
            if (!in_clip_y(py)) continue;
            const uint8_t *src = &f->atlas[(g->y + j) * f->atlas_w + g->x];
            uint32_t *row = &C->px[py * C->stride];
            for (int i = 0; i < g->w; i++) {
                int px = x + g->xoff + i;
                if (!src[i] || px < g_clip.x || px >= g_clip.x + g_clip.w) continue;
                blend(&row[px], c, src[i] * ca / 255);
            }
        }
        x += g->adv;
    }
    return x - x0;
}

int gfx_text_ellipsis(int font, int x, int y, int max_w, Color c, const char *s) {
    if (gfx_text_width(font, s) <= max_w) return gfx_text(font, x, y, c, s);
    const char *dots = "\xe2\x80\xa6";
    int budget = max_w - gfx_text_width(font, dots);
    char buf[512];
    size_t n = 0;
    const char *p = s;
    while (*p && n + 4 < sizeof(buf)) {
        uint32_t cp;
        const char *q = utf8_next(p, &cp);
        memcpy(buf + n, p, (size_t)(q - p));
        buf[n + (size_t)(q - p)] = 0;
        if (gfx_text_width(font, buf) > budget) break;
        n += (size_t)(q - p);
        p = q;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    memcpy(buf + n, dots, 4);
    return gfx_text(font, x, y, c, buf);
}

int gfx_text_wrap(int font, int x, int y, int max_w, int max_lines, Color c, const char *s) {
    char line[512];
    int lines = 0, lh = gfx_line_height(font);
    const char *p = s;
    while (p && *p && (max_lines <= 0 || lines < max_lines)) {
        size_t n = 0, last_space = 0;
        const char *q = p, *cut = NULL;
        while (*q && *q != '\n' && n + 4 < sizeof(line)) {
            uint32_t cp;
            const char *nq = utf8_next(q, &cp);
            memcpy(line + n, q, (size_t)(nq - q));
            line[n + (size_t)(nq - q)] = 0;
            if (gfx_text_width(font, line) > max_w && n > 0) break;
            n += (size_t)(nq - q);
            if (cp == ' ') { last_space = n; cut = nq; }
            q = nq;
        }
        if (*q && *q != '\n' && cut) { n = last_space; q = cut; }
        line[n] = 0;
        bool last = max_lines > 0 && lines == max_lines - 1 && *q && *q != '\n';
        if (last) gfx_text_ellipsis(font, x, y + lines * lh, max_w, c, p);
        else gfx_text(font, x, y + lines * lh, c, line);
        lines++;
        p = *q == '\n' ? q + 1 : q;
        while (*p == ' ') p++;
    }
    return lines * lh;
}

// ---- images
bool img_decode_jpeg(const void *data, int size, Image *out) {
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load_from_memory(data, size, &w, &h, &n, 4);
    if (!px) return false;
    out->w = w;
    out->h = h;
    out->px = (uint32_t *)px;
    return true;
}

bool img_scale(const Image *src, int w, int h, Image *out) {
    out->px = malloc((size_t)w * (size_t)h * 4);
    if (!out->px) return false;
    out->w = w;
    out->h = h;
    for (int y = 0; y < h; y++) {
        int sy0 = y * src->h / h, sy1 = (y + 1) * src->h / h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < w; x++) {
            int sx0 = x * src->w / w, sx1 = (x + 1) * src->w / w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    uint32_t p = src->px[yy * src->w + xx];
                    r += p & 0xff; g += (p >> 8) & 0xff; b += (p >> 16) & 0xff; a += p >> 24; n++;
                }
            out->px[y * w + x] = RGBA(r / n, g / n, b / n, a / n);
        }
    }
    return true;
}

void img_free(Image *img) {
    free(img->px);
    img->px = NULL;
    img->w = img->h = 0;
}
