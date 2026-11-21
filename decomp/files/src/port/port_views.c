/* The PC port's split-screen views (port code: .port_text). Built only into the port variant (--port-unit,
 * with -DTM2_PORT_HOOKS). One path for every split screen: views 2..N (2 players: view 2; 3-4 players:
 * views 2-4) are drawn by portViewsDraw, each exactly as the game draws its second view; view 1 is the
 * frame's main view (rtMainLoop, as with one player). Their frame records: rec_of (views 3-4's in the
 * port's high RAM, port_hooks.h PORT_FRAMES_BASE, the same addresses as the host's vplus/four.c, which
 * shows views 3-4 with their HUDs and the players' mirrors). */
#include <sys/types.h>
#include "semantic/rt_buffers.h" /* (libgte, libgpu) */
#include "semantic/port_hooks.h"

/* (rt_buffers.h: renderView0Frames[6], view 1's A/B, view 2's, the mirror's; D_80180EE4[3], the records
 * views 1, 2 and the mirror draw into this frame) */
extern int viewSplitScreenMode;
extern void viewProc(int view);
extern void streaksDraw(struct TM2BufferInitRecord *, int view);
extern void fxSpritesDraw(struct TM2BufferInitRecord *, int view);
extern void debrisDraw(struct TM2BufferInitRecord *);
extern void renderScene(struct TM2BufferInitRecord *, int view);
extern void hudDrawSplitVertical(int player, int first);
#ifdef TM2_PORT_FAR_DISTANCE
extern void portPacketBuffersApplyView(unsigned char *record, int view, int parity); /* port_far_distance.h (request 47) */
extern unsigned char *portPacketBufferCursor(unsigned char *record, unsigned char *before);
#endif
extern void hudDrawSplitHorizontal(int player, int first);

/* The frame record of view k (1: the second view .. 3: the fourth), buffer b (A/B): view 2's are the game's
 * (renderView0Frames[2..3], which its view creation sets up each match), views 3-4's the port's (high RAM,
 * set up by the port's view creation, vplus/four.c) */
#define FRAME_REC(k, b) ((struct TM2BufferInitRecord *)(PORT_FRAMES_BASE + 0x100 * ((k) - 1) + 0x80 * (b)))
static struct TM2BufferInitRecord *rec_of(int k, int b)
{
    return k == 1 ? &renderView0Frames[2 + b] : FRAME_REC(k, b);
}

/* After rtDoubleBufferInit (shellInitMachine): the six frame records' packet buffers (views 1-2 and the
 * mirror, A then B) four times as big, in high RAM; the table of views 2-4: each as the game's view 2
 * (its records, A and B), view 2 with the game's order tables, views 3-4 with their own. */
void portViewBuffersInit(void)
{
    struct TM2BufferInitRecord *r = renderView0Frames;
    int k, v, b, w;

    for (k = 0; k < 6; k++) {
        unsigned char *pk = (unsigned char *)((k & 1) ? PORT_PACKET_B : PORT_PACKET_A);

        r[k].field_4 = pk;
        r[k].field_C = pk + PORT_PACKET_SIZE;
    }
    for (v = 2; v < 4; v++) {
        for (b = 0; b < 2; b++) {
            int *to = (int *)FRAME_REC(v, b), *from = (int *)&renderView0Frames[2 + b];

            for (w = 0; w < 0x80 / 4; w++)
                to[w] = from[w];
            FRAME_REC(v, b)->field_0 = (unsigned long *)(PORT_OT_BASE + 0x4000 * (2 * (v - 2) + b));
        }
    }
}

/* rtMainLoop in split screen, after the main view: views 2..N, each as the game's second view (its other
 * buffer, its order table cleared, viewProc, the frame's packets carried on, streaks, debris, effect
 * sprites, renderScene); with 3-4 players the radars between view 2 and view 3 (where the host plotted
 * them). The record drawn into is view 2's D_80180EE4[1], as the game's HUD and present read it. */
void portViewsDraw(void)
{
    int n = portConfig.humans > 2 && viewSplitScreenMode == 2 ? portConfig.humans : 2;
    int b = D_80180EE4[0] == &renderView0Frames[0] ? 0 : 1, k;
    struct TM2BufferInitRecord *prev = D_80180EE4[0], *rec;

    if (n > 4)
        n = 4;
#ifdef TM2_PORT_FAR_DISTANCE
    /* views 3-4's records (A and B of each) at their view's own packet buffers (request 47: a buffer per view and
     * parity, 390 KB each), every frame as rtMainLoop applies views 1-2's and the mirror's */
    for (k = 2; k < n; k++) {
        portPacketBuffersApplyView((unsigned char *)FRAME_REC(k, 0), k, 0);
        portPacketBuffersApplyView((unsigned char *)FRAME_REC(k, 1), k, 1);
    }
#endif
    for (k = 1; k < n; k++) {
        rec = rec_of(k, b);
        if (k == 1)
            D_80180EE4[1] = rec;
        ClearOTagR(rec->field_0, 4096);
        viewProc(k);
        /* the frame's packets: in this view's own buffer from its start, or on from the view before's in retail's
         * shared one (one rule for every view, as rtMainLoop's for view 2 and the mirror) */
#ifdef TM2_PORT_FAR_DISTANCE
        rec->field_8 = portPacketBufferCursor((unsigned char *)rec, (unsigned char *)prev);
#else
        rec->field_8 = prev->field_8;
#endif
        streaksDraw(rec, k);
        debrisDraw(rec);
        fxSpritesDraw(rec, k);
        renderScene(rec, k);
        prev = rec;
    }
}

/* rtMainLoop's display in split screen (not in the attract demo): view 1 is on the screen already; each
 * view's HUD, and views 2..N each on the screen before its HUD, as the game shows its second view; the HUD
 * of the split's kind (side by side: hudDrawSplitVertical; one over another, and the quarters:
 * hudDrawSplitHorizontal). first: the frame's first buffer (the game's HUD argument). */
void portViewsPresent(int first)
{
    int n = portConfig.humans > 2 && viewSplitScreenMode == 2 ? portConfig.humans : 2;
    int b = D_80180EE4[0] == &renderView0Frames[0] ? 0 : 1, k;
    struct TM2BufferInitRecord *rec;

    if (n > 4)
        n = 4;
    for (k = 0; k < n; k++) {
        if (k > 0) {
            rec = k == 1 ? D_80180EE4[1] : rec_of(k, b);
            PutDrawEnv(&rec->field_10);
            DrawOTag(rec->field_0 + 4095);
        }
        if (viewSplitScreenMode == 1)
            hudDrawSplitVertical(k, first);
        else
            hudDrawSplitHorizontal(k, first);
    }
}
