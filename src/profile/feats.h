/* Feats: what a profile has done in matches (kills by weapon, deaths, falls,
 * freezes, pedestrians, pickups, top speed...), counted for the portrait
 * unlocks (docs/PORTRAIT_UNLOCKS.md). Player 1's are the signed-in
 * profile's, player 2's the profile player 2 signed in with (guests count
 * nothing). Kept in the profile file's [feats] section with a check, as the
 * progress. The game events come from feat_hooks.c (decomp GAME_EVENTS.md). */
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef enum {
    FEAT_KILLS,          /* cars destroyed, any way */
    FEAT_RAM_KILLS,
    FEAT_DEATHS,
    FEAT_FALLS,          /* fell off the map */
    FEAT_FREEZES,        /* enemies frozen with a freeze missile */
    FEAT_FROZEN,         /* frozen by an enemy's */
    FEAT_PEDESTRIANS,
    FEAT_TURBOS,         /* turbo pickups */
    FEAT_HEALTHS,        /* health pickups */
    FEAT_FIRE_KILLS,     /* fire missile */
    FEAT_HOMING_KILLS,
    FEAT_POWER_KILLS,
    FEAT_COUNTERS
} FeatCounter;

typedef enum {
    FEAT_SPECIAL_KILL,   /* + car-select index (14): a kill with the driver's own special */
    FEAT_REMOTE_KILL = FEAT_SPECIAL_KILL + 14,
    FEAT_MINE_KILL,
    FEAT_LIGHTNING_KILL,
    FEAT_NAPALM_KILL,
    FEAT_RICOCHET_KILL,
    FEAT_MG_KILL,        /* machine guns */
    FEAT_REAR_KILL,      /* a weapon fired backwards */
    FEAT_FIRE_KILL,      /* one of each pickup weapon, for "all weapons" */
    FEAT_HOMING_KILL,
    FEAT_POWER_KILL,
    FEAT_TOP_SPEED,
    FEAT_TOP_SPEED_HAMMERHEAD,
    FEAT_TURBO_EMPTIED,  /* the turbo used from full (999) to empty */
    FEAT_SELF_DAMAGE,
    FEAT_DIED_ON_FIRE,
    FEAT_SANTA,
    FEAT_EIFFEL,
    FEAT_TOURNAMENT_STARTED,
    FEAT_CUSTOM_FINISHED,  /* a custom battle (for now: a challenge match) played to its end */
    FEAT_LOST_ALL_LIVES,
    FEAT_TOURNAMENT_WON,
    FEAT_FLAGS
} FeatFlag;

/* The Stats screen's counters (profile [stats], its own check): two sets, local play and online play (the screen's
 * third page their sum); what a match counts goes to the set of the play it is (stats_online). */
typedef enum {
    STAT_KILLS, STAT_DEATHS, STAT_FALLS,
    STAT_MG_KILLS, STAT_FIRE_KILLS, STAT_HOMING_KILLS, STAT_POWER_KILLS, STAT_NAPALM_KILLS, STAT_REMOTE_KILLS,
    STAT_MINE_KILLS, STAT_RICOCHET_KILLS, STAT_LIGHTNING_KILLS, STAT_SPECIAL_KILLS, STAT_RAM_KILLS,
    STAT_FREEZES, STAT_FROZEN, STAT_PEDESTRIANS, STAT_TURBOS, STAT_HEALTHS,
    STAT_MATCHES,          /* custom matches and Survival played to their end */
    STAT_MATCH_WINS,       /* custom matches won (alone or the team) */
    STAT_SURVIVAL_BEST,    /* the most Survival rounds survived (a best, not a sum) */
    STAT_TOURNAMENTS,      /* tournaments started */
    STAT_TOURNAMENT_WINS,  /* tournaments won, any difficulty */
    STAT_SECONDS,          /* time in matches */
    STAT_V1_COUNT,         /* (the first set: every one in the check; those after, in it only when not 0) */
    STAT_SURVIVAL_TIME = STAT_V1_COUNT, /* the fastest Survival run won, seconds (a best, the lowest; 0 none) */
    STAT_SELF_KILLS,       /* destroyed by your own weapon (a mine, a missile's blast: the portrait goals, 2026-10-30) */
    STAT_SPECIAL_KILLS_CAR, /* + car-select index (14): kills with that driver's own special */
    STAT_VERSUS_GOLD = STAT_SPECIAL_KILLS_CAR + 14, /* Versus Tournaments finished first, second, third (2026-11-07) */
    STAT_VERSUS_SILVER, STAT_VERSUS_BRONZE,
    STAT_VERSUS_PLAYED,    /* Versus Tournaments played to the end (2026-11-12) */
    STAT_BOSSES_BEATEN,    /* Versus boss rounds won against the boss (the stage's winner) */
    STAT_BOSS_WINS,        /* Versus boss rounds won as the boss */
    STAT_JOINS,            /* online: matches joined while they were on (watched, then in) */
    STAT_DAMAGE,           /* damage dealt to other cars, every match (health points) */
    STAT_COUNT
} StatCounter;
enum { STATS_LOCAL, STATS_ONLINE, STATS_MODES };

typedef struct {
    uint32_t count[FEAT_COUNTERS];
    uint64_t flags;
    uint32_t stat[STATS_MODES][STAT_COUNT];
} Feats;

/* player 0 or 1 (a human's index in the match) */
void feat_add(int player, FeatCounter c, int n);
void feat_set(int player, FeatFlag f);
uint32_t feat_count(int player, FeatCounter c);
int feat_has(int player, FeatFlag f);
int feat_all_weapons(int player); /* a kill with each weapon: machine guns and the 7 pickups */
void feats_flush(void);           /* writes what changed to the profiles */
void feats_battle(int on);        /* feat_hooks.c: a match is running (events count only then) */
int feats_in_battle(void);        /* feat_hooks.c: a match is running */
void stat_add(int player, StatCounter s, int n); /* to the set of the play it is */
void stat_best(int player, StatCounter s, int v); /* a best: kept when higher */
void stat_best_low(int player, StatCounter s, int v); /* a best: kept when lower (0: none yet) */
void stats_online(int on);        /* online play (the online set) or local */
void feats_frame(void);           /* feat_hooks.c: once a frame (top speed, the turbo) */

/* The match's tally (feat_hooks.c): every car in it, from the records at the
 * round's start; kills and deaths from each destroyed car (the killer's
 * slot, GAME_EVENTS.md), places in the order the cars are out. */
typedef struct {
    int human;                    /* a human player (index), or -1 */
    int ai;                       /* a computer car (index), or -1 */
    int car;                      /* car-select index, -1 unknown */
    int kills, deaths, place;     /* place: 0 still in */
    int damage;                   /* dealt to other cars (health points) */
    int lives;                    /* left (a place once 0) */
    int points;                   /* the mode's: Hunted, Juggernaut points; Gun Game the weapon reached */
} MatchCar;
enum { MATCH_MAX = 32 }; /* the match tally's room: players and computer cars (16 + 16 at most online) */
void match_slot_moved(int from, int to); /* a car now on another slot (a car swap) */
void match_slots_home(void);             /* every car on its own slot again (a bind) */
void match_begin(int human_lives); /* at the round's start (rtMainLoop); the players' lives (computer cars: 1) */
int match_cars(void);
const MatchCar *match_car(int i);
int match_alive(void);
void match_add_points(int slot, int n); /* the car with this object slot */
int match_points_of(int slot);
void match_sample(void);          /* tests: a played-out match */

/* profile.c: the [feats] section */
Feats *feats_of(int player);
void feats_clear(int player);
void feats_key(int player, const char *key, const char *val);
int feats_check_ok(int player);   /* after a read: the check matches (none read: nothing counted yet) */
void feats_write(FILE *f, int player); /* [feats] and [stats] */
void stats_key(int player, const char *key, const char *val);
enum { FEATS_VIEW = 4 }; /* the slot a profile is read into for the Stats screen (nothing counts in it) */
int profile_read_stats(const char *who, Feats *out); /* profile.c: a profile's feats and stats, checked (0: none) */
