/* Challenge Match (battle.c): game types and the match settings, kept in
 * the profile. A game type is a rule set for one mode (built in and locked,
 * or the player's own); the match settings (arena, opponents, lives...) are
 * chosen for each match and never saved in a game type. Settings the port
 * cannot play yet are kept too and shown gray; docs/CUSTOM_BATTLE.md. */
#pragma once
#include <stdio.h>

enum { BATTLE_ARENAS = 8, BATTLE_SECRET_ARENAS = 3 }; /* arena: 0 random, 1..8 the tournament's, 9..11 the secret ones */
enum { MODE_LAST_CAR, MODE_DEATHMATCH, MODE_JUGGERNAUT, MODE_GUN_GAME, MODE_ODDBALL, MODE_KING_OF_THE_HILL, MODE_COUNT };
enum { BATTLE_OPPONENT_SLOTS = 15, BATTLE_OPPONENT_CARS = 12 }; /* opponents a match setup keeps (16 cars; the build plays
                                                              * cars_ai_max(), vplus/cars.h); cars 0..11 can be opponents */
enum { BATTLE_DRIVERS = 14 };
enum { BATTLE_WEAPONS = 7 };           /* the pickups: homing, fire, power, napalm, remote, ricochet, lightning */
enum { ENERGY_FREEZE, ENERGY_MINES, ENERGY_NAPALM, ENERGY_SHIELD, ENERGY_INVISIBILITY, ENERGY_VOLLEY, ENERGY_COUNT };
enum { HAZARD_ICE, HAZARD_LAVA, HAZARD_VEHICLES, HAZARD_COUNT };
enum { DEST_LA_QUAKE, DEST_EIFFEL, DEST_MOSCOW_DISH, DEST_NEW_YORK, DEST_HOLLAND, DEST_COUNT };
enum { FALL_DEATH, FALL_DAMAGE_10, FALL_DAMAGE_25, FALL_DAMAGE_50, FALL_DAMAGE_75, FALL_NONE, FALL_COUNT };
enum { PICKUPS_STANDARD, PICKUPS_FASTER, PICKUPS_SLOWER, PICKUPS_NEVER, PICKUPS_COUNT };
enum { SPECIAL_NONE, SPECIAL_HALF, SPECIAL_NORMAL, SPECIAL_DOUBLE, SPECIAL_QUADRUPLE, SPECIAL_QUARTER, SPECIAL_TRIPLE,
       SPECIAL_COUNT }; /* recharge speed (none: from pickups only; 0.25x and 3x added last, the saved numbers kept) */
enum { BATTLE_NAME_MAX = 16, BATTLE_DESC_MAX = 48, BATTLE_USER_TYPES = 24 };
enum { AI_TARGETS_GAME, AI_TARGETS_EQUAL, AI_TARGETS_HUMANS, AI_TARGETS_COUNT }; /* computer cars: the game's, everyone equal, humans only */

typedef struct {
    char name[BATTLE_NAME_MAX + 1];
    char desc[BATTLE_DESC_MAX + 1];    /* the player's own words (their game types) */
    int mode;                          /* MODE_x */
    int respawn;                       /* seconds */
    unsigned drivers;                  /* bit per car: can be picked */
    int dark_tooth;                    /* Dark Tooth drawn as a random opponent (his first phase; once unlocked) */
    int minion_random;                 /* Minion drawn as a random opponent (once unlocked) */
    int teams, friendly_fire;          /* teams: 0 off, else 2..4 */
    unsigned weapons;                  /* bit per pickup weapon: on */
    int ammo[BATTLE_WEAPONS];          /* per pickup (1..9; the game: 2, remote bombs 1) */
    int refill[BATTLE_WEAPONS];        /* auto-refill every N seconds (0 off) */
    int infinite, roulette, roulette_count; /* roulette: every N seconds (0 off), N of the weapon */
    int roulette_add;                  /* 0: each switch replaces the weapons (pickups give the roulette's); 1: adds to them */
    unsigned energy;                   /* bit per ENERGY_x: on */
    int health, turbo, pickup_respawn; /* pickup_respawn: PICKUPS_x */
    int specials, special_recharge, special_pickups; /* recharge: SPECIAL_x; pickups: a turned-off pickup may give one */
    int special_ammo;                  /* specials a special pickup gives */
    int ai_targets;                    /* AI_TARGETS_x */
    unsigned hazards, destructibles;   /* bit per HAZARD_x / DEST_x: on (the pages switch them all together) */
    int falloff;                       /* FALL_x */
    int spawn_shield;                  /* seconds of shield after a respawn (0 none) */
    int jugg_health;                   /* the Juggernaut's toughness, % (damage taken scaled by 100 / it) */
    int jugg_full_heal;                /* full health on becoming the Juggernaut */
    int jugg_weapon, jugg_ram;         /* the Juggernaut's weapon and ram damage, % */
    int jugg_infinite;                 /* the Juggernaut's weapons do not run out */
    int jugg_force, jugg_car;          /* the Juggernaut forced into a car: on/off, and which (car-select order; 13 Minion) */
    unsigned gun_levels;               /* Gun Game: bit per level (GUN_x): on */
    unsigned gun_specials;             /* Gun Game: the cars' specials as levels too, bit (type >> 4) of BATTLE_SPECIAL_AS (any car
                                        * fires it: the decomp's TM2_PORT_SPECIAL_AS); after the weapons (strongest first: before) */
    int gun_order;                     /* the weapons: 0 weakest first, 1 strongest first (then the ram kill, the machine gun) */
    int gun_infinite, gun_ammo;        /* the level's weapon never runs out; else its ammo (pickups refill it) */
    int hunted_count, hunted_attack;   /* Hunted: 1-4 targets; they may fire (the machine gun and ramming always) */
    int koth_move;                     /* King of the Hill: the hill moves every N minutes (0 never) */
    int koth_hills;                    /* King of the Hill: hills at once, 1..3 */
    int koth_spots;                    /* King of the Hill: 0 the arena's chosen spots (vplus/hill_spots.c), 1 any route point */
    int hunted_teams;                  /* Hunted with teams: 0 one by one (a target's killer, of another team, the next; both
                                        * teams may have targets), 1 swap team (the targets all on one team; a kill of one by
                                        * another team: the killer and others of its team at random the targets) */
    int jugg_invisible;                /* the Juggernaut shimmers, barely seen (the game's invisibility fade, held) */
    int sudden_death;                  /* Last Car Standing: two cars left, a countdown of this many seconds (0 off) */
    int sudden_death_end;              /* what its end does: SUDDEN_x */
    int sudden_death_start;            /* when the countdown starts: SD_START_x (the user's 2026-11-27) */
    int team_lives;                    /* Last Car Standing with teams: each team's extra lives pooled (the user's 2026-11-27) */
    int mines;                         /* how long a mine stays: MINES_x (decomp request 127) */
} GameType;
enum { MINES_AFTER_DEATH, MINES_AFTER_LAID, MINES_NEVER, MINES_COUNT }; /* removed 15 s after its owner's car is destroyed (the
                                                            * default), 15 s after it was laid, or never (the user's
                                                            * 2026-11-27: back as a choice) */
enum { SUDDEN_LOWER_HEALTH, SUDDEN_LEAST_DAMAGE, SUDDEN_END_COUNT }; /* sudden death's end: the car with less health (by its
                                                  * share) explodes, or the one that has dealt the least damage (the user's
                                                  * 2026-11-27) */
enum { SD_START_TWO_LEFT, SD_START_MATCH, SD_START_COUNT }; /* the countdown: once two cars are left, or from the match's
                                                  * start (at zero the worst car of all still in goes, then again) */
/* Gun Game's levels: the pickup weapons, weakest first; then a ram kill and the machine gun, last */
enum { GUN_FIRE, GUN_HOMING, GUN_RICOCHET, GUN_REMOTE, GUN_LIGHTNING, GUN_NAPALM, GUN_POWER, GUN_RAM, GUN_MG, GUN_COUNT,
       GUN_WEAPONS = GUN_RAM };
extern const char *const battle_gun_names[GUN_COUNT];
/* the specials any car can fire (the decomp's TM2_PORT_SPECIAL_AS_MASK: not Hammerhead, Grasshopper, Twister, Mr. Slam,
 * Dark Tooth), and those whose car file a match must load without that car in it (Sweet Tooth, Mr. Grimm, Spectre,
 * Roadkill, Axel, Shadow: portCarSpecialLoad) */
#define BATTLE_SPECIAL_AS 0x6BBAu
#define BATTLE_SPECIAL_FILES 0x2B22u
extern const char *const battle_special_names[16]; /* by type >> 4: the car whose special */
/* Gun Game's specials (type >> 4), strongest first by their most damage on one car (the decomp's CAR_STATS.md 6):
 * Thumper 47, Mr. Grimm 40, Roadkill 40, Minion 37, Shadow 25, Outlaw 2 20, Warthog 18, Sweet Tooth 15, Axel 15, Spectre 12 */
enum { BATTLE_SPECIAL_AS_COUNT = 10 };
extern const int battle_special_order[BATTLE_SPECIAL_AS_COUNT];

typedef struct {
    char game_type[BATTLE_NAME_MAX + 1]; /* by name: a built-in one or the profile's */
    int arena;
    int opponents;                     /* BATTLE_OPPONENT_SLOTS: as many as the arena holds (Full) */
    int opponent[BATTLE_OPPONENT_SLOTS];/* -1 random, else the car */
    int difficulty;                    /* 0..2 */
    int lives, kills, time_limit;      /* kills: 0 off; minutes: 0 off */
    int points;                        /* King of the Hill's and Hunted's points to win (a point a second): 0 off */
    int teams;                         /* 0 off, else 2..4 (Last Car Standing) */
    int team[BATTLE_OPPONENT_SLOTS];    /* each opponent's team (0 red, 1 blue, 2 green, 3 gold) */
    int player_team[16];               /* the players' teams, by human (the profile keeps the 4 of this PC's players) */
    int no_friendly_fire;              /* teams: teammates' weapons do not hurt (default: they do) */
    /* the match's own rules (Challenge Match: Drivers, Opponents > CPU logic, Arena hazards; the user's 2026-10-29), over the
     * game type's (battle_match_type); rules_set 0: the game type's (a profile from before them) */
    int rules_set;
    unsigned drivers;
    int dark_tooth, minion_random, ai_targets;
    unsigned hazards, destructibles;
    int falloff;
} MatchSetup;
enum { BATTLE_TEAM_COLORS = 4 };
extern const char *const battle_team_names[BATTLE_TEAM_COLORS]; /* "RED", "BLUE", "GREEN", "YELLOW" */
int battle_goal(const MatchSetup *m, int mode); /* what wins: the kills, or King of the Hill's and Hunted's points (0 off) */
int battle_match_teams(void); /* the match's teams (0 off; only Last Car Standing and Deathmatch have them) */

extern const char *const battle_mode_names[MODE_COUNT];

void battle_type_defaults(GameType *t, int mode); /* the game's own rules */
/* online: what a game type can never have there (Minion's volley and invisibility, the user's 2026-10-17); the host
 * applies it to the rules it sends (docs/ONLINE_PLAN.md) */
#define BATTLE_ONLINE_NEVER_ENERGY ((1u << ENERGY_VOLLEY) | (1u << ENERGY_INVISIBILITY))
void battle_online_rules(GameType *t);
int battle_type_playable(const GameType *t);       /* uses only what is made */

int battle_builtin_count(void);
const GameType *battle_builtin(int i);
const char *battle_builtin_desc(int i);

int battle_user_count(void);
GameType *battle_user(int i);
int battle_user_add(const GameType *t);            /* index, or -1 (full) */
void battle_user_delete(int i);
int battle_type_find(const char *name, int *user); /* the index (*user: 1 the profile's), or -1 */
/* the other signed-in players' game types (profile.c reads them; shown in the list, played, not edited here) */
void battle_guests_clear(void);
void battle_guest_key(int owner, const char *who, int n, const char *key, const char *val); /* owner 0-2, [game_type<n>] */
int battle_guest_count(void);
const GameType *battle_guest(int i);
const char *battle_guest_owner(int i);
const GameType *battle_type_named(const char *name); /* NULL: none */

MatchSetup *battle_match(void);                    /* the match being set (a custom tournament's stage while one runs) */
void battle_match_defaults(MatchSetup *m);
void battle_match_override(MatchSetup *m);         /* battle_match() is this one (NULL: the profile's again) */
void battle_type_override(const GameType *t);      /* battle_match_type() is this one (NULL: by the match's name again) */
const GameType *battle_type_overridden(void);      /* the one set (NULL: none) */

/* Custom Tournament (Local Play > Tournament): its stages in order, kept in the profile ([custom_tournament]) */
enum { TOUR_STAGES_MIN = 6, TOUR_STAGES_MAX = 8, TOUR_BOSSES_MAX = 2 };
typedef struct {
    int arena;                         /* as MatchSetup's: 0 random, 1..8, 9..11 the secret ones */
    int enemies;                       /* 0 random, else how many (the arena's room caps it) */
    int bosses;                        /* 0..TOUR_BOSSES_MAX: they arrive once the stage's enemies are out */
    int boss[TOUR_BOSSES_MAX];         /* each the car (car-select order: 13 Minion, 14 Dark Tooth) */
} TourStage;
typedef struct {
    int stages;                        /* TOUR_STAGES_MIN..MAX */
    TourStage stage[TOUR_STAGES_MAX];
    int difficulty;                    /* 0..2 (the lives: the tournament's) */
    unsigned roster;                   /* bit per car: the enemies are drawn from these (never a player's car): the 12,
                                        * Sweet Tooth (12), Minion (13) and Dark Tooth (14) once unlocked */
    int bosses_regular;                /* (no longer used: the roster's bits 13-14; kept for the layout) */
    int versus_lives;                  /* a Versus Tournament: each player's extra lives in a stage, 0..TOUR_VERSUS_LIVES_MAX
                                        * (the host's; its difficulty is then the computer cars' only) */
    int player_lives;                  /* a Custom Tournament: each player's lives at its start, 1..TOUR_LIVES_MAX (0: the
                                        * tournament's, 3 less one a player past 2); carried from stage to stage as ever */
    int cpu_lives;                     /* a Custom Tournament: each computer car's lives, 1..TOUR_LIVES_MAX (0: 1, the
                                        * tournament's) (the user's 2026-11-27, both) */
} CustomTour;
enum { TOUR_VERSUS_LIVES_MAX = 5, TOUR_LIVES_MAX = 9 };
enum { TOUR_MINION = 13, TOUR_DARK_TOOTH = 14 };
/* Dark Tooth for players (and picked as an opponent): his own Drivers row, a bit of drivers above the 14 (set: off; saved
 * as dark_tooth_player, the bits by car as before) */
enum { DRIVER_DT_OFF = 1u << 15 };
int battle_driver_allowed(const GameType *t, int car); /* car (0..14) may be picked in t's match: its Drivers row */
CustomTour *battle_tour(void);
void battle_tour_override(CustomTour *t);          /* battle_tour() is this one (a server's; NULL: the profile's again) */
void battle_tour_versus(int on);                   /* battle_tour() the profile's Versus Tournament setup (hosting one), not its
                                                    * Custom Tournament (the user's 2026-11-20: each its own, Versus from the
                                                    * original 8 levels) */
CustomTour *battle_versus_tour(void);              /* the profile's Versus Tournament setup */
const GameType *battle_match_type(void);           /* its game type (Twisted Metal when gone), the match's own rules over it */
void battle_match_rules_take(const GameType *t, int all); /* the match's rules from t (all 0: only those t has changed) */

/* profile.c: the [battle] and [game_typeN] sections */
void battle_clear(void);
void battle_key(const char *section, const char *key, const char *val);
void battle_write(FILE *f);
