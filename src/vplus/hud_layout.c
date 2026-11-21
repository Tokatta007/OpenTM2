/* The battle HUD laid out for any view (Customize HUD: Layout, Size, ...).
 *
 * The game draws a HUD in one of three layouts: one view (320x240), side by
 * side (160x240) and one above the other (320x120). Each HUD goes to the GPU
 * through DrawOTag (its own order table); here, while a HUD is drawn, its
 * primitives are copied into a list of our own, each moved and scaled, and
 * that list is drawn instead (the game's records keep their numbers).
 *
 * Every primitive belongs to a group by where the game put it in its layout:
 * the radar and target (top left), the health (top right), the energy and
 * weapons (bottom right), turbo and lives (bottom left), the message line
 * (top middle). Each group keeps its place relative to its corner of the
 * layout and goes to the same corner of the actual view, scaled: in a view
 * the layout's size it is the game's HUD exactly; in a wider view the
 * corners spread; in a smaller one things shrink, the radar most. The
 * message line stays at the top, between the radar and the health, and is
 * the first thing left out when there is no room (as the game's split HUD
 * has none). Optional: the radar and health raised to the top edge, and the
 * player's Size. Sprites become textured quads so they can scale. The
 * game's thick split divider is left out (thin lines are drawn over the
 * picture instead: hudlay_dividers).
 *
 * Classic layout: the game's own HUD, untouched (except in 3-4 player views,
 * which need this one). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psyz/video.h>
#include "runtime/runtime.h"
#include "settings.h"
#include "vplus.h"
#include "hud_layout.h"
#include "runtime/rebase_table.h"

#define DIVIDER GA(0x80183EE8u)         /* hudSplitDivider */
#define REAR_VIEW GA(0x801809DCu)       /* rtRearViewActive */
#define OUT_BASE 0x80DD2000u        /* our copy of the HUD's list */
#define OUT_END 0x80DDF000u

static int active, forced = -1;
static float pre_k = 1, pre_x, pre_y; /* hudlay_prescale: scaled about (pre_x, pre_y) first */
static HudLayout lay;

static const int SRC_W[3] = {320, 160, 320}, SRC_H[3] = {240, 240, 120};

int hudlay_group_of(int src, int x, int y, int mirror) /* a point of the game's layout: its group */
{
    switch (src) {
    case 0: /* one view */
        if (mirror && x >= 205 && y >= 14 && y < 56) return G_FIX; /* the rear-view mirror's frame */
        if (x < 100 && y < 100) return G_TL;
        if (x >= 100 && x < 205 && y < 60) return G_TM;
        if (x < 200 && y >= 150) return G_BL;
        if (x >= 200 && y >= 120) return G_BR;
        if (x >= 205 && y < 120) return G_TR;
        return G_C;
    case 1: /* side by side */
        if (x < 81 && y < 100) return G_TL;
        if (x >= 81 && y < 40) return G_TR;
        if (x >= 95 && y >= 120) return G_BR;
        if (x < 95 && y >= 150) return G_BL;
        return G_C;
    default: /* one above the other */
        if (x < 100 && y < 85) return G_TL;
        if (x >= 220 && y < 33) return G_TR;
        if (x >= 200 && y >= 33) return G_BR;
        if (x < 100 && y >= 85) return G_BL;
        if (y < 60) return G_TM;
        return G_C;
    }
}

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
/* raised: the radar and health at the top edge (the message line's height), by view kind (1 view, side by side, one
 * over another); the kill feed's top makes up for its group's (hudlay_feed_top) */
static const float up_tl[3] = {10, 13, 12}, up_tr[3] = {0, 14, 12}, up_tm[3] = {14, 0, 0};
void hudlay_compute(HudLayout *L, int src, int vw, int vh, int mirror) /* each group's place and size for a view */
{
    memset(L, 0, sizeof *L);
    L->src = src;
    L->sw = SRC_W[src];
    L->sh = SRC_H[src];
    L->vw = vw;
    L->vh = vh;
    float user = (settings_video.hud_size >= 50 && settings_video.hud_size <= 150 ? settings_video.hud_size : 100) / 100.0f;
    float k = (float)vw / L->sw < (float)vh / L->sh ? (float)vw / L->sw : (float)vh / L->sh; /* the view against the layout */
    float s_tl = user * CLAMP(k, 0.5f, 1.0f), s = user * CLAMP(sqrtf(k), 0.7f, 1.0f); /* the radar shrinks most */
    /* each group fits the view's height: the radar 66 high, the weapons column 100 */
    if (s_tl * 66 > vh * 0.6f) s_tl = vh * 0.6f / 66;
    float s_br = s;
    if (s_br * 100 > vh * 0.8f) s_br = vh * 0.8f / 100;
    if (vw < L->sw) s_br *= sqrtf((float)vw / L->sw); /* (narrower than its layout, a quarter: the weapons smaller too, not only
                                                      * as tall as a half view's; the user, 2026-10-14) */
    for (int g = 0; g < G_COUNT; g++) L->scale[g] = s;
    L->scale[G_TL] = s_tl;
    L->scale[G_BR] = s_br;
    if (getenv("TM2PC_HUD_DEBUG")) { /* tests: each view's layout and scales */
        static int said[8];
        int key = src * 2 + (vw > vh * 2);
        if (key < 8 && !said[key]++) fprintf(stderr, "hud layout: src %d view %dx%d k %.2f radar %.2f others %.2f weapons %.2f\n", src, vw, vh, k, s_tl, s, s_br);
    }
    L->scale[G_FIX] = 1;
    L->scale[G_VIEW] = 1;
    /* the anchors: the corner (or middle) of the layout and of the view */
    for (int g = 0; g < G_COUNT; g++) {
        float ax = g == G_TL || g == G_BL ? 0 : g == G_TR || g == G_BR ? (float)L->sw : L->sw / 2.0f;
        float ay = g == G_BL || g == G_BR ? (float)L->sh : g == G_C ? L->sh / 2.0f : 0;
        float vx = g == G_TL || g == G_BL ? 0 : g == G_TR || g == G_BR ? (float)vw : vw / 2.0f;
        float vy = g == G_BL || g == G_BR ? (float)vh : g == G_C ? vh / 2.0f : 0;
        L->ax[g] = ax; L->ay[g] = ay; L->vx[g] = vx; L->vy[g] = vy;
    }
    L->ax[G_FIX] = L->vx[G_FIX] = 0;
    L->ay[G_FIX] = L->vy[G_FIX] = 0;
    L->scale[G_VIEW] = 1;
    L->ax[G_VIEW] = L->vx[G_VIEW] = L->ay[G_VIEW] = L->vy[G_VIEW] = 0;
    /* raised: the radar and health at the top edge (the message line's height) */
    if (!settings_video.hud_low_top) {
        L->vy[G_TL] -= up_tl[src] * s_tl;
        L->vy[G_TR] -= up_tr[src] * s;
        L->vy[G_TM] -= up_tm[src] * s;
    }
    /* the message line: at the top, between the radar and the health; left out when there is no room */
    static const float tl_w[3] = {85, 81, 82}, tr_w[3] = {0, 4, 4}, tm_c[3] = {149, 77, 157}; /* (split: the health moved down) */
    float left = tl_w[src] * s_tl, right = vw - (src == 0 && mirror ? 115 : tr_w[src]) * s;
    /* split screen: the rear-view mirror (mirrors.c), 101x30 in a 2-pixel frame as the game's, at the
     * top-right group's scale, in the top-right corner level with the radar's top. Full size where it
     * fits beside the radar and a whole message line; else smaller (down to 55%), then the message
     * line smaller (down to 65%), then without it; under the radar's row if even that is too narrow.
     * (A wider view gives it room by itself.) */
    if (mirror && src != 0) {
        static const float radar_top[3] = {0, 20, 18}, radar_h[3] = {0, 72, 72};
        /* (Small, the setting or the rear-view button's: 70%, the message bar the room) */
        float sm = mirror == 2 ? 0.7f : 1.0f; /* (70% of Full's size in this view) */
        float ms = s < 1 ? s : 1, avail = vw - 3 - (left + 3) - 4, bar = settings_video.hud_no_messages ? 0 : 150 * s + 6;
        float k = (avail - bar) / 101;
        if (k < 0.55f) k = (avail - 0.65f * bar) / 101;
        if (k < 0.55f) k = avail / 101;
        if (k > ms) k = ms;
        L->mw = 101 * sm * (k >= 0.55f ? k : ms) + 4;
        L->mh = 30 * sm * (k >= 0.55f ? k : ms) + 4;
        L->mx = vw - 3 - L->mw;
        L->my = L->vy[G_TL] + radar_top[src] * s_tl; /* the frame level with the radar's */
        if (L->my < 2) L->my = 2;
        if (k < 0.55f) L->my = L->vy[G_TL] + (radar_top[src] + radar_h[src]) * s_tl + 2;
        else right = L->mx - 2;
    }
    L->vx[G_TM] += (L->sw / 2.0f - tm_c[src]) * s; /* centred in the view (the game's is a little left of it) */
    float half = 75 * s, c = vw / 2.0f, bc = c;
    L->show[G_TM] = 1;
    if (c - half < left || c + half > right) {
        /* centered in the view, smaller (down to 65%), where that clears the radar and the right side (3-4 players'
         * quarters, side by side); else between them (smaller, down to 65%), or left out */
        float room = (c - left < right - c ? c - left : right - c) - 2, fitc = room / half;
        float fit = (right - left - 4) / (2 * half);
        float top = L->vy[G_TM] + (src == 0 ? 19 : 4) * L->scale[G_TM]; /* the bar's top stays (one view: the bar at y 19) */
        /* (the larger of the two: centered in the view only when that is as large; the gap between the radar and a
         * split screen's mirror is off the view's middle, and centering in the view left room unused on one side and
         * squeezed the letters more: the user's 2026-11-15 co-op take) */
        if (fitc >= 0.65f && fitc >= (fit < 1 ? fit : 1) - 0.001f) {
            L->scale[G_TM] *= fitc;
            L->vy[G_TM] = top - (src == 0 ? 19 : 4) * L->scale[G_TM];
            L->vx[G_TM] = c + (L->vx[G_TM] - c) * fitc; /* (vx[G_TM] - c: its centre's offset) */
            half *= fitc;
        } else if (fit < 1 && fit >= 0.65f) {
            L->scale[G_TM] *= fit;
            L->vy[G_TM] = top - (src == 0 ? 19 : 4) * L->scale[G_TM];
            L->vx[G_TM] = (left + right) / 2 + (L->vx[G_TM] - c) * fit;
            half *= fit;
            bc = (left + right) / 2;
        } else if (fit >= 1) {
            L->vx[G_TM] += (left + right) / 2 - c;
            bc = (left + right) / 2;
        } else L->show[G_TM] = 0;
    }
    L->bar_r = bc + half;
    L->bar_b = L->vy[G_TM] + ((src == 0 ? 19 : 4) + 16) * L->scale[G_TM];
    if (settings_video.hud_no_messages) L->show[G_TM] = 0;
    for (int g = 0; g < G_COUNT; g++) if (g != G_TM) L->show[g] = 1;
}

int hudstyle(void);                     /* hud_styles.c */
int mirrors_wanted(void);               /* mirrors.c */
int hudstyle_drops(uint32_t offset);
extern int hudstyle_raw;
float wide_k(void);                     /* wide.c */
int hudlay_adaptive(void) { return settings_video.hud_layout == 0 || hudstyle() || wide_k() > 1; } /* (a style, widescreen: always laid out) */
static uint32_t hud_rec; /* the HUD record of the player whose HUD is drawn */
int mirror_on(int p);                   /* mirrors.c */
/* Lining up (the user's): at the top the message bar and the kill feed (in its corner) level with the radar's top; at
 * the bottom the score line ("Lives: N", "Kills: N") level with the weapons box's bottom and with the radar's left.
 * Each frame measures where they landed (the view's units, as drawn) and the next one moves the bar, the feed and the
 * bottom left group by the difference: any style, size, view and player count. Spectating (the score line over the
 * legend) leaves the bottom as it is. */
static uint32_t cur_addr; /* (below: the primitive's address) */
/* an address in a retail range, as the build has it: from its start's place (the range's end may be in another
 * block, moved elsewhere: GA(0x80182308) is past the HUD's player records, so the weapon icon counted as a letter of
 * the lives and moved with them, the user's 2026-10-31) */
static int in_retail(uint32_t a, uint32_t lo, uint32_t hi) { return a >= GA(lo) && a - GA(lo) < hi - lo; }
extern int hudstyle_box_ok[4], hudstyle_box_left[4], hudstyle_box_bottom[4]; /* hud_styles.c */
int hudstyle(void);
typedef struct { float radar_top, radar_left, bar_top, bar_bot, feed_top, lives_bot, lives_left, box_bot, fix_bot, bl_bot, bl_left; } HudSeen;
static HudSeen seen[4];
static struct { float tm, feed, blx, bly; int key, mode; uint64_t at; } fix[4]; /* mode: the feed's place (feed_corner) */
static int feed_on, feed_corner; /* feed_corner: 1 its corner (the radar's top), 2 under the bar, 3 under the mirror */
int vp_watch_hud(int player);
static void seen_reset(HudSeen *m)
{
    m->radar_top = m->bar_top = m->feed_top = m->box_bot = 1e9f;
    m->radar_left = m->lives_left = m->bl_left = 1e9f;
    m->lives_bot = m->bar_bot = m->fix_bot = m->bl_bot = -1e9f;
}
static void fix_update(int p) /* the last frame's measures into the corrections */
{
    HudSeen *m = &seen[p];
    float lim = 40;
    /* (each by the difference past a dead band: positions are whole units, a target between two would flicker) */
#define NUDGE(f, d) do { float d_ = (d); if (d_ > 0.75f || d_ < -0.75f) (f) += d_; } while (0)
    if (m->radar_top < 1e8f && m->bar_top < 1e8f) NUDGE(fix[p].tm, m->radar_top - m->bar_top);
    /* the feed's top: the radar's (its corner), else 2 units under the bar or the mirror */
    int fm = fix[p].mode;
    float want = fm == 1 ? m->radar_top : fm == 2 && m->bar_bot > -1e8f ? m->bar_bot + 2
               : fm == 3 ? (lay.src == 0 ? (m->fix_bot > -1e8f ? m->fix_bot + 2 : 1e9f) : lay.my + lay.mh + 2) : 1e9f;
    if (want < 1e8f && m->feed_top < 1e8f) NUDGE(fix[p].feed, want - m->feed_top);
    /* the score line's letters (their cell has a blank row or so under the letters) level with the box's bottom */
    float ink = 1.25f * (lay.scale[G_BL] > 0 ? lay.scale[G_BL] : 1);
    if (hudstyle() && hudstyle_box_ok[p]) /* a style's own box (drawn by itself; TM:Black's bottom given exactly) */
        m->box_bot = (float)(hudstyle_box_bottom[p] - (hudstyle() == 1 ? 0 : 1));
    if (!hudstyle() && lay.src == 0) {
        /* Twisted Metal 2's one view: the bottom left is the lives over the health bar, the bar further left and lower.
         * Lining the lives up moved the bar off the view's left edge and up to the energy bar (TM2 Icon), or under the
         * view's bottom (TM2 List: its box reaches lower), the user's. The whole group instead: its left the radar's,
         * its bottom where the PlayStation has the bar's (11 units over the view's bottom). */
        float want = (float)lay.vh - 11 * (lay.scale[G_BL] > 0 ? lay.scale[G_BL] : 1);
        if (!vp_watch_hud(p) && m->bl_bot > -1e8f) NUDGE(fix[p].bly, want - m->bl_bot);
        if (!vp_watch_hud(p) && m->bl_left < 1e8f && m->radar_left < 1e8f) NUDGE(fix[p].blx, m->radar_left - m->bl_left);
    } else {
        if (!vp_watch_hud(p) && m->lives_bot > -1e8f && m->box_bot < 1e8f) NUDGE(fix[p].bly, m->box_bot + ink - m->lives_bot);
        if (!vp_watch_hud(p) && m->lives_left < 1e8f && m->radar_left < 1e8f) NUDGE(fix[p].blx, m->radar_left - m->lives_left);
    }
    static int align_dbg = -1; /* (tests: TM2PC_ALIGN_DEBUG; read once, not every frame) */
    if (align_dbg < 0) align_dbg = getenv("TM2PC_ALIGN_DEBUG") != NULL;
    if (align_dbg && p == 0)
        fprintf(stderr, "align: radar %.0f,%.0f bar %.0f..%.0f feed %.0f lives %.0f,%.0f box %.0f -> tm %.1f feed %.1f bl %.1f,%.1f (mode %d)\n",
                m->radar_left, m->radar_top, m->bar_top, m->bar_bot, m->feed_top, m->lives_left, m->lives_bot, m->box_bot,
                fix[p].tm, fix[p].feed, fix[p].blx, fix[p].bly, fix[p].mode);
    float *f[4] = {&fix[p].tm, &fix[p].feed, &fix[p].blx, &fix[p].bly};
    for (int i = 0; i < 4; i++) if (*f[i] > lim || *f[i] < -lim) *f[i] = 0; /* (something else moved: start over) */
    seen_reset(m);
}
static void measure(int g, int x0, int y0, int x1, int y1, int sprite)
{
    if (!hud_rec) return;
    if (x1 < -200 || y1 < -200 || x0 > 4000 || y0 > 4000) return; /* (a part parked far off the view to hide it: a radar piece
                                                                    * at -998 made the lives' line jump 4 units right for a
                                                                    * frame now and then, the user's co-op take 2026-11-17) */
    HudSeen *m = &seen[(hud_rec - RB_hudPlayerRecords) / 1900u & 3];
    if (g == G_TL && !sprite) { /* the radar (its back and frame; the arrows are sprites) */
        if (y0 < m->radar_top) m->radar_top = (float)y0;
        if (x0 < m->radar_left) m->radar_left = (float)x0;
    }
    if (g == G_TM && !sprite && y0 < m->bar_top) m->bar_top = (float)y0;
    if (g == G_TM && !sprite && y1 > m->bar_bot) m->bar_bot = (float)y1;
    if (g == G_FIX && !sprite && y1 > m->fix_bot) m->fix_bot = (float)y1;
    if (feed_on && sprite && y0 < m->feed_top) m->feed_top = (float)y0;
    if (g == G_BL && x1 > x0 && y1 > y0) { /* (the whole group: the lives and the health bar) */
        if (y1 > m->bl_bot) m->bl_bot = (float)y1;
        if (x0 < m->bl_left) m->bl_left = (float)x0;
    }
    if (sprite && g == G_BL && in_retail(cur_addr, 0x8018208Cu, 0x80182308u)) { /* (the lives, the port's score line too) */
        if (y1 > m->lives_bot) m->lives_bot = (float)y1;
        if (x0 < m->lives_left) m->lives_left = (float)x0;
    }
    if (!sprite && (g == G_BR || g == G_VIEW) && y1 > -1e8f && (m->box_bot > 1e8f || y1 > m->box_bot)) m->box_bot = (float)y1;
}
void hudlay_feed_measure(int on) { feed_on = on; }   /* fixes.c: the kill feed's letters follow */
void hudlay_player(int p) /* the player whose HUD is laid out (split screen: their own mirror, or none) */
{
    hud_rec = RB_hudPlayerRecords + 1900u * (uint32_t)(p & 3);
    hud_select(p & 3); /* their HUD settings */
    if (active) {
        float wk = lay.wk;
        int mirror_split_mode(int p); /* mirrors.c: 0 none, 1 full, 2 small */
        hudlay_compute(&lay, lay.src, lay.vw, lay.vh, lay.src == 0 ? mem_r32(&M.cpu, REAR_VIEW) != 0 : mirror_split_mode(p & 3));
        lay.wk = wk;
        int q = p & 3, key = lay.src * 1000003 + lay.vw * 1009 + lay.vh * 17 + hudstyle() * 7 + settings_video.hud_size;
        if (fix[q].key != key) { memset(&fix[q], 0, sizeof fix[q]); fix[q].key = key; seen_reset(&seen[q]); }
        if (fix[q].at != M.vblanks) { fix[q].at = M.vblanks; fix_update(q); } /* (a new frame: the last one's measures) */
        lay.vy[G_TM] += fix[q].tm;
        lay.vy[G_BL] += fix[q].bly;
        lay.vx[G_BL] += fix[q].blx * (lay.wk > 1 ? lay.wk : 1);
    }
}

void hudlay_begin(int src, int vw, int vh)
{
    active = 1;
    forced = -1;
    float wk = wide_k(); /* widescreen: laid out for the view as seen (wider), drawn squeezed back into the game's frame */
    hudlay_compute(&lay, src < 0 || src > 2 ? 2 : src, (int)lrintf((float)vw * wk), vh, src == 0 ? mem_r32(&M.cpu, REAR_VIEW) != 0 : mirrors_wanted());
    lay.wk = wk;
}
void hudlay_force(int group) { forced = group; } /* the port's own lines: their group, -1 by place */
void hudlay_source(int src) { (void)src; }
void hudlay_end(void) { active = 0; forced = -1; pre_k = 1; }

static int cur_sprite; /* the primitive being placed is a sprite or textured quad (letters) */
static int cur_speed;  /* the primitive being placed is the speedometer's */
static uint32_t cur_off = 0xFFFFFFFFu, cur_addr; /* its offset in the player's HUD record (else ~0), its address */
static int mirror_dy; /* the rear-view mirror raised (hudlay_mirror): its frame with it */
/* The speedometer (its sprites' right and bottom edges, the frame before) beside a style's box */
extern int hudstyle_box_ok[4], hudstyle_box_left[4], hudstyle_box_bottom[4];
static int spd_r[4] = {-9999, -9999, -9999, -9999}, spd_b[4], spd_r_now[4] = {-9999, -9999, -9999, -9999}, spd_b_now[4];
static uint64_t spd_at[4];
static int cur_player(void) { return hud_rec ? (int)((hud_rec - RB_hudPlayerRecords) / 1900u) & 3 : 0; }
/* TM:2012's lives ("Lives: N", their letters' left and bottom edges, the frame before), above the energy bar */
extern int hudstyle_en_ok[4], hudstyle_en_left[4], hudstyle_en_top[4];
static int liv_l[4] = {-9999, -9999, -9999, -9999}, liv_b[4], liv_l_now[4] = {9999, 9999, 9999, 9999}, liv_b_now[4];
static uint64_t liv_at[4];
static int cur_lives; /* the primitive being placed is a letter of the lives' line */
static void lives_seen(int lo, int bot)
{
    int p = cur_player();
    if (liv_at[p] != M.vblanks) {
        if (liv_l_now[p] < 9999) { liv_l[p] = liv_l_now[p]; liv_b[p] = liv_b_now[p]; }
        liv_l_now[p] = 9999;
        liv_b_now[p] = -9999;
        liv_at[p] = M.vblanks;
    }
    if (lo < liv_l_now[p]) liv_l_now[p] = lo;
    if (bot > liv_b_now[p]) liv_b_now[p] = bot;
}
/* TM2 Icon / TM2 List: the health bar's left (the game's places, the frame before), for the lives' left (the user's) */
static int hp_l[4] = {-9999, -9999, -9999, -9999}, hp_l_now[4] = {9999, 9999, 9999, 9999};
static uint64_t hp_at[4];
static void health_seen(int lo)
{
    int p = cur_player();
    if (hp_at[p] != M.vblanks) {
        if (hp_l_now[p] < 9999) hp_l[p] = hp_l_now[p];
        hp_l_now[p] = 9999;
        hp_at[p] = M.vblanks;
    }
    if (lo < hp_l_now[p]) hp_l_now[p] = lo;
}
static void speedo_seen(int hi, int bot)
{
    int p = cur_player();
    if (spd_at[p] != M.vblanks) { /* a new frame: the last one's edges kept */
        if (spd_r_now[p] > -9999) { spd_r[p] = spd_r_now[p]; spd_b[p] = spd_b_now[p]; }
        spd_r_now[p] = -9999;
        spd_b_now[p] = -9999;
        spd_at[p] = M.vblanks;
    }
    if (hi > spd_r_now[p]) spd_r_now[p] = hi;
    if (bot > spd_b_now[p]) spd_b_now[p] = bot;
}
int vp_watch_hud(int player); /* fixes.c: spectating (their own car's numbers left out) */
int vp_spectating(void);       /* fixes.c: one view's spectate legend shown (1 chase, 2 cinematic) */
int vp_watching(int player);   /* split screen: this player's legend shown */
static int cur_player(void);
static int watch_drops(void) /* spectating: the car's own parts (health, energy, turbo, speed) */
{
    if (!vp_watch_hud(cur_player()) || cur_off == 0xFFFFFFFFu) return 0;
    return (cur_off >= 0x24 && cur_off < 0x6C) || (cur_off >= 0xB4 && cur_off < 0xFC) || (cur_off >= 0x5D0 && cur_off < 0x70C);
}
static int place(int cx, int cy, int *dx, int *dy, int *g)
{
    *dx = *dy = 0;
    *g = forced >= 0 ? forced : hudlay_group_of(lay.src, cx, cy, lay.src == 0 && mem_r32(&M.cpu, REAR_VIEW));
    if (forced < 0 && lay.src == 0 && cur_sprite && cy >= 20 && cy <= 36) *g = G_TM; /* the message's letters, however long (past the mirror's left edge too) */
    if (forced < 0 && cur_sprite && *g != G_TL && *g != G_TM && cx < 140 && cy >= 40 && cy < (lay.src == 2 ? 85 : 100))
        *g = G_TL; /* the target's name, however long ("DARK TOOTH": its last letters past the radar's group) */
    if (forced < 0 && lay.src == 0 && *g == G_TL && cy >= 64) *dx = -3; /* the target panel: its left under the radar frame's */
    if (*g == G_FIX) *dy = mirror_dy;
    if (hudstyle() == 2 && lay.src == 0 && *g == G_BL) { *dx = -17; *dy = 8; } /* TM:2012: the lives left, over the energy bar */
    if (hudstyle() == 1 && lay.src == 0 && *g == G_BL) { *dx = -19; *dy = 21; } /* TM:Black: the lives in the bottom left corner */
    if (hudstyle() == 1 && cur_speed) { *dx = 6; *dy = 11; } /* (until the box and the speedometer are seen) */
    if (hudstyle() == 2 && cur_speed) { *dx = -3; *dy = 10; }
    if ((hudstyle() == 1 || hudstyle() == 2) && cur_speed && hudstyle_box_ok[cur_player()] && spd_r[cur_player()] > -9999) {
        /* TM:Black, TM:2012: left of the box (energy and turbo; health and turbo), bottoms level, in the view's own
         * places as the box (so at any aspect) */
        int p = cur_player();
        *g = G_VIEW;
        *dx = hudstyle_box_left[p] - spd_r[p]; /* (each style's left: its gap already) */
        *dy = hudstyle_box_bottom[p] - spd_b[p];
    }
    if (hudstyle() == 2 && cur_lives && hudstyle_en_ok[cur_player()] && liv_l[cur_player()] > -9999) {
        /* TM:2012: the lives' line just above the energy bar, its left with the bar's (the view's own places: any aspect) */
        int p = cur_player();
        *g = G_VIEW;
        *dx = hudstyle_en_left[p] - liv_l[p];
        *dy = hudstyle_en_top[p] - 2 - liv_b[p];
    }
    if (lay.src == 0 && !hudstyle() && cur_lives && liv_l[cur_player()] > -9999 && hp_l[cur_player()] > -9999)
        *dx = hp_l[cur_player()] - liv_l[cur_player()]; /* TM2 Icon / List: the lives' left over the health bar's (as TM:2012) */
    if (lay.src != 0 && !hudstyle()) { /* split: the health at the bottom left as in one view, the turbo and lives over it */
        if (cur_off >= 0x24 && cur_off < 0x6C) {
            *g = G_BL;
            *dx = lay.src == 1 ? 10 - 82 : 10 - 238;
            *dy = lay.src == 1 ? 220 - 19 : 100 - 17; /* (the TM2 turbo bar's room under it) */
        } else if ((cur_off >= 0x6B8 && cur_off < 0x70C) || in_retail(cur_addr, 0x80182160u, 0x80182308u)) {
            *dy = -20;
        }
    }
    if (vp_watch_hud(cur_player()) && !feed_on && (*g == G_TL || *g == G_TR || *g == G_BR || watch_drops()))
        return 0; /* spectating (the kill feed stays: it is everyone's, not the watched car's; the user's 2026-11-18) */
    /* the spectate legend along the view's bottom (12 units, options.c scoreboard_frame): the score line over it */
    if (*g == G_BL && ((lay.src == 0 && vp_spectating()) || vp_watching(cur_player()))) *dy -= 14;
    return lay.show[*g];
}

static int tx, ty, tg; /* the current primitive's move */
static float px(int x) { return pre_x + (x - pre_x) * pre_k; }
static float py(int y) { return pre_y + (y - pre_y) * pre_k; }
static float mir_fx, mir_fy; /* (hudlay_mirror) */
static float mapf_x(int x) /* unrounded (precise vertices) */
{
    if (tg == G_FIX && lay.src == 0) return 312.0f - (312.0f - (px(x) + tx)) * mir_fx; /* the mirror's frame: its size, about its picture's corner */
    float v = lay.vx[tg] + (px(x) + tx - lay.ax[tg]) * lay.scale[tg];
    if (lay.wk > 1 && tg != G_FIX) v /= lay.wk; /* widescreen (the rear-view mirror's frame stays with its picture) */
    return v;
}
static float mapf_y(int y)
{
    if (feed_on && hud_rec) return lay.vy[tg] + (py(y) + ty - lay.ay[tg]) * lay.scale[tg] + fix[(hud_rec - RB_hudPlayerRecords) / 1900u & 3].feed;
    if (tg == G_FIX && lay.src == 0) return 20.0f + mirror_dy + (py(y) - 20.0f) * mir_fy; /* (its top kept) */
    return lay.vy[tg] + (py(y) + ty - lay.ay[tg]) * lay.scale[tg];
}
static int map_x(int x) { float v = mapf_x(x); return (int)(v + (v >= 0 ? 0.5f : -0.5f)); }
static int map_y(int y) { float v = mapf_y(y); return (int)(v + (v >= 0 ? 0.5f : -0.5f)); }
/* Bars, backs and boxes keep their exact places (the wobble setting's precise vertices, gte.c): each corner
 * rounded on its own would put a bar a pixel off its frame at some sizes (and widescreen); letters stay whole */
extern int gte_precise_on;
void gte_precise_put(uint32_t ea, uint32_t word, float x, float y, float w);
static uint32_t out_words_at(int i);
static uint32_t emitted; /* the last emit's address (0: none) */
static void precise_vertex(int word_index, int x, int y)
{
    if (emitted) gte_precise_put(emitted + 4 + 4u * (uint32_t)word_index, out_words_at(word_index), mapf_x(x), mapf_y(y), 1);
}
static float sc(void) { return lay.scale[tg] * pre_k; }
float hudlay_wk(void) { return active && lay.wk > 1 ? lay.wk : 1; } /* widescreen: the layout's squeeze (hud_styles.c) */
void hudlay_unmap(int X, int Y, int *x, int *y) /* a point of the view as seen: the layout's point the current group maps there */
{
    int g = forced >= 0 ? forced : G_C;
    float k = lay.scale[g] > 0 ? lay.scale[g] : 1;
    *x = (int)lrintf(lay.ax[g] + (X - lay.vx[g]) / k);
    *y = (int)lrintf(lay.ay[g] + (Y - lay.vy[g]) / k);
}
/* the kill feed's scale: its font's texels a whole number of screen pixels (about 0.64 of the HUD's own: 3 at
 * 2000x1124), so every letter alike; at half size a texel was 2.34 pixels there, some 2 and some 3 (the user's
 * 2026-11-01, option 1: 3 pixels) */
float hudlay_feed_k(void)
{
    PsyzSize size = Psyz_VideoGetDisplaySize();
    float per = size.h > 0 ? size.h / 240.0f * lay.scale[G_TR] : 0; /* (screen pixels a texel at the HUD's size) */
    if (per <= 0) return 0.5f;
    float n = floorf(per * 0.64f + 0.5f);
    if (n < 1) n = 1;
    return n / per;
}
void hudlay_prescale(int x, int y, float k) { pre_x = (float)x; pre_y = (float)y; pre_k = k > 0 ? k : 1; } /* k 1: none */
/* the kill feed's first line (the layout's units): in the top right corner; under the rear-view mirror, if any; under
 * the message bar where its lines (width: the widest, in the layout's units) would reach the bar */
int hudlay_feed_top(float width)
{
    feed_corner = 0;
    if (!active) return 10;
    feed_corner = 3; /* (under the mirror) */
    if (lay.mw > 0 && lay.scale[G_TR] > 0) return (int)((lay.my + lay.mh - lay.vy[G_TR]) / lay.scale[G_TR] + lay.ay[G_TR]) + 3;
    if (lay.src == 0 && mem_r32(&M.cpu, REAR_VIEW)) return 56 + mirror_dy;
    int raised = settings_video.hud_low_top || lay.src < 0 || lay.src > 2 ? 0 : (int)up_tr[lay.src]; /* (the group's raise: under it) */
    /* the feed's lines reaching the message bar (its left edge past the bar's right): the feed starts under the bar */
    feed_corner = 2;
    if (lay.show[G_TM] && !settings_video.hud_no_messages && lay.scale[G_TR] > 0 && lay.bar_r + 3 > lay.vw - 4 - width * lay.scale[G_TR])
        return (int)((lay.bar_b + 2 - lay.vy[G_TR]) / lay.scale[G_TR] + lay.ay[G_TR]);
    feed_corner = 1; /* (lined up with the radar's top: hudlay_player) */
    return (lay.src == 0 ? 6 : 4) + raised;
}
void hudlay_feed_place(void) /* fixes.c, after hudlay_feed_top: the place chosen is this player's (a new one: corrected anew) */
{
    int q = hud_rec ? (int)((hud_rec - RB_hudPlayerRecords) / 1900u) & 3 : 0;
    if (fix[q].mode != feed_corner) { fix[q].mode = feed_corner; fix[q].feed = 0; }
}

static uint32_t xy(uint32_t w) { int x = (int16_t)(w & 0xFFFF), y = (int16_t)(w >> 16); return ((uint32_t)(uint16_t)map_y(y) << 16) | (uint16_t)map_x(x); }

static int cmd_words(uint32_t cmd, uint32_t p, uint32_t end)
{
    if (cmd >= 0x20 && cmd < 0x40) {
        int n = (cmd & 8) ? 4 : 3, tex = (cmd & 4) != 0, g = (cmd & 0x10) != 0;
        return 1 + n * (1 + tex) + (g ? n - 1 : 0);
    }
    if (cmd >= 0x40 && cmd < 0x60) {
        if (!(cmd & 8)) return (cmd & 0x10) ? 4 : 3;
        int n = 1;
        for (uint32_t a = p + 4; a < end && (mem_r32(&M.cpu, a) & 0xF000F000u) != 0x50005000u; a += 4) n++;
        return n + 1;
    }
    if (cmd >= 0x60 && cmd < 0x80) return 2 + ((cmd & 4) != 0) + (((cmd >> 3) & 3) == 0);
    if (cmd == 0x02) return 3;
    if (cmd >= 0x80 && cmd < 0xA0) return 4;
    return 1;
}

static uint32_t out, out_prev;
static uint32_t out_words[16];
static uint32_t out_words_at(int i) { return out_words[i]; }
static void emit(int n) /* one primitive into our list */
{
    emitted = 0;
    if (n <= 0 || out + 4u * (uint32_t)(n + 1) >= OUT_END) return;
    emitted = out;
    mem_w32(&M.cpu, out, ((uint32_t)n << 24) | 0xFFFFFF);
    for (int i = 0; i < n; i++) mem_w32(&M.cpu, out + 4 + 4u * (uint32_t)i, out_words[i]);
    if (out_prev) mem_w32(&M.cpu, out_prev, (mem_r32(&M.cpu, out_prev) & 0xFF000000u) | (out & 0xFFFFFF));
    out_prev = out;
    out += 4u * (uint32_t)(n + 1);
}

/* a range of primitives (by address) scaled about a point, as hudlay_prescale, while the order table is walked: the
 * target panel's name too long for it, its letters shrunk to fit (fixes.c, hudlay_range_scale); one a player */
static struct { uint32_t a, b; float x, y, k; } rs[4];
void hudlay_range_scale(uint32_t a, uint32_t b, int x, int y, float k)
{
    static int next;
    int slot = -1;
    for (int i = 0; i < 4 && slot < 0; i++) if (rs[i].a == a) slot = i; /* (the same buffer: its entry again) */
    if (slot < 0) slot = next++ & 3;
    rs[slot].a = a, rs[slot].b = k != 1.0f ? b : a, rs[slot].x = (float)x, rs[slot].y = (float)y, rs[slot].k = k;
}
static void prim_placed(uint32_t p, int words, uint32_t *e1);
static void prim(uint32_t p, int words, uint32_t *e1)
{
    for (int i = 0; i < 4; i++)
        if (p - 4 >= rs[i].a && p - 4 < rs[i].b && pre_k == 1.0f) {
            pre_x = rs[i].x, pre_y = rs[i].y, pre_k = rs[i].k;
            prim_placed(p, words, e1);
            pre_k = 1;
            return;
        }
    prim_placed(p, words, e1);
}
static void prim_placed(uint32_t p, int words, uint32_t *e1)
{
    uint32_t off = p - 4 - hud_rec;
    if (hud_rec && off < 1900u && hudstyle_drops(off)) return; /* a style draws this part itself */
    if (hud_rec && off < 1900u && settings_video.hud_no_turbo && off >= 0x6B8 && off < 0x70C) return; /* the turbo count */
    if (hud_rec && off < 1900u && settings_video.hud_no_speed && ((off >= 0x5D0 && off < 0x620) || (off >= 0x670 && off < 0x6B8)))
        return; /* the speedometer (hudPrintNumber3's sprites) */
    cur_speed = hud_rec && off < 1900u && ((off >= 0x5D0 && off < 0x620) || (off >= 0x670 && off < 0x6B8));
    cur_off = hud_rec && off < 1900u ? off : 0xFFFFFFFFu;
    cur_addr = p - 4;
    if (settings_video.hud_no_lives && in_retail(p - 4, 0x8018208Cu, 0x80182308u)) return; /* "Lives: N" (each layout's text) */
    uint32_t cmd = mem_r32(&M.cpu, p) >> 24;
    for (int i = 0; i < words && i < 16; i++) out_words[i] = mem_r32(&M.cpu, p + 4u * (uint32_t)i);
    if (!hudstyle() && settings_video.hud_turbo_bar && hud_rec && off < 1900u && off >= 0x6B8 && off < 0x70C) return; /* the bar instead */
    if (hud_rec && off < 1900u && settings_video.hud_no_energy && off >= 0xB4 && off < 0xFC) return; /* the energy bar and its back */
    if (words > 16) return; /* a long poly-line: not in a HUD */
    if (cmd == 0xE1) *e1 = out_words[0];
    int vert[4], nv = 0;
    if (cmd >= 0x20 && cmd < 0x40) {
        int n = (cmd & 8) ? 4 : 3, tex = (cmd & 4) != 0, g = (cmd & 0x10) != 0, at = 1;
        for (int i = 0; i < n; i++) { if (g && i) at++; vert[nv++] = at; at += 1 + tex; }
    } else if (cmd >= 0x40 && cmd < 0x60 && !(cmd & 8)) {
        vert[nv++] = 1;
        vert[nv++] = (cmd & 0x10) ? 3 : 2;
    } else if (cmd >= 0x60 && cmd < 0x80) {
        vert[nv++] = 1;
    }
    if (!nv) { emit(words); return; }
    /* its element: by its middle */
    int lo = 4096, hi = -4096, top = 4096, bot = -4096;
    for (int i = 0; i < nv; i++) {
        int x = (int16_t)(out_words[vert[i]] & 0xFFFF), y = (int16_t)(out_words[vert[i]] >> 16);
        if (x < lo) lo = x; if (x > hi) hi = x; if (y < top) top = y; if (y > bot) bot = y;
    }
    int rw = 0, rh = 0;
    if (cmd >= 0x60 && cmd < 0x80) {
        int sz = (cmd >> 3) & 3;
        int wh_at = 2 + ((cmd & 4) != 0);
        rw = sz == 0 ? (int)(out_words[wh_at] & 0xFFFF) : sz == 1 ? 1 : sz == 2 ? 8 : 16;
        rh = sz == 0 ? (int)(out_words[wh_at] >> 16) : rw;
        hi = lo + rw;
        bot = top + rh;
    }
    if (p == DIVIDER + 4 || (hi - lo > 280 && bot - top < 12) || (bot - top > 200 && hi - lo < 12)) return; /* the split divider */
    cur_sprite = (cmd >= 0x60 && cmd < 0x80 && (cmd & 4)) || (cmd >= 0x24 && cmd < 0x40 && (cmd & 4));
    if (cur_speed) speedo_seen(hi, bot);
    /* (the lives' line; the port's score line, "Level: N", "Kills: N", is printed in the same buffers: fixes.c) */
    cur_lives = !cur_speed && cur_sprite && in_retail(cur_addr, 0x8018208Cu, 0x80182308u);
    if (cur_lives) lives_seen(lo, bot);
    if (!cur_sprite && cur_off >= 0x24 && cur_off < 0x6C) health_seen(lo);
    if (!place((lo + hi) / 2, (top + bot) / 2, &tx, &ty, &tg)) return; /* a part switched off, or no room */
    /* the message line's letters (hudMessageTextSprites): centered in the bar, a unit down (the bar stays level with
     * the radar); Classic: where the game has them, a little high */
    if (!settings_video.hud_msg_classic && in_retail(cur_addr, 0x80181E34u, 0x80182098u)) ty += 1;
    if (cmd >= 0x60 && cmd < 0x80) { /* a rectangle: scaled as a quad */
        int x0 = (int16_t)(out_words[1] & 0xFFFF), y0 = (int16_t)(out_words[1] >> 16);
        int X0 = map_x(x0), Y0 = map_y(y0), X1 = map_x(x0 + rw), Y1 = map_y(y0 + rh); /* both edges: letters keep their spacing */
        if (cur_sprite && sc() != 1.0f) { /* letters: one height wherever their row lands (each edge rounded on its own made
                                           * rows of small letters 4 or 5 units high: the kill feed in quarters) */
            float hk = rh * sc();
            Y0 = Y1 - (int)(hk + 0.5f); /* (from the bottom: a line's letters share it, so they round alike. From each
                                          * top, tall and short letters of one line came a unit apart, which way changing
                                          * as the line's place moved a fraction: "Lives: 0" uneven and jittering in
                                          * 4-player views, the user's 2026-11-15 co-op take) */
        }
        if (X1 <= X0) X1 = X0 + 1;
        if (Y1 <= Y0) Y1 = Y0 + 1;
        measure(tg, X0, Y0, X1, Y1, cur_sprite);
        if (!(cmd & 4) && gte_precise_on) { /* a tile, as a flat quad at its exact place */
            uint32_t c0 = out_words[0];
            out_words[0] = (c0 & 0x00FFFFFFu) | ((0x28u | (cmd & 2)) << 24);
            out_words[1] = ((uint32_t)(uint16_t)Y0 << 16) | (uint16_t)X0;
            out_words[2] = ((uint32_t)(uint16_t)Y0 << 16) | (uint16_t)X1;
            out_words[3] = ((uint32_t)(uint16_t)Y1 << 16) | (uint16_t)X0;
            out_words[4] = ((uint32_t)(uint16_t)Y1 << 16) | (uint16_t)X1;
            emit(5);
            precise_vertex(1, x0, y0);
            precise_vertex(2, x0 + rw, y0);
            precise_vertex(3, x0, y0 + rh);
            precise_vertex(4, x0 + rw, y0 + rh);
            return;
        }
        if (!(cmd & 4)) { /* a tile: any size */
            uint32_t c0 = out_words[0];
            out_words[0] = (c0 & 0x00FFFFFFu) | ((0x60u | (cmd & 2)) << 24);
            out_words[1] = ((uint32_t)(uint16_t)Y0 << 16) | (uint16_t)X0;
            out_words[2] = ((uint32_t)(Y1 - Y0) << 16) | (uint32_t)(X1 - X0);
            emit(3);
            return;
        }
        if ((sc() == 1.0f && (lay.wk <= 1 || tg == G_FIX)) || *e1 == 0) { /* a sprite at its size */
            out_words[1] = ((uint32_t)(uint16_t)Y0 << 16) | (uint16_t)X0;
            emit(words);
            return;
        }
        uint32_t c0 = out_words[0], uvc = out_words[2];
        int u = (int)(uvc & 0xFF), v = (int)((uvc >> 8) & 0xFF), u1 = u + rw > 255 ? 255 : u + rw, v1 = v + rh > 255 ? 255 : v + rh;
        uint32_t clut = uvc & 0xFFFF0000u, tpage = (*e1 & 0x9FFu) << 16;
        out_words[0] = (c0 & 0x00FFFFFFu) | ((0x2Cu | (cmd & 3)) << 24);
        out_words[1] = ((uint32_t)(uint16_t)Y0 << 16) | (uint16_t)X0;
        out_words[2] = clut | ((uint32_t)v << 8) | (uint32_t)u;
        out_words[3] = ((uint32_t)(uint16_t)Y0 << 16) | (uint16_t)X1;
        out_words[4] = tpage | ((uint32_t)v << 8) | (uint32_t)u1;
        out_words[5] = ((uint32_t)(uint16_t)Y1 << 16) | (uint16_t)X0;
        out_words[6] = ((uint32_t)v1 << 8) | (uint32_t)u;
        out_words[7] = ((uint32_t)(uint16_t)Y1 << 16) | (uint16_t)X1;
        out_words[8] = ((uint32_t)v1 << 8) | (uint32_t)u1;
        emit(9);
        if (gte_precise_on && pre_k != 1.0f) { /* the kill feed (hudlay_feed_k): its letters at their exact places, each
                                                * texel a whole number of pixels (rounded, a letter came a unit wider) */
            precise_vertex(1, x0, y0);
            precise_vertex(3, x0 + rw, y0);
            precise_vertex(5, x0, y0 + rh);
            precise_vertex(7, x0 + rw, y0 + rh);
        }
        return;
    }
    int ox[4], oy[4];
    for (int i = 0; i < nv; i++) {
        ox[i] = (int16_t)(out_words[vert[i]] & 0xFFFF);
        oy[i] = (int16_t)(out_words[vert[i]] >> 16);
        out_words[vert[i]] = xy(out_words[vert[i]]);
    }
    {
        int mx0 = 4096, my0 = 4096, mx1 = -4096, my1 = -4096;
        for (int i = 0; i < nv; i++) {
            int X = (int16_t)(out_words[vert[i]] & 0xFFFF), Y = (int16_t)(out_words[vert[i]] >> 16);
            if (X < mx0) mx0 = X; if (X > mx1) mx1 = X; if (Y < my0) my0 = Y; if (Y > my1) my1 = Y;
        }
        if (nv) measure(tg, mx0, my0, mx1, my1, cur_sprite);
    }
    emit(words);
    if (gte_precise_on && cmd >= 0x20 && cmd < 0x40 && !cur_sprite) /* a polygon (not a letter) */
        for (int i = 0; i < nv; i++) precise_vertex(vert[i], ox[i], oy[i]);
}

void F_80141A30_orig(CPU *c);
void vp_DrawOTag(CPU *c) /* DrawOTag(ot): a HUD, laid out for its view */
{
    if (!active || hudstyle_raw) { F_80141A30_orig(c); return; }
    out = OUT_BASE;
    out_prev = 0;
    uint32_t e1 = 0, node = c->r[4] & 0xFFFFFF, prev_node = 0;
    static int copy_log = -1; /* tests (decomp request 105's run 2): a HUD node with a VRAM copy (GP0 0x80) */
    if (copy_log < 0) copy_log = getenv("TM2PC_OT_COPY_LOG") ? 40 : 0;
    for (int guard = 0; node != 0xFFFFFF && guard < 8192; guard++) {
        uint32_t hdr = mem_r32(&M.cpu, 0x80000000u | node), len = hdr >> 24, p = 0x80000000u | (node + 4), end = p + 4 * len;
        static uint32_t trail[4]; /* (the walk's last nodes) */
        if (guard == 0) memset(trail, 0, sizeof trail);
        if (copy_log > 0 && ((node & 3) || node < 0x10000u)) { /* a link to no node (misaligned, or the kernel's first page) */
            copy_log--;
            fprintf(stderr, "hud bad link: vblank %llu hud list %08X: to %08X; the nodes before:", (unsigned long long)M.vblanks, c->r[4],
                    0x80000000u | node);
            for (int t = 0; t < 4; t++) if (trail[t]) {
                fprintf(stderr, " [%08X:", 0x80000000u | trail[t]);
                for (uint32_t k = 0; k < 6; k++) fprintf(stderr, " %08X", mem_r32(&M.cpu, 0x80000000u | (trail[t] + 4 * k)));
                fprintf(stderr, "]");
            }
            fprintf(stderr, "\n");
        }
        memmove(trail + 1, trail, 3 * sizeof trail[0]);
        trail[0] = node;
        if (copy_log > 0 && len >= 1 && (mem_r32(&M.cpu, p) >> 24) == 0x80) {
            copy_log--;
            fprintf(stderr, "hud copy: vblank %llu hud list %08X node %08X from %08X, %u words:", (unsigned long long)M.vblanks,
                    c->r[4], 0x80000000u | node, prev_node ? 0x80000000u | prev_node : 0, len);
            for (uint32_t k = 0; k <= len && k < 8; k++) fprintf(stderr, " %08X", mem_r32(&M.cpu, 0x80000000u | (node + 4 * k)));
            fprintf(stderr, "\n");
        }
        prev_node = node;
        while (p < end) {
            uint32_t cmd = mem_r32(&M.cpu, p) >> 24;
            int w = cmd_words(cmd, p, end);
            prim(p, w, &e1);
            p += 4u * (uint32_t)w;
        }
        node = hdr & 0xFFFFFF;
    }
    if (!out_prev) return; /* nothing to draw */
    c->r[4] = OUT_BASE;
    F_80141A30_orig(c);
}

/* ---- thin dividers, over the picture (window pixels: a line or two) ----
 * kind: 0 none, 1 side by side, 2 one above the other, 4 quarters, 3 three
 * (player 1 the top), 5 three (player 1 the left). Shown while HUDs are
 * drawn; taken away a few frames after they stop (menus, results). */
static int div_kind = -1, div_w, div_h, ui_owns;
static uint64_t div_seen;
void hudlay_overlay_ui(int on) { ui_owns = on; if (!on) div_kind = -1; } /* the UI's overlay (scoreboard, banner): it draws the lines */
int hudlay_divider_rects(int w, int h, int r[2][4]) /* the lines for a w x h overlay */
{
    int killcam_showing(void); /* vplus/killcam.c: its shots are full screen */
    if (div_kind <= 0 || M.vblanks > div_seen + 6 || killcam_showing()) return 0;
    int t = h / 540 > 1 ? h / 540 : 1, n = 0, hy = h / 2 - t / 2, vx = w / 2 - t / 2, kind = div_kind;
    if (kind == 2 || kind == 4 || kind == 3) { r[n][0] = 0; r[n][1] = hy; r[n][2] = w; r[n][3] = t; n++; }
    if (kind == 5) { r[n][0] = w / 2; r[n][1] = hy; r[n][2] = w - w / 2; r[n][3] = t; n++; }
    if (kind == 1 || kind == 4 || kind == 5) { r[n][0] = vx; r[n][1] = 0; r[n][2] = t; r[n][3] = h; n++; }
    if (kind == 3) { r[n][0] = vx; r[n][1] = h / 2; r[n][2] = t; r[n][3] = h - h / 2; n++; }
    return n;
}
static unsigned char *div_px;
void hudlay_dividers(int kind)
{
    div_seen = M.vblanks;
    PsyzSize size = Psyz_VideoGetDisplaySize();
    if (ui_owns) { div_kind = kind; return; } /* the UI's overlay draws them (hudlay_divider_rects) */
    if (kind == div_kind && size.w == div_w && size.h == div_h) return;
    div_kind = kind;
    div_w = size.w;
    div_h = size.h;
    if (!kind || size.w <= 0 || size.h <= 0) { Psyz_VideoSetOverlay(NULL, 0, 0, NULL, 0); return; }
    free(div_px);
    div_px = calloc((size_t)size.w * size.h, 3); /* black */
    if (!div_px) return;
    int t = size.h / 540 > 1 ? size.h / 540 : 1, w = size.w, h = size.h, r[2][4], n = 0;
    int hy = h / 2 - t / 2, vx = w / 2 - t / 2;
    if (kind == 2 || kind == 4 || kind == 3) { r[n][0] = 0; r[n][1] = hy; r[n][2] = w; r[n][3] = t; n++; }
    if (kind == 5) { r[n][0] = w / 2; r[n][1] = hy; r[n][2] = w - w / 2; r[n][3] = t; n++; }
    if (kind == 1 || kind == 4 || kind == 5) { r[n][0] = vx; r[n][1] = 0; r[n][2] = t; r[n][3] = h; n++; }
    if (kind == 3) { r[n][0] = vx; r[n][1] = h / 2; r[n][2] = t; r[n][3] = h - h / 2; n++; }
    Psyz_VideoSetOverlay(div_px, w, h, &r[0][0], n);
}
void hudlay_vblank(void) /* the dividers away once no HUD is drawn */
{
    if (div_kind > 0 && !ui_owns && M.vblanks > div_seen + 6) {
        Psyz_VideoSetOverlay(NULL, 0, 0, NULL, 0);
        div_kind = -1;
    }
}

/* The rear-view mirror (one view): its 3D view's drawing area (the mirror's two frame records,
 * renderMirrorFrames 8018138C / 8018140C: DRAWENV at +0x10, its clip y +0x12, offset y +0x1A)
 * raised level with the radar and the message bar (the game's y 20; its frame 18..52) */
/* Its size: in widescreen as wide as the game's on a 4:3 screen (the frame is not squeezed with the HUD: its
 * picture is in it), Small (Rear-view mirror: Small) 70%; right edge and top kept. The view-mode row 4 (w +0x18,
 * h +0x1C, x +0x20, the projection's H +0x08) is written each frame from its values as the game has them (kept
 * once), with the drawing area (clip x +0x10, w +0x14, h +0x16, offset x +0x18); the frame's bars follow (mir_fx). */
static float mir_fx = 1, mir_fy = 1;
void hudlay_mirror(void)
{
    int raised = hudlay_adaptive() && !settings_video.hud_low_top;
    int y = raised ? 7 : 20;
    mirror_dy = y - 20;
    const uint32_t row = RB_viewModeTable + 0x60u * 4;
    static int32_t ow, oh, ox, oH;
    if (!ow) { ow = (int32_t)mem_r32(&M.cpu, row + 0x18); oh = (int32_t)mem_r32(&M.cpu, row + 0x1C);
               ox = (int32_t)mem_r32(&M.cpu, row + 0x20); oH = (int32_t)mem_r32(&M.cpu, row + 0x08); }
    int mirror_mode(int p); /* mirrors.c: player 1's this round (the rear-view button: Full, Small, Off) */
    float size = mirror_mode(0) == 2 ? 0.7f : 1.0f, wk = hudlay_wk();
    int32_t w = (int32_t)lrintf(ow * size / wk), h = (int32_t)lrintf(oh * size), x = ox + ow - w;
    int row_x_was = (int32_t)mem_r32(&M.cpu, row + 0x20); /* (the buffers' x were made from it) */
    if (ow > 0) {
        mem_w32(&M.cpu, row + 0x18, (uint32_t)w);
        mem_w32(&M.cpu, row + 0x1C, (uint32_t)h);
        mem_w32(&M.cpu, row + 0x20, (uint32_t)x);
        mem_w32(&M.cpu, row + 0x08, (uint32_t)(int32_t)lrintf(oH * size));
    }
    mir_fx = size / wk;
    mir_fy = size;
    for (uint32_t r = GA(0x8018138Cu); r <= GA(0x8018140Cu); r += 0x80) {
        mem_w16(&M.cpu, r + 0x10 + 2, (uint16_t)y);
        mem_w16(&M.cpu, r + 0x10 + 10, (uint16_t)y);
        if (ow > 0) { /* each buffer's x from the row's (the second buffer's 320 on): moved with it. Snapped to the
                       * buffer's 0 or 320: when the width changed (the widescreen factor, a resolution or window change
                       * in a battle) one buffer came back 2 off and kept it, every other frame's mirror shifted out of
                       * its frame (the user's 2026-11-03, 8x to 6x) */
            int base = (int16_t)mem_r16(&M.cpu, r + 0x10) - row_x_was, obase = (int16_t)mem_r16(&M.cpu, r + 0x18) - row_x_was;
            base = (base + 160) / 320 * 320;
            obase = (obase + 160) / 320 * 320;
            mem_w16(&M.cpu, r + 0x10, (uint16_t)(base + x));
            mem_w16(&M.cpu, r + 0x10 + 4, (uint16_t)w);
            mem_w16(&M.cpu, r + 0x10 + 6, (uint16_t)h);
            mem_w16(&M.cpu, r + 0x10 + 8, (uint16_t)(obase + x));
            if (getenv("TM2PC_MIRROR_LOG"))
                fprintf(stderr, "mirror: vblank %u rec %08x base %d obase %d row x was %d now %d w %d\n", (unsigned)M.vblanks, r, base,
                        obase, row_x_was, x, w);
        }
    }
}
