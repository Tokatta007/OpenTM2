/* The game's in-battle fonts, drawn by the port (gamefont.h).
 *
 * As the game's fontPrint (decomp font_print.c, 80113724): a character is
 * mapped (fontMapUsefulCharacter, 80113500: '.' ';', '!' '\\', ',' '^', '\''
 * '`', '*' '{'), codes '0'..'|' are glyphs 0..76 of the font's table (else
 * glyph 47, blank), each glyph is placed with its bottom at the cursor's y
 * plus its y offset, and the cursor moves by its width plus the font's extra
 * spacing (a space: glyph 47's width). The sheet (UAFONT1.TIM) is a gray
 * texture the game tints with the font's color: texel * color / 128. */
#include <stdio.h>
#include <string.h>
#include "gamefont.h"

#define GLYPHS 77
#define FONTS 7

static struct {
    int loaded, spacing, v; /* v: where the font's sheet starts (rows, game pixels) */
    unsigned char g[GLYPHS][4]; /* u v w h */
    int yoff[GLYPHS];
} fonts[FONTS];
static Image sheet;

int gfont_load(const char *folder)
{
    char path[1200], line[128];
    snprintf(path, sizeof path, "%sui/gamefont.bmp", folder);
    sheet = image_load_bmp(path);
    snprintf(path, sizeof path, "%sui/gamefont.txt", folder);
    FILE *f = fopen(path, "r");
    if (!f || !sheet.px) {
        if (f) fclose(f);
        return 0;
    }
    int font = -1, n = 0;
    while (fgets(line, sizeof line, f)) {
        int a, b, c, d, e;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "font %d %d", &a, &b) == 2) {
            font = a >= 0 && a < FONTS ? a : -1;
            c = 0;
            sscanf(line, "font %*d %*d %d", &c);
            if (font >= 0) { fonts[font].spacing = b; fonts[font].v = c; fonts[font].loaded = 1; }
            n = 0;
        } else if (font >= 0 && n < GLYPHS && sscanf(line, "%d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
            fonts[font].g[n][0] = (unsigned char)a; fonts[font].g[n][1] = (unsigned char)b;
            fonts[font].g[n][2] = (unsigned char)c; fonts[font].g[n][3] = (unsigned char)d;
            fonts[font].yoff[n++] = e;
        }
    }
    fclose(f);
    return 1;
}

int gfont_ready(int font) { return sheet.px && font >= 0 && font < FONTS && fonts[font].loaded; }

static int glyph_of(unsigned char c)
{
    switch (c) {
    case '.': c = 0x3B; break;
    case '!': c = 0x5C; break;
    case ',': c = 0x5E; break;
    case '\'': c = 0x60; break;
    case '*': c = 0x7B; break;
    case '(': c = '['; break; /* (the port's: the fonts' parentheses are in '[' and ']', as the game's strings use them) */
    case ')': c = ']'; break;
    }
    return c >= 48 && c < 125 ? c - 48 : 47;
}

int gfont_width(int font, const char *text)
{
    int w = 0;
    if (!gfont_ready(font)) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        w += fonts[font].g[*p == ' ' ? 47 : glyph_of(*p)][2] + fonts[font].spacing;
    return w;
}

/* The sheet's pixels per game pixel (ui/gamefont.bmp is written larger
 * than the game's 256 wide: Scale2x and doubled; the tables stay in the
 * game's pixels). */
static int sheet_scale(void) { return sheet.w >= 256 ? sheet.w / 256 : 1; }

void gfont_draw(Image *dst, int font, const char *text, float x, float y, float scale, const uint8_t color[3])
{
    static const uint8_t key[3] = {255, 0, 255};
    if (!gfont_ready(font)) return;
    int k = sheet_scale();
    float step = (float)k / scale; /* sheet pixels per window pixel */
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int g = *p == ' ' ? 47 : glyph_of(*p);
        const unsigned char *e = fonts[font].g[g];
        if (*p != ' ') {
            /* the glyph's area in the window, sampled from the sheet with
             * soft edges (bilinear; transparent texels left out), tinted */
            float top = y + (float)(fonts[font].yoff[g] - e[3]) * scale;
            int x0 = (int)(x + 0.5f), y0 = (int)(top + 0.5f);
            int w = (int)(e[2] * scale + 0.5f), h = (int)(e[3] * scale + 0.5f);
            int u0 = e[0] * k, v0 = (e[1] + fonts[font].v) * k, uw = e[2] * k, vh = e[3] * k;
            for (int dy = 0; dy < h; dy++) {
                int oy = y0 + dy;
                if (oy < 0 || oy >= dst->h) continue;
                float fy = (dy + 0.5f) * step - 0.5f;
                int iy = (int)(fy < 0 ? -1 : fy);
                float ty = fy - iy;
                for (int dx = 0; dx < w; dx++) {
                    int ox = x0 + dx;
                    if (ox < 0 || ox >= dst->w) continue;
                    float fx = (dx + 0.5f) * step - 0.5f;
                    int ix = (int)(fx < 0 ? -1 : fx);
                    float tx = fx - ix, acc[3] = {0, 0, 0}, alpha = 0;
                    for (int j = 0; j < 2; j++)
                        for (int i = 0; i < 2; i++) {
                            int sx = ix + i, sy = iy + j;
                            if (sx < 0 || sy < 0 || sx >= uw || sy >= vh) continue;
                            const uint8_t *t = sheet.px + ((size_t)(v0 + sy) * sheet.w + (u0 + sx)) * 3;
                            if (t[0] == key[0] && t[1] == key[1] && t[2] == key[2]) continue;
                            float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                            for (int c = 0; c < 3; c++) acc[c] += t[c] * wgt;
                            alpha += wgt;
                        }
                    if (alpha <= 0.01f) continue;
                    uint8_t *o = dst->px + ((size_t)oy * dst->w + ox) * 3;
                    for (int c = 0; c < 3; c++) {
                        float v = acc[c] / alpha * color[c] / 128.0f; /* the game's tint */
                        if (v > 255) v = 255;
                        o[c] = (uint8_t)(o[c] * (1 - alpha) + v * alpha + 0.5f);
                    }
                }
            }
        }
        x += (float)(e[2] + fonts[font].spacing) * scale;
    }
}
