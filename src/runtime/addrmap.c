/* Where a retail address is in the executable the port runs (docs/HYBRID.md).
 *
 * The port names the game's code and data by their retail addresses. Built from the retail executable,
 * or from the decomp's source at the retail layout, they are where they say. Built from a build at a
 * new layout (the decomp's port variant), the recompiler writes its address map (gen/addrmap.c: each
 * retail range, where it starts now) and ga() follows it. */
#include <stdint.h>
#include "runtime.h"

extern const uint32_t rc_addrmap[][3];
extern const unsigned rc_addrmap_count;

static uint32_t ga_search(uint32_t retail)
{
    unsigned lo = 0, hi = rc_addrmap_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (retail < rc_addrmap[mid][0]) hi = mid;
        else if (retail >= rc_addrmap[mid][1]) lo = mid + 1;
        else return rc_addrmap[mid][2] + (retail - rc_addrmap[mid][0]);
    }
    return retail;
}
/* The map never changes: the answers kept (a 1,024-entry cache by address). The port names game addresses through GA()
 * in its per-polygon and per-node paths (the order tables' slots, the in-between frames): the 11-step search there was
 * 5% of a 4-player match (the user's 2026-11-15 profile) */
uint32_t ga(uint32_t retail)
{
    static volatile uint64_t cache[1024]; /* (address and answer in one aligned word: a thread reading while another writes
                                           * sees a whole entry) */
    unsigned h = ((retail >> 2) * 2654435761u) >> 22;
    uint64_t e = cache[h];
    if ((uint32_t)(e >> 32) == retail && retail) return (uint32_t)e;
    uint32_t v = ga_search(retail);
    cache[h] = (uint64_t)retail << 32 | v;
    return v;
}

/* The retail start of the function a return address is in, by the build's layout: a function the variant changed may
 * be longer than retail's (its calls move past retail's end), so it ends where the next range starts now. 0: none
 * (a retail-layout build: match the retail address itself). */
uint32_t gr_function(uint32_t now)
{
    for (unsigned i = 0; i < rc_addrmap_count; i++) {
        uint32_t start = rc_addrmap[i][2], end = ga(rc_addrmap[i][1]);
        if (end <= start) end = start + (rc_addrmap[i][1] - rc_addrmap[i][0]);
        if (now >= start && now < end) return rc_addrmap[i][0];
    }
    return 0;
}

uint32_t gr(uint32_t now) /* the other way: a retail address from where it is now (return addresses, by range) */
{
    for (unsigned i = 0; i < rc_addrmap_count; i++) {
        uint32_t size = rc_addrmap[i][1] - rc_addrmap[i][0];
        if (now >= rc_addrmap[i][2] && now - rc_addrmap[i][2] < size) return rc_addrmap[i][0] + (now - rc_addrmap[i][2]);
    }
    return now;
}
