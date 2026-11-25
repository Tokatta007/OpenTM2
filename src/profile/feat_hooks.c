/* The game events behind the feats (feats.h), from the decomp's
 * GAME_EVENTS.md. Each hook wraps a whole game function (src/overrides.json)
 * and tells its call sites apart by the return address: the weapon at a
 * killing blow is noted on the way into the damage function and read back
 * when the car is destroyed. Only the human players' feats are counted. */
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "runtime.h"
#include "rebase_table.h"
#include "feats.h"
#include "profile.h"
#include "vplus/cars.h"
extern int killcam_replaying; /* vplus/killcam.c */
void killcam_destroyed(int own, int killer);

#define SETTINGS GA(0x8016475Cu)
#define HUMAN_BASE RB_humanCars /* human records, 0x814 each */
#define HUMAN_SIZE HUMAN_REC_SIZE
#define AI_BASE GA(0x80188B5Cu)    /* computer cars, 0x854 each */
#define AI_SIZE AI_REC_SIZE
#define AI_MAX cars_ai_max()     /* the computer-car records there are (vplus/cars.h) */
#define KEYS (2 * CARS_MAX)      /* the keys' arrays: humans' then computer cars' (room for 16 of either) */
#define PROJECTILES GA(0x801A5B08u) /* PROJ_N records of 980 bytes */
enum { PROJ_MAX = 64 };            /* (sizes the tags) */
#define PROJ_N ((int)rc_projectile_count < PROJ_MAX ? (int)rc_projectile_count : PROJ_MAX) /* 16 retail, 48 in the 16-car variant */
#define PROJECTILE_SIZE 980u
#define EIFFEL GA(0x801AB810u)

enum { A_NONE, A_MG, A_FIRE, A_HOMING, A_POWER, A_NAPALM, A_FREEZE, A_REMOTE, A_MINE, A_RICOCHET, A_LIGHTNING,
       A_RAM, A_SPECIAL, A_OTHER };

#define HK CARS_HUMANS_MAX /* the players' keys (0..HK-1; the computer cars' from HK) */
static int humans(void) { int n = (int)mem_r32(&M.cpu, GA(0x80181014u)); return n > HK ? HK : n < 0 ? 0 : n; } /* humanCarTotal: 3-4 players too */
static uint32_t human_rec(int i) { return HUMAN_BASE + (uint32_t)i * HUMAN_SIZE; }
static uint32_t ai_rec(int i) { return AI_BASE + (uint32_t)i * AI_SIZE; }
static int16_t r16s(uint32_t a) { return (int16_t)mem_r16(&M.cpu, a); }
static int32_t r32s(uint32_t a) { return (int32_t)mem_r32(&M.cpu, a); }

/* a car-select index from a car type (0x10..0xF0); -1 for Dark Tooth */
static int car_index(int type)
{
    static const int8_t by_type[16] = {-1, 12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, -1};
    return by_type[(type >> 4) & 15];
}

/* the human player whose car has this slot, or -1 */
static int human_of_slot(int slot)
{
    if (slot < 0) return -1;
    for (int i = 0; i < humans(); i++)
        if (r16s(human_rec(i) + 4) == slot) return i;
    return -1;
}
/* a human's object id (decomp players.h TM2_HUMAN_ID: 1-7, then 9, then 11-18; 8 and 10 are other objects') */
static int human_of_id(int id)
{
    int h = id >= 1 && id <= 7 ? id - 1 : id == 9 ? 7 : id >= 11 && id <= 18 ? id - 3 : -1;
    return h >= 0 && h < humans() ? h : -1;
}
static int human_of_object(int id) { return human_of_id(id); }
static int slot_of_object(int id)
{
    if (human_of_id(id) >= 0) return r16s(human_rec(human_of_id(id)) + 4);
    if (id >= 50 && id < 50 + AI_MAX) return r16s(ai_rec(id - 50) + 6);
    return -1;
}
static int car_of_slot(int slot)
{
    int h = human_of_slot(slot);
    if (h >= 0) return car_index(r32s(human_rec(h) + 0x90));
    for (int i = 0; i < AI_MAX; i++)
        if (r16s(ai_rec(i) + 6) == slot) return car_index(r32s(ai_rec(i) + 0x30));
    return -1;
}

static int in_battle; /* the session runner is running a match */
void feats_battle(int on) { in_battle = on; }
int feats_in_battle(void) { return in_battle; }

static void match_damage(int attacker, int amount);
int vp_ai_ignore_ai(void); /* vplus/fixes.c: Only Target Humans */
int vp_team_blocks_damage(int victim_key, int attacker_slot); /* vplus/fixes.c: teams without friendly fire */
int vp_team_of_slot(int slot);                                /* vplus/fixes.c: a car's team, -1 none */
int vp_ai_vs_ai(void); /* vplus/fixes.c: computer cars fighting each other */
void vp_pickup_taken(int slot); /* vplus/fixes.c: a taken pickup comes back as a new pick */
void vp_gun_resupply(uint32_t rec, int human); /* vplus/fixes.c: Gun Game, a weapon pickup refills */
void vp_car_destroyed(int own_slot, int killer_slot, int attack); /* vplus/fixes.c: the modes, the kill feed */
int vp_mode_damage(int victim_slot, int attacker_slot, int ram, int amount); /* vplus/fixes.c: the Juggernaut's */
static int igniter[KEYS]; /* by key: who set it burning, + 1 (0: no one) */

/* ---- launches: which projectile records were fired backwards, or as a special ---- */
static struct { uint8_t rear, special; } tags[PROJ_MAX];
static uint16_t lives_before[PROJ_MAX];

static void tag_before(void)
{
    for (int i = 0; i < PROJ_N; i++) lives_before[i] = mem_r16(&M.cpu, PROJECTILES + (uint32_t)i * PROJECTILE_SIZE + 4);
}
static void tag_after(int rear, int special)
{
    for (int i = 0; i < PROJ_N; i++) {
        uint16_t life = mem_r16(&M.cpu, PROJECTILES + (uint32_t)i * PROJECTILE_SIZE + 4);
        if (life && life != lives_before[i]) { tags[i].rear = (uint8_t)rear; tags[i].special = (uint8_t)special; }
    }
}
static int rear_flag(uint32_t car) /* weapons +5, cleared by the launch */
{
    int human = car >= HUMAN_BASE && car < HUMAN_BASE + CARS_HUMANS_MAX * HUMAN_SIZE;
    return mem_r8(&M.cpu, car + (human ? 0x7C8u : 0x808u) + 5) != 0;
}

void F_801052BC_orig(CPU *c);
void feat_Launch(CPU *c) /* sub_801052BC(car, weapon, ...): missiles, napalm, freeze */
{
    int rear = rear_flag(c->r[4]);
    tag_before();
    F_801052BC_orig(c);
    tag_after(rear, 0);
}

void F_80105A7C_orig(CPU *c);
int vp_gun_blocks_special(uint32_t car, int human); /* vplus/fixes.c */
void feat_Special(CPU *c) /* sub_80105A7C(car, human): the car's special */
{
    if (vp_gun_blocks_special(c->r[4], (int)(c->r[5] & 0xFF))) return; /* Gun Game: a computer car's level is not its special */
    int rear = rear_flag(c->r[4]);
    tag_before();
    F_80105A7C_orig(c);
    tag_after(rear, 1);
}

/* ---- hits: the attack in progress ---- */
static int cur_attack = A_NONE, cur_rear, cur_attacker_override = -1;
static int ped_hitter = -1; /* the object that last hit a pedestrian (weapon hits) */

void F_800DC65C_orig(CPU *c);
void feat_WeaponHit(CPU *c) /* sub_800DC65C(hit, kind, sub, amount, dir, shooter) */
{
    uint32_t ra = c->r[31];
    int hit = (int16_t)c->r[4], shooter = r32s(c->r[29] + 20);
    int saved = cur_attack, saved_rear = cur_rear;
    cur_rear = 0;
    /* a projectile's hit: from weaponProjectileUpdate (800DB110..800DC308, its two calls: a car, an object). By the
     * function, not the call's address: the port's variant changes that function (its C), and its calls move.
     * Its record: in s1 (retail's register), else the one with this hit, damage and owner. */
    uint32_t rr = gr(ra), proj = 0;
    if ((rr >= 0x800DB110u && rr < 0x800DC308u) || gr_function(ra) == 0x800DB110u) { /* retail-address */
        uint32_t s1 = c->r[17];
        if (s1 >= PROJECTILES && s1 < PROJECTILES + (uint32_t)PROJ_N * PROJECTILE_SIZE && (s1 - PROJECTILES) % PROJECTILE_SIZE == 0) proj = s1;
        for (int i = 0; !proj && i < PROJ_N; i++) {
            uint32_t r = PROJECTILES + (uint32_t)i * PROJECTILE_SIZE;
            if (r32s(r + 0x0C) == (int32_t)c->r[4] && r16s(r + 0x42) == (int16_t)shooter && r16s(r + 0x0A) == (int16_t)c->r[7]) proj = r;
        }
    }
    switch (proj ? 0x800DC018u : rr) { /* (by retail address) */
    case 0x800D81ECu: cur_attack = A_MG; break; /* retail-address */
    case 0x800DC018u: { /* a projectile */ /* retail-address */
        uint32_t rec = proj;
        int type = r16s(rec + 2), idx = (int)((rec - PROJECTILES) / PROJECTILE_SIZE);
        int special = idx >= 0 && idx < PROJ_N && tags[idx].special;
        cur_rear = idx >= 0 && idx < PROJ_N && tags[idx].rear;
        int car = car_of_slot(slot_of_object(shooter));
        switch (type) {
        case 10: cur_attack = A_FIRE; break;
        case 11: cur_attack = A_HOMING; break;
        case 12: cur_attack = A_POWER; break;
        case 15: cur_attack = A_NAPALM; break;
        case 14: cur_attack = special && car == 13 ? A_SPECIAL : A_FREEZE; break;
        case 41: cur_attack = car == 2 || car == 13 ? A_SPECIAL : A_OTHER; break;
        case 43: case 49: case 51: case 61: cur_attack = A_SPECIAL; break;
        default: cur_attack = A_OTHER; break;
        }
        break;
    }
    case 0x80109BF4u: cur_attack = A_MINE; break; /* retail-address */
    case 0x8010D45Cu: case 0x8010D770u: cur_attack = A_RICOCHET; break; /* retail-address */
    case 0x8010AA08u: case 0x8010E424u: case 0x8010E7E4u: cur_attack = A_SPECIAL; break; /* Thumper, Twister */ /* retail-address */
    default: /* in a function the variant changed (its calls moved): by the function (tools/test/changed_sites.py) */
        switch (gr_function(ra)) {
        case 0x8010CC54u: cur_attack = A_RICOCHET; break;                    /* weaponRicochetUpdate */ /* retail-address */
        case 0x80109A9Cu: cur_attack = A_MINE; break;                        /* weaponDropCheckHit (moved: TM2_PORT_HUMANS=16) */ /* retail-address */
        case 0x800D7E30u: cur_attack = A_MG; break;                          /* bulletsMove */ /* retail-address */
        case 0x8010A7A0u: case 0x8010E040u: cur_attack = A_SPECIAL; break;   /* weaponFlameUpdate, weaponTornadoUpdate */ /* retail-address */
        default: { /* a retail body the variant moved to its own code (request 33-2a: one per car) */
            const char *fn = port_function_of(ra);
            cur_attack = fn && !strcmp(fn, "weaponTornadoUpdateOne") ? A_SPECIAL : A_OTHER;
            break;
        }
        }
        break;
    }
    if (hit >= 6000) ped_hitter = shooter;
    F_800DC65C_orig(c);
    cur_attack = saved;
    cur_rear = saved_rear;
}

/* splash (remote bombs, Shadow): what set it off */
static int splash_attack = A_NONE, eiffel_pending;
void F_8010B5C4_orig(CPU *c);
void feat_Splash(CPU *c) /* sub_8010B5C4(pos, owner slot, big) */
{
    uint32_t ra = c->r[31];
    int owner = (int16_t)c->r[5];
    int saved = splash_attack;
    /* (by the call's retail address, or its function where the variant changed it: weaponFireSpecial (Shadow), the
     * remote bombs' weaponDropWrite and 80109A9C) */
    uint32_t rr = gr(ra), fn = gr_function(ra);
    splash_attack = rr == 0x80106EACu || fn == 0x80105A7Cu ? A_SPECIAL /* retail-address */
                  : rr == 0x80109908u || rr == 0x80109CACu || fn == 0x801096F4u || fn == 0x80109A9Cu ? A_REMOTE : A_OTHER; /* retail-address */
    if (eiffel_pending && splash_attack == A_REMOTE) { /* the bomb that brought the tower down */
        eiffel_pending = 0;
        int h = human_of_slot(owner);
        if (h >= 0) feat_set(h, FEAT_EIFFEL);
    }
    F_8010B5C4_orig(c);
    splash_attack = saved;
}

void F_800D6710_orig(CPU *c);
void feat_EiffelStart(CPU *c) /* sub_800D6710: a remote bomb near the tower starts its fall */
{
    int before = mem_r8(&M.cpu, EIFFEL);
    F_800D6710_orig(c);
    if (!before && mem_r8(&M.cpu, EIFFEL)) eiffel_pending = 1; /* credited at the bomb's splash */
}

/* lightning: the caster (the game leaves it out) and who was struck */
static int lightning_caster = -1;
static uint64_t lightning_until; /* (the gameplay clock's) */
uint64_t vp_gv(void); /* vplus/killcam.c: the gameplay clock */
void F_80107B00_orig(CPU *c);
void feat_Lightning(CPU *c) /* sub_80107B00(caster, human) */
{
    uint32_t car = c->r[4];
    int human = c->r[5] != 0;
    lightning_caster = r16s(car + (human ? 4u : 6u));
    lightning_until = vp_gv() + 180; /* (the gameplay clock: a step run again sees the same) */
    F_80107B00_orig(c);
}

/* ---- damage and the killing blow ---- */
static struct { int attacker, attack, rear; } blow[KEYS];
/* The last car that hurt each car (and with what, when): a car that dies with no killer (a fall, the arena) or by
 * its own weapons within LAST_HIT_VBLANKS of it was pushed there (Mr. Slam's throw off a roof, a ram over an edge):
 * that car gets the kill. Gameplay (the Juggernaut passes to the killer): kept with the kill cam's machine. */
#define LAST_HIT_VBLANKS 360 /* 6 seconds (a fall takes 1-2) */
static struct { int slot, attack; uint64_t at; } last_hit[KEYS];
uint64_t vp_gv(void); /* vplus/killcam.c: the gameplay clock */
void killcam_keep(void *p, size_t n);

static int victim_of(int id, uint32_t *rec, int *human)
{
    if (human_of_id(id) >= 0) { *rec = human_rec(human_of_id(id)); *human = 1; return human_of_id(id); }
    if (id >= 50 && id < 50 + AI_MAX) { *rec = ai_rec(id - 50); *human = 0; return HK + id - 50; }
    return -1;
}

void F_800E5628_orig(CPU *c);
void feat_Damage(CPU *c) /* sub_800E5628(victim id, amount, dir, type, attacker slot) */
{
    uint32_t ra = c->r[31], rec;
    int id = (int)c->r[4], human, attacker = (int16_t)mem_r16(c, c->r[29] + 16);
    int key = victim_of(id, &rec, &human);
    if (getenv("TM2PC_FEAT_DEBUG") && key < 0) fprintf(stderr, "feat damage: id %d in_battle %d\n", id, in_battle);
    if (key < 0 || !in_battle) {
        F_800E5628_orig(c);
        return;
    }
    /* the lightning's damage: its caster, before the checks below. The arcs' attacker (+2) is the last taser user's
     * (the game never sets it for the lightning, RETAIL_BUGS.md 7): once a computer Outlaw 2 had used her taser, the
     * player's lightning was a computer car's hit on computer cars, dropped (the user's Hong Kong lightning that hurt
     * only the first time, 2026-11-04). The game is given the caster too (the kill's credit). */
    if ((gr(ra) == 0x80108B50u || gr(ra) == 0x80108BA0u || gr_function(ra) == 0x801084D8u) && /* retail-address */
        vp_gv() < lightning_until && lightning_caster >= 0 && attacker != lightning_caster) {
        attacker = lightning_caster;
        mem_w16(c, c->r[29] + 16, (uint16_t)attacker);
    }
    if (key >= HK && attacker >= 0 && human_of_slot(attacker) < 0 && vp_ai_ignore_ai()) return; /* computer cars leave each other be */
    if (vp_team_blocks_damage(key, attacker)) return; /* a teammate's hit, without friendly fire */
    /* computer car on computer car: carApplyDamage divides by 4 (decomp AI_VS_AI.md 3); fighting each other, full */
    if (key >= HK && attacker >= 0 && human_of_slot(attacker) < 0 && vp_ai_vs_ai()) c->r[5] *= 4;
    int attack = A_OTHER, rear = 0;
    switch (gr(ra)) { /* (by retail address) */
    case 0x800DC7ACu: attack = cur_attack; rear = cur_rear; break;          /* a weapon hit */ /* retail-address */
    case 0x800E3A20u: attack = A_RAM; break; /* retail-address */
    case 0x800E7340u: attack = car_of_slot(attacker) == 0 ? A_SPECIAL : A_RAM; break; /* crush: Hammerhead's special */ /* retail-address */
    case 0x8010B7E4u: case 0x8010BA00u: attack = splash_attack; break; /* retail-address */
    case 0x80108B50u: case 0x80108BA0u: /* the taser's and the lightning's damage */ /* retail-address */
        if (vp_gv() < lightning_until && lightning_caster >= 0) { attack = A_LIGHTNING; attacker = lightning_caster; }
        else attack = A_SPECIAL;
        break;
    case 0x8010F8D0u: case 0x801069E8u: case 0x800F4C18u: case 0x8010FC98u: case 0x80110638u: /* retail-address */
        attack = A_SPECIAL; break; /* Grasshopper, Axel, Mr. Slam, Shadow, Dark Tooth */
    case 0x800E7C04u: case 0x8011C1A0u: attack = A_NONE; break; /* wreck blasts: not a hit */ /* retail-address */
    default: /* in a function the variant changed (its calls moved): by the function (tools/test/changed_sites.py) */
        switch (gr_function(ra)) {
        case 0x800E6D14u: attack = car_of_slot(attacker) == 0 ? A_SPECIAL : A_RAM; break; /* carCrashStart */ /* retail-address */
        case 0x800E3800u: attack = A_RAM; break;                         /* carContactRespond */ /* retail-address */
        case 0x8010B5C4u: attack = splash_attack; break;                 /* areaDamage */ /* retail-address */
        case 0x801084D8u: /* weaponArcsDraw: the taser's and the lightning's */ /* retail-address */
            if (vp_gv() < lightning_until && lightning_caster >= 0) { attack = A_LIGHTNING; attacker = lightning_caster; }
            else attack = A_SPECIAL;
            break;
        case 0x8010F520u: case 0x80105A7Cu: attack = A_SPECIAL; break; /* weaponHomingLeapUpdate, weaponFireSpecial */ /* retail-address */
        case 0x8010FD44u: attack = A_SPECIAL; break; /* aiDarkToothBossUpdate (Dark Tooth's) */ /* retail-address */
        case 0x800E75F4u: case 0x8011BE10u: attack = A_NONE; break;    /* carWreckUpdate, carDestroyed */ /* retail-address */
        default: { /* the retail bodies the variant moved to its own code (request 33-2a: one per car) */
            const char *fn = port_function_of(ra);
            if (fn && (!strcmp(fn, "weaponHomingLeapUpdateOne") || !strcmp(fn, "weaponShadowStrikeUpdateOne") ||
                       !strcmp(fn, "weaponTornadoUpdateOne")))
                attack = A_SPECIAL;
            break;
        }
        }
        break;
    }
    static int team_dbg = -1; /* tests: TM2PC_TEAM_DEBUG, each hit on a teammate with its attack */
    if (team_dbg < 0) team_dbg = getenv("TM2PC_TEAM_DEBUG") != NULL;
    if (team_dbg && attacker >= 0 && vp_team_of_slot(attacker) >= 0 && vp_team_of_slot(attacker) == vp_team_of_slot(r16s(rec + (human ? 4u : 6u))) &&
        attacker != r16s(rec + (human ? 4u : 6u)))
        fprintf(stderr, "team hit attack: slot %d -> slot %d, attack %d\n", attacker, r16s(rec + (human ? 4u : 6u)), attack);
    c->r[5] = (uint32_t)vp_mode_damage(r16s(rec + (human ? 4u : 6u)), attacker, attack == A_RAM, (int)c->r[5]);
    uint32_t hp_at = rec + (human ? 0x1F0u : 0x5A0u);
    int32_t before = r32s(hp_at);
    F_800E5628_orig(c);
    int32_t after = r32s(hp_at);
    if (getenv("TM2PC_FEAT_DEBUG"))
        fprintf(stderr, "feat damage: victim %d (key %d) hp %d -> %d attacker %d attack %d ra %08x\n", id, key, before,
                after, attacker, attack, ra);
    if (attack == A_NONE) return;
    int victim_slot = r16s(rec + (human ? 4u : 6u));
    if (attacker >= 0 && attacker != victim_slot && after < before) /* the tally: damage dealt (to a wreck: what it had) */
        match_damage(attacker, (before > 0 ? before : 0) - (after > 0 ? after : 0));
    /* who set it on fire (napalm, Thumper's flame): the burn's damage is theirs */
    if (attacker >= 0 && attacker != victim_slot && key < HK + AI_MAX &&
        (attack == A_NAPALM || (attack == A_SPECIAL && car_of_slot(attacker) == 5)))
        igniter[key] = attacker + 1;
    if (attacker >= 0 && attacker != victim_slot && after < before && key < HK + AI_MAX) {
        last_hit[key].slot = attacker;
        last_hit[key].attack = attack;
        last_hit[key].at = vp_gv() + 1; /* (0: none) */
    }
    if (attacker >= 0 && attacker == victim_slot && after < before && human) /* hurt by your own weapon */
        feat_set(key, FEAT_SELF_DAMAGE);
    if (before > 0 && after <= 0) { blow[key].attacker = attacker; blow[key].attack = attack; blow[key].rear = rear; }
}

/* ---- the match's tally ---- */
static MatchCar mcars[MATCH_MAX];
static int mslot[MATCH_MAX], mhome[MATCH_MAX], mcount, malive; /* each car's slot now, and at the bind */
void match_begin(int human_lives)
{
    mcount = 0;
    int h = (int)mem_r32(&M.cpu, GA(0x80181014u)), a = (int)mem_r32(&M.cpu, GA(0x8018101Cu)); /* humanCarTotal, aiCarTotal */
    for (int i = 0; i < h && i < HK && mcount < MATCH_MAX; i++) { /* every player */
        mcars[mcount] = (MatchCar){i, -1, car_index(r32s(human_rec(i) + 0x90)), 0, 0, 0, 0, human_lives < 1 ? 1 : human_lives};
        mslot[mcount++] = r16s(human_rec(i) + 4);
    }
    for (int i = 0; i < a && i < AI_MAX && mcount < MATCH_MAX; i++) {
        /* (a computer Dark Tooth, type 0xF0: car 14, his name on the results; car_index has him -1 for the feats') */
        mcars[mcount] = (MatchCar){-1, i, r32s(ai_rec(i) + 0x30) == 0xF0 ? 14 : car_index(r32s(ai_rec(i) + 0x30)), 0, 0, 0, 0,
                                   r32s(ai_rec(i) + 0x30) == 0xF0 || human_lives < 1 ? 1 : human_lives}; /* Dark Tooth: one */
        mslot[mcount++] = r16s(ai_rec(i) + 6);
    }
    malive = mcount;
    memcpy(mhome, mslot, sizeof mhome);
}
/* a car that changed cars (the Juggernaut's, vplus/fixes.c) is on another slot: its tally follows it */
void match_slot_moved(int from, int to)
{
    for (int i = 0; i < mcount; i++) if (mslot[i] == from) { mslot[i] = to; break; }
}
void match_slots_home(void) { memcpy(mslot, mhome, sizeof mslot); } /* a bind: every car its own again */
int match_cars(void) { return mcount; }
const MatchCar *match_car(int i) { return i >= 0 && i < mcount ? &mcars[i] : NULL; }
int match_alive(void) { return malive; }
void match_add_points(int slot, int n) { for (int i = 0; i < mcount; i++) if (mslot[i] == slot) mcars[i].points += n; }
int match_points_of(int slot) { for (int i = 0; i < mcount; i++) if (mslot[i] == slot) return mcars[i].points; return 0; }
void match_sample(void) /* tests (ui-shot): a played-out match */
{
    static const MatchCar sample[6] = {{0, -1, 7, 3, 1, 3, 812, 0}, {-1, 0, 9, 2, 0, 0, 640, 1}, {-1, 1, 3, 1, 1, 2, 455, 0},
                                       {-1, 2, 11, 0, 1, 4, 120, 0}, {-1, 3, 0, 1, 1, 5, 310, 0}, {-1, 4, 5, 0, 1, 6, 95, 0}};
    mcount = getenv("TM2PC_SHOT_CARS") ? atoi(getenv("TM2PC_SHOT_CARS")) : 6;
    if (mcount < 1 || mcount > MATCH_MAX) mcount = 6;
    for (int i = 0; i < mcount; i++) {
        mcars[i] = sample[i % 6];
        if (i >= 6) { mcars[i].human = -1; mcars[i].ai = i; mcars[i].car = (i * 5) % 14; mcars[i].place = i + 1; }
        mslot[i] = 100 + i;
    }
    malive = 1;
}
static void match_damage(int attacker, int amount)
{
    if (killcam_replaying) return; /* the kill cam: counted once */
    for (int i = 0; i < mcount; i++)
        if (mslot[i] == attacker && amount > 0) {
            mcars[i].damage += amount;
            if (mcars[i].human >= 0) stat_add(mcars[i].human, STAT_DAMAGE, amount); /* (the Stats screen: every match) */
        }
}
int vp_suicide_costs_kill(void); /* vplus/fixes.c */
static void match_destroyed(int own, int killer)
{
    int suicide = (killer < 0 || killer == own) && vp_suicide_costs_kill(); /* (its own weapons, a fall, the arena) */
    if (getenv("TM2PC_FEAT_DEBUG")) fprintf(stderr, "match: car %d destroyed by %d (suicide %d, %d cars) at %llu\n", own, killer, suicide, mcount,
                                                (unsigned long long)M.vblanks);
    for (int i = 0; i < mcount; i++) {
        if (mslot[i] == own) {
            mcars[i].deaths++;
            if (suicide) { /* Deathmatch: no dying on purpose to keep a kill from the others */
                mcars[i].kills--;
                fprintf(stderr, "deathmatch: car %d killed itself: %d kills\n", own, mcars[i].kills);
            }
            if (mcars[i].lives > 0) mcars[i].lives--;
            if (!mcars[i].lives && !mcars[i].place) mcars[i].place = malive--; /* out of lives: a place */
        }
        if (mslot[i] == killer && killer != own && !(vp_team_of_slot(killer) >= 0 && vp_team_of_slot(killer) == vp_team_of_slot(own)))
            mcars[i].kills++; /* (a teammate's wreck is no kill, friendly fire or not) */
    }
}

/* ---- a car destroyed (once per death) ---- */
void F_8011BE10_orig(CPU *c);
/* carDestroyed itself: a player's death has the scream and the explosion only while the game's roundWonFlag is clear;
 * with no computer cars (the co-op duel, a players-only match) it is set from the start and the port's rules decide the
 * round, so a player went down quietly, a broken car (the user's co-op take 2026-11-17): cleared for the call */
int vp_round_won_hidden(void); /* vplus/fixes.c */
static void destroyed_orig(CPU *c)
{
    const uint32_t won = GA(0x80180FB0u); /* roundWonFlag */
    uint32_t was = mem_r32(&M.cpu, won);
    int clear = c->r[5] != 0 && was && vp_round_won_hidden();
    if (clear) mem_w32(&M.cpu, won, 0);
    F_8011BE10_orig(c);
    if (clear) mem_w32(&M.cpu, won, was);
}
void feat_Destroyed(CPU *c) /* sub_8011BE10(record, human) */
{
    uint32_t rec = c->r[4];
    int human = c->r[5] != 0;
    if (in_battle) {
        if (killcam_replaying) { /* the kill cam's replay: counted once; the mode's rules again (its Juggernaut, targets) */
            int rkey = human ? r16s(rec) : HK + r16s(rec + 4), rown = r16s(rec + (human ? 4u : 6u));
            int rkiller = (int8_t)mem_r8(&M.cpu, rec + (human ? 0x153u : 0x503u));
            if ((rkiller < 0 || rkiller == rown) && rkey >= 0 && rkey < HK + AI_MAX && last_hit[rkey].at &&
                last_hit[rkey].slot != rown && vp_gv() + 1 - last_hit[rkey].at <= LAST_HIT_VBLANKS)
                rkiller = last_hit[rkey].slot; /* (as below: pushed off, its killer) */
            if (rkey >= 0 && rkey < HK + AI_MAX) last_hit[rkey].at = 0;
            vp_car_destroyed(rown, rkiller, A_OTHER); /* (its rules only: no feed, no tally in a replay) */
            mem_w8(&M.cpu, rec + (human ? 0x153u : 0x503u), 0xFF);
            destroyed_orig(c);
            return;
        }
        int key = human ? r16s(rec) : HK + r16s(rec + 4);
        int killer = (int8_t)mem_r8(&M.cpu, rec + (human ? 0x153u : 0x503u));
        int own = r16s(rec + (human ? 4u : 6u));
        if ((killer < 0 || killer == own) && key >= 0 && key < HK + AI_MAX && last_hit[key].at && last_hit[key].slot != own &&
            vp_gv() + 1 - last_hit[key].at <= LAST_HIT_VBLANKS) { /* pushed off, or finished by its own after a hit: theirs */
            killer = last_hit[key].slot;
            blow[key].attacker = killer;
            blow[key].attack = last_hit[key].attack;
            blow[key].rear = 0;
        }
        if (key >= 0 && key < HK + AI_MAX) last_hit[key].at = 0; /* (a new life: nothing yet) */
        match_destroyed(own, killer);
        killcam_destroyed(own, killer);
        vp_car_destroyed(own, killer, key >= 0 && key < HK + AI_MAX && blow[key].attacker == killer ? blow[key].attack : A_OTHER);
        if (human && key >= 0 && key < HK) {
            feat_add(key, FEAT_DEATHS, 1);
            if (mem_r8(&M.cpu, rec + 0xB6)) feat_set(key, FEAT_DIED_ON_FIRE);
            if (killer < 0 && mem_r8(&M.cpu, rec + 0xBB) && !mem_r8(&M.cpu, rec + 0xC0)) feat_add(key, FEAT_FALLS, 1);
            if (killer == own && blow[key].attacker == own) stat_add(key, STAT_SELF_KILLS, 1); /* (your own mine or weapon) */
        }
        int k = human_of_slot(killer);
        if (k >= 0 && killer != own && key >= 0 && key < HK + AI_MAX) {
            int attack = blow[key].attacker == killer ? blow[key].attack : A_OTHER;
            feat_add(k, FEAT_KILLS, 1);
            switch (attack) {
            case A_MG: feat_set(k, FEAT_MG_KILL); stat_add(k, STAT_MG_KILLS, 1); break;
            case A_FIRE: feat_add(k, FEAT_FIRE_KILLS, 1); feat_set(k, FEAT_FIRE_KILL); break;
            case A_HOMING: feat_add(k, FEAT_HOMING_KILLS, 1); feat_set(k, FEAT_HOMING_KILL); break;
            case A_POWER: feat_add(k, FEAT_POWER_KILLS, 1); feat_set(k, FEAT_POWER_KILL); break;
            case A_NAPALM: feat_set(k, FEAT_NAPALM_KILL); stat_add(k, STAT_NAPALM_KILLS, 1); break;
            case A_REMOTE: feat_set(k, FEAT_REMOTE_KILL); stat_add(k, STAT_REMOTE_KILLS, 1); break;
            case A_MINE: feat_set(k, FEAT_MINE_KILL); stat_add(k, STAT_MINE_KILLS, 1); break;
            case A_RICOCHET: feat_set(k, FEAT_RICOCHET_KILL); stat_add(k, STAT_RICOCHET_KILLS, 1); break;
            case A_LIGHTNING: feat_set(k, FEAT_LIGHTNING_KILL); stat_add(k, STAT_LIGHTNING_KILLS, 1); break;
            case A_RAM: feat_add(k, FEAT_RAM_KILLS, 1); break;
            case A_SPECIAL: {
                int car = car_index(r32s(human_rec(k) + 0x90));
                if (car >= 0) feat_set(k, (FeatFlag)(FEAT_SPECIAL_KILL + car));
                if (car >= 0 && car < 14) stat_add(k, (StatCounter)(STAT_SPECIAL_KILLS_CAR + car), 1);
                stat_add(k, STAT_SPECIAL_KILLS, 1);
                break;
            }
            }
            if (blow[key].attacker == killer && blow[key].rear) feat_set(k, FEAT_REAR_KILL);
        }
        if (key >= 0 && key < HK + AI_MAX) blow[key].attacker = -1;
        /* The game writes the killer only when a car with an attacker brings it down (carApplyDamage), and nothing of
         * its own reads it: kept, a fall or the car's own weapons in its next life would name this killer again.
         * Forgotten here: such a death has no killer (Deathmatch: a kill taken away). */
        mem_w8(&M.cpu, rec + (human ? 0x153u : 0x503u), 0xFF);
    }
    destroyed_orig(c);
}

/* ---- freezes: a missile's new freeze (stats +0x1C set), at the hit test ---- */
void F_800D6FD8_orig(CPU *c);
void feat_HitTest(CPU *c) /* sub_800D6FD8(pos, owner id, radius, type) */
{
    int owner = (int)c->r[5], type = (int)c->r[7];
    if (type != 14 || !in_battle) { F_800D6FD8_orig(c); return; }
    int before[KEYS];
    for (int i = 0; i < HK; i++) before[i] = i < humans() ? r16s(human_rec(i) + 0x168) : 0;
    for (int i = 0; i < AI_MAX; i++) before[HK + i] = r16s(ai_rec(i) + 0x518);
    F_800D6FD8_orig(c);
    int shooter = human_of_object(owner);
    for (int i = 0; i < HK + AI_MAX; i++) {
        int now = i < HK ? (i < humans() ? r16s(human_rec(i) + 0x168) : 0) : r16s(ai_rec(i - HK) + 0x518);
        if (now && !before[i]) {
            if (shooter >= 0) feat_add(shooter, FEAT_FREEZES, 1);
            if (i < humans() && owner != i + 1) feat_add(i, FEAT_FROZEN, 1);
        }
    }
}

/* ---- pickups ---- */
void F_8010AFC8_orig(CPU *c);
int vp_pickup_id(int id); /* vplus/fixes.c: a Challenge Match's pickups */
int vp_pickup_special(uint32_t car, int human);
void feat_Pickup(CPU *c) /* sub_8010AFC8(car, human, id, map slot) */
{
    int given = (int)c->r[6] >= 441 ? (int)c->r[6] - 40 : (int)c->r[6];
    if (given != 417 && given != 418 && given != 430 && given >= 401 && given <= 430) vp_gun_resupply(c->r[4], c->r[5] & 0xFF);
    int asked = (int)c->r[6];
    c->r[6] = (uint32_t)vp_pickup_id((int)c->r[6]);
    static int dbg = -1; /* (tests: TM2PC_PICKUP_DEBUG, each pickup taken: its kind and what it gave) */
    if (dbg < 0) dbg = getenv("TM2PC_PICKUP_DEBUG") != NULL;
    if (dbg) fprintf(stderr, "pickup: %s %d took kind %d at slot %d: gave %d\n", c->r[5] & 0xFF ? "player" : "computer car",
                     r16s(c->r[4] + (c->r[5] & 0xFF ? 0u : 2u)), asked, (int)c->r[7], (int)c->r[6]);
    if (c->r[6] == 400 || c->r[6] == 440) c->r[6] = (uint32_t)vp_pickup_special(c->r[4], c->r[5] & 0xFF) + (c->r[6] == 440 ? 40 : 0);
    if (c->r[5] && in_battle) {
        int h = r16s(c->r[4]), id = (int)c->r[6];
        if (h >= 0 && h < 2) {
            if (id == 430 || id == 470) feat_add(h, FEAT_TURBOS, 1);
            if (id == 417 || id == 418 || id == 457 || id == 458) feat_add(h, FEAT_HEALTHS, 1);
        }
    }
    int slot = (int)c->r[7];
    extern int vp_msg_player;
    vp_msg_player = c->r[5] & 0xFF ? r16s(c->r[4]) : -2; /* its messages: that player's (a computer car: none shown) */
    F_8010AFC8_orig(c);
    vp_msg_player = -1;
    vp_pickup_taken(slot);
}

/* ---- pedestrians ---- */
void F_80124384_orig(CPU *c);
void feat_Target(CPU *c) /* sub_80124384(kind, instance, state) */
{
    int kind = (int)c->r[4], instance = (int)c->r[5], state = (int)c->r[6];
    if (in_battle && state == 2 && (kind == 514 || kind == 516 || kind == 517)) {
        int who = -1;
        uint32_t rr = gr(c->r[31]);
        if (rr == 0x80126C80u || gr_function(c->r[31]) == 0x80126464u) who = (int16_t)c->r[18]; /* run over by a human (s2) */ /* retail-address */
        else if (rr == 0x80127344u) who = human_of_object(ped_hitter); /* a weapon */ /* retail-address */
        if (who >= 0 && who < humans()) {
            feat_add(who, FEAT_PEDESTRIANS, 1);
            if (kind == 516 && instance == 2 && mem_r8(&M.cpu, SETTINGS + 24) == 6) feat_set(who, FEAT_SANTA);
        }
    }
    F_80124384_orig(c);
}

/* ---- every frame: top speed, the turbo from full to empty ---- */
static int turbo_armed[HK];
void vp_battle_frame(void); /* vplus/fixes.c: a Challenge Match's timed rules */
void killcam_destroyed(int own, int killer);
void feats_frame(void)
{
    if (!in_battle) return;
    vp_battle_frame();
    { /* tools (decomp HIDDEN_AREAS.md 9.6): player 1 placed, a view, the chase camera logged each frame */
        static int n;
        uint32_t h = human_rec(getenv("TM2PC_CAM_PLAYER") ? atoi(getenv("TM2PC_CAM_PLAYER")) : 0); /* (TM2PC_CAM_PLAYER=k: player k + 1) */
        int x, y, z, yaw, at;
        if (getenv("TM2PC_TEST_TELEPORT") && sscanf(getenv("TM2PC_TEST_TELEPORT"), "%d,%d,%d,%d@%d", &x, &y, &z, &yaw, &at) == 5 &&
            n == at) { /* TM2PC_TEST_TELEPORT=x,y,z,yaw@frame (position units; yaw: rot[2], 4096 a turn) */
            mem_w32(&M.cpu, h + 0x6FC, (uint32_t)x);
            mem_w32(&M.cpu, h + 0x700, (uint32_t)y);
            mem_w32(&M.cpu, h + 0x704, (uint32_t)z);
            mem_w32(&M.cpu, h + 0x710, (uint32_t)yaw);
        }
        static int scan[6] = {-1}; /* tests: TM2PC_TEST_SCAN=cx,cy,radius,step,z@frame: player 1 swept over a grid, 3 frames a spot */
        if (scan[0] == -1) {
            scan[0] = -2;
            if (getenv("TM2PC_TEST_SCAN")) sscanf(getenv("TM2PC_TEST_SCAN"), "%d,%d,%d,%d,%d@%d", &scan[0], &scan[1], &scan[2], &scan[3], &scan[4], &scan[5]);
        }
        if (scan[0] != -2 && n >= scan[5]) {
            int side = 2 * scan[2] / scan[3] + 1, k = (n - scan[5]) / 3;
            if (k < side * side) {
                mem_w32(&M.cpu, h + 0x6FC, (uint32_t)(scan[0] - scan[2] + (k % side) * scan[3]));
                mem_w32(&M.cpu, h + 0x700, (uint32_t)(scan[1] - scan[2] + (k / side) * scan[3]));
                mem_w32(&M.cpu, h + 0x704, (uint32_t)scan[4]);
                mem_w8(&M.cpu, h + 0x9C, 0); /* (alive) */
                mem_w32(&M.cpu, h + 0x1F0, 300); /* (its health kept) */
            }
        }
        if (getenv("TM2PC_TEST_VIEW") && n >= 30) mem_w32(&M.cpu, h + 0x10C, (uint32_t)atoi(getenv("TM2PC_TEST_VIEW")));
        static FILE *trace;
        if (getenv("TM2PC_CAM_TRACE") && !trace) trace = fopen(getenv("TM2PC_CAM_TRACE"), "w");
        if (trace) { /* frame, view, the camera, its look-at point, the car, its yaw */
            fprintf(trace, "%d %d  %d %d %d  %d %d %d  %d %d %d  %d\n", n, r32s(h + 0x10C), r32s(h + 0x14), r32s(h + 0x18),
                    r32s(h + 0x1C), r32s(h + 0x38), r32s(h + 0x3C), r32s(h + 0x40), r32s(h + 0x6FC), r32s(h + 0x700),
                    r32s(h + 0x704), r32s(h + 0x710));
            fflush(trace);
        }
        n++;
    }
    if (getenv("TM2PC_LA_DEBUG")) { /* tools: the LA quake records and where the player is */
        static int n;
        if (getenv("TM2PC_LA_TELEPORT") && n == 30) { /* tools: the player's car near the quake zone */
            uint32_t h = human_rec(0);
            mem_w32(&M.cpu, h + 0x6FC, (uint32_t)atoi(getenv("TM2PC_LA_TELEPORT")));
            mem_w32(&M.cpu, h + 0x700, (uint32_t)-1400);
        }
        if (getenv("TM2PC_LA_QUAKE") && n == atoi(getenv("TM2PC_LA_QUAKE"))) { /* tools: quake 1 started (laEventStart) */
            fprintf(stderr, "la quake: started at frame %d\n", n);
            guest_call(GA(0x800D58BCu), 1, 0, 0, 0);
        }
        if (getenv("TM2PC_LA_QUAKE") && n % 10 == 0) {
            uint32_t r = GA(0x801AB438u) + 196;
            if (mem_r8(&M.cpu, r + 1))
                fprintf(stderr, "la quake: frame %d timer %d lanes %u %u starts %d %d lengths %d %d end %u\n", n, r16s(r + 10),
                        mem_r8(&M.cpu, r + 6), mem_r8(&M.cpu, r + 7), r16s(r + 16), r16s(r + 18), r16s(r + 26), r16s(r + 28),
                        mem_r8(&M.cpu, r + 2));
        }
        if (n++ % 300 == 0) {
            for (int k = 1; k < 3; k++) {
                uint32_t r = GA(0x801AB438u) + (uint32_t)k * 196;
                fprintf(stderr, "la event %d: on %u active %u done %u section %d obj %08x %08x pos1 %d %d %d pos2 %d %d %d\n", k,
                        mem_r8(&M.cpu, r), mem_r8(&M.cpu, r + 1), mem_r8(&M.cpu, r + 4), r16s(r + 12), mem_r32(&M.cpu, r + 40),
                        mem_r32(&M.cpu, r + 44), r32s(r + 68), r32s(r + 72), r32s(r + 76), r32s(r + 80), r32s(r + 84), r32s(r + 88));
            }
            static int mapped;
            if (!mapped) { /* the arena's sections on a grid (x, y; 200 units) */
                mapped = 1;
                FILE *mf = fopen(getenv("TM2PC_LA_DEBUG"), "w");
                for (int y = -16000; mf && y <= 16000; y += 200) {
                    for (int x = -16000; x <= 16000; x += 200)
                        fprintf(mf, "%d ", (int)(int16_t)guest_call(GA(0x800EFF34u), (uint32_t)x, (uint32_t)y, 256, 0));
                    fprintf(mf, "\n");
                }
                if (mf) fclose(mf);
            }
            uint32_t ob = mem_r32(&M.cpu, GA(0x801AB438u) + 196 + 44);
            if (ob) {
                fprintf(stderr, "la obj:");
                for (int i = 0; i < 48; i++) fprintf(stderr, " %d", r32s(ob + (uint32_t)i * 4));
                fprintf(stderr, "\n");
            }
            uint32_t h = human_rec(0);
            fprintf(stderr, "la player at %d %d %d section %u\n", r32s(h + 0x6FC), r32s(h + 0x700), r32s(h + 0x704),
                    guest_call(GA(0x800EFF34u), (uint32_t)r32s(h + 0x6FC), (uint32_t)r32s(h + 0x700), (uint32_t)r32s(h + 0x704), 0));
        }
    }
    static int weak = -1; /* tests: TM2PC_FEAT_DEBUG=2 keeps the computer cars at 1 HP */
    if (weak < 0) weak = getenv("TM2PC_FEAT_DEBUG") && atoi(getenv("TM2PC_FEAT_DEBUG")) == 2;
    for (int i = 0; weak && i < AI_MAX; i++)
        if (r32s(ai_rec(i) + 0x5A0) > 1) mem_w32(&M.cpu, ai_rec(i) + 0x5A0, 1);
    { /* time in matches (the Stats screen): whole seconds, gaps over half a second (a pause, a menu) left out */
        static uint64_t last, ms;
        uint64_t now = SDL_GetTicks();
        if (last && now - last < 500) ms += now - last;
        last = now;
        if (ms >= 1000) {
            for (int i = 0; i < humans(); i++) stat_add(i, STAT_SECONDS, (int)(ms / 1000));
            ms %= 1000;
        }
    }
    for (int i = 0; i < humans(); i++) {
        uint32_t rec = human_rec(i);
        int speed = abs(r16s(rec + 0x6E6)), cap = r32s(rec + 0x204);
        if (cap > 0 && speed >= cap) {
            feat_set(i, FEAT_TOP_SPEED);
            if (car_index(r32s(rec + 0x90)) == 0) feat_set(i, FEAT_TOP_SPEED_HAMMERHEAD);
        }
        int turbo = r16s(rec + 0x7FA);
        if (turbo >= 999) turbo_armed[i] = 1;
        else if (turbo <= 0 && turbo_armed[i]) { turbo_armed[i] = 0; feat_set(i, FEAT_TURBO_EMPTIED); }
    }
}

/* ---- burning: its drain credited to whoever set the car on fire ----
 * groundFiresUpdate (8010C160) takes 1 HP in 4 frames from a burning car
 * (stops at 5: it cannot kill), outside the damage function, with no owner
 * (decomp GAME_EVENTS.md 9.4). The port notes the last napalm or flame hit
 * on each car (feat_Damage) and credits the drain to it. */
void F_8010C160_orig(CPU *c);
void feat_GroundFires(CPU *c)
{
    int32_t before[KEYS];
    int h = humans(), a = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    static long long burn_test = -2; /* tests: TM2PC_TEST_BURN=V, every computer car kept burning from VBlank V, their HP logged */
    if (burn_test == -2) burn_test = getenv("TM2PC_TEST_BURN") ? atoll(getenv("TM2PC_TEST_BURN")) : -1;
    if (burn_test >= 0 && in_battle && M.vblanks >= (uint64_t)burn_test) {
        for (int k = 0; k < h; k++) { /* (the players too: state +0x9C + 0x1A burning, stats +0x14C + 0x22 its time) */
            mem_w8(&M.cpu, human_rec(k) + 0xB6, 1);
            mem_w16(&M.cpu, human_rec(k) + 0x16E, 0);
            if (M.vblanks % 120 == 0) fprintf(stderr, "burn test: player %d (slot %d) hp %d\n", k + 1, r16s(human_rec(k) + 4),
                                              r32s(human_rec(k) + 0x1F0));
        }
        for (int k = 0; k < a && k < AI_MAX; k++) {
            mem_w8(&M.cpu, ai_rec(k) + 0x086, 1);
            mem_w16(&M.cpu, ai_rec(k) + 0x51E, 0);
            if (M.vblanks % 120 == 0) fprintf(stderr, "burn test: computer car %d (slot %d) hp %d\n", k, r16s(ai_rec(k) + 6),
                                              r32s(ai_rec(k) + 0x5A0));
        }
    }
    for (int i = 0; i < HK + AI_MAX; i++)
        before[i] = i < HK ? (i < h ? r32s(human_rec(i) + 0x1F0) : 0) : (i - HK < a ? r32s(ai_rec(i - HK) + 0x5A0) : 0);
    F_8010C160_orig(c);
    if (!in_battle) return;
    for (int i = 0; i < HK + AI_MAX; i++) {
        int32_t now = i < HK ? (i < h ? r32s(human_rec(i) + 0x1F0) : 0) : (i - HK < a ? r32s(ai_rec(i - HK) + 0x5A0) : 0);
        if (now < before[i]) { /* the Juggernaut's toughness on the burn too (it bypasses carApplyDamage, where
                                * vp_mode_damage takes it: a 400% Juggernaut burned as fast as anyone, KinggzZ's 2026-11-24) */
            uint32_t rec = i < HK ? human_rec(i) : ai_rec(i - HK);
            int taken = vp_mode_damage(r16s(rec + (i < HK ? 4u : 6u)), -1, 0, before[i] - now);
            if (taken != before[i] - now) {
                now = before[i] - taken;
                mem_w32(&M.cpu, rec + (i < HK ? 0x1F0u : 0x5A0u), (uint32_t)now);
            }
        }
        if (now < before[i] && igniter[i] > 0) match_damage(igniter[i] - 1, before[i] - now);
    }
}

/* Kept with the machine (the kill cam's keep list: its snapshots, the network's states, rollback's): who hit whom
 * (each car's attacker, the projectiles' tags, the burns, the last hits) and the match's tally (Gun Game's levels come
 * from it): every machine's the same, a machine joining a match gets the host's (vplus/fixes.c's vp_keep_register) */
void feats_keep_register(void)
{
#define KEEP(v) killcam_keep(&(v), sizeof(v))
    KEEP(last_hit); KEEP(igniter); KEEP(tags); KEEP(lives_before); KEEP(cur_attack); KEEP(cur_rear); KEEP(cur_attacker_override);
    KEEP(ped_hitter); KEEP(splash_attack); KEEP(eiffel_pending); KEEP(lightning_caster); KEEP(lightning_until); KEEP(blow);
    KEEP(mcars); KEEP(mslot); KEEP(mhome); KEEP(mcount); KEEP(malive);
#undef KEEP
}
/* Rollback alone (net/rb_state.c) puts these back with a step's state, so a step run again counts as it first did:
 * this machine's players' stats (feats.c) and the turbo feat's arming (each machine its own, not sent) */
void rbs_keep(void *p, size_t n);
void feats_stats_rb_register(void); /* feats.c */
void feats_rb_register(void)
{
    rbs_keep(turbo_armed, sizeof turbo_armed);
    feats_stats_rb_register();
}
