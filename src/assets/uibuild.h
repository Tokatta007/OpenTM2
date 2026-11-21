/* The first-time setup's builder (docs/FIRST_RUN_SETUP.md): the port's menu pictures (ui/) from the player's PS1 disc
 * image and the 1997 PC version (its .iso or folder), as tools/art/ui_assets.py and its helpers make them. */
#pragma once

/* Written in ui/setup.txt ("version N", "pc 0|1"): raised when the pictures change, so the game makes them again */
#define UI_BUILD_VERSION 3 /* (2: emblem2.bmp; 3: no Sweet Tooth Portrait 4, the Credits picture, the user's 2026-10-30) */

typedef void (*UiBuildProgress)(int percent, const char *what, void *ctx);

/* ps1: the PS1 disc's .cue (or .bin); pc: the PC version's .iso or folder (NULL: the PS1's pictures only); out: the
 * folder to write (it is made). 1: every picture written; 0: an error (ui_build_error). */
int ui_build(const char *ps1, const char *pc, const char *out, UiBuildProgress progress, void *ctx);
const char *ui_build_error(void);
