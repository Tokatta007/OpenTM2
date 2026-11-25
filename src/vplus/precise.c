/* No wobble (Video > Wobble, gte.c): the near-polygon pass.
 *
 * The model emitter (800CDC54) defers a polygon nearer than a threshold (the
 * scratchpad's +0x40: 70, 20 in split screen) to renderNearPolys (80154690),
 * which cuts it at screen-space midpoints from the integer coordinates: on the
 * PS1 that hides the affine texture warp up close. With perspective-correct
 * textures (wobble "none") nothing needs cutting, and cut pieces would lose
 * their precise vertices, so the threshold is 0 while the emitter runs: every
 * polygon goes to the order table whole, at its depth, as the far ones do. */
#include <stdlib.h>
#include "runtime/runtime.h"
uint64_t vp_gv(void); /* vplus/fixes.c: the gameplay clock */

#define NEAR_OTZ 0x1F800040u /* the emitter's near threshold (scratchpad) */

extern int gte_precise_on;
/* The car select's car (ui/options.c sets it around its draw): its polygons over 4 times the order-table slots. The
 * emitter's slot is the polygon's deepest SZ >> its mesh entry's shift (+0x10 of each 32-byte entry, renderWorldShift
 * 3: 8 depth units a slot); a car took only ~25 slots, so a small part and the body by it shared one and swapped as it
 * turned (Mr. Slam's rear stand flickering in the 4-player car select, the user). The walker rebuilds the entries for
 * every draw: nothing to put back. */
int vp_emit_fine;
void F_800CDC54_orig(CPU *c);
void vp_ModelEmit(CPU *c)
{
    static int off = -1;
    if (off < 0) off = getenv("TM2PC_NO_FINE_SORT") != NULL; /* (tests: as before) */
    { /* a draw entry whose mesh, vertices or primitives lie outside memory: left out (the user's 2026-11-09 co-op, Dark
       * Tooth's phase: both machines' emitter walked garbage at FDA1F150 and froze, one after the other; decomp
       * request 110 (PLAYTEST_ANSWERS.md)). Logged, so the object can be found. */
        uint32_t entries = c->r[5];
        int count = (int)c->r[6];
        static int said;
        for (int i = 0; i < count && i < 4096; i++) {
            uint32_t e = entries + 32u * (uint32_t)i, mesh = mem_r32(c, e);
            int bad = !mem_direct(c, mesh, 0x1C);
            if (!bad) {
                uint32_t verts = mem_r32(c, mesh + 4), prims = mem_r32(c, mesh + 0xC);
                bad = (verts && !mem_direct(c, verts, 8)) || (prims && !mem_direct(c, prims, 4));
                /* a mesh's primitives follow its header (+0x2C; +0x58 for the longer one), on the disc and in every test
                 * run (2026-11-25: the arenas, the cars, Minion's and Dark Tooth's phases): a pointer elsewhere, though in
                 * memory, is a damaged mesh (request 122's word written over, an absolute value), and its garbage drawn
                 * took the graphics driver down (Navi's and Terry's Minion phase, 2026-11-24). Left out and logged: the
                 * step and the mesh, to find what wrote it */
                if (!bad && prims && prims != mesh + 0x2C && prims != mesh + 0x58) {
                    static int told;
                    if (told++ < 20)
                        fprintf(stderr, "emitter: mesh %08X damaged: prims %08X (%+d, not +44 or +88), verts %08X, at VBlank %llu, "
                                "step %llu\n", mesh, prims, (int)(prims - mesh), verts, (unsigned long long)M.vblanks,
                                (unsigned long long)vp_gv());
                    bad = 1;
                }
            }
            if (!bad) continue;
            if (said++ < 20)
                fprintf(stderr, "emitter: draw entry %d of %d left out: mesh %08X (verts %08X, prims %08X), entry %08X %08X %08X\n",
                        i, count, mesh, mem_direct(c, mesh, 0x1C) ? mem_r32(c, mesh + 4) : 0,
                        mem_direct(c, mesh, 0x1C) ? mem_r32(c, mesh + 0xC) : 0, mem_r32(c, e + 0x14), mem_r32(c, e + 0x18),
                        mem_r32(c, e + 0x1C));
            for (int j = i; j + 1 < count; j++) /* (the rest moved down one) */
                for (uint32_t w = 0; w < 32; w += 4) mem_w32(c, entries + 32u * (uint32_t)j + w, mem_r32(c, entries + 32u * (uint32_t)(j + 1) + w));
            count--, i--;
        }
        c->r[6] = (uint32_t)count;
    }
    if (vp_emit_fine && !off) {
        uint32_t entries = c->r[5];
        int count = (int)c->r[6];
        for (int i = 0; i < count && i < 280; i++) {
            uint32_t at = entries + 32u * (uint32_t)i + 0x10;
            int shift = (int)mem_r32(c, at) - vp_emit_fine;
            mem_w32(c, at, (uint32_t)(shift < 0 ? 0 : shift));
        }
    }
    if (gte_precise_on < 2) { F_800CDC54_orig(c); return; }
    uint32_t near = mem_r32(c, NEAR_OTZ);
    mem_w32(c, NEAR_OTZ, 0);
    F_800CDC54_orig(c);
    mem_w32(c, NEAR_OTZ, near);
}
