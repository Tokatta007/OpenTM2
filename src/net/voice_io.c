/* Voice chat in the game (docs/ONLINE_PLAN.md 4; the codec: voice.c): this PC's microphone (Audio: the device, push
 * to talk or open mic, the open mic's microphone gate), each 20 ms encoded and sent through the server's session (the
 * lobby's: it lasts through the lobby and every match); each speaker's frames decoded and played on an audio stream of
 * their own at the voice volume. A machine this PC muted is dropped as it comes. The Audio page's mic test reads the
 * same microphone for its meter. Nothing here is part of the game's state. */
#include "voice_io.h"
#include "voice.h"
#include "lobby.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { OPEN_HOLD_MS = 400 }; /* open mic: still sending this long after the voice drops (the ends of words) */

static SDL_AudioStream *mic;       /* the microphone (open while it is needed) */
static char mic_name[128];         /* the device it was opened for ("": the default) */
static VoiceEnc *enc;
static uint16_t seq;
static int talking_here;           /* this PC sending now */
static uint32_t loud_ms;           /* open mic: the last frame over the microphone gate */
static float level;                /* the last frame's loudness (0..1, the meter's) */
static int testing;                /* the Audio page's mic test */
static unsigned muted;             /* (bits by machine) */
static struct {
    VoiceDec *dec;
    SDL_AudioStream *out;
    uint32_t heard_ms;             /* its last frame (the talking marks) */
} spk[SES_MAX_NODES];
static Lobby *bound;               /* the lobby whose voice comes here */

static const SDL_AudioSpec SPEC = {SDL_AUDIO_S16, 1, VOICE_RATE};

static void on_voice(void *ctx, int speaker, uint16_t s, const uint8_t *data, int len)
{
    (void)ctx;
    if (speaker < 0 || speaker >= SES_MAX_NODES || ((muted >> speaker) & 1)) return; /* (muted: dropped) */
    if (!spk[speaker].dec && !(spk[speaker].dec = voice_dec_new())) return;
    voice_dec_put(spk[speaker].dec, s, data, len);
    spk[speaker].heard_ms = (uint32_t)SDL_GetTicks();
}

static void mic_close(void)
{
    if (mic) SDL_DestroyAudioStream(mic);
    mic = NULL;
    level = 0;
}
static SDL_AudioDeviceID mic_device(void) /* the chosen microphone, or the default (not found: the default too) */
{
    if (!settings_game.mic[0]) return SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&n), found = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    for (int i = 0; ids && i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (name && !strcmp(name, settings_game.mic)) found = ids[i];
    }
    SDL_free(ids);
    return found;
}
static void mic_open(void)
{
    if (mic && strcmp(mic_name, settings_game.mic)) mic_close(); /* (another device chosen) */
    if (mic) return;
    mic = SDL_OpenAudioDeviceStream(mic_device(), &SPEC, NULL, NULL);
    snprintf(mic_name, sizeof mic_name, "%s", settings_game.mic);
    if (mic) SDL_ResumeAudioStreamDevice(mic);
    else fprintf(stderr, "voice: no microphone (%s)\n", SDL_GetError());
}
static float loudness(const int16_t *pcm) /* 0..1 on a decibel scale: -60 dB (silence) to 0 */
{
    double sum = 0;
    for (int i = 0; i < VOICE_FRAME; i++) sum += (double)pcm[i] * pcm[i];
    double rms = sqrt(sum / VOICE_FRAME) / 32768.0, db = rms > 0 ? 20 * log10(rms) : -60;
    return db <= -60 ? 0.f : (float)((db + 60) / 60);
}
float voice_io_open_threshold(void) /* open mic: the loudness a frame needs (the microphone gate: 1 the quietest) */
{
    return 0.22f + 0.04f * (float)settings_game.mic_gate; /* (1: 0.26, about -44 dB; 10: 0.62, -23 dB) */
}

void voice_io_frame(Lobby *l, int push_to_talk, unsigned muted_machines)
{
    muted = muted_machines;
    if (l != bound) { /* (another server, or none: every speaker's stream closed) */
        voice_io_close();
        bound = l;
        if (l) lobby_on_voice(l, on_voice, NULL);
    }
    int in = l && lobby_status(l) == LOBBY_IN && push_to_talk >= 0; /* (push_to_talk -1: the server's voice is off) */
    int open_mic = settings_game.voice_mode == 1;
    int want = testing || (in && (open_mic || push_to_talk));
    if (!want) { mic_close(); talking_here = 0; }
    else mic_open();
    /* this PC's microphone, 20 ms at a time: its loudness (the meter), sent while push to talk is held, or (open
     * mic) while the voice is loud enough and a moment after */
    if (mic && !enc) enc = voice_enc_new();
    int16_t pcm[VOICE_FRAME];
    uint8_t pkt[VOICE_PACKET_MAX];
    uint32_t now = (uint32_t)SDL_GetTicks();
    while (mic && enc && SDL_GetAudioStreamAvailable(mic) >= (int)sizeof pcm) {
        if (SDL_GetAudioStreamData(mic, pcm, (int)sizeof pcm) != (int)sizeof pcm) break;
        level = loudness(pcm);
        if (level >= voice_io_open_threshold()) loud_ms = now ? now : 1;
        int send = in && (open_mic ? loud_ms && now - loud_ms < OPEN_HOLD_MS : push_to_talk);
        if (!send) continue;
        int n = voice_encode(enc, pcm, pkt, (int)sizeof pkt);
        if (n > 0) lobby_voice_send(l, seq++, pkt, n);
    }
    if (mic && in) talking_here = open_mic ? loud_ms && now - loud_ms < OPEN_HOLD_MS : push_to_talk;
    else talking_here = 0;
    /* every speaker: what came in, played at the voice volume (a few frames queued ahead: the jitter's room) */
    for (int n = 0; n < SES_MAX_NODES; n++) {
        if (!spk[n].dec) continue;
        if (!spk[n].out) {
            spk[n].out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &SPEC, NULL, NULL);
            if (spk[n].out) SDL_ResumeAudioStreamDevice(spk[n].out);
        }
        if (!spk[n].out) continue;
        SDL_SetAudioStreamGain(spk[n].out, (float)settings_game.voice_volume / 100.f);
        while (SDL_GetAudioStreamQueued(spk[n].out) < 3 * (int)sizeof pcm && voice_dec_pull(spk[n].dec, pcm))
            SDL_PutAudioStreamData(spk[n].out, pcm, (int)sizeof pcm);
    }
}

void voice_io_mic_test(int on)
{
    testing = on != 0;
    if (!bound) voice_io_frame(NULL, 0, 0); /* (no server: the test's microphone read here) */
}
float voice_io_mic_level(void) { return level; }
int voice_io_mics(char names[][128], int max)
{
    int n = 0, out = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&n);
    for (int i = 0; ids && i < n && out < max; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (name) snprintf(names[out++], 128, "%s", name);
    }
    SDL_free(ids);
    return out;
}

int voice_io_talking(Lobby *l, int node)
{
    if (!l || node < 0 || node >= SES_MAX_NODES) return 0;
    if (node == lobby_node(l)) return talking_here;
    if ((muted >> node) & 1) return 0;
    return spk[node].dec && (voice_dec_talking(spk[node].dec) || (uint32_t)SDL_GetTicks() - spk[node].heard_ms < 300);
}

void voice_io_close(void)
{
    if (!testing) mic_close();
    talking_here = 0;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        if (spk[n].out) SDL_DestroyAudioStream(spk[n].out);
        voice_dec_free(spk[n].dec);
        memset(&spk[n], 0, sizeof spk[n]);
    }
    bound = NULL;
}
