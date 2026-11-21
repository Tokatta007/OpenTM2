/* A mod's sounds (MODDING.md 4.1: a character's engine.wav and special.wav): read from WAV and encoded as the SPU's
 * ADPCM, which the port plays from a bank of its own (vplus/mod_sounds.c, PSY-Z patch 22). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mods.h"

static unsigned u16le(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long u32le(const unsigned char *p) { return u16le(p) | (unsigned long)u16le(p + 2) << 16; }

/* PCM, 8 or 16 bits, mono or stereo (its sides mixed), 8000 to 48000 Hz, at most max_seconds long */
int mod_wav_read(const char *path, ModWav *w, int max_seconds, char *why, int why_size)
{
    memset(w, 0, sizeof *w);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, (size_t)why_size, "cannot be read"); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = n > 12 && n < 64L * 1024 * 1024 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); snprintf(why, (size_t)why_size, "cannot be read"); return 0; }
    fclose(f);
    int ok = 0, channels = 0, bits = 0;
    long rate = 0;
    const unsigned char *pcm = NULL;
    unsigned long pcm_n = 0;
    if (memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { snprintf(why, (size_t)why_size, "is not a WAV file"); goto out; }
    for (long at = 12; at + 8 <= n;) {
        unsigned long size = u32le(d + at + 4);
        if (size > (unsigned long)(n - at - 8)) size = (unsigned long)(n - at - 8);
        if (!memcmp(d + at, "fmt ", 4) && size >= 16) {
            if (u16le(d + at + 8) != 1) { snprintf(why, (size_t)why_size, "is not PCM (save it as 16-bit PCM)"); goto out; }
            channels = (int)u16le(d + at + 10);
            rate = (long)u32le(d + at + 12);
            bits = (int)u16le(d + at + 22);
        } else if (!memcmp(d + at, "data", 4)) {
            pcm = d + at + 8;
            pcm_n = size;
        }
        at += 8 + (long)size + (long)(size & 1);
    }
    if (!pcm || !channels) { snprintf(why, (size_t)why_size, "has no sound in it"); goto out; }
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16)) {
        snprintf(why, (size_t)why_size, "is %d channels, %d bits: mono or stereo, 8 or 16 bits", channels, bits);
        goto out;
    }
    if (rate < 8000 || rate > 48000) { snprintf(why, (size_t)why_size, "is %ld Hz: 8000 to 48000", rate); goto out; }
    long frames = (long)(pcm_n / (unsigned long)(channels * bits / 8));
    if (frames < 28) { snprintf(why, (size_t)why_size, "is too short"); goto out; }
    if (frames > (long)max_seconds * rate) {
        snprintf(why, (size_t)why_size, "is %.1f seconds: %d at most", (double)frames / (double)rate, max_seconds);
        goto out;
    }
    w->pcm = malloc((size_t)frames * sizeof(short));
    if (!w->pcm) { snprintf(why, (size_t)why_size, "is too large"); goto out; }
    for (long i = 0; i < frames; i++) {
        int s = 0;
        for (int c = 0; c < channels; c++)
            s += bits == 16 ? (short)u16le(pcm + 2 * (i * channels + c)) : ((int)pcm[i * channels + c] - 128) << 8;
        w->pcm[i] = (short)(s / channels);
    }
    w->frames = (int)frames;
    w->rate = (int)rate;
    ok = 1;
out:
    free(d);
    return ok;
}
void mod_wav_free(ModWav *w) { free(w->pcm); memset(w, 0, sizeof *w); }

/* ---- the SPU's ADPCM: 16-byte blocks of 28 samples (shift and filter, flags, 14 bytes of 4-bit values); decoded as
 * PSY-Z's spu_adpcm_decode_block does, s = t << (12 - shift) + (s1 * f0 >> 6) + (s2 * f1 >> 6), so each block is
 * encoded against the decoder's own history ---- */
static const int filter_pos[5] = {0, 60, 115, 98, 122}, filter_neg[5] = {0, 0, -52, -55, -60};
static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static long encode_try(const short *x, int filter, int shift, short *h1, short *h2, unsigned char *out)
{
    long err = 0;
    short p1 = *h1, p2 = *h2;
    for (int i = 0; i < 28; i++) {
        int pred = (p1 * filter_pos[filter] >> 6) + (p2 * filter_neg[filter] >> 6);
        int step = 1 << (12 - shift), d = x[i] - pred;
        int t = (d >= 0 ? d + step / 2 : d - step / 2) / step;
        t = t < -8 ? -8 : t > 7 ? 7 : t;
        int s = clamp16(t * step + pred);
        err += (long)(s - x[i]) * (s - x[i]) / 64;
        if (out) out[i / 2] |= (unsigned char)((t & 15) << (i & 1 ? 4 : 0));
        p2 = p1;
        p1 = (short)s;
    }
    *h1 = p1;
    *h2 = p2;
    return err;
}
/* the whole sound into out (blocks * 16 bytes, the caller's: mod_adpcm_size), looped from its start or played once */
int mod_adpcm_size(int frames) { return (frames + 27) / 28 * 16; }
void mod_adpcm_encode(const short *pcm, int frames, int loop, unsigned char *out)
{
    short h1 = 0, h2 = 0;
    int blocks = (frames + 27) / 28;
    for (int b = 0; b < blocks; b++) {
        short x[28];
        for (int i = 0; i < 28; i++) x[i] = b * 28 + i < frames ? pcm[b * 28 + i] : 0;
        int best_f = 0, best_s = 0;
        long best = -1;
        for (int f = 0; f < 5; f++)
            for (int s = 0; s <= 12; s++) {
                short t1 = h1, t2 = h2;
                long e = encode_try(x, f, s, &t1, &t2, NULL);
                if (best < 0 || e < best) best = e, best_f = f, best_s = s;
            }
        unsigned char *o = out + 16 * b;
        memset(o, 0, 16);
        o[0] = (unsigned char)(best_s | best_f << 4);
        encode_try(x, best_f, best_s, &h1, &h2, o + 2);
        /* flags: 4 the loop's start (the first block), 1 the end, 2 with it: jump back to the loop's start */
        if (b == 0 && loop) o[1] |= 4;
        if (b == blocks - 1) o[1] |= loop ? 3 : 1;
    }
}
