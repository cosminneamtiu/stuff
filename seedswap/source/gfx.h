// SPDX-License-Identifier: GPL-2.0-only
// SeedSwap software renderer: RGBA8888 canvas, AA rounded rectangles,
// pre-rendered font atlases (font_data.c), scaled RGBA images.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "font_data.h"
#include "platform.h"

typedef uint32_t Color;  // memory order R,G,B,A -> 0xAABBGGRR as a little-endian u32
#define RGBA(r, g, b, a) ((Color)(r) | ((Color)(g) << 8) | ((Color)(b) << 16) | ((Color)(a) << 24))
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define HEX(v) RGB(((v) >> 16) & 0xff, ((v) >> 8) & 0xff, (v) & 0xff)

typedef struct { int x, y, w, h; } Rect;
typedef struct { int w, h; uint32_t *px; } Image;  // RGBA, same layout as Color

void gfx_begin(Canvas *c);
void gfx_clip(Rect r);
void gfx_unclip(void);
void gfx_clear_gradient(Color top, Color bottom);
void gfx_fill(Rect r, Color c);
void gfx_round(Rect r, int radius, Color c);
void gfx_round_outline(Rect r, int radius, int thickness, Color c);
void gfx_circle(int cx, int cy, int radius, Color c);
void gfx_image(const Image *img, Rect dst, int radius, uint8_t alpha);

// text: x,y = top-left of the line box; returns the advance in pixels
int gfx_text(int font, int x, int y, Color c, const char *utf8);
int gfx_text_width(int font, const char *utf8);
int gfx_text_ellipsis(int font, int x, int y, int max_w, Color c, const char *utf8);
// word-wrapped paragraph; returns the height used (lines * line height)
int gfx_text_wrap(int font, int x, int y, int max_w, int max_lines, Color c, const char *utf8);
int gfx_line_height(int font);

bool img_decode_jpeg(const void *data, int size, Image *out);  // RGBA
bool img_scale(const Image *src, int w, int h, Image *out);    // area-averaged downscale
void img_free(Image *img);
