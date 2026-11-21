/* Mods (docs/MODDING.md sections 2 and 9): every folder under mods/<kind>/ with a mod.ini is a mod. At start each is
 * checked: its manifest ([mod]: id, uid, name, kind, version, author, game_version, requires), the files its kind
 * needs, the pictures' sizes, the mods it requires. A mod with a problem keeps the first one (the menus show it
 * grayed with it); `tm2pc --check-mod <folder>` prints every problem of one folder, for modders.
 *
 * Reading only: nothing here changes a match. Folders whose name starts with '_' (_TEMPLATE) are examples. */
#define _CRT_RAND_S /* (rand_s: a new mod's uid) */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "mods.h"
#include "mod_files.h"

const char *const mod_kind_folders[MOD_KINDS] = {"characters", "skins", "arenas", "tournaments"};
static const char *const kind_names[MOD_KINDS] = {"character", "skin", "arena", "tournament"};
static const char *const car_names[15] = { /* car-select numbers 0..14 (profile.c's characters, then Dark Tooth) */
    "Hammerhead", "Outlaw 2", "Warthog", "Mr. Grimm", "Grasshopper", "Thumper", "Spectre",
    "Roadkill", "Twister", "Axel", "Mr. Slam", "Shadow", "Sweet Tooth", "Minion", "Dark Tooth",
};

static Mod mods[MODS_MAX];
static int count;
static int verbose; /* --check-mod: every problem printed */

static void problem(Mod *m, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (!m->why[0]) snprintf(m->why, sizeof m->why, "%s", text);
    m->ok = 0;
    if (verbose) printf("  problem: %s\n", text);
}

/* ---- mod.ini ---- */
typedef struct { char section[32], key[32], value[400]; } IniLine;
enum { INI_MAX = 256 };
typedef struct { IniLine line[INI_MAX]; int n; } Ini;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t k = 0;
    while (s[k] && isspace((unsigned char)s[k])) k++;
    if (k) memmove(s, s + k, n - k + 1);
}
static int ini_read(const char *path, Ini *ini)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512], section[32] = "";
    ini->n = 0;
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, ';'); /* (comments, also after a value) */
        if (c) *c = 0;
        trim(line);
        if (!line[0]) continue;
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            snprintf(section, sizeof section, "%s", line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq || ini->n >= INI_MAX) continue;
        *eq = 0;
        IniLine *l = &ini->line[ini->n++];
        snprintf(l->section, sizeof l->section, "%s", section);
        snprintf(l->key, sizeof l->key, "%s", line);
        snprintf(l->value, sizeof l->value, "%s", eq + 1);
        trim(l->key);
        trim(l->value);
    }
    fclose(f);
    return 1;
}
static const char *ini_get(const Ini *ini, const char *section, const char *key)
{
    for (int i = 0; i < ini->n; i++)
        if (!SDL_strcasecmp(ini->line[i].section, section) && !SDL_strcasecmp(ini->line[i].key, key)) return ini->line[i].value;
    return NULL;
}

/* ---- files ---- */
static long file_size(const Mod *m, const char *name)
{
    char path[700];
    snprintf(path, sizeof path, "%s%s", m->dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}
static void need_file(Mod *m, const char *name)
{
    long n = file_size(m, name);
    if (n < 0) problem(m, "missing %s", name);
    else if (n == 0) problem(m, "%s is empty", name);
    else if (n > 16L * 1024 * 1024) problem(m, "%s is over 16 MB", name);
}
/* a picture: a BMP of this size (0: any; negative: at least -w x -h, the same shape), required or not */
static void check_bmp(Mod *m, const char *name, int w, int h, int required)
{
    char path[700];
    snprintf(path, sizeof path, "%s%s", m->dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (required) problem(m, "missing %s", name);
        return;
    }
    unsigned char hd[26] = {0};
    size_t got = fread(hd, 1, sizeof hd, f);
    fclose(f);
    if (got < sizeof hd || hd[0] != 'B' || hd[1] != 'M') { problem(m, "%s is not a BMP picture", name); return; }
    int pw = hd[18] | hd[19] << 8 | hd[20] << 16 | hd[21] << 24, ph = hd[22] | hd[23] << 8 | hd[24] << 16 | hd[25] << 24;
    if (ph < 0) ph = -ph; /* (top-down) */
    if (w > 0 && (pw != w || ph != h)) problem(m, "%s is %d x %d, not %d x %d", name, pw, ph, w, h);
    if (w < 0 && (pw < -w || ph < -h || (long)pw * -h != (long)ph * -w))
        problem(m, "%s is %d x %d: %d x %d or larger, the same shape", name, pw, ph, -w, -h);
}
static int car_number(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < 15; i++)
        if (!SDL_strcasecmp(name, car_names[i])) return i;
    return -1;
}

/* ---- a character's numbers ([character], MODDING.md 4.2; the decomp's CAR_PROFILES.md 2.1 and CAR_STATS.md 2) ----
 * Each key a stats-block field (offset, width) within limits around the 15 cars' own values, so a mod cannot make an
 * unbeatable or broken car by accident (a mass of 100 doubles a human's damage; above it spin-outs turn around). */
static const struct { const char *key; unsigned char offset, width; short lo, hi; } stat_keys[] = {
    {"top_speed", 0x52, 2, 80, 150},    /* the cars: 92 (Sweet Tooth, Mr. Slam) .. 141 (Twister) */
    {"acceleration", 0xAC, 4, 3, 12},   /* 4 (Mr. Slam) .. 11 (Mr. Grimm) */
    {"brakes", 0x5C, 2, 1, 5},          /* 1 (Hammerhead) .. 4 (Thumper) */
    {"turning", 0x4C, 2, 60, 110},      /* the speed below which it turns less: lower turns tighter; 65 .. 105 */
    {"steering", 0x6C, 2, 2, 9},        /* the low-speed steering rate: 3 .. 8 */
    {"slide_speed", 0x56, 2, 20, 150},  /* above it the car slides: 25 (Hammerhead) .. 150 (Minion) */
    {"armor", 0x3A, 2, 50, 300},        /* max HP: 65 (Mr. Grimm) .. 300 (Dark Tooth); a player gets 20 more */
    {"mass", 0x7A, 2, 5, 90},           /* 5 (Mr. Grimm) .. 60 (Minion); Dark Tooth's 100 is his alone */
    {"track_width", 0x48, 2, 40, 180},  /* the hit box: 50 (Shadow) .. 173 (Dark Tooth) */
    {"front_axle", 0x4E, 2, 30, 230},   /*   41 .. 216 */
    {"rear_axle", 0x50, 2, 30, 280},    /*   33 .. 269 (the game's is negative: behind the middle) */
    {"ride_height", 0x42, 2, 8, 120},   /*   12 .. 116 */
    {"roof_height", 0x46, 2, 24, 110},  /*   32 .. 106 */
};
/* the base cars' track width and rollover threshold (+0x48, +0x3E): a wider character tips over as its base would */
static const short base_track[15] = {86, 52, 56, 53, 73, 52, 52, 52, 72, 116, 74, 50, 66, 108, 173};
static const short base_roll[15] = {86, 52, 56, 159, 73, 52, 52, 52, 72, 116, 74, 50, 44, 108, 173};

static int section_value(Mod *m, const Ini *ini, const char *sec, const char *key, int lo, int hi, int *out)
{
    const char *v = ini_get(ini, sec, key);
    if (!v) return 0;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi) { problem(m, "[%s] %s = %s: %d to %d", sec, key, v, lo, hi); return 0; }
    *out = (int)n;
    return 1;
}
static int stat_value(Mod *m, const Ini *ini, const char *key, int lo, int hi, int *out)
{
    return section_value(m, ini, "character", key, lo, hi, out);
}

static void add_stat(ModStats *s, int offset, int width, int value);
/* [ai]: the computer driver (CAR_STATS.md 2, its stats-block AI numbers; the cars' own in the comments). Its mood is
 * rolled each time a mood's time is up: hunt (chase its target), attack, or else flee; the chances blend from the
 * full-health ones to the hurt ones as it loses health. Times in the game's ticks (600 most cars'). */
static const struct { const char *key; unsigned char offset; short lo, hi; } ai_keys[] = {
    {"hunt", 0x7C, 0, 100},           /* at full health: 20 (Outlaw) .. 100 (Hammerhead, Mr. Slam, Minion) */
    {"hunt_hurt", 0x7E, 0, 100},      /* at no health: 0 .. 100 */
    {"attack", 0x80, 0, 100},         /* at full health: 0 .. 80 (Outlaw); what is left of 100 is fleeing */
    {"attack_hurt", 0x82, 0, 100},    /* at no health: 0 .. 100 */
    {"hunt_time", 0x86, 150, 3600},   /* 300 .. 3600 (Hammerhead) */
    {"attack_time", 0x88, 150, 3600}, /* 300 .. 1800 (Mr. Grimm) */
    {"flee_time", 0x8A, 150, 3600},   /* 300 .. 600 */
    {"retaliate", 0x8E, 0, 100},      /* the share of its health it takes from one car before attacking it: 7 .. 100 */
    {"pass", 0x90, 0, 100},           /* its chance to try to pass a car in its way: 0 .. 90 */
    {"patience", 0x24, 10, 200},      /* how long a target may pull away before it plans a new way to it: 12 .. 180 */
};
static void check_ai(Mod *m, const Ini *ini)
{
    ModStats *s = &m->stats;
    int v, val[sizeof ai_keys / sizeof ai_keys[0]], set[sizeof ai_keys / sizeof ai_keys[0]];
    for (size_t k = 0; k < sizeof ai_keys / sizeof ai_keys[0]; k++) {
        set[k] = section_value(m, ini, "ai", ai_keys[k].key, ai_keys[k].lo, ai_keys[k].hi, &val[k]);
        if (set[k]) add_stat(s, ai_keys[k].offset, 2, val[k]);
    }
    /* hunt and attack together at most 100 (each set one with its base's other: the mod's own check only) */
    if (set[0] && set[2] && val[0] + val[2] > 100) problem(m, "[ai] hunt + attack = %d: 100 at most", val[0] + val[2]);
    if (set[1] && set[3] && val[1] + val[3] > 100) problem(m, "[ai] hunt_hurt + attack_hurt = %d: 100 at most", val[1] + val[3]);
    if (section_value(m, ini, "ai", "fire_chance", 1, 100, &v)) s->ai_fire = (unsigned char)v;
}
static void add_stat(ModStats *s, int offset, int width, int value)
{
    if (s->n >= MOD_STATS) return;
    s->offset[s->n] = (unsigned char)offset;
    s->width[s->n] = (unsigned char)width;
    s->value[s->n++] = (short)value;
}
static void check_stats(Mod *m, const Ini *ini)
{
    ModStats *s = &m->stats;
    memset(s, 0, sizeof *s);
    int v;
    for (size_t k = 0; k < sizeof stat_keys / sizeof stat_keys[0]; k++) {
        if (!stat_value(m, ini, stat_keys[k].key, stat_keys[k].lo, stat_keys[k].hi, &v)) continue;
        add_stat(s, stat_keys[k].offset, stat_keys[k].width, stat_keys[k].offset == 0x50 ? -v : v);
        if (stat_keys[k].offset == 0x48 && m->base >= 0) add_stat(s, 0x3E, 2, v * base_roll[m->base] / base_track[m->base]);
    }
    if (stat_value(m, ini, "step_height", 40, 160, &v)) s->flags |= MOD_STAT_STEP, s->step = (short)v;
    int recharge = 0, most = 0;
    if (stat_value(m, ini, "special_recharge", 300, 2400, &recharge) | stat_value(m, ini, "special_max", 1, 6, &most)) {
        s->flags |= MOD_STAT_SPECIAL; /* (one without the other: the other the base's, 0 in the profile) */
        s->recharge = (short)recharge;
        s->max_specials = (signed char)most;
    }
    /* the special (decomp request 73, SPECIAL_TEMPLATES.md 6): another car's or a pickup weapon's, and its numbers */
    static const struct { const char *name; short type; } templates[] = {
        {"sweet_tooth", 0x10}, {"outlaw", 0x30}, {"warthog", 0x40}, {"mr_grimm", 0x50}, {"thumper", 0x70}, {"spectre", 0x80},
        {"roadkill", 0x90},    {"axel", 0xB0},   {"shadow", 0xD0},  {"minion", 0xE0},   {"fire_missile", 0x101},
        {"homing_missile", 0x102}, {"remote_bomb", 0x103}, {"power_missile", 0x104}, {"napalm", 0x105},
        {"ricochet", 0x106},   {"mega_napalm", 0x10A}, {"freeze_missile", 0x164}};
    const char *sp = ini_get(ini, "character", "special");
    if (sp && SDL_strcasecmp(sp, "own")) {
        size_t k = 0;
        while (k < sizeof templates / sizeof templates[0] && SDL_strcasecmp(sp, templates[k].name)) k++;
        if (k == sizeof templates / sizeof templates[0])
            problem(m, "[character] special = %s: own, a car's (sweet_tooth, outlaw, warthog, mr_grimm, thumper, spectre, "
                       "roadkill, axel, shadow, minion) or a weapon's (fire_missile, homing_missile, remote_bomb, "
                       "power_missile, napalm, ricochet, mega_napalm, freeze_missile)", sp);
        else s->special = templates[k].type;
    }
    static const struct { const char *key; unsigned char field; short lo, hi; } sp_keys[] = {
        {"special_damage", 1, 0, 8191},        {"special_freeze_damage", 2, 0, 8191}, {"special_life", 3, 1, 8191},
        {"special_speed", 4, 1, 150},          {"special_turn", 5, 1, 4095},          {"special_range", 6, 1, 4000},
        {"special_height", 7, 1, 4000},        {"special_duration", 8, 40, 400},      {"special_freeze_time", 9, 1, 6000},
        {"special_burn_time", 10, 1, 2000},    {"special_blast_damage", 11, 0, 8191}, {"special_blast_radius", 12, 1, 400},
        {"special_turn_back", 13, 1, 4000}};
    for (size_t k = 0; k < sizeof sp_keys / sizeof sp_keys[0] && s->sp_n < 16; k++)
        if (stat_value(m, ini, sp_keys[k].key, sp_keys[k].lo, sp_keys[k].hi, &v)) {
            s->sp_field[s->sp_n] = sp_keys[k].field;
            s->sp_value[s->sp_n++] = (short)v;
        }
    if (stat_value(m, ini, "ai_special_ahead", 1, 100, &v)) s->ai_ahead = (unsigned char)v;
    if (stat_value(m, ini, "ai_special_behind", 1, 100, &v)) s->ai_behind = (unsigned char)v;
    const char *c = ini_get(ini, "character", "radar_color");
    if (c) {
        int r, g, b;
        if (sscanf(c, "%d , %d , %d", &r, &g, &b) != 3 || r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255)
            problem(m, "[character] radar_color = %s: red, green, blue (0 to 255 each)", c);
        else s->flags |= MOD_STAT_RADAR, s->radar[0] = (unsigned char)r, s->radar[1] = (unsigned char)g, s->radar[2] = (unsigned char)b;
    }
    check_ai(m, ini);
}

/* team_paint = 16-47, 80, 82-95: the palette entries (0..255) of car.tms's first image, its body texture, that take
 * the team's color in team games (as the game's cars, vplus/team_paint.h); without it the car keeps its own colors */
static void check_team(Mod *m, const Ini *ini, const char *sec)
{
    const char *v = ini_get(ini, sec, "team_paint");
    if (!v) return;
    char list[400];
    snprintf(list, sizeof list, "%s", v);
    for (char *t = strtok(list, ", "); t; t = strtok(NULL, ", ")) {
        char *end;
        long a = strtol(t, &end, 10), b = a;
        if (*end == '-') b = strtol(end + 1, &end, 10);
        if (end == t || *end || a < 0 || b > 255 || a > b) {
            problem(m, "[%s] team_paint = %s: palette entries 0 to 255, as 16-47, 80", sec, v);
            return;
        }
        for (long i = a; i <= b; i++) m->stats.team_paint[i >> 3] |= (unsigned char)(1 << (i & 7));
    }
    m->stats.has_team = 1;
}

/* ---- one mod ---- */
static void check_manifest(Mod *m, const Ini *ini)
{
    const char *v;
    snprintf(m->id, sizeof m->id, "%s", (v = ini_get(ini, "mod", "id")) ? v : "");
    snprintf(m->uid, sizeof m->uid, "%s", (v = ini_get(ini, "mod", "uid")) ? v : "");
    snprintf(m->name, sizeof m->name, "%s", (v = ini_get(ini, "mod", "name")) ? v : "");
    snprintf(m->version, sizeof m->version, "%s", (v = ini_get(ini, "mod", "version")) ? v : "");
    snprintf(m->author, sizeof m->author, "%s", (v = ini_get(ini, "mod", "author")) ? v : "");
    snprintf(m->requires, sizeof m->requires, "%s", (v = ini_get(ini, "mod", "requires")) ? v : "");
    m->format = (v = ini_get(ini, "mod", "game_version")) ? atoi(v) : 0;
    if (!m->id[0]) problem(m, "mod.ini has no id");
    for (const char *p = m->id; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-')) {
            problem(m, "the id \"%s\": only lowercase letters, digits and '-'", m->id);
            break;
        }
    if (!m->name[0]) problem(m, "mod.ini has no name");
    if (strlen(m->uid) != 32 || strspn(m->uid, "0123456789abcdef") != 32)
        problem(m, "the uid is not 32 lowercase hex digits (the mod tool makes one)");
    v = ini_get(ini, "mod", "kind");
    if (!v || SDL_strcasecmp(v, kind_names[m->kind]))
        problem(m, "kind = %s in mod.ini, but it is in mods/%s", v ? v : "(none)", mod_kind_folders[m->kind]);
    if (m->format < 1) problem(m, "mod.ini has no game_version");
    else if (m->format > MODS_FORMAT) problem(m, "made for a newer game (game_version %d; this game reads %d)", m->format, MODS_FORMAT);
}
/* an arena's files and arena.ini against the game's limits (mod_arena.c loads them; docs/ARENA_LOADING.md) */
static void check_arena(Mod *m)
{
    long dmd = file_size(m, "arena.dmd"), pts = file_size(m, "arena.pts"), grp = file_size(m, "arena.grp");
    long rmp = file_size(m, "arena.rmp");
    long tms = file_size(m, "arena.tms");
    if (dmd > 737300) problem(m, "arena.dmd is %ld bytes: at most 737,300 (the game's level memory)", dmd);
    if (tms > 737300) problem(m, "arena.tms is %ld bytes: at most 737,300 (it loads to the level memory first)", tms);
    if (pts > 0xD20) problem(m, "arena.pts is %ld bytes: at most 3360 (140 route points)", pts);
    if (grp > 0 && grp != 6036) problem(m, "arena.grp is %ld bytes, not 6036 (the game's GRP)", grp);
    if (rmp > 0 && rmp != 6300) problem(m, "arena.rmp is %ld bytes, not 6300 (225 surfaces)", rmp);
    Ini *ini = malloc(sizeof *ini);
    char path[700];
    snprintf(path, sizeof path, "%sarena.ini", m->dir);
    if (!ini || !ini_read(path, ini)) { free(ini); return; }
    const char *v;
    int points = (v = ini_get(ini, "arena", "points")) ? atoi(v) : 0;
    if (points < 1 || points > 140) problem(m, "[arena] points = %s: 1 to 140 (the route points in arena.pts)", v ? v : "(none)");
    else if (pts >= 0 && points * 24 > pts) problem(m, "[arena] points = %d, but arena.pts holds %ld", points, pts / 24);
    v = ini_get(ini, "arena", "sections");
    if (!v || atoi(v) < 1 || atoi(v) > 35) problem(m, "[arena] sections = %s: 1 to 35 (arena.grp's)", v ? v : "(none)");
    v = ini_get(ini, "arena", "surfaces");
    if (!v || atoi(v) < 0 || atoi(v) > 225) problem(m, "[arena] surfaces = %s: 0 to 225 (arena.rmp's)", v ? v : "(none)");
    if (!(v = ini_get(ini, "arena", "starts"))) problem(m, "[arena] has no starts (route points 0..%d)", points > 0 ? points - 1 : 139);
    else {
        int n = 0;
        for (const char *p = v; *p;) {
            char *end;
            long s = strtol(p, &end, 10);
            if (end == p) { p++; continue; }
            if (s < 0 || (points > 0 && s >= points)) { problem(m, "[arena] starts: %ld is not a route point (0..%d)", s, points - 1); break; }
            n++, p = end;
        }
        if (n > 15) problem(m, "[arena] starts: %d, at most 15", n);
    }
    free(ini);
}
static void check_kind(Mod *m, const Ini *ini)
{
    m->base = -1;
    switch (m->kind) {
    case MOD_SKIN:
    case MOD_CHARACTER: {
        const char *sec = m->kind == MOD_SKIN ? "skin" : "character", *key = m->kind == MOD_SKIN ? "car" : "base";
        const char *car = ini_get(ini, sec, key);
        m->base = car_number(car);
        if (m->base < 0) problem(m, car ? "[%s] %s = %s: no such car" : "[%s] has no %s (the car it is built on)", sec, key, car);
        need_file(m, "car.dmd");
        need_file(m, "car.tms");
        if (file_size(m, "carselect.dmd") >= 0 || file_size(m, "carselect.tms") >= 0) { /* (optional, as a pair) */
            need_file(m, "carselect.dmd");
            need_file(m, "carselect.tms");
        }
        int portraits = m->kind == MOD_CHARACTER; /* (a skin may keep its car's) */
        check_bmp(m, "portrait_1.bmp", -64, -128, portraits); /* (car select: the driver; 2 once chosen) */
        check_bmp(m, "portrait_2.bmp", -64, -128, portraits);
        check_bmp(m, "carinfo.bmp", 640, 480, portraits);     /* (CAR INFO: its stats screen; a skin may keep its car's) */
        check_bmp(m, "plate.bmp", -128, -64, portraits);      /* (the line-up's and lobby's picture of the car) */
        if (m->kind == MOD_CHARACTER) check_stats(m, ini);   /* (a skin: its car's numbers, always) */
        if (m->kind == MOD_CHARACTER) { /* its sounds (optional: else its base's) */
            static const struct { const char *name; int seconds; } wavs[] = {{"engine.wav", MOD_ENGINE_SECONDS}, {"special.wav", MOD_SPECIAL_SECONDS}};
            for (int k = 0; k < 2; k++) {
                if (file_size(m, wavs[k].name) < 0) continue;
                char path[700], why[80];
                ModWav w;
                snprintf(path, sizeof path, "%s%s", m->dir, wavs[k].name);
                if (!mod_wav_read(path, &w, wavs[k].seconds, why, sizeof why)) problem(m, "%s %s", wavs[k].name, why);
                mod_wav_free(&w);
            }
            if (file_size(m, "ending.mpg") >= 0) { /* its ending's video (optional) */
                char path[700], why[120];
                snprintf(path, sizeof path, "%sending.mpg", m->dir);
                if (!mod_video_check(path, why, sizeof why)) problem(m, "ending.mpg %s", why);
            }
        }
        check_team(m, ini, sec);
        break;
    }
    case MOD_ARENA:
        need_file(m, "arena.dmd");
        need_file(m, "arena.tms");
        need_file(m, "arena.grp");
        need_file(m, "arena.rmp");
        need_file(m, "arena.pts"); /* (the computer cars' route points: at most 140, 35 sections) */
        need_file(m, "arena.ini");
        check_bmp(m, "card.bmp", 344, 260, 1);
        check_bmp(m, "wide.bmp", 512, 256, 0);
        check_bmp(m, "lineup.bmp", -320, -240, 1); /* (the line-up's picture: 320 x 240 or larger, 4:3) */
        check_arena(m);
        break;
    case MOD_TOURNAMENT: {
        const char *v = ini_get(ini, "tournament", "levels");
        int levels = v ? atoi(v) : 0;
        if (levels < 1 || levels > 16) problem(m, "[tournament] levels = %s: 1 to 16", v ? v : "(none)");
        for (int k = 1; k <= levels && k <= 16; k++) {
            char sec[16];
            snprintf(sec, sizeof sec, "level%d", k);
            if (!ini_get(ini, sec, "arena")) problem(m, "[%s] has no arena", sec);
        }
        if (!ini_get(ini, "tournament", "drivers")) problem(m, "[tournament] has no drivers");
        break;
    }
    }
}
static void check_folder(Mod *m)
{
    Ini *ini = malloc(sizeof *ini);
    if (!ini) return;
    char path[700];
    snprintf(path, sizeof path, "%smod.ini", m->dir);
    m->ok = 1;
    if (!ini_read(path, ini)) problem(m, "missing mod.ini");
    else {
        check_manifest(m, ini);
        check_kind(m, ini);
    }
    free(ini);
}

/* ---- across mods ---- */
static const Mod *find_id(const char *id, const Mod *not)
{
    for (int i = 0; i < count; i++)
        if (&mods[i] != not && !strcmp(mods[i].id, id)) return &mods[i];
    return NULL;
}
static void check_across(void)
{
    for (int i = 0; i < count; i++) {
        Mod *m = &mods[i];
        if (!m->id[0]) continue;
        for (int k = 0; k < i; k++)
            if (!strcmp(mods[k].id, m->id) && !strcmp(mods[k].uid, m->uid))
                problem(m, "the same mod as %s (id and uid): keep one", mods[k].dir);
        char req[200];
        snprintf(req, sizeof req, "%s", m->requires);
        for (char *t = strtok(req, ", "); t; t = strtok(NULL, ", "))
            if (!find_id(t, m)) problem(m, "needs %s", t);
    }
    for (int pass = 0; pass < 4; pass++) /* (a mod needing one with a problem has one too) */
        for (int i = 0; i < count; i++) {
            Mod *m = &mods[i];
            if (!m->ok) continue;
            char req[200];
            snprintf(req, sizeof req, "%s", m->requires);
            for (char *t = strtok(req, ", "); t; t = strtok(NULL, ", ")) {
                const Mod *r = find_id(t, m);
                if (r && !r->ok) { problem(m, "needs %s, which has a problem", t); break; }
            }
        }
}

static int by_kind_and_name(const void *a, const void *b)
{
    const Mod *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind;
    return SDL_strcasecmp(x->name[0] ? x->name : x->dir, y->name[0] ? y->name : y->dir);
}

static char scanned_folder[600];
static int generation;
int mods_scan(const char *game_folder)
{
    if (game_folder != scanned_folder) snprintf(scanned_folder, sizeof scanned_folder, "%s", game_folder);
    generation++; /* (the mods' indices may change: their users rebuild) */
    count = 0;
    for (int k = 0; k < MOD_KINDS; k++) {
        char dir[600];
        snprintf(dir, sizeof dir, "%smods/%s", game_folder, mod_kind_folders[k]);
        int n = 0;
        char **list = SDL_GlobDirectory(dir, "*", 0, &n);
        for (int i = 0; list && i < n && count < MODS_MAX; i++) {
            if (list[i][0] == '_' || list[i][0] == '.') continue; /* (_TEMPLATE: an example) */
            Mod *m = &mods[count];
            memset(m, 0, sizeof *m);
            m->kind = k;
            snprintf(m->dir, sizeof m->dir, "%s/%s/", dir, list[i]);
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(m->dir, &info) || info.type != SDL_PATHTYPE_DIRECTORY) continue;
            check_folder(m);
            ModFile files[MOD_FILES_MAX];
            m->fingerprint = 0;
            m->bytes = 0;
            int nf = mod_package(m->dir, k, files, MOD_FILES_MAX, &m->fingerprint);
            for (int f = 0; f < nf; f++) m->bytes += files[f].size;
            count++;
        }
        SDL_free(list);
    }
    check_across();
    qsort(mods, (size_t)count, sizeof mods[0], by_kind_and_name);
    int ok = 0;
    for (int i = 0; i < count; i++) {
        ok += mods[i].ok;
        if (!mods[i].ok) fprintf(stderr, "mods: %s: %s\n", mods[i].dir, mods[i].why);
    }
    if (count) fprintf(stderr, "mods: %d found, %d without problems\n", count, ok);
    return count;
}
int mods_count(void) { return count; }
int mods_rescan(void) { return scanned_folder[0] ? mods_scan(scanned_folder) : count; } /* (after a download) */
const char *mods_game_folder(void) { return scanned_folder; }

void settings_save_section(const char *name); /* settings.c */

/* ---- the switches ([mods] in tm2pc.ini: uid = on / off; kept for mods not installed now) ---- */
typedef struct { char uid[40]; int on; } Switch;
static Switch switches[MODS_MAX];
static int switch_count;
static Switch *switch_of(const char *uid, int add)
{
    for (int i = 0; i < switch_count; i++)
        if (!strcmp(switches[i].uid, uid)) return &switches[i];
    if (!add || switch_count >= MODS_MAX || !uid[0]) return NULL;
    Switch *w = &switches[switch_count++];
    snprintf(w->uid, sizeof w->uid, "%s", uid);
    w->on = 1;
    return w;
}
static int cpu_characters = 1; /* random computer drivers include the characters that are on */
int mods_cpu_characters(void) { return cpu_characters; }
void mods_set_cpu_characters(int on)
{
    if (cpu_characters == on) return;
    cpu_characters = on;
    settings_save_section("mods");
}
void mods_ini_key(const char *key, const char *val)
{
    if (!SDL_strcasecmp(key, "random_characters")) { cpu_characters = SDL_strcasecmp(val, "off") != 0; return; }
    Switch *w = switch_of(key, 1);
    if (w) w->on = SDL_strcasecmp(val, "off") != 0;
}
void mods_ini_write(FILE *f)
{
    fprintf(f, "[mods]\n; random_characters: Random computer drivers include the mod characters that are on (Challenge Match,\n"
               ";   Custom Tournament, Survival; never the game's Tournament): on or off\n");
    fprintf(f, "random_characters = %s\n", cpu_characters ? "on" : "off");
    fprintf(f, "; each mod on or off (Options > Mods), by its uid; a mod not listed is on\n");
    for (int i = 0; i < switch_count; i++) {
        const char *name = "";
        for (int k = 0; k < count; k++)
            if (!strcmp(mods[k].uid, switches[i].uid)) name = mods[k].name;
        fprintf(f, "%s = %s%s%s\n", switches[i].uid, switches[i].on ? "on" : "off", name[0] ? " ; " : "", name);
    }
}
static char online_uids[MODS_MAX][40];
static int online_count = -1; /* -1: no server's set */
void mods_online_set(const char (*uids)[40], int count)
{
    online_count = uids ? (count < 0 ? 0 : count > MODS_MAX ? MODS_MAX : count) : -1;
    for (int i = 0; i < online_count; i++) snprintf(online_uids[i], sizeof online_uids[i], "%s", uids[i]);
    generation++;
}
int mods_online(void) { return online_count >= 0; }
const char *mods_online_uid(int i) { return i >= 0 && i < online_count ? online_uids[i] : NULL; } /* (the server's order) */
int mods_find(const char *uid, unsigned long long fingerprint)
{
    for (int i = 0; i < count; i++)
        if (mods[i].ok && !strcmp(mods[i].uid, uid) && mods[i].fingerprint == fingerprint) return i;
    return -1;
}
int mod_on(const Mod *m)
{
    if (!m->ok) return 0;
    if (online_count >= 0) { /* a server's: its mods only */
        for (int i = 0; i < online_count; i++)
            if (!strcmp(online_uids[i], m->uid)) return 1;
        return 0;
    }
    const Switch *w = switch_of(m->uid, 0);
    return !w || w->on;
}
void mod_set_on(int i, int on)
{
    if (i < 0 || i >= count || !mods[i].uid[0]) return;
    Switch *w = switch_of(mods[i].uid, 1);
    if (!w || w->on == on) return;
    w->on = on;
    generation++;
    settings_save_section("mods");
}
int mods_generation(void) { return generation; }
const Mod *mods_get(int i) { return i >= 0 && i < count ? &mods[i] : NULL; }

int mods_check_one(const char *folder)
{
    Mod *m = &mods[0];
    memset(m, 0, sizeof *m);
    size_t n = strlen(folder);
    snprintf(m->dir, sizeof m->dir, "%s%s", folder, n && (folder[n - 1] == '/' || folder[n - 1] == '\\') ? "" : "/");
    /* its kind: the folder it is in (mods/<kind>/<mod>), else mod.ini's kind */
    m->kind = -1;
    char up[600];
    snprintf(up, sizeof up, "%s", m->dir);
    up[strlen(up) - 1] = 0;
    char *slash = strrchr(up, '/'), *back = strrchr(up, '\\');
    if (back > slash) slash = back;
    if (slash) {
        *slash = 0;
        char *s2 = strrchr(up, '/'), *b2 = strrchr(up, '\\');
        const char *parent = (b2 > s2 ? b2 : s2) ? (b2 > s2 ? b2 : s2) + 1 : up;
        for (int k = 0; k < MOD_KINDS; k++)
            if (!SDL_strcasecmp(parent, mod_kind_folders[k])) m->kind = k;
    }
    verbose = 1;
    printf("checking %s\n", m->dir);
    if (m->kind < 0) {
        Ini *ini = malloc(sizeof *ini);
        char path[700];
        snprintf(path, sizeof path, "%smod.ini", m->dir);
        const char *v = ini && ini_read(path, ini) ? ini_get(ini, "mod", "kind") : NULL;
        for (int k = 0; v && k < MOD_KINDS; k++)
            if (!SDL_strcasecmp(v, kind_names[k])) m->kind = k;
        free(ini);
        if (m->kind < 0) {
            printf("  problem: not in a mods/<kind> folder and mod.ini names no kind (character, skin, arena, tournament)\n");
            return 0;
        }
    }
    check_folder(m);
    printf(m->ok ? "  OK: %s (%s)\n" : "  %s (%s): has problems (requires are checked when the game starts)\n",
           m->name[0] ? m->name : "(no name)", kind_names[m->kind]);
    verbose = 0;
    return m->ok;
}

/* ---- tm2pc --new-mod <kind> <id>: a new mod folder for modders (MODDING.md 2) ----
 * mods/<kind>s/<id>/mod.ini from the kind's _TEMPLATE, with the id and a fresh uid: 32 hex digits from the system's
 * random source (128 bits: two modders never get the same one, so no list of uids is needed). */
static void random_bytes(unsigned char *b, int n)
{
#ifdef _WIN32
    for (int i = 0; i < n; i++) { unsigned v = 0; if (rand_s(&v)) v = (unsigned)SDL_rand_bits(); b[i] = (unsigned char)v; }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, (size_t)n, f) != (size_t)n) for (int i = 0; i < n; i++) b[i] = (unsigned char)SDL_rand_bits();
    if (f) fclose(f);
#endif
}
int mods_new(const char *kind, const char *id)
{
    int k = -1;
    for (int i = 0; i < MOD_KINDS; i++)
        if (!SDL_strcasecmp(kind, kind_names[i]) || !SDL_strcasecmp(kind, mod_kind_folders[i])) k = i;
    if (k < 0) { printf("--new-mod: the kind is character, skin, arena or tournament\n"); return 0; }
    if (!id[0] || strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(id)) {
        printf("--new-mod: the id \"%s\": only lowercase letters, digits and '-' (as crimson-fury)\n", id);
        return 0;
    }
    const char *base = SDL_GetBasePath();
    char tpl[700], dir[700], path[760];
    snprintf(tpl, sizeof tpl, "%smods/%s/_TEMPLATE/mod.ini", base ? base : "", mod_kind_folders[k]);
    snprintf(dir, sizeof dir, "%smods/%s/%s", base ? base : "", mod_kind_folders[k], id);
    char sep = base && strchr(base, '\\') ? '\\' : '/'; /* (the paths as the system writes them) */
    for (char *p = tpl; *p; p++) if (*p == '/') *p = sep;
    for (char *p = dir; *p; p++) if (*p == '/') *p = sep;
    FILE *in = fopen(tpl, "r");
    if (!in) { printf("--new-mod: no %s to start from\n", tpl); return 0; }
    SDL_PathInfo info;
    if (SDL_GetPathInfo(dir, &info)) { fclose(in); printf("--new-mod: %s is there already\n", dir); return 0; }
    if (!SDL_CreateDirectory(dir)) { fclose(in); printf("--new-mod: cannot make %s\n", dir); return 0; }
    snprintf(path, sizeof path, "%s%cmod.ini", dir, sep);
    FILE *out = fopen(path, "w");
    if (!out) { fclose(in); printf("--new-mod: cannot write %s\n", path); return 0; }
    unsigned char b[16];
    char uid[33];
    random_bytes(b, 16);
    for (int i = 0; i < 16; i++) snprintf(uid + 2 * i, 3, "%02x", b[i]);
    char line[512];
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, "id =", 4) || !strncmp(line, "id=", 3)) fprintf(out, "id = %s\n", id);
        else if (!strncmp(line, "uid =", 5) || !strncmp(line, "uid=", 4)) fprintf(out, "uid = %s\n", uid);
        else fputs(line, out);
    }
    fclose(in);
    fclose(out);
    printf("made %s (uid %s)\nnext: edit its mod.ini (name, author...), add its files (the template's GUIDE.txt), then\n"
           "  tm2pc --check-mod \"%s\"\n", path, uid, dir);
    return 1;
}
