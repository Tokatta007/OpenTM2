/* Feats (feats.h): the counters and flags of each player's profile. */
#include <string.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "feats.h"
#include "profile.h"

static Feats feats[5];       /* players 1-4 (3-4: their profiles from the sign-in); 5: FEATS_VIEW */
static char checks[5][24];  /* as read */
static char stat_checks[5][24];
static int dirty[5];
static int online;          /* the play's stats set */
static int slot(int p) { return p >= 0 && p <= FEATS_VIEW ? p : 0; }

static const char *const stat_keys[STAT_COUNT] = {
    "kills", "deaths", "falls", "machine_gun_kills", "fire_missile_kills", "homing_missile_kills",
    "power_missile_kills", "napalm_kills", "remote_bomb_kills", "mine_kills", "ricochet_kills", "lightning_kills",
    "special_kills", "ram_kills", "freezes", "frozen", "pedestrians", "turbo_pickups", "health_pickups", "matches",
    "match_wins", "survival_best", "tournaments", "tournament_wins", "seconds", "survival_time", "self_kills",
    "special_kills_hammerhead", "special_kills_outlaw2", "special_kills_warthog", "special_kills_grimm",
    "special_kills_grasshopper", "special_kills_thumper", "special_kills_spectre", "special_kills_roadkill",
    "special_kills_twister", "special_kills_axel", "special_kills_slam", "special_kills_shadow",
    "special_kills_sweettooth", "special_kills_minion", "versus_gold", "versus_silver", "versus_bronze",
    "versus_played", "bosses_beaten", "boss_wins", "joins", "damage",
};
/* the feats' counters as stats (the same events: feat_add counts both) */
static const StatCounter feat_stat[FEAT_COUNTERS] = {
    STAT_KILLS, STAT_RAM_KILLS, STAT_DEATHS, STAT_FALLS, STAT_FREEZES, STAT_FROZEN, STAT_PEDESTRIANS, STAT_TURBOS,
    STAT_HEALTHS, STAT_FIRE_KILLS, STAT_HOMING_KILLS, STAT_POWER_KILLS,
};

static const char *const counter_keys[FEAT_COUNTERS] = {
    "kills", "ram_kills", "deaths", "falls", "freezes", "frozen", "pedestrians", "turbo_pickups",
    "health_pickups", "fire_missile_kills", "homing_missile_kills", "power_missile_kills",
};
static const char *const flag_keys[FEAT_FLAGS - FEAT_REMOTE_KILL] = {
    "remote_bomb_kill", "mine_kill", "lightning_kill", "napalm_kill", "ricochet_kill", "machine_gun_kill",
    "rear_fire_kill", "fire_missile_kill", "homing_missile_kill", "power_missile_kill", "top_speed",
    "top_speed_hammerhead", "turbo_emptied", "self_damage", "died_on_fire", "santa", "eiffel_tower",
    "tournament_started", "custom_battle_finished", "lost_all_lives", "tournament_won",
};

const char *profile_extra_name(int p); /* profile.c: players 3-4's profile ("" a guest) */
static int valid(int p) { return p == 0 ? profile_signed_in() : p == 1 ? profile_p2_name()[0] != 0 : p >= 2 && p <= 3 && profile_extra_name(p)[0] != 0; }
/* the match's human h's player on this PC (their profile's feats and stats), -1 another machine's: online only this PC's
 * players count, each in their own profile (vplus/humans.c; offline every human is local, h = the player) */
int humans_slot(int h);
static int local_of(int h) { return humans_slot(h); }

Feats *feats_of(int player) { return &feats[slot(player)]; }
void feats_clear(int player)
{
    int p = slot(player);
    memset(&feats[p], 0, sizeof feats[0]);
    checks[p][0] = 0;
    stat_checks[p][0] = 0;
}

extern int killcam_replaying;
void feat_add(int player, FeatCounter c, int n)
{
    player = local_of(player);
    if (killcam_replaying) return;
    if (!valid(player) || c >= FEAT_COUNTERS) return;
    feats[player].count[c] += (uint32_t)n;
    feats[player].stat[online][feat_stat[c]] += (uint32_t)n;
    dirty[player] = 1;
    fprintf(stderr, "feats: player %d: %s %u\n", player + 1, counter_keys[c], feats[player].count[c]);
}

void feat_set(int player, FeatFlag f)
{
    player = local_of(player);
    if (killcam_replaying) return;
    if (!valid(player) || f >= FEAT_FLAGS) return;
    if (!(feats[player].flags & (1ull << f))) {
        feats[player].flags |= 1ull << f;
        dirty[player] = 1;
        fprintf(stderr, "feats: player %d: %s\n", player + 1,
                f < FEAT_REMOTE_KILL ? "special kill" : flag_keys[f - FEAT_REMOTE_KILL]);
    }
}

uint32_t feat_count(int player, FeatCounter c) { return c < FEAT_COUNTERS ? feats[slot(player)].count[c] : 0; }
int feat_has(int player, FeatFlag f) { return f < FEAT_FLAGS && (feats[slot(player)].flags >> f) & 1; }

void stats_online(int on) { online = on ? STATS_ONLINE : STATS_LOCAL; }
void stat_add(int player, StatCounter s, int n)
{
    player = local_of(player);
    if (killcam_replaying || !valid(player) || s >= STAT_COUNT || n <= 0) return;
    feats[player].stat[online][s] += (uint32_t)n;
    dirty[player] = 1;
    if (s != STAT_SECONDS && s != STAT_DAMAGE) fprintf(stderr, "stats: player %d: %s %u\n", player + 1, stat_keys[s], feats[player].stat[online][s]);
}
void stat_best_low(int player, StatCounter s, int v)
{
    player = local_of(player);
    if (killcam_replaying || !valid(player) || s >= STAT_COUNT || v <= 0) return;
    uint32_t cur = feats[player].stat[online][s];
    if (cur && (uint32_t)v >= cur) return;
    feats[player].stat[online][s] = (uint32_t)v;
    dirty[player] = 1;
    fprintf(stderr, "stats: player %d: %s %d\n", player + 1, stat_keys[s], v);
}
void stat_best(int player, StatCounter s, int v)
{
    player = local_of(player);
    if (killcam_replaying || !valid(player) || s >= STAT_COUNT || v <= 0) return;
    if ((uint32_t)v <= feats[player].stat[online][s]) return;
    feats[player].stat[online][s] = (uint32_t)v;
    dirty[player] = 1;
    fprintf(stderr, "stats: player %d: %s %d\n", player + 1, stat_keys[s], v);
}

int feat_all_weapons(int player)
{
    static const FeatFlag all[8] = {FEAT_MG_KILL, FEAT_FIRE_KILL, FEAT_HOMING_KILL, FEAT_REMOTE_KILL,
                                    FEAT_POWER_KILL, FEAT_NAPALM_KILL, FEAT_RICOCHET_KILL, FEAT_LIGHTNING_KILL};
    for (int i = 0; i < 8; i++)
        if (!feat_has(player, all[i])) return 0;
    return 1;
}

void profile_p2_save_feats(void); /* profile.c */
void profile_extra_save_feats(int p);
void feats_flush(void)
{
    if (dirty[0]) { dirty[0] = 0; profile_save(); }
    if (dirty[1]) { dirty[1] = 0; profile_p2_save_feats(); }
    for (int p = 2; p < 4; p++)
        if (dirty[p]) { dirty[p] = 0; profile_extra_save_feats(p); }
}

/* ---- the [feats] section ---- */

static uint64_t sip(const uint8_t *in, size_t len); /* below */

static uint64_t feats_check(int p)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "tm2pc feats 1:%llx", (unsigned long long)feats[p].flags);
    for (int i = 0; i < FEAT_COUNTERS && n < (int)sizeof buf - 16; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, ":%u", feats[p].count[i]);
    return sip((const uint8_t *)buf, (size_t)n);
}

/* [stats]: "local_<stat>" and "online_<stat>", with their own check (the [feats] check is as it was) */
static uint64_t stats_check(int p)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "tm2pc stats 1");
    for (int m = 0; m < STATS_MODES; m++)
        for (int i = 0; i < STAT_COUNT && n < (int)sizeof buf - 24; i++)
            if (i < STAT_V1_COUNT) n += snprintf(buf + n, sizeof buf - (size_t)n, ":%u", feats[p].stat[m][i]);
            else if (feats[p].stat[m][i]) n += snprintf(buf + n, sizeof buf - (size_t)n, ":%d=%u", i, feats[p].stat[m][i]);
    return sip((const uint8_t *)buf, (size_t)n);
}

void stats_key(int player, const char *key, const char *val)
{
    int p = slot(player);
    if (!strcmp(key, "check")) { snprintf(stat_checks[p], sizeof stat_checks[0], "%s", val); return; }
    int m = !strncmp(key, "local_", 6) ? STATS_LOCAL : !strncmp(key, "online_", 7) ? STATS_ONLINE : -1;
    if (m < 0) return;
    key += m == STATS_LOCAL ? 6 : 7;
    for (int i = 0; i < STAT_COUNT; i++)
        if (!strcmp(key, stat_keys[i])) feats[p].stat[m][i] = (uint32_t)strtoul(val, NULL, 10);
}

/* after the feats' check: the stats' own; none read (a profile from before the stats) starts the local set from
 * the feats' counters */
static void stats_verify(int p)
{
    char want[24];
    if (!stat_checks[p][0]) {
        memset(feats[p].stat, 0, sizeof feats[p].stat);
        for (int i = 0; i < FEAT_COUNTERS; i++) feats[p].stat[STATS_LOCAL][feat_stat[i]] = feats[p].count[i];
        return;
    }
    snprintf(want, sizeof want, "%016llx", (unsigned long long)stats_check(p));
    if (SDL_strcasecmp(want, stat_checks[p])) {
        memset(feats[p].stat, 0, sizeof feats[p].stat);
        fprintf(stderr, "profile: the stats do not match their check: cleared\n");
    }
}

void feats_key(int player, const char *key, const char *val)
{
    Feats *f = &feats[slot(player)];
    if (!strcmp(key, "check")) { snprintf(checks[slot(player)], sizeof checks[0], "%s", val); return; }
    for (int i = 0; i < FEAT_COUNTERS; i++)
        if (!strcmp(key, counter_keys[i])) f->count[i] = (uint32_t)strtoul(val, NULL, 10);
    if (!strncmp(key, "special_kill_", 13)) {
        for (int c = 0; c < 14; c++) {
            char k[48];
            snprintf(k, sizeof k, "special_kill_%s", profile_character_name(c));
            for (char *q = k; *q; q++) if (*q == ' ' || *q == '.') *q = '_';
            if (!SDL_strcasecmp(key, k) && (!SDL_strcasecmp(val, "yes") || !strcmp(val, "1")))
                f->flags |= 1ull << (FEAT_SPECIAL_KILL + c);
        }
    }
    for (int i = 0; i < FEAT_FLAGS - FEAT_REMOTE_KILL; i++)
        if (!strcmp(key, flag_keys[i]) && (!SDL_strcasecmp(val, "yes") || !strcmp(val, "1")))
            f->flags |= 1ull << (FEAT_REMOTE_KILL + i);
}

int feats_check_ok(int player)
{
    int p = slot(player);
    char want[24];
    int ok = 1;
    if (!checks[p][0]) { /* none read: none counted yet, or edited away */
        static const uint32_t zero[FEAT_COUNTERS];
        if (feats[p].flags || memcmp(feats[p].count, zero, sizeof zero)) ok = 0;
    } else {
        snprintf(want, sizeof want, "%016llx", (unsigned long long)feats_check(p));
        if (SDL_strcasecmp(want, checks[p])) ok = 0;
    }
    if (!ok) { memset(feats[p].count, 0, sizeof feats[p].count); feats[p].flags = 0; }
    stats_verify(p);
    return ok;
}

void feats_write(FILE *out, int player)
{
    int p = slot(player);
    const Feats *f = &feats[p];
    fprintf(out, "\n[feats]\n; what this profile has done in matches, for the portrait unlocks\n");
    for (int i = 0; i < FEAT_COUNTERS; i++) fprintf(out, "%s = %u\n", counter_keys[i], f->count[i]);
    for (int c = 0; c < 14; c++) {
        char k[48];
        snprintf(k, sizeof k, "special_kill_%s", profile_character_name(c));
        for (char *q = k; *q; q++) if (*q == ' ' || *q == '.') *q = '_';
        fprintf(out, "%s = %s\n", k, (f->flags >> (FEAT_SPECIAL_KILL + c)) & 1 ? "yes" : "no");
    }
    for (int i = 0; i < FEAT_FLAGS - FEAT_REMOTE_KILL; i++)
        fprintf(out, "%s = %s\n", flag_keys[i], (f->flags >> (FEAT_REMOTE_KILL + i)) & 1 ? "yes" : "no");
    fprintf(out, "; the feats' check: edited feats no longer match it, and are not used\ncheck = %016llx\n",
            (unsigned long long)feats_check(p));
    fprintf(out, "\n[stats]\n; the Stats screen's counts: local play and online play\n");
    for (int m = 0; m < STATS_MODES; m++)
        for (int i = 0; i < STAT_COUNT; i++)
            fprintf(out, "%s_%s = %u\n", m == STATS_LOCAL ? "local" : "online", stat_keys[i], f->stat[m][i]);
    fprintf(out, "; the stats' check: edited stats no longer match it, and are cleared\ncheck = %016llx\n",
            (unsigned long long)stats_check(p));
}

/* SipHash-2-4, with the feats' own key */
static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
static uint64_t sip(const uint8_t *in, size_t len)
{
    const uint64_t k0 = 0x5457324d46454154ull, k1 = 0x5354414c4c595321ull;
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
#define ROUND() do { v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32); v2 += v3; v3 = rotl(v3, 16); \
    v3 ^= v2; v0 += v3; v3 = rotl(v3, 21); v3 ^= v0; v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32); } while (0)
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t m;
        memcpy(&m, in + i, 8);
        v3 ^= m; ROUND(); ROUND(); v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t j = 0; i + j < len; j++) b |= (uint64_t)in[i + j] << (8 * j);
    v3 ^= b; ROUND(); ROUND(); v0 ^= b;
    v2 ^= 0xff;
    ROUND(); ROUND(); ROUND(); ROUND();
#undef ROUND
    return v0 ^ v1 ^ v2 ^ v3;
}

void rbs_keep(void *p, size_t n), rbs_live(void *p, size_t n); /* net/rb_state.c */
void feats_stats_rb_register(void) /* (feat_hooks.c's feats_rb_register) a step run again counts its stats again */
{
    rbs_keep(feats, sizeof feats);
    /* (not `dirty`: a write is owed whatever ran; a step run again that changes a stat marks it again) */
    for (int p = 0; p < 5; p++) /* (the time in matches: real time, counted each frame, not the step's) */
        for (int m = 0; m < STATS_MODES; m++) {
            rbs_live(&feats[p].stat[m][STAT_SECONDS], sizeof feats[p].stat[m][STAT_SECONDS]);
            rbs_live(&feats[p].stat[m][STAT_JOINS], sizeof feats[p].stat[m][STAT_JOINS]); /* (counted outside the steps) */
        }
}
