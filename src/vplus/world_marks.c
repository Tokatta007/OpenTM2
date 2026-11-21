/* Marks in the 3D world over each player's view (Customize HUD: enemy markers, lock-on reticle; King of the
 * Hill's hill): where a place in the world is on a view, from the view's camera as the game has it for the
 * frame, and which car a player's missiles would home on. The drawing: vplus/hud_styles.c (hudstyle_marks).
 *
 * The camera (viewProc's, view_system.c): the eye viewEyePositions[view] (the negated eye: a place plus it is
 * the place from the eye) and the rotation viewMatrices[view] (4.12); the projection's scales and the view's
 * size from its row of viewModeTable (viewModes[view]: x +0x08, y +0x0C, w +0x18, h +0x1C), its centre the
 * middle (viewGetCentre). As the game's own effects project their sprites (car_damage_human.c). */
#include <math.h>
#include <string.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "cars.h" /* HUMAN_REC */
#include "world_marks.h"

#define WM_RAM 0x80CA0000u /* free high RAM (0x80CA0000..0x80CEFFFF unused): a word, then the marks' packets (hud_styles.c) */

static double gte_ofs[4][3]; /* each view's screen offset x, y and H as its world was drawn (0: not yet) */
void wm_capture_gte(int view)
{
    if (view < 0 || view > 3) return;
    gte_ofs[view][0] = (int32_t)M.cpu.gte_ctrl[24] / 65536.0;
    gte_ofs[view][1] = (int32_t)M.cpu.gte_ctrl[25] / 65536.0;
    gte_ofs[view][2] = (uint16_t)M.cpu.gte_ctrl[26];
}
int wm_view(int view, WmView *v)
{
    if (view < 0 || view > 3) return 0;
    int mode = (int32_t)mem_r32(&M.cpu, RB_viewModes + 4u * (uint32_t)view);
    if (mode < 0 || mode > 15) return 0;
    uint32_t row = RB_viewModeTable + 0x60u * (uint32_t)mode, mat = RB_viewMatrices + 32u * (uint32_t)view;
    v->hx = (int32_t)mem_r32(&M.cpu, row + 0x08);
    v->hy = (int32_t)mem_r32(&M.cpu, row + 0x0C);
    v->cx = (int32_t)mem_r32(&M.cpu, row + 0x18) / 2;
    v->cy = (int32_t)mem_r32(&M.cpu, row + 0x1C) / 2;
    for (int i = 0; i < 3; i++) {
        v->eye[i] = (int32_t)mem_r32(&M.cpu, RB_viewEyePositions + 12u * (uint32_t)view + 4u * (uint32_t)i);
        for (int j = 0; j < 3; j++) v->m[i][j] = (int16_t)mem_r16(&M.cpu, mat + 2u * (uint32_t)(i * 3 + j)) / 4096.0;
    }
    if (gte_ofs[view][2] > 0) { /* (the GTE's own: its centre where the view's camera has it, not always the middle) */
        v->cx = gte_ofs[view][0];
        v->cy = gte_ofs[view][1];
    }
    return v->hx > 0 && v->hy > 0;
}

int wm_project(const WmView *v, double x, double y, double z, double *sx, double *sy, double *depth)
{
    double p[3] = {x + v->eye[0], y + v->eye[1], z + v->eye[2]}, o[3];
    for (int i = 0; i < 3; i++) o[i] = v->m[i][0] * p[0] + v->m[i][1] * p[1] + v->m[i][2] * p[2];
    *depth = o[2];
    if (o[2] < 16) return 0;
    *sx = v->cx + o[0] * v->hx / o[2];
    *sy = v->cy + o[1] * v->hy / o[2];
    return 1;
}

/* The lock: weaponFindTargetAhead (80109F94; a query, it only writes the target), as a missile fired now would
 * choose its target: the nearest car in the cone ahead, within 8000 units. Only while the chosen weapon is one
 * that homes (the special, fire or homing missiles: HUD slots 0-2) and has a shot left. */
int wm_lock_slot(int player)
{
    if (player < 0 || player > 3 || humans_view_human(player) >= (int)mem_r32(&M.cpu, GA(0x80181014u))) return -1;
    uint32_t rec = VIEW_REC(player);
    if (mem_r8(&M.cpu, rec + 0x9C)) return -1; /* (destroyed) */
    int sel = (int8_t)mem_r8(&M.cpu, rec + 0x7D5);
    if (sel < 0 || sel > 2 || (int16_t)mem_r16(&M.cpu, rec + 0x7D6 + 2u * (uint32_t)sel) <= 0) return -1;
    const uint32_t out = WM_RAM; /* (its answer) */
    mem_w32(&M.cpu, out, 0);
    guest_call(GA(0x80109F94u), (uint32_t)player, out, 1, 0);
    uint32_t at = mem_r32(&M.cpu, out);
    if (!at) return -1;
    double tx = (int32_t)mem_r32(&M.cpu, at), ty = (int32_t)mem_r32(&M.cpu, at + 4);
    WmCar cars[24];
    int n = vp_mark_cars(cars, 24), best = -1;
    double best_d = 400.0 * 400.0; /* (the target's object and its car's motion block: the same place, near enough) */
    for (int i = 0; i < n; i++) {
        if (cars[i].human == player) continue;
        double d = (cars[i].x - tx) * (cars[i].x - tx) + (cars[i].y - ty) * (cars[i].y - ty);
        if (d < best_d) { best_d = d; best = cars[i].slot; }
    }
    return best;
}

/* Where the cars were drawn: each view's world is drawn (renderScene) before the cars' step and its HUD after it,
 * so the marks take the cars' places as they were when the view was drawn (four.c's renderScene wrap: per view;
 * a redraw between steps, vplus/interp.c: the blended places) */
#define WM_SLOTS 64
static double drawn_pos[4][WM_SLOTS][3];
static uint64_t drawn_at[4][WM_SLOTS];
static uint64_t drawn_gen[4];
void wm_capture(int view)
{
    if (view < 0 || view > 3) return;
    WmCar cars[24];
    int n = vp_mark_cars(cars, 24);
    drawn_gen[view]++;
    for (int i = 0; i < n; i++)
        if (cars[i].slot >= 0 && cars[i].slot < WM_SLOTS) {
            drawn_pos[view][cars[i].slot][0] = cars[i].x;
            drawn_pos[view][cars[i].slot][1] = cars[i].y;
            drawn_pos[view][cars[i].slot][2] = cars[i].z;
            drawn_at[view][cars[i].slot] = drawn_gen[view];
        }
}
void wm_drawn_place(int view, WmCar *car) /* the car's place as the view was drawn (if it was there then) */
{
    if (view < 0 || view > 3 || car->slot < 0 || car->slot >= WM_SLOTS || drawn_at[view][car->slot] != drawn_gen[view]) return;
    car->x = drawn_pos[view][car->slot][0];
    car->y = drawn_pos[view][car->slot][1];
    car->z = drawn_pos[view][car->slot][2];
}
