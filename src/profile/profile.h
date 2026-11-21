/* Player profiles: a name (the online screen name later), a picture, player
 * 1's controls and progress kept across sessions (profile.c). TM2 itself
 * keeps nothing: progress is only in passwords, and its secret characters
 * last until power-off. */
#pragma once
#include <stdint.h>
#include <stdio.h>

#define PROFILE_CHARACTERS 14   /* car-select indices 0..13 (Hammerhead .. Minion) */
#define PROFILE_MAX 32          /* profiles listed */
#define PROFILE_NAME_MAX 16     /* characters in a name */

/* Where profiles are kept (default: profiles next to the executable). */
void profile_set_folder(const char *folder);

/* The [profile] section of the settings file (settings.c): the profile
 * signed in last. */
void profile_ini_key(const char *key, const char *val);
void profile_ini_write(FILE *f);

/* Loads the selected profile (after the settings): its controls (player 1's
 * layout, buttons, keys and song keys) and progress. A profile file without
 * controls takes the current ones (those of an older tm2pc.ini). */
void profile_load(void);
/* Writes the signed-in profile's file (after its controls change). */
void profile_save(void);

const char *profile_name(void);
const char *profile_character_name(int car);
int profile_ending_earned(int car);
int profile_endings_count(void);

/* Unlocks earned by playing (the user's rules, docs/PROFILES.md). */
int profile_sweet_tooth_unlocked(void);
int profile_minion_unlocked(void);
int profile_dark_tooth_unlocked(void); /* every driver's ending (14), Sweet Tooth and Minion included */
int profile_guests_generation(void); /* changes when the players signed in together (or their endings) change */
int profile_secret_levels_unlocked(void);   /* 0..3 */

/* A human player won the tournament with car-select index car on
 * difficulty 0 Easy, 1 Medium, 2 Hard: records the ending if it counts. */
void profile_tournament_won(int car, int difficulty);
void profile_p2_tournament_won(int car, int difficulty); /* the same in player 2's profile (the co-op duel's winner) */
void profile_extra_tournament_won(int p, int car, int difficulty); /* and player 3 or 4's (p 2-3) */
const char *profile_extra_name(int p);        /* player 3 or 4's profile (p 2-3), "" a guest */
/* The same with a mod character (MODDING.md 6): its own ending, by its mod's uid (who: its name, for the log) */
void profile_mod_tournament_won(const char *uid, const char *who, int difficulty);
void profile_p2_mod_tournament_won(const char *uid, const char *who, int difficulty);
void profile_extra_mod_tournament_won(int p, const char *uid, const char *who, int difficulty);
int profile_mod_ending_earned(const char *uid);

/* ---- the profile screen (ui/options.c) ---- */

/* The profiles in the folder, by name (read again by profile_rescan). */
void profile_rescan(void);
int profile_count(void);
const char *profile_list_name(int i);
int profile_list_picture(int i);
int profile_exists(const char *name);       /* a profile of that name (any case) */
int profile_name_ok(const char *name);      /* a usable new name: not empty, safe in a file name */

/* Signs in: tm2pc.ini remembers it; loads its controls and progress. */
int profile_use(const char *name);
/* A new profile with the default controls and the given picture, signed in. */
int profile_create(const char *name, int picture);
/* Renames the signed-in profile (its file too). */
int profile_rename(const char *name);
int profile_rename_other(const char *from, const char *to); /* a profile not signed in, too */
int profile_set_picture_of(const char *who, int pic);
/* Deletes a profile's file; deleting the signed-in one leaves none signed in. */
int profile_delete(const char *name);
int profile_signed_in(void);
int profile_online_identity(uint8_t id[8], uint8_t secret[16]); /* the profile online (net/friends.h): made the first time; 0: a guest */

/* Portraits: three of each driver, picture = driver * 3 + n: the two
 * car-select portraits (ui/portraits.bmp) and the art of the driver's info
 * screen (ui/portraits_hq.bmp). The first is free while the driver can be
 * played; the others are earned (for now: with the driver's ending; how is
 * to be decided). Sweet Tooth and Minion stay secret (no name, no picture)
 * until unlocked. */
#define PROFILE_PORTRAITS 3
#define PROFILE_PICTURES (PROFILE_CHARACTERS * PROFILE_PORTRAITS)
int profile_picture(void);
void profile_set_picture(int picture);
int profile_picture_unlocked(int picture);  /* for the signed-in profile */
int profile_character_known(int car);       /* not a secret driver still locked */
const char *profile_picture_how(int picture); /* how a locked one is earned */

/* Portraits from the movies (ui/movie_portraits.txt, made by
 * tools/art/movie_portraits.py): pictures PROFILE_PICTURES + i. Each is from
 * the intro (free) or a driver's ending (earned with it; Sweet Tooth's and
 * Minion's stay secret until the driver is unlocked). */
#define PROFILE_WHO_CALYPSO (-1)
#define PROFILE_WHO_OTHER (-2)
void profile_load_movie_portraits(const char *path);
int profile_picture_total(void);            /* PROFILE_PICTURES + the movie portraits */
int profile_movie_count(void);
int profile_movie_who(int i);               /* the person: a driver, PROFILE_WHO_CALYPSO or _OTHER */
int profile_movie_owner(int i);
int profile_movie_extra(int i);             /* not a still: a driver's extra portrait (Portrait 4..) */             /* the driver whose ending it is from, -1 the intro */
int profile_picture_secret(int picture);    /* a secret driver's, still locked: shown as ??? */
int profile_picture_hidden(int picture);    /* not shown while locked (an ending's still, or secret) */

/* Tournament saves: bookmarks of a run, apart from the progress, in the
 * profile file ([save1] .. [save5], each with a check as the progress) of
 * player 1. The level (a save starts at its beginning: its bosses, Minion
 * and Dark Tooth, are part of it), how many players and their drivers, and
 * the difficulty; Sweet Tooth and Minion can be saved too. A save loads
 * with as many players as it was made with. */
#define PROFILE_SAVES 5
#define PROFILE_SAVE_PLAYERS 4
typedef struct {
    int used;
    int level;       /* 1..8 */
    int car;         /* player 1's car-select index 0..13 (= cars[0]) */
    int players;     /* 1..4 */
    int cars[PROFILE_SAVE_PLAYERS];
    int difficulty;  /* 0 Easy, 1 Medium, 2 Hard */
    char when[24];   /* "2026-10-05 21:40" */
    /* who played it (saves from 2026-11-10 on; older ones: ident 0, anyone loads them): each seat's profile, "" a guest
     * (every guest one profile); online, their profiles' online ids (all 0 a guest) and the lobby's names (shown) */
    int ident;
    int online;      /* made hosting a server: loads only there, with the same players */
    char who[PROFILE_SAVE_PLAYERS][32];
    unsigned char ids[PROFILE_SAVE_PLAYERS][8];
} ProfileSave;
const ProfileSave *profile_tournament_save(int slot);
void profile_set_tournament_save(int slot, int level, int players, const int *cars, int difficulty);
/* the same, with who played it (who: each seat's profile name, "" a guest; ids: online, else NULL) */
void profile_set_tournament_save_who(int slot, int level, int players, const int *cars, int difficulty, int online,
                                     const char (*who)[32], const unsigned char (*ids)[8]);
void profile_clear_tournament_save(int slot);
void profile_preview_save(int slot, int level, int players, const int *cars, int difficulty); /* tests */

/* Player 2 (split-screen sign-in): a profile's name, portrait and controls
 * for player 2 (layout and buttons) for the session; "" none. */
int profile_p2_use(const char *name);
void profile_p2_clear(void);
void profile_p2_save(void);                 /* player 2's controls into their profile */
const char *profile_p2_name(void);
int profile_hud_save(int p);                /* a player's HUD into their profile ([hud]); 0: none */
const char *profile_hud_owner(int p);       /* the profile keeping player p's HUD, "" none, the defaults */
int profile_p2_picture(void);
/* A new profile file (default controls, the picture) without signing in. */
int profile_make(const char *name, int picture);

/* Changes whenever another profile is signed in (or none): the game's
 * unlock flags follow the profile then (profile_hooks.c). */
int profile_generation(void);
