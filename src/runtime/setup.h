/* The first-time setup (docs/FIRST_RUN_SETUP.md): before the game's window, the menu pictures (ui/) made from the
 * player's own discs by the builder (assets/uibuild.c). */
#pragma once
#include <stddef.h>

/* When ui/ was not made by this builder (or forced: the Options row "Game discs"), the setup window: the PS1 disc and
 * the PC version, then the build. disc: the PS1 disc known so far (in), the one chosen (out). 1: the game goes on;
 * 0: the player closed the window without pictures (the game quits). */
int setup_pregame(const char *base, int forced, char *disc, size_t cap);

/* The Options row: the game starts again, into the setup window (Windows; 0 if it could not) */
int setup_restart(void);
