/* The Versus Tournament's podium scene (podium.c; the blocks: hud_styles.c hudstyle_podium; the flow, the banner
 * and the music: ui/options.c). */
#pragma once

#define PODIUM_LEVEL 5   /* New York */
/* the blocks' size (position units; a car is about 55 wide and 150 long: decomp request 108's stats), shared by
 * hud_styles.c (the drawing) and podium.c (the cars and the cameras) */
#define PODIUM_CELL_MIN 0.5 /* the numbers' cells: at least this much of a game pixel (hud_styles.c: coarser far away) */
#define PODIUM_BLOCK_W 170
#define PODIUM_BLOCK_D 190
#define PODIUM_BLOCK_H1 110 /* 1st */
#define PODIUM_BLOCK_H2 80  /* 2nd */
#define PODIUM_BLOCK_H3 55  /* 3rd */
/* the blocks as drawn now: the sizes above, grown when a big car stands on it (Dark Tooth: the user's 2026-11-08) */
double podium_block_w(void);
double podium_block_d(void);
double podium_block_h(int place); /* 0 1st, 1 2nd, 2 3rd */
double podium_shot_start(int s); /* (seconds) shot s's start */
enum { PODIUM_SHOTS = 5 }; /* far to the whole podium, 3rd, 2nd, the wide shot, the winner */

int podium_spot(double *x, double *y, double *z, double *yaw); /* its place (1), when it is drawn */
void podium_cast(int place, int kind, int index);   /* place 0-2 (1st-3rd): kind 0 a human, 1 a computer car's record */
/* tools (TM2PC_TEST_PODIUM_CARS with more than three cars: the art's line-up): every car added stands side by side in
 * car select's order on the roof, no blocks, one wide camera */
void podium_lineup_add(int kind, int index);
int podium_lineup_on(void);
/* its own roof (TM2PC_TEST_LINEUP_FLOOR=back: a spot whose roof has no drawn surface): half its width, how far in
 * front of the line (toward the camera) and behind it; 0 none */
int podium_lineup_floor(double *half_w, double *front, double *back);
void podium_scene(int on);                          /* the scene's frames to come (on), or over (the cast cleared) */
int podium_scene_on(void);
double podium_scene_seconds(void);                  /* its length */
int podium_shot_at(double t, double *u);            /* the shot at t seconds (u: 0..1 through it, eased) */
void podium_frame(double t);                        /* one frame at t seconds: the world drawn from the shot's camera,
                                                     * the cast posed, a VBlank */
