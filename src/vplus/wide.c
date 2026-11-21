/* Widescreen battles (Hor+: the same height, more to the sides), [video] widescreen.
 * docs/RENDER_PLAN.md section 3, the decomp's docs/WIDESCREEN.md section 6.
 *
 * Anamorphic: the game keeps drawing its 320x240 frame, its cameras' x axis squeezed by k (the window's
 * aspect over 4:3), and the frame is shown stretched by k. The view-mode table's rows (their x axis scale
 * at +0x28, which viewProc reads every frame) get 1/k; their side cull planes (+0x30 / +0x38, copied into
 * a view when it is made, viewLoadMode) open to the wider edge: an edge at angle a (the plane's (-4096 cos a,
 * 4096 sin a) and (+4096 cos a, ...)) moves to atan(k tan a). The rows are patched before the game makes a
 * level's views (sessionBindCars) and put back after the session; the stretch is on while a round runs.
 * Rows 7 (the shell's scenes), 8 and 13 (the split-screen mirror, made from row 4 every frame,
 * vplus/mirrors.c) are left alone; every other row, so one view, both splits, the quarters and the
 * rear-view mirror alike. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psyz/video.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "settings.h"

#define ROW_BYTES 0x60u
static uint8_t saved[16][ROW_BYTES]; /* the rows as the game has them */
static int saved_on;
static float cur_k = 1;
static float shot_k = 1; /* the last round's k: its frames captured after it (the level menu) are as wide */

static int patched_row(int r) { return r >= 1 && r != 7 && r != 8 && r != 13 && r < RB_VIEW_ROWS && r < 16; }

float wide_factor(void) /* k: the window's aspect over 4:3 (1: off, or a window not wider than 4:3) */
{
    if (!settings_video.widescreen) return 1;
    PsyzSize s = Psyz_VideoGetDisplaySize();
    if (getenv("TM2PC_WIDE_DEBUG")) fprintf(stderr, "widescreen: window %dx%d\n", s.w, s.h);
    if (s.w <= 0 || s.h <= 0) return 1;
    float k = (float)s.w / s.h / (4.0f / 3);
    return k > 1.01f ? (k > 3 ? 3 : k) : 1;
}

void wide_rows_restore(void)
{
    if (!saved_on) return;
    for (int r = 0; r < RB_VIEW_ROWS && r < 16; r++)
        if (patched_row(r))
            for (uint32_t i = 0; i < ROW_BYTES; i++) mem_w8(&M.cpu, RB_viewModeTable + ROW_BYTES * (uint32_t)r + i, saved[r][i]);
    saved_on = 0;
    cur_k = 1;
}

static int16_t s16_at(const uint8_t *row, int o) { return (int16_t)(row[o] | row[o + 1] << 8); }

static void rows_apply(float k)
{
    if (k <= 1) { wide_rows_restore(); return; }
    if (!saved_on) {
        for (int r = 0; r < RB_VIEW_ROWS && r < 16; r++)
            for (uint32_t i = 0; i < ROW_BYTES; i++) saved[r][i] = mem_r8(&M.cpu, RB_viewModeTable + ROW_BYTES * (uint32_t)r + i);
        saved_on = 1;
    }
    for (int r = 0; r < RB_VIEW_ROWS && r < 16; r++) {
        if (!patched_row(r)) continue;
        const uint8_t *o = saved[r];
        uint32_t row = RB_viewModeTable + ROW_BYTES * (uint32_t)r;
        int16_t ax = s16_at(o, 0x28);
        if (!ax) continue; /* (an empty row) */
        mem_w16(&M.cpu, row + 0x28, (uint16_t)(int16_t)lrintf(ax / k)); /* the x axis, squeezed */
        int16_t lx = s16_at(o, 0x30), ly = s16_at(o, 0x32), rx = s16_at(o, 0x38), ry = s16_at(o, 0x3A);
        if (!(lx | ly | rx | ry)) continue;
        for (int side = 0; side < 2; side++) { /* left (x < 0), right (x > 0): the edge opened by k */
            int16_t x = side ? rx : lx, y = side ? ry : ly;
            double a = atan2(y, x < 0 ? -x : x), a2 = atan(k * tan(a));
            int16_t nx = (int16_t)lrint(4096 * cos(a2)), ny = (int16_t)lrint(4096 * sin(a2));
            mem_w16(&M.cpu, row + (side ? 0x38u : 0x30u), (uint16_t)(x < 0 ? -nx : nx));
            mem_w16(&M.cpu, row + (side ? 0x3Au : 0x32u), (uint16_t)ny);
        }
    }
    if (k != cur_k) fprintf(stderr, "widescreen: battles %.2f times as wide (%.3f:1)\n", k, 4.0f / 3 * k);
    cur_k = k;
}

void wide_rows_apply(void) { rows_apply(wide_factor()); } /* before the game makes a level's views */

static int in_round;
void wide_round(int on) /* a round runs: its frame shown stretched by k (0: as the game's) */
{
    in_round = on;
    if (on) shot_k = cur_k;
    Psyz_VideoSetGameAspect(on && cur_k > 1 ? 4.0f / 3 * cur_k : 0);
}

/* The window resized (or Widescreen switched) during a round: the rows again for the new k, and each view's side planes (its
 * camera object's m40 columns 0 and 1, which viewLoadMode copied from its row when the view was made);
 * the axis is read from the row every frame and the HUD lays itself out by wide_k() */
void wide_vblank(void)
{
    if (!in_round || M.vblanks % 15) return; /* (also Widescreen switched in the pause menu: wide_factor 1 when off) */
    float k = wide_factor();
    if (fabsf(k - cur_k) < 0.005f) return;
    rows_apply(k);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t obj = mem_r32(&M.cpu, RB_viewCameraObjects + 4 * i);
        uint32_t mode = mem_r32(&M.cpu, RB_viewModes + 4 * i);
        if (!obj || mode >= (uint32_t)RB_VIEW_ROWS) continue;
        uint32_t row = RB_viewModeTable + ROW_BYTES * mode;
        mem_w16(&M.cpu, obj + 0x40, mem_r16(&M.cpu, row + 0x30)); /* a[0] (m[0][0], m[1][0]) */
        mem_w16(&M.cpu, obj + 0x46, mem_r16(&M.cpu, row + 0x32));
        mem_w16(&M.cpu, obj + 0x42, mem_r16(&M.cpu, row + 0x38)); /* a[1] (m[0][1], m[1][1]) */
        mem_w16(&M.cpu, obj + 0x48, mem_r16(&M.cpu, row + 0x3A));
    }
    wide_round(1);
}

float wide_k(void) { return cur_k; } /* the rows' k (the HUD's squeeze, hud_layout.c) */

/* The frame as shown (the pause screen, the results, photo mode): the game's captured frame (RGB888),
 * widened by k, as the player saw it */
unsigned char *wide_capture(int *w, int *h)
{
    unsigned char *px = Psyz_VideoAllocCapturedFrame(w, h);
    if (!px || shot_k <= 1 || *w <= 1) return px;
    int sw = *w, dw = (int)lrintf((float)sw * shot_k);
    unsigned char *out = malloc((size_t)dw * *h * 3);
    if (!out) return px;
    for (int y = 0; y < *h; y++) {
        const unsigned char *src = px + (size_t)y * sw * 3;
        unsigned char *dst = out + (size_t)y * dw * 3;
        for (int x = 0; x < dw; x++) { /* linear between the two nearest columns */
            float fx = ((float)x + 0.5f) / shot_k - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            float t = fx - (float)x0;
            if (t < 0) t = 0;
            for (int c = 0; c < 3; c++)
                dst[x * 3 + c] = (unsigned char)lrintf(src[x0 * 3 + c] * (1 - t) + src[x1 * 3 + c] * t);
        }
    }
    free(px);
    *w = dw;
    return out;
}
