/* Custom music: each of TM2's music tracks can play the disc track
 * (default), a file, or a random file from a folder.
 *
 * The tracks are the .DA files on the disc (one per audio track). In
 * tm2pc.ini:
 *
 *   [music]
 *   folder = music            ; next to the executable unless a full path
 *   FRWYSUB = default         ; the disc track
 *   TMLOGO = title.wav        ; a file (in the folder unless a full path)
 *   PARIS = random            ; a random file from the folder, each time
 *   HONG = random:hk          ; a random file from another folder
 *   LAVA = order              ; the folder's files in name order, one per start
 *
 * Every track set to order shares one position in the folder, so the songs
 * follow on from track to track. Next and previous song (music_skip) work on
 * a track set to random or order: next plays another file, previous goes
 * back through the files played before.
 *
 * Files: WAV (through SDL: PCM, float or ADPCM), MP3, FLAC and Ogg Vorbis
 * (decoders.c), any rate, converted to 44.1 kHz stereo; cd_audio.c plays them through the SPU's CD
 * input, so the game's music volume and fades apply. The Custom Music page
 * (ui/options.c) edits the same settings. */
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "runtime/runtime.h"
#include "music.h"
#include "decoders.h"
#include "settings.h"
#include "mods/mod_arena.h"
#include <math.h>

typedef struct {
    const char *da;     /* file on the disc, without .DA: the settings key */
    const char *where;  /* where the game plays it (for the settings file) */
    const char *name;   /* its name on the Custom Music page; NULL: not listed */
    char setting[512];  /* "", "default", "random", "random:FOLDER", "order" or a file */
    int track;          /* disc track (0-based), -1 until looked up */
} Slot;

static Slot slots[] = {
    {"FRWYSUB", "Hollywood", "Hollywood", "", -1},
    {"DISH", "Moscow", "Moscow", "", -1},
    {"PARIS", "Paris", "Paris", "", -1},
    {"LAVA", "Amazonia", "Amazonia", "", -1},
    {"RFNEW", "New York", "New York", "", -1},
    {"GLACIER", "Antarctica", "Antarctica", "", -1},
    {"DNMSWP", "Holland", "Holland", "", -1},
    {"HONG", "Hong Kong", "Hong Kong", "", -1},
    {"TMLEVELS", "Dark Tooth (boss): his whole fight, both its songs", "Dark Tooth", "", -1},
    {"TMLOGO", "title screen and menus (in the Dark Tooth fight: the Dark Tooth setting)", "Menus", "", -1},
    {"BLANK", "silence on the disc (where the game uses it is not known yet)", NULL, "", -1},
    {"MODARENA", "a mod arena: default (its own music.wav / .mp3 / .flac / .ogg, else a random arena's track from the disc), "
     "random, order or a file", "Mod arenas", "", -2},
    {"PODIUM", "a Versus Tournament's podium and final standings: default (the menus' music), random, order or a file",
     "Versus podium", "", -2},
    {"SHOWDOWN", "a battle's final moments: off, default (the Dark Tooth track as set), random, order or a file",
     "Showdown", "", -2},
};
#define SHOWDOWN_SLOT (SLOT_COUNT - 1)
static int showdown_playing;
#define SLOT_COUNT ((int)(sizeof slots / sizeof slots[0]))

static const char *const extensions[] = {"wav", "mp3", "flac", "ogg"};
static char folder[512] = "music";
static char folder_setting[512] = "music"; /* as written in the settings file */
static int next_at_end = 1; /* a random or order song that ends: the next one (0: it starts over) */
int music_next_at_end(void) { return next_at_end; }
void music_set_next_at_end(int on) { next_at_end = on != 0; }
static int tracks_known;

void music_set_folder(const char *f)
{
    snprintf(folder, sizeof folder, "%s", f);
}

static int is_off(const char *s) { return !*s || !SDL_strcasecmp(s, "off"); }
int music_showdown(void) { return !is_off(slots[SHOWDOWN_SLOT].setting); }
int music_podium_slot(void) /* the Versus podium's own song (its slot), or -1: the menus' music (default) */
{
    for (int i = 0; i < SLOT_COUNT; i++)
        if (!SDL_strcasecmp(slots[i].da, "PODIUM"))
            return *slots[i].setting && SDL_strcasecmp(slots[i].setting, "default") ? i : -1;
    return -1;
}
void music_showdown_play(int on) { showdown_playing = on; }

void music_ini_key(const char *key, const char *val)
{
    if (!SDL_strcasecmp(key, "folder")) {
        if (*val) {
            snprintf(folder_setting, sizeof folder_setting, "%s", val);
            /* relative to the executable's folder, which music_set_folder got first */
            if (val[0] == '/' || val[0] == '\\' || (val[0] && val[1] == ':')) {
                music_set_folder(val);
            } else {
                char base[512];
                snprintf(base, sizeof base, "%s", folder);
                char *cut = strrchr(base, '/'), *cut2 = strrchr(base, '\\');
                if (cut2 > cut) cut = cut2;
                if (cut) cut[1] = 0; else base[0] = 0;
                snprintf(folder, sizeof folder, "%s%s", base, val);
            }
        }
        return;
    }
    if (!SDL_strcasecmp(key, "at_end")) { next_at_end = SDL_strcasecmp(val, "repeat") != 0; return; }
    for (int i = 0; i < SLOT_COUNT; i++)
        if (!SDL_strcasecmp(key, slots[i].da))
            snprintf(slots[i].setting, sizeof slots[i].setting, "%s", val);
}

void music_ini_write(FILE *f)
{
    fprintf(f, "[music]\n"
               "; Each music track of the disc: default (the disc), a file (in the folder\n"
               "; unless a full path), random (a random file from the folder, each time the\n"
               "; track starts), random:FOLDER, or order (the folder's files in name order).\n"
               "; Files: WAV, MP3, FLAC, OGG.\n"
               "folder = %s\n"
               "; a song of a track set to random or order that ends: next (another song) or repeat\n"
               "at_end = %s\n", folder_setting, next_at_end ? "next" : "repeat");
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (*slots[i].where) fprintf(f, "; %s\n", slots[i].where);
        fprintf(f, "%s = %s\n", slots[i].da, *slots[i].setting ? slots[i].setting : i == SHOWDOWN_SLOT ? "off" : "default");
    }
}

static void find_tracks(void)
{
    tracks_known = 1;
    for (int i = 0; i < SLOT_COUNT; i++) {
        char path[32];
        uint32_t lba, size;
        snprintf(path, sizeof path, "\\%s.DA;1", slots[i].da);
        if (!cd_find_file(path, &lba, &size))
            continue;
        for (int t = 0; t < cd_track_count; t++)
            if (lba >= cd_tracks[t].file_start && lba < cd_tracks[t].file_start + cd_tracks[t].sectors)
                slots[i].track = t;
    }
}

static int mod_slot(void)
{
    static int at = -2;
    if (at == -2) {
        at = -1;
        for (int i = 0; i < SLOT_COUNT; i++) if (!SDL_strcasecmp(slots[i].da, "MODARENA")) at = i;
    }
    return at;
}
/* A mod arena plays as Holland (mod_arena.c): the game starts Holland's track. Without a file of its own (or a setting)
 * it is another arena's track instead, picked at random when the arena loads (cd_audio.c: music_remap). */
static int mod_arena_track = -1; /* (the track the game asks for: Holland's) */
static int mod_music_level;      /* (the arena whose track plays instead: 1..8) */
void music_mod_arena_loaded(void)
{
    if (!tracks_known) find_tracks();
    mod_arena_track = slots[MOD_ARENA_PLAYS_AS - 1].track;
    mod_music_level = 1 + (int)(SDL_GetTicks() % 8); /* (the music only: no game state, not SDL_rand's online sequence) */
    fprintf(stderr, "music: a mod arena: %s\n", mod_arena_music()[0] ? mod_arena_music() : slots[mod_music_level - 1].name);
}
int music_remap(int track, uint32_t *lba)
{
    int ma = mod_slot();
    if (ma < 0 || !mod_arena_on() || track != mod_arena_track || track < 0 || mod_music_level < 1) return track;
    if ((*slots[ma].setting && SDL_strcasecmp(slots[ma].setting, "default")) || mod_arena_music()[0]) return track;
    int to = slots[mod_music_level - 1].track;
    if (to < 0 || to == track) return track;
    uint32_t off = *lba - cd_tracks[track].file_start;
    if (off >= cd_tracks[to].sectors) off = 0; /* (a resume past its end: from its start) */
    *lba = cd_tracks[to].file_start + off;
    return to;
}

int music_slot(int track)
{
    if (!tracks_known)
        find_tracks();
    /* the Dark Tooth fight (audioDarkToothMusicFlag, 80180A05: the game's music modes 11 then 10, alternating as each
     * song ends: TMLOGO, the menus' track, then TMLEVELS): both are the Dark Tooth slot's while it is on (the user's
     * Dark Tooth song was not heard: the fight starts on the menus' track) */
    static int boss = -1, logo = -1;
    if (boss < 0)
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (!SDL_strcasecmp(slots[i].da, "TMLEVELS")) boss = i;
            if (!SDL_strcasecmp(slots[i].da, "TMLOGO")) logo = i;
        }
    int ma = mod_slot();
    if (ma >= 0 && mod_arena_on() && track == mod_arena_track) { /* a mod arena's music: the row's setting, else its own file */
        if (*slots[ma].setting && SDL_strcasecmp(slots[ma].setting, "default")) return ma;
        if (mod_arena_music()[0]) return ma;
    }
    if (boss >= 0 && logo >= 0 && mem_r8(&M.cpu, GA(0x80180A05u)) && (track == slots[boss].track || track == slots[logo].track))
        return *slots[boss].setting && SDL_strcasecmp(slots[boss].setting, "default") ? boss : -1;
    for (int i = 0; i < SLOT_COUNT; i++)
        if (slots[i].track == track) {
            const char *own = slots[SHOWDOWN_SLOT].setting; /* the showdown's own song on the boss track */
            if (showdown_playing && !SDL_strcasecmp(slots[i].da, "TMLEVELS") && !is_off(own) && SDL_strcasecmp(own, "default"))
                return SHOWDOWN_SLOT;
            return *slots[i].setting && SDL_strcasecmp(slots[i].setting, "default") ? i : -1;
        }
    return -1;
}

static int is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

static void in_folder(const char *name, char *out, size_t n)
{
    if (is_absolute(name)) snprintf(out, n, "%s", name);
    else snprintf(out, n, "%s/%s", folder, name);
}

/* The music files in dir, sorted by name (case-insensitive); the caller
 * frees each name and the list with SDL_free. */
static int compare_names(const void *a, const void *b)
{
    return SDL_strcasecmp(*(char *const *)a, *(char *const *)b);
}

static int list_files(const char *dir, char **found, int max)
{
    int count = 0;
    for (size_t e = 0; e < sizeof extensions / sizeof extensions[0]; e++) {
        char pattern[16];
        snprintf(pattern, sizeof pattern, "*.%s", extensions[e]);
        int k = 0;
        char **list = SDL_GlobDirectory(dir, pattern, SDL_GLOB_CASEINSENSITIVE, &k);
        for (int i = 0; list && i < k && count < max; i++)
            found[count++] = SDL_strdup(list[i]);
        SDL_free(list);
    }
    qsort(found, (size_t)count, sizeof *found, compare_names);
    return count;
}

static void free_files(char **found, int count)
{
    for (int i = 0; i < count; i++) SDL_free(found[i]);
}

/* A random music file from dir, into out, not `avoid` if there is another;
 * 0 if there is none. */
static int pick_random(const char *dir, const char *avoid, char *out, size_t n)
{
    char *found[256];
    int count = list_files(dir, found, 256);
    if (!count)
        return 0;
    int k = SDL_rand(count);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, found[k]);
    if (count > 1 && avoid && !strcmp(path, avoid))
        k = (k + 1 + SDL_rand(count - 1)) % count;
    snprintf(out, n, "%s/%s", dir, found[k]);
    free_files(found, count);
    return 1;
}

/* order: one position in the folder, shared by every track set to it */
static int order_pos = -1;

static int pick_in_order(const char *dir, int step, char *out, size_t n)
{
    char *found[256];
    int count = list_files(dir, found, 256);
    if (!count)
        return 0;
    order_pos = order_pos < 0 ? 0 : ((order_pos + step) % count + count) % count;
    snprintf(out, n, "%s/%s", dir, found[order_pos]);
    free_files(found, count);
    return 1;
}

/* The files played, newest last, for previous song. */
#define HISTORY 32
static char history[HISTORY][1024];
static int history_len, history_at; /* history_at: the one playing (index) */

static void remember(const char *path)
{
    if (history_len == HISTORY) {
        memmove(history[0], history[1], sizeof history[0] * (HISTORY - 1));
        history_len--;
    }
    snprintf(history[history_len], sizeof history[0], "%s", path);
    history_at = history_len++;
}

int music_file_count(void)
{
    char *found[256];
    int count = list_files(folder, found, 256);
    free_files(found, count);
    return count;
}

int music_file_name(int i, char *out, size_t n)
{
    char *found[256];
    int count = list_files(folder, found, 256);
    int ok = i >= 0 && i < count;
    if (ok) snprintf(out, n, "%s", found[i]);
    free_files(found, count);
    return ok;
}

const char *music_folder(void) { return folder; }

int music_page_count(void)
{
    int n = 0;
    for (int i = 0; i < SLOT_COUNT; i++) n += slots[i].name != NULL;
    return n;
}

static Slot *page_slot(int i)
{
    for (int k = 0; k < SLOT_COUNT; k++)
        if (slots[k].name && i-- == 0) return &slots[k];
    return NULL;
}

const char *music_page_name(int i) { Slot *s = page_slot(i); return s ? s->name : ""; }

const char *music_page_setting(int i)
{
    Slot *s = page_slot(i);
    return s && *s->setting ? s->setting : s == &slots[SHOWDOWN_SLOT] ? "off" : "default";
}
int music_page_has_off(int i) { return page_slot(i) == &slots[SHOWDOWN_SLOT]; }

void music_page_set(int i, const char *setting)
{
    Slot *s = page_slot(i);
    if (s) snprintf(s->setting, sizeof s->setting, "%s", setting);
}

int music_slot_changes(int slot)
{
    if (slot < 0 || slot >= SLOT_COUNT)
        return 0;
    const char *set = slots[slot].setting;
    return !SDL_strcasecmp(set, "order") || !SDL_strcasecmp(set, "random") || !SDL_strncasecmp(set, "random:", 7);
}

static short *decode(int slot, const char *path, size_t *frames);

/* step 0: the track starts; +1 next song; -1 previous song */
static short *load_stepped(int slot, int step, size_t *frames)
{
    if (slot < 0 || slot >= SLOT_COUNT)
        return NULL;
    const char *set = slots[slot].setting;
    char path[1024];
    if (step < 0 && history_at > 0) {          /* back through the songs played */
        snprintf(path, sizeof path, "%s", history[--history_at]);
        return decode(slot, path, frames);
    }
    if (step > 0 && history_at + 1 < history_len) { /* forward again after going back */
        snprintf(path, sizeof path, "%s", history[++history_at]);
        return decode(slot, path, frames);
    }
    if (!SDL_strcasecmp(set, "random") || !SDL_strncasecmp(set, "random:", 7)) {
        char dir[1024];
        if (set[6] == ':') in_folder(set + 7, dir, sizeof dir);
        else snprintf(dir, sizeof dir, "%s", folder);
        if (!pick_random(dir, history_len ? history[history_at] : NULL, path, sizeof path)) {
            fprintf(stderr, "music %s: no music files in %s\n", slots[slot].da, dir);
            return NULL;
        }
    } else if (!SDL_strcasecmp(set, "order")) {
        if (!pick_in_order(folder, step < 0 ? -1 : 1, path, sizeof path)) {
            fprintf(stderr, "music %s: no music files in %s\n", slots[slot].da, folder);
            return NULL;
        }
    } else if (slot == mod_slot() && (!*set || !SDL_strcasecmp(set, "default"))) {
        snprintf(path, sizeof path, "%s", mod_arena_music()); /* (a mod arena's own music file) */
    } else {
        in_folder(set, path, sizeof path);
    }
    if (step >= 0 && music_slot_changes(slot))
        remember(path);
    return decode(slot, path, frames);
}

short *music_load(int slot, size_t *frames) { return load_stepped(slot, 0, frames); }
short *music_load_next(int slot, int step, size_t *frames) { return load_stepped(slot, step, frames); }

/* Even out song volumes: a music file's loudness (the 90th percentile of
 * its 0.4 s windows' RMS, so quiet intros do not count) is brought to about
 * the game's own music's (its CD tracks measure 5400-8100), a little above, the gain
 * kept within 0.3..3, and peaks above 28000 rounded off instead of clipped. */
#define LEVEL_TARGET 9500.0 /* a little above the CD tracks: music over the effects */
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static void level_song(short *pcm, size_t frames, const char *path)
{
    const size_t win = 44100 * 4 / 10;
    size_t n = frames / win;
    if (n < 4) return;
    double *rms = malloc(n * sizeof *rms);
    if (!rms) return;
    for (size_t w = 0; w < n; w++) {
        double sum = 0;
        const short *p = pcm + w * win * 2;
        for (size_t i = 0; i < win * 2; i += 4) sum += (double)p[i] * p[i];
        rms[w] = sqrt(sum / (win / 2.0));
    }
    qsort(rms, n, sizeof *rms, cmp_double);
    double loud = rms[n * 9 / 10];
    free(rms);
    if (loud < 1) return;
    double gain = LEVEL_TARGET / loud;
    if (gain > 3) gain = 3;
    if (gain < 0.3) gain = 0.3;
    if (fabs(gain - 1) < 0.05) return;
    const double knee = 28000, room = 32767 - knee;
    for (size_t i = 0; i < frames * 2; i++) {
        double x = pcm[i] * gain, a = fabs(x);
        if (a > knee) a = knee + room * tanh((a - knee) / room);
        pcm[i] = (short)(x < 0 ? -a : a);
    }
    fprintf(stderr, "music: %s leveled (gain %.2f)\n", path, gain);
}

static short *decode(int slot, const char *path, size_t *frames)
{

    SDL_AudioSpec spec;
    Uint8 *data = NULL, *out = NULL;
    Uint32 len = 0;
    int out_len = 0, ch = 0, rate = 0;
    size_t n = 0;
    short *decoded = music_decode(path, &ch, &rate, &n);
    if (decoded) { /* MP3, FLAC, Ogg Vorbis */
        spec = (SDL_AudioSpec){SDL_AUDIO_S16, ch, rate};
        data = (Uint8 *)decoded;
        len = (Uint32)(n * (size_t)ch * sizeof(short));
    } else if (!SDL_LoadWAV(path, &spec, &data, &len)) {
        fprintf(stderr, "music %s: cannot load %s: %s\n", slots[slot].da, path, SDL_GetError());
        return NULL;
    }
    const SDL_AudioSpec want = {SDL_AUDIO_S16, 2, 44100};
    int ok = SDL_ConvertAudioSamples(&spec, data, (int)len, &want, &out, &out_len);
    if (decoded) free(decoded);
    else SDL_free(data);
    if (!ok || out_len < 4) {
        fprintf(stderr, "music %s: cannot convert %s: %s\n", slots[slot].da, path, SDL_GetError());
        SDL_free(out);
        return NULL;
    }
    short *pcm = malloc((size_t)out_len);
    if (pcm) memcpy(pcm, out, (size_t)out_len);
    SDL_free(out);
    *frames = (size_t)out_len / 4;
    if (M.trace) fprintf(stderr, "music %s: %s (%.1f s)\n", slots[slot].da, path, *frames / 44100.0);
    if (pcm && settings_game.level_songs) level_song(pcm, *frames, path);
    return pcm;
}
