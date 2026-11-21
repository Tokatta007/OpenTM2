/* The PC port's own changes to the game's C (the port repo's decomp/ overlay, docs/HYBRID.md there).
 *
 * Built only into the port variant with -DTM2_PORT_HOOKS (the overlay adds it to the variant's defines).
 * The port (host) fills portConfig from its settings; the game's C reads it. Without TM2_PORT_HOOKS
 * nothing here exists and every unit is the decomp's C. */
#ifndef TM2_SEMANTIC_PORT_HOOKS_H
#define TM2_SEMANTIC_PORT_HOOKS_H

#ifdef TM2_PORT_HOOKS
#define PORT_BOSS_ANYWHERE (portConfig.boss_anywhere != 0)
#else
#define PORT_BOSS_ANYWHERE 0
#endif
#ifdef TM2_PORT_HOOKS
/* 64 bytes, one block in .port_bss (config/US/port_variant_arrays.tsv); the port writes it by name
 * (out symbols.tsv). Add fields at the end; keep the size. */
struct TM2PortConfig {
    unsigned char ai_vs_ai;      /* +0x00 computer cars fighting each other (teams, spectating, "everyone equal") */
    unsigned char humans;        /* +0x01 3 or 4: the port's 3-4 player battle (views 3-4); else 0 */
    unsigned char left;          /* +0x02 3 players: player 1 has the left half (else the top) */
    unsigned char radar_solid;   /* +0x03 tests: radar dots every frame (no 1-in-5 for other heights) */
    unsigned char boss_anywhere; /* +0x04 Dark Tooth's fight on any arena (SURVIVAL.md 1.5): his weapons, his stage 2;
                                  * the Hong Kong map's jumps, warps and markers only on level 8;
                                  * bitmapInit leaves rows 240..255 free for car CLUTs */
    unsigned char precise;       /* +0x05 the renderer takes subpixel places (wobble smooth or none): PORT_PRECISE_PUT */
    unsigned char teams;         /* +0x06 a team game: team_human/team_ai hold each car's team */
    unsigned char team_human[4]; /* +0x07 */
    unsigned char team_ai[16];   /* +0x0B */
    unsigned char team_human_more[12]; /* +0x1B humans 5-16's teams (TM2_PORT_HUMANS=16: an online match's) */
    unsigned char pad_27[25];
};
extern struct TM2PortConfig portConfig;
#pragma tm2_absolute portConfig

/* the port's functions (port units, src/port: .port_text) */
extern int aiCarTargets[]; /* (s32; int here: not every unit has the types) */
int portAiAim(int owner, int *out, int mode); /* port_ai.c: AI vs AI's aimed shots */
void portRadarPlot(void);                     /* port_radar.c: 3-4 players' radars */
void portViewBuffersInit(void);               /* port_views.c: views 3-4's buffers (shellInitMachine) */
void portViewsDraw(void);                     /* port_views.c: split screen's views 2..N drawn (rtMainLoop) */
void portViewsPresent(int first);             /* port_views.c: and shown, with every view's HUD */

/* The port's high RAM (the PS1's 2 MB + 6 MB, the port's own): shared with the host (vplus/four.c, same
 * values): the frame's packet buffers (4 x retail's 0x17CF0, A and B), views 3-4's order tables and frame
 * records. */
#define PORT_PACKET_A 0x80D00000
#define PORT_PACKET_B 0x80D60000
#define PORT_PACKET_SIZE (4 * 0x17CF0)
#define PORT_OT_BASE 0x80DC0000
#define PORT_FRAMES_BASE 0x80DD0000
/* Subpixel places for vertices the game projects with integer C (not the GTE): a count, then entries
 * {vertex word's address, x and y in 1/16 pixel, depth}; the host takes them before it draws (gte.c). */
#define PORT_PRECISE_RING 0x80CB0000 /* (free high RAM to 0x80CEFFFF; the C emitter's, port_c_emitter.h) */
#define PORT_PRECISE_MAX 16380
#define PORT_PRECISE_PUT(p, x16, y16, z) do { \
        if (portConfig.precise) { \
            unsigned int *ring_ = (unsigned int *)PORT_PRECISE_RING, n_ = ring_[0]; \
            if (n_ < PORT_PRECISE_MAX) { \
                ring_[1 + n_ * 4] = (unsigned int)(p); \
                ring_[2 + n_ * 4] = (unsigned int)(x16); \
                ring_[3 + n_ * 4] = (unsigned int)(y16); \
                ring_[4 + n_ * 4] = (unsigned int)(z); \
                ring_[0] = n_ + 1; \
            } \
        } \
    } while (0)
/* a * b / c in 1/16 (a * b fits in 32 bits; c > 0) */
#define PORT_DIV16(q, c) (((q) / (c)) * 16 + (((q) % (c)) * 16) / (c))
#endif

#endif
