/* Interpolated frames (docs/RENDER_PLAN.md 5; decomp RENDER_ANSWERS.md R3).
 *
 * The game steps its world 20 times a second and shows each step for three
 * VBlanks. Here the step itself is left exactly as it is (so the battle plays
 * the same), and in the VBlanks the main loop waits through (its VSync(0)
 * calls after a step's drawing) the world is drawn again from the host, as
 * photo mode draws it, with the camera and the objects moved part of the way
 * from one step's drawn pose to the next.
 *
 * The game shows a step one step late: each frame record draws into one
 * buffer and displays the other (draw 320,0 with display 0,0), so at the end
 * of step j it draws j and shows j-1. In step j's wait the screen shows j-2 at
 * VBlank 0; the redraws go from j-2 to j-1 (a third, two thirds of the way)
 * before the game shows j-1. They draw into this step's draw areas (drawn over
 * by the game after the wait) and show them (the other record's display
 * environment).
 *
 * The poses: at each step's renderScene of view 1 the drawn objects' poses are
 * kept (the object pool, 128 x 0x128: +0x80 matrix, +0xA0 angles, +0xA8
 * position; the cameras' angles and positions: viewProc builds their matrix).
 * The other views (split screen's, the rear-view mirror) are drawn after the
 * world's step (rtMainLoop), a step newer: their poses are kept apart, at
 * their first renderScene. A redraw writes the blend, draws each view as the
 * main loop does (the views' frame records given the redraws' own order tables
 * and packets for the while), presents as it does (the HUDs; split screen:
 * the port's portViewsPresent, whose HUD wrap shows the players' mirrors), and
 * puts every object, record, the GTE, the scratchpad and viewProc's outputs
 * back. An object not drawn in one of the two steps, or moved too far (a
 * respawn, a warp), is drawn where it is.
 *
 * Kept once per step (R3 d), as no-ops while redrawing: the animations' steps
 * (animUpdateAll, animUpdateOne: they draw rand()), the HUD's radar swap, its
 * message timer and the weapon marker's blink, the tasers' arcs, the screen
 * flash's countdown; streaksDraw draws only (four.c's streaks_draw_only).
 * The effects drawn by hand are blended too (R3 (a)): effect sprites, impact particles, the bullets' streaks,
 * the debris bursts' moving parts; the tasers' arcs and the screen flash are drawn on in-between frames, their
 * records (and the random seed) put back after. Not blended yet: the model nodes written directly (car part
 * spinners, Mr. Slam's arm, the Eiffel tower), sprites that follow a car.
 *
 * The kill cam and photo mode rewind: a machine put back (killcam.c's load)
 * starts the history again; the kill cam's shots take the redraw's place
 * between steps (interp_step_offset). Uncapped: more in-between frames in
 * main.c's pacing wait (interp_extra), up to the display's refresh rate. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "settings.h"
#include "vplus.h"
#include "cars.h" /* the car records and counts */

#define POOL GA(0x8019A950u)          /* csObjectPool: objects of 0x128 (retail 128; the decomp's 16 cars: 20 a car) */
#define POOL_N 320                    /* (the most: TM2_MAX_COORD_SYSTEMS, 20 x 16 cars, request 40's mines) */
#define POOL_COUNT GA(0x801810DCu)    /* csPoolCount: the objects allocated */
/* the objects in use: past the retail 128 with 16 cars (each car's own object among them: not blended before, so the
 * car's body and the camera moved between steps without it, the car seen floating ahead) */
static int pool_used(void)
{
    int n = (int)mem_r32(&M.cpu, POOL_COUNT);
    return n < 0 ? 0 : n > POOL_N ? POOL_N : n;
}
#define OBJ 0x128u
#define VIEW0_FRAMES GA(0x8018118Cu)  /* view 1's frame record pair */
#define SLOT_VIEW0 GA(0x80180EE4u)    /* the records views 1, 2 and the mirror draw into this frame */
#define SLOT_VIEW1 GA(0x80180EE8u)
#define SLOT_MIRROR GA(0x80180EECu)
#define VIEW_SPLIT GA(0x801809E4u)
#define REAR_VIEW GA(0x801809DCu)
#define EYES GA(0x801A51A8u)          /* viewEyePositions (viewProc's) */
#define MATS GA(0x801A51C0u)          /* viewMatrices */
#define PORT_FRAMES_BASE 0x80DD0000u  /* views 3-4's frame records (port_hooks.h, four.c) */
#define HI_OT 0x80C00000u             /* the redraws' order tables: 5 of 4096 entries */
#define VIEWS 4
#define NTSC_HZ_ 59.83
#define CAMS (2 * VIEWS)

typedef struct { int16_t m[9]; int16_t rot[3]; int32_t pos[3]; int drawn; } Pose;
/* The effects drawn by hand (decomp RENDER_ANSWERS.md R3 (a)): effect sprites (explosions, smoke, sparks, flames),
 * impact particles, the machine gun's bullets (their streaks), the debris bursts */
/* the effect sprites (fxSprites, 48 bytes each: frame +10 (< 0 free), pos +20, follow +1): 150 at 80185144 in retail's
 * layout; with 16 cars the variant moves the pool to its port sections, 25 a car (TM2_MAX_FX_SPRITES, decomp request 102) */
enum { FX_SPRITES_MAX = 400 };
uint32_t fx_sprites_n(void) { return cars_ai_max() > 9 ? 400u : 150u; }
uint32_t fx_sprites_base(void)
{
    static uint32_t b = 1;
    if (b == 1) {
        b = port_sym("fxSprites");
        if (!b) b = GA(0x80185144u);
        fprintf(stderr, "interp: the effect sprites at %08X (%u)\n", (unsigned)b, fx_sprites_n());
    }
    return b;
}
#define FX_SPRITES fx_sprites_base()
#define FX_IMPACTS GA(0x80184E74u)   /* 20 x 36: pos +0, life +28 (<= 0 free) */
#define FX_BULLETS GA(0x80181498u)   /* 40 x 60: pos +0, prev +0x0C, life +0x28 */
#define FX_BURSTS GA(0x801A3E70u)    /* 4 x 0x228: active +0, age +8, off[4][3] +0x44 */
/* the tasers' and lightning's arcs (weaponArcs: 20 bytes each, count +4, from +0x0C, to +0x10): 5 in retail's layout,
 * one a car (16) where the variant moved the pool to its port sections (TM2_MAX_ARCS) */
static uint32_t arcs_n(void);
static uint32_t arcs_base(void)
{
    static uint32_t b = 1;
    if (b == 1) {
        b = port_sym("weaponArcs");
        if (!b) b = GA(0x801847B4u);
        fprintf(stderr, "interp: the arcs at %08X (%u)\n", (unsigned)b, arcs_n());
    }
    return b;
}
static uint32_t arcs_n(void) { return cars_ai_max() > 9 ? 16u : 5u; } /* (the pools raised with the cars: TM2_MAX_ARCS 16) */
#define FX_ARCS arcs_base()
#define FX_SHEET GA(0x80181DF8u)     /* the screen flash's two POLY_F4 (24 bytes each), its timer 80180A60 */
typedef struct { int32_t pos[3]; int alive, follow; } FxPos;
typedef struct { int32_t pos[3], prev[3]; int alive; } FxBullet;
typedef struct { int32_t off[12]; int32_t age; int alive; } FxBurst;
/* each car type's body node (carBodyObjects 801873D0: a type-5 model node, its matrix at +4, position +24), posed by
 * carBodyPoseUpdate from the car's own record: drawn with the car (its shadow under it) */
/* (with the decomp's car groups, request 32: per car key, portCarGroups.body at +0x3C8, 48 keys) */
enum { BODY_MAX = 48 }; /* car keys (retail: 16 types) */
static uint32_t car_groups(void) { static uint32_t g = 1; if (g == 1) g = port_sym("portCarGroups"); return g; }
static uint32_t car_bodies(void) { return car_groups() ? car_groups() + 0x3C8 : GA(0x801873D0u); }
static int car_body_count(void) { return car_groups() ? BODY_MAX : 16; }
#define CAR_BODIES car_bodies()
typedef struct { int16_t m[9]; int32_t t[3]; uint32_t node; } FxBody;
typedef struct { FxPos spr[FX_SPRITES_MAX], imp[20]; FxBullet bul[40]; FxBurst bur[4]; FxBody body[BODY_MAX]; } Effects;
/* the human cars' motion (record +0x6DC: +0x20 pos, +0x34 yaw, +0x50 rot_m): the mirror on the car's roof (portVisual
 * bit 9) puts its eye from it, not from the car's object (blended), so it moved only at the steps (the user's "Car"
 * mirror at about 20 frames a second) */
enum { MOTION_HUMANS = 4 };
typedef struct { int32_t pos[3], yaw; int16_t m[9]; } Motion;
typedef struct { Pose p[POOL_N + CAMS]; uint32_t cam[CAMS]; int valid, n; Effects fx; Motion motion[MOTION_HUMANS];
                 int32_t vyaw[4]; } Poses;
static const Poses *rad_a, *rad_b; /* (the radar between steps: interp_HudDisplayRadar, below) */
enum { RAD_MAX = 0x1000 };
static uint8_t rad_shown[4][RAD_MAX], rad_live[4][RAD_MAX]; /* each view's radar as its last full frame drew it; the live one */
static int32_t rad_shown_yaw[4];
static int rad_have[4], rad_swapped[4];
static void radar_put_back(void); /* the pool, then each view's camera and its parent */
enum { RING = 4 };
static Poses early[RING], late[RING]; /* view 1's (before the world's step), the others' (after it): steps j-3 .. j (by step % RING) */
static int blend_lag(void) /* the in-between frames go from step j-1-lag to j-lag (tests: TM2PC_INTERP_LAG) */
{
    static int lag = -1;
    if (lag < 0) lag = getenv("TM2PC_INTERP_LAG") ? atoi(getenv("TM2PC_INTERP_LAG")) : 1;
    return lag < 0 ? 0 : lag > RING - 2 ? RING - 2 : lag;
}
#define SLOT_BACK(k) ((step_no + RING - (unsigned)(k)) % RING) /* step j-k's slot */
#define POSE_A SLOT_BACK(blend_lag() + 1)
#define POSE_B SLOT_BACK(blend_lag())
static unsigned step_no;
static int redrawing, captured, late_captured, vsyncs;
static double redraw_t; /* the redraw's place between steps j-2 and j-1 */
extern int rt_fast_forward; /* main.c: photo mode rewind seeking (unseen) */
extern int killcam_replaying;
int port_config_on(void);
int four_humans(void);

int interp_redrawing(void) { return redrawing; }
double interp_redraw_t(void) { return redrawing ? redraw_t : 1.0; } /* (hud_styles.c: the marks between the shown step frames) */
double interp_step_offset(void) { return redrawing ? redraw_t - 2 : 0; } /* vplus/killcam.c: its shots' time */
int photo_open(void); /* ui/photo.c */
int podium_scene_on(void); /* vplus/podium.c */
static int enabled(void)
{
    if (photo_open() || podium_scene_on()) return 0; /* (photo mode and the Versus podium's scene draw their own frames) */
    static int env = -1;
    if (env < 0) env = getenv("TM2PC_INTERP") ? atoi(getenv("TM2PC_INTERP")) : -2;
    int on = env >= 0 ? env : settings_video.frame_rate > 0;
    return on && !M.headless;
}

static void read_pose(uint32_t o, Pose *p, int cam)
{
    for (int i = 0; i < 9; i++) p->m[i] = (int16_t)mem_r16(&M.cpu, o + 0x80 + 2u * (uint32_t)i);
    for (int i = 0; i < 3; i++) p->rot[i] = (int16_t)mem_r16(&M.cpu, o + 0xA0 + 2u * (uint32_t)i);
    for (int i = 0; i < 3; i++) p->pos[i] = (int32_t)mem_r32(&M.cpu, o + 0xA8 + 4u * (uint32_t)i);
    p->drawn = cam || mem_r32(&M.cpu, o + 0x08) != 0;
}
static void capture(Poses *s)
{
    s->n = pool_used();
    for (int i = 0; i < s->n; i++) read_pose(POOL + OBJ * (uint32_t)i, &s->p[i], 0);
    for (int v = 0; v < VIEWS; v++) {
        uint32_t cam = mem_r32(&M.cpu, RB_viewCameraObjects + 4u * (uint32_t)v);
        s->cam[2 * v] = cam;
        s->cam[2 * v + 1] = cam ? mem_r32(&M.cpu, cam + 0x10) : 0;
        for (int k = 0; k < 2; k++)
            if (s->cam[2 * v + k]) read_pose(s->cam[2 * v + k], &s->p[POOL_N + 2 * v + k], 1);
            else s->p[POOL_N + 2 * v + k].drawn = 0;
    }
    for (int v = 0; v < 4; v++) s->vyaw[v] = (int32_t)mem_r32(&M.cpu, VIEW_REC(v) + 0x710); /* (each view's player's yaw: the radar) */
    for (int h = 0; h < MOTION_HUMANS; h++) {
        uint32_t r = HUMAN_REC(h) + 0x6DC;
        for (int k = 0; k < 3; k++) s->motion[h].pos[k] = (int32_t)mem_r32(&M.cpu, r + 0x20 + 4u * (uint32_t)k);
        s->motion[h].yaw = (int32_t)mem_r32(&M.cpu, r + 0x34);
        for (int k = 0; k < 9; k++) s->motion[h].m[k] = (int16_t)mem_r16(&M.cpu, r + 0x50 + 2u * (uint32_t)k);
    }
    Effects *e = &s->fx;
    for (int i = 0; i < (int)fx_sprites_n(); i++) {
        uint32_t r = FX_SPRITES + 48u * (uint32_t)i;
        e->spr[i].alive = (int8_t)mem_r8(&M.cpu, r + 10) >= 0;
        e->spr[i].follow = mem_r8(&M.cpu, r + 1) != 0;
        for (int k = 0; k < 3; k++) e->spr[i].pos[k] = (int32_t)mem_r32(&M.cpu, r + 20 + 4u * (uint32_t)k);
    }
    for (int i = 0; i < 20; i++) {
        uint32_t r = FX_IMPACTS + 36u * (uint32_t)i;
        e->imp[i].alive = (int32_t)mem_r32(&M.cpu, r + 28) > 0;
        for (int k = 0; k < 3; k++) e->imp[i].pos[k] = (int32_t)mem_r32(&M.cpu, r + 4u * (uint32_t)k);
    }
    for (int i = 0; i < 40; i++) {
        uint32_t r = FX_BULLETS + 60u * (uint32_t)i;
        e->bul[i].alive = (int32_t)mem_r32(&M.cpu, r + 0x28) > 0;
        for (int k = 0; k < 3; k++) {
            e->bul[i].pos[k] = (int32_t)mem_r32(&M.cpu, r + 4u * (uint32_t)k);
            e->bul[i].prev[k] = (int32_t)mem_r32(&M.cpu, r + 0x0C + 4u * (uint32_t)k);
        }
    }
    for (int i = 0; i < car_body_count(); i++) {
        uint32_t n = mem_r32(&M.cpu, CAR_BODIES + 4u * (uint32_t)i);
        e->body[i].node = n;
        for (int k = 0; n && k < 9; k++) e->body[i].m[k] = (int16_t)mem_r16(&M.cpu, n + 4 + 2u * (uint32_t)k);
        for (int k = 0; n && k < 3; k++) e->body[i].t[k] = (int32_t)mem_r32(&M.cpu, n + 24 + 4u * (uint32_t)k);
    }
    for (int i = 0; i < 4; i++) {
        uint32_t r = FX_BURSTS + 0x228u * (uint32_t)i;
        e->bur[i].alive = mem_r32(&M.cpu, r) != 0;
        e->bur[i].age = (int32_t)mem_r32(&M.cpu, r + 8);
        for (int k = 0; k < 12; k++) e->bur[i].off[k] = (int32_t)mem_r32(&M.cpu, r + 0x44 + 4u * (uint32_t)k);
    }
    s->valid = 1;
}

/* each step's renderScene (four.c's wrap, before the game's): view 1's in the main loop, then the step's next one */
int killcam_showing(void); /* vplus/killcam.c */
void interp_capture(int view)
{
    if ((!enabled() && !killcam_showing()) || redrawing) return; /* (the kill cam's own full-screen view: at any frame rate) */
    if (view == 0) {
        if (getenv("TM2PC_INTERP_DEBUG")) fprintf(stderr, "interp: capture %u at %llu (ra %08X)\n", step_no + 1,
                                                    (unsigned long long)M.vblanks, M.cpu.r[31]);
        step_no++;
        capture(&early[step_no % RING]);
        if (getenv("TM2PC_INTERP_DEBUG")) {
            const Pose *cp = &early[step_no % RING].p[POOL_N];
            fprintf(stderr, "interp: cams %08X %08X, camera at %d %d %d\n", early[step_no % RING].cam[0], early[step_no % RING].cam[1],
                    cp->pos[0], cp->pos[1], cp->pos[2]);
        }
        late[step_no % RING].valid = 0;
        captured = 1;
        late_captured = 0;
        vsyncs = 0;
    } else if (captured && !late_captured) {
        capture(&late[step_no % RING]);
        late_captured = 1;
    }
}
static uint32_t visual_seed; /* (the animators' own stream, below) */
void interp_round_start(void) /* vp_battle_round: as the game seeds its stream each round, the animators' too */
{
    visual_seed = 0x2545F491u;
}
void interp_snap(void) /* a new round: nothing to blend from */
{
    for (int i = 0; i < RING; i++) early[i].valid = late[i].valid = 0;
}

static int16_t blend_angle(int16_t a, int16_t b, double t)
{
    int d = ((b - a + 2048) & 4095) - 2048;
    return (int16_t)(a + (int)lround(d * t));
}
/* a rotation matrix between two (4.12, rows), made orthonormal again */
static void blend_matrix(const int16_t *a, const int16_t *b, double t, int16_t *out)
{
    double r[3][3];
    for (int i = 0; i < 9; i++) r[i / 3][i % 3] = (a[i] + (b[i] - a[i]) * t) / 4096.0;
    for (int i = 0; i < 3; i++) { /* Gram-Schmidt over the rows */
        for (int j = 0; j < i; j++) {
            double d = r[i][0] * r[j][0] + r[i][1] * r[j][1] + r[i][2] * r[j][2];
            for (int k = 0; k < 3; k++) r[i][k] -= d * r[j][k];
        }
        double n = sqrt(r[i][0] * r[i][0] + r[i][1] * r[i][1] + r[i][2] * r[i][2]);
        if (n < 1e-6) { memcpy(out, b, 18); return; }
        for (int k = 0; k < 3; k++) r[i][k] /= n;
    }
    for (int i = 0; i < 9; i++) out[i] = (int16_t)lround(r[i / 3][i % 3] * 4096);
}
static int far_apart(const Pose *a, const Pose *b, double limit)
{
    double d = 0;
    for (int i = 0; i < 3; i++) d += ((double)b->pos[i] - a->pos[i]) * ((double)b->pos[i] - a->pos[i]);
    return d > limit * limit;
}
static void write_blend(uint32_t o, const Pose *a, const Pose *b, double t, int cam)
{
    if (!a->drawn || !b->drawn || far_apart(a, b, cam ? 512 : 400)) return; /* snapped: as it is */
    if (!cam) {
        int16_t m[9];
        blend_matrix(a->m, b->m, t, m);
        for (int i = 0; i < 9; i++) mem_w16(&M.cpu, o + 0x80 + 2u * (uint32_t)i, (uint16_t)m[i]);
    }
    for (int i = 0; i < 3; i++) mem_w16(&M.cpu, o + 0xA0 + 2u * (uint32_t)i, (uint16_t)blend_angle(a->rot[i], b->rot[i], t));
    for (int i = 0; i < 3; i++)
        mem_w32(&M.cpu, o + 0xA8 + 4u * (uint32_t)i, (uint32_t)(int32_t)lround(a->pos[i] + ((double)b->pos[i] - a->pos[i]) * t));
}
/* the car object whose record's motion.pos is at src (human records +0x6FC, computer cars' +0x73C), 0 if none */
static uint32_t car_object_of_motion(uint32_t src)
{
    for (int i = 0; i < CARS_HUMANS_MAX; i++) {
        uint32_t rec = HUMAN_REC(i);
        if (src == rec + 0x6FC) return car_slot_object((int16_t)mem_r16(&M.cpu, rec + 4));
    }
    for (int k = 0; k < cars_ai_max(); k++) {
        uint32_t rec = AI_REC(k);
        if (src == rec + 0x73C) return car_slot_object((int16_t)mem_r16(&M.cpu, rec + 6));
    }
    return 0;
}
/* every object and camera as the blend of two steps' poses (each as it is where they cannot be blended) */
static void apply(const Poses *a, const Poses *b, double t)
{
    for (int i = 0; i < a->n && i < b->n; i++) {
        uint32_t o = POOL + OBJ * (uint32_t)i;
        int is_cam = 0;
        for (int k = 0; k < CAMS; k++) if (b->cam[k] == o) is_cam = 1;
        if (!is_cam) write_blend(o, &a->p[i], &b->p[i], t, 0);
    }
    for (int k = 0; k < CAMS; k++)
        if (b->cam[k] && b->cam[k] == a->cam[k]) write_blend(b->cam[k], &a->p[POOL_N + k], &b->p[POOL_N + k], t, 1);
    /* the effects: between the two steps where alive in both and near (a new one in a reused slot: where it is) */
    const Effects *ea = &a->fx, *eb = &b->fx;
    for (int i = 0; i < (int)fx_sprites_n(); i++) {
        const FxPos *p = &ea->spr[i], *q = &eb->spr[i];
        if (!p->alive || !q->alive || q->follow) continue; /* (a follower sits on its car: drawn where its car is) */
        double d = 0;
        for (int k = 0; k < 3; k++) d += ((double)q->pos[k] - p->pos[k]) * ((double)q->pos[k] - p->pos[k]);
        if (d > 300.0 * 300.0) continue;
        for (int k = 0; k < 3; k++)
            mem_w32(&M.cpu, FX_SPRITES + 48u * (uint32_t)i + 20 + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(p->pos[k] + (q->pos[k] - (double)p->pos[k]) * t));
    }
    for (int i = 0; i < 20; i++) {
        const FxPos *p = &ea->imp[i], *q = &eb->imp[i];
        if (!p->alive || !q->alive) continue;
        double d = 0;
        for (int k = 0; k < 3; k++) d += ((double)q->pos[k] - p->pos[k]) * ((double)q->pos[k] - p->pos[k]);
        if (d > 300.0 * 300.0) continue;
        for (int k = 0; k < 3; k++)
            mem_w32(&M.cpu, FX_IMPACTS + 36u * (uint32_t)i + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(p->pos[k] + (q->pos[k] - (double)p->pos[k]) * t));
    }
    /* the bullets: a step's frame draws its streaks before their move (prev -> pos of the step before), so between the
     * two steps' frames the streak slides along the older step's own move: head = lerp(prev, pos), the same length */
    /* (between the two steps the cars are blended between: a bullet in both, its head from the older step's place to
     * the newer's, its streak the older's length; one fired in the newer step grows from where it started; one only in
     * the machine's present step not drawn yet. Sliding along the older step alone, the bullets trailed a step behind
     * the car at 60 FPS: they seemed to start behind it, the user's on Twister and Outlaw 2) */
    for (int i = 0; i < 40; i++) {
        const FxBullet *u = &ea->bul[i], *w = &eb->bul[i];
        uint32_t r = FX_BULLETS + 60u * (uint32_t)i;
        double dd = 0;
        for (int k = 0; k < 3; k++) dd += ((double)w->pos[k] - u->pos[k]) * ((double)w->pos[k] - u->pos[k]);
        if (u->alive && w->alive && dd < 1500.0 * 1500.0) { /* the same bullet in both */
            for (int k = 0; k < 3; k++) {
                double head = u->pos[k] + ((double)w->pos[k] - u->pos[k]) * t;
                mem_w32(&M.cpu, r + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(head));
                mem_w32(&M.cpu, r + 0x0C + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(head - ((double)u->pos[k] - u->prev[k])));
            }
        } else if (w->alive) { /* fired in the newer step: from its start, growing */
            for (int k = 0; k < 3; k++) {
                double head = w->prev[k] + ((double)w->pos[k] - w->prev[k]) * t;
                mem_w32(&M.cpu, r + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(head));
                mem_w32(&M.cpu, r + 0x0C + 4u * (uint32_t)k, (uint32_t)w->prev[k]);
            }
        } else if (u->alive) { /* gone in the newer step: as before, along its last move */
            for (int k = 0; k < 3; k++) {
                double head = u->prev[k] + ((double)u->pos[k] - u->prev[k]) * t;
                mem_w32(&M.cpu, r + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(head));
                mem_w32(&M.cpu, r + 0x0C + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(head - ((double)u->pos[k] - u->prev[k])));
            }
        } else mem_w32(&M.cpu, r + 0x28, 0); /* (the present step's: not yet; put back after the redraw) */
    }
    /* the car bodies (and with them their shadows) */
    for (int i = 0; i < car_body_count(); i++) {
        const FxBody *p = &ea->body[i], *q = &eb->body[i];
        if (!q->node || p->node != q->node) continue;
        double d = 0;
        for (int k = 0; k < 3; k++) d += ((double)q->t[k] - p->t[k]) * ((double)q->t[k] - p->t[k]);
        if (d > 400.0 * 400.0) continue;
        int16_t m[9];
        blend_matrix(p->m, q->m, t, m);
        for (int k = 0; k < 9; k++) mem_w16(&M.cpu, q->node + 4 + 2u * (uint32_t)k, (uint16_t)m[k]);
        for (int k = 0; k < 3; k++)
            mem_w32(&M.cpu, q->node + 24 + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(p->t[k] + ((double)q->t[k] - p->t[k]) * t));
    }
    /* a sprite following a car (a burning car's flames) reads the car's record, a step newer than the drawn car: its
     * source pointed for the redraw at the car's object (+0xA8, blended above) */
    for (int i = 0; i < (int)fx_sprites_n(); i++) {
        uint32_t r = FX_SPRITES + 48u * (uint32_t)i;
        if ((int8_t)mem_r8(&M.cpu, r + 10) < 0 || !mem_r8(&M.cpu, r + 1)) continue;
        uint32_t src = mem_r32(&M.cpu, r + 44), obj = car_object_of_motion(src);
        if (obj) mem_w32(&M.cpu, r + 44, obj + 0xA8);
    }
    for (int i = 0; i < 4; i++) {
        const FxBurst *p = &ea->bur[i], *q = &eb->bur[i];
        if (!p->alive || !q->alive || q->age < p->age) continue;
        for (int k = 0; k < 12; k++)
            mem_w32(&M.cpu, FX_BURSTS + 0x228u * (uint32_t)i + 0x44 + 4u * (uint32_t)k,
                    (uint32_t)(int32_t)lround(p->off[k] + ((double)q->off[k] - p->off[k]) * t));
    }
}

/* the frame records drawn into, given the redraws' order tables (and view 1's, their packets) for the while */
typedef struct { uint32_t rec; uint8_t saved[0x80]; } Rec;
static Rec recs[5];
static int rec_n;
static uint32_t take_rec(uint32_t rec, uint32_t twin)
{
    Rec *r = &recs[rec_n];
    r->rec = rec;
    for (uint32_t i = 0; i < 0x80; i += 4) *(uint32_t *)(r->saved + i) = mem_r32(&M.cpu, rec + i);
    mem_w32(&M.cpu, rec + 0, HI_OT + 0x4000u * (uint32_t)rec_n);
    /* the packets: the buffers of the view's twin record (the other of its pair: the frame before's, drawn and shown
     * already; this step's are still to be drawn), start +4 and end +0x0C: every view the room its own frames have,
     * one rule for every view and player count. (A redraw buffer of its own, 390 KB for every view together, cut 4
     * views at Farthest short.) */
    mem_w32(&M.cpu, rec + 4, mem_r32(&M.cpu, twin + 4));
    mem_w32(&M.cpu, rec + 12, mem_r32(&M.cpu, twin + 12));
    if (!rec_n) mem_w32(&M.cpu, rec + 8, mem_r32(&M.cpu, rec + 4));
    rec_n++;
    return rec;
}
static void draw_view(uint32_t rec, int view, uint32_t packets_from) /* as rtMainLoop draws a view */
{
    void four_streaks_draw_only(int on); /* four.c */
    guest_call(GA(0x80141938u), mem_r32(&M.cpu, rec), 4096, 0, 0); /* ClearOTagR */
    guest_call(GA(0x80155E80u), (uint32_t)view, 0, 0, 0);          /* viewProc */
    if (packets_from) /* the frame's packets go on in the same buffer; a view with a buffer of its own starts it */
        mem_w32(&M.cpu, rec + 8, mem_r32(&M.cpu, rec + 4) == mem_r32(&M.cpu, packets_from + 4) ? mem_r32(&M.cpu, packets_from + 8)
                                                                                           : mem_r32(&M.cpu, rec + 4));
    four_streaks_draw_only(1);
    guest_call(GA(0x800D8230u), rec, (uint32_t)view, 0, 0);        /* streaksDraw: its drawing only */
    four_streaks_draw_only(0);
    guest_call(GA(0x80110BE4u), rec, (uint32_t)view, 0, 0);        /* fxSpritesDraw */
    guest_call(GA(0x8014B484u), rec, 0, 0, 0);                     /* debrisDraw */
    guest_call(GA(0x80152284u), rec, (uint32_t)view, 0, 0);        /* renderScene */
}

/* the world again at t between two steps' poses, presented at the next VBlank */
static uint32_t save_pool_bytes;
static uint8_t save_pool[POOL_N * OBJ], save_cams[CAMS][OBJ], save_scratch[1024], save_eyes[VIEWS * 12], save_mats[VIEWS * 32];
/* the effects' records (and what drawing them may change: the arcs' counts, the flash's polygons and timer, the
 * active flag, the random seed), kept and put back around a redraw */
static const struct { uint32_t retail, size; } fx_regions[] = {
    {0x80185144u, FX_SPRITES_MAX * 48}, {0x80184E74u, 20 * 36}, {0x80181498u, 40 * 60}, {0x801A3E70u, 4 * 0x228}, {0x801847B4u, 5 * 20},
    {0x80181DF8u, 48}, {0x80180A60u, 4}, {0x80180B3Cu, 4}, {0x80180B60u, 4}, {0x80009010u, 4},
};
static uint8_t fx_saved[FX_SPRITES_MAX * 48 + 20 * 36 + 40 * 60 + 4 * 0x228 + 16 * 20 + 48 + 4 * 4];
static uint8_t body_saved[BODY_MAX][32];
static void fx_keep(int back)
{
    for (int i = 0; i < car_body_count(); i++) {
        uint32_t n = mem_r32(&M.cpu, CAR_BODIES + 4u * (uint32_t)i);
        for (uint32_t k = 0; n && k < 32; k += 4)
            if (back) mem_w32(&M.cpu, n + 4 + k, *(uint32_t *)(body_saved[i] + k));
            else *(uint32_t *)(body_saved[i] + k) = mem_r32(&M.cpu, n + 4 + k);
    }
    uint8_t *at = fx_saved;
    for (unsigned r = 0; r < sizeof fx_regions / sizeof fx_regions[0]; r++) {
        uint32_t a = fx_regions[r].retail == 0x80009010u ? 0x80009010u : GA(fx_regions[r].retail), size = fx_regions[r].size;
        if (fx_regions[r].retail == 0x801847B4u) a = arcs_base(), size = 20u * arcs_n(); /* (the arcs: wherever the build has them) */
        if (fx_regions[r].retail == 0x80185144u) a = fx_sprites_base(), size = 48u * fx_sprites_n(); /* (and the sprites) */
        for (uint32_t i = 0; i < size; i += 4, at += 4)
            if (back) mem_w32(&M.cpu, a + i, *(uint32_t *)at);
            else *(uint32_t *)at = mem_r32(&M.cpu, a + i);
    }
}
static void redraw(const Poses *ea, const Poses *eb, const Poses *la, const Poses *lb, double t)
{
    CPU *c = &M.cpu;
    uint32_t pool = POOL, cams[CAMS];
    memcpy(cams, eb->cam, sizeof cams);
    save_pool_bytes = (uint32_t)pool_used() * OBJ;
    for (uint32_t i = 0; i < save_pool_bytes; i += 4) *(uint32_t *)(save_pool + i) = mem_r32(c, pool + i);
    for (int k = 0; k < CAMS; k++)
        if (cams[k]) for (uint32_t i = 0; i < OBJ; i += 4) *(uint32_t *)(save_cams[k] + i) = mem_r32(c, cams[k] + i);
    for (uint32_t i = 0; i < sizeof save_eyes; i += 4) *(uint32_t *)(save_eyes + i) = mem_r32(c, EYES + i);
    for (uint32_t i = 0; i < sizeof save_mats; i += 4) *(uint32_t *)(save_mats + i) = mem_r32(c, MATS + i);
    memcpy(save_scratch, M.scratch, sizeof save_scratch);
    fx_keep(0);
    uint32_t gd[32], gc[32];
    memcpy(gd, c->gte_data, sizeof gd);
    memcpy(gc, c->gte_ctrl, sizeof gc);

    uint32_t slot = mem_r32(c, SLOT_VIEW0), other = slot == VIEW0_FRAMES ? VIEW0_FRAMES + 0x80 : VIEW0_FRAMES;
    int split = mem_r32(c, VIEW_SPLIT) != 0, mirror = !split && mem_r32(c, REAR_VIEW) != 0, b = slot == VIEW0_FRAMES ? 0 : 1;
    int n = split ? (four_humans() > 2 && mem_r32(c, VIEW_SPLIT) == 2 ? four_humans() : 2) : 1;
    if (n > VIEWS) n = VIEWS;
    rec_n = 0;
    redraw_t = t;
    /* (the radar: the poses view 1 is drawn from, before the world's step; else the late ones) */
    if (ea && eb && ea->valid && eb->valid) rad_a = ea, rad_b = eb;
    else rad_a = la, rad_b = lb;
    redrawing = 1;
    /* view 1 */
    apply(ea, eb, t);
    uint32_t dbg_obj = getenv("TM2PC_INTERP_DEBUG") ? mem_r32(c, GA(0x80188B38u)) : 0; /* tests: computer car 0's object */
    int32_t dbg_before = dbg_obj ? (int32_t)mem_r32(c, dbg_obj + 0xAC) : 0;
    uint32_t r0 = take_rec(slot, other);
    draw_view(r0, 0, 0);
    if (dbg_obj) {
        int dbg_i = (int)((dbg_obj - POOL) / OBJ), dbg_n = ea->n < eb->n ? ea->n : eb->n;
        fprintf(stderr, "interp: car 0 object %08X (pool %d of %d blended) y after the blend %d, after drawing %d; steps' %d %d\n",
                dbg_obj, dbg_i, dbg_n, dbg_before, (int32_t)mem_r32(c, dbg_obj + 0xAC),
                dbg_i >= 0 && dbg_i < dbg_n ? ea->p[dbg_i].pos[1] : -1, dbg_i >= 0 && dbg_i < dbg_n ? eb->p[dbg_i].pos[1] : -1);
    }
    /* the others, a step newer (drawn after the world's step) */
    if (n > 1 || mirror) {
        if (la->valid && lb->valid) apply(la, lb, t);
        uint32_t prev = r0;
        for (int k = 1; k < n; k++) {
            uint32_t twin = k == 1 ? VIEW0_FRAMES + 0x100u + 0x80u * (uint32_t)(1 - b) /* (the game's view 2 records) */
                                   : PORT_FRAMES_BASE + 0x100u * (uint32_t)(k - 1) + 0x80u * (uint32_t)(1 - b);
            uint32_t rec = take_rec(k == 1 ? mem_r32(c, SLOT_VIEW1) : PORT_FRAMES_BASE + 0x100u * (uint32_t)(k - 1) + 0x80u * (uint32_t)b,
                                    twin);
            draw_view(rec, k, prev);
            prev = rec;
        }
        if (mirror) {
            Motion kept[MOTION_HUMANS];
            int humans = (int)mem_r32(c, GA(0x80181014u)), blend = la->valid && lb->valid;
            for (int h = 0; blend && h < humans && h < MOTION_HUMANS; h++) { /* the mirror's eye between the steps too */
                uint32_t r = HUMAN_REC(h) + 0x6DC;
                const Motion *a = &la->motion[h], *bb = &lb->motion[h];
                double d = 0;
                for (int k = 0; k < 3; k++) d += fabs((double)bb->pos[k] - a->pos[k]);
                for (int k = 0; k < 3; k++) kept[h].pos[k] = (int32_t)mem_r32(c, r + 0x20 + 4u * (uint32_t)k);
                kept[h].yaw = (int32_t)mem_r32(c, r + 0x34);
                for (int k = 0; k < 9; k++) kept[h].m[k] = (int16_t)mem_r16(c, r + 0x50 + 2u * (uint32_t)k);
                if (d > 3000) continue; /* (a respawn, a warp: where it is) */
                int16_t m[9];
                blend_matrix(a->m, bb->m, t, m);
                for (int k = 0; k < 3; k++) mem_w32(c, r + 0x20 + 4u * (uint32_t)k, (uint32_t)(a->pos[k] + (int32_t)lround((bb->pos[k] - a->pos[k]) * t)));
                mem_w32(c, r + 0x34, (uint32_t)(int32_t)blend_angle((int16_t)(a->yaw & 4095), (int16_t)(bb->yaw & 4095), t) & 4095u);
                for (int k = 0; k < 9; k++) mem_w16(c, r + 0x50 + 2u * (uint32_t)k, (uint16_t)m[k]);
            }
            draw_view(take_rec(mem_r32(c, SLOT_MIRROR), VIEW0_FRAMES + 0x200u + 0x80u * (uint32_t)(1 - b)), 1, r0);
            for (int h = 0; blend && h < humans && h < MOTION_HUMANS; h++) {
                uint32_t r = HUMAN_REC(h) + 0x6DC;
                for (int k = 0; k < 3; k++) mem_w32(c, r + 0x20 + 4u * (uint32_t)k, (uint32_t)kept[h].pos[k]);
                mem_w32(c, r + 0x34, (uint32_t)kept[h].yaw);
                for (int k = 0; k < 9; k++) mem_w16(c, r + 0x50 + 2u * (uint32_t)k, (uint16_t)kept[h].m[k]);
            }
        }
    }
    /* presented as rtMainLoop presents (its display: this step's draw area) */
    guest_call(GA(0x801414CCu), 0, 0, 0, 0);                    /* DrawSync */
    guest_call(GA(0x80141AA4u), r0 + 0x10, 0, 0, 0);            /* PutDrawEnv */
    guest_call(GA(0x80141BA0u), other + 0x6C, 0, 0, 0);         /* PutDispEnv */
    guest_call(GA(0x80141A30u), mem_r32(c, r0) + 4095u * 4, 0, 0, 0); /* DrawOTag */
    uint32_t first = slot == VIEW0_FRAMES;
    if (!guest_call(GA(0x8011EDC4u), 0, 0, 0, 0)) {             /* gameIsAttractDemo: else the HUDs */
        if (split) guest_call(rc_port_views_present, first, 0, 0, 0); /* the other views and every HUD (the mirrors after) */
        else guest_call(GA(0x800F8370u), 0, first, 0, 0);        /* hudDrawSingleView */
    }
    if (mirror) {
        uint32_t rm = mem_r32(c, SLOT_MIRROR);
        guest_call(GA(0x80141AA4u), rm + 0x10, 0, 0, 0);
        guest_call(GA(0x80141A30u), mem_r32(c, rm) + 4095u * 4, 0, 0, 0);
    }
    redrawing = 0;
    radar_put_back();

    for (int i = rec_n - 1; i >= 0; i--) /* (the slot twice: the first saving holds the game's) */
        for (uint32_t k = 0; k < 0x80; k += 4) mem_w32(c, recs[i].rec + k, *(uint32_t *)(recs[i].saved + k));
    for (uint32_t i = 0; i < save_pool_bytes; i += 4) mem_w32(c, pool + i, *(uint32_t *)(save_pool + i));
    for (int k = 0; k < CAMS; k++)
        if (cams[k]) for (uint32_t i = 0; i < OBJ; i += 4) mem_w32(c, cams[k] + i, *(uint32_t *)(save_cams[k] + i));
    for (uint32_t i = 0; i < sizeof save_eyes; i += 4) mem_w32(c, EYES + i, *(uint32_t *)(save_eyes + i));
    for (uint32_t i = 0; i < sizeof save_mats; i += 4) mem_w32(c, MATS + i, *(uint32_t *)(save_mats + i));
    memcpy(M.scratch, save_scratch, sizeof save_scratch);
    fx_keep(1);
    memcpy(c->gte_data, gd, sizeof gd);
    memcpy(c->gte_ctrl, gc, sizeof gc);
}

/* The kill cam's shots (and photo mode rewind's playback): its replay runs as the battle ran (its split screen
 * and mirror too: the game's logic reads them), and each VBlank of its waits the view is drawn full screen
 * over it from the host, its camera the shot's (killcam_camera, ui_ViewProc's wrap), blended between steps
 * when the frame rate allows. The whole machine's RAM is kept and put back around the drawing, so nothing
 * of it (viewCreate's records and tables, the cameras) reaches the replay. */
static uint8_t *machine_ram;
static void killcam_present(const Poses *ea, const Poses *eb, double t);
static void killcam_present(const Poses *ea, const Poses *eb, double t)
{
    CPU *c = &M.cpu;
    if (!machine_ram && !(machine_ram = malloc(GUEST_RAM_BYTES))) return;
    memcpy(machine_ram, M.ram, GUEST_RAM_BYTES);
    memcpy(save_scratch, M.scratch, sizeof save_scratch);
    uint32_t gd[32], gc[32];
    memcpy(gd, c->gte_data, sizeof gd);
    memcpy(gc, c->gte_ctrl, sizeof gc);
    redrawing = 1;
    redraw_t = t;
    mem_w32(c, VIEW_SPLIT, 0);
    mem_w32(c, REAR_VIEW, 0);
    guest_call(GA(0x80155888u), VIEW0_FRAMES, 1, 0, 0);       /* viewCreate: view 1 full screen */
    if (ea && eb) apply(ea, eb, t);
    uint32_t slot = mem_r32(c, SLOT_VIEW0), other = slot == VIEW0_FRAMES ? VIEW0_FRAMES + 0x80 : VIEW0_FRAMES;
    rec_n = 0;
    draw_view(take_rec(slot, other), 0, 0); /* (the kill cam's view: the same rule) */
    guest_call(GA(0x801414CCu), 0, 0, 0, 0);                    /* DrawSync */
    guest_call(GA(0x80141AA4u), slot + 0x10, 0, 0, 0);          /* PutDrawEnv */
    guest_call(GA(0x80141BA0u), other + 0x6C, 0, 0, 0);         /* PutDispEnv */
    guest_call(GA(0x80141A30u), mem_r32(c, slot) + 4095u * 4, 0, 0, 0); /* DrawOTag */
    redrawing = 0;
    void render_budget_carry(int phase); /* vplus/render_stats.c: Farthest's detail budget, by the shot's own frames */
    render_budget_carry(0);
    memcpy(M.ram, machine_ram, GUEST_RAM_BYTES);
    memset(rad_swapped, 0, sizeof rad_swapped); /* (the whole RAM put back: the radars' too) */
    memcpy(M.scratch, save_scratch, sizeof save_scratch);
    memcpy(c->gte_data, gd, sizeof gd);
    memcpy(c->gte_ctrl, gc, sizeof gc);
    render_budget_carry(1);
}

/* Once per step (R3 d): while redrawing, not at all */
void F_80151700_orig(CPU *c);
void F_801518E8_orig(CPU *c);
void F_800FBDFC_orig(CPU *c);
void F_800FBC44_orig(CPU *c);
void F_800FB588_orig(CPU *c);
void F_801084D8_orig(CPU *c);
void F_800DC590_orig(CPU *c);
void interp_AnimUpdateAll(CPU *c) { if (!redrawing) F_80151700_orig(c); }       /* the animations: their rand() */
/* The model animators' rand() (random and weighted frames, their periods) runs only for what is drawn, once per
 * view: so how and what is drawn (the camera, the draw distance, the kill cam's shots) changed the game's one
 * random stream, and with it the battle. They draw from a stream of their own (the BIOS seed swapped around
 * them; decomp RAND_CALLERS.md 2: visual only). With the decomp's ANIM_IN_STEP (portRollbackOptions bit 3, online)
 * the step runs them on every machine alike: they stay on the game's stream, which the states, snapshots and hashes
 * carry (decomp ONLINE_ROLLBACK.md 14.4: a machine back from a state kept its own seed and picked other walls). */
#define BIOS_RAND_SEED 0x9010u
void F_80151BC4_orig(CPU *c);
void F_80151CB8_orig(CPU *c);
void F_80151DF4_orig(CPU *c);
static void visual_rand(CPU *c, void (*fn)(CPU *))
{
    static uint32_t rb_options = 1;
    if (rb_options == 1) rb_options = port_sym("portRollbackOptions");
    extern int rbs_rand_log, rbs_resimulating; /* (tests: net/rb_state.c's re-run check) */
    if (rbs_rand_log) fprintf(stderr, "anim %c node %08x from %08x\n", rbs_resimulating ? 'R' : 'L', c->r[4], c->r[31]);
    if (rb_options && (mem_r32(c, rb_options) & 0x8u)) { fn(c); return; }
    uint32_t game = mem_r32(c, 0x80000000u + BIOS_RAND_SEED);
    mem_w32(c, 0x80000000u + BIOS_RAND_SEED, visual_seed);
    fn(c);
    visual_seed = mem_r32(c, 0x80000000u + BIOS_RAND_SEED);
    mem_w32(c, 0x80000000u + BIOS_RAND_SEED, game);
}
void interp_AnimStepRandom(CPU *c) { visual_rand(c, F_80151BC4_orig); }
void interp_AnimStepWeightedRandom(CPU *c) { visual_rand(c, F_80151CB8_orig); }
void interp_AnimPickPeriod(CPU *c) { visual_rand(c, F_80151DF4_orig); }
void interp_AnimUpdateOne(CPU *c)
{
    uint32_t a = c->r[4];
    int before = mem_r8(c, a + 0x0C);
    if (!redrawing) F_801518E8_orig(c);
    static int dbg = -1; /* tests: TM2PC_ANIM_DEBUG, each 2-frame animation's frame changes */
    if (dbg < 0) dbg = getenv("TM2PC_ANIM_DEBUG") != NULL;
    if (dbg && mem_r8(c, a + 0x0B) == 2 && mem_r8(c, a + 0x0C) != before)
        fprintf(stderr, "anim %08x mode %d frame %d at vblank %llu (period %d, ticks %d, redraw %d)\n", a, mem_r8(c, a + 0x0A),
                mem_r8(c, a + 0x0C), (unsigned long long)M.vblanks, mem_r16(c, a + 6), (int)(int16_t)mem_r16(c, GA(0x8017EBF8u)),
                redrawing);
}
/* The radar between the steps. A full frame (rtMainLoop's) is shown a step after it is drawn (the PS1's two display
 * buffers): the frame drawn after step j shows while step j + 1 is waited for, after the in-between frames of the wait
 * before it. Those in-between frames drew the radar as it was then, step j + 1's blips: newer than the full frame
 * shown after them, so every third frame the dots went back a step (the user's 2026-11-14 videos: "trails behind the
 * dots" when turning; 3:48 of the online Versus take, frame by frame: two frames of one step's dots, then the step
 * before's again). Now an in-between frame draws the radar as the last full frame drew it (each view's record kept
 * at that draw, rad_shown), its blips turned about the radar's middle by how far the player's yaw has turned since,
 * as blended for this frame (hudRadarPlotCar placed them with the yaw then). Each blip's first point turned and the
 * whole primitive moved with it (a blip's two lines, a hill's square); one turned out of the radar is hidden. The
 * frame's lists point into the record until the frame is drawn: the live record is put back after it
 * (radar_put_back), the game's memory as the step left it. */
static void radar_put_back(void)
{
    for (int v = 0; v < 4; v++)
        if (rad_swapped[v]) {
            uint32_t radar = RB_hudRadars + rc_hud_radar_bytes * (uint32_t)v;
            for (uint32_t k = 0; k < rc_hud_radar_bytes; k += 4) mem_w32(&M.cpu, radar + k, *(uint32_t *)(rad_live[v] + k));
            rad_swapped[v] = 0;
        }
    rad_a = rad_b = NULL;
}
void F_800FBE68_orig(CPU *c);
void interp_HudDisplayRadar(CPU *c) /* hudDisplayRadar(ot, view) */
{
    int v = (int)c->r[5];
    static int dbg = -1; /* tests: TM2PC_RADAR_DEBUG, each draw's buffer */
    if (dbg < 0) dbg = getenv("TM2PC_RADAR_DEBUG") != NULL;
    static int off = -1; /* tests: TM2PC_NO_RADAR_TURN, the in-between frames' radar as the game has it (the newest step's) */
    if (off < 0) off = getenv("TM2PC_NO_RADAR_TURN") != NULL;
    int ok = v >= 0 && v < 4 && rc_hud_radar_bytes <= RAD_MAX;
    uint32_t radar = RB_hudRadars + rc_hud_radar_bytes * (uint32_t)(ok ? v : 0);
    if (!redrawing) { /* a full frame: its radar kept for the in-between frames shown after it */
        if (ok) {
            for (uint32_t k = 0; k < rc_hud_radar_bytes; k += 4) *(uint32_t *)(rad_shown[v] + k) = mem_r32(c, radar + k);
            rad_shown_yaw[v] = (int32_t)mem_r32(c, VIEW_REC(v) + 0x710) & 4095;
            rad_have[v] = 1;
        }
    } else if (ok && !off && rad_have[v] && !rad_swapped[v]) {
        for (uint32_t k = 0; k < rc_hud_radar_bytes; k += 4) {
            *(uint32_t *)(rad_live[v] + k) = mem_r32(c, radar + k);
            mem_w32(c, radar + k, *(uint32_t *)(rad_shown[v] + k));
        }
        rad_swapped[v] = 1;
        int d = 0;
        if (rad_a && rad_b && rad_a->valid && rad_b->valid) {
            int16_t seen = blend_angle((int16_t)(rad_a->vyaw[v] & 4095), (int16_t)(rad_b->vyaw[v] & 4095), redraw_t);
            d = ((seen - rad_shown_yaw[v]) % 4096 + 4096 + 2048) % 4096 - 2048;
        }
        int count = (int)mem_r32(c, radar + rc_hud_radar_bytes - 0x10u), buf = (int)mem_r32(c, radar + rc_hud_radar_bytes - 4u);
        if (d && count > 0 && buf >= 0 && buf < 2) {
            int split = (int)mem_r32(c, VIEW_SPLIT), cx = split == 1 ? 44 : 43, cy = split == 1 ? 43 : 41;
            double a = d * (2 * M_PI / 4096), cs = cos(a), sn = sin(a);
            for (int i = 0; i < count; i++) {
                uint32_t p = radar + 0x30 + 16u * (uint32_t)(buf + 2 * i);
                if (p + 16 > radar + rc_hud_radar_bytes - 0x10u) break;
                int x0 = (int16_t)mem_r16(c, p + 8), y0 = (int16_t)mem_r16(c, p + 10);
                double sx = x0 - cx, sy = y0 - cy; /* (as hudRadarPlotCar's sx, sy: turned by the yaw's change) */
                int nx = (int)lround(sx * cs + sy * sn), ny = (int)lround(-sx * sn + sy * cs);
                int dx = (nx + cx) - x0, dy = (ny + cy) - y0;
                if (nx + 36 < 0 || nx + 36 >= 72 || ny + 22 < 0 || ny + 22 >= 44) dx = -2000 - x0, dy = 0; /* (out: hidden) */
                int code = mem_r8(c, p + 7), pts = (code & 0xFC) == 0x40 ? 2 : 1; /* (a line: both points; a square: its corner) */
                for (int q = 0; q < pts; q++) {
                    mem_w16(c, p + 8 + 4u * (uint32_t)q, (uint16_t)(int16_t)((int16_t)mem_r16(c, p + 8 + 4u * (uint32_t)q) + dx));
                    mem_w16(c, p + 10 + 4u * (uint32_t)q, (uint16_t)(int16_t)((int16_t)mem_r16(c, p + 10 + 4u * (uint32_t)q) + dy));
                }
            }
        }
    }
    if (dbg && v == 0) {
        int cnt = (int)mem_r32(c, radar + rc_hud_radar_bytes - 0x10u), bf = (int)mem_r32(c, radar + rc_hud_radar_bytes - 4u);
        uint32_t p0 = radar + 0x30 + 16u * (uint32_t)(bf & 1);
        fprintf(stderr, "radar: vblank %llu redraw %d buf %d count %d first %d,%d yaw %u\n", (unsigned long long)M.vblanks, redrawing, bf, cnt,
                (int16_t)mem_r16(c, p0 + 8), (int16_t)mem_r16(c, p0 + 10), mem_r32(c, VIEW_REC(0) + 0x710) & 4095u);
    }
    F_800FBE68_orig(c);
}
void interp_HudRadarSwap(CPU *c) /* (redraws show this step's blips) */
{
    if (redrawing) return;
    /* With the decomp's STEP_SPLIT (portRollbackOptions bit 8: online) each step swaps (portRollbackHudStep, before its
     * plots). The radar's own swap at its draw (hudDisplayRadar 800FBE68) then came only on a frame drawn without a step
     * (a stall, waiting for another machine's inputs) and emptied the blips and arrows until the next step: the radar
     * blinked while turning (the user's 2026-11-13 online Versus video, the host stalling) */
    static uint32_t opts = 1;
    if (opts == 1) opts = port_sym("portRollbackOptions");
    static int dbg = -1; /* tests: TM2PC_RADAR_DEBUG, each swap */
    if (dbg < 0) dbg = getenv("TM2PC_RADAR_DEBUG") != NULL;
    if (dbg && c->r[4] == 0)
        fprintf(stderr, "radar: vblank %llu swap from %08x%s\n", (unsigned long long)M.vblanks, gr_function(c->r[31]),
                opts && (mem_r32(c, opts) & 0x100u) && gr_function(c->r[31]) == 0x800FBE68u ? " (skipped)" : "");
    if (opts && (mem_r32(c, opts) & 0x100u) && gr_function(c->r[31]) == 0x800FBE68u) return; /* retail-address */
    F_800FBDFC_orig(c);
}
/* carNearestToHuman(human, &is_human): the target panel's car, looked for anew at each draw; an in-between frame (the
 * cars blended) could find another car for a frame (a test: GRASSHOPPER, SHADOW for one frame, GRASSHOPPER): redraws
 * keep each player's car of the step */
void F_800EC8A0_orig(CPU *c);
/* The target panel's car another player (the user's 2026-11-14: in an online Versus every enemy is a player, and the
 * classic HUD's enemy name and health under the radar never showed): the game's panel draws only a computer car (a
 * player: *is_human 1, nothing drawn; its layout is the computer cars'). Here a player is given to it as a copy of
 * the fields it reads, in that layout: the slot (+0x006, a mod car's name), the type (+0x030), the stats block
 * (+0x4FC: max HP +0x3A, HP +0xA4, i.e. +0x5A0). Only the panel calls carNearestToHuman. */
static int target_human = -1; /* (the panel being drawn: its player, or -1 for a computer car) */
#define TARGET_SHADOW 0x80DF0880u /* 0x5E4 bytes of spare RAM (after WATCH_SHADOW's 0x854, before MIRROR_OT) */
static void target_of_human(CPU *c, uint32_t out)
{
    uint32_t h = c->r[2];
    static int off = -1; /* (tests: TM2PC_NO_TARGET_HUMANS=1, the game's) */
    if (off < 0) off = getenv("TM2PC_NO_TARGET_HUMANS") != NULL;
    if (!h || !out || off || mem_r8(&M.cpu, out) != 1) return;
    mem_w16(&M.cpu, TARGET_SHADOW + 0x06, mem_r16(&M.cpu, h + 0x04));
    mem_w32(&M.cpu, TARGET_SHADOW + 0x30, mem_r32(&M.cpu, h + 0x90));
    for (uint32_t o = 0; o < 0xE8; o += 4) mem_w32(&M.cpu, TARGET_SHADOW + 0x4FC + o, mem_r32(&M.cpu, h + 0x14C + o));
    mem_w8(&M.cpu, out, 0);
    c->r[2] = TARGET_SHADOW;
    for (int k = 0; k < 16; k++) if (HUMAN_REC(k) == h) target_human = k; /* (its name: the player's, vp_FontSpritePrintCentered) */
}
int interp_target_human(void) { return target_human; } /* the target panel's car a player (index), else -1 */
void interp_CarNearestToHuman(CPU *c)
{
    static uint32_t car[16];
    static uint8_t flag[16], known[16];
    int h = (int)c->r[4];
    uint32_t out = c->r[5];
    target_human = -1;
    if (redrawing && h >= 0 && h < 16 && known[h]) {
        if (out) mem_w8(&M.cpu, out, flag[h]);
        c->r[2] = car[h];
        target_of_human(c, out);
        return;
    }
    F_800EC8A0_orig(c);
    if (!redrawing && h >= 0 && h < 16) { car[h] = c->r[2]; flag[h] = out ? mem_r8(&M.cpu, out) : 0; known[h] = 1; }
    target_of_human(c, out);
}
void interp_HudMessageUpdate(CPU *c) { if (!redrawing) F_800FBC44_orig(c); }    /* its countdown */
void interp_HudBlinkMarker(CPU *c) { if (!redrawing) F_800FB588_orig(c); }      /* its counter */
void interp_WeaponArcsDraw(CPU *c) /* the tasers' and lightning's arcs: on an in-between frame drawn, their tick undone */
{
    if (!redrawing) { F_801084D8_orig(c); return; }
    for (uint32_t i = 0; i < arcs_n(); i++) { /* (its count's hits, at 39 and 0 after its decrement: never on a redraw) */
        uint32_t arc = FX_ARCS + 20u * i, at = arc + 4;
        int16_t n = (int16_t)mem_r16(&M.cpu, at);
        if (n == 40) mem_w16(&M.cpu, at, 38);
        else if (n == 1) mem_w16(&M.cpu, at, 2);
        /* its ends read the cars' records, a step newer than the drawn cars: pointed for the redraw at the cars'
         * objects (+0xA8, blended), as the sprites following a car (else the arc leads the car it comes from) */
        for (uint32_t e = 0x0C; n > 0 && e <= 0x10; e += 4) {
            uint32_t obj = car_object_of_motion(mem_r32(&M.cpu, arc + e));
            if (obj) mem_w32(&M.cpu, arc + e, obj + 0xA8);
        }
    }
    F_801084D8_orig(c); /* (counts, sparks, the seed, the ends: put back after the redraw, fx_keep) */
}
void interp_FxSheetDisplay(CPU *c) { F_800DC590_orig(c); } /* the screen flash: drawn on in-between frames too (its countdown and its polygons put back) */

/* ---- The shell's scenes (title, car select, the ending): sceneRun's frames, one a VBlank ----
 * Uncapped on a display that is not 60 Hz: a scene turned at 60 frames a second judders (100 Hz: some frames shown
 * once, some twice). Its frames are drawn by screenCarEnding (80122984: every listed object's draw, then shown);
 * here each one's objects' places and turns (+24, +36: s32[3] each) are kept, and on the display's clock the
 * list is drawn again between the last two frames (its RAM kept and put back), and only those frames are shown. */
static double refresh_hz(void);
/* Uncapped, and 60 FPS on a display of about 60 Hz: the frames on the display's clock (uncapped's way: there it is 60
 * too). The port's own pacing is the PS1's 59.83 Hz; under vsync at 60 the two drift a sixth of a frame a second, a frame
 * shown twice every 6 seconds or so (the user's 2026-11-01: "lagging a bit here and there" at 60, not uncapped). */
static int display_paced(void)
{
    if (settings_video.frame_rate == 2) return 1;
    if (settings_video.frame_rate != 1 || getenv("TM2PC_INTERP_HZ")) return 0;
    double hz = refresh_hz();
    return hz >= 58 && hz <= 62;
}
#define SCENE_OBJS 32
typedef struct { uint32_t obj[SCENE_OBJS]; int32_t pos[SCENE_OBJS][6]; int n; uint64_t at_ns; } SceneShot;
static SceneShot scene_prev, scene_cur;
static uint32_t scene_s, scene_frame;
static uint64_t scene_vblank;
static int scene_frames;
void F_80122984_orig(CPU *c);
void interp_SceneFrame(CPU *c) /* screenCarEnding(scene, frame) */
{
    uint32_t s = c->r[4], fr = c->r[5];
    if (display_paced() && !M.headless) {
        scene_prev = scene_cur;
        scene_cur.n = 0;
        for (int l = 0; l < 2; l++) {
            uint32_t count = mem_r32(c, s + (l ? 164u : 96u)), list = s + (l ? 168u : 100u);
            for (uint32_t i = 0; i < count && i < 16 && scene_cur.n < SCENE_OBJS; i++) {
                uint32_t o = mem_r32(c, list + 4 * i);
                scene_cur.obj[scene_cur.n] = o;
                for (int k = 0; k < 6; k++) scene_cur.pos[scene_cur.n][k] = (int32_t)mem_r32(c, o + 24 + 4u * (uint32_t)k);
                scene_cur.n++;
            }
        }
        scene_cur.at_ns = SDL_GetTicksNS();
        scene_frames = scene_s == s ? scene_frames + 1 : 1;
        scene_s = s;
        scene_frame = fr;
        scene_vblank = M.vblanks;
    }
    F_80122984_orig(c);
}
static int scene_clocked(void) /* a scene running, two frames kept, the display's clock wanted */
{
    return display_paced() && !M.headless && scene_s && scene_frames >= 2 && M.vblanks <= scene_vblank + 2 && !captured;
}
static uint8_t *scene_ram;
static double scene_t = -1;
double interp_scene_t(void) { return scene_t; } /* a scene drawn between its frames: where (0..1), else -1 (ui/options.c's own cars) */
static void scene_redraw(double t)
{
    CPU *c = &M.cpu;
    if (!scene_ram && !(scene_ram = malloc(GUEST_RAM_BYTES))) return;
    memcpy(scene_ram, M.ram, GUEST_RAM_BYTES);
    uint8_t scr[1024];
    memcpy(scr, M.scratch, sizeof scr);
    uint32_t gd[32], gc[32];
    memcpy(gd, c->gte_data, sizeof gd);
    memcpy(gc, c->gte_ctrl, sizeof gc);
    redrawing = 1;
    scene_t = t;
    for (int i = 0; i < scene_cur.n; i++) { /* each object between its last two frames (as it is where it jumped) */
        int j = -1;
        for (int k = 0; k < scene_prev.n; k++) if (scene_prev.obj[k] == scene_cur.obj[i]) { j = k; break; }
        if (j < 0) continue;
        const int32_t *a = scene_prev.pos[j], *b = scene_cur.pos[i];
        uint32_t o = scene_cur.obj[i];
        int jump = 0;
        for (int k = 0; k < 3; k++) if (llabs((long long)b[k] - a[k]) > 2000) jump = 1;
        if (jump) continue;
        for (int k = 0; k < 3; k++) mem_w32(c, o + 24 + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(a[k] + ((double)b[k] - a[k]) * t));
        for (int k = 3; k < 6; k++) {
            int d = ((b[k] - a[k] + 2048) & 4095) - 2048; /* (turns: the shortest way round) */
            if (abs(b[k] - a[k]) > 512 && abs(d) > 512) continue;
            mem_w32(c, o + 24 + 4u * (uint32_t)k, (uint32_t)(int32_t)lround(a[k] + d * t));
        }
    }
    /* as screenCarEnding draws (without its VSync): both order tables cleared, every listed object drawn */
    uint32_t s = scene_s, fr = scene_frame, other = mem_r32(c, s + 12);
    mem_w8(c, fr + 40, (uint8_t)mem_r32(c, s + 40));
    mem_w32(c, fr + 8, mem_r32(c, fr + 4));
    mem_w32(c, other + 8, mem_r32(c, other + 4));
    guest_call(GA(0x80141938u), mem_r32(c, fr), 4096, 0, 0);
    guest_call(GA(0x80141938u), mem_r32(c, other), 4096, 0, 0);
    for (int l = 0; l < 2; l++) {
        uint32_t count = mem_r32(c, s + (l ? 164u : 96u)), list = s + (l ? 168u : 100u);
        for (uint32_t i = 0; i < count && i < 16; i++) {
            uint32_t o = mem_r32(c, list + 4 * i), vt = mem_r32(c, o + 48);
            guest_call(mem_r32(c, vt + 12), o + (uint32_t)(int32_t)(int16_t)mem_r16(c, vt + 8), fr, 0, 0); /* its draw (a virtual call) */
        }
    }
    guest_call(GA(0x801414CCu), 0, 0, 0, 0); /* DrawSync */
    if (mem_r32(c, s + 3432)) {
        guest_call(GA(0x80141AA4u), other + 16, 0, 0, 0);
        guest_call(GA(0x80141A30u), mem_r32(c, other) + 4095u * 4, 0, 0, 0);
    }
    guest_call(GA(0x80141AA4u), fr + 16, 0, 0, 0); /* PutDrawEnv */
    /* shown from where it is drawn: the frame record (of the scene's pair) whose display area is its draw area */
    uint32_t base = mem_r32(c, s), disp = fr + 108;
    for (uint32_t r = base; r <= base + 128; r += 128)
        if (mem_r16(c, r + 108) == mem_r16(c, fr + 16) && mem_r16(c, r + 110) == mem_r16(c, fr + 18)) disp = r + 108;
    guest_call(GA(0x80141BA0u), disp, 0, 0, 0); /* PutDispEnv */
    if (mem_r32(c, s + 56)) guest_call(GA(0x8012201Cu), mem_r32(c, s + 56), fr, 0, 0); /* sceneDrawBackground */
    guest_call(GA(0x80141A30u), mem_r32(c, fr) + 4095u * 4, 0, 0, 0); /* DrawOTag */
    redrawing = 0;
    scene_t = -1;
    memcpy(M.ram, scene_ram, GUEST_RAM_BYTES);
    memcpy(M.scratch, scr, sizeof scr);
    memcpy(c->gte_data, gd, sizeof gd);
    memcpy(c->gte_ctrl, gc, sizeof gc);
}

/* Uncapped (the setting): the battle shown on the display's own clock. The driver's vsync is on (gpu_psyz.c), the
 * emulated VBlanks in a step's wait show nothing themselves, and main.c's pacing wait draws and shows frame after
 * frame, each held by vsync to a refresh of the display and blended for the moment it is shown: an even cadence at
 * any refresh rate (100 Hz: no 60-in-100 judder). */
static int display_clock(void) { return display_paced() && enabled(); }
static int history_ok(void) { return early[POSE_A].valid && early[POSE_B].valid; }
int interp_owns_present(void) /* gpu_psyz.c: the VBlank leaves the showing to the pacing wait */
{
    if (photo_open() || podium_scene_on()) return 0;
    if (scene_clocked()) return 1;
    return display_clock() && captured && !redrawing && !rt_fast_forward && (history_ok() || killcam_showing());
}
/* hle_VSync(0) (before its wait): in a step's wait, VBlanks 1 and 2 get a redraw */
void interp_vsync(void)
{
    if (display_clock() && captured && !redrawing) { vsyncs++; return; } /* (the pacing wait draws, on the display's clock) */
    if (captured && !redrawing && !rt_fast_forward && killcam_showing()) { /* every VBlank: the shot's own view */
        int n = vsyncs++;
        const Poses *ea = &early[POSE_A], *eb = &early[POSE_B];
        int blend = enabled() && ea->valid && eb->valid && n < 3;
        killcam_present(blend ? ea : NULL, blend ? eb : NULL, blend ? n / 3.0 : 2.0); /* (unblended: the step's own
                                                                                                * places, the camera's too) */
        return;
    }
    if (!enabled() || !captured || redrawing || rt_fast_forward) return;
    int n = ++vsyncs - 1; /* 0: step j-2's own frame shows at this VBlank */
    if (n < 1 || n > 2) return;
    const Poses *ea = &early[POSE_A], *eb = &early[POSE_B]; /* j-2, j-1 */
    const Poses *la = &late[POSE_A], *lb = &late[POSE_B];
    if (!ea->valid || !eb->valid) return;
    if (mem_r32(&M.cpu, VIEW_SPLIT) && !port_config_on()) return; /* (split screen: the port's C presents it) */
    if (getenv("TM2PC_INTERP_DEBUG")) fprintf(stderr, "interp: redraw at %llu, t %d/3\n", (unsigned long long)M.vblanks, n);
    redraw(ea, eb, la, lb, n / 3.0);
}
void interp_frame_start(void) { captured = 0; } /* SetFrameStart: the wait is over */

/* Uncapped: in the pacing wait after a VBlank of a step's wait (main.c's rt_pace_vblank: left, the part of
 * the VBlank still to wait), more in-between frames, shown at once, at most at the display's refresh rate:
 * t from the clock (VBlank n of the wait, and how far into it). */
static double refresh_hz(void)
{
    static double hz;
    static Uint64 asked;
    Uint64 now = SDL_GetTicks();
    if (!hz || now - asked > 2000) { /* (asked again now and then: the window may move to another display) */
        asked = now;
        const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        hz = m && m->refresh_rate > 0 ? m->refresh_rate : 60;
        if (getenv("TM2PC_INTERP_HZ")) hz = atof(getenv("TM2PC_INTERP_HZ")); /* tests */
    }
    return hz;
}
int interp_extra(double left)
{
    if (scene_clocked()) { /* a scene: frame after frame on the display's clock, between its last two frames */
        extern uint64_t gpu_last_present_ns;
        Uint64 now = SDL_GetTicksNS();
        if (now - gpu_last_present_ns < (Uint64)(1e9 / (refresh_hz() * 1.5))) return 0;
        double span = (double)(scene_cur.at_ns - scene_prev.at_ns);
        double t = span > 0 ? (double)(now - scene_cur.at_ns) / span : 1;
        if (t > 1) t = 1;
        scene_redraw(t);
        void gpu_psyz_present(void);
        gpu_psyz_present();
        (void)left;
        return 1;
    }
    if (!display_paced() && !getenv("TM2PC_INTERP_HZ")) return 0;
    if (!enabled() || !captured || redrawing || rt_fast_forward || vsyncs < 1) return 0;
    int showing = killcam_showing(), clocked = interp_owns_present();
    if (!clocked && vsyncs > 3) return 0;
    double hz = refresh_hz();
    if (!clocked && hz < 70) return 0; /* (60 Hz displays: one frame a VBlank already) */
    extern uint64_t gpu_last_present_ns; /* runtime/gpu_psyz.c: every frame shown counts */
    Uint64 now = SDL_GetTicksNS();
    /* on the display's clock: vsync holds each present to a refresh (the limit: a guard, a hidden window does not wait) */
    if (now - gpu_last_present_ns < (Uint64)(1e9 / (clocked ? hz * 1.5 : hz))) return 0;
    double into = 1 - left; /* how far into this VBlank */
    if (!clocked && left / NTSC_HZ_ < 0.5 / hz) return 0; /* (the VBlank's own frame is near) */
    const Poses *ea = &early[POSE_A], *eb = &early[POSE_B];
    const Poses *la = &late[POSE_A], *lb = &late[POSE_B];
    if (!ea->valid || !eb->valid) return 0;
    if (mem_r32(&M.cpu, VIEW_SPLIT) && !port_config_on() && !showing) return 0;
    double t = (vsyncs - 1 + into) / 3.0;
    if (t > 1) { if (!clocked) return 0; t = 1; } /* (a late step: held at its end) */
    static unsigned extras;
    static uint64_t counted_from;
    if (++extras % 600 == 0 && getenv("TM2PC_INTERP_DEBUG")) { /* tests: frames shown a second */
        fprintf(stderr, "interp: uncapped, %.0f extra frames a second\n", 600.0 * NTSC_HZ_ / (double)(M.vblanks - counted_from));
        counted_from = M.vblanks;
    }
    if (showing) killcam_present(ea, eb, t);
    else redraw(ea, eb, la, lb, t);
    void gpu_psyz_present(void); /* runtime/gpu_psyz.c */
    gpu_psyz_present();
    if (getenv("TM2PC_INTERP_DEBUG") && extras % 600 == 1)
        fprintf(stderr, "interp: a redraw and its present took %.2f ms\n", (double)(SDL_GetTicksNS() - now) / 1e6);
    return 1;
}
