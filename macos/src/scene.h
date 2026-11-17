/*
 * Shared scene engine for Nightfall's drawn styles (identical copies in linux/src, windows/src
 * and macos/src; the Mac app compiles the C file alongside its Swift code).
 *
 * Each style describes its frame as a list of simple shapes (rectangles, dots, glows, lines,
 * rings, text masks). The engine splits the display into NF_TILE x NF_TILE pixel tiles, hashes
 * the shapes that touch each tile, and re-renders only the tiles whose hash changed into a
 * 32-bit framebuffer (0xFFRRGGBB: BGRA bytes, opaque). The platform then puts just those tiles
 * on screen. A twinkling star or a growing line touches one or two tiles a frame, so the cost
 * follows what changes, not the screen size.
 */
#ifndef NIGHTFALL_SCENE_H
#define NIGHTFALL_SCENE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every style, in a fixed order (Windows stores the index in the registry). The first three
 * are drawn by each platform's own code; NF_FIRST_SCENE and later by this engine. */
enum {
    NF_CLOCK, NF_RAIN, NF_MOSAIC,
    NF_STARS, NF_ANALOG, NF_WORDS, NF_CURVES, NF_MAZE, NF_LIFE,
    NF_RIPPLES, NF_PLASMA, NF_FIREFLIES, NF_STARFIELD,
    NF_STYLE_COUNT
};
#define NF_FIRST_SCENE NF_STARS
#define NF_TILE 64

const char *nf_style_name(int style);    /* "clock", "rain", ... (command line, settings) */
const char *nf_style_title(int style);   /* "Clock", "Digital Rain", ... (menus) */
int nf_style_from_name(const char *name); /* -1 if unknown */

/* Renders text to a malloc'd 8-bit coverage mask, w*h bytes (row stride w), px tall. */
typedef unsigned char *(*nf_text_fn)(const char *utf8, int px, int *w, int *h, void *ctx);

typedef struct NfScene NfScene;

/* pixels: w*h framebuffer to draw into, or NULL to have the scene allocate one */
NfScene *nf_scene_new(int style, int w, int h, uint32_t *pixels, unsigned seed,
                      nf_text_fn text, void *text_ctx);
void nf_scene_free(NfScene *s);

int nf_scene_fps(const NfScene *s);
/* Advance one frame and re-render the tiles that changed. Returns how many. */
int nf_scene_step(NfScene *s);

int nf_scene_width(const NfScene *s);
int nf_scene_height(const NfScene *s);
const uint32_t *nf_scene_pixels(const NfScene *s);   /* row stride = width */
int nf_scene_tile_cols(const NfScene *s);
int nf_scene_tile_rows(const NfScene *s);
int nf_scene_tile_dirty(const NfScene *s, int tx, int ty);   /* changed in the last step */
int nf_scene_tile_lit(const NfScene *s, int tx, int ty);     /* has anything drawn in it */

#ifdef __cplusplus
}
#endif
#endif
