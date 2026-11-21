/* TIM, .TPC and PCX decoding (tim.h). */
#include "tim.h"

#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int looks_like_tim(const uint8_t *d, size_t len, size_t off)
{
    if (len < off + 20 || u32(d + off) != 0x10) return 0;
    return (u32(d + off + 4) & ~0x1Bu) == 0; /* (bit 4: set on many of the PC version's) */
}

int tim_parse(const uint8_t *d, size_t len, size_t off, int rgb, Tim *t, size_t *size)
{
    memset(t, 0, sizeof *t);
    if (!looks_like_tim(d, len, off)) return 0;
    uint32_t flags = u32(d + off + 4);
    static const int bpps[4] = {4, 8, 16, 24};
    t->bpp = bpps[flags & 3];
    t->swap_rb = rgb;
    t->clut_x = t->clut_y = -1;
    size_t p = off + 8;
    if (flags & 8) { /* the palettes */
        if (len < p + 12) return 0;
        uint32_t blen = u32(d + p);
        int cw = u16(d + p + 8), ch = u16(d + p + 10);
        if (blen < 12 || p + blen > len || (uint32_t)(cw * ch * 2 + 12) > blen) return 0;
        t->per = t->bpp == 4 ? 16 : t->bpp == 8 ? 256 : cw;
        int n = cw * ch;
        t->npal = t->per ? (n + t->per - 1) / t->per : 1;
        if (!t->npal) t->npal = 1;
        t->pal = calloc((size_t)t->npal * t->per + 1, 2);
        if (!t->pal) return 0;
        for (int i = 0; i < n; i++) t->pal[i] = u16(d + p + 12 + 2 * (size_t)i);
        t->clut_x = u16(d + p + 4);
        t->clut_y = u16(d + p + 6);
        p += blen;
    } else if (t->bpp <= 8) { /* none: the indices as grays (still visible) */
        int n = t->bpp == 4 ? 16 : 256;
        t->npal = 1;
        t->per = n;
        t->pal = malloc((size_t)n * 2);
        if (!t->pal) return 0;
        for (int v = 0; v < n; v++) t->pal[v] = (uint16_t)((v * 31 / (n - 1)) * 0x421 | (v == 0));
    }
    if (len < p + 12) { tim_free(t); return 0; }
    uint32_t blen = u32(d + p);
    int iw = u16(d + p + 8), ih = u16(d + p + 10);
    size_t nbytes = (size_t)iw * ih * 2;
    if (blen < 12 || !nbytes || p + 12 + nbytes > len) { tim_free(t); return 0; }
    t->vram_x = u16(d + p + 4);
    t->vram_y = u16(d + p + 6);
    t->w = t->bpp == 4 ? iw * 4 : t->bpp == 8 ? iw * 2 : t->bpp == 16 ? iw : iw * 2 / 3;
    t->h = ih;
    t->stride = iw * 2;
    t->data = d + p + 12;
    size_t end = p + (blen > 12 + nbytes ? blen : 12 + nbytes);
    *size = end - off;
    return 1;
}

int tim_scan(const uint8_t *d, size_t len, int rgb, Tim *out, int max)
{
    int n = 0;
    for (size_t off = 0; off + 20 <= len && n < max;) {
        size_t size;
        if (looks_like_tim(d, len, off) && tim_parse(d, len, off, rgb, &out[n], &size)) {
            out[n].off = off, out[n].bytes = size;
            n++;
            off += (size + 3) & ~(size_t)3;
        } else {
            off += 4;
        }
    }
    return n;
}

static void color(uint16_t c, int swap, uint8_t *o)
{
    if (!c) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    int r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    o[0] = (uint8_t)(swap ? b : r);
    o[1] = (uint8_t)g;
    o[2] = (uint8_t)(swap ? r : b);
    o[3] = 255;
}

void tim_rgba(const Tim *t, int palette, uint8_t *rgba)
{
    if (palette < 0 || palette >= t->npal) palette = 0;
    const uint16_t *pal = t->pal ? t->pal + (size_t)palette * t->per : NULL;
    for (int y = 0; y < t->h; y++) {
        const uint8_t *row = t->data + (size_t)y * t->stride;
        uint8_t *o = rgba + (size_t)y * t->w * 4;
        for (int x = 0; x < t->w; x++, o += 4) {
            if (t->bpp == 24) {
                o[0] = row[x * 3];
                o[1] = row[x * 3 + 1];
                o[2] = row[x * 3 + 2];
                o[3] = 255;
                continue;
            }
            int v = t->bpp == 4 ? (row[x >> 1] >> ((x & 1) * 4)) & 15 : t->bpp == 8 ? row[x] : -1;
            uint16_t c = v >= 0 ? (pal && v < t->per ? pal[v] : (uint16_t)v) : u16(row + x * 2);
            color(c, t->swap_rb, o);
        }
    }
}

void tim_free(Tim *t)
{
    free(t->pal);
    t->pal = NULL;
}

uint8_t *pcx_rgba(const uint8_t *d, size_t len, int *w, int *h)
{
    if (len < 128 || d[0] != 0x0A || d[2] != 1) return NULL;
    int bits = d[3], planes = d[65];
    int x0 = u16(d + 4), y0 = u16(d + 6), x1 = u16(d + 8), y1 = u16(d + 10), stride = u16(d + 66);
    *w = x1 - x0 + 1;
    *h = y1 - y0 + 1;
    if (bits != 8 || (planes != 1 && planes != 3) || *w <= 0 || *h <= 0 || stride < *w) return NULL;
    size_t need = (size_t)stride * planes * *h, got = 0;
    uint8_t *raw = malloc(need), *out = malloc((size_t)*w * *h * 4);
    if (!raw || !out) { free(raw); free(out); return NULL; }
    for (size_t i = 128; got < need && i < len;) { /* (RLE: 0xC0 | count, then the byte) */
        uint8_t b = d[i++];
        if (b >= 0xC0) {
            uint8_t v = i < len ? d[i++] : 0;
            for (int k = 0; k < (b & 0x3F) && got < need; k++) raw[got++] = v;
        } else {
            raw[got++] = b;
        }
    }
    memset(raw + got, 0, need - got);
    const uint8_t *pal = len > 769 && d[len - 769] == 0x0C ? d + len - 768 : NULL;
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            uint8_t *o = out + ((size_t)y * *w + x) * 4;
            if (planes == 3) {
                const uint8_t *row = raw + (size_t)y * stride * 3;
                o[0] = row[x];
                o[1] = row[stride + x];
                o[2] = row[2 * stride + x];
            } else {
                int v = raw[(size_t)y * stride + x];
                o[0] = pal ? pal[v * 3] : (uint8_t)v;
                o[1] = pal ? pal[v * 3 + 1] : (uint8_t)v;
                o[2] = pal ? pal[v * 3 + 2] : (uint8_t)v;
            }
            o[3] = 255;
        }
    free(raw);
    return out;
}
