/* Rear-view mirrors in split screen (Customize HUD: Rear-view mirror).
 *
 * The game has one mirror, in one view only: it is drawn through view slot
 * 1 (view mode 4: view 0's camera turned around), which split screen uses
 * for player 2. Here each player's mirror is drawn once the frame's views
 * and HUDs are on the screen, through that player's own view slot: its
 * camera turned around (as mode 4 turns it: yaw + 180 degrees, pitched up a
 * little), a mode row of our own (row 13: mode 4's, sized for the mirror),
 * and renderScene told it is a mirror (viewGetMode answers 4: the mirror's
 * shorter far distances, the flipped back-face test). Then the slot is put
 * back as it was (the camera's matrices and turn, the mode, the eye and view
 * matrix saved for the view, the FOV). One order table serves every mirror:
 * the GPU draws a list as DrawOTag hands it over. The mirror's place in the
 * view: hud_layout.c (left of the health, else under it). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "settings.h"
#include "hud_layout.h"

#define VIEW_SPLIT GA(0x801809E4u)   /* viewSplitScreenMode */
#define SLOT_VIEW0 GA(0x80180EE4u)   /* the frame record view 1 draws into */
#define SLOT_VIEW1 GA(0x80180EE8u)
#define VIEW_FOV GA(0x80180DE4u)     /* viewFov */
#define ROW_MIRROR 13            /* our row of the moved view-mode table (room for 16) */
#define MIRROR_OT 0x80DF1000u    /* 4096 entries */
#define MIRROR_REC 0x80DF5000u   /* a frame record: the mirror's DRAWENV, order table and packet cursor */
#define FRAME_REC 0x80DF5080u    /* a DRAWENV for the frame (the whole screen) */
#define FRAME_PRIMS 0x80DF5100u  /* the frame: four POLY_F4 */
#define MIRROR_W 101             /* the game's mirror (one view): 101x30 inside a 2-pixel black frame */
#define MIRROR_H 30

int four_views(void);
int four_humans(void);
void four_view_size(int player, int *w, int *h);
uint32_t four_view_rec(int k);
int vp_hud_hidden(int player);
int hudlay_adaptive(void);

static int mirror_view = -1; /* the view drawn as a mirror (viewGetMode answers 4) */
/* each player's mirror this round: their setting (0 off, 1 on, 2 small) until their rear-view button, which goes
 * Full, Small, Off and round again (the user's 2026-11-15: the three settings, not only on and off) */
static int mode_set[4], mode[4];

static int split_now(void) { return mem_r32(&M.cpu, VIEW_SPLIT) != 0 || four_views(); }
float wide_k(void); /* wide.c */
int mirror_mode(int p) /* 0 off, 1 full, 2 small: the player's this round */
{
    if (p < 0 || p > 3) return 0;
    int m = mode_set[p] ? mode[p] : hud_of(p)->mirror;
    return m >= 0 && m <= 2 ? m : 0;
}
int mirror_on(int p) { return RB_VIEW_ROWS > ROW_MIRROR && split_now() && mirror_mode(p) != 0; }
int mirror_split_mode(int p) { return mirror_on(p) ? mirror_mode(p) : 0; } /* (hud_layout.c: the split screen's size) */
int mirrors_wanted(void) /* split screen with someone's mirror on */
{
    if (RB_VIEW_ROWS <= ROW_MIRROR) return 0; /* (no row for it) */
    for (int p = 0; p < 4; p++) if (mirror_on(p)) return 1;
    return 0;
}
void mirrors_round(void) { for (int p = 0; p < 4; p++) mode_set[p] = 0; } /* each round: as the setting */
void mirrors_toggle(int p) /* the rear-view button in split screen */
{
    if (p < 0 || p > 3) return;
    int m = mirror_mode(p);
    mode[p] = m == 1 ? 2 : m == 2 ? 0 : 1; /* Full, Small, Off */
    mode_set[p] = 1;
    fprintf(stderr, "mirrors: player %d's %s\n", p + 1, mode[p] == 1 ? "full" : mode[p] == 2 ? "small" : "off");
}

void F_801562B8_orig(CPU *c);
void vp_ViewGetMode(CPU *c) /* viewGetMode(i) */
{
    if (mirror_view >= 0 && (int)c->r[4] == mirror_view) { c->r[2] = 4; return; }
    F_801562B8_orig(c);
}

/* a player's view: the game's layout it is drawn in (1 side by side, 2 one above the other) and its size */
static int player_view(int p, int *src, int *w, int *h)
{
    int n = four_views();
    if (n) {
        if (p >= n) return 0;
        four_view_size(p, w, h);
        *src = 2;
        return 1;
    }
    if (p > 1) return 0;
    int split = (int)mem_r32(&M.cpu, VIEW_SPLIT);
    *src = split == 1 ? 1 : 2;
    *w = split == 1 ? 160 : 320;
    *h = split == 1 ? 240 : 120;
    return 1;
}

static void w16(uint32_t a, int v) { mem_w16(&M.cpu, a, (uint16_t)(int16_t)v); }

static void frame_prim(uint32_t a, uint32_t next, int x0, int y0, int x1, int y1)
{
    mem_w32(&M.cpu, a, (5u << 24) | (next & 0xFFFFFF));
    mem_w32(&M.cpu, a + 4, 0x28000000u); /* POLY_F4, black */
    w16(a + 8, x0); w16(a + 10, y0);
    w16(a + 12, x1); w16(a + 14, y0);
    w16(a + 16, x0); w16(a + 18, y1);
    w16(a + 20, x1); w16(a + 22, y1);
}

static void mirror_draw(int p, int bx, int by, int x, int y, int w, int h)
{
    uint32_t cam = mem_r32(&M.cpu, RB_viewCameraObjects + 4u * (uint32_t)p);
    if (!cam) return;
    /* the slot as it is */
    uint8_t cam_saved[0x74], eye[12], mat[32];
    for (uint32_t i = 0; i < sizeof cam_saved; i++) cam_saved[i] = mem_r8(&M.cpu, cam + 0x40 + i); /* m40, m60, rotm, rot, pos */
    for (uint32_t i = 0; i < 12; i++) eye[i] = mem_r8(&M.cpu, RB_viewEyePositions + 12u * (uint32_t)p + i);
    for (uint32_t i = 0; i < 32; i++) mat[i] = mem_r8(&M.cpu, RB_viewMatrices + 32u * (uint32_t)p + i);
    uint32_t mode = mem_r32(&M.cpu, RB_viewModes + 4u * (uint32_t)p), fov = mem_r32(&M.cpu, VIEW_FOV);
    /* row 13: mode 4's, its size the mirror's (the same field of view) */
    uint32_t row = RB_viewModeTable + 0x60u * ROW_MIRROR, four = RB_viewModeTable + 0x60u * 4;
    for (uint32_t k = 0; k < 0x60; k += 4) mem_w32(&M.cpu, row + k, mem_r32(&M.cpu, four + k));
    int w4 = (int)mem_r32(&M.cpu, four + 0x18);
    if (w4 <= 0) w4 = MIRROR_W;
    mem_w32(&M.cpu, row + 0x08, (uint32_t)((int)mem_r32(&M.cpu, four + 0x08) * w / w4)); /* H */
    mem_w32(&M.cpu, row + 0x10, (uint32_t)w);
    mem_w32(&M.cpu, row + 0x14, (uint32_t)h);
    mem_w32(&M.cpu, row + 0x18, (uint32_t)w);
    mem_w32(&M.cpu, row + 0x1C, (uint32_t)h);
    mem_w32(&M.cpu, RB_viewModes + 4u * (uint32_t)p, ROW_MIRROR);
    guest_call(GA(0x80155B30u), ROW_MIRROR, (uint32_t)p, 0, 0); /* viewLoadMode: the mirror's cull planes */
    /* Mirror Height: Car (portVisual bit 9, decomp request 99): the eye on the player's roof, looking back level */
    static uint32_t mirror_eye = 1, visual = 1;
    if (mirror_eye == 1) mirror_eye = port_sym("portVisual") ? port_sym("portVisualMirrorEye") : 0;
    if (visual == 1) visual = port_sym("portVisual");
    uint32_t vis = visual ? mem_r32(&M.cpu, visual) : 0; /* (bit 9 this player's, through renderScene: it leaves
                                                           * the player's car out; put back after) */
    if (visual) mem_w32(&M.cpu, visual, hud_of(p)->mirror_height ? vis | 0x200u : vis & ~0x200u);
    int on_car = mirror_eye && guest_call(mirror_eye, (uint32_t)p, cam + 0xA8, cam + 0xA0, 0);
    if (!on_car) {
        mem_w16(&M.cpu, cam + 0xA4, (uint16_t)(mem_r16(&M.cpu, cam + 0xA4) + 0x800)); /* turned around (as mode 4) */
        mem_w16(&M.cpu, cam + 0xA0, (uint16_t)(mem_r16(&M.cpu, cam + 0xA0) - 56));
    }
    /* its frame record: view 1's, with the mirror's order table, the frame's packet cursor, its rectangle */
    uint32_t v0 = mem_r32(&M.cpu, SLOT_VIEW0), lo = mem_r32(&M.cpu, v0 + 4), hi = mem_r32(&M.cpu, v0 + 0xC), cursor = mem_r32(&M.cpu, v0 + 8);
    uint32_t recs[4] = {mem_r32(&M.cpu, SLOT_VIEW1), four_view_rec(2), four_view_rec(3), 0};
    for (int i = 0; i < 3; i++) {
        uint32_t cur = recs[i] ? mem_r32(&M.cpu, recs[i] + 8) : 0;
        if (cur > cursor && cur >= lo && cur < hi) cursor = cur;
    }
    for (uint32_t k = 0; k < 0x80; k += 4) mem_w32(&M.cpu, MIRROR_REC + k, mem_r32(&M.cpu, v0 + k));
    mem_w32(&M.cpu, MIRROR_REC, MIRROR_OT);
    mem_w32(&M.cpu, MIRROR_REC + 8, cursor);
    guest_call(GA(0x80141938u), MIRROR_OT, 4096, 0, 0);            /* ClearOTagR */
    mirror_view = p;
    guest_call(GA(0x80155E80u), (uint32_t)p, 0, 0, 0);              /* viewProc */
    guest_call(GA(0x800D8230u), MIRROR_REC, (uint32_t)p, 0, 0);     /* streaksDraw */
    guest_call(GA(0x80110BE4u), MIRROR_REC, (uint32_t)p, 0, 0);     /* fxSpritesDraw */
    guest_call(GA(0x8014B484u), MIRROR_REC, 0, 0, 0);               /* debrisDraw */
    guest_call(GA(0x80152284u), MIRROR_REC, (uint32_t)p, 0, 0);     /* renderScene */
    mirror_view = -1;
    if (visual) mem_w32(&M.cpu, visual, vis);
    /* drawn: its rectangle (cleared first, isbg, in the view's own color: the level's fog, which shows through the gaps
     * between the far road's pieces as in the view, not black), then the black frame around it over the whole screen */
    uint32_t env = MIRROR_REC + 0x10;
    w16(env + 0, bx + x); w16(env + 2, by + y); w16(env + 4, w); w16(env + 6, h);
    w16(env + 8, bx + x); w16(env + 10, by + y);
    mem_w8(&M.cpu, env + 0x18, 1);
    guest_call(GA(0x80141AA4u), env, 0, 0, 0);                      /* PutDrawEnv */
    guest_call(GA(0x80141A30u), MIRROR_OT + 4095u * 4, 0, 0, 0);    /* DrawOTag */
    for (uint32_t k = 0; k < 0x80; k += 4) mem_w32(&M.cpu, FRAME_REC + k, mem_r32(&M.cpu, MIRROR_REC + k));
    env = FRAME_REC + 0x10;
    w16(env + 0, bx); w16(env + 2, by); w16(env + 4, 320); w16(env + 6, 240);
    w16(env + 8, bx); w16(env + 10, by);
    mem_w8(&M.cpu, env + 0x18, 0);
    guest_call(GA(0x80141AA4u), env, 0, 0, 0);
    int x0 = x - 2, y0 = y - 2, x1 = x + w + 2, y1 = y + h + 2;
    frame_prim(FRAME_PRIMS, FRAME_PRIMS + 0x18, x0, y0, x1, y);
    frame_prim(FRAME_PRIMS + 0x18, FRAME_PRIMS + 0x30, x0, y + h, x1, y1);
    frame_prim(FRAME_PRIMS + 0x30, FRAME_PRIMS + 0x48, x0, y, x, y + h);
    frame_prim(FRAME_PRIMS + 0x48, 0xFFFFFF, x + w, y, x1, y + h);
    guest_call(GA(0x80141A30u), FRAME_PRIMS, 0, 0, 0);
    /* the slot back */
    for (uint32_t i = 0; i < sizeof cam_saved; i++) mem_w8(&M.cpu, cam + 0x40 + i, cam_saved[i]);
    for (uint32_t i = 0; i < 12; i++) mem_w8(&M.cpu, RB_viewEyePositions + 12u * (uint32_t)p + i, eye[i]);
    for (uint32_t i = 0; i < 32; i++) mem_w8(&M.cpu, RB_viewMatrices + 32u * (uint32_t)p + i, mat[i]);
    mem_w32(&M.cpu, RB_viewModes + 4u * (uint32_t)p, mode);
    mem_w32(&M.cpu, VIEW_FOV, fov);
}

void mirrors_present(void) /* after the last HUD of a split-screen frame: each player's mirror */
{
    if (!mirrors_wanted()) return;
    static int logged;
    uint32_t v0 = mem_r32(&M.cpu, SLOT_VIEW0);
    uint32_t row0 = RB_viewModeTable + 0x60u * mem_r32(&M.cpu, RB_viewModes);
    int bx = (int16_t)mem_r16(&M.cpu, v0 + 0x10) - (int)mem_r32(&M.cpu, row0 + 0x20);
    int by = (int16_t)mem_r16(&M.cpu, v0 + 0x12) - (int)mem_r32(&M.cpu, row0 + 0x24);
    for (int p = 0; p < 4; p++) {
        int src, w, h;
        if (!mirror_on(p) || !player_view(p, &src, &w, &h) || vp_hud_hidden(p)) continue;
        HudLayout L;
        hud_select(p); /* their HUD's size */
        float wk = wide_k(); /* widescreen: placed in the view as seen, squeezed back (as the HUD) */
        hudlay_compute(&L, src, (int)lrintf((float)w * wk), h, mirror_mode(p)); /* (1 full, 2 small) */
        if (L.mw <= 0) continue;
        L.mx /= wk;
        L.mw = (L.mw - 4) / wk + 4;
        uint32_t row = RB_viewModeTable + 0x60u * mem_r32(&M.cpu, RB_viewModes + 4u * (uint32_t)p);
        int vx = (int)mem_r32(&M.cpu, row + 0x20), vy = (int)mem_r32(&M.cpu, row + 0x24);
        int x = vx + (int)(L.mx + 0.5f) + 2, y = vy + (int)(L.my + 0.5f) + 2;
        int mw = (int)(L.mw + 0.5f) - 4, mh = (int)(L.mh + 0.5f) - 4;
        if (!logged) fprintf(stderr, "mirrors: player %d's at %d,%d %dx%d\n", p + 1, x, y, mw, mh);
        mirror_draw(p, bx, by, x, y, mw, mh);
    }
    logged = 1;
}
