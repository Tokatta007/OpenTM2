/* Mods (docs/MODDING.md): the folders under mods/, each with its mod.ini, checked when the game starts. */
#ifndef TM2PC_MODS_H
#define TM2PC_MODS_H

enum { MOD_CHARACTER, MOD_SKIN, MOD_ARENA, MOD_TOURNAMENT, MOD_KINDS };
enum { MODS_MAX = 512, MODS_FORMAT = 1 }; /* (MODS_FORMAT: the newest mod.ini format this game reads) */
enum { MOD_NAME_MAX = 16 }; /* a mod's name: letters at most (as a profile's, PROFILE_NAME_MAX: the game's fixed letter sprites) */

/* A character's own numbers ([character] in its mod.ini, MODDING.md 4.2), as the decomp's per-car profile takes them
 * (car_numbers.h TM2PortCarProfile, request 70): stats-block fields over the base car's, and the special's numbers */
enum { MOD_STATS = 24, MOD_STAT_STEP = 1, MOD_STAT_SPECIAL = 2, MOD_STAT_RADAR = 4 };
typedef struct {
    int n;                                  /* stat entries */
    unsigned char offset[MOD_STATS], width[MOD_STATS];
    short value[MOD_STATS];
    int flags;                              /* MOD_STAT_*: step, recharge / max_specials, radar replace the base's */
    short step, recharge;
    signed char max_specials;
    unsigned char radar[3];
    short special;                          /* the special's template (decomp request 73): 0 the base's own, a car type
                                             * (0x10..0xE0) or a pickup (0x101..0x164) */
    int sp_n;                               /* its numbers: field ids (TM2_SPN_*) and values */
    unsigned char sp_field[16];
    short sp_value[16];
    unsigned char ai_ahead, ai_behind;      /* computer drivers' chance to fire it (percent, 0: the template's own) */
    unsigned char ai_fire;                  /* [ai] fire_chance: how often its computer driver fires (1..100, 0: its
                                             * special template's, decomp request 87) */
    int has_team;                           /* team_paint set (a skin's too): else its own colors in team games */
    unsigned char team_paint[32];           /* car.tms image 0's palette entries that take the team's color, a bit each */
} ModStats;

typedef struct {
    char dir[600];       /* its folder, with a slash at the end */
    int kind;             /* MOD_* (its kind's folder) */
    char id[48], uid[40], name[64], version[16], author[48], requires[200];
    int format;           /* game_version */
    int base;             /* a character's or a skin's car (car-select number 0..14), else -1 */
    ModStats stats;       /* a character's own numbers (a skin: none, its car's; both: team_paint) */
    int ok;               /* passed every check */
    unsigned long long fingerprint; /* its allowed files' (mod_files.c): the same mod on every machine has the same */
    long bytes;           /* those files' size */
    char why[120];        /* else the first problem, for the menus ("missing car.dmd") */
} Mod;

extern const char *const mod_kind_folders[MOD_KINDS]; /* "characters", "skins", "arenas", "tournaments" */

int mods_scan(const char *game_folder); /* reads <game folder>/mods: the count found */
int mods_rescan(void);              /* again (a download added one) */
const char *mods_game_folder(void);
int mods_count(void);
const Mod *mods_get(int i);
/* One mod folder, checked alone (tm2pc --check-mod <folder>): 1 if it passes; every problem printed. */
int mods_check_one(const char *folder);
/* tm2pc --new-mod <kind> <id>: mods/<kind>s/<id>/mod.ini from the template, with a fresh uid; 1 if made */
int mods_new(const char *kind, const char *id);

/* Options > Mods: each mod's switch, kept in tm2pc.ini's [mods] by its uid (a new mod is on). A mod is used when it
 * has no problem and is on. mods_generation changes with every switch (the users rebuild their lists). */
int mod_on(const Mod *m);
void mod_set_on(int i, int on); /* (saves [mods]) */
int mods_generation(void);
/* Options > Mods: Random computer drivers include the characters that are on (Challenge Match, Custom Tournament and
 * Survival; the game's Tournament never), [mods] random_characters */
int mods_cpu_characters(void);
void mods_set_cpu_characters(int on);
/* Online: a server's mods are on and every other mod is off while it is set (uids: 32 hex digits each, count of them);
 * 0 clears it (back to Options > Mods). */
void mods_online_set(const char (*uids)[40], int count);
int mods_online(void); /* the server's set is in force */
const char *mods_online_uid(int i); /* its i-th mod's uid, in the server's order; NULL past the end */
int mods_find(const char *uid, unsigned long long fingerprint); /* the mod with that uid and those files: its index, -1 */
/* A character's sound (mod_wav.c): its samples mixed to mono, at their own rate */
typedef struct { short *pcm; int frames, rate; } ModWav;
enum { MOD_ENGINE_SECONDS = 4, MOD_SPECIAL_SECONDS = 6 }; /* (both fit one bank of the port's: vplus/mod_sounds.c) */
int mod_wav_read(const char *path, ModWav *w, int max_seconds, char *why, int why_size); /* 1 if read */
void mod_wav_free(ModWav *w);
int mod_adpcm_size(int frames); /* the SPU ADPCM's bytes for that many samples */
void mod_adpcm_encode(const short *pcm, int frames, int loop, unsigned char *out);
/* A character's ending video (mod_video.c): checked (1 if it may be played; else why), played (skip: 1 to stop) */
int mod_video_check(const char *path, char *why, int why_size);
int mod_video_play(const char *path, int (*skip)(void));

#include <stdio.h>
void mods_ini_key(const char *key, const char *val);
void mods_ini_write(FILE *f);

/* Dark Tooth's second phase as a look of his (decomp request 118): Select on Dark Tooth (14) cycles his own look, this,
 * then his skins. A port-side car number only (below the mods' 0x20 + m): the game is given 14 and the player's bit in
 * portDtPlayer.phase2 (vplus/mod_cars.c mod_dt2_*); online its lobby skin index is LOBBY_SKIN_DT2. */
#define LOOK_DT2 0x1F
#define LOBBY_SKIN_DT2 (-2)
int mod_is_look(int number);               /* a look Select gives a car (a skin, or LOOK_DT2) */
void mod_dt2_human(int h, int number);     /* human h's car number as handed to the game: LOOK_DT2 sets h's bit */
unsigned mod_dt2_mask(void);               /* the humans playing Dark Tooth's second phase (bit h) */

#endif
