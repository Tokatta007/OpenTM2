/* The adaptive battle HUD (hud_layout.c): its groups and their places, also
 * for the Customize HUD preview (ui/options.c). */
#pragma once
enum { G_TL, G_TR, G_BL, G_BR, G_TM, G_C, G_FIX, G_VIEW, G_COUNT }; /* G_VIEW: the view's own places (the styles' counts), only squeezed for widescreen */
typedef struct {
    int src, sw, sh, vw, vh;          /* the game's layout (0 one view, 1 side by side, 2 one above the other) and the view */
    float ax[G_COUNT], ay[G_COUNT];   /* each group's anchor in the layout */
    float vx[G_COUNT], vy[G_COUNT];   /* and in the view */
    float scale[G_COUNT];
    int show[G_COUNT];
    float mx, my, mw, mh;             /* split screen: the rear-view mirror with its frame (mw 0: none) */
    float wk;                         /* widescreen: the view drawn squeezed by wk (vw: its width as seen; 0, 1: none) */
    float bar_r, bar_b;               /* the message bar's right edge and bottom in the view (the kill feed goes under it) */
} HudLayout;
void hudlay_compute(HudLayout *L, int src, int vw, int vh, int mirror);
int hudlay_group_of(int src, int x, int y, int mirror);
int hudlay_adaptive(void);
