/* Compressed music formats for custom music (music.c): MP3 and FLAC with
 * dr_mp3 / dr_flac (third_party/dr_libs), Ogg Vorbis with stb_vorbis
 * (third_party/stb). All three are public domain. */
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include "decoders.h"

static short *copy_out(const short *src, size_t samples)
{
    short *out = malloc(samples * sizeof *out);
    if (out) memcpy(out, src, samples * sizeof *out);
    return out;
}

short *music_decode(const char *path, int *channels, int *rate, size_t *frames)
{
    const char *ext = SDL_strrchr(path, '.');
    if (!ext)
        return NULL;
    short *out = NULL;
    if (!SDL_strcasecmp(ext, ".mp3")) {
        drmp3_config cfg = {0};
        drmp3_uint64 n = 0;
        drmp3_int16 *p = drmp3_open_file_and_read_pcm_frames_s16(path, &cfg, &n, NULL);
        if (p && n && cfg.channels) {
            out = copy_out(p, (size_t)n * cfg.channels);
            *channels = (int)cfg.channels;
            *rate = (int)cfg.sampleRate;
            *frames = (size_t)n;
        }
        drmp3_free(p, NULL);
    } else if (!SDL_strcasecmp(ext, ".flac")) {
        unsigned ch = 0, sr = 0;
        drflac_uint64 n = 0;
        drflac_int16 *p = drflac_open_file_and_read_pcm_frames_s16(path, &ch, &sr, &n, NULL);
        if (p && n && ch) {
            out = copy_out(p, (size_t)n * ch);
            *channels = (int)ch;
            *rate = (int)sr;
            *frames = (size_t)n;
        }
        drflac_free(p, NULL);
    } else if (!SDL_strcasecmp(ext, ".ogg")) {
        int ch = 0, sr = 0;
        short *p = NULL;
        int n = stb_vorbis_decode_filename(path, &ch, &sr, &p);
        if (p && n > 0 && ch > 0) {
            out = copy_out(p, (size_t)n * (size_t)ch);
            *channels = ch;
            *rate = sr;
            *frames = (size_t)n;
        }
        free(p);
    }
    return out;
}
