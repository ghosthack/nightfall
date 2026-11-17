/*
 * Gray mosaic model shared by the Windows and Linux builds (identical copies in
 * windows/src and linux/src; macos/src/main.swift ports the same logic).
 *
 * Each display is split into a grid of rectangles. When a cell size of at most
 * MOSAIC_MAX_CELL pixels divides both the width and the height, the cells are that
 * size and square (1920x1800 -> 40 px cells, 48x45). Otherwise each axis uses its own
 * largest divisor, and if that would be tiny (1366 = 2 x 683) the cells differ by one
 * pixel instead. Every cell holds a dark gray; a few random cells at a time crossfade
 * to a new gray, so mosaic_update() reports only the cells whose shade changed.
 */
#ifndef NIGHTFALL_MOSAIC_H
#define NIGHTFALL_MOSAIC_H

#include <stdlib.h>
#include <string.h>

#define MOSAIC_FPS        15
#define MOSAIC_MAX_CELL   50     /* pixels */
#define MOSAIC_MIN_CELL   24     /* smaller exact divisors fall back to near-equal cells */
#define MOSAIC_MAX_GRAY   64     /* 0..64 of 255: black to about 25% gray */
#define MOSAIC_CHANGE_SEC 12     /* each cell starts a new fade about this often */
#define MOSAIC_FADE_MIN   (1 * MOSAIC_FPS)
#define MOSAIC_FADE_MAX   (3 * MOSAIC_FPS)

typedef struct {
    unsigned char gray, from, to, shown;
    unsigned short t, dur;               /* frame of the fade, its length; dur 0 = idle */
} MosaicCell;

typedef struct {
    int cols, rows;
    int *xs, *ys;                        /* cell edges in pixels, cols+1 and rows+1 entries */
    MosaicCell *cell;
    unsigned seed, carry;                /* carry: fades owed, in 1/(CHANGE_SEC*FPS) units */
} Mosaic;

static inline unsigned mosaic_rnd(Mosaic *m)
{
    m->seed ^= m->seed << 13; m->seed ^= m->seed >> 17; m->seed ^= m->seed << 5;
    return m->seed;
}

/* largest d in [MOSAIC_MIN_CELL, MOSAIC_MAX_CELL] dividing every nonzero argument, or 0 */
static inline int mosaic_divisor(int a, int b)
{
    for (int d = MOSAIC_MAX_CELL; d >= MOSAIC_MIN_CELL; d--)
        if (a % d == 0 && (b == 0 || b % d == 0)) return d;
    return 0;
}

/* number of cells along an axis of px pixels */
static inline int mosaic_count(int px, int square)
{
    if (square) return px / square;
    int d = mosaic_divisor(px, 0);
    if (d) return px / d;
    return (px + MOSAIC_MAX_CELL - 1) / MOSAIC_MAX_CELL;   /* near-equal cells, each <= max */
}

static inline int mosaic_init(Mosaic *m, int w, int h, unsigned seed)
{
    memset(m, 0, sizeof *m);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int sq = mosaic_divisor(w, h);
    m->cols = mosaic_count(w, sq);
    m->rows = mosaic_count(h, sq);
    if (m->cols < 1) m->cols = 1;
    if (m->rows < 1) m->rows = 1;
    m->seed = seed ? seed : 0x9E3779B9u;
    m->xs = malloc(sizeof(int) * (size_t)(m->cols + 1));
    m->ys = malloc(sizeof(int) * (size_t)(m->rows + 1));
    m->cell = calloc((size_t)m->cols * m->rows, sizeof *m->cell);
    if (!m->xs || !m->ys || !m->cell) return 0;
    for (int i = 0; i <= m->cols; i++) m->xs[i] = (int)((long long)i * w / m->cols);
    for (int i = 0; i <= m->rows; i++) m->ys[i] = (int)((long long)i * h / m->rows);
    /* start as a full mosaic, every cell already showing its own shade */
    for (int i = 0; i < m->cols * m->rows; i++) {
        MosaicCell *c = &m->cell[i];
        c->gray = c->to = (unsigned char)(mosaic_rnd(m) % (MOSAIC_MAX_GRAY + 1));
    }
    return 1;
}

static inline void mosaic_free(Mosaic *m)
{
    free(m->xs); free(m->ys); free(m->cell);
    memset(m, 0, sizeof *m);
}

/* Advance one frame: move fading cells along, and start new fades on random idle cells. */
static inline void mosaic_step(Mosaic *m)
{
    int n = m->cols * m->rows;
    for (int i = 0; i < n; i++) {
        MosaicCell *c = &m->cell[i];
        if (!c->dur) continue;
        c->t++;
        double p = (double)c->t / c->dur;
        p = p * p * (3 - 2 * p);                         /* ease in and out */
        c->gray = (unsigned char)(c->from + (c->to - c->from) * p + 0.5);
        if (c->t >= c->dur) { c->gray = c->to; c->dur = 0; }
    }
    m->carry += (unsigned)n;
    const unsigned per = MOSAIC_CHANGE_SEC * MOSAIC_FPS;
    for (; m->carry >= per; m->carry -= per) {
        MosaicCell *c = &m->cell[mosaic_rnd(m) % (unsigned)n];
        if (c->dur) continue;                            /* already fading: skip this one */
        c->from = c->gray;
        c->to = (unsigned char)(mosaic_rnd(m) % (MOSAIC_MAX_GRAY + 1));
        c->t = 0;
        c->dur = (unsigned short)(MOSAIC_FADE_MIN + mosaic_rnd(m) % (MOSAIC_FADE_MAX - MOSAIC_FADE_MIN + 1));
    }
}

/* Call fill(x, y, w, h, gray, ctx) for every cell whose shade changed since the last call
 * (every cell when all != 0), and remember it as shown. */
typedef void (*mosaic_fill_fn)(int x, int y, int w, int h, int gray, void *ctx);

static inline void mosaic_update(Mosaic *m, int all, mosaic_fill_fn fill, void *ctx)
{
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            MosaicCell *k = &m->cell[r * m->cols + c];
            if (!all && k->gray == k->shown) continue;
            k->shown = k->gray;
            fill(m->xs[c], m->ys[r], m->xs[c + 1] - m->xs[c], m->ys[r + 1] - m->ys[r], k->gray, ctx);
        }
}

#endif
