/* The port's settings for its own changes in the game's C (docs/HYBRID.md, "Our changes in C").
 *
 * A build of the decomp's port variant with the port's overlay (decomp/, -DTM2_PORT_HOOKS) has a
 * settings block, portConfig (include/semantic/port_hooks.h there), at rc_port_config. The changes
 * in that build read it; here the host fills it from its settings, at every frame's start. Other
 * builds have no block (rc_port_config 0) and keep the host's wraps. */
#include "../runtime/runtime.h"

int vp_ai_vs_ai(void); /* vplus/fixes.c */
int four_humans(void); /* vplus/four.c */
int four_player1_left(void);
int ui_survival_boss(void); /* ui/options.c */
int vp_teams_for_config(uint8_t human[16], uint8_t ai[16]); /* vplus/fixes.c: a team game's teams */

#include <stdlib.h>
#include <math.h>
#include <stdio.h>

int port_config_on(void) /* the build has the block, and it is used (tests: TM2PC_NO_PORT_CONFIG=1, the host's wraps instead) */
{
    static int on = -1;
    if (on < 0) on = rc_port_config && !getenv("TM2PC_NO_PORT_CONFIG");
    return on;
}

#include "../settings.h"
#include "vplus.h" /* vp_draw_distance */
void port_config_sync(void)
{
    /* the C emitter's subpixel beam and sprite corners (decomp request 61, FAR_DISTANCE.md addendum: the pickups, their labels and the
     * trees no longer wobble with wobble smooth / none): its x sizes times the widescreen squeeze, in 1/4096 */
    static uint32_t precise_emit = 1;
    if (precise_emit == 1) precise_emit = port_sym("portPreciseEmit");
    if (precise_emit) {
        extern int gte_precise_on;
        float wide_k(void), k = wide_k();
        mem_w32(&M.cpu, precise_emit, gte_precise_on && !getenv("TM2PC_NO_PRECISE_EMIT") && !(getenv("TM2PC_NO_PRECISE_C") && strcmp(getenv("TM2PC_NO_PRECISE_C"), "0")) ? (uint32_t)lrintf(4096.0f / (k > 0 ? k : 1)) : 0u);
    }
    int netplay_in_match(void), ui_net_carsel(void); /* net/game_tm2.c, ui/options.c */
    void mod_cars_sync(int online); /* vplus/mod_cars.c: skins in car select, offline */
    mod_cars_sync(netplay_in_match() || ui_net_carsel());
    /* Draw distance Far and Farthest: past its last band a model keeps drawing that (cheapest) band, so the
     * arena, the pickups and the cars stay to the far distance (decomp FAR_LOD.md; the variant's switch) */
    if (rc_port_far_lod) mem_w32(&M.cpu, rc_port_far_lod, vp_draw_distance() > 0 && !getenv("TM2PC_NO_FAR_LOD"));
    /* the mesh queue and the packet buffer at their most (decomp FAR_DISTANCE.md; PLAYTEST_ANSWERS.md 3): at retail's
     * 280 meshes a view and 97,520 bytes a frame, a far or crowded view (16 cars, Gun Game's fire) dropped what it
     * walked last, the player's own car first (the user's: the car, its napalms and other cars vanishing). Drawing
     * only: the battle the same. (tests: TM2PC_RETAIL_CAPS=1, retail's) */
    static uint32_t mesh_cap = 1, packet_bytes = 1;
    if (mesh_cap == 1) { mesh_cap = port_sym("portMeshListCap"); packet_bytes = port_sym("portPacketBufferBytes"); }
    int caps = !getenv("TM2PC_RETAIL_CAPS");
    if (mesh_cap) mem_w32(&M.cpu, mesh_cap, caps ? 2048u : 0u);
    if (packet_bytes) mem_w32(&M.cpu, packet_bytes, caps ? 390320u : 0u); /* (request 47 revised: a 390,400-byte buffer per view, 80 of slack) */
    /* 20 steps a second in every mode, co-op too, and every step 3 ticks, the first after a load or a pause too (the
     * user's: one rate everywhere; decomp request 48 (ONLINE_ROLLBACK.md) stage 1, FIXED_TICKS: retail's co-op ran 15, and a step measured
     * up to 5 ticks after a pause or a load). (tests: TM2PC_RETAIL_TICKS=1, retail's measured ticks) */
    static uint32_t rb_options = 1, step_ticks;
    if (rb_options == 1) { rb_options = port_sym("portRollbackOptions"); step_ticks = port_sym("portStepTicks"); }
    if (rb_options) {
        uint32_t o = mem_r32(&M.cpu, rb_options) & ~0x97Du;
        /* online, on every machine: the step itself advances the effects, the level's animations (their frames hold
         * walls the cars hit: decomp request 51 (ONLINE_ROLLBACK.md)) and the animators' random numbers, not each machine's drawing
         * (ONCE_PER_STEP, ANIM_IN_STEP, VISUAL_RAND: ONLINE_ROLLBACK.md 11, 13); offline as retail */
        int netplay_in_match(void), netplay_rollback(void); /* net/game_tm2.c */
        /* ROUND_CLEAN (0x800, request 60, ONLINE_ROLLBACK.md) offline too: a projectile still flying as a match ended flew on in the next
         * one (the user's 2026-10-30: Sweet Tooth's special fired as the last match ended, in flight at the next spawn) */
        mem_w32(&M.cpu, rb_options, o | (getenv("TM2PC_RETAIL_TICKS") ? 0u : 1u) | (netplay_in_match() ? 0x1Cu : 0u) |
                                        (getenv("TM2PC_NO_ROUND_CLEAN") ? 0u : 0x800u) |
                                        (getenv("TM2PC_TEST_RERUN") || netplay_rollback() ? 0x97Du : 0u)); /* (rollback: steps run
                                         * again, the step first then the drawing, no sound or palette upload while re-run:
                                         * net/rb_state.c; tests too; ROUND_CLEAN, request 60 (ONLINE_ROLLBACK.md): no missile of an earlier
                                         * round steers in the next one's first steps, at an object each machine binds
                                         * its own way) */
        mem_w32(&M.cpu, step_ticks, 0); /* (0: 3 ticks) */
    }
    /* the step's stack filled before each step (decomp request 86, ONLINE_ROLLBACK.md 23): the step reads stack words it
     * has not written, which the drawing left (the C near pass's locals), so the battle followed the drawing, online each
     * machine's own. 0x1200 bytes (the step's deepest chain 0x11A0), filled with 0. Off for a day: the stack (6 KB) ran
     * into libsnd's key-off masks, which the fill reached too (sounds keyed off a few frames in, the user's 2026-10-30);
     * decomp request 89 put it in port RAM. (tests: TM2PC_NO_STEP_SCRUB=1 off, TM2PC_STEP_SCRUB_FILL=hex another fill) */
    static uint32_t scrub = 1, scrub_fill;
    if (scrub == 1) { scrub = port_sym("portStepScrub"); scrub_fill = port_sym("portStepScrubFill"); }
    if (scrub) {
        mem_w32(&M.cpu, scrub, getenv("TM2PC_NO_STEP_SCRUB") ? 0u : 0x1200u); /* (on again: decomp request 89 moved the
                                                                                * stack, 64 KB, off libsnd's statics) */
        mem_w32(&M.cpu, scrub_fill, getenv("TM2PC_STEP_SCRUB_FILL") ? (uint32_t)strtoul(getenv("TM2PC_STEP_SCRUB_FILL"), NULL, 16) : 0u);
    }
    /* the stack's guard (request 89: the 16 bytes under portStack, written only by an overflow): a pattern once, then
     * checked at each sync; an overflow is told once in the log */
    static uint32_t guard = 1;
    static int told;
    if (guard == 1) {
        guard = port_sym("portStackGuard");
        for (uint32_t k = 0; guard && k < 16; k += 4) mem_w32(&M.cpu, guard + k, 0x5354414Bu); /* "STAK" */
    }
    for (uint32_t k = 0; guard && !told && k < 16; k += 4)
        if (mem_r32(&M.cpu, guard + k) != 0x5354414Bu) {
            told = 1;
            fprintf(stderr, "stack: OVERFLOW, the guard under portStack written (%08X at +%u, sp %08X)\n",
                    mem_r32(&M.cpu, guard + k), k, M.cpu.r[29]);
        }
    if (!port_config_on()) return;
    mem_w8(&M.cpu, rc_port_config + PCFG_AI_VS_AI, vp_ai_vs_ai() ? 1 : 0);
    int four_vn(void); /* vplus/four.c: the views on this machine past 2 (offline: the players; online: this machine's) */
    mem_w8(&M.cpu, rc_port_config + PCFG_HUMANS, (uint8_t)four_vn());
    mem_w8(&M.cpu, rc_port_config + PCFG_LEFT, (uint8_t)four_player1_left());
    mem_w8(&M.cpu, rc_port_config + PCFG_RADAR_SOLID, getenv("TM2PC_TEST_SOLID_RADAR") ? 1 : 0); /* tests */
    mem_w8(&M.cpu, rc_port_config + PCFG_BOSS_ANYWHERE, ui_survival_boss() ? 1 : 0);
    uint8_t th[16] = {0}, ta[16] = {0};
    int teams = vp_teams_for_config(th, ta);
    mem_w8(&M.cpu, rc_port_config + PCFG_TEAMS, (uint8_t)teams);
    for (int i = 0; i < 4; i++) mem_w8(&M.cpu, rc_port_config + PCFG_TEAMS + 1 + (uint32_t)i, th[i]);
    for (int k = 0; k < 16; k++) mem_w8(&M.cpu, rc_port_config + PCFG_TEAMS + 5 + (uint32_t)k, ta[k]);
    for (int i = 4; i < 16; i++) mem_w8(&M.cpu, rc_port_config + 0x1Bu + (uint32_t)(i - 4), th[i]); /* +0x1B: humans 5-16's */
    extern int gte_precise_on;
    mem_w8(&M.cpu, rc_port_config + PCFG_PRECISE, gte_precise_on && !(getenv("TM2PC_NO_PRECISE_C") && strcmp(getenv("TM2PC_NO_PRECISE_C"), "0")) ? 1 : 0); /* (tests: as the plain build) */
}

/* Subpixel places the variant's C computed for vertices it projects without the GTE (effect sprites: decomp/
 * overlay, PORT_PRECISE_PUT): taken into the precise table (gte.c) before a polygon is drawn. A place more than
 * 4 pixels from the vertex's word (an overflow in the C's arithmetic) is not used. */
#define PRECISE_RING 0x80CB0000u /* (free high RAM to 0x80CEFFFF: 16,380 entries, port_hooks.h's PORT_PRECISE_MAX; at 4,000
                                   * Holland's field at Farthest, 12,288 beam corners, fell back to whole pixels) */
#define PRECISE_MAX 16380u
void gte_precise_put(uint32_t ea, uint32_t word, float x, float y, float w);
/* ui/options.c, as car select's car is built again (in between frames): the places left from a build not drawn are
 * of packet words this build writes again (a sprite's corner on a model's vertex: Sweet Tooth's roof spike) */
void port_precise_ring_drop(void)
{
    if (port_config_on()) mem_w32(&M.cpu, PRECISE_RING, 0);
}
void port_precise_ring(void)
{
    if (!port_config_on()) return;
    uint32_t n = mem_r32(&M.cpu, PRECISE_RING);
    if (!n) return;
    static uint32_t emit_word = 1;
    if (emit_word == 1) emit_word = port_sym("portPreciseEmit");
    int emitting = emit_word && mem_r32(&M.cpu, emit_word) != 0;
    static int ring_log = -1; /* tests: TM2PC_PRECISE_RING_LOG, the entries now and then */
    if (ring_log < 0) ring_log = getenv("TM2PC_PRECISE_RING_LOG") != NULL;
    static unsigned long long ring_total, ring_shown;
    ring_total += n;
    if (ring_log && M.vblanks >= ring_shown + 60) {
        ring_shown = M.vblanks;
        fprintf(stderr, "precise ring: %llu entries in all at vblank %llu\n", ring_total, (unsigned long long)M.vblanks);
    }
    static uint32_t peak;
    if (n > peak) {
        peak = n;
        if (ring_log || n >= PRECISE_MAX) fprintf(stderr, "precise ring: %u entries at once (vblank %llu)%s\n", n,
                                                  (unsigned long long)M.vblanks, n >= PRECISE_MAX ? ": full (beams past it drawn rough)" : "");
    }
    if (n > PRECISE_MAX) n = PRECISE_MAX;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = PRECISE_RING + 4 + 16 * i, addr = mem_r32(&M.cpu, e), word = mem_r32(&M.cpu, addr);
        float x = (float)(int32_t)mem_r32(&M.cpu, e + 4) / 16, y = (float)(int32_t)mem_r32(&M.cpu, e + 8) / 16;
        int32_t z = (int32_t)mem_r32(&M.cpu, e + 12);
        float dx = x - (float)(int16_t)word, dy = y - (float)(int16_t)(word >> 16);
        float lim = emitting ? 600 : 4; /* (the emitter's: its exact sizes, squeezed by the widescreen's k, may differ much
                                         * from the whole-pixel ones; an overflow is far beyond) */
        if (ring_log && (dx > 8 || dx < -8 || dy > 8 || dy < -8))
            fprintf(stderr, "precise ring far: v%llu %d,%d -> %.1f,%.1f z %d%s\n", (unsigned long long)M.vblanks, (int16_t)word,
                    (int16_t)(word >> 16), x, y, z, emitting ? " emitting" : "");
        if (dx > lim || dx < -lim || dy > lim || dy < -lim) continue; /* (a turned sprite's whole-pixel size moves its corners up to ~3) */
        gte_precise_put(addr, word, x, y, z > 0 ? (float)z : 1);
    }
    mem_w32(&M.cpu, PRECISE_RING, 0);
}

/* Is the ordering table at madr (a DrawOTag's start) a view's 3D: the game's four (renderGetOrderingTable's, 0x4000
 * bytes each), the in-between frames' (vplus/interp.c), views 3-4's (four.c) or the split-screen mirrors'
 * (mirrors.c)? The HUDs have their own. */
/* The slot of a view's 3D order-table entry at addr (its index, 0..4095), or -1 for a packet or anything else. */
static uint32_t ot_base(void) /* the game's order tables (a build constant: GA's search once, not per order-table node) */
{
    static uint32_t base = 1;
    if (base == 1) base = GA(0x801E5A8Cu) & 0xFFFFFFu;
    return base;
}
int port_ot_slot(uint32_t addr)
{
    uint32_t a = addr & 0xFFFFFFu, base = ot_base();
    if (a >= base && a < base + 0x10000) return (int)((a - base) & 0x3FFF) >> 2;
    if ((a >= 0xC00000 && a < 0xC14000) || (a >= 0xDC0000 && a < 0xDD0000)) return (int)(a & 0x3FFF) >> 2;
    if (a >= 0xDF1000 && a < 0xDF5000) return (int)(a - 0xDF1000) >> 2; /* (the split-screen mirrors', mirrors.c) */
    return -1;
}

int port_ot_is_world(uint32_t madr)
{
    uint32_t a = madr & 0xFFFFFFu, base = ot_base(); /* (16 MB: interp.c's HI_OT, four.c's OT_BASE) */
    return (a >= base && a < base + 0x10000) || (a >= 0xC00000 && a < 0xC14000) || (a >= 0xDC0000 && a < 0xDD0000) ||
           (a >= 0xDF1000 && a < 0xDF5000);
}
