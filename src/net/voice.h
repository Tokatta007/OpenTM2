/* Voice chat (docs/ONLINE_PLAN.md 4): Opus at 48 kHz mono, 20 ms frames (as OpenCE). A machine encodes its microphone
 * and sends each frame through the host (session.c: never resent, never part of the game); each listener keeps a
 * small jitter buffer per speaker and decodes, covering a lost frame with Opus's concealment. */
#ifndef TM2_NET_VOICE_H
#define TM2_NET_VOICE_H

#include <stdint.h>

enum {
    VOICE_RATE = 48000,
    VOICE_FRAME = 960,          /* 20 ms */
    VOICE_BITRATE = 16000,      /* bits a second per speaker */
    VOICE_PACKET_MAX = 400,     /* an encoded frame's largest size (bytes) */
    VOICE_BUFFER = 16,          /* frames a speaker's jitter buffer holds */
    VOICE_START = 3,            /* frames buffered before playing (60 ms) */
};

typedef struct VoiceEnc VoiceEnc;
typedef struct VoiceDec VoiceDec;

VoiceEnc *voice_enc_new(void);
void voice_enc_free(VoiceEnc *e);
int voice_encode(VoiceEnc *e, const int16_t pcm[VOICE_FRAME], uint8_t *out, int cap); /* bytes (<= 0: failed) */

VoiceDec *voice_dec_new(void);
void voice_dec_free(VoiceDec *d);
void voice_dec_put(VoiceDec *d, uint16_t seq, const uint8_t *data, int len); /* a frame that arrived */
/* the next 20 ms to play: 1 decoded (or concealed: a frame lost), 0 silence (not talking) */
int voice_dec_pull(VoiceDec *d, int16_t pcm[VOICE_FRAME]);
int voice_dec_talking(const VoiceDec *d); /* (the lobby's microphone icon) */

typedef struct { int decoded, concealed, late, dropped; } VoiceStats;
const VoiceStats *voice_dec_stats(const VoiceDec *d);

#endif
