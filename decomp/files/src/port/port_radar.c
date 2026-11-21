/* The PC port's radars with 3-4 players (port code: .port_text). Built only into the port variant
 * (--port-unit, with -DTM2_PORT_HOOKS); called once a frame by rtMainLoop after view 2's drawing (rt_main.c,
 * the overlay; with 3-4 players worldUpdate's own N-human radar code is off, retail's for players 1-2 runs).
 *
 * Every player's radar shows every other player and every computer car, by the game's own rule for
 * players 1-2: a car within 400 of the radar owner's height every frame, one higher or lower on 1 frame
 * in 5 (rand() % 5 == 0: a faint, twinkling dot). The pairs of players 1 and 2, and the computer cars on
 * player 1's radar (and player 2's in co-op), are the game's. Order: radar by radar, its players, then the
 * computer cars (as the host's four_radars, vplus/four.c, so the dots overlap the same way).
 * portConfig.radar_solid (tests: TM2PC_TEST_SOLID_RADAR): every dot every frame, as the play build's. */
#include "semantic/cars.h"
#include "semantic/port_hooks.h"

extern int rand(void);
extern int gameMode;
extern void hudRadarPlotCar(int type, int *delta, int angle, int radar);

#define HUMAN(i) ((struct TM2HumanRecord *)&humanCars[i])
#define AI(k) ((struct TM2AIRecord *)&aiCars[k])

static void plot(int type, int *pos, struct TM2HumanRecord *owner, int radar)
{
    int delta[3], dz;

    dz = pos[2] - owner->motion.pos[2];
    if (dz < 0)
        dz = -dz;
    if (dz >= 400 && !portConfig.radar_solid && rand() % 5 != 0)
        return;
    delta[0] = pos[0] - owner->motion.pos[0];
    delta[1] = pos[1] - owner->motion.pos[1];
    delta[2] = pos[2] - owner->motion.pos[2];
    hudRadarPlotCar(type, delta, owner->motion.rot[2], radar);
}

void portRadarPlot(void)
{
    int n = humanCarTotal < TM2_MAX_HUMANS ? humanCarTotal : TM2_MAX_HUMANS;
    int i, j, k;

    for (j = 0; j < n; j++) {
        struct TM2HumanRecord *owner = HUMAN(j);

        for (i = 0; i < n; i++) {
            if (i == j || (i < 2 && j < 2))
                continue;
            if (HUMAN(i)->stats.hp > 0)
                plot(HUMAN(i)->type, HUMAN(i)->motion.pos, owner, j);
        }
        if (j == 0 || (j == 1 && gameMode == 2))
            continue;
        for (k = 0; k < aiCarTotal && k < 9; k++)
            if (AI(k)->stats.hp > 0)
                plot(AI(k)->type, AI(k)->motion.pos, owner, j);
    }
}
