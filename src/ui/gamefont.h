/* The game's in-battle fonts (UAFONT1.TIM and its glyph tables, from
 * ui/gamefont.bmp and .txt; tools/art/ui_assets.py), for screens that look
 * like the game's own (the pause menu). gamefont.c. */
#pragma once
#include <stdint.h>
#include "draw.h"

/* Loads ui/gamefont.bmp and .txt from folder; 0 if they are missing. */
int gfont_load(const char *folder);
int gfont_ready(int font);
/* The text's width in the game's pixels (as fontStringWidth). */
int gfont_width(int font, const char *text);
/* Draws text with its left at x and its baseline (the game's cursor y) at y,
 * each game pixel scale wide, tinted as the game tints the font (color / 128). */
void gfont_draw(Image *dst, int font, const char *text, float x, float y, float scale, const uint8_t color[3]);
