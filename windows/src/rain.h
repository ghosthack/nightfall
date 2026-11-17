/*
 * Digital rain model shared by the Windows and Linux builds (identical copies in
 * windows/src and linux/src; macos/src/main.swift ports the same logic).
 *
 * The screen is a grid of cells. Each column has a falling head with a fading trail.
 * Every frame rain_update() reports only the cells whose look changed, so the
 * renderer redraws a few hundred small glyphs per frame instead of the whole screen.
 */
#ifndef NIGHTFALL_RAIN_H
#define NIGHTFALL_RAIN_H

#include <stdlib.h>
#include <string.h>

#define RAIN_LEVELS  12     /* 0 = empty, 1 = head, 2..11 = trail from bright to dim */
#define RAIN_GLYPHS  66     /* 56 half-width katakana (U+FF66..U+FF9D) + 10 digits */
#define RAIN_FPS     15

/* codepoint of glyph i */
static inline unsigned rain_glyph_cp(int i) { return i < 56 ? 0xFF66u + (unsigned)i : '0' + (unsigned)(i - 56); }

/* RGB of brightness level (1..RAIN_LEVELS-1); level 0 is black */
static inline void rain_color(int level, int *r, int *g, int *b)
{
    if (level <= 0) { *r = *g = *b = 0; return; }
    if (level == 1) { *r = 170; *g = 255; *b = 180; return; }
    double t = 1.0 - 0.88 * (level - 2) / (double)(RAIN_LEVELS - 3);   /* 1.0 .. 0.12 */
    *r = (int)(10 * t); *g = (int)(220 * t); *b = (int)(70 * t);
}

typedef struct { int head, len, speed; } RainCol;

typedef struct {
    int cols, rows;
    RainCol *col;
    unsigned char *glyph;          /* glyph of each cell, cols*rows */
    unsigned char *shown_level;    /* what is on screen now */
    unsigned char *shown_glyph;
    unsigned frame, seed;
} Rain;

static inline unsigned rain_rnd(Rain *r)
{
    r->seed ^= r->seed << 13; r->seed ^= r->seed >> 17; r->seed ^= r->seed << 5;
    return r->seed;
}

static inline void rain_spawn(Rain *r, RainCol *c, int first)
{
    int maxlen = r->rows * 2 / 3;
    if (maxlen < 6) maxlen = 6;
    c->len = 6 + (int)(rain_rnd(r) % (unsigned)(maxlen - 5));
    c->speed = 1 + (int)(rain_rnd(r) % 3);               /* advance every 1..3 frames */
    /* start above the screen; the gap leaves some columns idle at any moment */
    c->head = first ? (int)(rain_rnd(r) % (unsigned)(r->rows * 2)) - r->rows
                    : -(int)(rain_rnd(r) % (unsigned)(r->rows + 1));
}

static inline int rain_init(Rain *r, int cols, int rows, unsigned seed)
{
    memset(r, 0, sizeof *r);
    if (cols < 1) cols = 1;
    if (rows < 2) rows = 2;
    r->cols = cols; r->rows = rows;
    r->seed = seed ? seed : 0x9E3779B9u;
    size_t n = (size_t)cols * rows;
    r->col = calloc((size_t)cols, sizeof *r->col);
    r->glyph = malloc(n);
    r->shown_level = calloc(n, 1);
    r->shown_glyph = calloc(n, 1);
    if (!r->col || !r->glyph || !r->shown_level || !r->shown_glyph) return 0;
    for (size_t i = 0; i < n; i++) r->glyph[i] = (unsigned char)(rain_rnd(r) % RAIN_GLYPHS);
    for (int c = 0; c < cols; c++) rain_spawn(r, &r->col[c], 1);
    return 1;
}

static inline void rain_free(Rain *r)
{
    free(r->col); free(r->glyph); free(r->shown_level); free(r->shown_glyph);
    memset(r, 0, sizeof *r);
}

static inline int rain_level(const RainCol *c, int row)
{
    int d = c->head - row;
    if (d < 0 || d >= c->len) return 0;
    if (d == 0) return 1;
    return 2 + (d - 1) * (RAIN_LEVELS - 2) / (c->len - 1);
}

/* Advance one frame. */
static inline void rain_step(Rain *r)
{
    r->frame++;
    for (int ci = 0; ci < r->cols; ci++) {
        RainCol *c = &r->col[ci];
        if (r->frame % (unsigned)c->speed) continue;
        c->head++;
        if (c->head >= 0 && c->head < r->rows)
            r->glyph[c->head * r->cols + ci] = (unsigned char)(rain_rnd(r) % RAIN_GLYPHS);
        if (c->head - c->len >= r->rows) rain_spawn(r, c, 0);
    }
    /* a few glyphs inside trails flicker to a new character */
    for (int k = r->cols / 6 + 1; k > 0; k--) {
        int ci = (int)(rain_rnd(r) % (unsigned)r->cols);
        RainCol *c = &r->col[ci];
        int row = c->head - 1 - (int)(rain_rnd(r) % (unsigned)c->len);
        if (row >= 0 && row < r->rows)
            r->glyph[row * r->cols + ci] = (unsigned char)(rain_rnd(r) % RAIN_GLYPHS);
    }
}

/* Call draw(col, row, level, glyph, ctx) for every cell whose look changed since the last
 * call, and remember it as shown. all != 0 reports every lit cell (after a full repaint). */
typedef void (*rain_draw_fn)(int col, int row, int level, int glyph, void *ctx);

static inline void rain_update(Rain *r, int all, rain_draw_fn draw, void *ctx)
{
    for (int ci = 0; ci < r->cols; ci++) {
        RainCol *c = &r->col[ci];
        int from = all ? 0 : c->head - c->len - 3;   /* includes cells the trail just left */
        int to = all ? r->rows - 1 : c->head;
        if (from < 0) from = 0;
        if (to >= r->rows) to = r->rows - 1;
        for (int row = from; row <= to; row++) {
            size_t i = (size_t)row * r->cols + ci;
            int lv = rain_level(c, row);
            int gl = r->glyph[i];
            if (!all && lv == r->shown_level[i] && (lv == 0 || gl == r->shown_glyph[i])) continue;
            if (all && lv == 0) { r->shown_level[i] = 0; continue; }
            r->shown_level[i] = (unsigned char)lv;
            r->shown_glyph[i] = (unsigned char)gl;
            draw(ci, row, lv, gl, ctx);
        }
    }
}

#endif
