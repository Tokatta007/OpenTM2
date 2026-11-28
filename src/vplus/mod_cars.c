/* Skins from mods/skins (docs/MODDING.md 5) and characters from mods/characters (4): each is the decomp's mod car (car_numbers.h, request 42, CUSTOM_TOURNAMENT_ANSWERS.md items 3 and 5):
 * number 0x20 + m in portModCars[m], built on its base car (the settings byte: the base's stats, special, sounds and
 * hit box), with its own model and textures handed to the load through portCarSource (CAR_GROUPS.md 7.7).
 *
 * - The table: portModCars[m] for every skin without a problem, in the scan's order (so m stays the same while the
 *   game runs); `used` follows its switch in Options > Mods. Car select offers them offline only: online every
 *   machine hashes the cars' models (rollback), so a server's skins only, the same files on every machine
 *   (net/lobby_mods.c: mods_online_set).
 * - Car select (the user's, 2026-10-25): Left / Right go from car to car; Select cycles the highlighted car's looks,
 *   the car's own and then each of its skins that is on (mod_next_look). The carousel's mod children show the base's
 *   turning car until the shown one's own model is loaded (mod_cars_carsel_frame, decomp request 71, CAR_PROFILES.md); the port's
 *   overlays show the skin's name and pictures (mod_car_name, mod_car_picture).
 * - Characters (the user's, 2026-10-26): their own cars in the Left / Right order, after every regular car (Spectre,
 *   Sweet Tooth, Minion and Dark Tooth as offered), in the scan's order; their own name, portraits and model, their own
 *   numbers from [character] (stats, size and hit box, the special's recharge and count, radar color: portCarProfile,
 *   decomp request 70 (CAR_PROFILES.md)), the base's special behavior and sounds. Select gives a character no other looks. Computer cars
 *   drive them too (ui/options.c ui_AiSelect: picked, or Random with Options > Mods' setting).
 * - The load (vplus/four.c's vp_SessionLoad): each car, human or computer, whose car number is a mod gets its car.dmd
 *   and car.tms read into high RAM and its load group pointed at them; every other group reads the disc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "runtime/runtime.h"
#include "mods/mods.h"

#define MOD_NAMES 0x80E60000u  /* 64 bytes a name (rollback's node list ends at 0x80E60000: net/rb_state.c) */
#define MOD_TARGETS 0x80E62000u /* 32 bytes a target panel name (portCarProfile's) */
#define MOD_ENDINGS 0x80E63000u /* 32 bytes a character's ending "movie" name: the port's ending page (ui/options.c) */
#define MOD_FILES 0x80E64000u  /* the skins' files during a load */
#define MOD_FILES_END 0x81000000u
enum { MOD_CARS = 128, NAME_BYTES = 64, TARGET_BYTES = 32, FIRST = 0x20, PROFILE_BYTES = 0xA0 };

static int skin_mod[MOD_CARS], skins = -1; /* mod car m: mods_get(skin_mod[m]) (a skin or a character) */
static int is_car(const Mod *m) { return (m->kind == MOD_SKIN || m->kind == MOD_CHARACTER) && m->ok && m->base >= 0; }

static void build(void)
{
    skins = 0;
    if (mods_online()) { /* a server's: its skins in its order, so every machine has the same numbers (rollback hashes
                          * portModCars and the cars' numbers: a machine's other mods must not shift them) */
        const char *uid;
        for (int k = 0; (uid = mods_online_uid(k)) != NULL && skins < MOD_CARS; k++)
            for (int i = 0; i < mods_count(); i++) {
                const Mod *m = mods_get(i);
                if (is_car(m) && !strcmp(m->uid, uid)) { skin_mod[skins++] = i; break; }
            }
        return;
    }
    for (int i = 0; i < mods_count() && skins < MOD_CARS; i++) {
        const Mod *m = mods_get(i);
        if (is_car(m)) skin_mod[skins++] = i;
    }
}
int mod_cars_count(void)
{
    static int built = -1; /* (again after a scan: a download) */
    if (skins < 0 || built != mods_generation()) { build(); built = mods_generation(); }
    return skins;
}
static const Mod *skin_of(int number) /* a car number's skin, else NULL */
{
    int m = number - FIRST;
    return m >= 0 && m < mod_cars_count() ? mods_get(skin_mod[m]) : NULL;
}
int vp_dt_player_offered(void); /* fixes.c: Dark Tooth in car select */
int mod_car_base(int number) /* a car number's base: itself below 0x20 (LOOK_DT2: Dark Tooth), a skin's base, else -1 */
{
    if (number == LOOK_DT2) return 14;
    if (number >= 0 && number < FIRST) return number;
    const Mod *m = skin_of(number);
    return m ? m->base : -1;
}
int mod_car_mod(int number) /* a skin's index in the mods list (mods_get), else -1 */
{
    int m = number - FIRST;
    return m >= 0 && m < mod_cars_count() ? skin_mod[m] : -1;
}
int mod_car_number_of(int mod_index) /* a skin's car number, from its index in the mods list; -1 */
{
    for (int m = 0; m < mod_cars_count(); m++)
        if (skin_mod[m] == mod_index) return FIRST + m;
    return -1;
}
int mod_car_on(int number) /* a skin that is on (offered, loadable) */
{
    if (number == LOOK_DT2) return vp_dt_player_offered();
    const Mod *m = skin_of(number);
    return m && mod_on(m);
}
int mod_car_is_character(int number) /* a character's number (on or off) */
{
    const Mod *m = skin_of(number);
    return m && m->kind == MOD_CHARACTER;
}
/* the characters that are on, in the scan's order (the server's online): how many, and the k-th one's number */
int mod_characters(void)
{
    int n = 0;
    for (int k = FIRST; k < FIRST + mod_cars_count(); k++) n += mod_car_is_character(k) && mod_car_on(k);
    return n;
}
int mod_character(int i)
{
    for (int k = FIRST; k < FIRST + mod_cars_count(); k++)
        if (mod_car_is_character(k) && mod_car_on(k) && i-- == 0) return k;
    return -1;
}
/* Select in car select: the next look of the highlighted car (its own, then each of its skins that is on, in order) */
int mod_next_look(int number)
{
    int base = mod_car_base(number);
    if (base < 0 || mod_car_is_character(number)) return number; /* (a character: itself) */
    if (number == 14 && vp_dt_player_offered()) return LOOK_DT2; /* (Dark Tooth: his second phase next, then his skins) */
    for (int k = number < FIRST ? FIRST : number + 1; k < FIRST + mod_cars_count(); k++)
        if (mod_car_base(k) == base && !mod_car_is_character(k) && mod_car_on(k)) return k;
    return base;
}
int mod_car_has_looks(int car) { return mod_next_look(car) != car; } /* (the car has a skin on) */
const char *mod_car_name(int number) /* a skin's or a character's name, else NULL */
{
    if (number == LOOK_DT2) return "Dark Tooth: Phase 2";
    const Mod *m = skin_of(number);
    return m ? m->name : NULL;
}
/* a skin's picture file (portrait_1.bmp, portrait_2.bmp, portrait_hq.bmp) when it has one, else 0 */
int mod_car_picture(int number, const char *file, char *path, int size)
{
    const Mod *m = skin_of(number);
    if (!m) return 0;
    snprintf(path, (size_t)size, "%s%s", m->dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* portCarProfile[m] (decomp request 70, CAR_PROFILES.md 2): a character's own numbers from its mod.ini (mods.c
 * check_stats), its name on the target panel; a skin's entry empty (its car's numbers, MODDING.md 5). Match state the
 * rollback hashes: written from the mods' files only, so the same on every machine with the server's mods. */
static void write_profiles(void)
{
    static uint32_t prof = 1;
    if (prof == 1) prof = port_sym("portCarProfile");
    if (!prof) return;
    for (int m = 0; m < MOD_CARS; m++) {
        uint32_t e = prof + PROFILE_BYTES * (uint32_t)m;
        for (uint32_t w = 0; w < PROFILE_BYTES; w += 4) mem_w32(&M.cpu, e + w, 0);
        const Mod *mod = m < mod_cars_count() ? mods_get(skin_mod[m]) : NULL;
        if (!mod || mod->kind != MOD_CHARACTER) continue;
        const ModStats *s = &mod->stats;
        uint32_t target = MOD_TARGETS + TARGET_BYTES * (uint32_t)m;
        int len = 0; /* (the HUD's font: upper-case letters, digits and spaces) */
        for (const char *c = mod->name; *c && len < 15; c++) {
            char u = (char)toupper((unsigned char)*c);
            if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || (u == ' ' && len && mem_r8(&M.cpu, target + (uint32_t)len - 1) != ' '))
                mem_w8(&M.cpu, target + (uint32_t)len++, (uint8_t)u);
        }
        while (len && mem_r8(&M.cpu, target + (uint32_t)len - 1) == ' ') len--;
        for (int k = len; k < TARGET_BYTES; k++) mem_w8(&M.cpu, target + (uint32_t)k, 0);
        mem_w8(&M.cpu, e, 1);
        mem_w8(&M.cpu, e + 1, (uint8_t)s->n);
        mem_w8(&M.cpu, e + 2, (uint8_t)s->flags);
        mem_w16(&M.cpu, e + 4, (uint16_t)s->step);
        mem_w16(&M.cpu, e + 6, (uint16_t)s->recharge);
        mem_w16(&M.cpu, e + 8, s->flags & MOD_STAT_SPECIAL ? 24 : 0); /* (the pickup capacity: every car's) */
        mem_w8(&M.cpu, e + 0x0A, (uint8_t)s->max_specials);
        for (int k = 0; k < 3; k++) mem_w8(&M.cpu, e + 0x0B + (uint32_t)k, s->radar[k]);
        mem_w32(&M.cpu, e + 0x10, len ? target : 0);
        for (int i = 0; i < s->n; i++) {
            mem_w8(&M.cpu, e + 0x40 + 4u * (uint32_t)i, s->offset[i]);
            mem_w8(&M.cpu, e + 0x41 + 4u * (uint32_t)i, s->width[i]);
            mem_w16(&M.cpu, e + 0x42 + 4u * (uint32_t)i, (uint16_t)s->value[i]);
        }
    }
}

/* the characters' specials (decomp request 73, SPECIAL_TEMPLATES.md, car_numbers.h TM2PortSpecialProfile, 0x48 bytes each by mod): read only
 * while the mod's portCarProfile entry is valid; match state, the same on every machine */
static void write_specials(void)
{
    static uint32_t sp = 1;
    if (sp == 1) sp = port_sym("portSpecialProfile");
    if (!sp) return;
    for (int m = 0; m < MOD_CARS; m++) {
        uint32_t e = sp + 0x48u * (uint32_t)m;
        for (uint32_t w = 0; w < 0x48; w += 4) mem_w32(&M.cpu, e + w, 0);
        const Mod *mod = m < mod_cars_count() ? mods_get(skin_mod[m]) : NULL;
        if (!mod || mod->kind != MOD_CHARACTER) continue;
        const ModStats *s = &mod->stats;
        mem_w16(&M.cpu, e, (uint16_t)s->special);
        mem_w8(&M.cpu, e + 2, (uint8_t)s->sp_n);
        mem_w8(&M.cpu, e + 3, s->ai_ahead);
        mem_w8(&M.cpu, e + 4, s->ai_behind);
        mem_w8(&M.cpu, e + 5, s->ai_fire); /* (request 87: the fire chance, in what was pad[0]) */
        for (int i = 0; i < s->sp_n; i++) {
            mem_w8(&M.cpu, e + 8 + 4u * (uint32_t)i, s->sp_field[i]);
            mem_w16(&M.cpu, e + 10 + 4u * (uint32_t)i, (uint16_t)s->sp_value[i]);
        }
    }
}

/* every frame (port_config_sync): the table and car select's offer */
void mod_cars_sync(int online)
{
    static uint32_t table = 1, sel;
    if (table == 1) { table = port_sym("portModCars"); sel = port_sym("portCarSel"); }
    if (!table || !sel) return;
    static int written = -1;
    if (written != mods_generation()) { /* (and after a switch in Options > Mods) */
        if (written < 0 && mod_cars_count())
            fprintf(stderr, "mods: %d mod car%s in car select (%d character%s)\n", mod_cars_count(),
                    mod_cars_count() == 1 ? "" : "s", mod_characters(), mod_characters() == 1 ? "" : "s");
        written = mods_generation();
        for (int m = 0; m < mod_cars_count(); m++) {
            const Mod *mod = mods_get(skin_mod[m]);
            uint32_t e = table + 0x20u * (uint32_t)m, name = MOD_NAMES + (uint32_t)(NAME_BYTES * m);
            /* the game's copy (carDisplayName: printed with the game's font into its fixed letter sprites, as car select's
             * menu text): MOD_NAME_MAX letters at most, as a profile's name; a longer one wrote past them (decomp request
             * 105: a 34-letter message over the lives text). The port's own screens show the whole name (drawn by it) */
            int cut = 0;
            for (int k = 0; k < NAME_BYTES; k++) {
                int keep = k < MOD_NAME_MAX && !cut && mod->name[k];
                if (!keep) cut = 1;
                mem_w8(&M.cpu, name + (uint32_t)k, keep ? (uint8_t)mod->name[k] : 0);
            }
            mem_w8(&M.cpu, e, mod_on(mod) ? 1 : 0); /* (off: carIsSelectable refuses its number) */
            mem_w8(&M.cpu, e + 1, (uint8_t)mod->base);
            mem_w32(&M.cpu, e + 4, name);
            /* car select's CAR INFO: the base's picture from the disc (carBioPictureFiles), so the game shows its
             * info screen, and over it the mod's own carinfo.bmp (ui_DisplayImage). None: the game's info went back
             * at once (the user's: CAR INFO only redrew the menu) */
            mem_w32(&M.cpu, e + 0x0C, mod->base < 14 ? mem_r32(&M.cpu, GA(0x80164868u) + 4u * (uint32_t)mod->base) : 0u);
            /* the tournament's ending (screenEnding's portCarEnding): a skin its car's movie (the winner movies' table
             * 801648C4: name, frames, wide); a character a name of the port's, which moviePlayEnding never plays: the
             * port's ending page instead (ui_MoviePlayEnding, MODDING.md 6) */
            uint32_t end = MOD_ENDINGS + 32u * (uint32_t)m;
            char mark[32];
            snprintf(mark, sizeof mark, "\\MODS\\ENDING%d", m);
            for (int k = 0; k < 32; k++) mem_w8(&M.cpu, end + (uint32_t)k, (uint8_t)(k < (int)strlen(mark) ? mark[k] : 0));
            uint32_t movie = mod->base < 14 ? GA(0x801648C4u) + 12u * (uint32_t)mod->base : 0;
            int character = mod->kind == MOD_CHARACTER;
            mem_w32(&M.cpu, e + 0x10, character ? end : movie ? mem_r32(&M.cpu, movie) : 0u);
            mem_w32(&M.cpu, e + 0x14, character ? 1u : movie ? mem_r32(&M.cpu, movie + 4) : 0u);
            mem_w32(&M.cpu, e + 0x18, character ? 0u : movie ? mem_r32(&M.cpu, movie + 8) : 0u);
        }
        for (int m = mod_cars_count(); m < MOD_CARS; m++) /* (the rest empty: the same on every machine) */
            for (uint32_t w = 0; w < 0x20; w += 4) mem_w32(&M.cpu, table + 0x20u * (uint32_t)m + w, 0);
        write_profiles();
        write_specials();
    }
    mem_w32(&M.cpu, sel, online && !mods_online() ? 0u : (uint32_t)mod_cars_count()); /* (online: a server's mods only) */
    /* the carousel's mod children (the empty node until set): the base's car and labels, in all three copies */
    int count = (int)mem_r32(&M.cpu, sel + 4);
    static int shown = -1; /* tests: TM2PC_MODS_DEBUG, the carousel's children */
    if (count != shown && getenv("TM2PC_MODS_DEBUG")) fprintf(stderr, "mods: car select carousel %d children\n", count);
    shown = count;
    uint32_t blank = sel + 0x10;
    for (int copy = 0; copy < 3 && count > FIRST; copy++) {
        uint32_t node = sel + 0x30 + (uint32_t)copy * (16 + 4 * 160), kids = node + 16;
        for (int k = FIRST; k < count; k++) {
            int base = mod_car_base(k);
            if (base < 0 || mem_r32(&M.cpu, kids + 4u * (uint32_t)k) != blank) continue;
            mem_w32(&M.cpu, kids + 4u * (uint32_t)k, mem_r32(&M.cpu, kids + 4u * (uint32_t)base));
        }
    }
}

/* a character's ending name (mod_cars_sync's, moviePlayEnding's argument): its car number, else -1 */
int mod_cars_ending_number(uint32_t name)
{
    if (name < MOD_ENDINGS || name >= MOD_ENDINGS + 32u * MOD_CARS || (name - MOD_ENDINGS) % 32) return -1;
    int number = FIRST + (int)((name - MOD_ENDINGS) / 32);
    return mod_car_is_character(number) ? number : -1;
}
/* a mod car's uid (a profile's [mod_endings] key), else NULL */
const char *mod_car_uid(int number)
{
    const Mod *m = skin_of(number);
    return m ? m->uid : NULL;
}
/* human h's car number: a mod's while the settings' byte is its base (as the load reads it), else the byte */
int mod_cars_human_car(int h)
{
    static uint32_t numbers = 1, more;
    if (numbers == 1) { numbers = port_sym("portSettingsHumanCar"); more = port_sym("sessionPortHumanCars"); }
    const uint32_t S = GA(0x8016475Cu);
    int byte = h < 2 ? (int8_t)mem_r8(&M.cpu, S + 8 + (uint32_t)h) : more && h < 16 ? (int8_t)mem_r8(&M.cpu, more + (uint32_t)(h - 2)) : -1;
    if (!numbers || h < 0 || h >= 16) return byte;
    int number = (int16_t)mem_r16(&M.cpu, numbers + 2u * (uint32_t)h);
    return number >= FIRST && mod_car_base(number) == byte ? number : byte;
}

/* players 3..16's car numbers (the settings keep their bytes in sessionPortHumanCars, written by four.c): a skin's
 * number, or -1 (the byte's car), so a skin of an earlier match is never left over */
/* (four.c: the texture banks) a player among the first `humans` drives a mod car (a skin or a character): its own
 * textures, as large as a modder makes them (Joodeh's Darkside, 178 KB: 4 of its images found no room in a tournament
 * arena's VRAM, "Relocatable texture not loaded"); in a bank of its own they always fit */
int mod_cars_human_on(int humans)
{
    static uint32_t numbers = 1;
    if (numbers == 1) numbers = port_sym("portSettingsHumanCar");
    const uint32_t S = GA(0x8016475Cu);
    static uint32_t more = 1;
    if (more == 1) more = port_sym("sessionPortHumanCars");
    for (int h = 0; numbers && h < humans && h < 16; h++) {
        int number = (int16_t)mem_r16(&M.cpu, numbers + 2u * (uint32_t)h);
        int byte = h < 2 ? (int8_t)mem_r8(&M.cpu, S + 8 + (uint32_t)h) : more ? (int8_t)mem_r8(&M.cpu, more + (uint32_t)(h - 2)) : -1;
        if (number >= FIRST && mod_car_on(number) && byte == mod_car_base(number)) return 1;
    }
    const char *t = getenv("TM2PC_TEST_P1_SKIN"); /* (tests: player 1's skin, set at the load) */
    return t && atoi(t) >= 0 && atoi(t) < mod_cars_count() && humans > 0;
}
void mod_cars_human_number(int h, int number)
{
    static uint32_t numbers = 1;
    if (numbers == 1) numbers = port_sym("portSettingsHumanCar");
    mod_dt2_human(h, number);
    if (!numbers || h < 2 || h >= 16) return;
    mem_w16(&M.cpu, numbers + 2u * (uint32_t)h, (uint16_t)(number >= FIRST && mod_car_on(number) ? number : -1));
}

static uint32_t read_into(const char *dir, const char *name, uint32_t at, uint32_t end)
{
    char path[700];
    snprintf(path, sizeof path, "%s%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t n = 0;
    int ch;
    while (at + n < end && (ch = fgetc(f)) != EOF) mem_w8(&M.cpu, at + n++, (uint8_t)ch);
    int more = fgetc(f) != EOF;
    fclose(f);
    return more ? 0 : n; /* (too big for the room left: the disc's instead) */
}

/* before sessionLoadArenaAndCars: every group's source (a car on a mod, human or computer: its files; else the disc).
 * The load groups (decomp CAR_GROUPS.md 7.7): human h is group h + 1, opponent i group humans + i + 1. A mod's files
 * are read once and handed to each of its cars (portCarLoadGroup copies them). humans -1: none (online without the
 * server's mods). */
static int group_number[33]; /* this load: each load group's mod number (0: the disc's car) */
int mod_cars_group_number(int g) { return g >= 0 && g < 33 && group_number[g] ? group_number[g] : -1; }
/* a mod car number's team paint (its body palette entries, a bit each; MODDING.md 4.2): NULL keeps its own colors */
const unsigned char *mod_car_team_paint(int number)
{
    const Mod *m = skin_of(number);
    return m && m->stats.has_team ? m->stats.team_paint : NULL;
}
void mod_cars_load(int humans)
{
    static uint32_t src = 1, numbers;
    if (src == 1) { src = port_sym("portCarSource"); numbers = port_sym("portSettingsHumanCar"); }
    memset(group_number, 0, sizeof group_number);
    if (!src) return;
    for (uint32_t g = 0; g <= 32; g++)
        for (uint32_t w = 0; w < 4; w++) mem_w32(&M.cpu, src + 16 * g + 4 * w, 0);
    if (!numbers || !mod_cars_count() || humans < 0) return;
    const uint32_t S = GA(0x8016475Cu);
    const char *t = getenv("TM2PC_TEST_P1_SKIN"); /* tests: player 1 on skin m, as car select would set it */
    if (t && atoi(t) >= 0 && atoi(t) < mod_cars_count() && humans > 0) {
        mem_w16(&M.cpu, numbers, (uint16_t)(FIRST + atoi(t)));
        mem_w8(&M.cpu, S + 8, (uint8_t)mod_car_base(FIRST + atoi(t))); /* (the settings byte: its base) */
    }
    int opponents = mem_r8(&M.cpu, S + 5);
    struct { int m; uint32_t tms, tms_n, dmd, dmd_n; } read[16];
    int nread = 0;
    uint32_t at = MOD_FILES;
    for (int car = 0; car < humans + opponents && car < 32; car++) {
        int number;
        if (car < humans) {
            if (car >= 16) continue;
            number = (int16_t)mem_r16(&M.cpu, numbers + 2u * (uint32_t)car);
            static uint32_t more = 1; /* (players 3..: their bytes in sessionPortHumanCars) */
            if (more == 1) more = port_sym("sessionPortHumanCars");
            int byte = car < 2 ? (int8_t)mem_r8(&M.cpu, S + 8 + (uint32_t)car)
                               : more ? (int8_t)mem_r8(&M.cpu, more + (uint32_t)(car - 2)) : -1;
            if (byte != mod_car_base(number)) continue; /* (a number left from another match: the car the settings hold) */
        } else /* settingsGetOpponentCar: the mod's number while its byte is the base, else the byte */
            number = (int)guest_call(GA(0x8013783Cu), S, (uint32_t)(car - humans), 0, 0);
        int m = number - FIRST;
        if (m < 0 || m >= mod_cars_count() || !mod_car_on(number)) continue;
        const Mod *mod = mods_get(skin_mod[m]);
        int r = 0;
        while (r < nread && read[r].m != m) r++;
        if (r == nread) {
            if (nread == 16) continue;
            uint32_t tms = at, tms_n = read_into(mod->dir, "car.tms", tms, MOD_FILES_END);
            uint32_t dmd = (tms + tms_n + 3) & ~3u, dmd_n = tms_n ? read_into(mod->dir, "car.dmd", dmd, MOD_FILES_END) : 0;
            if (!tms_n || !dmd_n) { fprintf(stderr, "mods: %s: its files did not fit (the car's own instead)\n", mod->name); continue; }
            read[nread].m = m, read[nread].tms = tms, read[nread].tms_n = tms_n, read[nread].dmd = dmd, read[nread].dmd_n = dmd_n;
            nread++;
            at = (dmd + dmd_n + 3) & ~3u;
        }
        uint32_t e = src + 16u * (uint32_t)(car + 1);
        group_number[car + 1] = number;
        mem_w32(&M.cpu, e, read[r].tms);
        mem_w32(&M.cpu, e + 4, read[r].tms_n);
        mem_w32(&M.cpu, e + 8, read[r].dmd);
        mem_w32(&M.cpu, e + 12, read[r].dmd_n);
        if (car < humans) fprintf(stderr, "mods: player %d drives %s (%s's car)\n", car + 1, mod->name, mod->id);
        else fprintf(stderr, "mods: computer car %d drives %s (%s's car)\n", car - humans + 1, mod->name, mod->id);
        /* its special's template, when that is a car's whose projectile is in its car file (Sweet Tooth, Mr. Grimm,
         * Spectre, Roadkill, Axel, Shadow: request 35's portCarSpecialLoad, a bit by type >> 4): that file loaded too */
        int tpl = mod->kind == MOD_CHARACTER ? mod->stats.special : 0;
        static uint32_t spec_load = 1;
        if (spec_load == 1) spec_load = port_sym("portCarSpecialLoad");
        if (spec_load && tpl >= 0x10 && tpl < 0x100 && (0x2B22u >> (tpl >> 4) & 1))
            mem_w32(&M.cpu, spec_load, mem_r32(&M.cpu, spec_load) | 1u << (tpl >> 4));
    }
}

/* Car select's turning car for a mod car or Dark Tooth (decomp request 71, CAR_PROFILES.md 5-7): at each frame boundary,
 * each player's shown car that has no model of its own on the carousel gets one, through portCarSelLoad into load slot
 * p (its own VRAM bank): a mod's carselect.dmd / .tms, else its car.dmd / .tms; Dark Tooth's BB.DMD / BB.TMS from the
 * disc. The DMD goes straight into the slot's copy, the TMS through MOD_FILES (no load runs in car select). A file
 * that fails keeps the base's car (mod_cars_sync's copy). */
int carsel_shown(int p); /* ui/options.c: player p's car number in car select, -1 none */
enum { CARSEL_LOADS = 4, CARSEL_SLOT = 0x203C0, CARSEL_DMD = 0x3C0, CARSEL_DMD_BYTES = 0x20000, DARK_TOOTH = 14 };
static uint32_t read_disc(const char *path, uint32_t at, uint32_t end)
{
    uint32_t lba, size;
    uint8_t sec[2048];
    if (!cd_find_file(path, &lba, &size) || size > end - at) return 0;
    for (uint32_t n = 0; n < size; n += 2048, lba++) {
        if (!cd_read_sector(lba, sec)) return 0;
        for (uint32_t k = 0; k < 2048 && n + k < size; k++) mem_w8(&M.cpu, at + n + k, sec[k]);
    }
    return size;
}
void mod_cars_carsel_frame(void)
{
    static uint32_t loads = 1, load_fn, sel;
    if (loads == 1) { loads = port_sym("portCarSelLoads"); load_fn = port_sym("portCarSelLoad"); sel = port_sym("portCarSel"); }
    /* Dark Tooth's head (request 109's transform node over root 249, the second root under the slot's group) a little
     * back from the truck's front (+y): the user's look, 2026-11-10 */
    for (int k = 0; loads && k < CARSEL_LOADS; k++) {
        uint32_t slot = loads + CARSEL_SLOT * (uint32_t)k, group = slot + 0x390;
        if (mem_r32(&M.cpu, slot + 0x3B8) != group || mem_r32(&M.cpu, slot + 0x0C) != 2) continue;
        uint32_t node = mem_r32(&M.cpu, group + 28), head = node ? mem_r32(&M.cpu, node + 40) : 0;
        if (mem_r8(&M.cpu, node) == 5 && head && mem_r16(&M.cpu, head + 2) == 249)
            mem_w32(&M.cpu, node + 28, (uint32_t)(-40));
    }
    static int failed[CARSEL_LOADS] = {-1, -1, -1, -1}; /* (a number whose files failed: not again this car select) */
    static int slot_dt2[CARSEL_LOADS]; /* (a slot's Dark Tooth: 1 his second phase, the head alone; request 118, DARK_TOOTH_PLAYER.md) */
    static int slot_team[CARSEL_LOADS] = {-1, -1, -1, -1}; /* (a slot's Dark Tooth: the team color he was loaded in, -1 his own) */
    int vp_carsel_team(int p); /* ui/options.c: player p's team color in car select, -1 no teams */
    int vp_team_dark_tooth_file(uint32_t buf, int team); /* vplus/fixes.c: his pink in the team's color */
    if (!loads || !load_fn || !sel || (int)mem_r32(&M.cpu, sel + 4) <= DARK_TOOTH) { /* (not the extended carousel) */
        for (int k = 0; k < CARSEL_LOADS; k++) failed[k] = -1, slot_dt2[k] = 0, slot_team[k] = -1;
        return;
    }
    for (int k = 0; k < CARSEL_LOADS; k++) {
        int number = carsel_shown(k), dt2 = number == LOOK_DT2;
        if (dt2) number = DARK_TOOTH;
        int team = number == DARK_TOOTH ? vp_carsel_team(k) : -1; /* (teams: his truck in this player's team color) */
        if (number < DARK_TOOTH || (number > DARK_TOOTH && !mod_car_on(number)) || number >= (int)mem_r32(&M.cpu, sel + 4)) continue;
        uint32_t slot = loads + CARSEL_SLOT * (uint32_t)k;
        int shown = 0;
        int other = 0; /* (Dark Tooth on the carousel in his other look: one child, the first player's look kept) */
        for (int j = 0; j < CARSEL_LOADS; j++)
            if ((int)mem_r32(&M.cpu, loads + CARSEL_SLOT * (uint32_t)j) == number) {
                if (number != DARK_TOOTH || (slot_dt2[j] == dt2 && slot_team[j] == team)) shown = 1;
                else if (j != k) other = 1;
            }
        if (shown || other || failed[k] == number + (dt2 << 8) + ((team + 1) << 12)) continue; /* (on the carousel already: another
                                                                                                * player's slot too; his team color:
                                                                                                * the first player's) */
        uint32_t dmd = slot + CARSEL_DMD, dmd_n, tms = MOD_FILES, tms_n;
        if (number == DARK_TOOTH) {
            dmd_n = read_disc("\\DMD\\BB.DMD;1", dmd, dmd + CARSEL_DMD_BYTES);
            tms_n = read_disc("\\TMS\\BB.TMS;1", tms, MOD_FILES_END);
            if (tms_n && team >= 0) vp_team_dark_tooth_file(tms, team);
        } else {
            const Mod *mod = mods_get(skin_mod[number - FIRST]);
            int own = mod_car_picture(number, "carselect.dmd", (char[700]){0}, 700);
            dmd_n = read_into(mod->dir, own ? "carselect.dmd" : "car.dmd", dmd, dmd + CARSEL_DMD_BYTES);
            tms_n = read_into(mod->dir, own ? "carselect.tms" : "car.tms", tms, MOD_FILES_END);
        }
        uint32_t index = (uint32_t)number | (dt2 ? 0x10000u : 0u); /* (TM2_CARSEL_DT_PHASE2: the head alone) */
        int r = dmd_n && tms_n ? (int)guest_call7(load_fn, (uint32_t)k, index, dmd, dmd_n, tms, tms_n, 0) : 0;
        if (r != 1) failed[k] = number + (dt2 << 8) + ((team + 1) << 12);
        else slot_dt2[k] = dt2, slot_team[k] = team;
        if (getenv("TM2PC_MODS_DEBUG") || r != 1)
            fprintf(stderr, "mods: car select slot %d shows car %d's own model (%u + %u bytes): %d\n", k, number, dmd_n, tms_n, r);
    }
}

/* ---- Dark Tooth's second phase (decomp request 118, DARK_TOOTH_PLAYER.md 12) ----
 * A look number of the port's (LOOK_DT2): the game is given Dark Tooth (14), and the human's bit here goes to
 * portDtPlayer.phase2 at the match's load (four.c vp_SessionLoad), only while that human's car is Dark Tooth. Set
 * wherever a human's car number is handed to the game (settingsSetHumanCar's wrap, mod_cars_human_number). */
static unsigned dt2_mask;
int mod_is_look(int number) { return number >= FIRST || number == LOOK_DT2; }
void mod_dt2_human(int h, int number)
{
    if (h < 0 || h >= 16) return;
    if (number == LOOK_DT2) dt2_mask |= 1u << h; /* (not this machine's unlock: online, every machine the same) */
    else dt2_mask &= ~(1u << h);
}
unsigned mod_dt2_mask(void) { return dt2_mask; }
