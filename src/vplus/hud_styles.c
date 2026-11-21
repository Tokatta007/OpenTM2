/* HUD styles (Customize HUD > Style): Twisted Metal 2 (the game's parts),
 * TM:Black and TM2012. In the last two the game's health, energy, turbo and
 * weapon parts are left out (hud_layout.c drops their primitives by their
 * place in the player's HUD record) and drawn here instead, from the
 * player's car (health, energy, turbo, the weapons and their counts), with
 * the game's own weapon pictures (the record's big icons) and bars in the
 * game's look. The radar, the target and the messages stay the game's.
 *
 * TM:Black: in the bottom right corner a weapons box (the chosen weapon,
 * the one before and after it small at its sides, its count), the health
 * as an upright bar to its right, under them two bars: energy (blue) and
 * turbo (orange).
 * TM2012: energy (blue) in the bottom left corner; health and under it a
 * smaller turbo bar (orange) in the bottom right; the weapons carried along
 * the bottom middle, see-through, the chosen one in the middle (the row
 * moves as the choice changes), only those with something left.
 *
 * Everything is drawn in the view's coordinates (under its drawing area),
 * scaled with the view and the HUD Size. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "settings.h"
#include "vplus.h"
#include "hud_layout.h"
#include "cars.h" /* HUMAN_REC */
void hudlay_force(int group);
extern int hudstyle_raw;

#define PRIMS 0x80DDF000u      /* our list (4 KB, under the moved arrays at 80DE0000) */
#define PRIMS_END 0x80DDFF00u
#define TEXT_BUF 0x80DDFF00u   /* a number to print */
#define HUD_RECORD 1900u

int hudstyle(void)
{
    static int test = -2; /* tests: TM2PC_TEST_HUD_STYLE=0/1/2, every player's style */
    if (test == -2) test = getenv("TM2PC_TEST_HUD_STYLE") ? atoi(getenv("TM2PC_TEST_HUD_STYLE")) : -1;
    if (test >= 0) return test;
    return settings_video.hud_style == 1 || settings_video.hud_style == 2 ? settings_video.hud_style : 0;
}

/* hud_layout.c: the record parts a style replaces (offsets in a player's HUD record) */
int hudstyle_drops(uint32_t offset)
{
    if (!hudstyle()) return 0;
    return (offset >= 0x024 && offset < 0x06C) || /* health and its back */
           (offset >= 0x0B4 && offset < 0x0FC) || /* energy and its back */
           (offset >= 0x100 && offset < 0x5D0) || /* weapon markers, icons, counts, the list's box */
           (offset >= 0x6B8 && offset < 0x70C);   /* turbo's icon and number (the speedometer stays) */
}

static uint32_t at, prev_node, at_end = PRIMS_END;
static void begin(void) { at = PRIMS; at_end = PRIMS_END; prev_node = 0; }
static uint32_t node(int words) /* a primitive of n words after its tag, linked after the last */
{
    if (at + 4u * (uint32_t)(words + 1) > at_end) return 0;
    uint32_t n = at;
    mem_w32(&M.cpu, n, ((uint32_t)words << 24) | 0xFFFFFF);
    if (prev_node) mem_w32(&M.cpu, prev_node, (mem_r32(&M.cpu, prev_node) & 0xFF000000u) | (n & 0xFFFFFF));
    prev_node = n;
    at += 4u * (uint32_t)(words + 1);
    return n + 4;
}
static float sq = 1; /* widescreen: drawn in the view as seen (wider), x squeezed back into the game's frame */
static int sqx(int x) { return sq == 1 ? x : (int)lrintf(x * sq); }
static uint32_t xy(int x, int y) { return ((uint32_t)(uint16_t)y << 16) | (uint16_t)sqx(x); }
static void mode(int abr, uint32_t tpage) /* draw mode: blending (abr) and texture page */
{
    uint32_t p = node(1);
    if (p) mem_w32(&M.cpu, p, 0xE1000000u | 0x200u | (tpage & 0x1FF & ~0x60u) | ((uint32_t)(abr & 3) << 5));
}
static void rect(int x, int y, int w, int h, int r, int g, int b, int semi)
{
    if (w <= 0 || h <= 0) return;
    uint32_t p = node(3);
    if (!p) return;
    mem_w32(&M.cpu, p, ((0x60u | (semi ? 2u : 0)) << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r);
    int x1 = sqx(x + w);
    mem_w32(&M.cpu, p + 4, xy(x, y));
    mem_w32(&M.cpu, p + 8, ((uint32_t)h << 16) | (uint32_t)(x1 - sqx(x) > 0 ? x1 - sqx(x) : 1));
}
static void bar(int x, int y, int w, int h, int r0, int g0, int b0, int r1, int g1, int b1) /* a gradient, left to right */
{
    if (w <= 0 || h <= 0) return;
    uint32_t p = node(8);
    if (!p) return;
    mem_w32(&M.cpu, p, (0x38u << 24) | ((uint32_t)b0 << 16) | ((uint32_t)g0 << 8) | (uint32_t)r0);
    mem_w32(&M.cpu, p + 4, xy(x, y));
    mem_w32(&M.cpu, p + 8, ((uint32_t)b1 << 16) | ((uint32_t)g1 << 8) | (uint32_t)r1);
    mem_w32(&M.cpu, p + 12, xy(x + w, y));
    mem_w32(&M.cpu, p + 16, ((uint32_t)b0 << 16) | ((uint32_t)g0 << 8) | (uint32_t)r0);
    mem_w32(&M.cpu, p + 20, xy(x, y + h));
    mem_w32(&M.cpu, p + 24, ((uint32_t)b1 << 16) | ((uint32_t)g1 << 8) | (uint32_t)r1);
    mem_w32(&M.cpu, p + 28, xy(x + w, y + h));
}
static void rect_px(int x0, int y, int x1, int h, int r, int g, int b, int semi) /* x already squeezed (widescreen) */
{
    if (x1 <= x0 || h <= 0) return;
    uint32_t p = node(3);
    if (!p) return;
    mem_w32(&M.cpu, p, ((0x60u | (semi ? 2u : 0)) << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r);
    mem_w32(&M.cpu, p + 4, ((uint32_t)(uint16_t)y << 16) | (uint16_t)x0);
    mem_w32(&M.cpu, p + 8, ((uint32_t)h << 16) | (uint32_t)(x1 - x0));
}
static void frame(int x, int y, int w, int h, int t) /* the game's dark frame: a back, a thin light edge */
{
    int x0 = sqx(x), x1 = sqx(x + w); /* (the edge t whole pixels on both sides, however squeezed) */
    rect_px(x0 - t, y - t, x1 + t, h + 2 * t, 20, 60, 20, 1);
    rect_px(x0, y, x1, h, 0, 0, 0, 1);
}
/* a sprite of the HUD record, scaled: the big weapon icons (TM2Sprt at +0x240 + slot x 0x14, their draw
 * modes after the ten at + 10 x 0x14 + slot x 12) or the weapon names of the list (+0x100, the same); its
 * texture page from the draw mode; rgb 0x808080 as it is (a name: the HUD's green); see-through or not */
static void sprite(uint32_t hud, uint32_t base, int slot, int cx, int cy, float s, int semi, uint32_t rgb, int left)
{
    uint32_t spr = hud + base + 0x14u * (uint32_t)slot, md = hud + base + 0x14u * 10 + 12u * (uint32_t)slot;
    uint32_t uvc = mem_r32(&M.cpu, spr + 12), wh = mem_r32(&M.cpu, spr + 16);
    int w = (int)(wh & 0xFFFF), h = (int)(wh >> 16);
    if (w <= 0 || h <= 0 || w > 128 || h > 128) return;
    uint32_t e1 = mem_r32(&M.cpu, md + 4);
    if ((e1 >> 24) != 0xE1) e1 = mem_r32(&M.cpu, md + 8);
    mode(0, e1);
    int dw = (int)(w * s + 0.5f), dh = (int)(h * s + 0.5f), x0 = left ? cx : cx - dw / 2, y0 = cy - dh / 2;
    int u = (int)(uvc & 0xFF), v = (int)((uvc >> 8) & 0xFF), u1 = u + w > 255 ? 255 : u + w, v1 = v + h > 255 ? 255 : v + h;
    uint32_t p = node(9);
    if (!p) return;
    mem_w32(&M.cpu, p, ((0x2Cu | (semi ? 2u : 0)) << 24) | (rgb & 0xFFFFFF));
    mem_w32(&M.cpu, p + 4, xy(x0, y0));
    mem_w32(&M.cpu, p + 8, (uvc & 0xFFFF0000u) | ((uint32_t)v << 8) | (uint32_t)u);
    mem_w32(&M.cpu, p + 12, xy(x0 + dw, y0));
    mem_w32(&M.cpu, p + 16, ((e1 & 0x9FFu) << 16) | ((uint32_t)v << 8) | (uint32_t)u1);
    mem_w32(&M.cpu, p + 20, xy(x0, y0 + dh));
    mem_w32(&M.cpu, p + 24, ((uint32_t)v1 << 8) | (uint32_t)u);
    mem_w32(&M.cpu, p + 28, xy(x0 + dw, y0 + dh));
    mem_w32(&M.cpu, p + 32, ((uint32_t)v1 << 8) | (uint32_t)u1);
}
static void sprite_ink(uint32_t hud, uint32_t base, int slot, int *l, int *r);
static void icon(uint32_t hud, int slot, int cx, int cy, float s, int semi) /* centered on its picture (not its sprite's box) */
{
    int l, r;
    sprite_ink(hud, 0x240, slot, &l, &r);
    sprite(hud, 0x240, slot, cx - (int)((l + r) * s / 2 + 0.5f), cy, s, semi, 0x808080u, 1);
}

/* A sprite's lettering, left to right (its columns with a non-clear pixel): the weapon names sit at
 * the left of their images. Read once from VRAM per image (its place, size and palette). */
void gpu_vram_read(int x, int y, int w, int h, uint16_t *p); /* runtime/gpu_psyz.c */
static void sprite_ink(uint32_t hud, uint32_t base, int slot, int *l, int *r)
{
    static struct { uint32_t uvc, wh, e1; int l, r; } cache[32];
    static int next;
    static uint16_t pix[128 * 130], pal[256];
    uint32_t spr = hud + base + 0x14u * (uint32_t)slot, md = hud + base + 0x14u * 10 + 12u * (uint32_t)slot;
    uint32_t uvc = mem_r32(&M.cpu, spr + 12), wh = mem_r32(&M.cpu, spr + 16), e1 = mem_r32(&M.cpu, md + 4);
    if ((e1 >> 24) != 0xE1) e1 = mem_r32(&M.cpu, md + 8);
    int w = (int)(wh & 0xFFFF), h = (int)(wh >> 16);
    *l = 0;
    *r = w;
    if (w <= 0 || h <= 0 || w > 128 || h > 128) return;
    for (int i = 0; i < 32; i++)
        if (cache[i].uvc == uvc && cache[i].wh == wh && cache[i].e1 == e1) { *l = cache[i].l; *r = cache[i].r; return; }
    int tp = (int)(e1 >> 7) & 3, tx = (int)(e1 & 0xF) * 64, ty = (int)((e1 >> 4) & 1) * 256;
    int u = (int)(uvc & 0xFF), v = (int)((uvc >> 8) & 0xFF), clut = (int)(uvc >> 16);
    int per = tp == 0 ? 4 : tp == 1 ? 2 : 1, x0 = tx + u / per, words = (u % per + w + per - 1) / per;
    if (tp < 2) gpu_vram_read((clut & 0x3F) * 16, (clut >> 6) & 0x1FF, tp == 0 ? 16 : 256, 1, pal);
    gpu_vram_read(x0, ty + v, words, h, pix);
    int lo = w, hi = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int t = u % per + x;
            uint16_t word = pix[y * words + t / per];
            uint16_t c = tp == 0 ? pal[(word >> (4 * (t & 3))) & 0xF] : tp == 1 ? pal[(word >> (8 * (t & 1))) & 0xFF] : word;
            if (c) { if (x < lo) lo = x; if (x > hi) hi = x; }
        }
    if (hi >= lo) { *l = lo; *r = hi + 1; }
    cache[next].uvc = uvc; cache[next].wh = wh; cache[next].e1 = e1; cache[next].l = *l; cache[next].r = *r;
    next = (next + 1) % 32;
}
static void weapon_name(uint32_t hud, int slot, int cx, int cy, float s) /* its lettering centred on cx */
{
    int l, r;
    sprite_ink(hud, 0x100, slot, &l, &r);
    sprite(hud, 0x100, slot, cx - (int)((l + r) * s / 2 + 0.5f), cy, s, 0, 0x30E830u, 1);
}
/* A count: the game's font 2 (as at full size), centred on x, in the HUD's green, at the HUD's scale k: its
 * letters as sprites in an order table of our own, drawn through the layout (hud_layout.c) scaled by k about
 * the count's middle first, so it shrinks with the rest of the HUD (more players, the HUD's size). The kill
 * feed's way (fixes.c feed_text); fontPrintXY draws straight to the screen, past the layout. */
#define NUM_OT 0x80DF6A00u      /* (high RAM: an order table of 2, */
#define NUM_SPRITES 0x80DF6A10u /*  then the draw mode and 20 bytes a letter) */
void hudlay_prescale(int x, int y, float k);
void hudlay_unmap(int X, int Y, int *x, int *y);
float hudlay_wk(void);
static void number(int n, int x, int y, float k)
{
    const int font = 2;
    char t[12];
    int len = snprintf(t, sizeof t, "%d", n);
    for (int i = 0; i <= len; i++) mem_w8(&M.cpu, TEXT_BUF + (uint32_t)i, (uint8_t)t[i]);
    uint32_t font_was = vp_font_color_get(font);
    guest_call(GA(0x80114284u), (uint32_t)font, 36, 150, 36); /* fontSetColor */
    int w = (int)guest_call(GA(0x8011355Cu), (uint32_t)font, TEXT_BUF, 0, 0); /* fontStringWidth: centred on x exactly */
    for (uint32_t o = 0; o < 12; o += 4) mem_w32(&M.cpu, NUM_SPRITES + o, mem_r32(&M.cpu, GA(0x80181E28u) + o)); /* font 2's draw mode */
    /* through the layout: in the view's own places (squeezed for widescreen there), scaled by k about its middle */
    hudlay_force(G_VIEW);
    hudstyle_raw = 0;
    hudlay_prescale(x, y + 5, k);
    guest_call(GA(0x80141938u), NUM_OT, 2, 0, 0); /* ClearOTagR */
    guest_call7(GA(0x801139D8u), (uint32_t)font, (uint32_t)(x - w / 2), (uint32_t)y, NUM_SPRITES, NUM_SPRITES + 12, TEXT_BUF, NUM_OT); /* fontSpritePrintXY */
    guest_call(GA(0x80141A30u), NUM_OT + 4, 0, 0, 0); /* DrawOTag */
    hudlay_prescale(0, 0, 1);
    hudstyle_raw = 1;
    hudlay_force(G_C);
    vp_font_color_put(font, font_was); /* (the color it had: vp_font_color_get) */
}
/* the game's health bar colors (UAdashHealthColor by hudHealthLevel: >= 41%, >= 13%, > 0): its two ends */
static void health_rgb(int pct, int c[6])
{
    static const int lv[4][6] = {{255, 0, 0, 255, 0, 0}, {255, 42, 0, 255, 115, 0}, {186, 155, 32, 221, 231, 50},
                                 {0, 112, 10, 49, 148, 41}};
    int l = pct >= 41 ? 3 : pct >= 13 ? 2 : pct > 0;
    for (int i = 0; i < 6; i++) c[i] = lv[l][i];
}
static void vbar(int x, int y, int w, int h, const int c[6]) /* upright: the dark end at the bottom */
{
    if (w <= 0 || h <= 0) return;
    uint32_t p = node(8);
    if (!p) return;
    uint32_t top = ((uint32_t)c[5] << 16) | ((uint32_t)c[4] << 8) | (uint32_t)c[3], bot = ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[0];
    mem_w32(&M.cpu, p, (0x38u << 24) | top);
    mem_w32(&M.cpu, p + 4, xy(x, y));
    mem_w32(&M.cpu, p + 8, top);
    mem_w32(&M.cpu, p + 12, xy(x + w, y));
    mem_w32(&M.cpu, p + 16, bot);
    mem_w32(&M.cpu, p + 20, xy(x, y + h));
    mem_w32(&M.cpu, p + 24, bot);
    mem_w32(&M.cpu, p + 28, xy(x + w, y + h));
}

int hudstyle_raw; /* hud_layout.c: our own list and text, drawn as they are */

/* The kill feed's weapon: a HUD icon (the player's HUD record, by weapon slot), w wide, its left at x
 * and its middle at cy, in the game's HUD coordinates (drawn through the layout, as the feed's text) */
void hudstyle_feed_icon(int player, int slot, int x, int cy, int w)
{
    if (player < 0 || player > 3 || slot < 0 || slot > 9) return;
    uint32_t hud = RB_hudPlayerRecords + HUD_RECORD * (uint32_t)player, spr = hud + 0x240 + 0x14u * (uint32_t)slot;
    int sw = (int)(mem_r32(&M.cpu, spr + 16) & 0xFFFF);
    if (sw <= 0 || sw > 128) return;
    begin();
    sprite(hud, 0x240, slot, x, cy, (float)w / sw, 0, 0x808080u, 1);
    guest_call(GA(0x80141A30u), PRIMS, 0, 0, 0);
}

/* The kill feed's mine (which 0) or freeze shot (1): the sprite in this arena's VRAM (decomp request 81:
 * portWeaponIcon), w wide, opaque (the game blends them; opaque reads best on the HUD). 0: none here (w 0: only that) */
int hudstyle_feed_vram_icon(int which, int x, int cy, int w)
{
    static uint32_t fn = 1;
    if (fn == 1) fn = port_sym("portWeaponIcon");
    const uint32_t out = TEXT_BUF + 0x80; /* (a TM2PortIcon: tpage, clut, u v w h, colour) */
    if (!fn || !guest_call(fn, (uint32_t)which, 0, out, 0)) return 0;
    uint32_t tpage = mem_r16(&M.cpu, out), clut = mem_r16(&M.cpu, out + 2);
    int u = mem_r8(&M.cpu, out + 4), v = mem_r8(&M.cpu, out + 5), iw = mem_r8(&M.cpu, out + 6), ih = mem_r8(&M.cpu, out + 7);
    if (iw <= 0 || ih <= 0) return 0;
    if (w <= 0) return 1;
    int dh = (ih * w + iw / 2) / iw, x0 = x, y0 = cy - dh / 2;
    int u1 = u + iw > 255 ? 255 : u + iw, v1 = v + ih > 255 ? 255 : v + ih;
    begin();
    mode(0, tpage);
    uint32_t p = node(9);
    if (!p) return 0;
    uint32_t rgb = mem_r32(&M.cpu, out + 8) & 0xFFFFFFu; /* (the model's colour: the freeze's blue) */
    if (!rgb) rgb = 0x808080u;
    mem_w32(&M.cpu, p, (0x2Cu << 24) | rgb);
    mem_w32(&M.cpu, p + 4, xy(x0, y0));
    mem_w32(&M.cpu, p + 8, (clut << 16) | ((uint32_t)v << 8) | (uint32_t)u);
    mem_w32(&M.cpu, p + 12, xy(x0 + w, y0));
    mem_w32(&M.cpu, p + 16, ((tpage & 0x9FFu) << 16) | ((uint32_t)v << 8) | (uint32_t)u1);
    mem_w32(&M.cpu, p + 20, xy(x0, y0 + dh));
    mem_w32(&M.cpu, p + 24, ((uint32_t)v1 << 8) | (uint32_t)u);
    mem_w32(&M.cpu, p + 28, xy(x0 + w, y0 + dh));
    mem_w32(&M.cpu, p + 32, ((uint32_t)v1 << 8) | (uint32_t)u1);
    guest_call(GA(0x80141A30u), PRIMS, 0, 0, 0);
    return 1;
}

/* The TM2 style's turbo as a thin orange bar (Turbo shown as: Bar) under the health bar, in the game's
 * HUD coordinates (laid out with the health: hud_layout.c; in split screen the health is moved up to
 * make room). src: the game's layout (0 one view, 1 side by side, 2 one above the other). */
int vp_watch_hud(int player); /* fixes.c: spectating (their own car's numbers left out) */
void hudstyle_tm2_turbo(int player, int src)
{
    if (vp_watch_hud(player)) return;
    if (hudstyle() || !settings_video.hud_turbo_bar || settings_video.hud_no_turbo || player < 0 || player > 3) return;
    int turbo = (int16_t)mem_r16(&M.cpu, VIEW_REC(player) + 0x7FA);
    int pct = turbo < 0 ? 0 : turbo > 1000 ? 100 : turbo / 10;
    int y = src == 0 ? 227 : src == 1 ? 234 : 114;
    hudlay_force(-1); /* (grouped by its place, with the health: the survival banner before it left G_C forced, and the bar
                       * went with the view's middle, right of the health and lower; the user's 2026-10-31) */
    begin();
    mode(0, 0);
    /* as long as the health bar and its back: one view 9..81 (the bar from 10), split 10..82 (from 11) */
    int bx = src == 0 ? 9 : 10, bw = 72, ix = src == 0 ? 10 : 11;
    /* the frame: the health bar's back (the HUD record's POLY_F4 after the health's POLY_G4, +0x48), its color and blending */
    uint32_t back = mem_r32(&M.cpu, RB_hudPlayerRecords + HUD_RECORD * (uint32_t)player + 0x48 + 4);
    rect(bx, y - 1, bw, 5, (int)(back & 0xFF), (int)(back >> 8 & 0xFF), (int)(back >> 16 & 0xFF), (back >> 25) & 1);
    rect(ix, y, 70, 3, 0, 0, 0, 1);
    bar(ix, y, 70 * pct / 100, 3, 160, 60, 0, 255, 150, 20);
    guest_call(GA(0x80141A30u), PRIMS, 0, 0, 0); /* (through the layout) */
}
static void draw_style(int player, int vw, int vh);
/* the box the speedometer stands beside (TM:Black: energy and turbo; TM:2012: health and turbo), per player, in the
 * view's own coordinates: its outer left and bottom edges (hud_layout.c puts the speedometer's right and bottom there) */
int hudstyle_box_ok[4], hudstyle_box_left[4], hudstyle_box_bottom[4];
/* TM:2012's energy bar (bottom left), per player, in the same coordinates: the lives go just above it (hud_layout.c) */
int hudstyle_en_ok[4], hudstyle_en_left[4], hudstyle_en_top[4];
void hudstyle_draw(int player, int vw, int vh)
{
    if (vp_watch_hud(player)) return; /* spectating: the box shows their own wreck's */
    if (!hudstyle() || player < 0 || player > 3) return;
    hudstyle_raw = 1;
    float wk = hudlay_wk();
    sq = 1 / wk;
    draw_style(player, (int)lrintf(vw * wk), vh);
    sq = 1;
    hudstyle_raw = 0;
}
static void draw_style(int player, int vw, int vh)
{
    int style = hudstyle();
    uint32_t rec = VIEW_REC(player), hud = RB_hudPlayerRecords + HUD_RECORD * (uint32_t)player;
    int hp = (int32_t)mem_r32(&M.cpu, rec + 0x1F0), max_hp = (int16_t)mem_r16(&M.cpu, rec + 0x186);
    int energy = (int16_t)mem_r16(&M.cpu, rec + 0x7F8), turbo = (int16_t)mem_r16(&M.cpu, rec + 0x7FA);
    int sel = (int8_t)mem_r8(&M.cpu, rec + 0x7D5), count = (int16_t)mem_r16(&M.cpu, hud + 0xFC);
    if (count < 1 || count > 10) count = 10;
    int ammo[10];
    for (int i = 0; i < 10; i++) ammo[i] = (int16_t)mem_r16(&M.cpu, rec + 0x7D6 + 2u * (uint32_t)i);
    int hp_pct = max_hp > 0 ? hp * 100 / max_hp : 0;
    if (hp_pct < 0) hp_pct = 0;
    if (hp_pct > 100) hp_pct = 100;
    int en_pct = energy < 0 ? 0 : energy > 1000 ? 100 : energy / 10;
    int tu_pct = turbo < 0 ? 0 : turbo > 1000 ? 100 : turbo / 10;
    float user = (settings_video.hud_size >= 50 && settings_video.hud_size <= 150 ? settings_video.hud_size : 100) / 100.0f;
    float k = vw / 320.0f < vh / 240.0f ? vw / 320.0f : vh / 240.0f;
    /* a quarter (3-4 players): smaller than a half view, not only as tall (the user); vw comes widened in widescreen */
    const float QUARTER = vh <= 120 && vw <= (int)(170 * hudlay_wk()) ? 0.71f : 1.0f;
    float s = user * (k >= 1 ? 1 : powf(k, 0.85f) < 0.4f ? 0.4f : powf(k, 0.85f));
    int small = s < 0.8f, font = small ? 3 : 2, hc[6];
    health_rgb(hp_pct, hc);
    /* the weapons carried, in the game's order (its next weapon: the following slot) */
    int have[10], n = 0, at_sel = 0;
    for (int i = 0; i < count; i++) if (ammo[i] > 0) { if (i == sel) at_sel = n; have[n++] = i; }
    begin();
    mode(0, 0);
    if (style == 1) {
        /* TM:Black: an upright weapons box in the bottom right corner (the chosen weapon big, the one
         * before and after it small and higher up, behind it; its count above, its name under), the
         * health upright to its right, under both a box with energy (blue) and turbo (orange) */
        s = (s < 0.7f ? 0.7f : s) * 0.9f * QUARTER; /* (a small view: room for the count's digits, a fixed size; a quarter smaller) */
        int bw = (int)(46 * s), bh = (int)(64 * s), hw = (int)(9 * s) + 1, gap = (int)(3 * s) + 1, lh = (int)(3 * s) + 1;
        int energy_on = !settings_video.hud_no_energy, turbo_on = !settings_video.hud_no_turbo;
        int lw = bw + gap / 2 + 1 + hw, m = (int)(8 * s), bars = energy_on + turbo_on; /* energy, turbo */
        int x0 = vw - m - lw, y0 = vh - m - (bars ? bars * lh + (bars + 1) * gap + gap : 0) - bh;
        int hx = x0 + bw + gap / 2 + 1, in = (int)(1.5f * s); /* the health: near the bay, its bar inside a border */
        frame(x0, y0, bw, bh, 1);
        frame(hx, y0, hw, bh, 1);
        int hh = (bh - 2 * in) * hp_pct / 100;
        vbar(hx + in, y0 + bh - in - hh, hw - 2 * in, hh, hc);
        int ly = y0 + bh + gap;
        if (bars) frame(x0, ly, lw, bars * lh + (bars + 1) * gap, 1);
        hudstyle_box_ok[player & 3] = 1;
        hudstyle_box_left[player & 3] = x0 - gap; /* (the user's: the speedometer as far from the box as the box is from
                                                   * the weapons and health above it, bottoms level) */
        hudstyle_box_bottom[player & 3] = bars ? ly + bars * lh + (bars + 1) * gap : y0 + bh;
        if (energy_on) bar(x0 + gap, ly + gap, (lw - 2 * gap) * en_pct / 100, lh, 20, 60, 160, 60, 140, 255); /* energy: blue */
        if (turbo_on) bar(x0 + gap, ly + gap + energy_on * (gap + lh), (lw - 2 * gap) * tu_pct / 100, lh, 160, 60, 0, 255, 150, 20); /* turbo: orange */
        if (n && ammo[sel] > 0) {
            int prev = n > 1 ? have[(at_sel - 1 + n) % n] : -1, next = n > 2 ? have[(at_sel + 1) % n] : -1;
            int cy = y0 + bh / 2 + (int)(5 * s), ty = y0 + (int)(13 * s);
            /* left to right as the TM2 list top to bottom: the one before on the left, the next on the right */
            if (prev >= 0) icon(hud, prev, x0 + (int)(10 * s), ty, s * 0.42f, 1);
            if (next >= 0) icon(hud, next, x0 + bw - (int)(10 * s), ty, s * 0.42f, 1);
            icon(hud, sel, x0 + bw / 2, cy, s * 1.1f, 0); /* as big as the box allows */
            weapon_name(hud, sel, x0 + bw / 2, y0 + bh - (int)(6 * s), s * (small ? 0.8f : 1.0f));
            guest_call(GA(0x80141A30u), PRIMS, 0, 0, 0);
            /* between the small ones, clear of the top; with the bay's size. number() scales the count about its middle,
             * 5 pixels under the y it is given, so the middle is put where the bay's proportions have it (20.5 of its 64:
             * as one player's at s 0.9). With y0 + 15 s the middle was 5 pixels lower at every size: a quarter's count
             * sat on the weapon (the user's 2026-11-15 4-player take) */
            number(ammo[sel], x0 + bw / 2, y0 + (int)lrintf(20.5f * s) - 5, s);
            return;
        }
    } else {
        /* TM:2012: energy (blue) bottom left, under the lives; health and a thinner turbo bar (orange), as
         * long, bottom right; the weapons along the bottom middle, see-through, as a revolver: the chosen
         * one in the middle, the next ones to its right and the last ones to its left, round */
        s *= QUARTER;
        int bw = (int)(70 * s), bh = (int)(6 * s) + 1, th = (int)(3 * s) + 1, m = (int)(8 * s);
        hudstyle_en_ok[player & 3] = !settings_video.hud_no_energy;
        hudstyle_en_left[player & 3] = m;
        hudstyle_en_top[player & 3] = vh - m - bh;
        if (!settings_video.hud_no_energy) {
            frame(m, vh - m - bh, bw, bh, 1);
            bar(m, vh - m - bh, bw * en_pct / 100, bh, 20, 60, 160, 60, 140, 255);
        }
        int turbo_bar = !settings_video.hud_no_turbo;
        int rx = vw - m - bw, ry = vh - m - bh - (turbo_bar ? th + 2 : 0);
        hudstyle_box_ok[player & 3] = 1;
        hudstyle_box_left[player & 3] = rx - (int)(5 * s) - 2; /* (the speedometer a little apart from the bars) */
        hudstyle_box_bottom[player & 3] = ry + bh + (turbo_bar ? 2 + th : 0) + 1;
        frame(rx, ry, bw, bh, 1);
        bar(rx, ry, bw * hp_pct / 100, bh, hc[0], hc[1], hc[2], hc[3], hc[4], hc[5]); /* (emptying right to left) */
        if (turbo_bar) {
            frame(rx, ry + bh + 2, bw, th, 1);
            bar(rx, ry + bh + 2, bw * tu_pct / 100, th, 160, 60, 0, 255, 150, 20);
        }
        if (n) {
            /* in line with the bars: the chosen weapon's bottom on their bottom edge, its count on its
             * bottom right corner */
            float is = (s < 0.85f ? 0.85f : s) * QUARTER; /* the icons: never smaller than the count beside them */
            int step = (int)(26 * is), cx = vw / 2, cy = vh - m - (int)(9 * is), most = small ? 1 : 3;
            for (int d = -(n - 1) / 2; d <= n / 2; d++) { /* round: each weapon once; the next ones on the right (the TM2 list's order) */
                if (d == 0 || d < -most || d > most) continue;
                icon(hud, have[((at_sel + d) % n + n) % n], cx + d * step, cy, is * 0.55f, 1);
            }
            icon(hud, have[at_sel], cx, cy, is * 0.75f, 1);
            guest_call(GA(0x80141A30u), PRIMS, 0, 0, 0);
            int ny = vh - m + 5 < vh - 2 ? vh - m + 5 : vh - 2;
            number(ammo[have[at_sel]], cx + (int)(12 * is), ny, is); /* on the corner, partly below it */
            return;
        }
    }
    guest_call(GA(0x80141A30u), PRIMS, 0, 0, 0);
}

/* ---- The marks over the view (vplus/world_marks.c): King of the Hill's hill, the cars' markers, the lock-on ----
 * Drawn after the HUD in the view's own coordinates, from the world's places through the view's camera; every
 * corner at its exact place (gte_precise_put: no whole-pixel steps as the camera moves). Sizes follow the
 * distance (as the car looks), within limits, and the view's size; widescreen: widths squeezed as the HUD's.
 * - The hill (King of the Hill): a wireframe box over its circle, white while free, the holder's color (its team's,
 *   else its car's radar color); white again while contested.
 * - Enemy markers (Customize HUD): over each other car a small triangle pointing down, in its radar color (its
 *   team's in a team game), red while your missiles are locked on it; over it a thin health bar (green, yellow,
 *   red, as the HUD's) and its name: players only (their profile names) or every car (computer cars by car).
 * - Lock-on reticle (Customize HUD): TM2012's, four red arcs round the car your missiles would home on. */
#include "world_marks.h"
#define WM_OT 0x80CA0010u      /* (world_marks.c's RAM: an order table of 2, */
#define WM_SPRITES 0x80CA0020u /*  the font's draw mode and 20 bytes a letter, */
#define WM_TEXT 0x80CA0220u    /*  a name, */
#define WM_PRIMS 0x80CA0400u   /*  the marks' primitives, 32 KB) */
#define WM_PRIMS_END 0x80CA8400u
void gte_precise_put(uint32_t ea, uint32_t word, float x, float y, float w); /* runtime/gte.c */
static uint32_t wm_word(float x, float y)
{
    int ix = (int)lrintf(x), iy = (int)lrintf(y);
    ix = ix < -1000 ? -1000 : ix > 1000 ? 1000 : ix;
    iy = iy < -1000 ? -1000 : iy > 1000 ? 1000 : iy;
    return ((uint32_t)(uint16_t)iy << 16) | (uint16_t)ix;
}
static float wm_w[2] = {1, 1}; /* the depths of a line's two ends (the hill's box, in the world); 1: the HUD's marks */
static void wm_vertex(uint32_t at_word, float x, float y, float w)
{
    uint32_t word = wm_word(x, y);
    mem_w32(&M.cpu, at_word, word);
    gte_precise_put(at_word, word, x, y, w);
}
static uint32_t wm_rgb(const int c[3]) { return ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[0]; }
static int wm_far(float x, float y) { return x < -900 || x > 900 || y < -900 || y > 900; }
/* In a view's world (the hill's box): the quads go into its frame's order table at wm_otz, from its packets */
static uint32_t wm_rec; /* the frame record (order table +0, packets +8, their end +0xC), 0: our own list */
static int wm_otz;
static uint32_t wm_world_node(void)
{
    uint32_t cur = mem_r32(&M.cpu, wm_rec + 8), end = mem_r32(&M.cpu, wm_rec + 0xC);
    if (cur + 24 > end) return 0;
    mem_w32(&M.cpu, wm_rec + 8, cur + 24);
    uint32_t slot = mem_r32(&M.cpu, wm_rec) + 4u * (uint32_t)wm_otz, link = mem_r32(&M.cpu, slot);
    mem_w32(&M.cpu, cur, (5u << 24) | (link & 0xFFFFFF)); /* addPrim */
    mem_w32(&M.cpu, slot, (link & 0xFF000000u) | (cur & 0xFFFFFF));
    return cur + 4;
}
static void wm_quad(const float x[4], const float y[4], const int c[3], int semi) /* corners in Z order */
{
    for (int i = 0; i < 4; i++) if (wm_far(x[i], y[i])) return;
    uint32_t p = wm_rec ? wm_world_node() : node(5);
    if (!p) return;
    mem_w32(&M.cpu, p, ((0x28u | (semi ? 2u : 0)) << 24) | wm_rgb(c));
    for (int i = 0; i < 4; i++) wm_vertex(p + 4 + 4u * (uint32_t)i, x[i], y[i], wm_w[i & 1]); /* (a line's: 0, 2 its start) */
}
static void wm_tri(float x0, float y0, float x1, float y1, float x2, float y2, const int c[3])
{
    if (wm_far(x0, y0) || wm_far(x1, y1) || wm_far(x2, y2)) return;
    uint32_t p = node(4);
    if (!p) return;
    mem_w32(&M.cpu, p, (0x20u << 24) | wm_rgb(c));
    wm_vertex(p + 4, x0, y0, 1);
    wm_vertex(p + 8, x1, y1, 1);
    wm_vertex(p + 12, x2, y2, 1);
}
static void wm_line(float x0, float y0, float x1, float y1, float t, const int c[3]) /* t: half its thickness */
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * t * sq, ny = dx / len * t;
    const float x[4] = {x0 + nx, x1 + nx, x0 - nx, x1 - nx}, y[4] = {y0 + ny, y1 + ny, y0 - ny, y1 - ny};
    wm_quad(x, y, c, 0);
}
static void wm_rect(float x, float y, float w, float h, const int c[3], int semi)
{
    const float qx[4] = {x, x + w, x, x + w}, qy[4] = {y, y, y + h, y + h};
    wm_quad(qx, qy, c, semi);
}
/* a name centred on x, its bottom at y: the game's font 2 in the color given, scaled by k (through the layout: the
 * view's places, as the counts) */
static void wm_name(const char *text, float x, float y, float k, const int c[3])
{
    const int font = 2;
    int len = 0;
    for (; text[len] && len < 23; len++) mem_w8(&M.cpu, WM_TEXT + (uint32_t)len, (uint8_t)text[len]);
    mem_w8(&M.cpu, WM_TEXT + (uint32_t)len, 0);
    uint32_t font_was = vp_font_color_get(font);
    guest_call(GA(0x80114284u), (uint32_t)font, (uint32_t)c[0] / 2, (uint32_t)c[1] / 2, (uint32_t)c[2] / 2); /* fontSetColor (128: the font's own) */
    int w = (int)guest_call(GA(0x8011355Cu), (uint32_t)font, WM_TEXT, 0, 0);
    for (uint32_t o = 0; o < 12; o += 4) mem_w32(&M.cpu, WM_SPRITES + o, mem_r32(&M.cpu, GA(0x80181E28u) + o));
    int vx = (int)lrintf(x / sq), vy = (int)lrintf(y); /* (the layout's view places: unsqueezed) */
    hudlay_force(G_VIEW);
    hudstyle_raw = 0;
    hudlay_prescale(vx, vy, k);
    guest_call(GA(0x80141938u), WM_OT, 2, 0, 0);
    guest_call7(GA(0x801139D8u), (uint32_t)font, (uint32_t)(vx - w / 2), (uint32_t)(vy - 9), WM_SPRITES, WM_SPRITES + 12, WM_TEXT, WM_OT);
    guest_call(GA(0x80141A30u), WM_OT + 4, 0, 0, 0);
    hudlay_prescale(0, 0, 1);
    hudstyle_raw = 1;
    vp_font_color_put(font, font_was); /* (the color it had: vp_font_color_get) */
}

static void wm_begin(void) { at = WM_PRIMS; at_end = WM_PRIMS_END; prev_node = 0; }
static void wm_flush(void)
{
    if (at != WM_PRIMS) guest_call(GA(0x80141A30u), WM_PRIMS, 0, 0, 0);
    wm_begin();
    mode(0, 0);
}

/* the hill: a box over its circle (as wide), 600 units high, each edge in pieces (a piece behind the eye left out).
 * In the view's world: each piece sorted at its depth among the arena and the cars (the emitter's order-table
 * index: depth >> 3, compressed past 1024 with the far draw distance, as portFarDistanceFxOtz), so whatever is
 * nearer (a car, a wall) is drawn over it, and the HUD after it. */
static int wm_otz_of(double depth)
{
    static uint32_t far_word = 1;
    if (far_word == 1) far_word = port_sym("portFarDistance");
    int z = (int)(depth / 8);
    if (far_word && mem_r32(&M.cpu, far_word) && z >= 1024)
        z = z < 4096 ? ((z - 1024) >> 2) + 1024 : z < 16384 ? ((z - 4096) >> 3) + 1792 : ((z - 16384) >> 5) + 3328;
    return z < 1 ? 1 : z > 4094 ? 4094 : z;
}
static void wm_hill_one(const WmView *v, float k, int which);
static void wm_hill(const WmView *v, float k) /* every hill */
{
    for (int i = 0; i < 3; i++) wm_hill_one(v, k, i);
}
static void wm_hill_one(const WmView *v, float k, int which)
{
    double hx, hy, hz, half;
    uint8_t rgb[3];
    if (!vp_hill_mark(which, &hx, &hy, &hz, &half, rgb)) return;
    const int c[3] = {rgb[0], rgb[1], rgb[2]};
    const double H = 600;
    double cx[8], cy[8], cz[8];
    for (int i = 0; i < 8; i++) {
        cx[i] = hx + ((i & 1) ? half : -half);
        cy[i] = hy + ((i & 2) ? half : -half);
        cz[i] = hz + WM_UP * ((i & 4) ? H : 12); /* (the bottom a little off the ground) */
    }
    static const int edge[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    float t = 0.7f * k;
    if (t < 0.5f) t = 0.5f;
    for (int e = 0; e < 12; e++) {
        const int a = edge[e][0], b = edge[e][1], pieces = 12;
        double px = 0, py = 0, pd = 0, d;
        int prev = 0;
        for (int s = 0; s <= pieces; s++) {
            double f = (double)s / pieces, sx = 0, sy = 0;
            int in = wm_project(v, cx[a] + (cx[b] - cx[a]) * f, cy[a] + (cy[b] - cy[a]) * f, cz[a] + (cz[b] - cz[a]) * f, &sx, &sy, &d);
            int z = wm_otz_of((pd + d) / 2); /* (a little nearer, more far away: over the ground's big polygons, sorted at their
                                                * middles; up close as it is, under the cars on it) */
            wm_otz = z - (a < 4 && b < 4 ? z / 6 : z / 16);
            if (in && prev) { /* the edge, then a dark rim under it (an order table slot draws its newest first): seen
                               * on snow and sand alike. With the depth buffer at its own depth (it was given the eye's:
                               * drawn over a car in front of it, the user's Axel 2026-10-29) */
                const int rim[3] = {20, 20, 20};
                wm_w[0] = (float)(pd > 1 ? pd : 1), wm_w[1] = (float)(d > 1 ? d : 1);
                wm_line((float)px, (float)py, (float)sx, (float)sy, t, c);
                /* the rim a little behind its edge: at the same depth the depth test let it win every other piece, the
                 * edges dashed black and changing as the eye moved (the user's New York hill, 2026-11-17) */
                wm_w[0] *= 1.004f, wm_w[1] *= 1.004f;
                wm_line((float)px, (float)py, (float)sx, (float)sy, t + 0.6f, rim);
                wm_w[0] = wm_w[1] = 1;
            }
            prev = in, px = sx, py = sy, pd = d;
        }
    }
}

/* The Versus Tournament's podium (vplus/podium.c): three solid blocks in the world, 1st in the middle and highest,
 * 2nd on its left, 3rd on its right as seen from the front, each with its number on the front face. Each face cut in
 * pieces, each piece sorted at its depth in the view's order table and drawn with its corners' depths (the depth
 * buffer: a car in front of a block covers it, a block in front of a car covers it). Faces turned away are left out. */
static void wm_world_quad(const WmView *v, const double P[4][3], const int c[3], double bias)
{
    float x[4], y[4], w[4];
    double dsum = 0;
    for (int i = 0; i < 4; i++) {
        double sx, sy, d;
        if (!wm_project(v, P[i][0], P[i][1], P[i][2], &sx, &sy, &d)) return;
        x[i] = (float)sx, y[i] = (float)sy, w[i] = (float)d, dsum += d;
        if (wm_far(x[i], y[i])) return;
    }
    int z = wm_otz_of(dsum / 4 - bias);
    wm_otz = z;
    uint32_t q = wm_world_node();
    if (!q) return;
    mem_w32(&M.cpu, q, (0x28u << 24) | wm_rgb(c));
    for (int i = 0; i < 4; i++) wm_vertex(q + 4 + 4u * (uint32_t)i, x[i], y[i], w[i]);
}
/* a face: corners a (origin), along u (n pieces) and along t (m pieces); P = a + u * i / n + t * j / m */
static void wm_world_face(const WmView *v, const double a[3], const double u[3], const double t[3], int n, int m, const int c[3], double bias)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++) {
            double P[4][3];
            for (int k = 0; k < 4; k++) {
                double fi = (double)(i + (k & 1)) / n, fj = (double)(j + (k >> 1)) / m;
                for (int e = 0; e < 3; e++) P[k][e] = a[e] + u[e] * fi + t[e] * fj;
            }
            wm_world_quad(v, P, c, bias);
        }
}
static int podium_facing(const WmView *v, const double ctr[3], const double nrm[3]) /* the face's front toward the eye */
{
    double d = 0;
    for (int e = 0; e < 3; e++) d += nrm[e] * (-v->eye[e] - ctr[e]);
    return d > 0;
}
/* The numbers' shapes: from one of the game's fonts (ui/gamefont.c: the in-battle fonts, their own pixels) or the
 * menus' font (ui/text.c), drawn once into a picture and read back as cells (lit: the letter itself, not its outline
 * or shadow). podium_font: 0-6 a game font, 7 the menus' font, -1 the built-in 5x7 blocks. */
#include "ui/gamefont.h"
#include "podium.h"
enum { PG_MAX = 40 };
static struct { int w, h; unsigned char on[PG_MAX][PG_MAX]; } pg[3];
static int pg_font = -2;
static int podium_font(void)
{
    static int f = -3;
    if (f == -3) f = getenv("TM2PC_TEST_PODIUM_FONT") ? atoi(getenv("TM2PC_TEST_PODIUM_FONT")) : 7; /* (the menus' font, as the standings' 1ST, 2ND: the user's 2026-11-07) */
    return f;
}
static int pg_build(int font)
{
    Image im = {0, 0, NULL};
    if (font < 0 || font > 7 || !image_resize(&im, 160, 160)) return 0;
    static const uint8_t white[3] = {255, 255, 255};
    int ok = 1;
    for (int d = 0; d < 3 && ok; d++) {
        memset(im.px, 0, (size_t)im.w * im.h * 3);
        char str[2] = {(char)('1' + d), 0};
        int down = 1; /* (the menus' font: drawn large, read at half) */
        if (font < 7) {
            if (!gfont_ready(font)) { ok = 0; break; }
            gfont_draw(&im, font, str, 8, 120, 2, white);
            down = 2; /* (drawn at 2x: read back at the font's own pixels) */
        } else {
            if (!text_ready()) { ok = 0; break; }
            const TextColor wc = {{255, 255, 255}, {255, 255, 255}};
            text_set_style(0.02f, 1.12f, 0.045f); /* (the standings' lettering) */
            text_draw(&im, str, 8, 8, 64, wc);
            text_set_style(0, 1, 0);
            down = 2;
        }
        int x0 = im.w, y0 = im.h, x1 = -1, y1 = -1;
        for (int y = 0; y < im.h; y++)
            for (int x = 0; x < im.w; x++) {
                const uint8_t *q = im.px + ((size_t)y * im.w + x) * 3;
                if (q[0] + q[1] + q[2] > 3 * 150) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
        if (x1 < 0) { ok = 0; break; }
        int w = (x1 - x0) / down + 1, h = (y1 - y0) / down + 1;
        if (w > PG_MAX) w = PG_MAX;
        if (h > PG_MAX) h = PG_MAX;
        pg[d].w = w, pg[d].h = h;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int lit = 0, n = 0;
                for (int dy = 0; dy < down; dy++)
                    for (int dx = 0; dx < down; dx++) {
                        int sx = x0 + x * down + dx, sy = y0 + y * down + dy;
                        if (sx > x1 || sy > y1) continue;
                        const uint8_t *q = im.px + ((size_t)sy * im.w + sx) * 3;
                        lit += q[0] + q[1] + q[2] > 3 * 150, n++;
                    }
                pg[d].on[y][x] = n && lit * 2 >= n;
            }
    }
    image_free(&im);
    return ok;
}
void hudstyle_podium(const WmView *v)
{
    int podium_spot(double *x, double *y, double *z, double *yaw); /* vplus/podium.c */
    double px, py, pz, yaw;
    if (!podium_spot(&px, &py, &pz, &yaw)) return;
    double fw, ff, fb;
    if (podium_lineup_on() && podium_lineup_floor(&fw, &ff, &fb)) { /* tools: the line-up's own roof (a spot without one
                                                                    * drawn): the roof's color, each tile a shade apart */
        double n[3] = {sin(yaw), cos(yaw), 0}, r[3] = {-cos(yaw), sin(yaw), 0};
        enum { TX = 96, TY = 40 };
        double tw = 2 * fw / TX, td = (ff + fb) / TY;
        for (int i = 0; i < TX; i++)
            for (int j = 0; j < TY; j++) {
                unsigned h = (unsigned)(i * 73856093u ^ j * 19349663u);
                h = (h ^ (h >> 13)) * 2654435761u;
                int d = (int)(h >> 30) - 1; /* (-1..2) */
                const int c[3] = {34 + d, 33 + d, 36 + d};
                double a[3], u[3], t[3];
                for (int e = 0; e < 3; e++) {
                    a[e] = (e == 0 ? px : e == 1 ? py : pz) - r[e] * fw + r[e] * tw * i - n[e] * fb + n[e] * td * j;
                    u[e] = r[e] * tw, t[e] = n[e] * td;
                }
                wm_world_face(v, a, u, t, 1, 1, c, 0);
            }
    }
    if (podium_lineup_on()) return; /* (tools: the line-up stands on the roof, no blocks) */
    const double W = podium_block_w(), D = podium_block_d(); /* (grown for a big car: vplus/podium.c) */
    const double H[3] = {podium_block_h(0), podium_block_h(1), podium_block_h(2)}; /* 1st, 2nd, 3rd */
    static const double off[3] = {0, -1, 1};          /* along the right (as seen from the front): 2nd left, 3rd right */
    double n[3] = {sin(yaw), cos(yaw), 0}, r[3] = {-cos(yaw), sin(yaw), 0}, up[3] = {0, 0, 1};
    /* light gray, a little under white (the user's: at night the white blocks glared), shaded by face */
    static const int face_rgb[5][3] = {{192, 192, 196}, {168, 168, 174}, {144, 144, 152}, {144, 144, 152}, {124, 124, 132}}; /* top, front, sides, back */
    static const int red[3] = {184, 26, 30};
    static int dumped;
    if (getenv("TM2PC_TEST_PODIUM_FONTS") && !dumped) { /* tests: every font's numbers in the log */
        dumped = 1;
        for (int f = 0; f <= 7; f++) {
            if (!pg_build(f)) { fprintf(stderr, "podium font %d: none\n", f); continue; }
            for (int d = 0; d < 3; d++) {
                fprintf(stderr, "podium font %d digit %d: %dx%d\n", f, d + 1, pg[d].w, pg[d].h);
                for (int y = 0; y < pg[d].h; y++) {
                    char line[PG_MAX + 1];
                    for (int x = 0; x < pg[d].w; x++) line[x] = pg[d].on[y][x] ? '#' : '.';
                    line[pg[d].w] = 0;
                    fprintf(stderr, "  %s\n", line);
                }
            }
        }
        pg_font = -2;
    }
    if (pg_font != podium_font()) pg_font = pg_build(podium_font()) ? podium_font() : -1;
    static const char *const digit[3][7] = {
        {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."},
        {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"},
        {"####.", "....#", "....#", ".###.", "....#", "....#", "####."},
    };
    for (int b = 0; b < 3; b++) {
        double base[3] = {px + r[0] * W * off[b], py + r[1] * W * off[b], pz}, h = H[b];
        /* corners: the front's bottom left (as seen from the front: -r is the viewer's left... the right vector r) */
        double fl[3], u[3], t[3], c[3], nn[3];
        for (int e = 0; e < 3; e++) fl[e] = base[e] + n[e] * D / 2 - r[e] * W / 2; /* front, left, bottom */
        /* front: along r, up */
        for (int e = 0; e < 3; e++) u[e] = r[e] * W, t[e] = up[e] * h, c[e] = base[e] + n[e] * D / 2 + up[e] * h / 2;
        if (podium_facing(v, c, n)) {
            /* The face in pieces, its number over it as a few solid bars (the user's 2026-11-14: the number made of a
             * grid of font cells, up to 40 x 40, crawled like TV static, far and near: every cell edge a place for
             * pixels to come and go). The bars stand a little out from the face (DIGIT_OUT), so the depth test keeps
             * them in front at any angle. Each digit in a 5 x 7 box: x right, y down. */
            static const float bars[3][5][8] = {
                {{2.0f, 0, 3.2f, 0, 3.2f, 7, 2.0f, 7}, {2.0f, 0, 2.0f, 1.4f, 0.7f, 2.3f, 0.7f, 1.0f},
                 {0.8f, 5.9f, 4.4f, 5.9f, 4.4f, 7, 0.8f, 7}},                                             /* 1 */
                {{0.6f, 0, 4.4f, 0, 4.4f, 1.1f, 0.6f, 1.1f}, {3.3f, 0, 4.4f, 0, 4.4f, 3.0f, 3.3f, 3.0f},
                 {3.3f, 2.4f, 4.4f, 3.4f, 1.8f, 6.0f, 0.6f, 6.0f}, {0.6f, 5.9f, 4.4f, 5.9f, 4.4f, 7, 0.6f, 7},
                 {0.6f, 0, 1.7f, 0, 1.7f, 1.9f, 0.6f, 1.9f}},                                             /* 2 */
                {{0.6f, 0, 4.4f, 0, 4.4f, 1.1f, 0.6f, 1.1f}, {1.6f, 2.95f, 4.4f, 2.95f, 4.4f, 4.05f, 1.6f, 4.05f},
                 {0.6f, 5.9f, 4.4f, 5.9f, 4.4f, 7, 0.6f, 7}, {3.3f, 0, 4.4f, 0, 4.4f, 7, 3.3f, 7}},      /* 3 */
            };
            static const int nbars[3] = {3, 5, 4};
            const double DIGIT_OUT = 6.0; /* (2 lost to the face at some angles: parts of a 1 cut away) */
            wm_world_face(v, fl, u, t, 6, 4, face_rgb[1], 0);
            double bh = 0.84 * H[2], bw = bh * 5 / 7, gx0 = (W - bw) / 2, gz1 = h / 2.0 + bh / 2;
            for (int q = 0; q < nbars[b]; q++) {
                double P[4][3];
                for (int k = 0; k < 4; k++) { /* (corners 0, 1, 3, 2: a quad's order) */
                    int kk = k == 2 ? 3 : k == 3 ? 2 : k;
                    double ox = gx0 + bars[b][q][2 * kk] * bw / 5, oz = gz1 - bars[b][q][2 * kk + 1] * bh / 7;
                    for (int e2 = 0; e2 < 3; e2++) P[k][e2] = fl[e2] + r[e2] * ox + up[e2] * oz + n[e2] * DIGIT_OUT;
                }
                wm_world_quad(v, P, red, 400); /* (sorted after every piece of its face: a nearer piece drawn after
                                                * it covered it where their depths tied) */
            }
        }
        /* top: along r, then back */
        double tl[3];
        for (int e = 0; e < 3; e++) tl[e] = fl[e] + up[e] * h, u[e] = r[e] * W, t[e] = -n[e] * D, c[e] = base[e] + up[e] * h;
        if (podium_facing(v, c, up)) wm_world_face(v, tl, u, t, 6, 6, face_rgb[0], 0);
        /* the sides (left: -r, right: +r) and the back (-n). A side against a neighbor only above it (the part inside
         * the neighbor, drawn, bled through the neighbor's front at its edge: the user's 2026-11-08); none against a
         * taller one */
        for (int side = 0; side < 2; side++) {
            double sg = side ? 1 : -1, a[3], sn[3], lo = 0;
            for (int k = 0; k < 3; k++)
                if (off[k] == off[b] + sg) lo = H[k];
            if (lo >= h) continue;
            for (int e = 0; e < 3; e++) {
                sn[e] = r[e] * sg;
                a[e] = base[e] + r[e] * sg * W / 2 + n[e] * D / 2 + up[e] * lo;
                u[e] = -n[e] * D, t[e] = up[e] * (h - lo), c[e] = base[e] + r[e] * sg * W / 2 + up[e] * (h + lo) / 2;
            }
            if (podium_facing(v, c, sn)) wm_world_face(v, a, u, t, 6, lo > 0 ? 2 : 3, face_rgb[2 + side], 0);
        }
        double a[3];
        for (int e = 0; e < 3; e++) {
            nn[e] = -n[e];
            a[e] = base[e] - n[e] * D / 2 - r[e] * W / 2;
            u[e] = r[e] * W, t[e] = up[e] * h, c[e] = base[e] - n[e] * D / 2 + up[e] * h / 2;
        }
        if (podium_facing(v, c, nn)) wm_world_face(v, a, u, t, 6, 3, face_rgb[4], 0);
    }
}

uint64_t vp_gv(void); /* killcam.c: the gameplay clock (the target diamond's pulse) */

/* four.c, after a view's world is built (renderScene; not a mirror's): the hill's box into its order table */
float hudlay_wk(void);
void hudstyle_world_marks(int view, uint32_t rec)
{
    WmView v;
    if (!rec || !wm_view(view, &v)) return;
    float k = (float)(v.cx * 2 / 320.0 < v.cy * 2 / 240.0 ? v.cx * 2 / 320.0 : v.cy * 2 / 240.0);
    sq = 1 / hudlay_wk();
    wm_rec = rec;
    wm_hill(&v, k);
    hudstyle_podium(&v);
    wm_rec = 0;
    sq = 1;
}

/* The marks between steps (Frame rate 60 and Uncapped, vplus/interp.c): a step's frame is shown a step after it is
 * drawn (the PS1's two buffers), the in-between frames at once. Projected from the redraw's own car places, the marks
 * ran a step ahead of the step frames around them and jumped back at each one (the user's stuttering names, health
 * bars and lock-on circle). So each step frame keeps where it put every mark, and an in-between frame puts it between
 * the step frame on screen (the one before) and the next (the one just drawn), as far as the redraw is between them. */
enum { MARK_KINDS = 3, MARK_SLOTS_MAX = 64 }; /* (kinds: 0 the marker's top, 1 the lock circle's middle, 2 Hunted's) */
typedef struct { double x, y, d; unsigned gen; int in; } MarkPlace;
static MarkPlace mark_at[4][MARK_SLOTS_MAX][MARK_KINDS][2]; /* (by the step's parity) */
static unsigned mark_gen[4];
static int mark_project(const WmView *v, int player, int slot, int kind, double x, double y, double z, double *sx, double *sy,
                        double *d)
{
    double interp_redraw_t(void); /* vplus/interp.c (1 between frames: a step's own) */
    int interp_redrawing(void);
    if (slot < 0 || slot >= MARK_SLOTS_MAX || kind < 0 || kind >= MARK_KINDS) return wm_project(v, x, y, z, sx, sy, d);
    unsigned g = mark_gen[player];
    if (!interp_redrawing()) { /* a step's frame: projected and kept */
        MarkPlace *m = &mark_at[player][slot][kind][g & 1];
        m->in = wm_project(v, x, y, z, &m->x, &m->y, &m->d);
        m->gen = g;
        *sx = m->x, *sy = m->y, *d = m->d;
        return m->in;
    }
    const MarkPlace *b = &mark_at[player][slot][kind][g & 1], *a = &mark_at[player][slot][kind][(g + 1) & 1];
    if (g < 2 || b->gen != g || a->gen != g - 1 || !a->in || !b->in) return wm_project(v, x, y, z, sx, sy, d); /* (new: as it is) */
    double t = interp_redraw_t();
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    *sx = a->x + (b->x - a->x) * t, *sy = a->y + (b->y - a->y) * t, *d = a->d + (b->d - a->d) * t;
    return 1;
}

void hudstyle_marks(int player, int vw, int vh)
{
    if (player < 0 || player > 3) return;
    WmView v;
    if (!wm_view(player, &v)) return;
    int interp_redrawing(void);
    if (!interp_redrawing()) mark_gen[player]++; /* (a step's frame: its marks the newest kept) */
    int markers = settings_video.hud_markers, health = !settings_video.hud_no_marker_health;
    int names = settings_video.hud_marker_names % 3, reticle = settings_video.hud_reticle; /* names: 0 players, 1 all, 2 off */
    float wk = hudlay_wk(), view_k = vw / 320.0f < vh / 240.0f ? vw / 320.0f : vh / 240.0f;
    sq = 1 / wk;
    hudstyle_raw = 1;
    wm_begin();
    mode(0, 0);
    { /* Hunted: a diamond over each target, its team's color, its edge pulsing (the markers' place, a little higher) */
        WmCar cars[24];
        int n = vp_mark_cars(cars, 24);
        for (int i = 0; i < n; i++) {
            WmCar *car = &cars[i];
            if (!car->target || car->human == player) continue;
            wm_drawn_place(player, car);
            double hx = 0, hy = 0, d = 0;
            if (!mark_project(&v, player, car->slot, 2, car->x, car->y, car->z + WM_UP * 30, &hx, &hy, &d) || d > 20000) continue;
            if (hx < -40 || hx > v.cx * 2 + 40 || hy < -40 || hy > v.cy * 2 + 40) continue;
            float s = (float)(v.hx * 9.0 / d);
            s = s < 3.5f ? 3.5f : s > 9.0f ? 9.0f : s;
            s *= view_k < 0.5f ? 0.5f : view_k;
            float cx = (float)hx, cy = (float)hy - (markers ? s * 3.2f + 10 : 4) - s * 1.6f; /* (over the marker and its bar) */
            int phase = (int)(vp_gv() % 40), lit = phase < 20 ? phase : 40 - phase; /* (0..20..0: two-thirds of a second) */
            float o = s * 1.6f + 1.2f, in = s * 1.6f;
            const int dark[3] = {10, 10, 10};
            int col[3];
            for (int j = 0; j < 3; j++) col[j] = car->target_rgb[j] + (255 - car->target_rgb[j]) * lit / 40; /* (brighter, and back) */
            float ox[4] = {cx, cx + o * sq * 0.75f, cx - o * sq * 0.75f, cx}, oy[4] = {cy - o, cy, cy, cy + o};
            float ix[4] = {cx, cx + in * sq * 0.75f, cx - in * sq * 0.75f, cx}, iy[4] = {cy - in, cy, cy, cy + in};
            wm_quad(ox, oy, dark, 0);
            /* a cut stone: four facets lit from the top left (the color's own on the top right), so it reads as metal or
             * a gem, not a flat sign (the Juggernaut's platinum, a team's color) */
            static const int shade[4] = {130, 100, 78, 60}; /* (percent: top left, top right, bottom left, bottom right) */
            for (int f = 0; f < 4; f++) {
                int fc[3];
                for (int j = 0; j < 3; j++) {
                    int v2 = shade[f] > 100 ? col[j] + (255 - col[j]) * (shade[f] - 100) / 50 : col[j] * shade[f] / 100;
                    fc[j] = v2 > 255 ? 255 : v2;
                }
                int top = f < 2, left = f == 0 || f == 2; /* (the facet between the middle, a side point and the top or bottom) */
                wm_tri(cx, cy, left ? ix[2] : ix[1], cy, cx, top ? iy[0] : iy[3], fc);
            }
        }
        wm_flush();
    }
    if (markers || reticle) {
        int vp_teams_for_config(uint8_t human[16], uint8_t ai[16]); /* vplus/fixes.c: a team game's teams (0: none) */
        uint8_t th[16], ta[16];
        int team_game = vp_teams_for_config(th, ta) != 0; /* (it returns 1 for a team game: the red lock triangle off then) */
        WmCar cars[24];
        int n = vp_mark_cars(cars, 24), lock = wm_lock_slot(player);
        for (int i = 0; i < n; i++) wm_drawn_place(player, &cars[i]);
        int watched = -1; /* spectating: the car the camera follows (the nearest to it), as one's own */
        if (vp_watch_hud(player)) {
            double best = 700.0 * 700.0;
            for (int i = 0; i < n; i++) {
                double dx = cars[i].x + v.eye[0], dy = cars[i].y + v.eye[1], dz = cars[i].z + v.eye[2], dd = dx * dx + dy * dy + dz * dz;
                if (dd < best) { best = dd; watched = i; }
            }
        }
        for (int i = 0; i < n; i++) {
            const WmCar *car = &cars[i];
            if (car->human == humans_view_human(player) || i == watched || car->ghost) continue; /* (online: the view's human) */
            double hx = 0, hy = 0, d = 0, bx = 0, by = 0, bd = 0;
            int in = mark_project(&v, player, car->slot, 0, car->x, car->y, car->z + WM_UP * 30, &hx, &hy, &d);
            if (!in || d > (vp_draw_distance() == 2 ? 33000 : vp_draw_distance() ? 17000 : 9000)) continue;
            /* (as far as the cars are drawn: Far 16,000, Farthest 32,000; the user's target circle missing on Antarctica) */
            if (hx < -40 || hx > v.cx * 2 + 40 || hy < -40 || hy > v.cy * 2 + 40) continue;
            float s = (float)(v.hx * 7.0 / d); /* the triangle's half width (as 7 units at the focal distance) */
            s = s < 2.2f ? 2.2f : s > 7.5f ? 7.5f : s;
            s *= view_k < 0.5f ? 0.5f : view_k;
            int locked = car->slot == lock;
            if (locked && getenv("TM2PC_MARKS_DEBUG")) { /* tests: the locked car's mark, frame by frame */
                int interp_redrawing(void);
                uint32_t car_slot_object(int slot);
                uint32_t o = car_slot_object(car->slot);
                fprintf(stderr, "marks: %llu %s slot %d object %08X (at %d %d %d) car %.0f %.0f %.0f eye %d %d %d -> %.1f %.1f\n",
                        (unsigned long long)M.vblanks, interp_redrawing() ? "redraw" : "step", car->slot, o,
                        o ? (int32_t)mem_r32(&M.cpu, o + 0xA8) : 0, o ? (int32_t)mem_r32(&M.cpu, o + 0xAC) : 0,
                        o ? (int32_t)mem_r32(&M.cpu, o + 0xB0) : 0, car->x, car->y, car->z, v.eye[0], v.eye[1], v.eye[2], hx, hy);
            }
            const int red[3] = {255, 30, 30}, col[3] = {car->rgb[0], car->rgb[1], car->rgb[2]}, dark[3] = {10, 10, 10};
            if (reticle && locked && mark_project(&v, player, car->slot, 1, car->x, car->y, car->z - WM_UP * 4, &bx, &by, &bd)) {
                /* TM2012's lock: a circle of four arcs, gaps at the top, bottom and sides */
                float r = (float)(v.hx * 62.0 / bd), t = r * 0.07f;
                r = r < 7 ? 7 : r > 70 ? 70 : r;
                t = t < 0.6f ? 0.6f : t > 2.0f ? 2.0f : t;
                for (int q = 0; q < 4; q++) {
                    const int pieces = 8;
                    float a0 = (float)(q * 1.5707963 + 0.28), a1 = (float)((q + 1) * 1.5707963 - 0.28), px = 0, py = 0;
                    for (int k = 0; k <= pieces; k++) {
                        float a = a0 + (a1 - a0) * k / pieces, x = (float)bx + cosf(a) * r * sq, y = (float)by + sinf(a) * r;
                        if (k) wm_line(px, py, x, y, t, red);
                        px = x, py = y;
                    }
                }
            }
            if (!markers) continue;
            float tx = (float)hx, ty = (float)hy - 2; /* the triangle: its point down, over the car */
            float ts = s * 0.9f; /* (the triangle a little smaller than the bar's measure) */
            wm_tri(tx - (ts + 0.6f) * sq, ty - ts * 1.4f - 0.4f, tx + (ts + 0.6f) * sq, ty - ts * 1.4f - 0.4f, tx, ty + 0.9f, dark);
            /* (a team game: never red when locked on, the team's color only: red read as the red team, the user's; the
             * lock-on reticle still shows the target) */
            wm_tri(tx - ts * sq, ty - ts * 1.4f, tx + ts * sq, ty - ts * 1.4f, tx, ty, locked && !team_game ? red : col);
            float top = ty - ts * 1.4f - 2;
            if (health && car->max_hp > 0) {
                int pct = car->hp * 100 / car->max_hp, hc[6];
                pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
                health_rgb(pct, hc);
                float bw = s * 5.0f, bh = s * 0.56f < 1 ? 1 : s * 0.56f;
                const int back[3] = {0, 0, 0}, fill[3] = {hc[3], hc[4], hc[5]};
                wm_rect(tx - (bw / 2 + 1) * sq, top - bh - 1, (bw + 2) * sq, bh + 2, back, 1);
                wm_rect(tx - bw / 2 * sq, top - bh, bw * pct / 100 * sq, bh, fill, 0);
                top -= bh + 3;
            }
            if (names < 2 && (names == 1 || car->human >= 0)) {
                wm_flush();
                const int white[3] = {255, 255, 255};
                wm_name(car->name, tx, top, s / 10.0f < 0.35f ? 0.35f : s / 10.0f, white);
            }
        }
        wm_flush();
    }
    hudstyle_raw = 0;
    sq = 1;
}
