/* Voice chat (voice.h). */
#include "voice.h"

#include <opus.h>
#include <stdlib.h>
#include <string.h>

struct VoiceEnc { OpusEncoder *op; };

struct VoiceDec {
    OpusDecoder *op;
    uint8_t data[VOICE_BUFFER][VOICE_PACKET_MAX];
    int len[VOICE_BUFFER];
    uint16_t seq[VOICE_BUFFER];
    uint8_t have[VOICE_BUFFER];
    uint16_t next;           /* the frame to play next */
    int playing;             /* playing (VOICE_START frames came in) */
    int missing;             /* frames concealed in a row (a speaker who stopped: silence after a few) */
    VoiceStats st;
};

VoiceEnc *voice_enc_new(void)
{
    VoiceEnc *e = calloc(1, sizeof *e);
    int err = 0;
    if (!e) return NULL;
    e->op = opus_encoder_create(VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) { free(e); return NULL; }
    opus_encoder_ctl(e->op, OPUS_SET_BITRATE(VOICE_BITRATE));
    opus_encoder_ctl(e->op, OPUS_SET_INBAND_FEC(1));       /* (a lost frame partly rebuilt from the next) */
    opus_encoder_ctl(e->op, OPUS_SET_PACKET_LOSS_PERC(5));
    return e;
}

void voice_enc_free(VoiceEnc *e)
{
    if (!e) return;
    opus_encoder_destroy(e->op);
    free(e);
}

int voice_encode(VoiceEnc *e, const int16_t pcm[VOICE_FRAME], uint8_t *out, int cap)
{
    return opus_encode(e->op, pcm, VOICE_FRAME, out, cap);
}

VoiceDec *voice_dec_new(void)
{
    VoiceDec *d = calloc(1, sizeof *d);
    int err = 0;
    if (!d) return NULL;
    d->op = opus_decoder_create(VOICE_RATE, 1, &err);
    if (err != OPUS_OK) { free(d); return NULL; }
    return d;
}

void voice_dec_free(VoiceDec *d)
{
    if (!d) return;
    opus_decoder_destroy(d->op);
    free(d);
}

static int buffered(const VoiceDec *d)
{
    int n = 0;
    for (int i = 0; i < VOICE_BUFFER; i++) n += d->have[i];
    return n;
}

void voice_dec_put(VoiceDec *d, uint16_t seq, const uint8_t *data, int len)
{
    if (len <= 0 || len > VOICE_PACKET_MAX) return;
    if (d->playing && (int16_t)(uint16_t)(seq - d->next) < 0) { d->st.late++; return; } /* (its time has passed) */
    if (!d->playing && !buffered(d)) d->next = seq;               /* (the first of a new talk) */
    if (!d->playing && (int16_t)(uint16_t)(seq - d->next) < 0) d->next = seq;
    if ((int16_t)(uint16_t)(seq - d->next) >= VOICE_BUFFER) { d->st.dropped++; return; } /* (too far ahead) */
    int k = seq % VOICE_BUFFER;
    memcpy(d->data[k], data, (size_t)len);
    d->len[k] = len;
    d->seq[k] = seq;
    d->have[k] = 1;
    if (!d->playing && buffered(d) >= VOICE_START) d->playing = 1;
}

int voice_dec_pull(VoiceDec *d, int16_t pcm[VOICE_FRAME])
{
    if (!d->playing) {
        memset(pcm, 0, sizeof(int16_t) * VOICE_FRAME);
        return 0;
    }
    int k = d->next % VOICE_BUFFER, n;
    if (d->have[k] && d->seq[k] == d->next) {
        n = opus_decode(d->op, d->data[k], d->len[k], pcm, VOICE_FRAME, 0);
        d->have[k] = 0;
        d->missing = 0;
        d->st.decoded++;
    } else {
        int j = (d->next + 1) % VOICE_BUFFER; /* the next frame here: this one rebuilt from its in-band copy */
        if (d->have[j] && d->seq[j] == (uint16_t)(d->next + 1)) n = opus_decode(d->op, d->data[j], d->len[j], pcm, VOICE_FRAME, 1);
        else n = opus_decode(d->op, NULL, 0, pcm, VOICE_FRAME, 0);
        d->missing++;
        d->st.concealed++;
    }
    if (n < VOICE_FRAME) memset(pcm + (n > 0 ? n : 0), 0, sizeof(int16_t) * (size_t)(VOICE_FRAME - (n > 0 ? n : 0)));
    d->next++;
    if (d->missing >= 5 || (d->missing && !buffered(d))) { /* (the talk ended, or a long gap: wait for the next) */
        d->playing = 0;
        d->missing = 0;
        for (int i = 0; i < VOICE_BUFFER; i++) d->have[i] = 0;
    }
    return 1;
}

int voice_dec_talking(const VoiceDec *d) { return d->playing; }

const VoiceStats *voice_dec_stats(const VoiceDec *d) { return &d->st; }
