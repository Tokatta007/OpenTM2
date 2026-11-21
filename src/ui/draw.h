/* Drawing for the port's own menu pages: RGB888 pictures composed on the
 * CPU at the window's size, then shown by PSY-Z (Psyz_VideoSetDisplayRGB24). */
#pragma once
#include <stdint.h>

typedef struct {
    int w, h;
    uint8_t *px; /* RGB888, row-major; NULL: no picture */
} Image;

typedef struct {
    uint8_t top[3], bottom[3]; /* fill color at the top and bottom of the letters */
} TextColor;

/* ---- pictures (draw.c) ---- */

Image image_load_bmp(const char *path); /* (or a .png) px NULL if it cannot be read */
int image_resize(Image *im, int w, int h); /* contents undefined after */
void image_free(Image *im);
int image_save_bmp(const Image *im, const char *path); /* 24-bit BMP */

/* Scales the part (sx, sy, sw, sh) of src into (dx, dy, dw, dh) of dst,
 * bilinear, clipped to dst; mirror flips it left to right. */
void draw_image(Image *dst, const Image *src, float sx, float sy, float sw, float sh,
                int dx, int dy, int dw, int dh, int mirror);

/* As draw_image, for a sprite: source pixels of the key color are
 * transparent, and the edges blend (the key is left out of the filtering). */
void draw_image_keyed(Image *dst, const Image *src, float sx, float sy, float sw, float sh,
                      int dx, int dy, int dw, int dh, const uint8_t key[3]);

/* A w x h rectangle centered on (cx, cy), turned clockwise by deg: the part (sx, sy, sw, sh) of src scaled
 * into it (bilinear), or with src NULL filled with rgb. Clipped to dst. */
void draw_rotated(Image *dst, const Image *src, float sx, float sy, float sw, float sh, const uint8_t rgb[3],
                  float cx, float cy, float w, float h, float deg);

/* Mixes a color into a rectangle, alpha 0-255. */
void draw_fill(Image *dst, int x, int y, int w, int h, const uint8_t rgb[3], int alpha);

/* Darkens a rectangle with black, alpha from a0 at its left to a1 at its right. */
void draw_darken_x(Image *dst, int x, int y, int w, int h, int a0, int a1);

/* A blurred copy of src at w x h: averaged down to a few pixels, then
 * scaled up smoothly. */
void draw_blurred(Image *dst, const Image *src, int x, int y, int w, int h);

/* ---- text (text.c, stb_truetype) ---- */

int text_init(const char *ttf_path);
/* Weight, width and spacing added to the font for what follows: bold
 * thickens each letter by that fraction of the size (0: the font as it is),
 * wide stretches it horizontally (1: as it is), track adds that fraction of
 * the size between letters. */
void text_set_style(float bold, float wide, float track);
int text_ready(void);
int text_lettering(void); /* 1: the lettering atlas (Flexure) is the menu font, 0: Averia */
int text_width(const char *s, int size);           /* size: line height in pixels */
/* The capital letters' top (from the top of the line) and height, for
 * centering text on a line. */
void text_caps(int size, int *top, int *height);
/* Letters with a black outline and drop shadow, in the game's menu style;
 * y is the top of the line, size its height. */
void text_draw(Image *dst, const char *s, int x, int y, int size, TextColor c);
