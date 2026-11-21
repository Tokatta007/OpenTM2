/* make_atlas FONT OUT: the menu lettering's glyph atlas (src/ui/text.c reads it) from a font installed on this PC.
 *
 * For a font whose license allows its letters only as pictures (Flexure, T26's game extension): each glyph a
 * signed-distance picture (from an 8x rendering) plus the metrics to set it (advances, bearings, boxes, kerning). The font file itself is
 * never read by the game nor shipped. The set is small: printable ASCII and the dot accent (U+02D9, set over the
 * dotless i / j by the renderer). Build: cmake --build <dir> --target font_atlas.
 *
 * File (little endian): "TM2GLYF1", then int32 count, ascent, descent, line_gap, kern_count, atlas_w, atlas_h, pad,
 * float scale (font units to atlas pixels), float dist_scale (atlas value per pixel of distance; 128 on the edge);
 * count glyphs { int32 codepoint, advance, lsb, x0, y0, x1, y1 (font units), sx, sy, sw, sh (atlas rect), xoff,
 * yoff (the picture's corner from the pen, atlas pixels, y down) }; kern_count { int32 a, b, value (font units) };
 * then atlas_w * atlas_h bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

enum { EM_PX = 72, PAD = 8, ATLAS_W = 1024 };

static void w32(FILE *f, int v) { fwrite(&v, 4, 1, f); }

/* Felzenszwalb's 1D squared distance transform of f (n values, stride st), in place; v, z, d scratch */
static void edt1(float *f, int n, int st, int *v, float *z, float *d)
{
    int k = 0;
    v[0] = 0, z[0] = -1e20f, z[1] = 1e20f;
    for (int q = 1; q < n; q++) {
        float s;
        for (;;) {
            int p = v[k];
            s = ((f[q * st] + (float)q * q) - (f[p * st] + (float)p * p)) / (2.0f * q - 2.0f * p);
            if (s <= z[k] && k > 0) k--;
            else break;
        }
        if (s <= z[k]) { v[0] = q, z[0] = -1e20f, z[1] = 1e20f, k = 0; continue; }
        k++, v[k] = q, z[k] = s, z[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        d[q] = (float)(q - v[k]) * (q - v[k]) + f[v[k] * st];
    }
    for (int q = 0; q < n; q++) f[q * st] = d[q];
}

/* squared distance from each pixel to the nearest pixel where in[] == want (w x h) */
static float *edt(const unsigned char *in, int w, int h, int want)
{
    int m = w > h ? w : h;
    float *f = malloc(sizeof(float) * (size_t)w * h), *z = malloc(sizeof(float) * (m + 1)), *d = malloc(sizeof(float) * m);
    int *v = malloc(sizeof(int) * m);
    for (int i = 0; i < w * h; i++) f[i] = (in[i] != 0) == want ? 0 : 1e20f;
    for (int x = 0; x < w; x++) edt1(f + x, h, w, v, z, d);
    for (int y = 0; y < h; y++) edt1(f + (size_t)y * w, w, 1, v, z, d);
    free(z), free(d), free(v);
    return f;
}

/* glyph gi's distance picture at `scale` (atlas pixels per font unit), PAD pixels round it: made from a rendering HI
 * times larger (stb_truetype's own SDF mishandles this font's cubic curves); NULL for an empty glyph */
enum { HI = 8 };
static unsigned char *glyph_sdf(stbtt_fontinfo *font, int gi, float scale, float dist, int *sw, int *sh, int *xoff, int *yoff)
{
    int bw, bh, bx, by;
    unsigned char *b = stbtt_GetGlyphBitmap(font, scale * HI, scale * HI, gi, &bw, &bh, &bx, &by);
    if (!b || bw <= 0 || bh <= 0) { if (b) stbtt_FreeBitmap(b, NULL); return NULL; }
    int ax0 = (bx >= 0 ? bx / HI : -((-bx + HI - 1) / HI)) - PAD, ay0 = (by >= 0 ? by / HI : -((-by + HI - 1) / HI)) - PAD;
    int ax1 = (bx + bw + HI - 1 >= 0 ? (bx + bw + HI - 1) / HI : -((-(bx + bw)) / HI)) + PAD;
    int ay1 = (by + bh + HI - 1 >= 0 ? (by + bh + HI - 1) / HI : -((-(by + bh)) / HI)) + PAD;
    int gw = (ax1 - ax0) * HI, gh = (ay1 - ay0) * HI; /* the high grid over the whole atlas rectangle */
    unsigned char *in = calloc((size_t)gw * gh, 1);
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) in[(size_t)(by - ay0 * HI + y) * gw + (bx - ax0 * HI + x)] = b[y * bw + x] >= 128;
    stbtt_FreeBitmap(b, NULL);
    float *out = edt(in, gw, gh, 1), *ins = edt(in, gw, gh, 0);
    *sw = ax1 - ax0, *sh = ay1 - ay0, *xoff = ax0, *yoff = ay0;
    unsigned char *sdf = malloc((size_t)*sw * *sh);
    for (int v = 0; v < *sh; v++)
        for (int u = 0; u < *sw; u++) {
            size_t i = (size_t)(v * HI + HI / 2) * gw + (u * HI + HI / 2);
            float d = in[i] ? sqrtf(ins[i]) - 0.5f : -(sqrtf(out[i]) - 0.5f); /* high pixels, + inside */
            float val = 128.0f + d / HI * dist;
            sdf[v * *sw + u] = (unsigned char)(val < 0 ? 0 : val > 255 ? 255 : val + 0.5f);
        }
    free(in), free(out), free(ins);
    return sdf;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: make_atlas FONT.otf OUT.atlas\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0))) { fprintf(stderr, "not a font\n"); return 1; }
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    float scale = (float)EM_PX / (float)(ascent - descent), dist = 128.0f / PAD;

    int cps[128], count = 0;
    for (int c = 32; c < 127; c++) cps[count++] = c;
    cps[count++] = 0x02D9;

    struct { int cp, adv, lsb, x0, y0, x1, y1, sx, sy, sw, sh, xoff, yoff; unsigned char *sdf; } g[128];
    int x = 0, y = 0, row = 0;
    for (int i = 0; i < count; i++) {
        int gi = stbtt_FindGlyphIndex(&font, cps[i]);
        memset(&g[i], 0, sizeof g[i]);
        g[i].cp = cps[i];
        if (!gi && cps[i] != ' ') fprintf(stderr, "no glyph for U+%04X\n", cps[i]);
        stbtt_GetGlyphHMetrics(&font, gi, &g[i].adv, &g[i].lsb);
        stbtt_GetGlyphBox(&font, gi, &g[i].x0, &g[i].y0, &g[i].x1, &g[i].y1);
        g[i].sdf = gi ? glyph_sdf(&font, gi, scale, dist, &g[i].sw, &g[i].sh, &g[i].xoff, &g[i].yoff) : NULL;
        if (!g[i].sdf) g[i].sw = g[i].sh = 0;
        if (x + g[i].sw > ATLAS_W) x = 0, y += row, row = 0;
        g[i].sx = x, g[i].sy = y;
        x += g[i].sw + 1;
        if (g[i].sh + 1 > row) row = g[i].sh + 1;
    }
    int atlas_h = y + row;
    unsigned char *atlas = calloc((size_t)ATLAS_W * atlas_h, 1);
    for (int i = 0; i < count; i++)
        for (int r = 0; r < g[i].sh; r++)
            memcpy(atlas + (size_t)(g[i].sy + r) * ATLAS_W + g[i].sx, g[i].sdf + (size_t)r * g[i].sw, (size_t)g[i].sw);

    int kerns = 0;
    for (int a = 0; a < count; a++)
        for (int b = 0; b < count; b++) kerns += stbtt_GetCodepointKernAdvance(&font, cps[a], cps[b]) != 0;

    FILE *o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fwrite("TM2GLYF1", 8, 1, o);
    w32(o, count), w32(o, ascent), w32(o, descent), w32(o, gap), w32(o, kerns), w32(o, ATLAS_W), w32(o, atlas_h), w32(o, PAD);
    fwrite(&scale, 4, 1, o);
    fwrite(&dist, 4, 1, o);
    for (int i = 0; i < count; i++) {
        int v[13] = {g[i].cp, g[i].adv, g[i].lsb, g[i].x0, g[i].y0, g[i].x1, g[i].y1, g[i].sx, g[i].sy, g[i].sw, g[i].sh,
                     g[i].xoff, g[i].yoff};
        fwrite(v, 4, 13, o);
    }
    for (int a = 0; a < count; a++)
        for (int b = 0; b < count; b++) {
            int k = stbtt_GetCodepointKernAdvance(&font, cps[a], cps[b]);
            if (k) w32(o, cps[a]), w32(o, cps[b]), w32(o, k);
        }
    fwrite(atlas, 1, (size_t)ATLAS_W * atlas_h, o);
    fclose(o);
    printf("%d glyphs, %d kerning pairs, atlas %dx%d -> %s\n", count, kerns, ATLAS_W, atlas_h, argv[2]);
    return 0;
}
