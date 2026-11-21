/* The settings file, tm2pc.ini (settings.c). */
#pragma once
#include <stdio.h>

/* Reads the file into the modules' settings. If it does not exist and
 * create is set, writes one with the current settings. */
void settings_load(const char *path, int create);

/* Rewrites one section of the file read by settings_load (e.g. "video")
 * with the current settings; the rest of the file is kept as it is. */
void settings_save_section(const char *name);

/* The disc image: settings_set_disc before loading (from the command line),
 * settings_disc after (from the file, "" if none), settings_remember_disc
 * stores the command line's disc in a file that has none. */
void settings_set_disc(const char *path);
const char *settings_disc(void);
void settings_remember_disc(const char *path);
/* The first-time setup's: the PC version ("" if none), and both stored (the file's [game] rewritten). */
const char *settings_pc_version(void);
void settings_set_game_discs(const char *ps1, const char *pc);

/* [video] (used by the PSY-Z renderer, gpu_psyz.c) */
typedef struct {
    int resolution;  /* internal resolution multiplier 1..8; 0: auto (fits the window) */
    int dithering;   /* 0 auto (as the PS1 at 1x, off when upscaled), 1 on, 2 off */
    int textures;    /* 0 the sharpest (the PC version's where they are the same pictures), 1 the PlayStation's only */
    int menu_sides;  /* menus on screens wider than 4:3: 0 extension, 1 blur, 2 black */
    /* the battle HUD (Customize HUD; 0 = as the game starts a battle) */
    int hud_no_radar;     /* the radar (the map) hidden */
    int hud_full_weapons; /* TM2 style: 0 the chosen weapon's icon (the game's start), 1 the full list */
    int hud_no_target;    /* the targeted car's name and health hidden */
    int hud_no_messages;  /* the top messages ("Enemies: N") hidden */
    int hud_msg_classic;  /* the message text where the game has it in its bar (else the bar a unit up: centered on it) */
    int hud_no_turbo;     /* the turbo count (or a style's turbo bar) hidden (adaptive layout) */
    int hud_no_speed;     /* the speedometer hidden (adaptive layout) */
    int hud_no_lives;     /* the "Lives: N" text hidden (adaptive layout) */
    int hud_no_energy;    /* the energy bar hidden (every style) */
    int hud_turbo_bar;    /* TM2 style: the turbo as a thin bar by the health bar, not the TURBO: count */
    int hud_style;        /* 0 Twisted Metal 2, 1 TM:Black, 2 TM2012 (vplus/hud_styles.c) */
    int hud_buttons;      /* the game's radar and weapon-list buttons work in a battle (0: "HUD buttons disabled", the default) */
    int hud_mirror;       /* the rear-view mirror on at a battle's start (one view; split screen: vplus/mirrors.c) */
    int hud_low_top;      /* the radar and health where the game has them (else raised to the top edge) */
    int hud_layout;       /* 0 adaptive (laid out for each view: vplus/hud_layout.c), 1 classic (the game's) */
    int hud_size;         /* percent of the normal size, 50..150 (0: 100) */
    int hud_markers;      /* the triangles over the other cars shown (vplus/hud_styles.c, hudstyle_marks; off by default) */
    int hud_no_marker_health; /* their thin health bars hidden */
    int hud_marker_names; /* the names over them: 0 the players' only, 1 every car's, 2 none */
    int hud_reticle;      /* the lock-on reticle shown (TM2012's four arcs; off by default) */
    int hud_mirror_height; /* the rear-view mirror's eye: 0 the original's (the chase camera's, high), 1 on the car's roof */
    int draw_distance;    /* 0 the game's (4800), 1 far (6000), 2 farthest (8000) */
    int widescreen;       /* battles as wide as the window (Hor+: more to the sides), vplus/wide.c; 0: 4:3 (default on) */
    int frame_rate;       /* 0 the game's (each 20 Hz step shown 3 VBlanks), 1 60 fps (in-between frames, vplus/interp.c; default),
                           * 2 uncapped (more in-between frames, up to the display's refresh rate) */
    int wobble;           /* 0 the PS1's (whole pixels, affine textures), 1 smooth (subpixel vertices), 2 none (also perspective textures, default) */
    int depth;            /* hidden surfaces: 1 a depth buffer (default; with wobble none: runtime/gpu.c), 0 the PS1's order only */
    int window_mode;      /* 0 a window, 1 borderless fullscreen (default), 2 exclusive fullscreen; F4 switches */
    int display_w, display_h; /* exclusive fullscreen's display mode (0: the desktop's) */
    float display_hz;
    int split;            /* split screen: 0 by arena (the game's), 1 top and bottom, 2 side by side (3 players: player 1's half) */
} VideoSettings;
extern VideoSettings settings_video;
/* Each player's battle HUD (Customize HUD): kept in their profile ([hud]); players without one
 * (guests) have the defaults of tm2pc.ini's [video]. settings_video's hud_* fields are the ones in
 * use: a player's are put there (hud_select) before their HUD is drawn, and the menu edits them there
 * (hud_store keeps them). Slot -1: the defaults. */
typedef struct {
    int no_radar, full_weapons, no_target, no_messages, no_turbo, no_speed, no_lives;
    int style, buttons, mirror, low_top, layout, size, no_energy, turbo_bar;
    int markers, no_marker_health, marker_names, reticle, mirror_height;
} HudSettings;
extern HudSettings hud_players[4];
void hud_select(int p);
void hud_store(int p);
const HudSettings *hud_of(int p);
int hud_parse(HudSettings *h, const char *key, const char *val); /* 1: a hud_ key, read */
void hud_write(FILE *f, const HudSettings *h);                    /* its [hud] section */
void hud_reset_player(int p);                                      /* to the defaults (a guest) */
void video_settings_changed(void); /* gpu_psyz.c: applies resolution and dithering now */

/* The game's own options, which the port's Options pages replace (src/ui):
 * [gameplay] difficulty and world_map, [controls] keypad1/keypad2, and the
 * port's volumes in [audio]. ui/options.c writes the game's values into its
 * settings block. */
typedef struct {
    int difficulty;     /* 0 easy, 1 medium, 2 hard */
    int map;            /* the world globe between tournament levels: 1 on, 0 off */
    int keypad[2];      /* button layout per player: 0 standard, 1 run 'n gun, 2 control freak, 3 deadly force */
    int music, effects; /* volumes, 0-100 (cd_audio.c, PSY-Z's voice gain) */
    int menu_sounds;    /* the port's menus play the game's cursor and select sounds */
    int level_songs;    /* custom music files brought to the game's own music's loudness */
    int voice_mode;     /* online voice chat: 0 push to talk (Controls), 1 open mic (sends while you speak) */
    int voice_volume;   /* the other players' voices, 0-100 */
    int mic_gate;       /* open mic: the microphone gate, 1-10: how loud a voice must be to send (1 the quietest) */
    char mic[128];      /* the microphone's name ("": the system's default) */
} GameSettings;
extern GameSettings settings_game;
/* [network] masters: the master servers, "host:port, host:port"; the ini's empty: the game's own list, this (the main
 * one first: OpenTM2-MasterServer1, a DigitalOcean droplet; then OpenTM2-MasterServer2, the user's Synology NAS, the
 * backup) */
#define MASTERS_DEFAULT "147.182.184.67:27522, opentm2-masterserver2.synology.me:27522"
extern char settings_masters[512];
extern int settings_mod_download_mb; /* [network] mod_download_limit: the most a server's mods may download, MB (0: no limit) */
/* [update]: the check at start (on / off) and where it asks: a GitHub Releases "latest" answer (runtime/update.h; the
 * releases' repository is the user's to make public) */
#define UPDATE_URL_DEFAULT "https://api.github.com/repos/Tokatta007/OpenTM2/releases/latest"
extern int settings_update_check;
extern char settings_update_url[512];
