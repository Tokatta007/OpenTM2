/* The Versus Tournament's podium (the user's 2026-11-07, after Mario Kart 64's and Unreal Tournament's): after the
 * last stage, a scene on New York, the top three cars on a podium of three blocks (hud_styles.c draws them), a few
 * camera shots with honorable mentions, then the final standings.
 *
 * The place (the user's pick, 2026-11-07): New York's east roof (floor 7400), near its north edge, the front facing
 * south: from the front the city's skyline is behind 2nd place and the Statue of Liberty behind 3rd.
 *
 * The scene (ui/options.c runs it after the podium round, a New York round of the three that ends at once): nothing
 * of the game runs. Each frame the world is drawn again as photo mode draws it (ui/photo.c: view 1 full screen, the
 * machine's RAM kept and put back after), from the shot's camera, the three cars posed on their blocks first
 * (portCarPose, decomp request 108; without it they stay where the round started them). So every machine shows the
 * same scene and the game's state never changes.
 *
 * Tests: TM2PC_TEST_PODIUM=1 draws the podium in a New York match; TM2PC_TEST_PODIUM_AT=x,y,yaw (yaw in degrees, the
 * front facing that way; its floor found under it) anywhere. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "vplus.h"
#include "podium.h"
#include "cars.h"

#define PODIUM_X 7191.0 /* (centered on the roof edge's guardrail behind it: the user's 2026-11-11, then 16 units right, measured) */
#define PODIUM_Y 3100.0
#define PODIUM_YAW_DEG 180.0    /* the front toward -y (south) */
static const double BLOCK_H0[3] = {PODIUM_BLOCK_H1, PODIUM_BLOCK_H2, PODIUM_BLOCK_H3}, BLOCK_OFF[3] = {0, -1, 1};
/* The podium's size for its cast: the widest car's footprint from its stats (the wheels' track, with its body over
 * them: about 1.3 x the track) with room around it. Everything (width, depth, heights, the cameras) grows by one
 * factor, so the blocks keep podium.h's near-square shape (the user's 2026-11-08: grown by the wheelbase too, Dark
 * Tooth's blocks were four times as deep as wide; the stats' axles are not the body's length). The sizes of podium.h
 * for every car but the biggest (Dark Tooth: about 1.7 x). */
static double kw = 1, kd = 1, kc = 1;
static int sized;
double podium_block_w(void) { return PODIUM_BLOCK_W * kw; }
double podium_block_d(void) { return PODIUM_BLOCK_D * kd; }
double podium_block_h(int place) { return place >= 0 && place < 3 ? BLOCK_H0[place] * kw : 0; }
static void cast_stats(int p, int *track, int *front, int *rear); /* (below) */
static void podium_size(void)
{
    double need_w = 0;
    for (int p = 0; p < 3; p++) {
        int track, front, rear;
        cast_stats(p, &track, &front, &rear);
        if (track <= 0) continue;
        double w = track * 1.3 * 1.3;
        if (w > need_w) need_w = w;
    }
    kw = kd = kc = need_w > PODIUM_BLOCK_W ? need_w / PODIUM_BLOCK_W : 1;
    sized = 1;
    if (kw > 1) fprintf(stderr, "podium: grown for its biggest car: %.0f wide, %.0f deep (x%.2f)\n",
                        podium_block_w(), podium_block_d(), kw);
}

#define POS_BUF 0x80DF6A00u /* a position for worldGroundHeightAt (high RAM) */
static double ground_at(double x, double y, double z) /* the floor under (x, y), from height z down (800EF340) */
{
    mem_w32(&M.cpu, POS_BUF, (uint32_t)(int32_t)x);
    mem_w32(&M.cpu, POS_BUF + 4, (uint32_t)(int32_t)y);
    mem_w32(&M.cpu, POS_BUF + 8, (uint32_t)(int32_t)z);
    return (int32_t)guest_call(GA(0x800EF340u), POS_BUF, 0, 0, 0);
}

static int spot_on, scene_on;
static double probe_top = 12000; /* (the floor under the spot: looked for from here down) */
static int lineup_n, lineup_kind[32], lineup_index[32]; /* tools: the line-up (below) */
static double lineup_width; /* (from the first pose: the camera's) */
static void lineup_cam(double *k, double *hz, double *lz, double *side); /* (below) */
static double spot[4]; /* x, y, z (its floor), yaw (radians) */

int podium_spot(double *x, double *y, double *z, double *yaw)
{
    static int read;
    if (!read || (scene_on && !spot_on)) {
        read = 1;
        double sx, sy, deg, top = 12000;
        if (getenv("TM2PC_TEST_PODIUM_AT") && !strcmp(getenv("TM2PC_TEST_PODIUM_AT"), "car") && scene_on) {
            /* tests: where player 1's car is (the round's start point: any arena), facing its way */
            spot[0] = (int32_t)mem_r32(&M.cpu, RB_humanCars + 0x6DC + 0x20);
            spot[1] = (int32_t)mem_r32(&M.cpu, RB_humanCars + 0x6DC + 0x24);
            spot[3] = (((int32_t)mem_r32(&M.cpu, RB_humanCars + 0x710) & 4095) * 2 * M_PI / 4096);
            spot_on = 2;
        } else if (getenv("TM2PC_TEST_PODIUM_AT") && sscanf(getenv("TM2PC_TEST_PODIUM_AT"), "%lf,%lf,%lf,%lf", &sx, &sy, &deg, &top) >= 3) {
            spot[0] = sx, spot[1] = sy, spot[3] = deg * M_PI / 180; /* (a 4th: the floor looked for from that height
                                                                      * down: a street under a roof or a bridge) */
            probe_top = top;
            spot_on = 2; /* (its floor found at the first frame drawn: the arena loaded) */
        } else if (scene_on || (getenv("TM2PC_TEST_PODIUM") && mem_r8(&M.cpu, GA(0x8016475Cu) + 24) == PODIUM_LEVEL)) {
            spot[0] = PODIUM_X, spot[1] = PODIUM_Y, spot[3] = PODIUM_YAW_DEG * M_PI / 180;
            spot_on = 2;
        } else read = 0; /* (not yet New York: asked again) */
    }
    if (!spot_on || (!scene_on && !getenv("TM2PC_TEST_PODIUM") && !getenv("TM2PC_TEST_PODIUM_AT"))) return 0;
    if (spot_on == 2) {
        spot[2] = ground_at(spot[0], spot[1], probe_top);
        spot_on = 1;
        fprintf(stderr, "podium: at %.0f %.0f, floor %.0f, facing %.0f degrees\n", spot[0], spot[1], spot[2], spot[3] * 180 / M_PI);
        int mstep = getenv("TM2PC_TEST_PODIUM_MAP") ? atoi(getenv("TM2PC_TEST_PODIUM_MAP")) : 0;
        if (mstep) /* tests: the floor round it, a letter per step units (rows +y down): 1 = 100 near (the spot's floor
                    * #, higher ^, lower .); more = that step, by height (. under 0, # ground, + up to 400, ^ higher) */
            for (int yy = -20 * (mstep > 1 ? mstep : 100); yy <= 20 * (mstep > 1 ? mstep : 100); yy += mstep > 1 ? mstep : 100) {
                char row[64];
                int k = 0, st = mstep > 1 ? mstep : 100;
                for (int xx = -25 * st; xx <= 25 * st && k < 62; xx += st, k++) {
                    double z = ground_at(spot[0] + xx, spot[1] + yy, mstep > 1 ? 12000 : spot[2] + 300), g = z - spot[2];
                    row[k] = xx == 0 && yy == 0 ? '@'
                           : mstep > 1 ? (z < -50 ? '.' : z <= 50 ? '#' : z <= 400 ? '+' : '^')
                                       : (g > 8 ? '^' : g < -8 ? '.' : '#');
                }
                row[k] = 0;
                fprintf(stderr, "podium map %+5d: %s\n", yy, row);
            }
    }
    *x = spot[0], *y = spot[1], *z = spot[2], *yaw = spot[3];
    return 1;
}

/* a block's top middle: place 0 1st, 1 2nd, 2 3rd */
static void block_top(int place, double out[3])
{
    double n[2] = {sin(spot[3]), cos(spot[3])}, r[2] = {-cos(spot[3]), sin(spot[3])};
    (void)n;
    out[0] = spot[0] + r[0] * podium_block_w() * BLOCK_OFF[place];
    out[1] = spot[1] + r[1] * podium_block_w() * BLOCK_OFF[place];
    out[2] = spot[2] + podium_block_h(place);
}

/* ---- the shots: (u: 0..1 through the shot) the eye and what it looks at ---- */
/* seconds, each shot's end (the user's 2026-11-11: about twice as long; far and slowly in, the honorable mentions over
 * the first three, the wide shot, then the winner: their title and the roar, the results after) */
static const double SHOT_END[PODIUM_SHOTS] = {12.0, 19.0, 26.0, 31.0, 36.0};
double podium_scene_seconds(void) { return SHOT_END[PODIUM_SHOTS - 1]; }
double podium_shot_start(int s) { return s <= 0 ? 0 : SHOT_END[(s > PODIUM_SHOTS ? PODIUM_SHOTS : s) - 1]; }
int podium_shot_at(double t, double *u)
{
    int s = 0;
    while (s + 1 < PODIUM_SHOTS && t >= SHOT_END[s]) s++;
    double t0 = s ? SHOT_END[s - 1] : 0, f = (t - t0) / (SHOT_END[s] - t0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    if (u) *u = f * f * (3 - 2 * f); /* (eased) */
    return s;
}
static void shot_camera(int s, double u, double eye[3], double look[3])
{
    double n[3] = {sin(spot[3]), cos(spot[3]), 0}, r[3] = {-cos(spot[3]), sin(spot[3]), 0}, P[3] = {spot[0], spot[1], spot[2]};
    double c[3];
    if (lineup_n) { /* tools: the line-up, one wide shot (TM2PC_TEST_LINEUP_CAM=distance,height,look height,sideways:
                     * the distance in line-up widths, the others in units) */
        double k, hz, lz, side, wd = lineup_width > 0 ? lineup_width : lineup_n * 120.0;
        lineup_cam(&k, &hz, &lz, &side);
        for (int e = 0; e < 2; e++) eye[e] = P[e] + n[e] * wd * k + r[e] * side, look[e] = P[e] + r[e] * side;
        eye[2] = P[2] + hz, look[2] = P[2] + lz;
        (void)u;
        return;
    }
    switch (s) {
    case 0: /* far away, slowly coming in to the whole podium from the front */
        for (int e = 0; e < 3; e++) {
            eye[e] = P[e] + (n[e] * (2400 - 1650 * u) - r[e] * 260 * (1 - u)) * kc;
            look[e] = P[e];
        }
        eye[2] = P[2] + (700 - 460 * u) * kc, look[2] = P[2] + 70 * kc;
        break;
    case 1: /* 3rd place, from the front right, turning toward its front */
    case 2: { /* 2nd place, from the front left */
        int place = s == 1 ? 2 : 1;
        double side = s == 1 ? 1 : -1, a = side * (0.75 - 0.45 * u);
        block_top(place, c);
        for (int e = 0; e < 2; e++) {
            eye[e] = c[e] + (n[e] * cos(a) + r[e] * sin(a)) * (300 - 30 * u) * kc;
            look[e] = c[e];
        }
        eye[2] = c[2] + 70 * kc, look[2] = c[2] + 20 * kc;
        break;
    }
    case 3: /* wide, the skyline and the statue behind; 1st place (P) centered, coming in */
        for (int e = 0; e < 2; e++) {
            eye[e] = P[e] + n[e] * (1050 - 350 * u) * kc;
            look[e] = P[e];
        }
        eye[2] = P[2] + (420 - 160 * u) * kc, look[2] = P[2] + 90 * kc;
        break;
    default: /* the last: the winner, low in front, rising (their title and the roar over it) */
        block_top(0, c);
        for (int e = 0; e < 2; e++) eye[e] = c[e] + n[e] * (400 - 90 * u) * kc, look[e] = c[e];
        eye[2] = c[2] + (15 + 75 * u) * kc, look[2] = c[2] + 30 * kc; /* (from just over the block's top: the car whole) */
        break;
    }
}

/* ---- the cast: who stands where (kind 0 a human, 1 a computer car's record; -1 none) ---- */
static int cast_kind[3] = {-1, -1, -1}, cast_index[3];
void podium_cast(int place, int kind, int index)
{
    if (place < 0 || place > 2) return;
    cast_kind[place] = kind, cast_index[place] = index;
    sized = 0;
}
static uint32_t cast_stats_at(int p) /* the place's car's stats block (its record's), 0 none */
{
    if (cast_kind[p] < 0) return 0;
    return cast_kind[p] ? AI_REC(cast_index[p]) + 0x4FC : RB_humanCars + HUMAN_REC_SIZE * (uint32_t)cast_index[p] + 0x14C;
}
static void cast_stats(int p, int *track, int *front, int *rear)
{
    uint32_t st = cast_stats_at(p);
    *track = st ? (int16_t)mem_r16(&M.cpu, st + 0x48) : 0;
    *front = st ? (int16_t)mem_r16(&M.cpu, st + 0x4E) : 0;
    *rear = st ? (int16_t)mem_r16(&M.cpu, st + 0x50) : 0;
}
/* How far over the block's top a car stands beyond its ride height, by its type (0x10..0xF0, /16): the big-wheeled
 * cars' tires reach below the ride height the physics keeps them at (on the ground the game's own floor hides it; on
 * a block the depth buffer showed them sunk in: the user's 2026-11-08). TM2PC_TEST_PODIUM_LIFT=type:lift,... tunes. */
static uint32_t car_rec(int kind, int index) { return kind ? AI_REC(index) : RB_humanCars + HUMAN_REC_SIZE * (uint32_t)index; }
static int car_type(int kind, int index) { return (int32_t)mem_r32(&M.cpu, car_rec(kind, index) + (kind ? 0x30u : 0x90u)); }
static uint32_t car_object(int kind, int index)
{
    return mem_r32(&M.cpu, (kind ? GA(0x80188B38u) : RB_humanCarObjects) + 4u * (uint32_t)index); /* (aiCarObjects) */
}
static int car_lift(int kind, int index)
{
    static int lift[16] = {0, 0, 20, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* 0x20 Hammerhead, 0x30 Outlaw 2 (the user's
                                                                              * 2026-11-11); 0xC0 Mr. Slam none (its 24
                                                                              * floated it over the block, the user's
                                                                              * 2026-11-15 Versus podium) */
    static int read;
    if (!read && getenv("TM2PC_TEST_PODIUM_LIFT")) {
        const char *s = getenv("TM2PC_TEST_PODIUM_LIFT");
        int t, l, n;
        while (sscanf(s, "%x:%d%n", &t, &l, &n) == 2) {
            if (t >= 0x10 && t <= 0xF0) lift[t >> 4] = l;
            s += n;
            if (*s == ',') s++;
        }
    }
    read = 1;
    if (kind != 1) return 0; /* (a player's car none: the lifts were set with computer cars on the blocks, a player's car
                              * rides higher, Hammerhead and Mr. Slam floated over theirs: the user's 2026-11-15 and
                              * 2026-11-17 Versus podiums) */
    int type = car_type(kind, index);
    return type >= 0x10 && type <= 0xF0 ? lift[type >> 4] : 0;
}
/* How far forward a car stands of its place, by its type (0x10..0xF0, /16): a model drawn back from its origin (Outlaw
 * 2's body about 45 units behind its axles' middle: its rear wheel off the block's back edge, the user's 2026-11-11).
 * TM2PC_TEST_PODIUM_FWD=type:units,... tunes. */
static int car_fwd(int kind, int index)
{
    static int fwd[16] = {0, 0, 0, 45, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* 0x30 Outlaw 2 */
    static int read;
    if (!read && getenv("TM2PC_TEST_PODIUM_FWD")) {
        const char *s = getenv("TM2PC_TEST_PODIUM_FWD");
        int t, l, n;
        while (sscanf(s, "%x:%d%n", &t, &l, &n) == 2) {
            if (t >= 0x10 && t <= 0xF0) fwd[t >> 4] = l;
            s += n;
            if (*s == ',') s++;
        }
    }
    read = 1;
    if (kind < 0) return 0;
    int type = car_type(kind, index);
    return type >= 0x10 && type <= 0xF0 ? fwd[type >> 4] : 0;
}
/* every car and its shadow this far over the block's top: the shadow lay on the top's own plane and the two fought over
 * the depth, parts of it flickering as the camera moved (Axel's, the user's 2026-11-11) */
enum { SHADOW_UP = 3 };
static int cast_lift(int p) { return cast_kind[p] < 0 ? 0 : car_lift(cast_kind[p], cast_index[p]); }

/* ---- tools: the line-up (the art's banner) ---- */
void podium_lineup_add(int kind, int index)
{
    if (lineup_n < 32) lineup_kind[lineup_n] = kind, lineup_index[lineup_n++] = index;
}
int podium_lineup_on(void) { return lineup_n > 0; }
int podium_lineup_floor(double *half_w, double *front, double *back)
{
    if (!lineup_n || !getenv("TM2PC_TEST_LINEUP_FLOOR") || lineup_width <= 0) return 0;
    double k, hz, lz, side;
    lineup_cam(&k, &hz, &lz, &side);
    *half_w = lineup_width * (0.5 + k) + 400, *front = lineup_width * k + 100, *back = atof(getenv("TM2PC_TEST_LINEUP_FLOOR"));
    return 1;
}
static int type_number(int type) /* a car type's car number (0 Hammerhead .. 14 Dark Tooth) */
{
    static const int number[16] = {99, 12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14};
    return type >= 0x10 && type <= 0xF0 ? number[type >> 4] : 99;
}
static int select_rank(int type) /* its place in car select (ui/options.c grid_car, the secret cars last; Dark Tooth after) */
{
    static const int order[15] = {7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 12, 13, 14};
    int number = type_number(type);
    for (int i = 0; i < 15; i++) if (order[i] == number) return i;
    return 99;
}
/* the line-up's camera: TM2PC_TEST_LINEUP_CAM=distance (in line-up widths),height,look height,sideways */
static void lineup_cam(double *k, double *hz, double *lz, double *side)
{
    *k = 0.62, *hz = 160, *lz = 40, *side = 0;
    if (getenv("TM2PC_TEST_LINEUP_CAM")) sscanf(getenv("TM2PC_TEST_LINEUP_CAM"), "%lf,%lf,%lf,%lf", k, hz, lz, side);
}
/* extra room before a car in the line-up, by its type (its body longer than its stats say: turned, Minion's tank came
 * up against Sweet Tooth, the user's 2026-11-08). TM2PC_TEST_LINEUP_ROOM=type:units,... tunes */
static double lineup_room(int type)
{
    static int room[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 70, 0}; /* 0xE0 Minion */
    static int read;
    if (!read && getenv("TM2PC_TEST_LINEUP_ROOM")) {
        const char *s = getenv("TM2PC_TEST_LINEUP_ROOM");
        int t, l, n;
        while (sscanf(s, "%x:%d%n", &t, &l, &n) == 2) {
            if (t >= 0x10 && t <= 0xF0) room[t >> 4] = l;
            s += n;
            if (*s == ',') s++;
        }
    }
    read = 1;
    return type >= 0x10 && type <= 0xF0 ? room[type >> 4] : 0;
}
static void pose_lineup(uint32_t pose, int yaw)
{
    int order[32], n = lineup_n;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) /* (car select's order) */
        for (int j = i; j > 0 && select_rank(car_type(lineup_kind[order[j]], lineup_index[order[j]])) <
                                   select_rank(car_type(lineup_kind[order[j - 1]], lineup_index[order[j - 1]])); j--) {
            int t = order[j]; order[j] = order[j - 1], order[j - 1] = t;
        }
    double gap = getenv("TM2PC_TEST_LINEUP_GAP") ? atof(getenv("TM2PC_TEST_LINEUP_GAP")) : 40, w[32], total = 0;
    for (int i = 0; i < n; i++) {
        uint32_t st = car_rec(lineup_kind[order[i]], lineup_index[order[i]]) + (lineup_kind[order[i]] ? 0x4FCu : 0x14Cu);
        int track = (int16_t)mem_r16(&M.cpu, st + 0x48);
        w[i] = (track > 50 ? track : 50) * 1.45;
        total += w[i] + (i ? gap + lineup_room(car_type(lineup_kind[order[i]], lineup_index[order[i]])) : 0);
    }
    lineup_width = total;
    static int starts_logged;
    if (!starts_logged++ && getenv("TM2PC_TEST_PODIUM_MAP")) /* tests: where the round put each car (the arena's start points) */
        for (int i = 0; i < n; i++) {
            uint32_t mp = car_rec(lineup_kind[i], lineup_index[i]) + (lineup_kind[i] ? 0x0u : 0x6DCu + 0x20u);
            if (lineup_kind[i]) continue; /* (the computer cars' motion offset: logged below by their objects) */
            fprintf(stderr, "podium start: kind 0 record %d at %d %d %d\n", lineup_index[i], (int32_t)mem_r32(&M.cpu, mp),
                    (int32_t)mem_r32(&M.cpu, mp + 4), (int32_t)mem_r32(&M.cpu, mp + 8));
        }
    if (starts_logged == 1 && getenv("TM2PC_TEST_PODIUM_MAP"))
        for (int i = 0; i < n; i++) {
            uint32_t o = car_object(lineup_kind[i], lineup_index[i]);
            if (o < 0x80000000u || o >= 0x80000000u + GUEST_RAM_BYTES) continue;
            fprintf(stderr, "podium start: kind %d record %d object at %d %d %d\n", lineup_kind[i], lineup_index[i],
                    (int32_t)mem_r32(&M.cpu, o + 0xA8), (int32_t)mem_r32(&M.cpu, o + 0xAC), (int32_t)mem_r32(&M.cpu, o + 0xB0));
        }
    double r[2] = {-cos(spot[3]), sin(spot[3])}, at = -total / 2;
    double turn = getenv("TM2PC_TEST_LINEUP_YAW") ? atof(getenv("TM2PC_TEST_LINEUP_YAW")) : 0; /* (degrees: each car turned) */
    double ck, chz, clz, cside;
    lineup_cam(&ck, &chz, &clz, &cside);
    double dist = total * ck;
    static int logged;
    for (int i = 0; i < n; i++) {
        int k = lineup_kind[order[i]], idx = lineup_index[order[i]];
        if (i) at += lineup_room(car_type(k, idx));
        double off = at + w[i] / 2; /* (its middle along the line) */
        at += w[i] + gap;
        /* each car turned the same to the camera (the same view of each: one heading for all looked turned more at
         * one end than the other, the user's 2026-11-08): its own line of sight's angle added */
        double a = (double)yaw * 2 * M_PI / 4096 - atan2(cside - off, dist) + turn * M_PI / 180;
        /* its wheelbase's middle on the line (the stats' axles are not even about the car's origin) */
        uint32_t st = car_rec(k, idx) + (k ? 0x4FCu : 0x14Cu);
        double mid = ((int16_t)mem_r16(&M.cpu, st + 0x4E) + (int16_t)mem_r16(&M.cpu, st + 0x50)) / 2.0;
        double x = spot[0] + r[0] * off - sin(a) * mid, y = spot[1] + r[1] * off - cos(a) * mid;
        int ya = (int)lrint(a * 4096 / (2 * M_PI)) & 4095;
        guest_call7(pose, (uint32_t)k, (uint32_t)idx, (uint32_t)(int32_t)lrint(x), (uint32_t)(int32_t)lrint(y),
                    (uint32_t)(int32_t)lrint(spot[2]) + (uint32_t)car_lift(k, idx), (uint32_t)ya, 0);
        if (!logged) fprintf(stderr, "podium line-up: %d (kind %d, record %d, car %d) at %.0f %.0f, heading %d\n", i + 1, k, idx,
                             type_number(car_type(k, idx)), x, y, ya);
    }
    logged = 1;
    for (int kind = 0; kind < 2; kind++) /* (shown: the line-up's; the others hidden) */
        for (int i = 0; i < 16; i++) {
            uint32_t o = car_object(kind, i), in = 0;
            if (o < 0x80000000u || o >= 0x80000000u + GUEST_RAM_BYTES) continue;
            for (int j = 0; j < n; j++) in |= car_object(lineup_kind[j], lineup_index[j]) == o;
            mem_w32(&M.cpu, o + 8, in);
        }
}

static void pose_cars(void)
{
    static uint32_t pose = 1;
    if (pose == 1) {
        pose = port_sym("portCarPose"); /* (decomp request 108) */
        if (!pose) fprintf(stderr, "podium: no portCarPose in this link (decomp request 108): the cars stay where they started\n");
    }
    if (!pose) return;
    int yaw = (int)lrint(atan2(sin(spot[3]), cos(spot[3])) * 4096 / (2 * M_PI)) & 4095; /* (facing the front) */
    if (lineup_n) { pose_lineup(pose, yaw); return; }
    static int logged;
    for (int p = 0; p < 3; p++) {
        if (cast_kind[p] < 0) continue;
        double c[3];
        block_top(p, c);
        { /* its wheelbase's middle on the block's middle, as the line-up's (the stats' axles are not even about the car's
           * origin: Outlaw 2 stood almost on the block's back edge, the user's 2026-11-11) */
            int track, front, rear;
            cast_stats(p, &track, &front, &rear);
            double a = (double)yaw * 2 * M_PI / 4096, mid = (front + rear) / 2.0 - car_fwd(cast_kind[p], cast_index[p]);
            c[0] -= sin(a) * mid, c[1] -= cos(a) * mid;
        }
        uint32_t ok = guest_call7(pose, (uint32_t)cast_kind[p], (uint32_t)cast_index[p], (uint32_t)(int32_t)lrint(c[0]),
                                  (uint32_t)(int32_t)lrint(c[1]), (uint32_t)(int32_t)lrint(c[2]) + (uint32_t)cast_lift(p) + SHADOW_UP,
                                  (uint32_t)yaw, 0);
        if (!logged) {
            uint32_t st = cast_stats_at(p);
            fprintf(stderr, "podium: place %d (kind %d, record %d) posed at %.0f %.0f %.0f: %u (ride %d, track %d, axles %d/%d, "
                    "roof %d)\n", p + 1, cast_kind[p], cast_index[p], c[0], c[1], c[2], ok, (int16_t)mem_r16(&M.cpu, st + 0x42),
                    (int16_t)mem_r16(&M.cpu, st + 0x48), (int16_t)mem_r16(&M.cpu, st + 0x4E), (int16_t)mem_r16(&M.cpu, st + 0x50),
                    (int16_t)mem_r16(&M.cpu, st + 0x46));
        }
    }
    /* the others (a computer car of the round not in the top 3, a parked player): hidden from the scene's pictures
     * (the object's visible word, as the invisibility special does; the RAM is put back after each frame) */
    CPU *cc = &M.cpu;
    uint32_t keep[3] = {0, 0, 0}; /* (the cast's objects: a slot out of use may share one) */
    for (int p = 0; p < 3; p++)
        if (cast_kind[p] >= 0)
            keep[p] = mem_r32(cc, (cast_kind[p] ? GA(0x80188B38u) : RB_humanCarObjects) + 4u * (uint32_t)cast_index[p]);
    for (int kind = 0; kind < 2; kind++)
        for (int i = 0; i < 16; i++) {
            uint32_t o = mem_r32(cc, (kind ? GA(0x80188B38u) : RB_humanCarObjects) + 4u * (uint32_t)i); /* (aiCarObjects) */
            if (!logged && getenv("TM2PC_TEST_PODIUM_LOG")) fprintf(stderr, "podium: kind %d record %d object %08X visible %d\n",
                                                                kind, i, o, o >= 0x80000000u && o < 0x80000000u + GUEST_RAM_BYTES ? (int)mem_r32(cc, o + 8) : -1);
            if (o < 0x80000000u || o >= 0x80000000u + GUEST_RAM_BYTES) continue;
            /* the cast shown (a computer car of a round ended at once may not be yet), the others hidden */
            mem_w32(cc, o + 8, o == keep[0] || o == keep[1] || o == keep[2]);
        }
    logged = 1;
}

/* ---- a frame of the scene: the world drawn from the shot's camera (as ui/photo.c's render_frame) ---- */
#define VIEW0_FRAMES GA(0x8018118Cu)
#define SLOT_VIEW0 GA(0x80180EE4u)
static uint8_t *kept_ram, kept_scratch[1024];
static uint32_t kept_gte[64];
void podium_scene(int on)
{
    scene_on = on;
    if (!on) for (int p = 0; p < 3; p++) cast_kind[p] = -1;
    if (!on) lineup_n = 0;
    kw = kd = kc = 1, sized = 0;
}
int podium_scene_on(void) { return scene_on; }
void podium_frame(double t)
{
    CPU *c = &M.cpu;
    double x, y, z, yaw;
    if (!podium_spot(&x, &y, &z, &yaw)) { rt_vblank(); return; }
    double u, eye[3], look[3];
    int s = podium_shot_at(t, &u);
    if (!sized) podium_size();
    shot_camera(s, u, eye, look);
    if (!kept_ram && !(kept_ram = malloc(GUEST_RAM_BYTES))) { rt_vblank(); return; }
    memcpy(kept_ram, M.ram, GUEST_RAM_BYTES);
    memcpy(kept_scratch, M.scratch, sizeof kept_scratch);
    memcpy(kept_gte, c->gte_data, 128);
    memcpy(kept_gte + 32, c->gte_ctrl, 128);
    pose_cars();
    uint32_t cam = mem_r32(c, RB_viewCameraObjects), parent = mem_r32(c, RB_viewCameraObjectTable);
    if ((int)mem_r32(c, RB_viewModes) != 1) guest_call(GA(0x80155888u), VIEW0_FRAMES, 1, 0, 0); /* viewCreate: full screen */
    static const int16_t ident[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
    for (int i = 0; i < 9; i++) mem_w16(c, parent + 0x80 + 2u * (uint32_t)i, (uint16_t)ident[i]);
    for (uint32_t i = 0; i < 12; i++) mem_w8(c, parent + 0x8C + i, 0);
    for (uint32_t i = 0; i < 8; i++) mem_w8(c, parent + 0xA0 + i, 0);
    for (uint32_t i = 0; i < 12; i++) mem_w8(c, parent + 0xA8 + i, 0);
    double fx = look[0] - eye[0], fy = look[1] - eye[1], fz = look[2] - eye[2], h = sqrt(fx * fx + fy * fy);
    int cyaw = (int)(atan2(fx, fy) * 4096 / (2 * M_PI)) & 4095, pitch = (int)(atan2(fz, h > 1 ? h : 1) * 4096 / (2 * M_PI));
    mem_w16(c, cam + 0xA0, (uint16_t)(int16_t)-pitch);
    mem_w16(c, cam + 0xA2, 0);
    mem_w16(c, cam + 0xA4, (uint16_t)(int16_t)cyaw);
    for (int i = 0; i < 3; i++) mem_w32(c, cam + 0xA8 + 4u * (uint32_t)i, (uint32_t)(int32_t)lrint(eye[i]));
    uint32_t sky = guest_call(GA(0x801563A0u), 0, 0, 0, 0); /* viewGetFollowObj(0): the sky round the camera */
    if (sky) {
        mem_w32(c, sky + 0xA8, (uint32_t)(int32_t)lrint(eye[0]));
        mem_w32(c, sky + 0xAC, (uint32_t)(int32_t)lrint(eye[1]));
        /* (tools: TM2PC_TEST_LINEUP_SKY_Z lowers the backdrop, the skyline round the camera, so it starts below the
         * roof's edge, not on the horizon: the user's 2026-11-08) */
        mem_w32(c, sky + 0xB0, (uint32_t)(lineup_n && getenv("TM2PC_TEST_LINEUP_SKY_Z") ? atoi(getenv("TM2PC_TEST_LINEUP_SKY_Z")) : 0));
    }
    uint32_t cur = mem_r32(c, SLOT_VIEW0), rec = cur == VIEW0_FRAMES ? VIEW0_FRAMES + 0x80 : VIEW0_FRAMES;
    mem_w32(c, rec + 8, mem_r32(c, rec + 4));
    mem_w32(c, SLOT_VIEW0, rec);
    guest_call(GA(0x80141938u), mem_r32(c, rec), 4096, 0, 0); /* ClearOTagR */
    guest_call(GA(0x80155E80u), 0, 0, 0, 0);                   /* viewProc(0) */
    guest_call(GA(0x80110BE4u), rec, 0, 0, 0);                 /* fxSpritesDraw */
    guest_call(GA(0x8014B484u), rec, 0, 0, 0);                 /* debrisDraw */
    /* the cars' parts smoothed as car select's (runtime/gte.c): the camera moves round still cars, and each part's
     * whole-unit place rounded on its own, Outlaw 2's panels and badge slid over its body (the user's 2026-11-11). The
     * RAM put back after each frame, the scene's nodes come in the same order each time. */
    void gte_tr_smooth_begin(int slot, double between), gte_tr_smooth_end(void);
    static int no_smooth = -1; /* (tests: TM2PC_PODIUM_NO_SMOOTH=1, as before) */
    if (no_smooth < 0) no_smooth = getenv("TM2PC_PODIUM_NO_SMOOTH") != NULL;
    if (!no_smooth) gte_tr_smooth_begin(3, -1);
    guest_call(GA(0x80152284u), rec, 0, 0, 0);                 /* renderScene (the podium with the view's world) */
    if (!no_smooth) gte_tr_smooth_end();
    guest_call(GA(0x801414CCu), 0, 0, 0, 0);                   /* DrawSync */
    guest_call(GA(0x80141AA4u), rec + 0x10, 0, 0, 0);          /* PutDrawEnv */
    guest_call(GA(0x80141BA0u), cur + 0x6C, 0, 0, 0);          /* PutDispEnv */
    extern int gpu_order_ties; /* (runtime/gpu.c: near-ties inside a slot to the later polygon, as the PS1 drew them) */
    static int no_ties = -1; /* (tests: TM2PC_PODIUM_NO_TIES=1, as before) */
    if (no_ties < 0) no_ties = getenv("TM2PC_PODIUM_NO_TIES") != NULL;
    gpu_order_ties = !no_ties;
    guest_call(GA(0x80141A30u), mem_r32(c, rec) + 4095u * 4, 0, 0, 0); /* DrawOTag */
    gpu_order_ties = 0;
    void render_budget_carry(int phase);
    render_budget_carry(0);
    memcpy(M.ram, kept_ram, GUEST_RAM_BYTES);
    memcpy(M.scratch, kept_scratch, sizeof kept_scratch);
    memcpy(c->gte_data, kept_gte, 128);
    memcpy(c->gte_ctrl, kept_gte + 32, 128);
    render_budget_carry(1);
    rt_vblank();
}
