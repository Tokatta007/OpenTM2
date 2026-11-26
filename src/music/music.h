/* Custom music: replace TM2's disc music track by track (music.c). */
#pragma once
#include <stdio.h>
#include <stddef.h>

/* The folder for file names and "random" (default: music next to the exe). */
void music_set_folder(const char *folder);

/* The [music] section of the settings file (settings.c). */
void music_ini_key(const char *key, const char *val);
void music_ini_write(FILE *f);

/* For cd_audio.c: the slot replacing disc track t (0-based), or -1 for the
 * disc music. music_load decodes that slot's file (a new random pick each
 * time) as 44.1 kHz stereo 16-bit; NULL if it cannot be loaded. */
int music_slot(int track);

/* Final showdown music, the last row of the page: a battle's final moments
 * switch to the boss fight's track (vplus/fixes.c). Its setting: off (the
 * default), default (the Dark Tooth track as set), or its own random, order
 * or file. music_showdown_play: the showdown is on; the boss track then plays
 * the showdown's own setting (until a new round: 0). */
int music_showdown(void);
void music_showdown_play(int on);
int music_page_has_off(int i);
const char *music_page_dir(int i); /* the row's arena folder (in the music folder), NULL none */
int music_include_original(void);  /* an arena's own song among its random / order songs */
void music_set_include_original(int on);
void music_make_folders(void);     /* each arena's folder, created empty */ /* the row has an Off setting (the showdown) */
/* The Versus Tournament's podium (ui/options.c): its own song's slot, or -1 (default: the menus' music). */
int music_podium_slot(void);
short *music_load(int slot, size_t *frames);
/* A mod arena (mods/mod_arena.c): its round music. music_mod_arena_loaded at its load (a random arena's track picked
 * for it, unless it has its own file); music_remap (cd_audio.c) gives the disc track and sector to play instead of
 * the one asked for. */
void music_mod_arena_loaded(void);
#include <stdint.h>
int music_remap(int track, uint32_t *lba);

/* music_load for next (step 1) or previous (step -1) song of a slot set to
 * random or order (music_slot_changes). */
int music_slot_changes(int slot);
short *music_load_next(int slot, int step, size_t *frames);
/* When a song of a track set to random or order ends: 0 it starts over, 1 the next song plays (the user's 2026-10-29:
 * long matches). [music] at_end = repeat / next. */
int music_next_at_end(void);
void music_set_next_at_end(int on);

/* For the Custom Music page: the tracks it lists (name, setting, set), and
 * the music files in the folder, sorted by name. */
int music_page_count(void);
const char *music_page_name(int i);
const char *music_page_setting(int i);
void music_page_set(int i, const char *setting);
int music_file_count(void);
int music_file_name(int i, char *out, size_t n);
const char *music_folder(void);
