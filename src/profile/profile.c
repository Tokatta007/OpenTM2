/* Player profiles (profile.h).
 *
 * tm2pc.ini remembers the profile signed in last:   [profile] name = Player
 * Each profile is profiles/<name>.ini next to the executable:
 *
 *   [profile]
 *   name = Player
 *   picture = Axel 1         ; a driver's car-select portrait, 1 or 2
 *   [controls]               ; player 1's layout, buttons, keys, song keys (bindings.c)
 *   layout = standard
 *   ...
 *   [hud]                    ; this player's battle HUD (Customize HUD; settings.c hud_*)
 *   [endings]                ; earned by winning the tournament on Medium or Hard
 *   Hammerhead = no
 *   Axel = yes
 *   ...
 *
 * Unlock rules (the user's, docs/PROFILES.md): Sweet Tooth after any one
 * ending; Minion after 7 different drivers' endings (2026-10-14; was all 13
 * others); Dark Tooth after all 14 drivers' endings, Sweet Tooth and Minion
 * included; the secret levels one by one at 2, 3 and 4 endings. Portraits: each
 * driver's first while the driver can be played, the second and third (for
 * now) with the driver's ending. */
#include <ctype.h>
#define _CRT_RAND_S /* (rand_s: the online secret) */
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "feats.h"
#include "battle.h"
#include "profile.h"
#include "controls/bindings.h"
#include "settings.h"

static const char *const characters[PROFILE_CHARACTERS] = {
    "Hammerhead", "Outlaw 2", "Warthog", "Mr. Grimm", "Grasshopper", "Thumper", "Spectre",
    "Roadkill", "Twister", "Axel", "Mr. Slam", "Shadow", "Sweet Tooth", "Minion",
};
#define SWEET_TOOTH 12
#define MINION 13
#define SAVE_DARK_TOOTH 14 /* a tournament save's driver can be Dark Tooth (car 14) too */

static char folder[512] = "profiles";
static char name[64] = "Player";
static int picture;
static unsigned char endings[PROFILE_CHARACTERS];
/* mod characters' endings earned ([mod_endings]: each mod's uid; MODDING.md 6, only inside their mod: never the game's) */
enum { MOD_ENDINGS = 256 };
static char mod_endings[MOD_ENDINGS][33];
static int mod_endings_n;
static int active; /* a profile is loaded (runs with a settings file) */
static int generation; /* another profile signed in */

int profile_generation(void) { return generation; }

void profile_set_folder(const char *f) { snprintf(folder, sizeof folder, "%s", f); }
const char *profile_name(void) { return name; }
const char *profile_character_name(int car) { return car >= 0 && car < PROFILE_CHARACTERS ? characters[car] : car == 14 ? "Dark Tooth" : "?"; } /* (14: Survival's boss) */
int profile_ending_earned(int car) { return car >= 0 && car < PROFILE_CHARACTERS && endings[car]; }
int profile_signed_in(void) { return active && name[0]; }
int profile_mod_ending_earned(const char *uid)
{
    for (int i = 0; i < mod_endings_n; i++)
        if (!strcmp(mod_endings[i], uid)) return 1;
    return 0;
}
static void mod_ending_add(const char *uid)
{
    if (strlen(uid) == 32 && !profile_mod_ending_earned(uid) && mod_endings_n < MOD_ENDINGS)
        snprintf(mod_endings[mod_endings_n++], sizeof mod_endings[0], "%s", uid);
}

int profile_endings_count(void)
{
    int n = 0;
    for (int i = 0; i < PROFILE_CHARACTERS; i++) n += endings[i];
    return n;
}

/* The unlocks count the endings of everyone signed in together (the user's, 2026-10-14): one player who has Sweet
 * Tooth, Minion, Dark Tooth or a secret level opens it for all of them while they play together; alone, a profile has
 * only its own. (Its own endings count, the profile screen's, stays its own: profile_endings_count.) */
static int shared_ending(int i); /* (below, with the other players' profiles) */
static int shared_count(void)
{
    /* TM2PC_UNLOCK_ALL (tests: tm2pc-unlocked.bat): every driver, Dark Tooth and the secret arenas offered, nothing
     * written to the profile (its endings stay its own) */
    if (getenv("TM2PC_UNLOCK_ALL")) return PROFILE_CHARACTERS;
    int n = 0;
    for (int i = 0; i < PROFILE_CHARACTERS; i++) n += shared_ending(i);
    return n;
}

int profile_sweet_tooth_unlocked(void) { return shared_count() >= 1; }

int profile_minion_unlocked(void) { return shared_count() >= 7; }

int profile_dark_tooth_unlocked(void) { return shared_count() >= PROFILE_CHARACTERS; }

int profile_secret_levels_unlocked(void)
{
    int n = shared_count();
    return n >= 4 ? 3 : n >= 3 ? 2 : n >= 2 ? 1 : 0;
}

/* ---- pictures ---- */

/* ---- movie portraits ---- */

#define MOVIE_MAX 256
static struct { char id[32]; int who, owner, extra; } movies[MOVIE_MAX];
static int movie_count;

static const char *const who_keys[PROFILE_CHARACTERS] = {
    "hammerhead", "outlaw2", "warthog", "grimm", "grasshopper", "thumper", "spectre",
    "roadkill", "twister", "axel", "slam", "shadow", "sweettooth", "minion",
};
/* each ending's movie, by car-select index (the decomp's docs/UNLOCKS.md) */
static const char *const ending_movies[PROFILE_CHARACTERS] = {
    "MONSTERT", "COPCAR", "HUMVEE", "MRGRIM", "DUNE", "IMPALLA", "VETTE",
    "ROADKILL", "INDY", "AXEL", "FRONTL", "HEARSE", "SWEET", "MINION",
};

void profile_load_movie_portraits(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[160];
    movie_count = 0;
    if (!f)
        return;
    while (fgets(line, sizeof line, f) && movie_count < MOVIE_MAX) {
        char id[32], who[32], movie[32];
        if (line[0] == '#' || sscanf(line, "%31s %31s %31s", id, who, movie) != 3)
            continue; /* the first line is the picks' hash (movie_portraits.py) */
        snprintf(movies[movie_count].id, sizeof movies[0].id, "%s", id);
        movies[movie_count].who = !strcmp(who, "calypso") ? PROFILE_WHO_CALYPSO : PROFILE_WHO_OTHER;
        for (int c = 0; c < PROFILE_CHARACTERS; c++)
            if (!strcmp(who, who_keys[c])) movies[movie_count].who = c;
        movies[movie_count].owner = -1;
        for (int c = 0; c < PROFILE_CHARACTERS; c++)
            if (!SDL_strcasecmp(movie, ending_movies[c])) movies[movie_count].owner = c;
        movies[movie_count].extra = !SDL_strcasecmp(movie, "PROMO"); /* a driver's extra portrait, not a still */
        if (movies[movie_count].extra) movies[movie_count].owner = movies[movie_count].who;
        movie_count++;
    }
    fclose(f);
}

int profile_movie_count(void) { return movie_count; }
int profile_picture_total(void) { return PROFILE_PICTURES + movie_count; }
int profile_movie_who(int i) { return i >= 0 && i < movie_count ? movies[i].who : PROFILE_WHO_OTHER; }
int profile_movie_owner(int i) { return i >= 0 && i < movie_count ? movies[i].owner : -1; }
int profile_movie_extra(int i) { return i >= 0 && i < movie_count && movies[i].extra; }

int profile_picture(void) { return picture; }
void profile_set_picture(int p) { if (p >= 0 && p < profile_picture_total()) picture = p; }

int profile_character_known(int car)
{
    return car == SWEET_TOOTH ? profile_sweet_tooth_unlocked() : car == MINION ? profile_minion_unlocked() : 1;
}

/* the driver a picture belongs to for unlocking (-1: free) */
static int picture_owner(int p)
{
    return p < PROFILE_PICTURES ? p / PROFILE_PORTRAITS : profile_movie_owner(p - PROFILE_PICTURES);
}

int profile_picture_secret(int p)
{
    int car = picture_owner(p);
    return car >= 0 && !profile_character_known(car);
}

int profile_picture_hidden(int p)
{
    if (profile_picture_secret(p)) return 1;
    return p >= PROFILE_PICTURES && picture_owner(p) >= 0 && !profile_picture_unlocked(p);
}

/* The goals (docs/PORTRAIT_UNLOCKS.md, the user's list): stills earned by
 * a feat rather than with their driver's ending, by movie id. */
enum { GOAL_FLAG, GOAL_COUNT, GOAL_ALL_WEAPONS, GOAL_UNIQUE_WINS, GOAL_STAT /* a stat, local and online together */ };
static const struct { const char *id; int kind, what, count; const char *how; } goals[] = {
    {"ROADKILL_02_marcus", GOAL_FLAG, FEAT_REMOTE_KILL, 0, "Get a remote bomb kill."},
    {"INDY_03", GOAL_COUNT, FEAT_TURBOS, 10, "Pick up 10 turbo powerups."},
    {"INDY_05_twister", GOAL_COUNT, FEAT_TURBOS, 88, "Pick up 88 turbo powerups."},
    {"INDY_09_c1", GOAL_FLAG, FEAT_TOP_SPEED, 0, "Reach the top possible speed."},
    {"FRONTL_14", GOAL_COUNT, FEAT_DEATHS, 1, "Die once in any match."},
    {"FRONTL_26", GOAL_COUNT, FEAT_DEATHS, 10, "Die 10 times."},
    {"FRONTL_28", GOAL_COUNT, FEAT_DEATHS, 50, "Die 50 times."},
    {"FRONTL_29_look", GOAL_COUNT, FEAT_FALLS, 1, "Fall off any map."},
    {"FRONTL_30_fall", GOAL_COUNT, FEAT_FALLS, 5, "Fall off any map 5 times."},
    {"MONSTERT_02_afro", GOAL_COUNT, FEAT_RAM_KILLS, 1, "Get a ram kill."},
    {"MONSTERT_02_cap", GOAL_COUNT, FEAT_RAM_KILLS, 10, "Get 10 ram kills."},
    {"COPCAR_06_last", GOAL_COUNT, FEAT_FIRE_KILLS, 10, "Get 10 kills with a fire missile."},
    {"COPCAR_08", GOAL_STAT, STAT_SELF_KILLS, 1, "Kill yourself with your own mine or weapon."},
    {"HUMVEE_01_warthog", GOAL_COUNT, FEAT_KILLS, 105, "Get 105 kills in all."},
    {"MRGRIM_03", GOAL_COUNT, FEAT_POWER_KILLS, 10, "Get 10 kills with a power missile."},
    {"DUNE_04", GOAL_FLAG, FEAT_MINE_KILL, 0, "Get a kill with a mine."},
    {"DUNE_08", GOAL_COUNT, FEAT_FREEZES, 15, "Freeze 15 enemies."},
    {"DUNE_22z", GOAL_COUNT, FEAT_FROZEN, 15, "Get frozen by enemies 15 times."},
    {"VETTE_04_spectre", GOAL_COUNT, FEAT_HOMING_KILLS, 10, "Get 10 kills with a homing missile."},
    {"INTRO_06a", GOAL_FLAG, FEAT_TOURNAMENT_STARTED, 0, "Start a tournament."},
    {"INTRO_21_last", GOAL_FLAG, FEAT_CUSTOM_FINISHED, 0, "Play a custom battle to its end."},
    {"INTRO_22", GOAL_FLAG, FEAT_LOST_ALL_LIVES, 0, "Lose all your lives in a tournament."},
    {"INTRO_25_eye", GOAL_FLAG, FEAT_TOURNAMENT_WON, 0, "Win a tournament."},
    {"AXEL_03_calypso", GOAL_FLAG, FEAT_LIGHTNING_KILL, 0, "Get a kill with lightning."},
    {"AXEL_23", GOAL_UNIQUE_WINS, 0, 7, "Win the tournament with 7 different drivers."},
    {"INTRO_15_left", GOAL_COUNT, FEAT_PEDESTRIANS, 1, "Kill a pedestrian."},
    {"INTRO_15_right", GOAL_COUNT, FEAT_PEDESTRIANS, 10, "Kill 10 pedestrians."},
    {"HEARSE_34_kid", GOAL_FLAG, FEAT_SANTA, 0, "Kill the Santa pedestrian."},
    {"INDY_14_chef", GOAL_FLAG, FEAT_NAPALM_KILL, 0, "Kill an enemy with napalm."},
    {"INDY_14_family", GOAL_FLAG, FEAT_DIED_ON_FIRE, 0, "Die while on fire."},
    {"INDY_29", GOAL_COUNT, FEAT_RAM_KILLS, 50, "Get 50 ram kills."},
    {"INDY_30_kid_b", GOAL_ALL_WEAPONS, 0, 0, "Get a kill with every weapon."},
    {"INDY_18_pirate_c2", GOAL_FLAG, FEAT_RICOCHET_KILL, 0, "Kill an enemy with a ricochet bomb."},
    {"MRGRIM_15_gunman", GOAL_STAT, STAT_MG_KILLS, 50, "Kill 50 enemies with your machine guns."},
    {"MRGRIM_18_c2", GOAL_COUNT, FEAT_KILLS, 500, "Get 500 kills."},
    {"MRGRIM_19_c1", GOAL_FLAG, FEAT_EIFFEL, 0, "Destroy the Eiffel Tower."},
    {"ROADKILL_09_doctor", GOAL_COUNT, FEAT_HEALTHS, 100, "Pick up 100 health packs."},
    {"SWEET_05_gardener", GOAL_STAT, STAT_SURVIVAL_BEST, 6, "Make it past round 5 in Survival."},
};

static int goal_of(int p) /* the picture's goal, or -1 */
{
    if (p < PROFILE_PICTURES || p >= profile_picture_total()) return -1;
    for (int g = 0; g < (int)(sizeof goals / sizeof goals[0]); g++)
        if (!strcmp(goals[g].id, movies[p - PROFILE_PICTURES].id)) return g;
    return -1;
}

static unsigned stat_total(StatCounter s) /* a stat of player 1's profile, local and online together (a best: the higher) */
{
    const Feats *f = feats_of(0);
    unsigned l = f->stat[STATS_LOCAL][s], o = f->stat[STATS_ONLINE][s];
    return s == STAT_SURVIVAL_BEST ? (l > o ? l : o) : l + o;
}
static unsigned goal_have(int g)
{
    return goals[g].kind == GOAL_STAT ? stat_total((StatCounter)goals[g].what) : feat_count(0, (FeatCounter)goals[g].what);
}

static int goal_met(int g)
{
    switch (goals[g].kind) {
    case GOAL_STAT: return (int)goal_have(g) >= goals[g].count;
    case GOAL_FLAG: return feat_has(0, (FeatFlag)goals[g].what);
    case GOAL_COUNT: return (int)feat_count(0, (FeatCounter)goals[g].what) >= goals[g].count;
    case GOAL_ALL_WEAPONS: return feat_all_weapons(0);
    default: return profile_endings_count() >= goals[g].count;
    }
}

int profile_picture_unlocked(int p)
{
    if (p < 0 || p >= profile_picture_total() || profile_picture_secret(p)) return 0;
    if (p >= PROFILE_PICTURES) { /* a movie's: its goal, or (an ending's) with the ending */
        int g = goal_of(p);
        if (g >= 0) return goal_met(g);
        int car = picture_owner(p);
        return car >= 0 && endings[car];
    }
    int car = p / PROFILE_PORTRAITS, k = p % PROFILE_PORTRAITS;
    if (k == 0) return 1;
    if (k == 1) /* 10 kills with the driver's special (the user's 2026-10-30; or the ending, earned before they were counted) */
        return stat_total((StatCounter)(STAT_SPECIAL_KILLS_CAR + car)) >= 10 || endings[car];
    return endings[car];
}

/* how a locked picture is earned; a goal with a number shows its progress on a line below */
const char *profile_picture_how(int p)
{
    static char how[128];
    int car = picture_owner(p), g = goal_of(p);
    if (profile_picture_secret(p))
        snprintf(how, sizeof how, "???"); /* a secret until unlocked */
    else if (g >= 0 && (goals[g].kind == GOAL_COUNT || goals[g].kind == GOAL_STAT) && goals[g].count > 1 &&
             !(goals[g].kind == GOAL_STAT && goals[g].what == STAT_SURVIVAL_BEST)) { /* (Survival's is done or not: no "4 / 6" for "past round 5") */
        unsigned have = goal_have(g); /* shown capped at the goal (the count keeps going) */
        snprintf(how, sizeof how, "%s\n%u / %d", goals[g].how, have > (unsigned)goals[g].count ? (unsigned)goals[g].count : have,
                 goals[g].count);
    } else if (g >= 0 && goals[g].kind == GOAL_UNIQUE_WINS)
        snprintf(how, sizeof how, "%s\n%d / %d", goals[g].how,
                 profile_endings_count() > goals[g].count ? goals[g].count : profile_endings_count(), goals[g].count);
    else if (g >= 0)
        snprintf(how, sizeof how, "%s", goals[g].how);
    else if (p < PROFILE_PICTURES && p % PROFILE_PORTRAITS == 0)
        snprintf(how, sizeof how, "Unlocked from the start.");
    else if (p < PROFILE_PICTURES && p % PROFILE_PORTRAITS == 1) {
        unsigned have = stat_total((StatCounter)(STAT_SPECIAL_KILLS_CAR + car));
        snprintf(how, sizeof how, "Get 10 kills with %s's special.\n%u / 10", profile_character_name(car), have > 10 ? 10 : have);
    }
    else if (car < 0)
        snprintf(how, sizeof how, "How to unlock it is coming soon.");
    else
        snprintf(how, sizeof how, "Win the tournament as %s on Medium or Hard.", profile_character_name(car));
    return how;
}

static void picture_name(char *out, size_t n, int p)
{
    if (p >= PROFILE_PICTURES && p < profile_picture_total())
        snprintf(out, n, "movie %s", movies[p - PROFILE_PICTURES].id);
    else
        snprintf(out, n, "%s %d", profile_character_name(p / PROFILE_PORTRAITS), p % PROFILE_PORTRAITS + 1);
}

static int picture_from_name(const char *s)
{
    for (int p = 0; p < profile_picture_total(); p++) {
        char pn[48];
        picture_name(pn, sizeof pn, p);
        if (!SDL_strcasecmp(s, pn)) return p;
    }
    return 0;
}

/* ---- the [profile] section of tm2pc.ini ---- */

void profile_ini_key(const char *key, const char *val)
{
    if (!SDL_strcasecmp(key, "name") && profile_name_ok(val)) /* (a hand-edited name past PROFILE_NAME_MAX: not taken) */
        snprintf(name, sizeof name, "%s", val);
}

void profile_ini_write(FILE *f)
{
    fprintf(f, "[profile]\n; the profile signed in last (profiles\\<name>.ini; later also your online name)\n"
               "name = %s\n", name);
}

/* ---- profile files ---- */

int profile_name_ok(const char *s)
{
    int any = 0;
    if (!s || strlen(s) > PROFILE_NAME_MAX) return 0;
    for (const char *p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == ' ' || *p == '-' || *p == '_' || *p == '.')) return 0;
        if (*p != ' ' && *p != '.') any = 1;
    }
    return any && s[0] != ' ' && s[strlen(s) - 1] != ' ';
}

static void path_for(char *out, size_t n, const char *who)
{
    snprintf(out, n, "%s/%s.ini", folder, who);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* ---- backups and the progress check ----
 * Each save first keeps the file before it: profiles/backup/<name>.1.ini (the
 * newest) .. .5.ini. The progress ([endings]) carries a check, a keyed hash
 * (SipHash-2-4) of it with a key in the program: progress edited by hand
 * no longer matches, and is taken from the newest backup that does (or
 * dropped). The controls are not checked: they are the player's to edit.
 * Not tied to the computer: a profile copied to another PC keeps its
 * progress. (A determined player can still find the key in the program;
 * this stops editing the file.) */
#define BACKUPS 5

static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

static uint64_t siphash(const uint8_t *in, size_t len, uint64_t k0, uint64_t k1)
{
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
#define SIPROUND                                                                        \
    do {                                                                                \
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);                       \
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;                                          \
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;                                          \
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);                       \
    } while (0)
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t m = 0;
        for (int k = 7; k >= 0; k--) m = (m << 8) | in[i + k];
        v3 ^= m; SIPROUND; SIPROUND; v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (int k = 0; i + k < len; k++) b |= (uint64_t)in[i + k] << (8 * k);
    v3 ^= b; SIPROUND; SIPROUND; v0 ^= b;
    v2 ^= 0xff;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
#undef SIPROUND
    return v0 ^ v1 ^ v2 ^ v3;
}

static uint64_t progress_check(void)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "tm2pc progress 1:");
    for (int i = 0; i < PROFILE_CHARACTERS; i++) buf[n++] = endings[i] ? '1' : '0';
    return siphash((const uint8_t *)buf, (size_t)n, 0x5457324d45544131ull, 0x53574545544f4f54ull);
}

static char loaded_check[24]; /* the check read with the progress ("" none) */

/* ---- tournament saves ---- */

static ProfileSave saves[PROFILE_SAVES];
static char save_checks[PROFILE_SAVES][24]; /* as read */

static uint64_t save_check(int slot, const ProfileSave *v)
{
    char buf[512];
    if (v->ident) { /* (who played it too: a save from 2026-11-10 on) */
        int n = snprintf(buf, sizeof buf, "tm2pc save 4:%d:%d:%d:%s:%d:%d,%d,%d,%d:%d", slot, v->level, v->difficulty, v->when,
                         v->players, v->cars[0], v->cars[1], v->cars[2], v->cars[3], v->online);
        for (int p = 0; p < v->players && p < PROFILE_SAVE_PLAYERS && n < (int)sizeof buf - 64; p++) {
            n += snprintf(buf + n, sizeof buf - (size_t)n, ":%s:", v->who[p]);
            for (int i = 0; i < 8; i++) n += snprintf(buf + n, sizeof buf - (size_t)n, "%02x", v->ids[p][i]);
        }
        return siphash((const uint8_t *)buf, (size_t)n, 0x5457324d45544131ull, 0x53415645534c4f54ull);
    }
    int n = v->players <= 1 /* as saves from before co-op saves */
                ? snprintf(buf, sizeof buf, "tm2pc save 2:%d:%d:%d:%d:%s", slot, v->level, v->car, v->difficulty, v->when)
                : snprintf(buf, sizeof buf, "tm2pc save 3:%d:%d:%d:%s:%d:%d,%d,%d,%d", slot, v->level, v->difficulty,
                           v->when, v->players, v->cars[0], v->cars[1], v->cars[2], v->cars[3]);
    return siphash((const uint8_t *)buf, (size_t)n, 0x5457324d45544131ull, 0x53415645534c4f54ull);
}

const ProfileSave *profile_tournament_save(int slot)
{
    return slot >= 0 && slot < PROFILE_SAVES ? &saves[slot] : NULL;
}

void profile_set_tournament_save(int slot, int level, int players, const int *cars, int difficulty)
{
    profile_set_tournament_save_who(slot, level, players, cars, difficulty, 0, NULL, NULL);
}
void profile_set_tournament_save_who(int slot, int level, int players, const int *cars, int difficulty, int online,
                                     const char (*who)[32], const unsigned char (*ids)[8])
{
    if (slot < 0 || slot >= PROFILE_SAVES) return;
    ProfileSave *v = &saves[slot];
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->ident = who != NULL;
    v->online = online != 0;
    for (int p = 0; who && p < PROFILE_SAVE_PLAYERS; p++) {
        snprintf(v->who[p], sizeof v->who[p], "%s", who[p]);
        for (char *q = v->who[p]; *q; q++) if (*q == '\n' || *q == '=' || *q == '[' || *q == ';') *q = ' '; /* (one ini line) */
        if (ids) memcpy(v->ids[p], ids[p], 8);
    }
    v->level = level;
    v->players = players < 1 ? 1 : players > PROFILE_SAVE_PLAYERS ? PROFILE_SAVE_PLAYERS : players;
    for (int i = 0; i < v->players; i++) v->cars[i] = cars[i];
    v->car = v->cars[0];
    v->difficulty = difficulty;
    SDL_Time now = 0;
    SDL_DateTime dt;
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
        snprintf(v->when, sizeof v->when, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
    else
        v->when[0] = 0;
    profile_save();
    fprintf(stderr, "profile %s: tournament saved in slot %d (level %d, %d player(s), car %d)\n", name, slot + 1, level,
            v->players, v->car);
}

/* tests (ui previews): a save in memory only, not written */
void profile_preview_save(int slot, int level, int players, const int *cars, int difficulty)
{
    if (slot < 0 || slot >= PROFILE_SAVES) return;
    ProfileSave *v = &saves[slot];
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->level = level;
    v->players = players;
    for (int i = 0; i < players && i < PROFILE_SAVE_PLAYERS; i++) v->cars[i] = cars[i];
    v->car = cars[0];
    v->difficulty = difficulty;
    snprintf(v->when, sizeof v->when, "2026-10-06 12:00");
}

void profile_clear_tournament_save(int slot)
{
    if (slot < 0 || slot >= PROFILE_SAVES) return;
    memset(&saves[slot], 0, sizeof saves[slot]);
    profile_save();
}

static void load_save_key(int slot, const char *key, const char *val)
{
    ProfileSave *v = &saves[slot];
    if (!v->used) v->players = 1;
    if (!strcmp(key, "level")) v->level = atoi(val);
    else if (!strcmp(key, "players")) v->players = atoi(val);
    else if (!strncmp(key, "driver", 6)) { /* driver (player 1), driver2 .. driver4 */
        int p = key[6] ? key[6] - '1' : 0;
        if (p >= 0 && p < PROFILE_SAVE_PLAYERS)
            for (int i = 0; i <= SAVE_DARK_TOOTH; i++)
                if (!SDL_strcasecmp(val, profile_character_name(i))) v->cars[p] = i;
        v->car = v->cars[0];
    } else if (!strcmp(key, "difficulty")) v->difficulty = !SDL_strcasecmp(val, "hard") ? 2 : !SDL_strcasecmp(val, "medium");
    else if (!strcmp(key, "saved")) snprintf(v->when, sizeof v->when, "%s", val);
    else if (!strcmp(key, "check")) snprintf(save_checks[slot], sizeof save_checks[slot], "%s", val);
    else if (!strcmp(key, "online")) v->online = !SDL_strcasecmp(val, "yes");
    else if (!strncmp(key, "player", 6) && key[6] >= '1' && key[6] < '1' + PROFILE_SAVE_PLAYERS && !key[7]) {
        v->ident = 1; /* (a seat's profile, or "guest") */
        snprintf(v->who[key[6] - '1'], sizeof v->who[0], "%s", SDL_strcasecmp(val, "guest") ? val : "");
    } else if (!strncmp(key, "id", 2) && key[2] >= '1' && key[2] < '1' + PROFILE_SAVE_PLAYERS && !key[3] && strlen(val) == 16)
        for (int i = 0; i < 8; i++) {
            char h[3] = {val[2 * i], val[2 * i + 1], 0};
            v->ids[key[2] - '1'][i] = (unsigned char)strtoul(h, NULL, 16);
        }
    v->used = 1;
}

/* After a read: saves that fail their check (or make no sense) are dropped. */
static void verify_saves(void)
{
    for (int i = 0; i < PROFILE_SAVES; i++) {
        if (!saves[i].used) continue;
        char want[24];
        snprintf(want, sizeof want, "%016llx", (unsigned long long)save_check(i, &saves[i]));
        int bad_car = saves[i].players < 1 || saves[i].players > PROFILE_SAVE_PLAYERS;
        for (int p = 0; p < saves[i].players && !bad_car; p++)
            bad_car = saves[i].cars[p] < 0 || saves[i].cars[p] > SAVE_DARK_TOOTH;
        if (SDL_strcasecmp(save_checks[i], want) || saves[i].level < 1 || saves[i].level > 8 || bad_car) {
            fprintf(stderr, "profile %s: tournament save %d does not match its check: dropped\n", name, i + 1);
            memset(&saves[i], 0, sizeof saves[i]);
        }
    }
}

/* the progress just read matches its check (none needed for none) */
static int progress_ok(void)
{
    char want[24];
    snprintf(want, sizeof want, "%016llx", (unsigned long long)progress_check());
    return !SDL_strcasecmp(loaded_check, want) || !profile_endings_count();
}

static void backup_path(char *out, size_t n, const char *who, int k)
{
    snprintf(out, n, "%s/backup/%s.%d.ini", folder, who, k);
}

/* Reads a profile file: each key = value with its section to fn. */
static int read_path(const char *path, void (*fn)(const char *section, const char *key, const char *val, void *ctx),
                     void *ctx)
{
    char line[256], section[32] = "";
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *c = strchr(s, ';');
        if (c) *c = 0;
        s = trim(s);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", s + 1); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        fn(section, trim(s), trim(eq + 1), ctx);
    }
    fclose(f);
    return 1;
}

static int read_file(const char *who, void (*fn)(const char *section, const char *key, const char *val, void *ctx),
                     void *ctx)
{
    char path[640];
    path_for(path, sizeof path, who);
    return read_path(path, fn, ctx);
}

static void load_progress(const char *section, const char *key, const char *val, void *ctx)
{
    (void)ctx;
    if (strcmp(section, "endings"))
        return;
    if (!strcmp(key, "check"))
        snprintf(loaded_check, sizeof loaded_check, "%s", val);
    for (int i = 0; i < PROFILE_CHARACTERS; i++)
        if (!SDL_strcasecmp(key, characters[i]))
            endings[i] = !SDL_strcasecmp(val, "yes") || !strcmp(val, "1");
}

/* After the file is read: progress that fails its check comes from the
 * newest backup that passes, or is dropped. */
static void verify_progress(void)
{
    if (progress_ok())
        return;
    fprintf(stderr, "profile %s: the progress does not match its check (edited?)\n", name);
    for (int k = 1; k <= BACKUPS; k++) {
        char path[640];
        backup_path(path, sizeof path, name, k);
        memset(endings, 0, sizeof endings), mod_endings_n = 0;
        loaded_check[0] = 0;
        if (read_path(path, load_progress, NULL) && progress_ok()) {
            fprintf(stderr, "profile %s: progress restored from %s\n", name, path);
            profile_save();
            return;
        }
    }
    memset(endings, 0, sizeof endings), mod_endings_n = 0;
    fprintf(stderr, "profile %s: no backup passes: progress cleared\n", name);
    profile_save();
}

/* ---- the profile online (net/friends.h): its id (5 random bytes: the friend code) and secret, made once ---- */
static uint8_t online_id[8], online_secret[16];
static int online_have;
static int unhex_n(uint8_t *b, const char *s, int n)
{
    for (int i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0; b[i] = (uint8_t)v; }
    return 1;
}
static void random_bytes(uint8_t *b, int n)
{
#ifdef _WIN32
    for (int i = 0; i < n; i++) { unsigned v = 0; if (rand_s(&v)) v = (unsigned)SDL_rand_bits(); b[i] = (uint8_t)v; }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, (size_t)n, f) != (size_t)n) for (int i = 0; i < n; i++) b[i] = (uint8_t)SDL_rand_bits();
    if (f) fclose(f);
#endif
}
int profile_online_identity(uint8_t id[8], uint8_t secret[16]) /* 0: no profile signed in (a guest) */
{
    if (!profile_signed_in()) return 0;
    if (online_have != 3) { /* (both read: 3) */
        random_bytes(online_id, 5);
        memset(online_id + 5, 0, 3);
        random_bytes(online_secret, 16);
        online_have = 3;
        profile_save();
    }
    memcpy(id, online_id, 8);
    memcpy(secret, online_secret, 16);
    return 1;
}
static void load_key(const char *section, const char *key, const char *val, void *ctx)
{
    if (!strcmp(section, "online")) {
        if (!strcmp(key, "id") && strlen(val) >= 16 && unhex_n(online_id, val, 8)) online_have |= 1;
        else if (!strcmp(key, "secret") && strlen(val) >= 32 && unhex_n(online_secret, val, 16)) online_have |= 2;
        return;
    }
    int *had_controls = ctx;
    if (!strcmp(section, "profile") && !strcmp(key, "picture")) {
        picture = picture_from_name(val);
    } else if (!strcmp(section, "controls")) {
        *had_controls = 1;
        bind_profile_key(key, val);
    } else if (!strcmp(section, "endings")) {
        load_progress(section, key, val, NULL);
    } else if (!strcmp(section, "mod_endings")) {
        if (!SDL_strcasecmp(val, "yes")) mod_ending_add(key);
    } else if (!strncmp(section, "save", 4) && section[4] >= '1' && section[4] < '1' + PROFILE_SAVES && !section[5]) {
        load_save_key(section[4] - '1', key, val);
    } else if (!strcmp(section, "feats")) {
        feats_key(0, key, val);
    } else if (!strcmp(section, "stats")) {
        stats_key(0, key, val);
    } else if (!strncmp(section, "battle", 6) || !strncmp(section, "game_type", 9) || !strcmp(section, "custom_tournament") ||
               !strcmp(section, "versus_tournament")) {
        battle_key(section, key, val);
    } else if (!strcmp(section, "hud")) {
        hud_parse(&hud_players[0], key, val);
    }
}

/* A profile file read whole: not empty, with its [profile] section (a file cut short by a crash is not). */
static int file_whole(const char *path)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) ok = !strncmp(line, "[profile]", 9);
    fclose(f);
    return ok;
}

/* A profile file that is there but broken (empty, cut short): the newest whole backup in its place. */
static void restore_broken(const char *who)
{
    char path[640], from[640];
    path_for(path, sizeof path, who);
    if (!SDL_GetPathInfo(path, NULL) || file_whole(path)) return;
    for (int k = 1; k <= BACKUPS; k++) {
        backup_path(from, sizeof from, who, k);
        if (file_whole(from) && SDL_CopyFile(from, path)) {
            fprintf(stderr, "profile %s: the file was broken: restored from %s\n", who, from);
            return;
        }
    }
    fprintf(stderr, "profile %s: the file is broken and no backup is whole\n", who);
}

void profile_save(void)
{
    char path[640], pn[48];
    if (!profile_signed_in())
        return;
    path_for(path, sizeof path, name);
    SDL_CreateDirectory(folder);
    if (file_whole(path)) { /* keep the file before: backup/<name>.1 .. .5 (a broken one is not kept over them) */
        char from[640], to[640], dir[600];
        snprintf(dir, sizeof dir, "%s/backup", folder);
        SDL_CreateDirectory(dir);
        backup_path(to, sizeof to, name, BACKUPS);
        SDL_RemovePath(to);
        for (int k = BACKUPS - 1; k >= 1; k--) {
            backup_path(from, sizeof from, name, k);
            backup_path(to, sizeof to, name, k + 1);
            SDL_RenamePath(from, to);
        }
        backup_path(to, sizeof to, name, 1);
        SDL_CopyFile(path, to);
    }
    /* written whole to <name>.ini.new, then put in its place: a crash while writing never leaves a broken profile */
    char tmp[660];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "profile %s: cannot write %s\n", name, tmp);
        return;
    }
    picture_name(pn, sizeof pn, picture);
    fprintf(f, "; Twisted Metal 2 PC profile\n\n[profile]\nname = %s\n"
               "; a driver's portrait: the driver's name and 1, 2 (car select) or 3 (info screen art),\n"
               "; or movie <id> (a still from the movies)\npicture = %s\n\n", name, pn);
    bind_profile_write(f);
    fprintf(f, "\n[endings]\n; earned by winning the tournament on Medium or Hard\n");
    for (int i = 0; i < PROFILE_CHARACTERS; i++)
        fprintf(f, "%s = %s\n", characters[i], endings[i] ? "yes" : "no");
    fprintf(f, "; the progress's check: edited progress no longer matches it, and is not used\n"
               "check = %016llx\n", (unsigned long long)progress_check());
    if (mod_endings_n) {
        fprintf(f, "\n[mod_endings]\n; mod characters' endings earned, by each mod's uid\n");
        for (int i = 0; i < mod_endings_n; i++) fprintf(f, "%s = yes\n", mod_endings[i]);
    }
    static const char *const diff_names[3] = {"easy", "medium", "hard"};
    for (int i = 0; i < PROFILE_SAVES; i++) {
        const ProfileSave *v = &saves[i];
        if (!v->used) continue;
        fprintf(f, "\n[save%d]\n; a tournament saved after a level (checked as the progress)\n"
                   "level = %d\ndriver = %s\n", i + 1, v->level, profile_character_name(v->car));
        if (v->players > 1) {
            fprintf(f, "players = %d\n", v->players);
            for (int p = 1; p < v->players; p++) fprintf(f, "driver%d = %s\n", p + 1, profile_character_name(v->cars[p]));
        }
        if (v->ident) { /* who played it: each seat's profile ("guest" none), online their ids too */
            if (v->online) fprintf(f, "online = yes\n");
            for (int p = 0; p < v->players && p < PROFILE_SAVE_PLAYERS; p++) {
                fprintf(f, "player%d = %s\n", p + 1, v->who[p][0] ? v->who[p] : "guest");
                if (v->online) {
                    fprintf(f, "id%d = ", p + 1);
                    for (int i = 0; i < 8; i++) fprintf(f, "%02x", v->ids[p][i]);
                    fputc('\n', f);
                }
            }
        }
        fprintf(f, "difficulty = %s\nsaved = %s\ncheck = %016llx\n", diff_names[v->difficulty % 3], v->when,
                (unsigned long long)save_check(i, v));
    }
    feats_write(f, 0);
    battle_write(f);
    fputc('\n', f);
    hud_write(f, &hud_players[0]);
    if (online_have == 3) {
        fprintf(f, "\n[online]\n; this profile online (its friend code, its key): keep it to keep your friends\nid = ");
        for (int i = 0; i < 8; i++) fprintf(f, "%02x", online_id[i]);
        fprintf(f, "\nsecret = ");
        for (int i = 0; i < 16; i++) fprintf(f, "%02x", online_secret[i]);
        fprintf(f, "\n");
    }
    int bad = ferror(f);
    if (fclose(f) || bad || !SDL_RenamePath(tmp, path)) {
        fprintf(stderr, "profile %s: cannot write %s (the file before kept)\n", name, path);
        SDL_RemovePath(tmp);
    }
}

void profile_load(void)
{
    int had_controls = 0;
    memset(endings, 0, sizeof endings), mod_endings_n = 0;
    memset(saves, 0, sizeof saves);
    memset(save_checks, 0, sizeof save_checks);
    loaded_check[0] = 0;
    online_have = 0; /* (a half-read identity is not one: made again) */
    feats_clear(0);
    battle_clear();
    hud_reset_player(0);
    picture = 0;
    active = 1;
    generation++;
    if (!name[0])
        return;
    char path[640];
    path_for(path, sizeof path, name);
    restore_broken(name);
    int exists = SDL_GetPathInfo(path, NULL);
    if (!exists && profile_name_ok(name)) {
        /* named in tm2pc.ini but not made yet (a first start, or an older
         * build's profile without progress): made now, with the current controls */
        profile_save();
        settings_save_section("controls");
        fprintf(stderr, "profile %s: made, with the controls from tm2pc.ini\n", name);
    } else if (exists) {
        /* the profile's controls, from the defaults (none kept from another profile)... */
        int keypad = settings_game.keypad[0];
        if (read_file(name, load_key, &had_controls) && !had_controls) {
            /* ...unless it has none yet (made by an older build): it takes
             * the current ones, from tm2pc.ini, and keeps them from now on */
            settings_game.keypad[0] = keypad;
            profile_save();
            settings_save_section("controls");
            fprintf(stderr, "profile %s: controls moved from tm2pc.ini\n", name);
        } else if (had_controls) {
            /* read again over the defaults, so keys the file leaves out are the defaults */
            bind_player1_defaults();
            memset(endings, 0, sizeof endings), mod_endings_n = 0;
            memset(saves, 0, sizeof saves);
            memset(save_checks, 0, sizeof save_checks);
            loaded_check[0] = 0;
            feats_clear(0);
            battle_clear();
            hud_reset_player(0);
            had_controls = 0;
            read_file(name, load_key, &had_controls);
        }
        verify_saves();
        verify_progress();
        if (!feats_check_ok(0)) fprintf(stderr, "profile %s: the feats do not match their check: cleared\n", name);
    }
    if (!profile_picture_unlocked(picture)) { /* a locked portrait is not shown: Roadkill's first, as car select starts */
        picture = 7 * PROFILE_PORTRAITS;
        if (exists) profile_save();
    }
    fprintf(stderr, "profile %s: %d of %d endings%s%s\n", name, profile_endings_count(), PROFILE_CHARACTERS,
            profile_sweet_tooth_unlocked() ? ", Sweet Tooth" : "", profile_minion_unlocked() ? ", Minion" : "");
}

/* ---- players 3 and 4: their controls (the sign-in) ---- */

static char extra_name[2][64]; /* players 3-4's profiles ("" a guest) */
static void extra_key(const char *section, const char *key, const char *val, void *ctx)
{
    int p = *(int *)ctx;
    if (!strcmp(section, "controls")) bind_profile_key_p(p, key, val);
    else if (!strcmp(section, "feats")) feats_key(p, key, val);
    else if (!strcmp(section, "stats")) stats_key(p, key, val);
    else if (!strcmp(section, "hud")) hud_parse(&hud_players[p], key, val);
}
void profile_extra_use(int p, const char *who) /* p 2-3; "" or no profile: the defaults (a guest: no feats) */
{
    if (p < 2 || p > 3) return;
    bind_player_defaults(p);
    feats_clear(p);
    hud_reset_player(p);
    extra_name[p - 2][0] = 0;
    if (who && who[0] && profile_name_ok(who) && read_file(who, extra_key, &p)) {
        snprintf(extra_name[p - 2], sizeof extra_name[0], "%s", who);
        if (!feats_check_ok(p)) fprintf(stderr, "profile %s: the feats do not match their check: cleared\n", who);
        fprintf(stderr, "profile: player %d is %s\n", p + 1, who);
    }
}
const char *profile_extra_name(int p) { return p >= 2 && p <= 3 ? extra_name[p - 2] : ""; }

/* ---- player 2 ---- */

static char p2_name[64];
static int p2_picture;

static void p2_key(const char *section, const char *key, const char *val, void *ctx)
{
    (void)ctx;
    if (!strcmp(section, "profile") && !strcmp(key, "picture"))
        p2_picture = picture_from_name(val);
    else if (!strcmp(section, "controls"))
        bind_profile_key_p2(key, val);
    else if (!strcmp(section, "feats"))
        feats_key(1, key, val);
    else if (!strcmp(section, "stats"))
        stats_key(1, key, val);
    else if (!strcmp(section, "hud"))
        hud_parse(&hud_players[1], key, val);
}

int profile_p2_use(const char *who)
{
    if (!profile_name_ok(who))
        return 0;
    bind_player2_defaults();
    p2_picture = 0;
    feats_clear(1);
    hud_reset_player(1);
    if (!read_file(who, p2_key, NULL))
        return 0;
    snprintf(p2_name, sizeof p2_name, "%s", who);
    if (!feats_check_ok(1)) fprintf(stderr, "profile %s: the feats do not match their check: cleared\n", who);
    fprintf(stderr, "profile: player 2 is %s\n", p2_name);
    return 1;
}

void profile_p2_clear(void)
{
    p2_name[0] = 0;
    feats_clear(1);
    hud_reset_player(1);
}

/* Rewrites a profile file with some of its sections replaced: the whole file read (any size; blank lines kept), the
 * sections named in `drop` left out, `emit` writing their new text (in_place: where the first of them was, else at the
 * end), into "<file>.tmp" which then replaces the file, so a failed write never leaves a cut-off profile. */
static int rewrite_sections(const char *who, const char *const *drop, int in_place, void (*emit)(FILE *, void *), void *ctx)
{
    char path[640], tmp[660];
    if (!who || !who[0]) return 0;
    path_for(path, sizeof path, who);
    size_t len = 0;
    char *text = SDL_LoadFile(path, &len);
    if (!text) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { SDL_free(text); return 0; }
    int skip = 0, wrote = 0, blanks = 0; /* (blank lines held back: written before the next kept line, never at the end) */
    for (char *line = text, *end; line < text + len; line = end + 1) {
        end = memchr(line, '\n', (size_t)(text + len - line));
        if (!end) end = text + len;
        size_t n = (size_t)(end - line);
        if (n && line[n - 1] == '\r') n--;
        if (line[0] == '[') {
            skip = 0;
            for (int i = 0; drop[i]; i++)
                if (n >= strlen(drop[i]) && !strncmp(line, drop[i], strlen(drop[i]))) skip = 1;
            if (skip && in_place && !wrote) {
                if (blanks) fputc('\n', f);
                blanks = 0;
                emit(f, ctx);
                fputc('\n', f);
                wrote = 1;
            }
        }
        if (skip) continue;
        if (!n) { blanks++; continue; }
        for (; blanks; blanks--) fputc('\n', f);
        fprintf(f, "%.*s\n", (int)n, line);
    }
    if (!wrote) { fputc('\n', f); emit(f, ctx); }
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    SDL_free(text);
    if (!ok || !SDL_RenamePath(tmp, path)) {
        SDL_RemovePath(tmp);
        fprintf(stderr, "profile %s: could not be saved\n", who);
        return 0;
    }
    return 1;
}
static void emit_p2_controls(FILE *f, void *ctx) { (void)ctx; bind_profile_write_p2(f); }

/* Writes player 2's controls into their profile: its [controls] section
 * replaced (the rest of the file, its progress and checks, kept as it is). */
void profile_p2_save(void)
{
    static const char *const drop[] = {"[controls]", NULL};
    if (rewrite_sections(p2_name, drop, 1, emit_p2_controls, NULL))
        fprintf(stderr, "profile %s: player 2's controls saved\n", p2_name);
}

/* Writes player 2's feats into their profile: its [feats] section replaced
 * (or added), the rest kept as it is. */
static void save_feats_of(const char *name, int p);
void profile_p2_save_feats(void) { save_feats_of(p2_name, 1); }
void profile_extra_save_feats(int p) { if (p >= 2 && p <= 3) save_feats_of(extra_name[p - 2], p); }
/* A player's feats into their profile: its [feats] and [stats] sections replaced (the rest kept as it is). */
static void emit_feats(FILE *f, void *ctx) { feats_write(f, *(int *)ctx); }
static void save_feats_of(const char *name, int p)
{
    static const char *const drop[] = {"[feats]", "[stats]", NULL};
    rewrite_sections(name, drop, 0, emit_feats, &p);
}

/* The Stats screen: a profile's feats and stats read into FEATS_VIEW and checked (what is counted so far is
 * written first, so the players' own are up to date) */
static void view_key(const char *section, const char *key, const char *val, void *ctx)
{
    (void)ctx;
    if (!strcmp(section, "feats")) feats_key(FEATS_VIEW, key, val);
    else if (!strcmp(section, "stats")) stats_key(FEATS_VIEW, key, val);
}
int profile_read_stats(const char *who, Feats *out)
{
    feats_flush();
    feats_clear(FEATS_VIEW);
    int ok = who && who[0] && profile_name_ok(who) && read_file(who, view_key, NULL);
    if (ok) feats_check_ok(FEATS_VIEW);
    *out = *feats_of(FEATS_VIEW);
    return ok;
}

static void emit_hud(FILE *f, void *ctx) { hud_write(f, ctx); }
/* A player's HUD into their profile: its [hud] section replaced (or added), the rest kept as it is
 * (player 1: the whole profile written). 0: no profile. */
int profile_hud_save(int p)
{
    if (p == 0) { if (!profile_signed_in()) return 0; profile_save(); return 1; }
    const char *who = p == 1 ? p2_name : p >= 2 && p <= 3 ? extra_name[p - 2] : "";
    static const char *const drop[] = {"[hud]", NULL};
    return rewrite_sections(who, drop, 0, emit_hud, &hud_players[p]);
}
const char *profile_hud_owner(int p) /* the profile a player's HUD is kept in ("" none: the defaults) */
{
    return p == 0 ? (profile_signed_in() ? name : "") : p == 1 ? p2_name : p >= 2 && p <= 3 ? extra_name[p - 2] : "";
}

const char *profile_p2_name(void) { return p2_name; }

/* The other players' profiles (players 2-4) read for their endings: again whenever who is signed in changes or a
 * tournament is won (wins_gen); profile_guests_generation tells profile_hooks.c to set the game's flags again */
static unsigned char guest_endings[PROFILE_CHARACTERS];
static char guest_key[320];
static int guest_gen, wins_gen;
static void guests_refresh(void)
{
    char key[320];
    int n = snprintf(key, sizeof key, "%d|%s|", wins_gen, active ? name : "");
    for (int p = 1; p < 4 && n > 0 && n < (int)sizeof key; p++) n += snprintf(key + n, sizeof key - (size_t)n, "%s|", profile_hud_owner(p));
    if (!strcmp(key, guest_key)) return;
    snprintf(guest_key, sizeof guest_key, "%s", key);
    guest_gen++;
    memset(guest_endings, 0, sizeof guest_endings);
    battle_guests_clear(); /* (their game types too: everyone's in the list while signed in together) */
    for (int p = 1; p < 4; p++) {
        const char *who = profile_hud_owner(p);
        if (!who[0] || (active && !SDL_strcasecmp(who, name))) continue;
        char path[700], line[256];
        path_for(path, sizeof path, who);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int in = 0, gt = 0; /* in: [endings]; gt: [game_type<gt>] */
        while (fgets(line, sizeof line, f)) {
            char *s = trim(line), *eq = strchr(s, '=');
            if (*s == '[') {
                in = !SDL_strncasecmp(s, "[endings]", 9);
                gt = !SDL_strncasecmp(s, "[game_type", 10) ? atoi(s + 10) : 0;
                continue;
            }
            if (!eq) continue;
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            if (gt) { battle_guest_key(p - 1, who, gt, k, v); continue; }
            if (!in) continue;
            for (int i = 0; i < PROFILE_CHARACTERS; i++)
                if (!SDL_strcasecmp(k, characters[i]) && (!SDL_strcasecmp(v, "yes") || !strcmp(v, "1"))) guest_endings[i] = 1;
        }
        fclose(f);
    }
}
static int shared_ending(int i)
{
    guests_refresh();
    return i >= 0 && i < PROFILE_CHARACTERS && (endings[i] || guest_endings[i]);
}
int profile_guests_generation(void) { guests_refresh(); return guest_gen; }
int profile_p2_picture(void) { return p2_picture; }

int profile_make(const char *who, int pic)
{
    char path[640], pn[48];
    if (!profile_name_ok(who) || profile_exists(who))
        return 0;
    path_for(path, sizeof path, who);
    SDL_CreateDirectory(folder);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    picture_name(pn, sizeof pn, pic >= 0 && pic < PROFILE_PICTURES ? pic : 0);
    /* the default controls: what a profile without them reads as */
    fprintf(f, "; Twisted Metal 2 PC profile\n\n[profile]\nname = %s\npicture = %s\n\n"
               "[controls]\nlayout = standard\n", who, pn);
    fclose(f);
    profile_rescan();
    fprintf(stderr, "profile: made %s\n", who);
    return 1;
}

/* ---- the list ---- */

static struct { char name[64]; int picture; } list[PROFILE_MAX];
static int list_count;

static void peek_picture(const char *section, const char *key, const char *val, void *ctx)
{
    if (!strcmp(section, "profile") && !strcmp(key, "picture"))
        *(int *)ctx = picture_from_name(val);
}

static SDL_EnumerationResult SDLCALL add_listed(void *ud, const char *dir, const char *file)
{
    (void)ud;
    (void)dir;
    size_t n = strlen(file);
    if (n > 4 && !SDL_strcasecmp(file + n - 4, ".ini") && list_count < PROFILE_MAX && n - 4 < sizeof list[0].name) {
        memcpy(list[list_count].name, file, n - 4);
        list[list_count].name[n - 4] = 0;
        if (profile_name_ok(list[list_count].name)) {
            list[list_count].picture = 0;
            read_file(list[list_count].name, peek_picture, &list[list_count].picture);
            list_count++;
        }
    }
    return SDL_ENUM_CONTINUE;
}

static int by_name(const void *a, const void *b) { return SDL_strcasecmp((const char *)a, (const char *)b); }

void profile_rescan(void)
{
    list_count = 0;
    SDL_EnumerateDirectory(folder, add_listed, NULL);
    qsort(list, (size_t)list_count, sizeof list[0], by_name);
}

int profile_count(void) { return list_count; }
const char *profile_list_name(int i) { return i >= 0 && i < list_count ? list[i].name : ""; }
int profile_list_picture(int i) { return i >= 0 && i < list_count ? list[i].picture : 0; }

int profile_exists(const char *who)
{
    for (int i = 0; i < list_count; i++)
        if (!SDL_strcasecmp(list[i].name, who)) return 1;
    return 0;
}

int profile_use(const char *who)
{
    if (!profile_name_ok(who))
        return 0;
    if (strcmp(who, name) || !active) {
        snprintf(name, sizeof name, "%s", who);
        bind_player1_defaults();
        profile_load();
    }
    settings_save_section("profile");
    fprintf(stderr, "profile: signed in as %s\n", name);
    return 1;
}

int profile_create(const char *who, int pic)
{
    if (!profile_name_ok(who) || profile_exists(who))
        return 0;
    snprintf(name, sizeof name, "%s", who);
    active = 1;
    generation++;
    /* fresh, as profile_load's start: nothing of the profile signed in before (its feats and stats were saved into
     * the new one, unlocking its portraits, and its online id with them: the user's 2026-11-18) */
    memset(endings, 0, sizeof endings), mod_endings_n = 0;
    memset(saves, 0, sizeof saves);
    memset(save_checks, 0, sizeof save_checks);
    loaded_check[0] = 0;
    online_have = 0; /* (its own made at its first time online) */
    feats_clear(0);
    battle_clear();
    hud_reset_player(0);
    picture = pic >= 0 && pic < PROFILE_PICTURES ? pic : 0;
    bind_player1_defaults();
    profile_save();
    settings_save_section("profile");
    profile_rescan();
    fprintf(stderr, "profile: created %s\n", name);
    return 1;
}

int profile_rename(const char *who)
{
    char from[640], to[640];
    if (!profile_signed_in() || !profile_name_ok(who))
        return 0;
    if (SDL_strcasecmp(who, name) && profile_exists(who))
        return 0;
    path_for(from, sizeof from, name);
    path_for(to, sizeof to, who);
    if (strcmp(from, to) && !SDL_RenamePath(from, to)) {
        fprintf(stderr, "profile: cannot rename %s to %s\n", from, to);
        return 0;
    }
    for (int k = 1; k <= BACKUPS; k++) {
        backup_path(from, sizeof from, name, k);
        backup_path(to, sizeof to, who, k);
        SDL_RenamePath(from, to);
    }
    fprintf(stderr, "profile: %s renamed %s\n", name, who);
    snprintf(name, sizeof name, "%s", who);
    profile_save();
    settings_save_section("profile");
    profile_rescan();
    return 1;
}

/* Renames a profile that may not be the signed-in one (it is signed in for
 * the rename, then the one before again). */
int profile_rename_other(const char *from, const char *to)
{
    char was[64];
    snprintf(was, sizeof was, "%s", profile_signed_in() ? name : "");
    if (!SDL_strcasecmp(from, was)) return profile_rename(to);
    if (!profile_use(from)) return 0;
    int ok = profile_rename(to);
    if (was[0]) profile_use(was);
    return ok;
}

/* Sets a profile's picture, the same way. */
int profile_set_picture_of(const char *who, int pic)
{
    char was[64];
    snprintf(was, sizeof was, "%s", profile_signed_in() ? name : "");
    if (SDL_strcasecmp(who, was) && !profile_use(who)) return 0;
    profile_set_picture(pic);
    profile_save();
    if (was[0] && SDL_strcasecmp(who, was)) profile_use(was);
    profile_rescan();
    return 1;
}

int profile_delete(const char *who)
{
    char path[640];
    path_for(path, sizeof path, who);
    if (!SDL_RemovePath(path))
        return 0;
    for (int k = 1; k <= BACKUPS; k++) { /* for good: the backups too */
        backup_path(path, sizeof path, who, k);
        SDL_RemovePath(path);
    }
    fprintf(stderr, "profile: deleted %s\n", who);
    if (!SDL_strcasecmp(who, name)) {
        name[0] = 0;
        generation++;
        memset(endings, 0, sizeof endings), mod_endings_n = 0;
        settings_save_section("profile");
    }
    profile_rescan();
    return 1;
}

void profile_tournament_won(int car, int difficulty)
{
    wins_gen++; /* (the shared unlocks read again) */
    if (!profile_signed_in()) return;
    stat_add(0, STAT_TOURNAMENT_WINS, 1); /* (any difficulty, any driver) */
    if (car < 0 || car >= PROFILE_CHARACTERS) { /* Dark Tooth (14) has no ending */
        feats_flush();
        return;
    }
    if (difficulty < 1) {
        fprintf(stderr, "profile %s: %s won on Easy; endings count on Medium or Hard\n", name, characters[car]);
        feats_flush();
        return;
    }
    feat_set(0, FEAT_TOURNAMENT_WON);
    if (endings[car]) {
        feats_flush();
        return;
    }
    int sweet = profile_sweet_tooth_unlocked(), minion = profile_minion_unlocked();
    int levels = profile_secret_levels_unlocked();
    endings[car] = 1;
    profile_save();
    fprintf(stderr, "profile %s: earned %s's ending (%d of %d)\n", name, characters[car],
            profile_endings_count(), PROFILE_CHARACTERS);
    if (!sweet && profile_sweet_tooth_unlocked()) fprintf(stderr, "profile %s: unlocked Sweet Tooth\n", name);
    if (!minion && profile_minion_unlocked()) fprintf(stderr, "profile %s: unlocked Minion\n", name);
    if (profile_secret_levels_unlocked() > levels)
        fprintf(stderr, "profile %s: unlocked secret level %d\n", name, profile_secret_levels_unlocked());
}

/* A tournament won with a mod character (its mod's uid): its own ending, as a driver's (Medium or Hard), and never its
 * base car's */
void profile_mod_tournament_won(const char *uid, const char *who, int difficulty)
{
    wins_gen++;
    if (!profile_signed_in()) return;
    stat_add(0, STAT_TOURNAMENT_WINS, 1);
    if (difficulty < 1) {
        fprintf(stderr, "profile %s: %s won on Easy; endings count on Medium or Hard\n", name, who);
        feats_flush();
        return;
    }
    feat_set(0, FEAT_TOURNAMENT_WON);
    if (profile_mod_ending_earned(uid)) {
        feats_flush();
        return;
    }
    mod_ending_add(uid);
    profile_save();
    fprintf(stderr, "profile %s: earned %s's ending (a mod's)\n", name, who);
}
static void as_profile(const char *other, const char *uid, const char *who, int difficulty)
{
    char me[sizeof name];
    snprintf(me, sizeof me, "%s", name);
    feats_flush();
    if (!profile_use(other)) return;
    profile_mod_tournament_won(uid, who, difficulty);
    profile_use(me);
}

/* The co-op duel's winner (ui/options.c): the ending goes to both players'
 * profiles. Player 2's is recorded as player 1's is, signed in as player 2
 * for the moment (its file rewritten with its check), then back. */
void profile_extra_tournament_won(int p, int car, int difficulty) /* player 3 or 4's profile, as player 2's below */
{
    wins_gen++; /* (the shared unlocks read again) */
    if (p < 2 || p > 3 || !extra_name[p - 2][0] || !profile_signed_in() || !SDL_strcasecmp(extra_name[p - 2], name)) return;
    char me[sizeof name];
    snprintf(me, sizeof me, "%s", name);
    feats_flush();
    if (!profile_use(extra_name[p - 2])) return;
    profile_tournament_won(car, difficulty);
    profile_use(me);
}
void profile_p2_mod_tournament_won(const char *uid, const char *who, int difficulty)
{
    wins_gen++;
    if (p2_name[0] && profile_signed_in() && SDL_strcasecmp(p2_name, name)) as_profile(p2_name, uid, who, difficulty);
}
void profile_extra_mod_tournament_won(int p, const char *uid, const char *who, int difficulty)
{
    wins_gen++;
    if (p >= 2 && p <= 3 && extra_name[p - 2][0] && profile_signed_in() && SDL_strcasecmp(extra_name[p - 2], name))
        as_profile(extra_name[p - 2], uid, who, difficulty);
}
void profile_p2_tournament_won(int car, int difficulty)
{
    wins_gen++; /* (the shared unlocks read again) */
    if (!p2_name[0] || !profile_signed_in() || !SDL_strcasecmp(p2_name, name)) return;
    char me[sizeof name];
    snprintf(me, sizeof me, "%s", name);
    feats_flush();
    if (!profile_use(p2_name)) return;
    profile_tournament_won(car, difficulty);
    profile_use(me);
}
