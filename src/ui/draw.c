/* Pictures for the menu pages (draw.h). */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"

Image image_load_bmp(const char *path)
{
    Image im = {0, 0, NULL};
    size_t n = strlen(path);
    SDL_Surface *s = n > 4 && !SDL_strcasecmp(path + n - 4, ".png") ? SDL_LoadPNG(path) : SDL_LoadBMP(path);
    if (!s)
        return im;
    SDL_Surface *rgb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24);
    SDL_DestroySurface(s);
    if (!rgb)
        return im;
    im.px = malloc((size_t)rgb->w * rgb->h * 3);
    if (im.px) {
        im.w = rgb->w;
        im.h = rgb->h;
        for (int y = 0; y < rgb->h; y++)
            memcpy(im.px + (size_t)y * im.w * 3, (uint8_t *)rgb->pixels + (size_t)y * rgb->pitch, (size_t)im.w * 3);
    }
    SDL_DestroySurface(rgb);
    return im;
}

int image_resize(Image *im, int w, int h)
{
    if (im->px && im->w == w && im->h == h)
        return 1;
    uint8_t *px = realloc(im->px, (size_t)w * h * 3);
    if (!px)
        return 0;
    im->px = px;
    im->w = w;
    im->h = h;
    return 1;
}

void image_free(Image *im)
{
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

void draw_image(Image *dst, const Image *src, float sx, float sy, float sw, float sh,
                int dx, int dy, int dw, int dh, int mirror)
{
    if (!src->px || dw <= 0 || dh <= 0)
        return;
    int x0 = dx < 0 ? 0 : dx, x1 = dx + dw > dst->w ? dst->w : dx + dw;
    int y0 = dy < 0 ? 0 : dy, y1 = dy + dh > dst->h ? dst->h : dy + dh;
    for (int y = y0; y < y1; y++) {
        float fy = sy + ((y - dy) + 0.5f) * sh / dh - 0.5f;
        if (fy < 0) fy = 0;
        if (fy > src->h - 1) fy = (float)(src->h - 1);
        int iy = (int)fy, iy1 = iy + 1 < src->h ? iy + 1 : iy;
        float ty = fy - iy;
        uint8_t *out = dst->px + ((size_t)y * dst->w + x0) * 3;
        for (int x = x0; x < x1; x++, out += 3) {
            float u = ((x - dx) + 0.5f) * sw / dw;
            float fx = (mirror ? sx + sw - u : sx + u) - 0.5f;
            if (fx < 0) fx = 0;
            if (fx > src->w - 1) fx = (float)(src->w - 1);
            int ix = (int)fx, ix1 = ix + 1 < src->w ? ix + 1 : ix;
            float tx = fx - ix;
            const uint8_t *a = src->px + ((size_t)iy * src->w + ix) * 3, *b = src->px + ((size_t)iy * src->w + ix1) * 3;
            const uint8_t *c = src->px + ((size_t)iy1 * src->w + ix) * 3, *d = src->px + ((size_t)iy1 * src->w + ix1) * 3;
            for (int k = 0; k < 3; k++) {
                float top = a[k] + (b[k] - a[k]) * tx, bottom = c[k] + (d[k] - c[k]) * tx;
                out[k] = (uint8_t)(top + (bottom - top) * ty + 0.5f);
            }
        }
    }
}

void draw_image_keyed(Image *dst, const Image *src, float sx, float sy, float sw, float sh,
                      int dx, int dy, int dw, int dh, const uint8_t key[3])
{
    if (!src->px || dw <= 0 || dh <= 0)
        return;
    int x0 = dx < 0 ? 0 : dx, x1 = dx + dw > dst->w ? dst->w : dx + dw;
    int y0 = dy < 0 ? 0 : dy, y1 = dy + dh > dst->h ? dst->h : dy + dh;
    for (int y = y0; y < y1; y++) {
        float fy = sy + ((y - dy) + 0.5f) * sh / dh - 0.5f;
        int iy = (int)floorf(fy);
        float ty = fy - iy;
        uint8_t *out = dst->px + ((size_t)y * dst->w + x0) * 3;
        for (int x = x0; x < x1; x++, out += 3) {
            float fx = sx + ((x - dx) + 0.5f) * sw / dw - 0.5f;
            int ix = (int)floorf(fx);
            float tx = fx - ix, acc[3] = {0, 0, 0}, alpha = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int px = ix + i, py = iy + j;
                    /* stay inside this sprite's own frame (sx..sx+sw) */
                    if (px < (int)sx || px >= (int)(sx + sw) || py < (int)sy || py >= (int)(sy + sh)) continue;
                    const uint8_t *p = src->px + ((size_t)py * src->w + px) * 3;
                    if (p[0] == key[0] && p[1] == key[1] && p[2] == key[2]) continue;
                    float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                    for (int k = 0; k < 3; k++) acc[k] += p[k] * wgt;
                    alpha += wgt;
                }
            if (alpha <= 0.01f)
                continue;
            for (int k = 0; k < 3; k++) /* premultiplied: the color is acc / alpha, covering alpha */
                out[k] = (uint8_t)(out[k] * (1 - alpha) + acc[k] + 0.5f);
        }
    }
}

void draw_fill(Image *dst, int x, int y, int w, int h, const uint8_t rgb[3], int alpha)
{
    int x0 = x < 0 ? 0 : x, x1 = x + w > dst->w ? dst->w : x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h > dst->h ? dst->h : y + h;
    for (int j = y0; j < y1; j++) {
        uint8_t *p = dst->px + ((size_t)j * dst->w + x0) * 3;
        for (int i = x0; i < x1; i++, p += 3)
            for (int k = 0; k < 3; k++)
                p[k] = (uint8_t)((p[k] * (255 - alpha) + rgb[k] * alpha) / 255);
    }
}

void draw_darken_x(Image *dst, int x, int y, int w, int h, int a0, int a1)
{
    static const uint8_t black[3] = {0, 0, 0};
    for (int i = 0; i < w; i++)
        draw_fill(dst, x + i, y, 1, h, black, a0 + (a1 - a0) * i / (w > 1 ? w - 1 : 1));
}

void draw_blurred(Image *dst, const Image *src, int x, int y, int w, int h)
{
    enum { SW = 16, SH = 12 };
    if (!src->px)
        return;
    uint8_t small[SW * SH * 3];
    for (int j = 0; j < SH; j++)
        for (int i = 0; i < SW; i++) {
            int sx0 = i * src->w / SW, sx1 = (i + 1) * src->w / SW;
            int sy0 = j * src->h / SH, sy1 = (j + 1) * src->h / SH;
            unsigned sum[3] = {0, 0, 0}, n = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++, n++)
                    for (int k = 0; k < 3; k++) sum[k] += src->px[((size_t)yy * src->w + xx) * 3 + k];
            for (int k = 0; k < 3; k++) small[(j * SW + i) * 3 + k] = (uint8_t)(n ? sum[k] / n : 0);
        }
    Image s = {SW, SH, small};
    draw_image(dst, &s, 0, 0, SW, SH, x, y, w, h, 0);
}

int image_save_bmp(const Image *im, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f || !im->px) {
        if (f) fclose(f);
        return 0;
    }
    int stride = (im->w * 3 + 3) & ~3;
    uint32_t size = 54u + (uint32_t)stride * (uint32_t)im->h;
    uint8_t hdr[54] = {'B', 'M'};
    for (int i = 0; i < 4; i++) {
        hdr[2 + i] = (uint8_t)(size >> (8 * i));
        hdr[18 + i] = (uint8_t)((uint32_t)im->w >> (8 * i));
        hdr[22 + i] = (uint8_t)((uint32_t)im->h >> (8 * i));
    }
    hdr[10] = 54; hdr[14] = 40; hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(1, (size_t)stride);
    for (int y = im->h - 1; row && y >= 0; y--) {
        const uint8_t *p = im->px + (size_t)y * im->w * 3;
        for (int x = 0; x < im->w; x++) {
            row[x * 3] = p[x * 3 + 2];
            row[x * 3 + 1] = p[x * 3 + 1];
            row[x * 3 + 2] = p[x * 3];
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
    return 1;
}

void draw_rotated(Image *dst, const Image *src, float sx, float sy, float sw, float sh, const uint8_t rgb[3],
                  float cx, float cy, float w, float h, float deg)
{
    float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a), hw = w / 2, hh = h / 2;
    float ex = fabsf(hw * c) + fabsf(hh * s), ey = fabsf(hw * s) + fabsf(hh * c);
    int x0 = (int)floorf(cx - ex), x1 = (int)ceilf(cx + ex), y0 = (int)floorf(cy - ey), y1 = (int)ceilf(cy + ey);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    for (int y = y0; y < y1; y++) {
        uint8_t *p = dst->px + ((size_t)y * dst->w + x0) * 3;
        for (int x = x0; x < x1; x++, p += 3) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float u = dx * c + dy * s, v = -dx * s + dy * c; /* (into the rectangle's own frame) */
            if (u < -hw || u >= hw || v < -hh || v >= hh) continue;
            if (!src || !src->px) {
                p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2];
                continue;
            }
            float fx = sx + (u + hw) / w * sw - 0.5f, fy = sy + (v + hh) / h * sh - 0.5f;
            if (fx < sx) fx = sx;
            if (fy < sy) fy = sy;
            if (fx > sx + sw - 1) fx = sx + sw - 1;
            if (fy > sy + sh - 1) fy = sy + sh - 1;
            int ix = (int)fx, iy = (int)fy, jx = ix + 1 < src->w ? ix + 1 : ix, jy = iy + 1 < src->h ? iy + 1 : iy;
            float tx = fx - ix, ty = fy - iy;
            const uint8_t *a00 = src->px + ((size_t)iy * src->w + ix) * 3, *a10 = src->px + ((size_t)iy * src->w + jx) * 3;
            const uint8_t *a01 = src->px + ((size_t)jy * src->w + ix) * 3, *a11 = src->px + ((size_t)jy * src->w + jx) * 3;
            for (int k = 0; k < 3; k++) {
                float top = a00[k] + (a10[k] - a00[k]) * tx, bot = a01[k] + (a11[k] - a01[k]) * tx;
                p[k] = (uint8_t)(top + (bot - top) * ty + 0.5f);
            }
        }
    }
}
