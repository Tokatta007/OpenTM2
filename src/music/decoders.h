/* Compressed music formats (decoders.c). */
#pragma once
#include <stddef.h>

/* Decodes a whole .mp3, .flac or .ogg file to interleaved 16-bit samples
 * (free with free()), chosen by the file's contents. NULL with *why NULL:
 * a WAV file (or another extension), for SDL_LoadWAV; NULL with *why set: the
 * file cannot be played, *why says why. */
short *music_decode(const char *path, int *channels, int *rate, size_t *frames, const char **why);
