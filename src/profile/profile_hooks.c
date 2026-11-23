/* Game hooks for profiles (src/overrides.json). */
#include "runtime/runtime.h"
#include "profile.h"

/* 80136D98 plays the winner movies after a won single-player tournament
 * (screenScrollText 80136BB0 calls it in mode 1 only; co-op shows
 * COWIN.TIM instead): for each human player, the ending of their car (table 801648C4: name, last frame, flag,
 * indexed by car-select index) and then YOUWON.STR. It reads the settings
 * struct 8016475C: +0 game mode (1 tournament), +4 human players, +8 + i
 * player i's car-select index, +36 difficulty (0 Easy, 1 Medium, 2 Hard). */
#define SETTINGS GA(0x8016475Cu)
#define MODE_TOURNAMENT 1

void F_80136D98_orig(CPU *c);
void profile_WinnerMovies(CPU *c)
{
    if (mem_r32(c, SETTINGS) == MODE_TOURNAMENT) {
        int humans = mem_r8(c, SETTINGS + 4), difficulty = mem_r8(c, SETTINGS + 36);
        int mod_cars_human_car(int h), mod_car_is_character(int number); /* vplus/mod_cars.c */
        const char *mod_car_uid(int number), *mod_car_name(int number);
        for (int i = 0; i < humans && i < 2; i++) {
            int number = mod_cars_human_car(i); /* (a mod character: its own ending, never its base car's) */
            if (number >= 0x20 && mod_car_is_character(number)) profile_mod_tournament_won(mod_car_uid(number), mod_car_name(number), difficulty);
            else profile_tournament_won((int8_t)mem_r8(c, SETTINGS + 8 + (uint32_t)i), difficulty);
        }
    }
    F_80136D98_orig(c);
}

/* Unlocked characters (the decomp's docs/UNLOCKS.md): car select offers Sweet
 * Tooth (index 12) when D_80180D04 is set and Minion (13) when D_80180D05 is;
 * their codes at car select set them, and they last until power-off. A
 * profile that has earned them keeps them set (only ever set while playing:
 * the codes still work, and the tournament opponent picker's save, force and
 * restore of D_80180D04 is unaffected). When another profile signs in (on
 * the profile screen, between games) they become that profile's. Called at
 * every frame start. */
#define SWEET_TOOTH_UNLOCKED GA(0x80180D04u)
#define MINION_UNLOCKED GA(0x80180D05u)

static int seen = -1, seen_guests = -1;
/* an online match over: the flags this PC's players' own again (a joiner took the host's whole memory, its players'
 * unlocks with it: the user's online playtest 2026-11-01, a new profile with Sweet Tooth and Minion) */
void profile_unlocks_mine(void) { seen = -1; }

void profile_apply_unlocks(CPU *c)
{
    /* online, the flags are part of the match's state, the same on every machine: a joiner whose players have Sweet
     * Tooth or Minion and a host whose players don't made them differ every step, and the host's state was sent again
     * every 8 steps all match long (the players' v1.0.11 logs: "1 words from 80184CFC", the relocated D_80180D04).
     * Car select is before the match and still offers this PC's own; after it, profile_unlocks_mine sets them back. */
    int netplay_in_match(void); /* net/game_tm2.c */
    if (netplay_in_match()) return;
    /* another profile signed in: the flags are its own (a code entered
     * before is forgotten with the profile that entered it) */
    if (seen != profile_generation() || seen_guests != profile_guests_generation()) { /* (or who plays with them: the
                                                                                          * unlocks are everyone's) */
        seen = profile_generation();
        seen_guests = profile_guests_generation();
        mem_w8(c, SWEET_TOOTH_UNLOCKED, (uint8_t)profile_sweet_tooth_unlocked());
        mem_w8(c, MINION_UNLOCKED, (uint8_t)profile_minion_unlocked());
        return;
    }
    if (profile_sweet_tooth_unlocked() && !mem_r8(c, SWEET_TOOTH_UNLOCKED))
        mem_w8(c, SWEET_TOOTH_UNLOCKED, 1);
    if (profile_minion_unlocked() && !mem_r8(c, MINION_UNLOCKED))
        mem_w8(c, MINION_UNLOCKED, 1);
}
