/* STR video frame decoder (mdec.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Decodes one frame (the data of its sectors, joined) into width*height
 * RGB triplets. Returns 0 if the data is not a version 1/2 frame or the
 * bitstream is broken. */
int mdec_decode_frame(const uint8_t *frame, size_t size, int width, int height, uint8_t *rgb);
