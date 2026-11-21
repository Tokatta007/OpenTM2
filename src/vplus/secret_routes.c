/* The secret arenas' computer-car routes (Suicide Swamp 10, Cyburbia 11, Roof Tops 12): the game ships them unfinished
 * (decomp SECRET_LEVELS.md 4 and 6). Each arena's loader (arenaLoadSuicideSwamp 80119F4C, arenaLoadCyburbia 80119F94,
 * arenaLoadRoofTops 80119FDC: the route PTS, then the GRP sections and tables, then the RMP) is wrapped, and after it
 * the port's fixes go into the loaded data:
 * - every route point's section byte (node +0x17): the files have 6 in all of them, so a car's own section never
 *   matched its route (spawn height, stuck recovery);
 * - Suicide Swamp: its two sections (the west field 0, the rest 1) joined through the corridor: next node and next
 *   section for 0 -> 1 (node 4, then into 1) and 1 -> 0 (node 5, then into 0); inside a section the cars chase their
 *   target straight, which suits the open field and the course (the Jet Moto course: a long strip);
 * - Cyburbia: a whole route of our own (cyburbia_route.inc, made by tools/routes/cyburbia.py): 127 points over the
 *   two street grids, the ring road and the plaza, 35 sections with their next-node / next-section tables, 15
 *   start points; the sections' ground as retail's (the plaza's box and surfaces kept, the streets as its section 1);
 * - Roof Tops: a whole route of our own (roof_tops_route.inc, made by tools/routes/roof_tops.py): 51 points with New
 *   York's way of jumping (a run-up point before the edge, the landing point past the far one), retail's 14 sections
 *   kept, our next-node / next-section tables for its 13 roofs and a few speed-table entries (jump and drop speeds);
 *   and two of New York's roof rules on Roof Tops too (below): no straight chase at a target on another roof, and
 *   braking near a roof's edge; melee cars' straight chase only at a target on their own roof (below). */
#include <stdint.h>
#include <stdio.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "cars.h"

#define ROUTE_NODES GA(0x801B1DE8u)   /* aiRouteNodes: 24 bytes a point */
#define ROUTE_COUNT GA(0x80180AD8u)   /* aiRouteNodeCount */
#define GRP_BASE GA(0x801A9848u)      /* the loaded GRP */
#define NEXT_NODE (GRP_BASE + 0x354u) /* [35][35] s8: the exit point in my section toward a target's section */
#define NEXT_SECT (GRP_BASE + 0x81Du) /* [35][35] s8: the section after mine on that way */
#define SECTIONS 35

#include "cyburbia_route.inc"
#include "roof_tops_route.inc"

static void route_bytes(const uint8_t *bytes, int n)
{
    int count = (int)mem_r32(&M.cpu, ROUTE_COUNT);
    for (int i = 0; i < n && i < count; i++) mem_w8(&M.cpu, ROUTE_NODES + 24u * (uint32_t)i + 0x17, bytes[i]);
}
static void table_set(int from, int to, int node, int section)
{
    mem_w8(&M.cpu, NEXT_NODE + (uint32_t)(from * SECTIONS + to), (uint8_t)(int8_t)node);
    mem_w8(&M.cpu, NEXT_SECT + (uint32_t)(from * SECTIONS + to), (uint8_t)(int8_t)section);
}

void F_80119F4C_orig(CPU *c);
void vp_ArenaLoadSwamp(CPU *c)
{
    F_80119F4C_orig(c);
    static const uint8_t bytes[8] = {0, 0, 0, 0, 0, 1, 1, 1};
    route_bytes(bytes, 8);
    table_set(0, 1, 4, 1); /* the field to the strip: by node 4 (its link to 5 is in section 1) */
    table_set(1, 0, 5, 0); /* back: by node 5 (its link to 4) */
}

void F_80119F94_orig(CPU *c);
void vp_ArenaLoadCyburbia(CPU *c)
{
    F_80119F94_orig(c);
    cyb_route_write();
}

void F_80119FDC_orig(CPU *c);
void vp_ArenaLoadRoofTops(CPU *c)
{
    F_80119FDC_orig(c);
    sroof_route_write();
}

/* New York's roof rules on Roof Tops too: aiShouldChaseDirect (800EBB74; New York: a straight chase only for a
 * melee special on the target's point) and aiNearSectionEdge (800F02D0; New York and Antarctica: braking within 480
 * of a section's edge) read the level; on Roof Tops they run as on New York (the level read as 5 meanwhile). */
#define LEVEL (GA(0x8016475Cu) + 0x18) /* the session's level (sessionSettings +0x18) */
static int as_new_york(void)
{
    if (mem_r32(&M.cpu, LEVEL) != 12) return 0;
    static int told;
    if (!told) told = 1, fprintf(stderr, "roof tops: New York's roof rules on\n");
    mem_w32(&M.cpu, LEVEL, 5);
    return 1;
}

/* A computer car's target's section (route group): its target car's (aiCarTargets; -1 the nearest player, as
 * aiGetTargetCar), -1 none. Melee cars (a ram or grab special) keep chasing straight once set (the New York branch
 * never clears it; a landed special sets it with no section test, aiMovementUpdate): on Roof Tops that drove them
 * off roofs after targets on others, so there it holds only while the target is on the car's own roof. */
static int target_section(uint32_t ai)
{
    uint32_t k = (ai - AI_REC(0)) / AI_REC_SIZE;
    if (ai < AI_REC(0) || k >= (uint32_t)cars_ai_max()) return -1;
    int tg = (int32_t)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * k);
    if (tg >= 0) return tg < cars_ai_max() ? (int16_t)mem_r16(&M.cpu, AI_REC(tg) + 0x46A) : -1;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), best = -1;
    double bd = 1e30, x = (int32_t)mem_r32(&M.cpu, ai + 0x73C), y = (int32_t)mem_r32(&M.cpu, ai + 0x740);
    for (int i = 0; i < humans && i < 4; i++) {
        double dx = (int32_t)mem_r32(&M.cpu, HUMAN_REC(i) + 0x6FC) - x, dy = (int32_t)mem_r32(&M.cpu, HUMAN_REC(i) + 0x700) - y;
        if (dx * dx + dy * dy < bd) bd = dx * dx + dy * dy, best = i;
    }
    return best >= 0 ? (int16_t)mem_r16(&M.cpu, HUMAN_REC(best) + 0x9C + 0x2C) : -1; /* (its car state's route group) */
}
static int target_elsewhere(uint32_t ai)
{
    int t = target_section(ai);
    return t >= 0 && t != (int16_t)mem_r16(&M.cpu, ai + 0x46A);
}
void F_800EBB74_orig(CPU *c);
void vp_AiShouldChaseDirect(CPU *c)
{
    uint32_t ai = c->r[4];
    int ny = as_new_york();
    F_800EBB74_orig(c);
    if (!ny) return;
    mem_w32(&M.cpu, LEVEL, 12);
    if (c->r[2] && target_elsewhere(ai)) {
        mem_w8(&M.cpu, ai + 0x3B, 0); /* (the straight chase) */
        c->r[2] = 0;
    }
}
void F_800E8F38_orig(CPU *c);
void vp_AiMovementUpdate(CPU *c)
{
    uint32_t ai = c->r[4];
    if (mem_r32(&M.cpu, LEVEL) == 12 && mem_r8(&M.cpu, ai + 0x458) && target_elsewhere(ai)) {
        mem_w8(&M.cpu, ai + 0x458, 0); /* a landed special's follow-through ends: its target is on another roof */
        mem_w8(&M.cpu, ai + 0x3B, 0);
    }
    void ma_ai_rank(int on); /* mods/mod_arena.c: levels past 8 as 7 for the AI's fire timers */
    static int test = -1; /* tests: TM2PC_TEST_AI_FIRE=1 counts the fire requests (and =2 without the levels as 7) */
    if (test < 0) test = getenv("TM2PC_TEST_AI_FIRE") ? atoi(getenv("TM2PC_TEST_AI_FIRE")) : 0;
    int16_t cool = (int16_t)mem_r16(&M.cpu, ai + 0x82E); /* (the weapon cooldown: cleared by the timers to fire) */
    ma_ai_rank(test != 2);
    F_800E8F38_orig(c);
    ma_ai_rank(0);
    if (test) {
        static unsigned fires, calls;
        calls++;
        fires += cool > 1 && mem_r16(&M.cpu, ai + 0x82E) == 0;
        if (calls % 6000 == 0) fprintf(stderr, "ai fire: %u requests in %u car updates\n", fires, calls);
    }
}
void F_800F02D0_orig(CPU *c);
void vp_AiNearSectionEdge(CPU *c)
{
    int ny = as_new_york();
    F_800F02D0_orig(c);
    if (ny) mem_w32(&M.cpu, LEVEL, 12);
}
