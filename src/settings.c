/* The settings file, tm2pc.ini (next to the executable).
 *
 * An INI file: [section] lines, key = value lines, ';' starts a comment.
 * Each section belongs to a module: [game] (here: the disc image),
 * [profile] (profile.c), [video] (here, used by gpu_psyz.c), [gameplay],
 * [audio] and [controls] (here, the game's own options, written into the
 * game by ui/options.c), [vanilla_plus] (vplus.c) and [music] (music.c). Unknown sections and keys are ignored, so files from other
 * builds still load. A missing file is written with every setting listed,
 * and sections a file lacks (from an older build) are added to it. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "vplus/vplus.h"
#include "music/music.h"
#include "profile/profile.h"
#include "controls/bindings.h"
#include "mods/mods.h"

/* ---- [game] ---- */

static char disc[1024];        /* given on the command line */
static char saved_disc[1024];  /* from the file */
static char pc_version[1024];  /* the 1997 PC version (its disc image or folder), for the first-time setup's pictures */

void settings_set_disc(const char *path) { snprintf(disc, sizeof disc, "%s", path); }
const char *settings_disc(void) { return saved_disc; }
const char *settings_pc_version(void) { return pc_version; }

static void game_key(const char *key, const char *val)
{
    if (!strcmp(key, "disc")) snprintf(saved_disc, sizeof saved_disc, "%s", val);
    else if (!strcmp(key, "pc_version")) snprintf(pc_version, sizeof pc_version, "%s", val);
}

static void game_write(FILE *f)
{
    if (!*disc) snprintf(disc, sizeof disc, "%s", saved_disc); /* (a section rewritten: the file's) */
    fprintf(f, "[game]\n; the Twisted Metal 2 disc image (.cue)\ndisc = %s\n", disc);
    fprintf(f, "; the 1997 PC version (its disc image: .iso, .cue, .mdf, .img; or installed folder), for sharper menu pictures (empty: the PS1's)\n"
               "pc_version = %s\n", pc_version);
    snprintf(saved_disc, sizeof saved_disc, "%s", disc);
}

void settings_set_game_discs(const char *ps1, const char *pc)
{
    snprintf(disc, sizeof disc, "%s", ps1);
    snprintf(pc_version, sizeof pc_version, "%s", pc ? pc : "");
    settings_save_section("game");
}

/* The disc came from the command line: store it if the file has none, so
 * the next start needs no arguments. */
void settings_remember_disc(const char *path)
{
    if (*saved_disc || !*disc)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "\n");
    game_write(f);
    fclose(f);
}

/* ---- [video] ---- */

VideoSettings settings_video = {.widescreen = 1, .window_mode = 1, .wobble = 2, .depth = 1, .frame_rate = 1, .draw_distance = 2};

static void video_key(const char *key, const char *val)
{
    if (!strcmp(key, "resolution")) {
        int n = atoi(val);
        settings_video.resolution = (n >= 1 && n <= 8) ? n : 0;
    } else if (!strcmp(key, "dithering")) {
        settings_video.dithering = !strcmp(val, "on") ? 1 : !strcmp(val, "off") ? 2 : 0;
    } else if (!strcmp(key, "textures")) {
        settings_video.textures = !strcmp(val, "playstation");
    } else if (!strcmp(key, "menu_sides")) {
        settings_video.menu_sides = !strcmp(val, "blur") ? 1 : !strcmp(val, "black") ? 2 : 0;
    } else if (!strcmp(key, "split")) {
        settings_video.split = !strcmp(val, "top_bottom") ? 1 : 2; /* (side by side unless top and bottom: "arena" is gone) */
    } else if (!strcmp(key, "widescreen")) {
        settings_video.widescreen = !strcmp(val, "on");
    } else if (!strcmp(key, "frame_rate")) {
        settings_video.frame_rate = !strcmp(val, "original") ? 0 : !strcmp(val, "uncapped") ? 2 : 1;
    } else if (!strcmp(key, "wobble")) {
        settings_video.wobble = !strcmp(val, "ps1") ? 0 : !strcmp(val, "smooth") ? 1 : 2;
    } else if (!strcmp(key, "hidden_surfaces")) {
        settings_video.depth = strcmp(val, "ps1") != 0;
    } else if (!strcmp(key, "window")) {
        settings_video.window_mode = !strcmp(val, "window") ? 0 : !strcmp(val, "exclusive") ? 2 : 1;
    } else if (!strcmp(key, "display_mode")) {
        int w = 0, h = 0;
        float hz = 0;
        if (sscanf(val, "%dx%d@%f", &w, &h, &hz) < 2 || w <= 0 || h <= 0) w = h = 0, hz = 0;
        settings_video.display_w = w, settings_video.display_h = h, settings_video.display_hz = hz;
    } else if (!strcmp(key, "draw_distance")) {
        settings_video.draw_distance = !strcmp(val, "far") ? 1 : !strcmp(val, "farthest") ? 2 : 0;
    } else if (!strcmp(key, "hud_radar")) {
        settings_video.hud_no_radar = !strcmp(val, "off");
    } else if (!strcmp(key, "hud_weapons")) {
        settings_video.hud_full_weapons = !strcmp(val, "full") || !strcmp(val, "both"); /* ("both": no longer offered) */
    } else if (!strcmp(key, "hud_target")) {
        settings_video.hud_no_target = !strcmp(val, "off");
    } else if (!strcmp(key, "hud_messages")) {
        settings_video.hud_no_messages = !strcmp(val, "off");
    } else if (!strcmp(key, "hud_message_text")) {
        settings_video.hud_msg_classic = !strcmp(val, "classic");
    } else if (!strcmp(key, "hud_turbo")) {
        settings_video.hud_no_turbo = !strcmp(val, "off");
    } else if (!strcmp(key, "hud_speed")) {
        settings_video.hud_no_speed = !strcmp(val, "off");
    } else if (!strcmp(key, "hud_lives")) {
        settings_video.hud_no_lives = !strcmp(val, "off");
    } else if (!strcmp(key, "hud_energy")) {
        settings_video.hud_no_energy = !strcmp(val, "off"); /* (on; "classic" and "blue" were colors before) */
    } else if (!strcmp(key, "hud_turbo_shown_as")) {
        settings_video.hud_turbo_bar = !strcmp(val, "bar");
    } else if (!strcmp(key, "hud_style")) {
        settings_video.hud_style = !strcmp(val, "black") ? 1 : !strcmp(val, "2012") ? 2 : 0;
    } else if (!strcmp(key, "hud_buttons")) {
        settings_video.hud_buttons = !strcmp(val, "on");
    } else if (!strcmp(key, "hud_mirror")) {
        settings_video.hud_mirror = !strcmp(val, "on") ? 1 : !strcmp(val, "small") ? 2 : 0;
    } else if (!strcmp(key, "hud_top")) {
        settings_video.hud_low_top = !strcmp(val, "classic");
    } else if (!strcmp(key, "hud_layout")) {
        settings_video.hud_layout = !strcmp(val, "classic");
    } else if (!strcmp(key, "hud_size")) {
        int n = atoi(val);
        settings_video.hud_size = n >= 50 && n <= 150 ? n : 100;
    } else if (!strcmp(key, "hud_enemy_markers")) { /* (2026-10-13's hud_markers, on by default then: not read) */
        settings_video.hud_markers = !strcmp(val, "on");
    } else if (!strcmp(key, "hud_marker_health")) {
        settings_video.hud_no_marker_health = !strcmp(val, "off");
    } else if (!strcmp(key, "hud_marker_names")) {
        settings_video.hud_marker_names = !strcmp(val, "all") ? 1 : !strcmp(val, "off") ? 2 : 0;
    } else if (!strcmp(key, "hud_lock_on")) { /* (and hud_lock_reticle) */
        settings_video.hud_reticle = !strcmp(val, "on");
    } else if (!strcmp(key, "hud_mirror_height")) {
        settings_video.hud_mirror_height = !strcmp(val, "car");
    }
}

/* ---- each player's HUD (settings.h) ---- */
#define HUD_FIELDS(X) X(no_radar) X(full_weapons) X(no_target) X(no_messages) X(no_turbo) X(no_speed) X(no_lives) \
    X(style) X(buttons) X(mirror) X(low_top) X(layout) X(size) X(no_energy) X(turbo_bar) \
    X(markers) X(no_marker_health) X(marker_names) X(reticle) X(mirror_height)
HudSettings hud_players[4];
static HudSettings hud_defaults;
static void hud_from_video(HudSettings *h)
{
#define FROM(n) h->n = settings_video.hud_##n;
    HUD_FIELDS(FROM)
#undef FROM
}
static void hud_to_video(const HudSettings *h)
{
#define TO(n) settings_video.hud_##n = h->n;
    HUD_FIELDS(TO)
#undef TO
}
void hud_select(int p) { hud_to_video(p >= 0 && p < 4 ? &hud_players[p] : &hud_defaults); }
void hud_store(int p) { hud_from_video(p >= 0 && p < 4 ? &hud_players[p] : &hud_defaults); }
const HudSettings *hud_of(int p) { return p >= 0 && p < 4 ? &hud_players[p] : &hud_defaults; }
void hud_reset_player(int p) { if (p >= 0 && p < 4) hud_players[p] = hud_defaults; }
static void video_key(const char *key, const char *val);
static void hud_lines(FILE *f);
int hud_parse(HudSettings *h, const char *key, const char *val)
{
    if (strncmp(key, "hud_", 4)) return 0;
    HudSettings active;
    hud_from_video(&active);
    hud_to_video(h);
    video_key(key, val);
    hud_from_video(h);
    hud_to_video(&active);
    return 1;
}
void hud_write(FILE *f, const HudSettings *h)
{
    HudSettings active;
    hud_from_video(&active);
    hud_to_video(h);
    fprintf(f, "[hud]\n; this player's battle HUD (Customize HUD)\n");
    hud_lines(f);
    hud_to_video(&active);
}

static void video_write(FILE *f)
{
    static const char *const dither[] = {"auto", "on", "off"};
    static const char *const sides[] = {"extension", "blur", "black"};
    fprintf(f, "[video]\n"
               "; resolution: auto (to fit the window or screen) or 1 to 8 times the PS1's;\n"
               "; F9 cycles it while playing. dithering: auto (as on the PS1 at 1x, off when\n"
               "; upscaled), on or off. menu_sides: what fills the sides of menus on screens\n"
               "; wider than 4:3: extension (the picture continued), blur or black.\n");
    if (settings_video.resolution) fprintf(f, "resolution = %d\n", settings_video.resolution);
    else fprintf(f, "resolution = auto\n");
    fprintf(f, "dithering = %s\n", dither[settings_video.dithering]);
    fprintf(f, "; textures: sharpest (the PC version's where it has the same pictures, sharper; its changed ones never)\n"
               ";   or playstation (the PlayStation's only)\n");
    fprintf(f, "textures = %s\n", settings_video.textures ? "playstation" : "sharpest");
    fprintf(f, "menu_sides = %s\n", sides[settings_video.menu_sides]);
    static const char *const far[] = {"original", "far", "farthest"};
    fprintf(f, "; frame_rate: original (the game's 20 steps a second, each shown three times), 60 (frames in between) or uncapped\n"
               ";   (more frames in between, up to the display's refresh rate).\n");
    static const char *const rates[] = {"original", "60", "uncapped"};
    fprintf(f, "frame_rate = %s\n", rates[settings_video.frame_rate % 3]);
    static const char *const wobble[] = {"ps1", "smooth", "none"};
    fprintf(f, "; wobble: ps1 (whole pixels, affine textures), smooth (subpixel vertices) or none (also perspective textures).\n");
    fprintf(f, "wobble = %s\n", wobble[settings_video.wobble % 3]);
    fprintf(f, "; hidden_surfaces: depth (what is in front hides what is behind it; needs wobble none) or ps1 (drawn back to\n"
               ";   front as the PlayStation: walls and ground sometimes show what is behind them).\n");
    fprintf(f, "hidden_surfaces = %s\n", settings_video.depth ? "depth" : "ps1");
    static const char *const window[] = {"window", "borderless", "exclusive"};
    fprintf(f, "; window: window, borderless (fullscreen at the desktop's resolution) or exclusive (fullscreen). F4 switches.\n");
    fprintf(f, "window = %s\n", window[settings_video.window_mode % 3]);
    fprintf(f, "; display_mode: exclusive fullscreen's resolution and refresh rate (as 2560x1440@144), or desktop.\n");
    if (settings_video.display_w)
        fprintf(f, "display_mode = %dx%d@%g\n", settings_video.display_w, settings_video.display_h, settings_video.display_hz);
    else fprintf(f, "display_mode = desktop\n");
    fprintf(f, "; widescreen: on (battles as wide as the window: more to the sides) or off (4:3, as the PS1).\n");
    fprintf(f, "widescreen = %s\n", settings_video.widescreen ? "on" : "off");
    fprintf(f, "; draw_distance: original (as the PS1), far or farthest (16000 / 32000 units, 4800 the PS1's).\n");
    fprintf(f, "draw_distance = %s\n", far[settings_video.draw_distance % 3]);
    static const char *const split[] = {"arena", "top_bottom", "side_by_side"};
    fprintf(f, "; split: the split screen: side_by_side (the default) or top_bottom (3 players: player 1's half\n"
               "; at the left or on top). Select in a battle switches it.\n");
    fprintf(f, "split = %s\n", split[settings_video.split % 3]);
    fprintf(f, "; the battle HUD of players without a profile (Customize HUD; a profile keeps its own in [hud]):\n"
               "; hud_radar on/off, hud_weapons selected/full, hud_target (the targeted car's name and health)\n"
               "; on/off, hud_messages on/off.\n");
    HudSettings active;
    hud_from_video(&active);
    hud_to_video(&hud_defaults);
    hud_lines(f);
    hud_to_video(&active);
}

/* the hud_ keys of settings_video, as they are */
static void hud_lines(FILE *f)
{
    fprintf(f, "hud_radar = %s\n", settings_video.hud_no_radar ? "off" : "on");
    static const char *const weap[2] = {"selected", "full"};
    fprintf(f, "hud_weapons = %s\n", weap[settings_video.hud_full_weapons != 0]);
    fprintf(f, "hud_buttons = %s\n", settings_video.hud_buttons ? "on" : "off");
    static const char *const style[3] = {"tm2", "black", "2012"};
    fprintf(f, "; hud_style: tm2 (the game's), black (Twisted Metal: Black's) or 2012 (Twisted Metal 2012's)\n");
    fprintf(f, "hud_style = %s\n", style[settings_video.hud_style % 3]);
    fprintf(f, "hud_target = %s\n", settings_video.hud_no_target ? "off" : "on");
    fprintf(f, "hud_messages = %s\n", settings_video.hud_no_messages ? "off" : "on");
    fprintf(f, "hud_message_text = %s\n", settings_video.hud_msg_classic ? "classic" : "centered");
    fprintf(f, "; hud_layout: adaptive (each part at its corner of the view, smaller in a small view)\n"
               "; or classic (the game's). hud_size: 50 to 150 (percent of the normal size).\n");
    fprintf(f, "hud_layout = %s\n", settings_video.hud_layout ? "classic" : "adaptive");
    fprintf(f, "hud_turbo = %s\n", settings_video.hud_no_turbo ? "off" : "on");
    fprintf(f, "hud_speed = %s\n", settings_video.hud_no_speed ? "off" : "on");
    fprintf(f, "hud_lives = %s\n", settings_video.hud_no_lives ? "off" : "on");
    fprintf(f, "; hud_energy: the energy bar on or off (every style); TM2 style: hud_turbo_shown_as number or bar\n");
    fprintf(f, "hud_energy = %s\n", settings_video.hud_no_energy ? "off" : "on");
    fprintf(f, "hud_turbo_shown_as = %s\n", settings_video.hud_turbo_bar ? "bar" : "number");
    fprintf(f, "hud_top = %s\n", settings_video.hud_low_top ? "classic" : "raised");
    fprintf(f, "hud_mirror = %s\n", settings_video.hud_mirror == 2 ? "small" : settings_video.hud_mirror ? "on" : "off");
    fprintf(f, "; hud_mirror_height: original (the mirror looks back from the chase camera) or car (from the car's roof)\n");
    fprintf(f, "hud_mirror_height = %s\n", settings_video.hud_mirror_height ? "car" : "original");
    fprintf(f, "hud_size = %d\n", settings_video.hud_size >= 50 && settings_video.hud_size <= 150 ? settings_video.hud_size : 100);
    fprintf(f, "; hud_enemy_markers: a triangle over each other car (on/off, off by default), hud_marker_health its health\n"
               "; bar, hud_marker_names players (other players' names), all (computer cars by car too) or off;\n"
               "; hud_lock_on: the lock-on circle round the car your missiles would home on (off by default).\n");
    fprintf(f, "hud_enemy_markers = %s\n", settings_video.hud_markers ? "on" : "off");
    fprintf(f, "hud_marker_health = %s\n", settings_video.hud_no_marker_health ? "off" : "on");
    static const char *const names[3] = {"players", "all", "off"};
    fprintf(f, "hud_marker_names = %s\n", names[settings_video.hud_marker_names % 3]);
    fprintf(f, "hud_lock_on = %s\n", settings_video.hud_reticle ? "on" : "off");
}

/* ---- [gameplay], [controls], [audio]: the game's own options ---- */

GameSettings settings_game = {1, 1, {0, 0}, 100, 100, 1, 1, 0, 100, 6, ""};

static const char *const difficulty_names[] = {"easy", "medium", "hard"};
static const char *const keypad_names[] = {"standard", "run_n_gun", "control_freak", "deadly_force"};

static int name_index(const char *val, const char *const *names, int n, int fallback)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(val, names[i])) return i;
    return fallback;
}

static int percent(const char *val, int fallback)
{
    char *end;
    long n = strtol(val, &end, 10);
    return end == val ? fallback : n < 0 ? 0 : n > 100 ? 100 : (int)n;
}

static void gameplay_key(const char *key, const char *val)
{
    if (!strcmp(key, "difficulty"))
        settings_game.difficulty = name_index(val, difficulty_names, 3, settings_game.difficulty);
    if (!strcmp(key, "world_map"))
        settings_game.map = strcmp(val, "off") != 0;
}

static void gameplay_write(FILE *f)
{
    fprintf(f, "[gameplay]\n; difficulty: easy, medium or hard. world_map: the world globe between\n"
               "; tournament levels, on or off\n");
    fprintf(f, "difficulty = %s\n", difficulty_names[settings_game.difficulty]);
    fprintf(f, "world_map = %s\n", settings_game.map ? "on" : "off");
}

static void controls_key(const char *key, const char *val)
{
    if (!strcmp(key, "keypad1")) settings_game.keypad[0] = name_index(val, keypad_names, 4, settings_game.keypad[0]);
    if (!strcmp(key, "keypad2")) settings_game.keypad[1] = name_index(val, keypad_names, 4, settings_game.keypad[1]);
    bind_ini_key(key, val);
}

static void controls_write(FILE *f)
{
    fprintf(f, "[controls]\n; player 2's button layout: standard, run_n_gun, control_freak or deadly_force\n"
               "; (player 1's is in the profile)\n");
    fprintf(f, "keypad2 = %s\n", keypad_names[settings_game.keypad[1]]);
    bind_ini_write(f);
}

char settings_masters[512]; /* (empty: the game's own list, MASTERS_DEFAULT) */
int settings_mod_download_mb = 50;
static void network_key(const char *key, const char *val)
{
    if (!strcmp(key, "masters")) snprintf(settings_masters, sizeof settings_masters, "%s", val);
    if (!strcmp(key, "mod_download_limit")) settings_mod_download_mb = !strcmp(val, "none") ? 0 : atoi(val) > 0 ? atoi(val) : 50;
}
static void network_write(FILE *f)
{
    fprintf(f, "[network]\n; masters: the master servers the internet's server list comes from, host:port, comma separated (the\n"
               "; first the main one; a server is listed on all of them, the browser asks them all); empty: the game's own list\n");
    fprintf(f, "masters = %s\n", settings_masters);
    fprintf(f, "; mod_download_limit: joining a server, its mods are downloaded only up to this many MB (none: no limit)\n");
    if (settings_mod_download_mb > 0) fprintf(f, "mod_download_limit = %d\n", settings_mod_download_mb);
    else fprintf(f, "mod_download_limit = none\n");
}
int settings_update_check = 1;
char settings_update_url[512] = UPDATE_URL_DEFAULT;
static void update_key(const char *key, const char *val)
{
    if (!strcmp(key, "check")) settings_update_check = strcmp(val, "off") != 0;
    if (!strcmp(key, "url")) snprintf(settings_update_url, sizeof settings_update_url, "%s", *val ? val : UPDATE_URL_DEFAULT);
}
static void update_write(FILE *f)
{
    fprintf(f, "[update]\n; check: at start, ask whether a newer version is out (on or off); url: where (a GitHub Releases latest\n"
               "; answer)\n");
    fprintf(f, "check = %s\nurl = %s\n", settings_update_check ? "on" : "off", settings_update_url);
}
static void audio_key(const char *key, const char *val)
{
    if (!strcmp(key, "music")) settings_game.music = percent(val, settings_game.music);
    if (!strcmp(key, "effects")) settings_game.effects = percent(val, settings_game.effects);
    if (!strcmp(key, "menu_sounds")) settings_game.menu_sounds = strcmp(val, "off") != 0;
    if (!strcmp(key, "level_songs")) settings_game.level_songs = strcmp(val, "off") != 0;
    if (!strcmp(key, "voice_mode")) settings_game.voice_mode = !strcmp(val, "open_mic");
    if (!strcmp(key, "voice_volume")) settings_game.voice_volume = percent(val, settings_game.voice_volume);
    if (!strcmp(key, "mic_gate") || !strcmp(key, "mic_sensitivity")) { /* (the sensitivity of before: the other way round) */
        int v = atoi(val);
        v = v < 1 ? 1 : v > 10 ? 10 : v;
        settings_game.mic_gate = strcmp(key, "mic_gate") ? 11 - v : v;
    }
    if (!strcmp(key, "microphone")) snprintf(settings_game.mic, sizeof settings_game.mic, "%s", strcmp(val, "default") ? val : "");
}

static void audio_write(FILE *f)
{
    fprintf(f, "[audio]\n; volumes, 0 to 100: music (the game's music and your own) and sound effects\n");
    fprintf(f, "music = %d\neffects = %d\n", settings_game.music, settings_game.effects);
    fprintf(f, "; menu_sounds: the port's menus play the game's cursor and select sounds, on or off\n");
    fprintf(f, "menu_sounds = %s\n", settings_game.menu_sounds ? "on" : "off");
    fprintf(f, "; level_songs: your music files brought to about the loudness of the game's own music, on or off\n");
    fprintf(f, "level_songs = %s\n", settings_game.level_songs ? "on" : "off");
    fprintf(f, "; online voice chat: voice_mode push_to_talk (its key and button in [controls]) or open_mic; voice_volume\n"
               "; 0 to 100 (the others' voices); mic_gate 1 to 10 (open mic: how loud a voice must be to send; 1 the quietest);\n"
               "; microphone: default or its name\n");
    fprintf(f, "voice_mode = %s\nvoice_volume = %d\nmic_gate = %d\nmicrophone = %s\n",
            settings_game.voice_mode ? "open_mic" : "push_to_talk", settings_game.voice_volume, settings_game.mic_gate,
            settings_game.mic[0] ? settings_game.mic : "default");
}

/* ---- the file ---- */

static const struct {
    const char *name;
    void (*key)(const char *key, const char *val);
    void (*write)(FILE *f);
} sections[] = {
    {"game", game_key, game_write},
    {"network", network_key, network_write},
    {"update", update_key, update_write},
    {"profile", profile_ini_key, profile_ini_write},
    {"gameplay", gameplay_key, gameplay_write},
    {"video", video_key, video_write},
    {"audio", audio_key, audio_write},
    {"controls", controls_key, controls_write},
    {"vanilla_plus", vp_ini_key, vp_ini_write},
    {"music", music_ini_key, music_ini_write},
    {"mods", mods_ini_key, mods_ini_write},
};
#define SECTION_COUNT ((int)(sizeof sections / sizeof sections[0]))

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void write_default(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "; Twisted Metal 2 PC settings\n");
    for (int i = 0; i < SECTION_COUNT; i++) {
        fprintf(f, "\n");
        sections[i].write(f);
    }
    fclose(f);
}

static char file_path[1100]; /* the file settings_load read, for saving */

void settings_load(const char *path, int create)
{
    snprintf(file_path, sizeof file_path, "%s", path);
    vp_ini_begin();
    FILE *f = fopen(path, "r");
    if (!f) {
        if (create)
            write_default(path);
        vp_ini_end();
        return;
    }
    char line[1024];
    int current = -1, seen[SECTION_COUNT] = {0};
    while (fgets(line, sizeof line, f)) {
        char *s = line, *c = strchr(s, ';');
        if (c) *c = 0;
        s = trim(s);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) continue;
            *e = 0;
            current = -1;
            for (int i = 0; i < SECTION_COUNT; i++)
                if (!strcmp(s + 1, sections[i].name)) current = i;
            if (current >= 0) seen[current] = 1;
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || current < 0)
            continue;
        *eq = 0;
        sections[current].key(trim(s), trim(eq + 1));
    }
    fclose(f);
    vp_ini_end();
    hud_from_video(&hud_defaults); /* the defaults; each player's from them until their profile's are read */
    for (int p = 0; p < 4; p++) hud_players[p] = hud_defaults;
    if (!create)
        return;
    /* sections this file lacks: add them with the current values ([game]
     * only once there is a disc to store) */
    FILE *a = NULL;
    for (int i = 0; i < SECTION_COUNT; i++) {
        if (seen[i] || (sections[i].write == game_write && !*disc))
            continue;
        if (!a && !(a = fopen(path, "a")))
            return;
        fprintf(a, "\n");
        sections[i].write(a);
    }
    if (a) fclose(a);
}

/* The file's lines are copied to a new file, except those of the named
 * section, which is written anew where it was (or at the end if the file
 * lacks it); the new file then replaces the old. */
void settings_save_section(const char *name)
{
    int sec = -1;
    for (int i = 0; i < SECTION_COUNT; i++)
        if (!strcmp(sections[i].name, name)) sec = i;
    if (sec < 0 || !*file_path)
        return;
    char tmp[1200], line[1024], copy[1024];
    snprintf(tmp, sizeof tmp, "%s.new", file_path);
    FILE *in = fopen(file_path, "r"), *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        return;
    }
    int inside = 0, written = 0;
    while (in && fgets(line, sizeof line, in)) {
        char *s = trim(strcpy(copy, line));
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) *e = 0;
            inside = e && !strcmp(s + 1, name);
            if (inside) {
                sections[sec].write(out);
                fprintf(out, "\n");
                written = 1;
                continue;
            }
        }
        if (!inside)
            fputs(line, out);
    }
    if (in) fclose(in);
    if (!written) {
        fprintf(out, "\n");
        sections[sec].write(out);
    }
    fclose(out);
    remove(file_path);
    if (rename(tmp, file_path))
        fprintf(stderr, "settings: cannot write %s\n", file_path);
}
