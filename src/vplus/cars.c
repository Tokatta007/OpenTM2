/* The cars of a match: counts and limits (cars.h). */
#include "../runtime/runtime.h"
#include "cars.h"
#include "../runtime/gpu_backend.h"
#include <stdlib.h>
#include <string.h>

extern const struct { const char *name; uint32_t addr; } rc_port_symbols[];
extern const unsigned rc_port_symbol_count;
uint32_t port_sym(const char *name)
{
    for (unsigned i = 0; i < rc_port_symbol_count; i++)
        if (rc_port_symbols[i].name && !strcmp(rc_port_symbols[i].name, name)) return rc_port_symbols[i].addr;
    return 0;
}

extern const struct { const char *name; uint32_t addr, size; } rc_port_functions[];
extern const unsigned rc_port_function_count;
const char *port_function_of(uint32_t addr) /* the port function (functions.tsv kind port) holding an address, or NULL */
{
    for (unsigned i = 0; i < rc_port_function_count; i++)
        if (addr - rc_port_functions[i].addr < rc_port_functions[i].size) return rc_port_functions[i].name;
    return NULL;
}

int cars_ai_max(void) /* the game's aiCars[9]; the variant's TM2_MAX_AI_CARS (12 with TM2_PORT_CARS=16) */
{
    return rc_ai_cars_max > 0 && rc_ai_cars_max <= CARS_MAX ? (int)rc_ai_cars_max : 9;
}

int cars_ai_count(void)
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)); /* aiCarTotal */
    return n < 0 ? 0 : n > cars_ai_max() ? cars_ai_max() : n;
}

/* The game's room per arena (cars, players included): its start points and memory. Request 32 step 2 (the
 * farthest-free start picker, the car DMDs in the high area) lifts it to CARS_MAX on every arena. */
int cars_arena_room(int level)
{
    static const int room[9] = {8, 11, 11, 8, 11, 11, 10, 11, 9}; /* 0: random (the smallest) */
    if (cars_ai_max() > 9) return CARS_MAX; /* request 32 step 2 in the build: every arena 16 (the types still per cars_arena_types) */
    return level >= 0 && level <= 8 ? room[level] : room[0];
}

/* Different car types whose textures fit next to the arena's (decomp SIXTEEN_CARS.md 4.4, largest first: the
 * worst order). Duplicates cost no VRAM. Per-car CLUTs (request 33 step 1: rows 240..255) raise Antarctica,
 * Amazonia and Hong Kong; texture banks (step 4) lift it. */
int cars_banks(void) /* the decomp's texture banks in the build (TM2_PORT_TEXTURE_BANKS) and in the renderer */
{
    static int has = -1;
    if (has < 0) {
        const char *e = getenv("TM2PC_TEX_BANKS");
        has = port_sym("portTexBanks") && port_sym("portTexLoadImage") && gpu_backend->banks && gpu_backend->banks() &&
              !(e && e[0] == '0');
    }
    return has;
}

int cars_arena_types(int level)
{
    if (cars_banks()) return CARS_MAX; /* each car's textures in a bank of its own: any mix on any arena */
    return cars_arena_types_vram(level);
}
int cars_arena_types_vram(int level)
{
    static const int types[9] = {8, 12, 14, 8, 12, 12, 10, 14, 9}; /* 0: random (the smallest) */
    return level >= 0 && level <= 8 ? types[level] : types[0];
}

/* Two cars of one type: the decomp's TM2_PORT_CAR_GROUPS (request 32 step 1) in the build: every car its own group
 * and key (the port picks the opponents itself; portCarOptions bit 0 only opens the game's own pickers). Without it a
 * type plays once per match (a second would share the first's tables). */
int cars_duplicates(void)
{
    static int has = -1;
    if (has < 0) has = port_sym("portCarGroups") != 0;
    return has;
}

uint32_t car_slot_object(int slot)
{
    return slot >= 0 && slot < CARS_MAX ? mem_r32(&M.cpu, GA(0x80187AD4u) + 4u * (uint32_t)slot) : 0;
}
