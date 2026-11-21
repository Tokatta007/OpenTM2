/* The port's own menu pages (Options), drawn over the game at the window's
 * full resolution. */
#pragma once

/* Where the pictures (the .bmp files in ui/, from tools/art/ui_assets.py) and the font
 * (fonts/) are: the executable's folder, with its trailing separator. */
void ui_set_folder(const char *base);

/* Once per VBlank (hle.c): opens the Options pages when F1 (or a gamepad's
 * touchpad or guide button) is pressed, anywhere, e.g. during a match.
 * While they are open the game is stopped: this returns when they are
 * closed (backing out with O). */
void ui_vblank(void);

/* Draws one page into a BMP without running the game, for checking the
 * layout (--ui-shot): page 0 Options, 1 Gameplay, 2 Video, 3 Audio,
 * 4 Controls, 5 Vanilla Plus, 6 Custom Music, 7 Controllers; sel is the
 * chosen row. Returns 0 on failure. */
int ui_shot(int page, int sel, int w, int h, const char *bmp_path);
int ui_battle_live(void); /* a Challenge Match started from the Challenge Match pages */
int ui_survival(void);    /* that match is a Survival run */
int ui_mute_game_sounds(void); /* the live car select handing its picks to the game */
