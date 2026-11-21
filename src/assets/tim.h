/* PlayStation TIM images (and the 1997 PC version's .TPC packs of them) and PCX screens, decoded to RGBA for the
 * first-time setup (docs/FIRST_RUN_SETUP.md). The same as tools/disc/tim.py and pcx.py, which they replace. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int bpp;               /* 4, 8, 16 or 24 */
    int w, h;              /* in pixels */
    int vram_x, vram_y;    /* its place in VRAM */
    int clut_x, clut_y;    /* its palettes' (-1: none) */
    int npal, per;         /* palettes, colors in each */
    uint16_t *pal;         /* npal * per colors */
    const uint8_t *data;   /* its pixels in the blob (4 / 8 bpp indices, 16 bpp colors, 24 bpp RGB rows) */
    int stride;            /* bytes a row */
    int swap_rb;           /* (.TPC: its 15-bit colors are RGB) */
    size_t off, bytes;     /* (tim_scan's: where it starts in the blob, its size) */
} Tim;

/* the TIM at d[off]: 1 and its size in bytes, or 0 (not one); t->data points into d (keep d) */
int tim_parse(const uint8_t *d, size_t len, size_t off, int rgb, Tim *t, size_t *size);
/* every TIM in a blob (a .TMS / .TPC pack: they follow one another, each at a 4-byte boundary): up to max of them */
int tim_scan(const uint8_t *d, size_t len, int rgb, Tim *out, int max);
void tim_rgba(const Tim *t, int palette, uint8_t *rgba); /* w * h * 4 bytes; color 0 transparent */
void tim_free(Tim *t);

/* a ZSoft PCX (8-bit palette or 24-bit, RLE) as RGBA (malloc'ed), NULL if not one */
uint8_t *pcx_rgba(const uint8_t *d, size_t len, int *w, int *h);
