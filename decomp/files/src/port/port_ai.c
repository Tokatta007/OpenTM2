/* The PC port's own functions for the computer cars (port code: .port_text, docs/HYBRID.md in the port repo).
 * Built only into the port variant (--port-unit, with -DTM2_PORT_HOOKS). */
#include "semantic/cars.h"
#include "semantic/world.h"
#include "semantic/port_hooks.h"

extern struct TM2Object *humanCarObject(int);
extern struct TM2Object *aiCarObject(int);

/* AI vs AI, a computer car's aimed shot (weaponFindTargetAhead / Behind, mode 0): it steers at the owner's
 * own target, its target computer car (aiCarTargets) while that one is alive, else the nearest human still
 * in (distance: each axis / 16, squared, in 32 bits unsigned, as the host's), not of its team (portConfig.teams). Writes *out (the target
 * object's position) and returns 1; 0: nothing chosen, the retail code runs. */
int portAiAim(int owner, int *out, int mode)
{
    struct TM2AIRecord *me;
    int t, i, best;
    unsigned int d, best_d;

    if (!portConfig.ai_vs_ai || mode || owner < 0 || owner >= aiCarTotal)
        return 0;
    me = (struct TM2AIRecord *)&aiCars[owner];
    if (me->state.dead)
        return 0;
    t = aiCarTargets[owner];
    if (t >= 0 && t < 9) {
        if (t < aiCarTotal && !((struct TM2AIRecord *)&aiCars[t])->state.dead) {
            *out = (int)aiCarObject(t)->pos;
            return 1;
        }
        return 0;
    }
    best = -1;
    best_d = 0;
    for (i = 0; i < humanCarTotal && i < TM2_MAX_HUMANS; i++) {
        struct TM2HumanRecord *h = (struct TM2HumanRecord *)&humanCars[i];
        int dx = (h->motion.pos[0] - me->motion.pos[0]) / 16;
        int dy = (h->motion.pos[1] - me->motion.pos[1]) / 16;

        if (h->state.dead)
            continue;
        if (portConfig.teams && i < 16 && owner < 16 &&
            (i < 4 ? portConfig.team_human[i] : portConfig.team_human_more[i - 4]) == portConfig.team_ai[owner])
            continue; /* (a team game: never a teammate) */
        d = (unsigned int)dx * (unsigned int)dx + (unsigned int)dy * (unsigned int)dy;
        if (best < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    *out = (int)humanCarObject(best)->pos;
    return 1;
}
