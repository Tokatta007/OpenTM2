/* TM2 PC runtime: the machine the recompiled game runs on.
 *
 * The recompiled code (build/recomp/gen) runs on CPU + guest RAM. This
 * runtime supplies everything the PSX would: BIOS functions, interrupts and
 * callbacks, timers, the CD (read from the disc image), and hardware
 * registers. Selected SDK functions are replaced by host versions
 * (src/overrides.json -> hle_* functions).
 *
 * Time model: recompiled code runs at host speed and does not count
 * cycles. Emulated time advances in VBlank steps when the game waits for
 * VSync; each step fires the game clock (root counter 2) events, refreshes
 * the pads and runs the VBlank callbacks. Other interrupts (DMA, GPU) are
 * raised as pending and delivered at safe points: when the game polls
 * hardware or leaves a critical section. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "psx_cpu.h"

#define NTSC_VBLANK_HZ 59.826
#define CLOCK_TICKS_PER_VBLANK 1.0800 /* root counter 2 at sysclk/8, target 0xFFFF */

enum {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5,
    IRQ_TMR2 = 6, IRQ_PAD = 7, IRQ_SIO = 8, IRQ_SPU = 9, IRQ_LIGHTPEN = 10, IRQ_COUNT = 11
};

typedef struct Event {
    uint32_t cls, spec, mode, func; /* as passed to OpenEvent */
    int open, enabled, ready;
} Event;

typedef struct Machine {
    CPU cpu;
    uint8_t *ram, *scratch;

    /* interrupts */
    int critical;                 /* inside EnterCriticalSection */
    int in_interrupt;             /* delivering a callback right now */
    uint32_t i_stat, i_mask;
    uint32_t irq_handler[IRQ_COUNT]; /* InterruptCallback */
    uint32_t dma_handler[8];         /* DMACallback */
    uint32_t vsync_callback;         /* VSyncCallback */
    uint32_t vsync_callbacks[8];     /* VSyncCallbacks(slot, f) */
    int callbacks_stopped;
    uint32_t dma_pending;            /* channels whose completion callback is due */

    /* events (BIOS) */
    Event events[16];
    int rcnt_autoclear[4];           /* ChangeClearRCnt: BIOS acknowledges the counter IRQ (default 1) */

    /* time */
    uint64_t vblanks;                /* VBlanks since boot */
    uint64_t last_vsync;             /* VBlank count at the last VSync(0/n) */
    double clock_acc;                /* root counter 2 accumulator */
    uint64_t clock_ticks;
    uint64_t vblank_offset;          /* the game's VBlank count (VSync(-1)): vblanks - this (a kill cam replay sets it back) */
    int headless;                    /* no pacing to wall-clock time */

    /* pads (InitPAD2 buffers) */
    uint32_t pad_buf[2], pad_len[2];
    uint16_t pad_buttons[2];         /* active-low PSX button bits */
    uint8_t pad_connected[2];        /* a pad is plugged into the port */
    int live_input;                  /* pads from the keyboard/gamepads (input_psyz.c) */

    /* diagnostics */
    int trace;
    unsigned unexpected;
} Machine;

extern Machine M;

/* core.c */
void rt_unexpected(const char *what, uint32_t a, uint32_t b);
uint32_t guest_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t guest_call5(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
uint32_t guest_call7(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6);
void rt_raise_irq(int irq);
void rt_deliver_pending(void);

/* hle_etc.c */
void rt_vblank(void);
void rt_events_fire(uint32_t cls, uint32_t spec);

/* bios.c */
void bios_call(CPU *c, uint32_t table, uint32_t num);
void bios_syscall(CPU *c);

/* hw.c */
uint32_t hw_read(uint32_t addr, int size);
int hw_audio_init(int output);
int hw_wav_open(const char *path);
void hw_wav_vblank(void);
void hw_write(uint32_t addr, uint32_t value, int size);

/* gpu.c */
uint32_t gpu_read_status(void);
uint32_t gpu_read_data(void);
void gpu_write_gp0(uint32_t w);
void gpu_write_gp1(uint32_t w);
void gpu_dma(uint32_t madr, uint32_t bcr, uint32_t chcr);

/* cd.c */
typedef struct Track {
    char path[512];
    uint32_t file_start; /* first sector of the track's .bin (including its pregap) */
    uint32_t start;      /* INDEX 01: where the track itself begins */
    uint32_t sectors;    /* sectors in the .bin */
    int audio;
} Track;
extern Track cd_tracks[100];
extern int cd_track_count;
int cd_read_raw(uint32_t lba, uint8_t *dst);
int cd_open(const char *cue_or_bin);
int cd_read_sector(uint32_t lba, uint8_t *dst2048);
int cd_find_file(const char *path, uint32_t *lba, uint32_t *size);

/* cd_audio.c: the host volumes (0-100), and which SPU voices are music,
 * checked each VBlank */
void audio_set_volumes(int music, int effects);
void audio_volumes_vblank(void);
void cd_audio_pause(int on);
extern int spu_port_keyon;          /* hw.c: set while the port starts its own sounds (not thinned) */        /* cd_audio.c: the music stops where it is, and goes on */
extern uint16_t (*rt_pad_filter)(int player, uint16_t buttons); /* hle.c: set while a screen feeds the game's pads */
void vp_vram_upload(int x, int y, int w, int h, const uint16_t *pix); /* vplus/fixes.c: each image put in VRAM (gpu.c) */
/* addrmap.c: a retail address where it is in this build (the decomp's port variant moves things;
 * the retail layout: the same); gr() the other way. GA() for the port's named addresses. */
uint32_t ga(uint32_t retail);
uint32_t gr(uint32_t now);
uint32_t gr_function(uint32_t now); /* the retail start of the function a return address is in (variant builds), else 0 */
#define GA(retail) ga(retail)
void rt_live_shot(void); /* main.c, tests: TM2PC_LIVE_SHOT */
/* The port's settings block in a build of the game's C with the port's own changes (decomp/ overlay,
 * include/semantic/port_hooks.h there): its address, 0 in other builds. Those changes then run in the
 * game's code, and the host only fills the block (vplus/port_config.c). */
extern const uint32_t rc_port_config;
extern const uint32_t rc_port_views_present; /* the variant's portViewsPresent (split screen's views and HUDs), 0 if none */
extern const uint32_t rc_port_far_lod;
extern const unsigned rc_hud_radar_bytes; /* a HUD radar record: 0x370 retail (12 blips), more with the variant's blips */
extern const unsigned rc_projectile_count; /* fxRecords: 16 retail, 48 in the 16-car variant */
extern const unsigned rc_shield_count; /* weaponShieldPool's entries: 4 retail, 16 in the 16-car variant */
extern const unsigned rc_car_pair_stride; /* the car-pair tables' row: 15 retail, 64 in the 16-car variant */
extern const unsigned rc_ai_cars_max; /* the build's computer-car records (9 retail; 12 with decomp TM2_PORT_CARS=16) */
uint32_t port_sym(const char *name);
const char *port_function_of(uint32_t addr); /* vplus/cars.c: the build's port function holding an address, or NULL */ /* vplus/cars.c: a port symbol of the build (decomp request 32), 0 if none */ /* the variant's far-LOD switch (decomp FAR_LOD.md), 0 if none */
#define PCFG_AI_VS_AI 0x00 /* u8 */
#define PCFG_HUMANS 0x01   /* u8: 3-4 players (vplus/four.c), else 0 */
#define PCFG_LEFT 0x02     /* u8: 3 players, player 1 the left half */
#define PCFG_RADAR_SOLID 0x03 /* u8: tests, radar dots every frame */
#define PCFG_PRECISE 0x05 /* u8: subpixel places wanted (the C's PORT_PRECISE_PUT) */
#define PCFG_TEAMS 0x06 /* u8: a team game; +0x07 4 players' teams, +0x0B 16 computer cars' */
#define PCFG_BOSS_ANYWHERE 0x04 /* u8: Survival with Dark Tooth: his fight on any arena, 16 more CLUT rows */
extern volatile unsigned rt_heartbeat; /* main.c: a frame shown (the game's or a menu's): the freeze watchdog */
void cd_audio_hold(int on);
void cd_audio_stop_music(void); /* cd_audio.c: the CD music stopped (a movie starts) */
int cd_audio_loop_count(void);
int cd_audio_music_playing(void);
void cd_audio_skip(int step);     /* next (+1) or previous (-1) song, on a random or order track */
void cd_audio_skip_vblank(void);

/* input.c: the controllers and keys, for the Controls pages */
uint16_t input_pad_buttons(void);
int input_new_key(void);
int input_controller_count(void);
void input_set_menu_player(int player); /* the port's menus read player 1's controller (or 2's) and the keyboard */
void input_recheck(void);
unsigned short input_easy_buttons(int player);
int input_steer_amount(int player); /* analog steering: 0..4096 of full lock (4096: digital) */ /* a player's D-pad when it gives energy attacks (Controls) */              /* the controllers looked for anew (SDL's gamepad system restarted) */
int input_player_pad_kind(int player);
const char *input_controller_name(int i);
int input_controller_active(int i);
int input_controller_player(int i);
int input_player_controller(int player);
void input_set_player_controller(int player, int i);
int input_last_menu_controller(void);   /* the gamepad last used in the port's menus, -1 none */
uint16_t input_controller_buttons(int i);       /* one controller's PS1 buttons */
uint16_t input_player_menu_buttons(int player); /* a player's controller (and keyboard for player 1) */
uint16_t input_player_pad_buttons(int player);  /* a player's controller only */
uint16_t input_player_dpad_buttons(int player); /* photo mode: the real D-pad (no left stick) and player 1's keys */
