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

/* The file is read with SDL (UTF-8 paths, so names with accents or other
 * non-English letters open on Windows) and decoded by its contents, so a file
 * whose extension does not match its format still plays. */
short *music_decode(const char *path, int *channels, int *rate, size_t *frames, const char **why)
{
    *why = NULL;
    const char *ext = SDL_strrchr(path, '.');
    if (!ext)
        return NULL;
    size_t size = 0;
    unsigned char *d = SDL_LoadFile(path, &size);
    if (!d) {
        *why = SDL_GetError();
        return NULL;
    }
    short *out = NULL;
    int is_wav = size >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4);
    int is_flac = size >= 4 && !memcmp(d, "fLaC", 4);
    int is_ogg = size >= 4 && !memcmp(d, "OggS", 4);
    int is_m4a = size >= 8 && !memcmp(d + 4, "ftyp", 4);
    int is_webm = size >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3;
    if (is_wav) {
        /* a WAV file: music.c loads it */
    } else if (is_m4a) {
        *why = "this is an AAC / M4A file with the wrong extension (convert it to MP3, FLAC, Ogg or WAV)";
    } else if (is_webm) {
        *why = "this is a WebM / Opus file with the wrong extension (convert it to MP3, FLAC, Ogg or WAV)";
    } else if (is_flac || (!is_ogg && !SDL_strcasecmp(ext, ".flac"))) {
        unsigned ch = 0, sr = 0;
        drflac_uint64 n = 0;
        drflac_int16 *p = drflac_open_memory_and_read_pcm_frames_s16(d, size, &ch, &sr, &n, NULL);
        if (p && n && ch) {
            out = copy_out(p, (size_t)n * ch);
            *channels = (int)ch;
            *rate = (int)sr;
            *frames = (size_t)n;
        } else
            *why = "not a FLAC file that can be decoded";
        drflac_free(p, NULL);
    } else if (is_ogg) {
        int ch = 0, sr = 0;
        short *p = NULL;
        int n = size <= 0x7FFFFFFF ? stb_vorbis_decode_memory(d, (int)size, &ch, &sr, &p) : -1;
        if (p && n > 0 && ch > 0) {
            out = copy_out(p, (size_t)n * (size_t)ch);
            *channels = ch;
            *rate = sr;
            *frames = (size_t)n;
        } else
            *why = "not an Ogg Vorbis file that can be decoded (Ogg Opus is not supported)";
        free(p);
    } else if (!SDL_strcasecmp(ext, ".mp3") || !SDL_strcasecmp(ext, ".ogg")) {
        drmp3_config cfg = {0};
        drmp3_uint64 n = 0;
        drmp3_int16 *p = drmp3_open_memory_and_read_pcm_frames_s16(d, size, &cfg, &n, NULL);
        if (p && n && cfg.channels) {
            out = copy_out(p, (size_t)n * cfg.channels);
            *channels = (int)cfg.channels;
            *rate = (int)cfg.sampleRate;
            *frames = (size_t)n;
        } else
            *why = "not an MP3 file that can be decoded";
        drmp3_free(p, NULL);
    }
    SDL_free(d);
    return out;
}
