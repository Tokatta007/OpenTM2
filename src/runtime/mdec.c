/* STR video frame decoder: what the PSX does with DecDCTvlc (bitstream to
 * MDEC run-level codes) and the MDEC (dequantize, IDCT, YCbCr to RGB).
 *
 * Frame data (versions 1 and 2, all TM2 uses): an 8-byte header
 * (run-level halfword count / 2, 0x3800, quantizer scale, version), then a
 * bitstream read as little-endian 16-bit words, most significant bit first.
 * Per 16x16 macroblock (in columns, top to bottom) six 8x8 blocks: Cr, Cb,
 * Y1 Y2 Y3 Y4. Per block a 10-bit DC value, then MPEG-1 AC codes (table
 * B.14) up to the end-of-block code "10"; the escape 000001 is followed by a
 * 6-bit run and a 10-bit level. Dequantization follows the MDEC:
 * DC * q[0], AC (level * q[k] * scale + 4) / 8, with libpress's table.
 * The IDCT here is exact (floating point) rather than the MDEC's integer
 * one; the difference is below one step of 8-bit color. */
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "mdec.h"

static const uint8_t zigzag[64] = { /* zigzag position -> raster index */
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static const uint8_t quant[64] = { /* libpress DecDCTReset table, zigzag order */
    2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
    27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
    29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
    35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83 };

/* MPEG-1 dct_coeff_next codes without the sign bit: code, length, run, level */
static const struct { uint16_t code; uint8_t len, run, level; } ac_codes[] = {
    {0x3, 2, 0, 1}, {0x3, 3, 1, 1}, {0x4, 4, 0, 2}, {0x5, 4, 2, 1},
    {0x5, 5, 0, 3}, {0x7, 5, 3, 1}, {0x6, 5, 4, 1},
    {0x6, 6, 1, 2}, {0x7, 6, 5, 1}, {0x5, 6, 6, 1}, {0x4, 6, 7, 1},
    {0x6, 7, 0, 4}, {0x4, 7, 2, 2}, {0x7, 7, 8, 1}, {0x5, 7, 9, 1},
    {0x26, 8, 0, 5}, {0x21, 8, 0, 6}, {0x25, 8, 1, 3}, {0x24, 8, 3, 2},
    {0x27, 8, 10, 1}, {0x23, 8, 11, 1}, {0x22, 8, 12, 1}, {0x20, 8, 13, 1},
    {0x0A, 10, 0, 7}, {0x0C, 10, 1, 4}, {0x0B, 10, 2, 3}, {0x0F, 10, 4, 2},
    {0x09, 10, 5, 2}, {0x0E, 10, 14, 1}, {0x0D, 10, 15, 1}, {0x08, 10, 16, 1},
    {0x1D, 12, 0, 8}, {0x18, 12, 0, 9}, {0x13, 12, 0, 10}, {0x10, 12, 0, 11},
    {0x1B, 12, 1, 5}, {0x14, 12, 2, 4}, {0x1C, 12, 3, 3}, {0x12, 12, 4, 3},
    {0x1E, 12, 6, 2}, {0x15, 12, 7, 2}, {0x11, 12, 8, 2}, {0x1F, 12, 17, 1},
    {0x1A, 12, 18, 1}, {0x19, 12, 19, 1}, {0x17, 12, 20, 1}, {0x16, 12, 21, 1},
    {0x1A, 13, 0, 12}, {0x19, 13, 0, 13}, {0x18, 13, 0, 14}, {0x17, 13, 0, 15},
    {0x16, 13, 1, 6}, {0x15, 13, 1, 7}, {0x14, 13, 2, 5}, {0x13, 13, 3, 4},
    {0x12, 13, 5, 3}, {0x11, 13, 9, 2}, {0x10, 13, 10, 2}, {0x1F, 13, 22, 1},
    {0x1E, 13, 23, 1}, {0x1D, 13, 24, 1}, {0x1C, 13, 25, 1}, {0x1B, 13, 26, 1},
    {0x1F, 14, 0, 16}, {0x1E, 14, 0, 17}, {0x1D, 14, 0, 18}, {0x1C, 14, 0, 19},
    {0x1B, 14, 0, 20}, {0x1A, 14, 0, 21}, {0x19, 14, 0, 22}, {0x18, 14, 0, 23},
    {0x17, 14, 0, 24}, {0x16, 14, 0, 25}, {0x15, 14, 0, 26}, {0x14, 14, 0, 27},
    {0x13, 14, 0, 28}, {0x12, 14, 0, 29}, {0x11, 14, 0, 30}, {0x10, 14, 0, 31},
    {0x18, 15, 0, 32}, {0x17, 15, 0, 33}, {0x16, 15, 0, 34}, {0x15, 15, 0, 35},
    {0x14, 15, 0, 36}, {0x13, 15, 0, 37}, {0x12, 15, 0, 38}, {0x11, 15, 0, 39},
    {0x10, 15, 0, 40}, {0x1F, 15, 1, 8}, {0x1E, 15, 1, 9}, {0x1D, 15, 1, 10},
    {0x1C, 15, 1, 11}, {0x1B, 15, 1, 12}, {0x1A, 15, 1, 13}, {0x19, 15, 1, 14},
    {0x13, 16, 1, 15}, {0x12, 16, 1, 16}, {0x11, 16, 1, 17}, {0x10, 16, 1, 18},
    {0x14, 16, 6, 3}, {0x1A, 16, 11, 2}, {0x19, 16, 12, 2}, {0x18, 16, 13, 2},
    {0x17, 16, 14, 2}, {0x16, 16, 15, 2}, {0x15, 16, 16, 2}, {0x1F, 16, 27, 1},
    {0x1E, 16, 28, 1}, {0x1D, 16, 29, 1}, {0x1C, 16, 30, 1}, {0x1B, 16, 31, 1},
};

/* Lookup by the next 16 bits: (run, level, length incl. sign) or special. */
enum { AC_INVALID = 0, AC_EOB = 0xFE, AC_ESCAPE = 0xFD };
static struct { uint8_t run, level, len; } ac_lut[1 << 16];
static float idct_cos[8][8];
static int tables_ready;

static void init_tables(void)
{
    for (int i = 0; i < (int)(sizeof ac_codes / sizeof ac_codes[0]); i++) {
        int len = ac_codes[i].len, shift = 16 - len;
        for (int rest = 0; rest < (1 << shift); rest++) {
            int idx = (ac_codes[i].code << shift) | rest;
            ac_lut[idx].run = ac_codes[i].run;
            ac_lut[idx].level = ac_codes[i].level;
            ac_lut[idx].len = (uint8_t)(len + 1); /* sign bit */
        }
    }
    for (int rest = 0; rest < (1 << 14); rest++) { /* "10": end of block */
        ac_lut[(0x2 << 14) | rest].run = AC_EOB;
        ac_lut[(0x2 << 14) | rest].len = 2;
    }
    for (int rest = 0; rest < (1 << 10); rest++) { /* "000001": escape */
        ac_lut[(0x1 << 10) | rest].run = AC_ESCAPE;
        ac_lut[(0x1 << 10) | rest].len = 6;
    }
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            idct_cos[x][u] = (float)((u ? 1.0 : M_SQRT1_2) * 0.5 * cos((2 * x + 1) * u * M_PI / 16));
    tables_ready = 1;
}

typedef struct {
    const uint8_t *data;
    size_t size, pos;       /* pos: byte offset of the next 16-bit word */
    uint32_t bits;          /* left-aligned bit buffer */
    int count;              /* valid bits in it */
} Bits;

static void fill(Bits *b)
{
    while (b->count <= 16) {
        uint32_t w = 0;
        if (b->pos + 1 < b->size) w = (uint32_t)(b->data[b->pos] | b->data[b->pos + 1] << 8);
        b->pos += 2;
        b->bits |= w << (16 - b->count);
        b->count += 16;
    }
}

static uint32_t peek(Bits *b, int n) { fill(b); return b->bits >> (32 - n); }
static void skip(Bits *b, int n) { b->bits <<= n; b->count -= n; }
static uint32_t take(Bits *b, int n) { uint32_t v = peek(b, n); skip(b, n); return v; }
static int sign10(uint32_t v) { return (int)(v << 22) >> 22; }

/* One 8x8 block to spatial values (signed, about -128..127). */
static int decode_block(Bits *b, int scale, float out[64])
{
    float coef[64] = {0};
    int k = 0;
    int dc = sign10(take(b, 10));
    coef[0] = (float)(dc * quant[0]);
    for (;;) {
        uint32_t w = peek(b, 16);
        int run, level;
        if (ac_lut[w].run == AC_EOB) { skip(b, 2); break; }
        if (ac_lut[w].run == AC_ESCAPE) {
            skip(b, 6);
            run = (int)take(b, 6);
            level = sign10(take(b, 10));
        } else if (ac_lut[w].len) {
            run = ac_lut[w].run;
            level = ac_lut[w].level;
            skip(b, ac_lut[w].len - 1);
            if (take(b, 1)) level = -level;
        } else {
            return 0; /* invalid code */
        }
        k += run + 1;
        if (k > 63) return 0;
        int v = (level * quant[k] * scale + 4) / 8;
        if (v < -1024) v = -1024;
        if (v > 1023) v = 1023;
        coef[zigzag[k]] = (float)v;
    }
    float tmp[64];
    for (int y = 0; y < 8; y++) /* rows: horizontal frequencies */
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += idct_cos[x][u] * coef[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += idct_cos[y][v] * tmp[v * 8 + x];
            out[y * 8 + x] = s;
        }
    return 1;
}

static uint8_t clamp8(float v)
{
    int i = (int)lrintf(v);
    return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

int mdec_decode_frame(const uint8_t *frame, size_t size, int width, int height, uint8_t *rgb)
{
    if (!tables_ready)
        init_tables();
    if (size < 8)
        return 0;
    int scale = frame[4] | frame[5] << 8, version = frame[6] | frame[7] << 8;
    if (version != 1 && version != 2)
        return 0;
    Bits b = { frame, size, 8, 0, 0 };
    for (int mx = 0; mx < (width + 15) / 16; mx++)
        for (int my = 0; my < (height + 15) / 16; my++) {
            float cr[64], cb[64], y[4][64];
            if (!decode_block(&b, scale, cr) || !decode_block(&b, scale, cb))
                return 0;
            for (int i = 0; i < 4; i++)
                if (!decode_block(&b, scale, y[i]))
                    return 0;
            for (int py = 0; py < 16; py++)
                for (int px = 0; px < 16; px++) {
                    int X = mx * 16 + px, Y = my * 16 + py;
                    if (X >= width || Y >= height) continue;
                    float l = y[(py >> 3) * 2 + (px >> 3)][(py & 7) * 8 + (px & 7)] + 128.0f;
                    float r = cr[(py >> 1) * 8 + (px >> 1)], u = cb[(py >> 1) * 8 + (px >> 1)];
                    uint8_t *o = rgb + ((size_t)Y * width + X) * 3;
                    o[0] = clamp8(l + 1.402f * r);
                    o[1] = clamp8(l - 0.3437f * u - 0.7143f * r);
                    o[2] = clamp8(l + 1.772f * u);
                }
        }
    return 1;
}
