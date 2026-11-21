/* Marks in the 3D world drawn over each player's view (vplus/world_marks.c): the cars' markers (a triangle
 * over each car, its health and its name), the lock-on reticle and King of the Hill's hill. */
#ifndef VPLUS_WORLD_MARKS_H
#define VPLUS_WORLD_MARKS_H
#include <stdint.h>

typedef struct {
    int slot;              /* the car's object slot */
    int human;             /* 0-3 a player's car, else -1 */
    double x, y, z;        /* its place (the motion block's) */
    int hp, max_hp;
    uint8_t rgb[3];        /* its color: its team's, else its radar blip's */
    char name[24];         /* a player's profile name, else the car's (upper case) */
    int target;            /* Hunted: one of the targets (its diamond, hud_styles.c) */
    int ghost;             /* the ghost Juggernaut, hidden now: no marker (but in its own view) */
    uint8_t target_rgb[3]; /* (its team's color, else gold) */
} WmCar;

/* fixes.c */
int vp_mark_cars(WmCar *out, int max);  /* the cars alive */
int vp_hill_mark(int i, double *x, double *y, double *z, double *half, uint8_t rgb[3]); /* King of the Hill: 1 and hill i */
int vp_slot_human(int slot);            /* 0-3, or -1 */

/* world_marks.c: a view's camera as the game has it for the frame */
typedef struct {
    double m[3][3], eye[3];
    double hx, hy, cx, cy; /* the projection's scales and its centre (the view's own coordinates) */
} WmView;
int wm_view(int view, WmView *v);
int wm_project(const WmView *v, double x, double y, double z, double *sx, double *sy, double *depth); /* 1: in front */
void wm_capture(int view);                    /* four.c: as the view is drawn (renderScene) */
void wm_drawn_place(int view, WmCar *car);   /* the car where the view was drawn with it */
int wm_lock_slot(int player); /* the car a missile of the player's would home on now (their missiles chosen), -1 none */
#define WM_UP (1.0)           /* world z: up is positive */

#endif
