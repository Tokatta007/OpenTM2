/* Compressed music formats (decoders.c). */
#pragma once
#include <stddef.h>

/* Decodes a whole .mp3, .flac or .ogg file to interleaved 16-bit samples
 * (free with free()). NULL if the extension is not one of these or the file
 * does not decode. */
short *music_decode(const char *path, int *channels, int *rate, size_t *frames);
