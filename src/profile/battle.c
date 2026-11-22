/* Challenge Match game types and match settings (battle.h). The match
 * settings and the profile's own game types are kept in the profile file:
 * [battle] and [game_type1] .. [game_type24]. */
#include "battle.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

const char *const battle_mode_names[MODE_COUNT] = {"Last Car Standing", "Deathmatch", "Juggernaut", "Gun Game",
                                                   "Hunted", "King of the Hill"};

static MatchSetup match;
static GameType user[BATTLE_USER_TYPES];
static int user_count, cleared;

enum { ALL_DRIVERS = (1u << BATTLE_DRIVERS) - 1, ALL_WEAPONS = (1u << BATTLE_WEAPONS) - 1,
       ALL_ENERGY = (1u << ENERGY_COUNT) - 1, ALL_HAZARDS = (1u << HAZARD_COUNT) - 1,
       ALL_DEST = (1u << DEST_COUNT) - 1 };

void battle_online_rules(GameType *t) { t->energy &= ~BATTLE_ONLINE_NEVER_ENERGY; }

void battle_type_defaults(GameType *t, int mode)
{
    memset(t, 0, sizeof *t);
    t->mode = mode;
    t->respawn = 0; /* (extra seconds after the burning) */
    t->drivers = ALL_DRIVERS;
    t->weapons = ALL_WEAPONS;
    for (int w = 0; w < BATTLE_WEAPONS; w++) t->ammo[w] = w == 4 ? 1 : 2; /* remote bombs: 1 (pickupAmmoTable) */
    t->roulette_count = 2;
    t->energy = ALL_ENERGY;
    t->health = t->turbo = 1;
    t->specials = 1;
    t->special_recharge = SPECIAL_NORMAL;
    t->special_ammo = 1;
    t->hazards = ALL_HAZARDS;
    t->destructibles = ALL_DEST;
    t->spawn_shield = 2;
    t->ai_targets = AI_TARGETS_EQUAL; /* (the user: every game type, the computer cars after everyone) */
    t->jugg_health = 200;
    t->jugg_full_heal = 1;
    t->jugg_weapon = t->jugg_ram = 100;
    t->jugg_car = 7; /* (Roadkill, the user's 2026-10-29: the first choice offered) */
    t->gun_levels = ((1u << GUN_COUNT) - 1) & ~(1u << GUN_LIGHTNING | 1u << GUN_RAM); /* (no lightning, no ram kill) */
    t->gun_specials = BATTLE_SPECIAL_AS; /* every special a level */
    t->gun_order = 1;                    /* strongest first */
    t->gun_ammo = 10; /* the pickups refill it; Infinite ammo for pure action */
    t->hunted_count = 1;
    t->koth_move = 1; /* (every minute) */
    t->koth_hills = 1;
    t->koth_spots = 0; /* (the arena's chosen spots: Random is no longer offered, the user's 2026-10-30) */
    t->hunted_attack = 1;
    t->hunted_teams = 0; /* (one by one) */
    /* the user's defaults by mode (2026-10-30): the modes with respawns 3 seconds after the burning, the Juggernaut's
     * weapons and rams 200% */
    if (mode == MODE_JUGGERNAUT || mode == MODE_ODDBALL || mode == MODE_KING_OF_THE_HILL) t->respawn = 3;
    if (mode == MODE_JUGGERNAUT) t->jugg_weapon = t->jugg_ram = 200;
}
const char *const battle_special_names[16] = {"", "Sweet Tooth", "Hammerhead", "Outlaw 2", "Warthog", "Mr. Grimm",
    "Grasshopper", "Thumper", "Spectre", "Roadkill", "Twister", "Axel", "Mr. Slam", "Shadow", "Minion", "Dark Tooth"};
const int battle_special_order[BATTLE_SPECIAL_AS_COUNT] = {7, 5, 9, 14, 13, 3, 4, 1, 11, 8};
const char *const battle_gun_names[GUN_COUNT] = {"Fire missiles", "Homing missiles", "Ricochet bombs", "Remote bombs", "Lightning",
                                                 "Napalm", "Power missiles", "Ram kill", "Machine gun"};

/* Made so far: Last Car Standing; the drivers allowed, the weapons on, the
 * ammo per pickup, infinite weapons, the energy attacks, health and turbo
 * pickups and how soon pickups come back may differ from the game's. */
int battle_type_playable(const GameType *t)
{
    GameType a = *t, b;
    battle_type_defaults(&b, MODE_LAST_CAR);
    memset(a.name, 0, sizeof a.name);
    memset(a.desc, 0, sizeof a.desc);
    a.drivers = b.drivers;
    a.weapons = b.weapons;
    memcpy(a.ammo, b.ammo, sizeof a.ammo);
    a.infinite = b.infinite;
    a.energy = b.energy;
    a.health = b.health;
    a.turbo = b.turbo;
    a.pickup_respawn = b.pickup_respawn;
    a.specials = b.specials;
    a.special_recharge = b.special_recharge;
    a.special_pickups = b.special_pickups;
    a.special_ammo = b.special_ammo;
    a.ai_targets = b.ai_targets;
    memcpy(a.refill, b.refill, sizeof a.refill);
    a.roulette = b.roulette;
    a.roulette_count = b.roulette_count;
    a.roulette_add = b.roulette_add;
    a.falloff = b.falloff;
    a.teams = b.teams;                 /* teams are the match's now (the Match page) */
    a.spawn_shield = b.spawn_shield;   /* made: the spawn shield, the Juggernaut's */
    a.jugg_health = b.jugg_health;
    a.jugg_full_heal = b.jugg_full_heal;
    a.jugg_weapon = b.jugg_weapon;
    a.jugg_ram = b.jugg_ram;
    a.jugg_infinite = b.jugg_infinite;
    a.jugg_force = b.jugg_force;
    a.jugg_car = b.jugg_car;
    a.gun_levels = b.gun_levels;
    a.gun_specials = b.gun_specials;
    a.gun_order = b.gun_order;
    a.gun_infinite = b.gun_infinite;
    a.gun_ammo = b.gun_ammo;
    a.hunted_count = b.hunted_count;
    a.hunted_attack = b.hunted_attack;
    a.hunted_teams = b.hunted_teams;
    a.koth_hills = b.koth_hills;
    a.koth_spots = b.koth_spots;
    a.hazards = b.hazards;             /* made: decomp request 38 (ARENA_SWITCHES.md; the hybrid's portArenaOptions) */
    a.destructibles = b.destructibles;
    a.jugg_invisible = b.jugg_invisible;
    a.koth_move = b.koth_move;
    a.respawn = b.respawn; /* made: every mode (Last Car Standing's: with lives) */
    if (a.mode != MODE_LAST_CAR) a.mode = b.mode; /* the modes with respawns: made */
    a.friendly_fire = b.friendly_fire;
    return !memcmp(&a, &b, sizeof a);
}

/* ---- built-in game types ---- */
static GameType builtin[16];
static const char *builtin_descs[16];
static int builtin_count;
/* each change to a list of game types (built-in, the profile's, the other players'): battle_type_named's answer
 * found again */
static unsigned types_gen;

static GameType *add_builtin(const char *name, int mode, const char *desc)
{
    GameType *t = &builtin[builtin_count];
    battle_type_defaults(t, mode);
    /* the built-in types (the user's, 2026-10-30): no invisibility, Minion's volley or napalm energy attacks */
    t->energy &= ~(1u << ENERGY_INVISIBILITY | 1u << ENERGY_VOLLEY | 1u << ENERGY_NAPALM);
    snprintf(t->name, sizeof t->name, "%s", name);
    builtin_descs[builtin_count++] = desc;
    types_gen++;
    return t;
}

static void make_builtin(void)
{
    if (builtin_count) return;
    GameType *t;
    add_builtin("Twisted Metal", MODE_LAST_CAR, "The original: the last car alive wins.");
    t = add_builtin("Weapon Roulette", MODE_LAST_CAR, "Everyone's weapon changes every 20 seconds.");
    t->roulette = 20;
    t->roulette_count = 4; /* (the user's: 20 seconds, 4 of each weapon) */
    add_builtin("Deathmatch", MODE_DEATHMATCH, "The most kills wins. Destroyed cars come back.");
    add_builtin("Juggernaut", MODE_JUGGERNAUT, "One car is the Juggernaut: tough and hunted. Kill it to take its place.");
    t = add_builtin("Gun Game", MODE_GUN_GAME, "Each kill gives you the next weapon. The first through them all wins.");
    t->infinite = 1;
    add_builtin("Hunted", MODE_ODDBALL, "Score points by staying alive as the target. Kill the target, and become the target.");
    add_builtin("King of the Hill", MODE_KING_OF_THE_HILL, "Hold the zone the longest. It moves around the arena.");
}

int battle_builtin_count(void) { make_builtin(); return builtin_count; }
const GameType *battle_builtin(int i) { make_builtin(); return i >= 0 && i < builtin_count ? &builtin[i] : NULL; }
const char *battle_builtin_desc(int i) { make_builtin(); return i >= 0 && i < builtin_count ? builtin_descs[i] : ""; }

/* ---- the profile's game types ---- */
int battle_user_count(void) { return user_count; }
GameType *battle_user(int i) { types_gen++; return i >= 0 && i < user_count ? &user[i] : NULL; } /* (to change: a name) */

int battle_user_add(const GameType *t)
{
    if (user_count >= BATTLE_USER_TYPES) return -1;
    user[user_count] = *t;
    types_gen++;
    return user_count++;
}

void battle_user_delete(int i)
{
    if (i < 0 || i >= user_count) return;
    memmove(&user[i], &user[i + 1], (size_t)(user_count - i - 1) * sizeof user[0]);
    user_count--;
    types_gen++;
}

int battle_type_find(const char *name, int *is_user)
{
    make_builtin();
    for (int i = 0; i < builtin_count; i++)
        if (!SDL_strcasecmp(builtin[i].name, name)) { if (is_user) *is_user = 0; return i; }
    for (int i = 0; i < user_count; i++)
        if (!SDL_strcasecmp(user[i].name, name)) { if (is_user) *is_user = 1; return i; }
    return -1;
}

/* the other signed-in players' game types: by owner (players 2-4) and their section number, the list in that order */
static GameType guest[3][BATTLE_USER_TYPES];
static unsigned char guest_used[3][BATTLE_USER_TYPES];
static char guest_who[3][64];
void battle_guests_clear(void) { memset(guest_used, 0, sizeof guest_used); types_gen++; }
static void type_key(GameType *t, const char *key, const char *val);
void battle_guest_key(int owner, const char *who, int n, const char *key, const char *val)
{
    if (owner < 0 || owner > 2 || n < 1 || n > BATTLE_USER_TYPES) return;
    GameType *t = &guest[owner][n - 1];
    types_gen++;
    if (!guest_used[owner][n - 1]) {
        guest_used[owner][n - 1] = 1;
        battle_type_defaults(t, MODE_LAST_CAR);
        snprintf(t->name, sizeof t->name, "Game type %d", n);
        snprintf(guest_who[owner], sizeof guest_who[owner], "%s", who);
    }
    type_key(t, key, val);
}
int battle_guest_count(void)
{
    int c = 0;
    for (int o = 0; o < 3; o++)
        for (int i = 0; i < BATTLE_USER_TYPES; i++) c += guest_used[o][i];
    return c;
}
static int guest_at(int k, int *o, int *i) /* the k-th used guest type */
{
    for (*o = 0; *o < 3; (*o)++)
        for (*i = 0; *i < BATTLE_USER_TYPES; (*i)++)
            if (guest_used[*o][*i] && k-- == 0) return 1;
    return 0;
}
const GameType *battle_guest(int k) { int o, i; return guest_at(k, &o, &i) ? &guest[o][i] : NULL; }
const char *battle_guest_owner(int k) { int o, i; return guest_at(k, &o, &i) ? guest_who[o] : ""; }

static const GameType *type_named(const char *name);
/* The match's game type is looked up by name from many places each step (battle_match_type: the rules of each car,
 * each weapon, each HUD element), case-insensitively through every list: a fifth of a 4-player match's main thread
 * (the user's 2026-11-15 profile). The answer kept for the same name until a list changes. */
const GameType *battle_type_named(const char *name)
{
    static char last[128];
    static const GameType *found;
    static unsigned gen = ~0u;
    if (gen == types_gen && !strcmp(last, name)) return found;
    found = type_named(name);
    if (strlen(name) < sizeof last) snprintf(last, sizeof last, "%s", name), gen = types_gen;
    else gen = ~0u;
    return found;
}
static const GameType *type_named(const char *name)
{
    int u, i = battle_type_find(name, &u);
    if (i >= 0) return u ? &user[i] : &builtin[i];
    for (int k = 0; k < battle_guest_count(); k++) /* (another signed-in player's) */
        if (!SDL_strcasecmp(battle_guest(k)->name, name)) return battle_guest(k);
    return NULL;
}

/* ---- the match ---- */
static void match_defaults(MatchSetup *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->game_type, sizeof m->game_type, "Twisted Metal");
    m->opponents = BATTLE_OPPONENT_SLOTS;
    for (int i = 0; i < BATTLE_OPPONENT_SLOTS; i++) m->opponent[i] = -1;
    m->difficulty = 1;
    m->lives = 1;
    m->kills = 10; /* Deathmatch */
    m->points = 100; /* King of the Hill, Hunted: a point a second */
    for (int i = 0; i < BATTLE_OPPONENT_SLOTS; i++) m->team[i] = 1; /* the player red, the others blue */
    m->no_friendly_fire = 1; /* teams: teammates' weapons (specials' blasts too) do not hurt (the setting: on) */
}

const char *const battle_team_names[BATTLE_TEAM_COLORS] = {"RED", "BLUE", "GREEN", "YELLOW"};

int battle_goal(const MatchSetup *m, int mode)
{
    return mode == MODE_KING_OF_THE_HILL || mode == MODE_ODDBALL ? m->points : m->kills;
}
int battle_match_teams(void)
{
    int mode = battle_match_type()->mode;
    return mode == MODE_LAST_CAR || mode == MODE_DEATHMATCH || mode == MODE_ODDBALL || mode == MODE_KING_OF_THE_HILL ? battle_match()->teams : 0;
}

static MatchSetup *match_override; /* a custom tournament's stage */
MatchSetup *battle_match(void)
{
    if (!cleared) battle_clear();
    return match_override ? match_override : &match;
}
void battle_match_defaults(MatchSetup *m) { match_defaults(m); }
void battle_match_override(MatchSetup *m) { match_override = m; }

static CustomTour tour;
static void tour_defaults(CustomTour *t) /* the tournament's own: its 8 arenas in order, Minion after Amazonia, Dark Tooth last */
{
    memset(t, 0, sizeof *t);
    t->stages = TOUR_STAGES_MAX;
    for (int i = 0; i < TOUR_STAGES_MAX; i++) {
        t->stage[i].arena = i + 1;
        t->stage[i].bosses = i == 3 || i == 7;
        t->stage[i].boss[0] = i == 7 ? TOUR_DARK_TOOTH : TOUR_MINION;
        t->stage[i].boss[1] = TOUR_MINION;
    }
    t->difficulty = 1;
    t->roster = (1u << (BATTLE_OPPONENT_CARS + 1)) - 1; /* the twelve regular drivers and Sweet Tooth (the bosses off) */
}
static CustomTour vtour;          /* the profile's Versus Tournament setup ([versus_tournament]) */
static int tour_versus;           /* battle_tour(): vtour (the menus, hosting a Versus Tournament) */
static CustomTour *tour_override; /* a server's Versus Tournament (the host's setup) */
void battle_tour_override(CustomTour *t) { tour_override = t; }
void battle_tour_versus(int on) { tour_versus = on; }
CustomTour *battle_versus_tour(void)
{
    battle_match();
    return &vtour;
}
CustomTour *battle_tour(void)
{
    battle_match();
    return tour_override ? tour_override : tour_versus ? &vtour : &tour;
}

static const GameType *type_override; /* a server's game type (the host's, which this profile may not have) */
void battle_type_override(const GameType *t) { type_override = t; }
const GameType *battle_type_overridden(void) { return type_override; }
const GameType *battle_match_type(void)
{
    const GameType *t = type_override ? type_override : battle_type_named(battle_match()->game_type);
    if (!t) t = battle_builtin(0);
    const MatchSetup *m = battle_match();
    if (!m->rules_set) return t;
    static GameType merged; /* the match's own rules over its game type */
    merged = *t;
    merged.drivers = m->drivers;
    merged.dark_tooth = m->dark_tooth;
    merged.minion_random = m->minion_random;
    merged.ai_targets = m->ai_targets;
    merged.hazards = m->hazards;
    merged.destructibles = m->destructibles;
    merged.falloff = m->falloff;
    return &merged;
}
void battle_match_rules_take(const GameType *t, int all)
{
    MatchSetup *m = battle_match();
    GameType d;
    battle_type_defaults(&d, t->mode);
    if (!m->rules_set) { /* (first: the game's own, then t's changes over them) */
        m->drivers = d.drivers, m->dark_tooth = d.dark_tooth, m->minion_random = d.minion_random;
        m->ai_targets = d.ai_targets, m->hazards = d.hazards, m->destructibles = d.destructibles, m->falloff = d.falloff;
        m->rules_set = 1;
    }
    if (all || t->drivers != d.drivers || t->dark_tooth != d.dark_tooth || t->minion_random != d.minion_random)
        m->drivers = t->drivers, m->dark_tooth = t->dark_tooth, m->minion_random = t->minion_random;
    if (all || t->ai_targets != d.ai_targets) m->ai_targets = t->ai_targets;
    if (all || t->hazards != d.hazards || t->destructibles != d.destructibles || t->falloff != d.falloff)
        m->hazards = t->hazards, m->destructibles = t->destructibles, m->falloff = t->falloff;
}

/* ---- the profile file ---- */
void battle_clear(void)
{
    cleared = 1;
    match_defaults(&match);
    tour_defaults(&tour);
    tour_defaults(&vtour);
    memset(user, 0, sizeof user);
    user_count = 0;
    types_gen++;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a comma-separated list of numbers into n ints */
static void ints(int *out, int n, const char *val, int lo, int hi, int dflt)
{
    const char *p = val;
    for (int i = 0; i < n; i++) {
        out[i] = *p ? clamp(atoi(p), lo, hi) : dflt;
        p = strchr(p, ',');
        p = p ? p + 1 : "";
    }
}

static void type_key(GameType *t, const char *key, const char *val)
{
    static const struct { const char *key; size_t at; int lo, hi; } fields[] = {
        {"mode", offsetof(GameType, mode), 0, MODE_COUNT - 1},
        {"respawn", offsetof(GameType, respawn), 0, 99},
        {"dark_tooth", offsetof(GameType, dark_tooth), 0, 1},
        {"minion_random", offsetof(GameType, minion_random), 0, 1},
        {"teams", offsetof(GameType, teams), 0, 4},
        {"friendly_fire", offsetof(GameType, friendly_fire), 0, 1},
        {"infinite", offsetof(GameType, infinite), 0, 1},
        {"roulette", offsetof(GameType, roulette), 0, 999},
        {"roulette_count", offsetof(GameType, roulette_count), 1, 9},
        {"roulette_add", offsetof(GameType, roulette_add), 0, 1},
        {"health", offsetof(GameType, health), 0, 1},
        {"turbo", offsetof(GameType, turbo), 0, 1},
        {"pickup_respawn", offsetof(GameType, pickup_respawn), 0, PICKUPS_COUNT - 1},
        {"falloff", offsetof(GameType, falloff), 0, FALL_COUNT - 1},
        {"specials", offsetof(GameType, specials), 0, 1},
        {"special_recharge", offsetof(GameType, special_recharge), 0, SPECIAL_COUNT - 1},
        {"special_pickups", offsetof(GameType, special_pickups), 0, 1},
        {"special_ammo", offsetof(GameType, special_ammo), 1, 9},
        {"ai_targets", offsetof(GameType, ai_targets), 0, AI_TARGETS_COUNT - 1},
        {"spawn_shield", offsetof(GameType, spawn_shield), 0, 5},
        {"jugg_health", offsetof(GameType, jugg_health), 100, 500},
        {"jugg_full_heal", offsetof(GameType, jugg_full_heal), 0, 1},
        {"jugg_weapon", offsetof(GameType, jugg_weapon), 100, 500},
        {"jugg_ram", offsetof(GameType, jugg_ram), 100, 500},
        {"jugg_infinite", offsetof(GameType, jugg_infinite), 0, 1},
        {"jugg_force", offsetof(GameType, jugg_force), 0, 1},
        {"jugg_car", offsetof(GameType, jugg_car), 0, 13},
        {"gun_order", offsetof(GameType, gun_order), 0, 1},
        {"gun_infinite", offsetof(GameType, gun_infinite), 0, 1},
        {"gun_ammo", offsetof(GameType, gun_ammo), 1, 20},
        {"hunted_count", offsetof(GameType, hunted_count), 1, 4},
        {"hunted_attack", offsetof(GameType, hunted_attack), 0, 1},
        {"koth_move", offsetof(GameType, koth_move), 0, 5},
        {"koth_hills", offsetof(GameType, koth_hills), 1, 3},
        {"koth_spots", offsetof(GameType, koth_spots), 0, 1},
        {"hunted_teams", offsetof(GameType, hunted_teams), 0, 1},
        {"jugg_invisible", offsetof(GameType, jugg_invisible), 0, 1},
    };
    static const struct { const char *key; size_t at; unsigned all; } masks[] = {
        {"drivers", offsetof(GameType, drivers), ALL_DRIVERS},
        {"weapons", offsetof(GameType, weapons), ALL_WEAPONS},
        {"energy", offsetof(GameType, energy), ALL_ENERGY},
        {"hazards", offsetof(GameType, hazards), ALL_HAZARDS},
        {"destructibles", offsetof(GameType, destructibles), ALL_DEST},
    };
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++)
        if (!strcmp(key, fields[i].key)) {
            *(int *)((char *)t + fields[i].at) = clamp(atoi(val), fields[i].lo, fields[i].hi);
            return;
        }
    for (unsigned i = 0; i < sizeof masks / sizeof masks[0]; i++)
        if (!strcmp(key, masks[i].key)) {
            *(unsigned *)((char *)t + masks[i].at) = (unsigned)strtoul(val, NULL, 16) & masks[i].all;
            return;
        }
    if (!strcmp(key, "gun_levels")) { t->gun_levels = (unsigned)strtoul(val, NULL, 16) & ((1u << GUN_COUNT) - 1); return; }
    if (!strcmp(key, "gun_specials")) { t->gun_specials = (unsigned)strtoul(val, NULL, 16) & BATTLE_SPECIAL_AS; return; }
    if (!strcmp(key, "name")) snprintf(t->name, sizeof t->name, "%s", val);
    else if (!strcmp(key, "description")) snprintf(t->desc, sizeof t->desc, "%s", val);
    else if (!strcmp(key, "ammo")) ints(t->ammo, BATTLE_WEAPONS, val, 1, 9, 2);
    else if (!strcmp(key, "refill")) ints(t->refill, BATTLE_WEAPONS, val, 0, 999, 0);
}

void battle_key(const char *section, const char *key, const char *val)
{
    battle_match();
    if (!strcmp(section, "custom_tournament") || !strcmp(section, "versus_tournament")) {
        CustomTour *T = section[0] == 'v' ? &vtour : &tour;
        if (!strcmp(key, "stages")) T->stages = atoi(val) > TOUR_STAGES_MIN ? TOUR_STAGES_MAX : TOUR_STAGES_MIN; /* 6 or 8 */
        else if (!strcmp(key, "difficulty")) T->difficulty = clamp(atoi(val), 0, 2);
        else if (!strcmp(key, "roster")) T->roster = (unsigned)strtoul(val, NULL, 16) & 0x7FFFu;
        else if (!strcmp(key, "bosses_regular")) /* (an older file, written after roster: Sweet Tooth was never offered, on;
                                                  * the bosses this switch's, their roster bits unused then) */
            T->roster = (T->roster & ~(3u << 13)) | 1u << 12 | (!strcmp(val, "on") || !strcmp(val, "1") ? 3u << 13 : 0u);
        else if (!strcmp(key, "versus_lives")) {
            int n = atoi(val);
            T->versus_lives = n < 0 ? 0 : n > TOUR_VERSUS_LIVES_MAX ? TOUR_VERSUS_LIVES_MAX : n;
        }
        else if (!strncmp(key, "stage", 5) && atoi(key + 5) >= 1 && atoi(key + 5) <= TOUR_STAGES_MAX) {
            int v[5];
            ints(v, 5, val, 0, 14, 0);
            TourStage *s = &T->stage[atoi(key + 5) - 1];
            s->arena = clamp(v[0], 0, BATTLE_ARENAS + BATTLE_SECRET_ARENAS);
            s->enemies = clamp(v[1], 0, BATTLE_OPPONENT_SLOTS);
            s->bosses = clamp(v[2], 0, TOUR_BOSSES_MAX);
            s->boss[0] = v[3];
            s->boss[1] = v[4];
        }
        return;
    }
    if (!strcmp(section, "battle")) {
        if (!strcmp(key, "game_type")) snprintf(match.game_type, sizeof match.game_type, "%s", val);
        else if (!strcmp(key, "arena")) (void)val; /* each start: a random arena and random cars (as the defaults) */
        else if (!strcmp(key, "opponents")) match.opponents = clamp(atoi(val), 0, BATTLE_OPPONENT_SLOTS);
        else if (!strcmp(key, "opponent_cars")) (void)val;
        else if (!strcmp(key, "difficulty")) match.difficulty = clamp(atoi(val), 0, 2);
        else if (!strcmp(key, "lives")) match.lives = clamp(atoi(val), 1, 10);
        else if (!strcmp(key, "kills")) match.kills = clamp(atoi(val), 0, 99);
        else if (!strcmp(key, "points")) match.points = clamp(atoi(val), 0, 1000);
        else if (!strcmp(key, "time_limit")) match.time_limit = clamp(atoi(val), 0, 99);
        else if (!strcmp(key, "teams")) match.teams = atoi(val) >= 2 ? clamp(atoi(val), 2, BATTLE_TEAM_COLORS) : 0;
        else if (!strcmp(key, "opponent_teams")) ints(match.team, BATTLE_OPPONENT_SLOTS, val, 0, BATTLE_TEAM_COLORS - 1, 1);
        else if (!strcmp(key, "player_teams")) ints(match.player_team, 4, val, 0, BATTLE_TEAM_COLORS - 1, 0);
        else if (!strcmp(key, "friendly_fire")) match.no_friendly_fire = !strcmp(val, "off");
        else if (!strcmp(key, "drivers") || !strcmp(key, "dark_tooth") || !strcmp(key, "minion_random") || !strcmp(key, "ai_targets") ||
                 !strcmp(key, "hazards") || !strcmp(key, "destructibles") || !strcmp(key, "falloff")) {
            if (!match.rules_set) { GameType d; battle_type_defaults(&d, MODE_LAST_CAR); battle_match_rules_take(&d, 1); }
            if (!strcmp(key, "drivers")) match.drivers = (unsigned)strtoul(val, NULL, 16) & ALL_DRIVERS;
            else if (!strcmp(key, "dark_tooth")) match.dark_tooth = atoi(val) != 0;
            else if (!strcmp(key, "minion_random")) match.minion_random = atoi(val) != 0;
            else if (!strcmp(key, "ai_targets")) match.ai_targets = clamp(atoi(val), 0, AI_TARGETS_COUNT - 1);
            else if (!strcmp(key, "hazards")) match.hazards = (unsigned)strtoul(val, NULL, 16) & ALL_HAZARDS;
            else if (!strcmp(key, "destructibles")) match.destructibles = (unsigned)strtoul(val, NULL, 16) & ALL_DEST;
            else match.falloff = clamp(atoi(val), 0, FALL_COUNT - 1);
            if (!match.drivers) match.drivers = ALL_DRIVERS;
        }
    } else if (!strncmp(section, "game_type", 9)) {
        int n = atoi(section + 9);
        if (n < 1 || n > BATTLE_USER_TYPES) return;
        while (user_count < n) { /* sections in order; a gap is a plain Last Car Standing */
            battle_type_defaults(&user[user_count], MODE_LAST_CAR);
            snprintf(user[user_count].name, sizeof user[0].name, "Game type %d", user_count + 1);
            user_count++;
        }
        type_key(&user[n - 1], key, val);
        types_gen++;
    }
}

static void write_ints(FILE *f, const char *key, const int *v, int n)
{
    fprintf(f, "%s = ", key);
    for (int i = 0; i < n; i++) fprintf(f, "%s%d", i ? "," : "", v[i]);
    fprintf(f, "\n");
}

void battle_write(FILE *f)
{
    battle_match();
    fprintf(f, "\n[battle]\n; the Challenge Match settings (arena 0 random, 1..8 the tournament's; opponents 0-15\n"
               "; (an arena with less room: fewer); cars by car-select order, -1 random)\n"
               "game_type = %s\narena = %d\nopponents = %d\n", match.game_type, match.arena, match.opponents);
    write_ints(f, "opponent_cars", match.opponent, BATTLE_OPPONENT_SLOTS);
    fprintf(f, "difficulty = %d\nlives = %d\nkills = %d\npoints = %d\ntime_limit = %d\n", match.difficulty, match.lives, match.kills,
            match.points, match.time_limit);
    fprintf(f, "; teams: 0 off or 2..4; each opponent's and player's team: 0 red, 1 blue, 2 green, 3 gold\n"
               "teams = %d\n", match.teams);
    write_ints(f, "opponent_teams", match.team, BATTLE_OPPONENT_SLOTS);
    write_ints(f, "player_teams", match.player_team, 4);
    fprintf(f, "friendly_fire = %s\n", match.no_friendly_fire ? "off" : "on");
    if (match.rules_set) /* (the match's own rules: Drivers, CPU logic, Arena hazards) */
        fprintf(f, "; the drivers allowed (a bit per car, hex), Dark Tooth / Minion as random opponents, CPU logic (0 classic,\n"
                   "; 1 everyone equal, 2 only humans), hazards and destructibles (hex), falling off (0 death .. 5 none)\n"
                   "drivers = %04x\ndark_tooth = %d\nminion_random = %d\nai_targets = %d\nhazards = %02x\ndestructibles = %02x\n"
                   "falloff = %d\n", match.drivers, match.dark_tooth, match.minion_random, match.ai_targets, match.hazards,
                match.destructibles, match.falloff);
    for (int k = 0; k < 2; k++) { /* the Custom Tournament, then the Versus Tournament (each its own) */
        const CustomTour *T = k ? &vtour : &tour;
        fprintf(f, "\n[%s]\n; stageN = arena (0 random, 1..8, 9..11 the secret ones), enemies (0 random), bosses (0-2),\n"
                   "; boss 1, boss 2 (cars in car-select order: 13 Minion, 14 Dark Tooth); roster: a bit per car (hex, 12 Sweet "
                   "Tooth)\nstages = %d\ndifficulty = %d\nroster = %04X\nversus_lives = %d\n", k ? "versus_tournament" : "custom_tournament",
                T->stages, T->difficulty, T->roster, T->versus_lives);
        for (int i = 0; i < TOUR_STAGES_MAX; i++)
            fprintf(f, "stage%d = %d, %d, %d, %d, %d\n", i + 1, T->stage[i].arena, T->stage[i].enemies, T->stage[i].bosses,
                    T->stage[i].boss[0], T->stage[i].boss[1]);
    }
    for (int i = 0; i < user_count; i++) {
        const GameType *t = &user[i];
        fprintf(f, "\n[game_type%d]\nname = %s\n", i + 1, t->name);
        if (t->desc[0]) fprintf(f, "description = %s\n", t->desc);
        fprintf(f, "mode = %d\nrespawn = %d\ndrivers = %04x\ndark_tooth = %d\nminion_random = %d\nteams = %d\n"
                   "friendly_fire = %d\nweapons = %02x\n", t->mode, t->respawn, t->drivers,
                t->dark_tooth, t->minion_random, t->teams, t->friendly_fire, t->weapons);
        write_ints(f, "ammo", t->ammo, BATTLE_WEAPONS);
        write_ints(f, "refill", t->refill, BATTLE_WEAPONS);
        fprintf(f, "infinite = %d\nroulette = %d\nroulette_count = %d\nenergy = %02x\nhealth = %d\nturbo = %d\n"
                   "pickup_respawn = %d\nhazards = %02x\ndestructibles = %02x\nfalloff = %d\n", t->infinite, t->roulette,
                t->roulette_count, t->energy, t->health, t->turbo, t->pickup_respawn, t->hazards, t->destructibles,
                t->falloff);
        fprintf(f, "roulette_add = %d\n", t->roulette_add);
        fprintf(f, "specials = %d\nspecial_recharge = %d\nspecial_pickups = %d\nspecial_ammo = %d\nai_targets = %d\n",
                t->specials, t->special_recharge, t->special_pickups, t->special_ammo, t->ai_targets);
        fprintf(f, "spawn_shield = %d\njugg_health = %d\njugg_full_heal = %d\njugg_weapon = %d\njugg_ram = %d\njugg_infinite = %d\njugg_force = %d\njugg_car = %d\n",
                t->spawn_shield, t->jugg_health, t->jugg_full_heal, t->jugg_weapon, t->jugg_ram, t->jugg_infinite, t->jugg_force,
                t->jugg_car);
        fprintf(f, "gun_levels = %02x\ngun_specials = %04x\ngun_order = %d\ngun_infinite = %d\ngun_ammo = %d\n", t->gun_levels,
                t->gun_specials, t->gun_order, t->gun_infinite, t->gun_ammo);
        fprintf(f, "hunted_count = %d\nhunted_attack = %d\nkoth_move = %d\n", t->hunted_count, t->hunted_attack, t->koth_move);
        fprintf(f, "koth_hills = %d\nkoth_spots = %d\nhunted_teams = %d\njugg_invisible = %d\n", t->koth_hills, t->koth_spots,
                t->hunted_teams, t->jugg_invisible);
    }
}
