/* CD audio (Red Book) playback from the disc image into the SPU's CD input.
 *
 * TM2 starts music with CdlSetmode(CDDA) and CdlPlay at an absolute disc
 * position (the .DA files in the file system point into the audio
 * tracks). Playback reads raw 2352-byte sectors (588 stereo 16-bit frames
 * at 44.1 kHz) from the track's .bin. PSY-Z's SPU pulls them through
 * Psyz_CdPullSamples on the audio thread and applies the game's CD volume
 * registers. At the end of a track, playback loops back to where the game
 * started it (TM2 also loops itself from the positions it is reported).
 * A track can be replaced by a music file (music.c): it is decoded whole
 * and loops, and the reported position stays inside the disc track. */
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include <psyz/audio.h>
#include <psyz/spu.h>
#include <SDL3/SDL.h>
#include "music/music.h"

static struct {
    int playing, muted;
    uint32_t start, pos, end;   /* LBAs: loop point, next sector, end of track */
    int track;
    FILE *file;
    uint32_t file_start;
    short buf[588 * 2];          /* current sector's frames */
    int buf_frames, buf_pos;
    /* custom music: a whole file as 44.1 kHz stereo, looping */
    int custom, slot;
    short *pcm;
    size_t pcm_frames, pcm_pos;
    uint32_t custom_start;       /* where the game started the track */
} cda;

int cd_audio_stop(void);
static volatile int held; /* (below) */

static int track_of(uint32_t lba)
{
    for (int i = 0; i < cd_track_count; i++)
        if (lba >= cd_tracks[i].file_start && lba < cd_tracks[i].file_start + cd_tracks[i].sectors)
            return i;
    return -1;
}

/* A new start: the CD audio the SPU has read ahead from the old position is
 * dropped (or its last moments would be heard first), and the new music
 * fades in over a few milliseconds instead of starting with a click.
 * Audio lock held. */
#define FADE_IN_FRAMES 256
static int fade_in;
/* and out, before a movie: the music's sudden stop popped (the user's: into the tournament's ending) */
#define FADE_OUT_FRAMES 4410 /* 0.1 s */
static int fade_out;

static void restart_output(void)
{
    Psyz_SpuCdFlush();
    fade_in = FADE_IN_FRAMES;
}

/* A music file's start. Decoding (and leveling) a song takes 0.3-0.4 s,
 * which froze the game as each battle began. The file is decoded on a thread
 * and swapped in at a VBlank (cd_audio_skip_vblank), fading in like any new
 * start; until then the old music goes on (as it did during the decode) and
 * the position reported is the new track's start, so the game does not
 * restart it. A newer start or a disc track drops a pending one (its
 * generation); a stop while pending keeps it, stopped, for the resume. */
typedef struct PlayJob {
    int slot, track;
    uint32_t lba;
    int gen;
    short *pcm;
    size_t frames;
} PlayJob;
static SDL_Mutex *load_mutex;   /* music.c's loads one at a time (the starts' and the skip's) */
static SDL_Mutex *job_mutex;    /* ready */
static PlayJob *ready;          /* a finished start of the current generation */
static SDL_AtomicInt play_gen;
static struct { int active, slot, stopped; uint32_t lba; } pend; /* game thread */

static void make_mutexes(void)
{
    if (!load_mutex) load_mutex = SDL_CreateMutex();
    if (!job_mutex) job_mutex = SDL_CreateMutex();
}

static int SDLCALL play_thread(void *data)
{
    PlayJob *j = data;
    if (load_mutex) SDL_LockMutex(load_mutex);
    if (SDL_GetAtomicInt(&play_gen) == j->gen) /* (not dropped while waiting) */
        j->pcm = music_load(j->slot, &j->frames);
    if (load_mutex) SDL_UnlockMutex(load_mutex);
    PlayJob *drop = j;
    SDL_LockMutex(job_mutex);
    if (SDL_GetAtomicInt(&play_gen) == j->gen) {
        drop = ready;
        ready = j;
    }
    SDL_UnlockMutex(job_mutex);
    if (drop) {
        free(drop->pcm);
        free(drop);
    }
    return 0;
}

static void play_disc(int t, uint32_t lba)
{
    Psyz_AudioLock();
    cda.custom = 0;
    if (t != cda.track || !cda.file) {
        if (cda.file) fclose(cda.file);
        cda.file = fopen(cd_tracks[t].path, "rb");
        cda.track = t;
        cda.file_start = cd_tracks[t].file_lba; /* (where the .bin starts: a whole-disc .bin holds every track) */
    }
    cda.start = cda.pos = lba;
    cda.end = cd_tracks[t].file_start + cd_tracks[t].sectors;
    cda.buf_frames = cda.buf_pos = 0;
    cda.playing = cda.file != 0;
    restart_output();
    Psyz_AudioUnlock();
    if (M.trace) fprintf(stderr, "CD audio: track %d from sector %u\n", t + 1, lba);
}

static void install_custom(short *pcm, size_t frames, int slot, uint32_t lba, int play)
{
    Psyz_AudioLock();
    short *old = cda.pcm;
    cda.pcm = pcm;
    cda.pcm_frames = frames;
    cda.pcm_pos = 0;
    cda.slot = slot;
    cda.custom_start = lba;
    cda.custom = 1;
    cda.playing = play;
    restart_output();
    Psyz_AudioUnlock();
    free(old);
}

/* Game thread, each VBlank: a finished start swapped in. */
static void play_vblank(void)
{
    if (!pend.active || !job_mutex)
        return;
    SDL_LockMutex(job_mutex);
    PlayJob *j = ready;
    ready = NULL;
    SDL_UnlockMutex(job_mutex);
    if (!j)
        return;
    if (j->gen == SDL_GetAtomicInt(&play_gen)) {
        pend.active = 0;
        if (getenv("TM2PC_MUSIC_DEBUG"))
            fprintf(stderr, "music: custom slot %d decoded (%s, vblank %llu; the old music %s)\n", j->slot, j->pcm ? (pend.stopped ? "stopped" : "playing") : "failed",
                    (unsigned long long)M.vblanks, cda.playing ? "still playing" : "silent");
        if (j->pcm) {
            install_custom(j->pcm, j->frames, j->slot, j->lba, !pend.stopped);
            j->pcm = NULL;
        } else if (!pend.stopped) {
            play_disc(j->track, j->lba); /* the file could not be loaded: the disc track plays */
        }
    }
    free(j->pcm);
    free(j);
}

/* Game thread. */
void cd_audio_play(uint32_t lba)
{
    if (getenv("TM2PC_NO_CDDA")) /* debugging: sound effects only */
        return;
    int t = track_of(lba);
    /* The game starts a track a little before its INDEX 01, in the 2-second pregap every audio track has on the disc.
     * An image that left the pregap out (a PREGAP line in its .cue, no INDEX 00, or one .bin with no gap) puts that
     * place in the track before it, or in the data: there it is this track's start (else stage music stays silent, and
     * the title song lasts a moment so the attract demo starts at once). An image with its pregaps has it already. */
    for (int i = 1; i < cd_track_count; i++)
        if (i != t && cd_tracks[i].audio && lba < cd_tracks[i].start && lba + 150 >= cd_tracks[i].start) {
            t = i;
            if (lba < cd_tracks[i].file_start) lba = cd_tracks[i].file_start;
            break;
        }
    if (t < 1 || !cd_tracks[t].audio) {
        cd_audio_stop();
        rt_unexpected("CD play outside an audio track", lba, (uint32_t)t);
        return;
    }
    /* Custom music (music.c). The game starts a track from its first whole
     * second; a later position is a resume after the pause menu, which
     * continues the file where it stopped. */
    t = music_remap(t, &lba); /* (a mod arena: another arena's track) */
    int slot = music_slot(t);
    if (getenv("TM2PC_MUSIC_DEBUG")) /* tests: each music start, its disc track and custom slot */
        fprintf(stderr, "music: disc track %d from sector %u, custom slot %d (old: playing %d custom %d slot %d held %d muted %d pend %d, vblank %llu)\n", t + 1, lba, slot,
                cda.playing, cda.custom, cda.slot, held, cda.muted, pend.active, (unsigned long long)M.vblanks);
    if (slot >= 0 && pend.active && pend.slot == slot && lba >= pend.lba) { /* its start, still decoding */
        pend.stopped = 0;
        return;
    }
    if (slot >= 0 && !pend.active && cda.pcm && cda.slot == slot && lba > cda.custom_start) {
        Psyz_AudioLock();
        cda.custom = cda.playing = 1;
        Psyz_AudioUnlock();
        return;
    }
    SDL_AddAtomicInt(&play_gen, 1); /* drops a pending start */
    pend.active = 0;
    if (slot >= 0) {
        /* Another song than the one playing: it stops now, not when the new one is decoded. The
         * game asks for the next arena's track with the last one still playing (never stopped:
         * a tournament's levels too), and for the 0.3 s of the decode a moment of the last arena's
         * song was heard as the next battle began (the user's, 2026-11-19). The same slot again
         * goes on until its swap, as before. */
        if (!cda.custom || cda.slot != slot) {
            Psyz_AudioLock();
            if (cda.playing) {
                cda.playing = 0;
                restart_output(); /* (what the SPU read ahead of it: dropped too) */
            }
            Psyz_AudioUnlock();
        }
        make_mutexes();
        PlayJob *j = calloc(1, sizeof *j);
        SDL_Thread *th = NULL;
        if (j && job_mutex) {
            j->slot = slot;
            j->track = t;
            j->lba = lba;
            j->gen = SDL_GetAtomicInt(&play_gen);
            pend.active = 1;
            pend.slot = slot;
            pend.lba = lba;
            pend.stopped = 0;
            th = SDL_CreateThread(play_thread, "music start", j);
        }
        if (th) {
            SDL_DetachThread(th);
            return;
        }
        pend.active = 0;
        free(j);
        size_t frames = 0; /* no thread: decoded here, as before */
        short *pcm = music_load(slot, &frames);
        if (pcm) {
            install_custom(pcm, frames, slot, lba, 1);
            return;
        }
        /* the file could not be loaded: the disc track plays */
    }
    play_disc(t, lba);
}

/* A custom music slot played by itself (no disc track: the Versus podium's song), looping. 0: it could not be loaded. */
int cd_audio_play_slot(int slot)
{
    size_t frames = 0;
    short *pcm = music_load(slot, &frames);
    if (!pcm) return 0;
    SDL_AddAtomicInt(&play_gen, 1); /* (a pending start dropped) */
    pend.active = 0;
    install_custom(pcm, frames, slot, 0, 1);
    return 1;
}

/* Stop (or pause): the position is kept for cd_audio_resume. */
int cd_audio_stop(void)
{
    if (M.trace || getenv("TM2PC_MUSIC_DEBUG")) fprintf(stderr, "CD audio: stop (vblank %llu)\n", (unsigned long long)M.vblanks);
    Psyz_AudioLock();
    int was = cda.playing || (pend.active && !pend.stopped);
    cda.playing = 0;
    fade_out = 0;
    Psyz_AudioUnlock();
    pend.stopped = 1;
    return was;
}
/* the music faded out over a tenth of a second (the audio thread's), then stopped */
void cd_audio_fade_stop(void)
{
    Psyz_AudioLock();
    int playing = cda.playing;
    if (playing) fade_out = FADE_OUT_FRAMES;
    Psyz_AudioUnlock();
    for (int i = 0; playing && i < 200; i++) { /* (at most 0.2 s) */
        Psyz_AudioLock();
        int left = fade_out;
        Psyz_AudioUnlock();
        if (left <= 0) break;
        SDL_Delay(1);
    }
    cd_audio_stop();
}

/* Play after Pause: continue where the music stopped, same loop point. */
void cd_audio_resume(void)
{
    if (getenv("TM2PC_MUSIC_DEBUG")) fprintf(stderr, "CD audio: resume (pend %d custom %d slot %d, vblank %llu)\n", pend.active, cda.custom, cda.slot, (unsigned long long)M.vblanks);
    if (pend.active) { /* a start still decoding: it plays when ready */
        pend.stopped = 0;
        return;
    }
    Psyz_AudioLock();
    cda.playing = cda.custom ? cda.pcm != 0 : cda.file != 0;
    Psyz_AudioUnlock();
}

/* Where the drive is: the sector being played (or where it stopped). */
uint32_t cd_audio_position(void)
{
    if (pend.active) /* a start still decoding: at its start */
        return pend.lba;
    Psyz_AudioLock();
    uint32_t pos = cda.buf_frames ? cda.pos - 1 : cda.pos;
    if (cda.custom) { /* inside the track's range, so the game does not restart it */
        size_t s = cda.pcm_pos / 588;
        pos = cda.custom_start + (uint32_t)(s < 75 ? s : 75);
    }
    Psyz_AudioUnlock();
    return pos;
}

int cd_audio_playing(void)
{
    return cda.playing || (pend.active && !pend.stopped);
}

static volatile int held; /* cd_audio_hold: the CD music is silent (it keeps its place) */
static volatile int music_loops; /* times the music has reached its end and started over */
static volatile int song_ended; /* a random or order song came to its end (Next at end): the next is asked for */

/* How many times the music has played to its end (the title waits for it). */
int cd_audio_loop_count(void) { return music_loops; }
/* Whether music is playing (CD or a music file). */
int cd_audio_music_playing(void) { return cda.playing || (pend.active && !pend.stopped); }

/* The port's menus opened from the title menu: the CD music goes quiet, as
 * in the game's own submenus (which play the menu loop on the SPU). */
void cd_audio_hold(int on)
{
    if (getenv("TM2PC_MUSIC_DEBUG") && held != on) fprintf(stderr, "CD audio: hold %d (vblank %llu)\n", on, (unsigned long long)M.vblanks);
    held = on;
}

/* The pause menu: the music stops where it is (CD or a music file) and goes
 * on from there. */
static volatile int paused;
void cd_audio_pause(int on)
{
    paused = on;
}

void cd_audio_mute(int on)
{
    Psyz_AudioLock();
    cda.muted = on;
    Psyz_AudioUnlock();
}

/* XA audio of a movie (movie.c pushes it as the stream is read), 37800 Hz
 * stereo, resampled here to the SPU's 44100 Hz. While it runs it replaces
 * CD-DA, as on the drive. A mod's ending video (mods/mod_video.c) streams its
 * sound the same way at its own rate (cd_audio_xa_rate). */
#define XA_RATE 37800
#define XA_RING (48000 * 2)              /* frames: 2 s at the highest rate */
static struct {
    int active, rate;
    short ring[XA_RING][2];
    unsigned head, tail;                 /* frames written, read */
    unsigned frac;                       /* 16.16 position between tail and tail+1 */
} xa;

/* The CD music stopped (not just held): a movie about to start lets go of the hold, and the held
 * music would sound for the moment before the movie's own (the first movie's pop). */
void cd_audio_stop_music(void)
{
    pend.stopped = 1;
    Psyz_AudioLock();
    cda.playing = 0;
    Psyz_AudioUnlock();
}

void cd_audio_xa_begin(void)
{
    Psyz_AudioLock();
    cda.playing = 0;
    xa.head = xa.tail = xa.frac = 0;
    xa.rate = XA_RATE;
    xa.active = 1;
    Psyz_AudioUnlock();
}

void cd_audio_xa_rate(int rate) /* (after cd_audio_xa_begin) the pushed frames' rate, 8000..48000 */
{
    Psyz_AudioLock();
    xa.rate = rate < 8000 ? 8000 : rate > 48000 ? 48000 : rate;
    Psyz_AudioUnlock();
}

void cd_audio_xa_end(void)
{
    Psyz_AudioLock();
    xa.active = 0;
    Psyz_AudioUnlock();
}

/* Game thread: frames of interleaved stereo. Drops the oldest if full. */
void cd_audio_xa_push(const short *stereo, int frames)
{
    Psyz_AudioLock();
    for (int i = 0; i < frames; i++) {
        if (xa.head - xa.tail >= XA_RING - 1)
            xa.tail++;
        xa.ring[xa.head % XA_RING][0] = stereo[2 * i];
        xa.ring[xa.head % XA_RING][1] = stereo[2 * i + 1];
        xa.head++;
    }
    Psyz_AudioUnlock();
}

static size_t xa_pull(short *out, size_t frames)
{
    const unsigned step = (unsigned)(((uint64_t)(xa.rate ? xa.rate : XA_RATE) << 16) / 44100);
    size_t n = 0;
    for (; n < frames && xa.head - xa.tail >= 2; n++) {
        const short *a = xa.ring[xa.tail % XA_RING], *b = xa.ring[(xa.tail + 1) % XA_RING];
        int f = (int)(xa.frac >> 4); /* 12 bits */
        out[2 * n] = (short)(a[0] + (((b[0] - a[0]) * f) >> 12));
        out[2 * n + 1] = (short)(a[1] + (((b[1] - a[1]) * f) >> 12));
        xa.frac += step;
        xa.tail += xa.frac >> 16;
        xa.frac &= 0xFFFF;
    }
    return n;
}

/* Host volumes ([audio] in tm2pc.ini), 0-100. Music: CD audio and music
 * files (movies, XA, keep theirs) and the SPU voices playing the menus'
 * music loop; sound effects: every other SPU voice (PSY-Z's per-voice gain,
 * psyz-06-host-audio.patch). Both use the square of the setting, so the
 * steps sound even. */
#define MENU_MUSIC_SAMPLE 0xEF4Cu /* SPU start address register value (sample at 0x77A60) */
static volatile int music_volume = 100;
static int effects_volume = 100;

static int gain_of(int percent) { return percent * percent * 32768 / 10000; }

void audio_get_volumes(int *music, int *effects) { *music = music_volume; *effects = effects_volume; }
void audio_set_volumes(int music, int effects)
{
    music_volume = music < 0 ? 0 : music > 100 ? 100 : music;
    effects_volume = effects < 0 ? 0 : effects > 100 ? 100 : effects;
    audio_volumes_vblank();
}

/* Each VBlank (and while the port's menus are open): which voices are
 * music. */
/* A voice's own level on top of the effects volume (percent; 0: none set): car select's
 * specials, scaled by how many copies of their sample the game's sound starts (keyed_on_by) */
/* (every voice: the SPU's 24 and the port's 24 more, PSYZ_SPU_ALL_VOICES; their registers at 1F801C00, 1F802C00: hw.c) */
#define ALL_VOICES 48
#define EFFECTS_TRIM 841 /* per mille: -1.5 dB on every effect (the user's playtests: 2026-10-29 too loud beside the music, -3 dB;
                           * 2026-10-30 too much, half way back) */
static uint32_t voice_reg(int v) { return v < 24 ? 0x1F801C00u + 16u * (uint32_t)v : 0x1F802C00u + 16u * (uint32_t)(v - 24); }
static int voice_scale[ALL_VOICES];
void audio_voice_scale(int voice, int percent) { if (voice >= 0 && voice < ALL_VOICES) voice_scale[voice] = percent; }
void audio_voice_scale_clear(void) { for (int v = 0; v < ALL_VOICES; v++) voice_scale[v] = 0; }
void audio_volumes_vblank(void)
{
    static int set[ALL_VOICES];
    static int known;
    /* The menus' loop (sound 27) keys its sample on two voices; the sound driver sets the
     * second one's volume at its tick, which the port's menus don't run: there it stayed
     * silent and the loop was half as loud as in the game's screens (car select). The
     * second voice takes the first one's volume, so it is the same everywhere. */
    {
        uint32_t vl = 0, vr = 0;
        for (int v = 0; v < ALL_VOICES && !vl; v++)
            if ((rc_io_read(&M.cpu, voice_reg(v) + 6, 2) & 0xFFFF) == MENU_MUSIC_SAMPLE &&
                (rc_io_read(&M.cpu, voice_reg(v) + 0xC, 2) & 0xFFFF)) {
                vl = rc_io_read(&M.cpu, voice_reg(v) + 0, 2) & 0xFFFF;
                vr = rc_io_read(&M.cpu, voice_reg(v) + 2, 2) & 0xFFFF;
            }
        /* and at the voice's full volume (0x3FFF; the game's 0x3468): the Music volume (its gain) sets it */
        if (vl && !(vl & 0x8000)) vl = vr = 0x3FFF;
        for (int v = 0; v < ALL_VOICES && vl; v++)
            if ((rc_io_read(&M.cpu, voice_reg(v) + 6, 2) & 0xFFFF) == MENU_MUSIC_SAMPLE &&
                (rc_io_read(&M.cpu, voice_reg(v) + 0xC, 2) & 0xFFFF) &&
                (rc_io_read(&M.cpu, voice_reg(v) + 0, 2) & 0xFFFF) != vl) {
                rc_io_write(&M.cpu, voice_reg(v) + 0, vl, 2);
                rc_io_write(&M.cpu, voice_reg(v) + 2, vr, 2);
            }
    }
    static int ext_test = -1; /* (hw.c's TM2PC_SPU_EXT_TEST: voice 24 + n plays voice n, its scale too) */
    if (ext_test < 0) ext_test = getenv("TM2PC_SPU_EXT_TEST") != NULL;
    for (int v = 0; v < ALL_VOICES; v++) {
        uint32_t start = rc_io_read(&M.cpu, voice_reg(v) + 6, 2) & 0xFFFF;
        int gain = gain_of(start == MENU_MUSIC_SAMPLE ? music_volume : effects_volume);
        if (start != MENU_MUSIC_SAMPLE) gain = gain * EFFECTS_TRIM / 1000; /* (the effects 1.5 dB under the music: the user's, both full) */
        int scale = ext_test && v >= 24 ? voice_scale[v - 24] : voice_scale[v];
        if (scale && start != MENU_MUSIC_SAMPLE) gain = gain * scale / 100;
        if (!known || set[v] != gain) {
            Psyz_SpuSetVoiceGain(v, gain);
            set[v] = gain;
        }
    }
    known = 1;
    static int trace = -1, tick; /* tests: TM2PC_LOOP_TRACE=1, the menus' loop voices each half second */
    if (trace < 0) trace = getenv("TM2PC_LOOP_TRACE") != NULL;
    if (trace && ++tick % 30 == 0) {
        char line[256];
        int n = snprintf(line, sizeof line, "loop %llu:", (unsigned long long)M.vblanks);
        for (int v = 0; v < 24; v++)
            if ((rc_io_read(&M.cpu, 0x1F801C06u + (uint32_t)v * 16, 2) & 0xFFFF) == MENU_MUSIC_SAMPLE &&
                (rc_io_read(&M.cpu, 0x1F801C0Cu + (uint32_t)v * 16, 2) & 0xFFFF))
                n += snprintf(line + n, sizeof line - (size_t)n, " v%d p%04X %04X/%04X", v, rc_io_read(&M.cpu, 0x1F801C04u + (uint32_t)v * 16, 2) & 0xFFFF,
                              rc_io_read(&M.cpu, 0x1F801C00u + (uint32_t)v * 16, 2) & 0xFFFF,
                              rc_io_read(&M.cpu, 0x1F801C02u + (uint32_t)v * 16, 2) & 0xFFFF);
        fprintf(stderr, "%s gain %d\n", line, gain_of(music_volume));
    }
}

static size_t music_pull(short *out, size_t frames);

/* Audio thread (called with the audio lock held). */
size_t Psyz_CdPullSamples(short *out, size_t frames)
{
    if (xa.active)
        return xa_pull(out, frames);
    if (paused) { /* silence, the music's place kept */
        memset(out, 0, frames * 4);
        return frames;
    }
    size_t n = music_pull(out, frames);
    if (held) {
        memset(out, 0, n * 4);
        return n;
    }
    if (fade_out > 0) { /* fading out (a movie next): down to silence over FADE_OUT_FRAMES, then nothing */
        for (size_t i = 0; i < n; i++) {
            int k = fade_out > 0 ? fade_out-- : 0;
            out[i * 2] = (short)(out[i * 2] * k / FADE_OUT_FRAMES);
            out[i * 2 + 1] = (short)(out[i * 2 + 1] * k / FADE_OUT_FRAMES);
        }
        if (!fade_out) fade_out = -1; /* (done: silent until cd_audio_stop) */
        return n;
    }
    if (fade_out < 0) { memset(out, 0, n * 4); return n; }
    for (size_t i = 0; i < n && fade_in > 0; i++, fade_in--) {
        int k = FADE_IN_FRAMES - fade_in;
        out[i * 2] = (short)(out[i * 2] * k / FADE_IN_FRAMES);
        out[i * 2 + 1] = (short)(out[i * 2 + 1] * k / FADE_IN_FRAMES);
    }
    int v = music_volume;
    if (v < 100) {
        int gain = gain_of(v);
        for (size_t i = 0; i < n * 2; i++)
            out[i] = (short)((out[i] * gain) >> 15);
    }
    return n;
}

static size_t music_pull(short *out, size_t frames)
{
    size_t n = 0;
    if (cda.custom) {
        while (n < frames && cda.playing && cda.pcm_frames) {
            if (cda.pcm_pos >= cda.pcm_frames) {
                if (music_next_at_end() && music_slot_changes(cda.slot)) { /* the next song (cd_audio_skip_vblank): silence till then */
                    song_ended = 1;
                    memset(out + n * 2, 0, (frames - n) * 4);
                    n = frames;
                    break;
                }
                cda.pcm_pos = 0; /* loop */
                music_loops++;
            }
            size_t take = cda.pcm_frames - cda.pcm_pos;
            if (take > frames - n) take = frames - n;
            if (cda.muted)
                memset(out + n * 2, 0, take * 4);
            else
                memcpy(out + n * 2, cda.pcm + cda.pcm_pos * 2, take * 4);
            cda.pcm_pos += take;
            n += take;
        }
        return n;
    }
    while (n < frames && cda.playing) {
        if (cda.buf_pos >= cda.buf_frames) {
            if (cda.pos >= cda.end) {
                cda.pos = cda.start; /* loop */
                music_loops++;
            }
            uint8_t raw[2352];
            fseek(cda.file, (long)(cda.pos - cda.file_start) * 2352, SEEK_SET);
            if (fread(raw, 1, sizeof raw, cda.file) != sizeof raw) {
                cda.playing = 0;
                break;
            }
            memcpy(cda.buf, raw, sizeof raw);
            cda.buf_frames = 588;
            cda.buf_pos = 0;
            cda.pos++;
        }
        size_t take = (size_t)(cda.buf_frames - cda.buf_pos);
        if (take > frames - n) take = frames - n;
        if (cda.muted)
            memset(out + n * 2, 0, take * 4);
        else
            memcpy(out + n * 2, cda.buf + cda.buf_pos * 2, take * 4);
        cda.buf_pos += (int)take;
        n += take;
    }
    return n;
}

/* ---- next / previous song ----
 * On a track set to random or order (music.c), the next or previous file is
 * decoded on a thread, so the game does not stall, and swapped in at the
 * next VBlank, fading in like any new start. */
static SDL_AtomicInt skip_state; /* 0 idle, 1 decoding, 2 ready */
/* (song_ended, set by the audio thread at a song's end with Next at end: the VBlank asks for the next song) */
static struct { int slot, step; short *pcm; size_t frames; } skip_job;

static int SDLCALL skip_thread(void *unused)
{
    (void)unused;
    if (load_mutex) SDL_LockMutex(load_mutex);
    skip_job.pcm = music_load_next(skip_job.slot, skip_job.step, &skip_job.frames);
    if (load_mutex) SDL_UnlockMutex(load_mutex);
    SDL_SetAtomicInt(&skip_state, 2);
    return 0;
}

void cd_audio_skip(int step)
{
    if (SDL_GetAtomicInt(&skip_state))
        return;
    Psyz_AudioLock();
    int slot = cda.custom && cda.playing ? cda.slot : -1;
    Psyz_AudioUnlock();
    if (slot < 0 || !music_slot_changes(slot))
        return;
    if (pend.active) /* a start still decoding: the skip would be the old song's */
        return;
    make_mutexes();
    skip_job.slot = slot;
    skip_job.step = step;
    SDL_SetAtomicInt(&skip_state, 1);
    SDL_Thread *t = SDL_CreateThread(skip_thread, "music skip", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&skip_state, 0);
}

void cd_audio_skip_vblank(void)
{
    play_vblank();
    if (song_ended && !SDL_GetAtomicInt(&skip_state)) {
        Psyz_AudioLock();
        int still = song_ended && cda.custom && cda.playing && music_slot_changes(cda.slot);
        song_ended = 0;
        Psyz_AudioUnlock();
        if (still) { fprintf(stderr, "music: song over, the next\n"); cd_audio_skip(1); }
        return;
    }
    if (SDL_GetAtomicInt(&skip_state) != 2)
        return;
    short *old = NULL;
    if (skip_job.pcm) {
        Psyz_AudioLock();
        if (cda.custom && cda.slot == skip_job.slot) { /* still the same track playing */
            old = cda.pcm;
            cda.pcm = skip_job.pcm;
            cda.pcm_frames = skip_job.frames;
            cda.pcm_pos = 0;
            song_ended = 0;
            restart_output();
        } else {
            old = skip_job.pcm;
        }
        Psyz_AudioUnlock();
    }
    free(old);
    SDL_SetAtomicInt(&skip_state, 0);
}
