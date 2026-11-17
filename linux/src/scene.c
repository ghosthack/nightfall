/*
 * Shared scene engine and the drawn styles. See scene.h.
 * Identical copies in linux/src, windows/src and macos/src.
 */
#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- style table ---------- */

static const char *const NAMES[NF_STYLE_COUNT] = {
    "clock", "rain", "mosaic", "stars", "analog", "words", "curves", "maze", "life",
    "ripples", "plasma", "fireflies", "starfield",
};
static const char *const TITLES[NF_STYLE_COUNT] = {
    "Clock", "Digital Rain", "Gray Mosaic", "Twinkling Stars", "Analog Clock", "Word Clock",
    "Curves", "Maze", "Life", "Ripples", "Plasma", "Fireflies", "Starfield",
};

const char *nf_style_name(int s) { return s >= 0 && s < NF_STYLE_COUNT ? NAMES[s] : ""; }
const char *nf_style_title(int s) { return s >= 0 && s < NF_STYLE_COUNT ? TITLES[s] : ""; }
int nf_style_from_name(const char *n)
{
    for (int i = 0; n && i < NF_STYLE_COUNT; i++)
        if (strcmp(n, NAMES[i]) == 0) return i;
    return -1;
}

/* ---------- shapes ---------- */

enum { P_RECT, P_DISC, P_GLOW, P_LINE, P_RING, P_MASK };

typedef struct {
    int32_t kind, mask;      /* mask: index into the scene's text masks */
    uint32_t color;          /* 0xRRGGBB */
    float a, b, c, d, e;     /* see the emit functions */
} Prim;

typedef struct { unsigned char *a; int w, h, gen; } Mask;

#define MAX_MASKS 4

struct NfScene {
    int style, w, h, fps;
    uint32_t *px;
    int own_px;
    int tc, tr;                         /* tiles */
    uint64_t *hash, *prev;              /* per tile */
    unsigned char *dirty, *lit;
    int *count, *start, *index;         /* per-tile shape lists */
    size_t index_cap;
    Prim *prim;
    size_t nprim, cap;
    unsigned seed;
    unsigned frame;
    float u;                            /* 1% of the shorter side, in pixels */
    nf_text_fn text;
    void *text_ctx;
    Mask mask[MAX_MASKS];
    void *st;                           /* the style's own state */
};

static unsigned rnd(NfScene *s)
{
    s->seed ^= s->seed << 13; s->seed ^= s->seed >> 17; s->seed ^= s->seed << 5;
    return s->seed;
}
static float frand(NfScene *s) { return (rnd(s) >> 8) * (1.0f / 16777216.0f); }   /* [0, 1) */
static float frange(NfScene *s, float lo, float hi) { return lo + (hi - lo) * frand(s); }
static int irange(NfScene *s, int lo, int hi) { return lo + (int)(rnd(s) % (unsigned)(hi - lo + 1)); }

static uint32_t rgb(float r, float g, float b)
{
    int R = (int)(r + 0.5f), G = (int)(g + 0.5f), B = (int)(b + 0.5f);
    R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
    return (uint32_t)(R << 16 | G << 8 | B);
}
static uint32_t dim(uint32_t c, float f)
{
    return rgb(((c >> 16) & 255) * f, ((c >> 8) & 255) * f, (c & 255) * f);
}
/* hue 0..1, saturation 0..1, value 0..255 */
static uint32_t hsv(float h, float sat, float v)
{
    h = (h - floorf(h)) * 6;
    int i = (int)h;
    float f = h - i, p = v * (1 - sat), q = v * (1 - sat * f), t = v * (1 - sat * (1 - f));
    switch (i % 6) {
    case 0: return rgb(v, t, p);
    case 1: return rgb(q, v, p);
    case 2: return rgb(p, v, t);
    case 3: return rgb(p, q, v);
    case 4: return rgb(t, p, v);
    default: return rgb(v, p, q);
    }
}

static Prim *emit(NfScene *s, int kind, uint32_t color)
{
    if (!color && kind != P_MASK) return NULL;   /* black on black: nothing to draw */
    if (s->nprim == s->cap) {
        size_t n = s->cap ? s->cap * 2 : 256;
        Prim *p = realloc(s->prim, n * sizeof *p);
        if (!p) return NULL;
        s->prim = p; s->cap = n;
    }
    Prim *p = &s->prim[s->nprim++];
    memset(p, 0, sizeof *p);
    p->kind = kind; p->color = color;
    return p;
}
static void rect(NfScene *s, float x, float y, float w, float h, uint32_t c)
{
    Prim *p = emit(s, P_RECT, c);
    if (p) { p->a = x; p->b = y; p->c = w; p->d = h; }
}
static void disc(NfScene *s, float x, float y, float r, uint32_t c)
{
    Prim *p = emit(s, P_DISC, c);
    if (p) { p->a = x; p->b = y; p->c = r; }
}
static void glow(NfScene *s, float x, float y, float r, uint32_t c)    /* soft falloff */
{
    Prim *p = emit(s, P_GLOW, c);
    if (p) { p->a = x; p->b = y; p->c = r; }
}
static void line(NfScene *s, float x0, float y0, float x1, float y1, float width, uint32_t c)
{
    Prim *p = emit(s, P_LINE, c);
    if (p) { p->a = x0; p->b = y0; p->c = x1; p->d = y1; p->e = width / 2; }
}
static void ring(NfScene *s, float x, float y, float r, float width, uint32_t c)
{
    Prim *p = emit(s, P_RING, c);
    if (p) { p->a = x; p->b = y; p->c = r; p->d = width / 2; }
}
static void text_mask(NfScene *s, int m, float x, float y, uint32_t c)
{
    if (m < 0 || m >= MAX_MASKS || !s->mask[m].a || !c) return;
    Prim *p = emit(s, P_MASK, c);
    if (p) { p->mask = m; p->a = floorf(x); p->b = floorf(y); p->e = (float)s->mask[m].gen; }  /* new text = new hash */
}

/* pixel bounds of a shape: [x0, x1) x [y0, y1) */
static void bounds(const NfScene *s, const Prim *p, float *x0, float *y0, float *x1, float *y1)
{
    float r;
    switch (p->kind) {
    case P_RECT: *x0 = p->a; *y0 = p->b; *x1 = p->a + p->c; *y1 = p->b + p->d; return;
    case P_DISC: case P_GLOW: r = p->c + 1; break;
    case P_RING: r = p->c + p->d + 1; break;
    case P_LINE:
        r = p->e + 1;
        *x0 = fminf(p->a, p->c) - r; *x1 = fmaxf(p->a, p->c) + r;
        *y0 = fminf(p->b, p->d) - r; *y1 = fmaxf(p->b, p->d) + r;
        return;
    default:
        *x0 = p->a; *y0 = p->b;
        *x1 = p->a + s->mask[p->mask].w; *y1 = p->b + s->mask[p->mask].h;
        return;
    }
    *x0 = p->a - r; *y0 = p->b - r; *x1 = p->a + r; *y1 = p->b + r;
}

/* does a ring cross tile (tx, ty)? (a big ring's bounding box covers many tiles it misses) */
static int ring_hits(const Prim *p, int tx, int ty)
{
    float x0 = (float)tx * NF_TILE, y0 = (float)ty * NF_TILE, x1 = x0 + NF_TILE, y1 = y0 + NF_TILE;
    float nx = fmaxf(x0, fminf(p->a, x1)) - p->a, ny = fmaxf(y0, fminf(p->b, y1)) - p->b;
    float fx = fmaxf(fabsf(x0 - p->a), fabsf(x1 - p->a)), fy = fmaxf(fabsf(y0 - p->b), fabsf(y1 - p->b));
    float dmin = sqrtf(nx * nx + ny * ny), dmax = sqrtf(fx * fx + fy * fy);
    return dmin <= p->c + p->d + 1 && dmax >= p->c - p->d - 1;
}

static int tile_range(const NfScene *s, const Prim *p, int *tx0, int *ty0, int *tx1, int *ty1)
{
    float x0, y0, x1, y1;
    bounds(s, p, &x0, &y0, &x1, &y1);
    if (x1 <= 0 || y1 <= 0 || x0 >= s->w || y0 >= s->h || x1 <= x0 || y1 <= y0) return 0;
    *tx0 = x0 < 0 ? 0 : (int)x0 / NF_TILE;
    *ty0 = y0 < 0 ? 0 : (int)y0 / NF_TILE;
    *tx1 = (int)ceilf(x1) - 1; *ty1 = (int)ceilf(y1) - 1;
    if (*tx1 >= s->w) *tx1 = s->w - 1;
    if (*ty1 >= s->h) *ty1 = s->h - 1;
    *tx1 /= NF_TILE; *ty1 /= NF_TILE;
    return 1;
}

/* ---------- rasterizing ---------- */

static inline void blend(uint32_t *d, uint32_t c, float a)
{
    if (a <= 0) return;
    if (a >= 1) { *d = 0xFF000000u | c; return; }
    uint32_t o = *d;
    int r = (int)((o >> 16) & 255), g = (int)((o >> 8) & 255), b = (int)(o & 255);
    r += (int)((((c >> 16) & 255) - r) * a);
    g += (int)((((c >> 8) & 255) - g) * a);
    b += (int)(((c & 255) - b) * a);
    *d = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static void draw_prim(NfScene *s, const Prim *p, int cx0, int cy0, int cx1, int cy1)
{
    float fx0, fy0, fx1, fy1;
    bounds(s, p, &fx0, &fy0, &fx1, &fy1);
    int x0 = (int)floorf(fx0), y0 = (int)floorf(fy0), x1 = (int)ceilf(fx1), y1 = (int)ceilf(fy1);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t c = p->color;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = s->px + (size_t)y * s->w;
        float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, a;
            switch (p->kind) {
            case P_RECT:
                row[x] = 0xFF000000u | c;
                continue;
            case P_DISC: {
                float dx = px - p->a, dy = py - p->b;
                a = p->c + 0.5f - sqrtf(dx * dx + dy * dy);
                break;
            }
            case P_GLOW: {
                float dx = px - p->a, dy = py - p->b;
                float t = 1 - sqrtf(dx * dx + dy * dy) / p->c;
                a = t <= 0 ? 0 : t * t;
                break;
            }
            case P_RING: {
                float dx = px - p->a, dy = py - p->b;
                a = p->d + 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - p->c);
                break;
            }
            case P_LINE: {
                float vx = p->c - p->a, vy = p->d - p->b, wx = px - p->a, wy = py - p->b;
                float l2 = vx * vx + vy * vy, t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                float dx = wx - t * vx, dy = wy - t * vy;
                a = p->e + 0.5f - sqrtf(dx * dx + dy * dy);
                break;
            }
            default: {
                const Mask *m = &s->mask[p->mask];
                a = m->a[(y - (int)p->b) * m->w + (x - (int)p->a)] * (1.0f / 255);
                break;
            }
            }
            blend(&row[x], c, a > 1 ? 1 : a);
        }
    }
}

/* Hash every tile's shapes, re-render the tiles that changed. */
static int render(NfScene *s)
{
    int nt = s->tc * s->tr;
    memset(s->count, 0, sizeof(int) * (size_t)nt);
    for (int t = 0; t < nt; t++) s->hash[t] = 1469598103934665603ull;
    size_t refs = 0;
    for (size_t i = 0; i < s->nprim; i++) {
        const Prim *p = &s->prim[i];
        int tx0, ty0, tx1, ty1;
        if (!tile_range(s, p, &tx0, &ty0, &tx1, &ty1)) continue;
        const unsigned char *bytes = (const unsigned char *)p;
        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = tx0; tx <= tx1; tx++) {
                if (p->kind == P_RING && !ring_hits(p, tx, ty)) continue;
                int t = ty * s->tc + tx;
                uint64_t h = s->hash[t];
                for (size_t k = 0; k < sizeof *p; k++) { h ^= bytes[k]; h *= 1099511628211ull; }
                s->hash[t] = h;
                s->count[t]++;
                refs++;
            }
    }
    if (refs > s->index_cap) {
        int *ix = realloc(s->index, refs * sizeof *ix);
        if (!ix) return 0;
        s->index = ix; s->index_cap = refs;
    }
    int off = 0;
    for (int t = 0; t < nt; t++) { s->start[t] = off; off += s->count[t]; s->count[t] = 0; }
    for (size_t i = 0; i < s->nprim; i++) {
        const Prim *p = &s->prim[i];
        int tx0, ty0, tx1, ty1;
        if (!tile_range(s, p, &tx0, &ty0, &tx1, &ty1)) continue;
        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = tx0; tx <= tx1; tx++) {
                if (p->kind == P_RING && !ring_hits(p, tx, ty)) continue;
                int t = ty * s->tc + tx;
                s->index[s->start[t] + s->count[t]++] = (int)i;
            }
    }
    int changed = 0;
    for (int ty = 0; ty < s->tr; ty++)
        for (int tx = 0; tx < s->tc; tx++) {
            int t = ty * s->tc + tx;
            s->dirty[t] = s->hash[t] != s->prev[t];
            if (!s->dirty[t]) continue;
            s->prev[t] = s->hash[t];
            s->lit[t] = s->count[t] > 0;
            changed++;
            int x0 = tx * NF_TILE, y0 = ty * NF_TILE;
            int x1 = x0 + NF_TILE < s->w ? x0 + NF_TILE : s->w, y1 = y0 + NF_TILE < s->h ? y0 + NF_TILE : s->h;
            for (int y = y0; y < y1; y++) {
                uint32_t *row = s->px + (size_t)y * s->w;
                for (int x = x0; x < x1; x++) row[x] = 0xFF000000u;
            }
            for (int k = 0; k < s->count[t]; k++)
                draw_prim(s, &s->prim[s->index[s->start[t] + k]], x0, y0, x1, y1);
        }
    return changed;
}

/* ---------- shared helpers for the styles ---------- */

/* a cell that eases from one value to another over dur frames */
typedef struct { float v, from, to; unsigned short t, dur; } Fade;

static void fade_step(Fade *f)
{
    if (!f->dur) return;
    f->t++;
    float p = (float)f->t / f->dur;
    p = p * p * (3 - 2 * p);
    f->v = f->from + (f->to - f->from) * p;
    if (f->t >= f->dur) { f->v = f->to; f->dur = 0; }
}
static void fade_to(Fade *f, float to, int frames)
{
    f->from = f->v; f->to = to; f->t = 0; f->dur = (unsigned short)(frames > 0 ? frames : 1);
}

/* a "build, hold, fade out, start over" cycle used by curves and maze */
enum { PH_BUILD, PH_HOLD, PH_FADE };

static void local_hm(int *h, int *m, int *sec)
{
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    tmv = *localtime(&t);
#else
    localtime_r(&t, &tmv);
#endif
    *h = tmv.tm_hour; *m = tmv.tm_min; *sec = tmv.tm_sec;
}

/* ---------- stars: dim points, a few at a time brighten or fade ---------- */

typedef struct { float x, y, r; uint32_t tint; Fade f; } Star;
typedef struct { int n; Star *st; } Stars;

static void stars_init(NfScene *s)
{
    Stars *z = calloc(1, sizeof *z);
    s->st = z;
    if (!z) return;
    z->n = (int)((float)s->w * s->h / (s->u * s->u * 28));
    if (z->n > 4000) z->n = 4000;
    z->st = calloc((size_t)z->n, sizeof *z->st);
    if (!z->st) { z->n = 0; return; }
    for (int i = 0; i < z->n; i++) {
        Star *a = &z->st[i];
        a->x = frand(s) * s->w; a->y = frand(s) * s->h;
        float big = frand(s);
        a->r = fmaxf(0.7f, s->u * (0.07f + 0.16f * big * big * big));
        float warm = frand(s);
        a->tint = warm < 0.2f ? rgb(255, 225, 190) : warm < 0.45f ? rgb(195, 210, 255) : rgb(230, 232, 240);
        a->f.v = frange(s, 0.05f, 0.6f);
    }
}
static void stars_frame(NfScene *s)
{
    Stars *z = s->st;
    if (!z) return;
    /* each star starts a new twinkle about every 15 s */
    for (int k = z->n / (15 * s->fps) + ((int)(rnd(s) % (unsigned)(15 * s->fps)) < z->n % (15 * s->fps)); k > 0; k--) {
        Star *a = &z->st[rnd(s) % (unsigned)z->n];
        if (!a->f.dur) fade_to(&a->f, frange(s, 0.02f, 0.75f), irange(s, s->fps / 2, s->fps * 2));
    }
    for (int i = 0; i < z->n; i++) {
        Star *a = &z->st[i];
        fade_step(&a->f);
        float b = a->f.v * 0.62f;
        if (b > 0.02f) disc(s, a->x, a->y, a->r, dim(a->tint, b));
    }
}

/* ---------- analog clock: a dim face that moves every 20 s ---------- */

typedef struct { float cx, cy; int last_move; } Analog;

static void analog_place(NfScene *s, Analog *a)
{
    float r = fminf(s->w, s->h) * 0.24f;
    a->cx = r * 1.1f + frand(s) * fmaxf(0, s->w - 2.2f * r);
    a->cy = r * 1.1f + frand(s) * fmaxf(0, s->h - 2.2f * r);
}
static void analog_init(NfScene *s)
{
    Analog *a = calloc(1, sizeof *a);
    s->st = a;
    if (a) analog_place(s, a);
}
static void analog_frame(NfScene *s)
{
    Analog *a = s->st;
    if (!a) return;
    if (s->frame - (unsigned)a->last_move >= 20u * (unsigned)s->fps) { a->last_move = (int)s->frame; analog_place(s, a); }
    int h, m, sec;
    local_hm(&h, &m, &sec);
    float R = fminf(s->w, s->h) * 0.24f, cx = a->cx, cy = a->cy;
    uint32_t face = rgb(62, 62, 68), hand = rgb(92, 92, 100), red = rgb(120, 52, 46);
    for (int i = 0; i < 60; i++) {
        float t = i * (float)M_PI / 30, sx = sinf(t), cy_ = -cosf(t);
        if (i % 5 == 0) line(s, cx + sx * R * 0.86f, cy + cy_ * R * 0.86f, cx + sx * R, cy + cy_ * R, R * 0.028f, face);
        else disc(s, cx + sx * R * 0.96f, cy + cy_ * R * 0.96f, fmaxf(0.8f, R * 0.009f), dim(face, 0.8f));
    }
    float th = ((h % 12) + m / 60.0f) * (float)M_PI / 6, tm = (m + sec / 60.0f) * (float)M_PI / 30, ts = sec * (float)M_PI / 30;
    line(s, cx, cy, cx + sinf(th) * R * 0.5f, cy - cosf(th) * R * 0.5f, R * 0.045f, hand);
    line(s, cx, cy, cx + sinf(tm) * R * 0.78f, cy - cosf(tm) * R * 0.78f, R * 0.03f, hand);
    line(s, cx - sinf(ts) * R * 0.15f, cy + cosf(ts) * R * 0.15f, cx + sinf(ts) * R * 0.86f, cy - cosf(ts) * R * 0.86f,
         fmaxf(1, R * 0.01f), red);
    disc(s, cx, cy, R * 0.035f, red);
}

/* ---------- word clock: "it's about ten past four", moves every 20 s ---------- */

typedef struct { char line1[48], line2[48]; float x, y; int last_move; } Words;

static void words_phrase(Words *w)
{
    static const char *const HOURS[12] = { "twelve", "one", "two", "three", "four", "five", "six",
                                           "seven", "eight", "nine", "ten", "eleven" };
    static const char *const MINS[12] = { "", "five past", "ten past", "quarter past", "twenty past",
                                          "twenty-five past", "half past", "twenty-five to", "twenty to",
                                          "quarter to", "ten to", "five to" };
    int h, m, sec;
    local_hm(&h, &m, &sec);
    int r = (m + 2) / 5;                 /* nearest five minutes, 0..12 */
    const char *about = m % 5 ? "about " : "";
    if (r >= 7) h++;
    if (r == 12) r = 0;
    const char *hour = HOURS[h % 12];
    char l1[48], l2[48];
    if (r == 0) {
        snprintf(l1, sizeof l1, "%s", about[0] ? "it's about" : "it's");
        snprintf(l2, sizeof l2, "%s o'clock", hour);
    } else {
        snprintf(l1, sizeof l1, "it's %s%s", about, MINS[r]);
        snprintf(l2, sizeof l2, "%s", hour);
    }
    memcpy(w->line1, l1, sizeof l1);
    memcpy(w->line2, l2, sizeof l2);
}
static void words_masks(NfScene *s, Words *w)
{
    for (int i = 0; i < 2; i++) {
        free(s->mask[i].a);
        s->mask[i].a = NULL;
        s->mask[i].gen++;
        if (!s->text) continue;
        int px = (int)(fminf(s->w, s->h) * (i == 0 ? 0.075f : 0.14f));
        s->mask[i].a = s->text(i == 0 ? w->line1 : w->line2, px, &s->mask[i].w, &s->mask[i].h, s->text_ctx);
    }
}
static void words_place(NfScene *s, Words *w)
{
    int bw = s->mask[0].w > s->mask[1].w ? s->mask[0].w : s->mask[1].w;
    int bh = s->mask[0].h + s->mask[1].h;
    w->x = frand(s) * fmaxf(0, s->w - bw - s->u * 4) + s->u * 2;
    w->y = frand(s) * fmaxf(0, s->h - bh - s->u * 4) + s->u * 2;
}
static void words_init(NfScene *s)
{
    Words *w = calloc(1, sizeof *w);
    s->st = w;
    if (!w) return;
    words_phrase(w);
    words_masks(s, w);
    words_place(s, w);
}
static void words_frame(NfScene *s)
{
    Words *w = s->st;
    if (!w) return;
    char l1[48], l2[48];
    memcpy(l1, w->line1, sizeof l1); memcpy(l2, w->line2, sizeof l2);
    words_phrase(w);
    if (strcmp(l1, w->line1) || strcmp(l2, w->line2)) words_masks(s, w);
    if (s->frame - (unsigned)w->last_move >= 20u * (unsigned)s->fps) { w->last_move = (int)s->frame; words_place(s, w); }
    text_mask(s, 0, w->x, w->y, rgb(58, 58, 64));
    text_mask(s, 1, w->x - s->mask[1].h * 0.03f, w->y + s->mask[0].h, rgb(84, 84, 92));
}

/* ---------- curves: a hypotrochoid drawn slowly, held, faded, replaced ---------- */

typedef struct {
    float cx, cy, scale, R, r, d, period, width;
    uint32_t color;
    int n, shown, phase, phase_t;
} Curves;

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static void curves_new(NfScene *s, Curves *c)
{
    int R = irange(s, 5, 13), r;
    do r = irange(s, 2, R - 1); while (gcd(R, r) == r);
    c->R = (float)R; c->r = (float)r; c->d = r * frange(s, 0.45f, 1.25f);
    c->period = 2 * (float)M_PI * (float)(r / gcd(R, r));
    float ext = (c->R - c->r) + c->d;
    float size = fminf(s->w, s->h) * frange(s, 0.28f, 0.42f);
    c->scale = size / ext;
    c->cx = size + frand(s) * fmaxf(0, s->w - 2 * size);
    c->cy = size + frand(s) * fmaxf(0, s->h - 2 * size);
    c->n = (int)(c->period / (2 * (float)M_PI) * 260);
    if (c->n > 5000) c->n = 5000;
    c->width = fmaxf(1.0f, s->u * 0.12f);
    c->color = hsv(frand(s), frange(s, 0.35f, 0.6f), 110);
    c->shown = 0; c->phase = PH_BUILD; c->phase_t = 0;
}
static void curves_point(const Curves *c, int i, float *x, float *y)
{
    float t = c->period * i / c->n, k = (c->R - c->r) / c->r;
    *x = c->cx + c->scale * ((c->R - c->r) * cosf(t) + c->d * cosf(k * t));
    *y = c->cy + c->scale * ((c->R - c->r) * sinf(t) - c->d * sinf(k * t));
}
static void curves_init(NfScene *s)
{
    Curves *c = calloc(1, sizeof *c);
    s->st = c;
    if (c) curves_new(s, c);
}
static void curves_frame(NfScene *s)
{
    Curves *c = s->st;
    if (!c) return;
    float f = 1;
    if (c->phase == PH_BUILD) {
        c->shown += c->n / (45 * s->fps) + 1;                 /* about 45 s to draw */
        if (c->shown >= c->n) { c->shown = c->n; c->phase = PH_HOLD; c->phase_t = 0; }
    } else if (c->phase == PH_HOLD) {
        if (++c->phase_t >= 8 * s->fps) { c->phase = PH_FADE; c->phase_t = 0; }
    } else {
        f = 1 - (float)++c->phase_t / (2 * s->fps);
        if (f <= 0) { curves_new(s, c); f = 1; }
    }
    uint32_t col = dim(c->color, f);
    float px, py, x, y;
    curves_point(c, 0, &px, &py);
    for (int i = 1; i <= c->shown; i++) {
        curves_point(c, i, &x, &y);
        line(s, px, py, x, y, c->width, col);
        px = x; py = y;
    }
    if (c->phase == PH_BUILD) disc(s, px, py, c->width * 1.6f, rgb(170, 170, 180));
}

/* ---------- maze: carved one passage at a time, held, faded, rebuilt ---------- */

typedef struct {
    int mx, my, cell, ox, oy;           /* nodes; pixels per grid square; origin */
    unsigned char *open;                /* (2mx-1) x (2my-1) grid: 1 = carved */
    int *stack, sp;
    int phase, phase_t, per_frame;
} Maze;

static void maze_reset(NfScene *s, Maze *z)
{
    memset(z->open, 0, (size_t)(2 * z->mx - 1) * (2 * z->my - 1));
    z->sp = 0;
    int start = irange(s, 0, z->mx * z->my - 1);
    z->stack[z->sp++] = start;
    z->open[(start / z->mx) * 2 * (2 * z->mx - 1) + (start % z->mx) * 2] = 1;
    z->phase = PH_BUILD; z->phase_t = 0;
}
static void maze_init(NfScene *s)
{
    Maze *z = calloc(1, sizeof *z);
    s->st = z;
    if (!z) return;
    z->cell = (int)fmaxf(6, fminf(s->w, s->h) / 55);
    int gw = s->w / z->cell, gh = s->h / z->cell;     /* grid squares incl. walls */
    if (gw % 2 == 0) gw--;
    if (gh % 2 == 0) gh--;
    z->mx = (gw - 1) / 2; z->my = (gh - 1) / 2;
    if (z->mx < 2) z->mx = 2;
    if (z->my < 2) z->my = 2;
    z->ox = (s->w - (2 * z->mx - 1) * z->cell) / 2;
    z->oy = (s->h - (2 * z->my - 1) * z->cell) / 2;
    z->open = calloc((size_t)(2 * z->mx - 1) * (2 * z->my - 1), 1);
    z->stack = calloc((size_t)z->mx * z->my, sizeof *z->stack);
    if (!z->open || !z->stack) { free(z->open); free(z->stack); free(z); s->st = NULL; return; }
    z->per_frame = z->mx * z->my / (100 * s->fps) + 1;   /* about 100 s to carve */
    maze_reset(s, z);
}
static void maze_step(NfScene *s, Maze *z)
{
    int gwid = 2 * z->mx - 1;
    while (z->sp > 0) {
        int cur = z->stack[z->sp - 1], x = cur % z->mx, y = cur / z->mx;
        int dirs[4], nd = 0;
        static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= z->mx || ny >= z->my) continue;
            if (!z->open[ny * 2 * gwid + nx * 2]) dirs[nd++] = d;
        }
        if (!nd) { z->sp--; continue; }               /* dead end: back up (free) */
        int d = dirs[rnd(s) % (unsigned)nd], nx = x + DX[d], ny = y + DY[d];
        z->open[(y * 2 + DY[d]) * gwid + x * 2 + DX[d]] = 1;
        z->open[ny * 2 * gwid + nx * 2] = 1;
        z->stack[z->sp++] = ny * z->mx + nx;
        return;
    }
}
static void maze_frame(NfScene *s)
{
    Maze *z = s->st;
    if (!z) return;
    float f = 1;
    if (z->phase == PH_BUILD) {
        for (int k = 0; k < z->per_frame; k++) maze_step(s, z);
        if (z->sp == 0) { z->phase = PH_HOLD; z->phase_t = 0; }
    } else if (z->phase == PH_HOLD) {
        if (++z->phase_t >= 10 * s->fps) { z->phase = PH_FADE; z->phase_t = 0; }
    } else {
        f = 1 - (float)++z->phase_t / (3 * s->fps);
        if (f <= 0) { maze_reset(s, z); f = 1; }
    }
    int gw = 2 * z->mx - 1, gh = 2 * z->my - 1, c = z->cell;
    uint32_t col = dim(rgb(46, 54, 74), f);
    /* merge horizontal runs of carved squares into one rectangle */
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw;) {
            if (!z->open[y * gw + x]) { x++; continue; }
            int x0 = x;
            while (x < gw && z->open[y * gw + x]) x++;
            rect(s, (float)(z->ox + x0 * c), (float)(z->oy + y * c), (float)((x - x0) * c), (float)c, col);
        }
    if (z->phase == PH_BUILD && z->sp > 0) {
        int cur = z->stack[z->sp - 1];
        rect(s, (float)(z->ox + (cur % z->mx) * 2 * c), (float)(z->oy + (cur / z->mx) * 2 * c), (float)c, (float)c,
             rgb(120, 140, 175));
    }
}

/* ---------- life: Conway's rules on a grid, cells fade in and out ---------- */

typedef struct {
    int cols, rows, cell, ox, oy, gen;
    unsigned char *alive, *next;
    float *glow;
    uint32_t hist[24];
    int stale;
} Life;

static void life_seed(NfScene *s, Life *l)
{
    for (int i = 0; i < l->cols * l->rows; i++) l->alive[i] = frand(s) < 0.22f;
    l->gen = 0; l->stale = 0;
    memset(l->hist, 0, sizeof l->hist);
}
static void life_init(NfScene *s)
{
    Life *l = calloc(1, sizeof *l);
    s->st = l;
    if (!l) return;
    l->cell = (int)fmaxf(5, fminf(s->w, s->h) / 72);
    l->cols = s->w / l->cell; l->rows = s->h / l->cell;
    l->ox = (s->w - l->cols * l->cell) / 2; l->oy = (s->h - l->rows * l->cell) / 2;
    size_t n = (size_t)l->cols * l->rows;
    l->alive = calloc(n, 1); l->next = calloc(n, 1); l->glow = calloc(n, sizeof *l->glow);
    if (!l->alive || !l->next || !l->glow) {
        free(l->alive); free(l->next); free(l->glow); free(l); s->st = NULL; return;
    }
    life_seed(s, l);
}
static void life_generation(NfScene *s, Life *l)
{
    int W = l->cols, H = l->rows;
    uint32_t h = 2166136261u;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx || dy) n += l->alive[((y + dy + H) % H) * W + (x + dx + W) % W];
            unsigned char a = l->alive[y * W + x];
            unsigned char v = (unsigned char)(n == 3 || (a && n == 2));
            l->next[y * W + x] = v;
            if (v) h = (h ^ (uint32_t)(y * W + x)) * 16777619u;
        }
    unsigned char *t = l->alive; l->alive = l->next; l->next = t;
    l->gen++;
    /* settled into a still life or a short loop: let it sit a little, then reseed */
    int seen = 0;
    for (int i = 0; i < 24; i++) seen |= l->hist[i] == h;
    l->hist[l->gen % 24] = h;
    if (seen) l->stale++;
    if (l->stale > 40 || l->gen > 3000) life_seed(s, l);
}
static void life_frame(NfScene *s)
{
    Life *l = s->st;
    if (!l) return;
    int per = s->fps / 2 > 0 ? s->fps / 2 : 1;          /* 2 generations a second */
    if (s->frame % (unsigned)per == 0) life_generation(s, l);
    int c = l->cell, gap = c >= 8 ? 1 : 0;
    for (int y = 0; y < l->rows; y++)
        for (int x = 0; x < l->cols; x++) {
            int i = y * l->cols + x;
            float g = l->glow[i];
            g = l->alive[i] ? 1 : g > 0.5f ? 0.35f : 0;   /* dying cells linger one dim frame */
            l->glow[i] = g;
            if (g > 0) rect(s, (float)(l->ox + x * c), (float)(l->oy + y * c), (float)(c - gap), (float)(c - gap),
                            rgb(24 * g, 96 * g, 90 * g));
        }
}

/* ---------- ripples: drops land, rings spread and fade ---------- */

#define MAX_RINGS 24
typedef struct { float x, y, r, born; } Ring;
typedef struct { Ring ring[MAX_RINGS]; int n, next_drop; } Ripples;

static void ripples_init(NfScene *s) { s->st = calloc(1, sizeof(Ripples)); }
static void ripples_frame(NfScene *s)
{
    Ripples *z = s->st;
    if (!z) return;
    float maxr = fminf(s->w, s->h) * 0.42f, speed = fminf(s->w, s->h) * 0.07f / s->fps;
    if ((int)s->frame >= z->next_drop) {
        float x = frand(s) * s->w, y = frand(s) * s->h;
        int rings = 2;
        for (int k = 0; k < rings && z->n < MAX_RINGS; k++) {
            Ring *r = &z->ring[z->n++];
            r->x = x; r->y = y; r->r = -k * maxr * 0.08f; r->born = 0;
        }
        z->next_drop = (int)s->frame + irange(s, s->fps * 3 / 2, s->fps * 4);
    }
    float width = fmaxf(1.2f, s->u * 0.16f);
    for (int i = 0; i < z->n;) {
        Ring *r = &z->ring[i];
        r->r += speed;
        if (r->r >= maxr) { z->ring[i] = z->ring[--z->n]; continue; }
        if (r->r > 0) {
            float f = 1 - r->r / maxr;
            ring(s, r->x, r->y, r->r, width, rgb(70 * f, 110 * f, 150 * f));
        }
        i++;
    }
}

/* ---------- plasma: a slow color field on a coarse grid ---------- */

typedef struct { int cols, rows; float t; float *v; } Plasma;

static void plasma_init(NfScene *s)
{
    Plasma *p = calloc(1, sizeof *p);
    s->st = p;
    if (!p) return;
    int cell = (int)fmaxf(8, fminf(s->w, s->h) / 30);
    p->cols = (s->w + cell - 1) / cell; p->rows = (s->h + cell - 1) / cell;
    p->t = frand(s) * 100;
}
static void plasma_frame(NfScene *s)
{
    Plasma *p = s->st;
    if (!p) return;
    p->t += 0.2f / s->fps;
    float t = p->t;
    for (int y = 0; y < p->rows; y++)
        for (int x = 0; x < p->cols; x++) {
            float fx = (float)x / p->cols * 6, fy = (float)y / p->rows * 6 * p->rows / p->cols;
            float v = sinf(fx + t) + sinf(fy * 1.3f - t * 0.8f) + sinf((fx + fy) * 0.7f + t * 0.6f)
                    + sinf(sqrtf((fx - 3) * (fx - 3) + (fy - 2) * (fy - 2)) * 1.5f - t);
            v = v / 8 + 0.5f;                               /* 0..1 */
            v = roundf(v * 32) / 32;                        /* coarse steps: most cells hold still */
            uint32_t c = hsv(0.55f + 0.35f * v + roundf(t * 0.02f * 32) / 32, 0.75f, 20 + 42 * v);
            int X0 = x * s->w / p->cols, X1 = (x + 1) * s->w / p->cols;
            int Y0 = y * s->h / p->rows, Y1 = (y + 1) * s->h / p->rows;
            rect(s, (float)X0, (float)Y0, (float)(X1 - X0), (float)(Y1 - Y0), c);
        }
}

/* ---------- fireflies: soft glows wandering and pulsing ---------- */

typedef struct { float x, y, vx, vy, heading; Fade f; int wait; } Fly;
typedef struct { int n; Fly fly[64]; } Fireflies;

static void fireflies_init(NfScene *s)
{
    Fireflies *z = calloc(1, sizeof *z);
    s->st = z;
    if (!z) return;
    z->n = (int)((float)s->w * s->h / (s->u * s->u * 450));
    z->n = z->n < 12 ? 12 : z->n > 64 ? 64 : z->n;
    for (int i = 0; i < z->n; i++) {
        Fly *f = &z->fly[i];
        f->x = frand(s) * s->w; f->y = frand(s) * s->h;
        f->heading = frand(s) * 2 * (float)M_PI;
        f->wait = irange(s, 0, s->fps * 4);
    }
}
static void fireflies_frame(NfScene *s)
{
    Fireflies *z = s->st;
    if (!z) return;
    float speed = s->u * 2.2f / s->fps;
    for (int i = 0; i < z->n; i++) {
        Fly *f = &z->fly[i];
        f->heading += frange(s, -0.25f, 0.25f);
        /* steer gently back from the edges */
        float tx = s->w / 2.0f - f->x, ty = s->h / 2.0f - f->y;
        if (f->x < s->u * 5 || f->x > s->w - s->u * 5 || f->y < s->u * 5 || f->y > s->h - s->u * 5)
            f->heading = atan2f(ty, tx) + frange(s, -0.6f, 0.6f);
        f->x += cosf(f->heading) * speed; f->y += sinf(f->heading) * speed;
        fade_step(&f->f);
        if (!f->f.dur) {
            if (f->wait > 0) f->wait--;
            else if (f->f.v < 0.5f) { fade_to(&f->f, frange(s, 0.6f, 1), irange(s, s->fps / 2, s->fps * 3 / 2)); }
            else { fade_to(&f->f, 0, irange(s, s->fps, s->fps * 2)); f->wait = irange(s, s->fps, s->fps * 6); }
        }
        float b = f->f.v;
        if (b < 0.03f) continue;
        glow(s, f->x, f->y, s->u * 1.6f, rgb(150 * b, 190 * b, 60 * b));
        disc(s, f->x, f->y, fmaxf(0.8f, s->u * 0.14f), rgb(230 * b, 255 * b, 150 * b));
    }
}

/* ---------- starfield: flying forward through dim stars ---------- */

typedef struct { float x, y, z; } Point3;
typedef struct { int n; Point3 p[400]; } Starfield;

static void starfield_spawn(NfScene *s, Point3 *p, int far)
{
    p->x = frange(s, -1, 1); p->y = frange(s, -1, 1) * s->h / s->w;
    p->z = far ? 1 : frange(s, 0.15f, 1);
}
static void starfield_init(NfScene *s)
{
    Starfield *z = calloc(1, sizeof *z);
    s->st = z;
    if (!z) return;
    z->n = (int)((float)s->w * s->h / (s->u * s->u * 160));
    z->n = z->n < 40 ? 40 : z->n > 400 ? 400 : z->n;
    for (int i = 0; i < z->n; i++) starfield_spawn(s, &z->p[i], 0);
}
static void starfield_frame(NfScene *s)
{
    Starfield *z = s->st;
    if (!z) return;
    float cx = s->w / 2.0f, cy = s->h / 2.0f, fov = s->w * 0.5f;
    for (int i = 0; i < z->n; i++) {
        Point3 *p = &z->p[i];
        p->z -= 0.12f / s->fps;
        float X = cx + p->x / p->z * fov, Y = cy + p->y / p->z * fov;
        if (p->z <= 0.04f || X < -10 || Y < -10 || X > s->w + 10 || Y > s->h + 10) {
            starfield_spawn(s, p, 1);
            continue;
        }
        float close = 1 - p->z;
        float b = close * close;
        if (b < 0.02f) continue;
        disc(s, X, Y, fmaxf(0.6f, s->u * 0.28f * close * close), rgb(200 * b, 205 * b, 225 * b));
    }
}

/* ---------- scene object ---------- */

typedef struct { void (*init)(NfScene *); void (*frame)(NfScene *); int fps; } StyleDef;

static const StyleDef DEFS[NF_STYLE_COUNT] = {
    [NF_STARS] = { stars_init, stars_frame, 10 },
    [NF_ANALOG] = { analog_init, analog_frame, 2 },
    [NF_WORDS] = { words_init, words_frame, 1 },
    [NF_CURVES] = { curves_init, curves_frame, 15 },
    [NF_MAZE] = { maze_init, maze_frame, 15 },
    [NF_LIFE] = { life_init, life_frame, 4 },
    [NF_RIPPLES] = { ripples_init, ripples_frame, 10 },
    [NF_PLASMA] = { plasma_init, plasma_frame, 4 },
    [NF_FIREFLIES] = { fireflies_init, fireflies_frame, 15 },
    [NF_STARFIELD] = { starfield_init, starfield_frame, 15 },
};

static void free_state(NfScene *s)
{
    void *st = s->st;
    if (!st) return;
    switch (s->style) {
    case NF_STARS: free(((Stars *)st)->st); break;
    case NF_MAZE: free(((Maze *)st)->open); free(((Maze *)st)->stack); break;
    case NF_LIFE: free(((Life *)st)->alive); free(((Life *)st)->next); free(((Life *)st)->glow); break;
    }
    free(st);
    s->st = NULL;
}

NfScene *nf_scene_new(int style, int w, int h, uint32_t *pixels, unsigned seed, nf_text_fn text, void *text_ctx)
{
    if (style < NF_FIRST_SCENE || style >= NF_STYLE_COUNT || w < 1 || h < 1) return NULL;
    NfScene *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->style = style; s->w = w; s->h = h; s->fps = DEFS[style].fps;
    s->seed = seed ? seed : 0x9E3779B9u;
    s->u = (w < h ? w : h) / 100.0f;
    s->text = text; s->text_ctx = text_ctx;
    s->px = pixels;
    if (!s->px) { s->px = malloc((size_t)w * h * 4); s->own_px = 1; }
    s->tc = (w + NF_TILE - 1) / NF_TILE; s->tr = (h + NF_TILE - 1) / NF_TILE;
    size_t nt = (size_t)s->tc * s->tr;
    s->hash = calloc(nt, 8); s->prev = calloc(nt, 8);
    s->dirty = calloc(nt, 1); s->lit = calloc(nt, 1);
    s->count = calloc(nt, sizeof(int)); s->start = calloc(nt, sizeof(int));
    if (!s->px || !s->hash || !s->prev || !s->dirty || !s->lit || !s->count || !s->start) { nf_scene_free(s); return NULL; }
    for (size_t i = 0; i < (size_t)w * h; i++) s->px[i] = 0xFF000000u;
    /* an empty tile hashes to the FNV offset basis: start every tile as "black, shown" */
    for (size_t t = 0; t < nt; t++) s->prev[t] = 1469598103934665603ull;
    DEFS[style].init(s);
    return s;
}

void nf_scene_free(NfScene *s)
{
    if (!s) return;
    free_state(s);
    for (int i = 0; i < MAX_MASKS; i++) free(s->mask[i].a);
    if (s->own_px) free(s->px);
    free(s->hash); free(s->prev); free(s->dirty); free(s->lit);
    free(s->count); free(s->start); free(s->index); free(s->prim);
    free(s);
}

int nf_scene_fps(const NfScene *s) { return s->fps; }

int nf_scene_step(NfScene *s)
{
    s->nprim = 0;
    s->frame++;
    DEFS[s->style].frame(s);
    return render(s);
}

int nf_scene_width(const NfScene *s) { return s->w; }
int nf_scene_height(const NfScene *s) { return s->h; }
const uint32_t *nf_scene_pixels(const NfScene *s) { return s->px; }
int nf_scene_tile_cols(const NfScene *s) { return s->tc; }
int nf_scene_tile_rows(const NfScene *s) { return s->tr; }
int nf_scene_tile_dirty(const NfScene *s, int tx, int ty) { return s->dirty[ty * s->tc + tx]; }
int nf_scene_tile_lit(const NfScene *s, int tx, int ty) { return s->lit[ty * s->tc + tx]; }
