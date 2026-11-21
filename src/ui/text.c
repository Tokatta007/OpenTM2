/* Menu text (draw.h), drawn like the game's menu labels: colored letters with a black outline and a soft drop shadow
 * down and to the right.
 *
 * Two sources of letters:
 * - the lettering atlas, fonts/flexure.atlas (tools/font/make_atlas.c): Flexure, the game's own menu face, as
 *   signed-distance pictures plus metrics, used by permission of T26 (never the font file). Set as the game sets its
 *   labels (the user's picks from the 2026-11-06 trial): fitted width and spacing, whole pixels across, capitals a
 *   whole number of pixels tall, steeper edges, and the game's lettering composed from Flexure's own glyphs (':' two
 *   small square dots, 'i' / 'j' with the dot accent, '=' at the capitals' middle);
 * - Averia Serif Libre (SIL OFL), through stb_truetype: the whole menu when the atlas is missing (or TM2PC_FONT=averia),
 *   and any character the atlas does not have. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "draw.h"

static unsigned char *font_data;
static stbtt_fontinfo font;
static int t_ascent, t_descent;

/* the atlas (make_atlas.c's file) */
typedef struct { int cp, adv, lsb, x0, y0, x1, y1, sx, sy, sw, sh, xoff, yoff; } AtGlyph;
static struct {
    int on, count, ascent, descent, w, h;
    float scale, dist;
    AtGlyph g[128];
    short kern[96][96]; /* printable ASCII pairs, font units */
    unsigned char *px;
} at;

static int at_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    int hdr[8];
    int ok = fread(magic, 8, 1, f) == 1 && !memcmp(magic, "TM2GLYF1", 8) && fread(hdr, 4, 8, f) == 8 &&
             fread(&at.scale, 4, 1, f) == 1 && fread(&at.dist, 4, 1, f) == 1;
    int kerns = ok ? hdr[4] : 0;
    if (ok) {
        at.count = hdr[0], at.ascent = hdr[1], at.descent = hdr[2], at.w = hdr[5], at.h = hdr[6];
        ok = at.count > 0 && at.count <= 128 && at.w > 0 && at.h > 0 && at.w * at.h <= 1 << 24 &&
             fread(at.g, sizeof at.g[0], (size_t)at.count, f) == (size_t)at.count;
    }
    memset(at.kern, 0, sizeof at.kern);
    for (int k = 0; ok && k < kerns; k++) {
        int v[3];
        ok = fread(v, 4, 3, f) == 3;
        if (ok && v[0] >= 32 && v[0] < 128 && v[1] >= 32 && v[1] < 128) at.kern[v[0] - 32][v[1] - 32] = (short)v[2];
    }
    if (ok) {
        at.px = malloc((size_t)at.w * at.h);
        ok = at.px && fread(at.px, 1, (size_t)at.w * at.h, f) == (size_t)at.w * at.h;
    }
    for (int i = 0; ok && i < at.count; i++)
        ok = at.g[i].sw >= 0 && at.g[i].sh >= 0 && at.g[i].sx >= 0 && at.g[i].sy >= 0 && at.g[i].sx + at.g[i].sw <= at.w &&
             at.g[i].sy + at.g[i].sh <= at.h;
    fclose(f);
    if (!ok) {
        free(at.px);
        memset(&at, 0, sizeof at);
        fprintf(stderr, "text: %s is not a lettering atlas\n", path);
        return 0;
    }
    at.on = 1;
    return 1;
}

int text_init(const char *ttf_path)
{
    FILE *f = fopen(ttf_path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    font_data = malloc((size_t)n);
    if (!font_data || fread(font_data, 1, (size_t)n, f) != (size_t)n ||
        !stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
        fclose(f);
        free(font_data);
        font_data = NULL;
        return 0;
    }
    fclose(f);
    stbtt_GetFontVMetrics(&font, &t_ascent, &t_descent, NULL);
    /* the lettering atlas beside it (fonts/flexure.atlas), unless TM2PC_FONT=averia */
    const char *want = getenv("TM2PC_FONT");
    if (!at.on && !(want && !strcmp(want, "averia"))) {
        char path[1200];
        const char *slash = strrchr(ttf_path, '/'), *back = strrchr(ttf_path, '\\');
        if (back > slash) slash = back;
        int dir = slash ? (int)(slash - ttf_path) + 1 : 0;
        snprintf(path, sizeof path, "%.*sflexure.atlas", dir, ttf_path);
        at_load(path);
    }
    return 1;
}

int text_ready(void) { return font_data != NULL; }
int text_lettering(void) { return at.on; }

/* ---- glyphs from either source ---- */

typedef struct { int atlas, index; } Glyph; /* atlas: at.g[index]; else the TrueType glyph index */

static const AtGlyph *at_find(int cp)
{
    for (int i = 0; i < at.count; i++)
        if (at.g[i].cp == cp) return &at.g[i];
    return NULL;
}

static Glyph glyph_of(int cp)
{
    Glyph g = {0, 0};
    const AtGlyph *a = at.on ? at_find(cp) : NULL;
    if (a) g.atlas = 1, g.index = (int)(a - at.g);
    else if (font_data) g.index = stbtt_FindGlyphIndex(&font, cp);
    else if (at.on && (a = at_find('?')) != NULL) g.atlas = 1, g.index = (int)(a - at.g);
    return g;
}

/* pixels per font unit of a glyph's source at a line height of size */
static float unit_scale(int atlas, int size)
{
    return (float)size / (float)(atlas ? at.ascent - at.descent : t_ascent - t_descent);
}
static int line_ascent(void) { return at.on ? at.ascent : t_ascent; }
static float scale_for(int size) { return unit_scale(at.on, size); }

static int glyph_advance(Glyph g)
{
    if (g.atlas) return at.g[g.index].adv;
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&font, g.index, &adv, &lsb);
    return adv;
}

static int glyph_kern(int a, int b, Glyph ga, Glyph gb)
{
    if (ga.atlas != gb.atlas) return 0;
    if (ga.atlas) return a >= 32 && a < 128 && b >= 32 && b < 128 ? at.kern[a - 32][b - 32] : 0;
    return stbtt_GetGlyphKernAdvance(&font, ga.index, gb.index);
}

static void glyph_box(Glyph g, int *x0, int *y0, int *x1, int *y1)
{
    if (g.atlas) *x0 = at.g[g.index].x0, *y0 = at.g[g.index].y0, *x1 = at.g[g.index].x1, *y1 = at.g[g.index].y1;
    else if (!stbtt_GetGlyphBox(&font, g.index, x0, y0, x1, y1)) *x0 = *y0 = *x1 = *y1 = 0;
}

static float at_sample(const AtGlyph *a, float u, float v) /* bilinear, outside the picture: far outside */
{
    int iu = (int)(u < 0 ? u - 1 : u), iv = (int)(v < 0 ? v - 1 : v);
    float fu = u - iu, fv = v - iv, s = 0;
    for (int k = 0; k < 4; k++) {
        int x = iu + (k & 1), y = iv + (k >> 1);
        float w = (k & 1 ? fu : 1 - fu) * (k >> 1 ? fv : 1 - fv);
        int d = x >= 0 && y >= 0 && x < a->sw && y < a->sh ? at.px[(size_t)(a->sy + y) * at.w + a->sx + x] : 0;
        s += w * d;
    }
    return s;
}

/* a glyph's coverage at sx x sy pixels per font unit, shifted right by shift (0..1): malloc'd gw x gh, its corner at
 * (gx, gy) from the pen on the baseline */
static unsigned char *glyph_raster(Glyph g, float sx, float sy, float shift, int *gw, int *gh, int *gx, int *gy)
{
    if (!g.atlas) return stbtt_GetGlyphBitmapSubpixel(&font, sx, sy, shift, 0, g.index, gw, gh, gx, gy);
    const AtGlyph *a = &at.g[g.index];
    *gw = *gh = *gx = *gy = 0;
    if (a->sw <= 0 || a->sh <= 0) return NULL;
    float kx = sx / at.scale, ky = sy / at.scale, k = kx < ky ? kx : ky; /* output pixels per atlas pixel */
    int x0 = (int)(a->xoff * kx + shift + (a->xoff < 0 ? -1 : 0)), y0 = (int)(a->yoff * ky + (a->yoff < 0 ? -1 : 0));
    int w = (int)(a->sw * kx) + 3, h = (int)(a->sh * ky) + 3;
    unsigned char *out = malloc((size_t)w * h);
    if (!out) return NULL;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            float u = (x0 + i + 0.5f - shift) / kx - a->xoff - 0.5f, v = (y0 + j + 0.5f) / ky - a->yoff - 0.5f;
            float d = (at_sample(a, u, v) - 128.0f) / at.dist * k + 0.5f; /* signed distance in output pixels */
            out[j * w + i] = (unsigned char)(d <= 0 ? 0 : d >= 1 ? 255 : d * 255 + 0.5f);
        }
    *gw = w, *gh = h, *gx = x0, *gy = y0;
    return out;
}

/* Weight and width on top of the font's own (text_set_style). */
static float style_bold, style_wide = 1.0f, style_track;

void text_set_style(float bold, float wide, float track)
{
    if (at.on && bold > 0) bold = 0.01f, wide = 1.083f, track = 0.064f; /* (Flexure fitted to the game's lettering) */
    style_bold = bold < 0 ? 0 : bold;
    style_wide = wide > 0 ? wide : 1.0f;
    style_track = track;
}

/* Extra pixels each letter is thickened by, to the right and down. */
static int bold_px(int size) { return (int)(size * style_bold + 0.5f); }
/* Extra space after each letter. */
static float track_px(int size) { return size * style_track; }

int text_width(const char *s, int size)
{
    if (!font_data && !at.on)
        return 0;
    float x = 0;
    int e = bold_px(size);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        Glyph g = glyph_of(*p);
        float sx = unit_scale(g.atlas, size) * style_wide;
        x += glyph_advance(g) * sx + e + (p[1] ? track_px(size) : 0);
        if (p[1]) x += glyph_kern(p[0], p[1], g, glyph_of(p[1])) * sx;
        if (at.on) x = (float)(int)(x + 0.5f); /* (whole pixels across, as drawn) */
    }
    return (int)(x + 0.5f);
}

/* max over a (2r+1) square, separable: the outline */
static void grow(const uint8_t *in, uint8_t *out, uint8_t *tmp, int w, int h, int r)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t m = 0;
            for (int d = -r; d <= r; d++)
                if (x + d >= 0 && x + d < w && in[y * w + x + d] > m) m = in[y * w + x + d];
            tmp[y * w + x] = m;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t m = 0;
            for (int d = -r; d <= r; d++)
                if (y + d >= 0 && y + d < h && tmp[(y + d) * w + x] > m) m = tmp[(y + d) * w + x];
            out[y * w + x] = m;
        }
}

static void blend(uint8_t *p, const uint8_t c[3], int a)
{
    for (int k = 0; k < 3; k++)
        p[k] = (uint8_t)((p[k] * (255 - a) + c[k] * a) / 255);
}

/* one character as drawn: its glyphs and their offsets (font units, y up) and sizes; the lettering atlas composes the
 * game's marks from Flexure's own glyphs, nothing in them changed */
typedef struct { Glyph g; float dx, dy, k; } Part;
static int char_parts(int ch, Part *part)
{
    Glyph g = glyph_of(ch);
    if (g.atlas) {
        if (ch == ':' || ch == ';') { /* two small square dots (';': the top one over a comma) */
            part[0] = (Part){glyph_of(ch == ':' ? '.' : ','), ch == ':' ? 45.0f : 66.0f, 0, 0.7f};
            part[1] = (Part){glyph_of('.'), 45, 348, 0.7f};
            return 2;
        }
        part[0] = (Part){g, 0, ch == '=' ? 65.0f : 0, 1};
        if ((ch == 'i' || ch == 'j') && at_find(0x02D9)) { /* Flexure's i / j are dotless: its dot accent set over them */
            part[1] = (Part){glyph_of(0x02D9), ch == 'i' ? 27.0f : 16.0f, 0, 1};
            return 2;
        }
        return 1;
    }
    part[0] = (Part){g, 0, 0, 1};
    return 1;
}

void text_draw(Image *dst, const char *s, int x, int y, int size, TextColor c)
{
    if ((!font_data && !at.on) || !*s)
        return;
    float sc = scale_for(size);
    int e = bold_px(size);                   /* thickening */
    int r = size / 14 > 1 ? size / 14 : 1;  /* outline */
    int sh = r + 1;                          /* shadow offset */
    int pad = r + sh + 1;
    int w = text_width(s, size) + 2 * pad + 2, h = size + 2 * pad + e;
    uint8_t *cov = calloc((size_t)w * h, 3), *line = cov + (size_t)w * h, *tmp = line + (size_t)w * h;
    if (!cov)
        return;
    int base = pad + (int)(line_ascent() * sc + 0.5f);
    /* the lettering: the capitals a whole number of pixels tall */
    float cap_fit = 1;
    if (at.on) {
        const AtGlyph *H = at_find('H');
        if (H && H->y1 > 0) {
            float cap = H->y1 * sc;
            cap_fit = (float)(int)(cap + 0.5f) / cap;
        }
    }
    float pen = (float)pad;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        Part part[2];
        int parts = char_parts(*p, part);
        Glyph g = glyph_of(*p);
        float us = unit_scale(g.atlas, size), sx = us * style_wide, sy = g.atlas ? us * cap_fit : us;
        if (at.on) pen = (float)(int)(pen + 0.5f); /* (whole pixels across: crisp) */
        float shift = pen - (int)pen;
        for (int q = 0; q < parts; q++) {
            int gw, gh, gx, gy;
            int ox = (int)(part[q].dx * sx + 0.5f), oy = (int)(part[q].dy * sy + 0.5f);
            unsigned char *b = glyph_raster(part[q].g, sx * part[q].k, sy * part[q].k, shift, &gw, &gh, &gx, &gy);
            if (!b) continue;
            for (int dy = 0; dy <= e / 2; dy++) /* thickened: the letter stamped across e pixels */
                for (int dx = 0; dx <= e; dx++)
                    for (int j = 0; j < gh; j++)
                        for (int i = 0; i < gw; i++) {
                            int X = (int)pen + ox + gx + i + dx, Y = base - oy + gy + j + dy;
                            if (X >= 0 && X < w && Y >= 0 && Y < h && b[j * gw + i] > cov[Y * w + X])
                                cov[Y * w + X] = b[j * gw + i];
                        }
            if (part[q].g.atlas) free(b);
            else stbtt_FreeBitmap(b, NULL);
        }
        pen += glyph_advance(g) * sx + e + track_px(size);
        if (p[1]) pen += glyph_kern(p[0], p[1], g, glyph_of(p[1])) * sx;
    }
    if (at.on) /* the lettering's edges steeper (coverage through a contrast curve) */
        for (int i = 0; i < w * h; i++) {
            float t = cov[i] / 255.0f, v = (t - 0.5f) * 2.2f + 0.5f;
            cov[i] = (uint8_t)(v < 0 ? 0 : v > 1 ? 255 : v * 255 + 0.5f);
        }
    grow(cov, line, tmp, w, h, r);
    static const uint8_t black[3] = {0, 0, 0};
    int top = base - (int)(line_ascent() * sc * 0.72f), bottom = base; /* letter body, for the fill gradient */
    int ox = x - pad, oy = y - pad;
    for (int j = 0; j < h + sh; j++) {
        int Y = oy + j;
        if (Y < 0 || Y >= dst->h) continue;
        float t = bottom > top ? (float)(j - top) / (float)(bottom - top) : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        uint8_t fill[3];
        for (int k = 0; k < 3; k++) fill[k] = (uint8_t)(c.top[k] + (c.bottom[k] - c.top[k]) * t);
        for (int i = 0; i < w + sh; i++) {
            int X = ox + i;
            if (X < 0 || X >= dst->w) continue;
            uint8_t *px = dst->px + ((size_t)Y * dst->w + X) * 3;
            int si = i - sh, sj = j - sh;
            if (si >= 0 && sj >= 0 && si < w && sj < h && line[sj * w + si])
                blend(px, black, line[sj * w + si] * 150 / 255);
            if (i < w && j < h) {
                if (line[j * w + i]) blend(px, black, line[j * w + i]);
                if (cov[j * w + i]) blend(px, fill, cov[j * w + i]);
            }
        }
    }
    free(cov);
}

void text_caps(int size, int *top, int *height)
{
    int x0, y0, x1, y1;
    *top = *height = 0;
    if (!font_data && !at.on)
        return;
    float sc = scale_for(size);
    glyph_box(glyph_of('H'), &x0, &y0, &x1, &y1);
    float sy = sc;
    if (at.on && y1 > 0) sy = (float)(int)(y1 * sc + 0.5f) / (float)y1; /* (as drawn: whole pixels tall) */
    int iy0 = (int)floorf(-y1 * sy), iy1 = (int)ceilf(-y0 * sy);
    *top = (int)(line_ascent() * sc + 0.5f) + iy0;
    *height = iy1 - iy0;
}
