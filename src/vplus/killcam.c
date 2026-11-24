/* The final kill cam (Vanilla Plus: Final kill cam).
 *
 * The game is deterministic: from the same RAM and the same buttons it plays
 * the same frames (TM2PC_STATE_LOG showed it frame for frame). So in a battle
 * the whole machine (guest RAM, scratchpad, CPU and the root counter's
 * fraction) is kept every quarter second at one point of the main loop (its
 * SetFrameStart, hle.c), the last few seconds of them, with each frame's
 * buttons (and the port's analog steering and D-pad energy reads). When the
 * battle's last kill ends it (the round's end scheduled within 2 seconds of a
 * car destroyed), a second after it the machine is kept as it is, put back
 * to about a second before the kill, and those seconds play again with the
 * same buttons, three times, each from another of the shots below (three
 * picked at random, all at the same speed), full screen, without
 * the HUD, with its sound effects, "FINAL KILL" over it. Then the machine kept at
 * the start comes back and the battle ends as it did. While replaying, the
 * port's own bookkeeping (feats, the match's tally, messages, lives, rounds)
 * is left alone (killcam_replaying).
 *
 * The C call stack is the same at every SetFrameStart of the main loop, and
 * the recompiled code keeps all its state in the guest registers and RAM: so
 * putting the machine back there is enough.
 *
 * Exactly (2026-10-11): a replay must play as the battle did, so nothing of the replay is changed to show it.
 * Its views stay the battle's (split screen and the mirror: the game's logic reads them, mines and flames tick
 * once per view); the shots are drawn full screen over each frame from the host with the whole RAM kept and
 * put back around the drawing (interp.c's killcam_present); the port's per-frame rules run in replays with
 * their state kept with the machine (vp_keep_register); the model animators draw from their own random stream
 * (interp.c), so what is drawn never moves the game's. The replay is checked frame by frame against the
 * battle's car hashes (TM2PC_KILLCAM_DEBUG: the first fields that differ; TM2PC_KILLCAM_DUMP=N: the whole
 * RAM, scratchpad and kept host state at frame N in the battle and in the replay, to diff). Photo mode
 * rewind's playback is checked the same way. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "runtime/gpu_backend.h"
#include "vplus.h"
#include "cars.h" /* the car records and counts */

#define RAM_SIZE GUEST_RAM_BYTES
#define SNAPS 10
#define SNAP_EVERY 15           /* frames between snapshots */
#define REC 4096                /* frames of buttons kept (photo mode rewind: over 30 s) */
#define BEFORE 42               /* frames shown before the kill (the user: 3/4 of 75, 90% of that, then the window 10% later) */
#define AFTER 39                /* and after it (the user's: 10% more of the aftermath) */
#define PASSES (vp_value(VP_FINAL_KILL_CAM) >= 1 && vp_value(VP_FINAL_KILL_CAM) <= 3 ? vp_value(VP_FINAL_KILL_CAM) : 3) /* (shots: Vanilla Plus) */
#define CAR_SLOT_OBJECTS GA(0x80187AD4u) /* each car slot's world object (its position at +0xA8) */
#define VIEW_SPLIT GA(0x801809E4u)
#define VIEW0_FRAMES GA(0x8018118Cu)

typedef struct {
    uint8_t *ram; uint8_t scratch[1024]; CPU cpu; double clock_acc; uint64_t frame; int used; uint64_t gv; uint8_t *host;
    /* the rest of the machine the game sees: interrupts, events, the counters, the last VSync */
    uint32_t i_stat, i_mask, dma_pending; int critical; Event events[16]; uint64_t clock_ticks, last_vsync, offset; uint32_t timers[3];
} Snap;
void hw_timer_counts(uint32_t *get, const uint32_t *set); /* runtime/hw.c */
/* The port's own game state kept with the machine: its gameplay blocks (killcam_keep, vplus/fixes.c), its
 * gameplay clock (VBlanks the replays leave out: vp_gv) and its random numbers (vp_rand: in guest RAM) */
#define KEEP_MAX 128
static struct { void *p; size_t n; } keep[KEEP_MAX];
static int keep_n;
static size_t keep_bytes;
void killcam_keep(void *p, size_t n)
{
    if (keep_n < KEEP_MAX) { keep[keep_n].p = p; keep[keep_n].n = n; keep_n++; keep_bytes += n; }
    else fprintf(stderr, "killcam: KEEP_MAX (%d) blocks: one more left out of the replays and network states\n", KEEP_MAX);
}
int killcam_keep_block(int i, void **p, size_t *n) /* the i-th kept block (net/rb_state.c's snapshots); 0: no more */
{
    if (i < 0 || i >= keep_n) return 0;
    *p = keep[i].p;
    *n = keep[i].n;
    return 1;
}
void killcam_keep_debug(int step) /* tests: each kept block's hash (two machines' lists diff by block) */
{
    for (int i = 0; i < keep_n; i++) {
        uint32_t h = 2166136261u;
        for (size_t k = 0; k < keep[i].n; k++) h = (h ^ ((uint8_t *)keep[i].p)[k]) * 16777619u;
        fprintf(stderr, "keep %d: block %d (%u bytes) %08x\n", step, i, (unsigned)keep[i].n, h);
    }
}
uint64_t vp_gv(void) { return M.vblanks - M.vblank_offset; } /* the gameplay clock: the game's VBlanks, set back with a replay */
#define RAND_STATE 0x80DF6FF0u
uint32_t vp_rand(void) /* the port's own random numbers (0..0x7FFF), kept in guest RAM: a replay draws the same */
{
    uint32_t x = mem_r32(&M.cpu, RAND_STATE) * 1103515245u + 12345u;
    mem_w32(&M.cpu, RAND_STATE, x);
    return (x >> 16) & 0x7FFF;
}
void vp_keep_register(void); /* vplus/fixes.c */
static Snap snaps[SNAPS], now_snap;
static uint64_t frame_no;            /* frames of this battle */
uint16_t humans_held(int h);         /* vplus/humans.c */
int killcam_battle_frame(void) { return frame_no > 0x7FFFFFFF ? 0x7FFFFFFF : (int)frame_no; }
static int rerun_back = -1;          /* (rollback) a step run again: this many frames before frame_no; -1 live */
static uint32_t rec[REC][16][4];     /* per frame, per human: pad (active low), steering, easy buttons, throttle (vplus/humans.c) */
static uint8_t rec_set[REC][16][4];
static uint32_t rec_hash[REC];       /* per frame: the cars' state (the replay's check) */
static uint64_t diverged_at;
/* TM2PC_KILLCAM_DEBUG: each frame's hashed records kept, the first difference shown field by field */
#define DBG_HUMANS (CARS_HUMANS_MAX * HUMAN_REC_SIZE)
#define DBG_AIS ((uint32_t)cars_ai_max() * AI_REC_SIZE)
#define DBG_SIZE (CARS_HUMANS_MAX * HUMAN_REC_SIZE + CARS_MAX * AI_REC_SIZE)
static uint8_t (*dbg_rec)[DBG_SIZE];
static void dbg_copy(uint8_t *to)
{
    for (uint32_t i = 0; i < DBG_HUMANS; i += 4) *(uint32_t *)(to + i) = mem_r32(&M.cpu, RB_humanCars + i);
    for (uint32_t i = 0; i < DBG_AIS; i += 4) *(uint32_t *)(to + DBG_HUMANS + i) = mem_r32(&M.cpu, GA(0x80188B5Cu) + i);
}
static void dbg_diff(uint64_t frame)
{
    static uint8_t now[DBG_SIZE];
    if (!dbg_rec) return;
    dbg_copy(now);
    const uint8_t *was = dbg_rec[frame % REC];
    int shown = 0;
    for (uint32_t i = 0; i < sizeof now && shown < 24; i += 4) {
        uint32_t a = *(const uint32_t *)(was + i), b = *(const uint32_t *)(now + i);
        if (a == b || (i < DBG_HUMANS && i % 0x814 < 0x110)) continue;
        if (i < DBG_HUMANS) fprintf(stderr, "kill cam diff: human %u +0x%03X %08X -> %08X\n", i / 0x814, i % 0x814, a, b);
        else fprintf(stderr, "kill cam diff: ai %u +0x%03X %08X -> %08X\n", (i - DBG_HUMANS) / 0x854, (i - DBG_HUMANS) % 0x854, a, b);
        shown++;
    }
}
/* TM2PC_KILLCAM_DUMP=N: the RAM, scratchpad and the kept host blocks as frame N ends, in the battle and in the
 * replay's first pass (kc-battle-N.ram, kc-replay-N.ram next to the state log): diffed, where they part */
static void dbg_dump(const char *what, uint64_t frame)
{
    static long long at = -2;
    if (at == -2) at = getenv("TM2PC_KILLCAM_DUMP") ? atoll(getenv("TM2PC_KILLCAM_DUMP")) : -1;
    if (at < 0 || frame != (uint64_t)at) return;
    char path[1024], dir[900] = ".";
    if (getenv("TM2PC_STATE_LOG")) {
        snprintf(dir, sizeof dir, "%s", getenv("TM2PC_STATE_LOG"));
        char *s = strrchr(dir, '\\'), *t = strrchr(dir, '/');
        if (t > s) s = t;
        if (s) *s = 0;
    }
    snprintf(path, sizeof path, "%s/kc-%s-%lld.ram", dir, what, at);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(M.ram, 1, RAM_SIZE, f);
    fwrite(M.scratch, 1, 1024, f);
    for (int i = 0; i < keep_n; i++) fwrite(keep[i].p, 1, keep[i].n, f);
    fclose(f);
    fprintf(stderr, "kill cam: dumped %s\n", path);
}
static uint32_t cars_hash(void)
{
    uint32_t h = 2166136261u;
    uint32_t total = mem_r32(&M.cpu, GA(0x80181014u)); /* humanCarTotal: every human (not their camera and view bytes) */
    for (uint32_t i = 0; i < total && i < 16; i++)
        for (uint32_t o = 0; o < HUMAN_REC_SIZE; o += 4) h = (h ^ human_rec_state_word(o, mem_r32(&M.cpu, HUMAN_REC(i) + o))) * 16777619u;
    for (uint32_t a = AI_REC(0); a < AI_REC(cars_ai_max()); a += 4) h = (h ^ mem_r32(&M.cpu, a)) * 16777619u;
    return h;
}
void killcam_cars_dump(FILE *f, int32_t step) /* tests (TM2PC_NET_CARDUMP): the hashed records as they are, a step's */
{
    fwrite(&step, 4, 1, f);
    for (uint32_t i = 0; i < 16; i++)
        for (uint32_t o = 0; o < HUMAN_REC_SIZE; o += 4) {
            uint32_t w = human_rec_state_word(o, mem_r32(&M.cpu, HUMAN_REC(i) + o));
            fwrite(&w, 4, 1, f);
        }
    for (uint32_t a = AI_REC(0); a < AI_REC(cars_ai_max()); a += 4) {
        uint32_t w = mem_r32(&M.cpu, a);
        fwrite(&w, 4, 1, f);
    }
    uint32_t fx_sprites_base(void), fx_sprites_n(void); /* vplus/interp.c */
    for (uint32_t a = fx_sprites_base(), e = a + fx_sprites_n() * 48u; a < e; a += 4) { /* (the effect sprites: 150 or 400 x 48) */
        uint32_t w = mem_r32(&M.cpu, a);
        fwrite(&w, 4, 1, f);
    }
}
/* The machines' state compared (net/game_tm2.c): the cars, the game's rand() seed, and the models' mutable node fields
 * (the level's animations: a wall frame one machine has and another has not; decomp ONLINE_ROLLBACK.md 13.3, 14.1),
 * listed by net/rb_state.c once a load (every loaded model file's nodes: 15.1) */
int rbs_dmd_list(uint32_t *buf); /* net/rb_state.c */
static int dmd_n;
static uint32_t dmd_buf;
static void dmd_refresh(void) { dmd_n = rbs_dmd_list(&dmd_buf); }
void killcam_dmd_dump(FILE *f, int32_t step) /* tests (TM2PC_NET_DMDDUMP): the hashed model fields, a step's */
{
    dmd_refresh();
    uint32_t seed = mem_r32(&M.cpu, 0x80009010u);
    fwrite(&step, 4, 1, f);
    fwrite(&seed, 4, 1, f);
    fwrite(&dmd_n, 4, 1, f);
    for (int i = 0; i < dmd_n; i++) {
        uint32_t a = mem_r32(&M.cpu, dmd_buf + 8u * (uint32_t)i), n = mem_r32(&M.cpu, dmd_buf + 8u * (uint32_t)i + 4u);
        fwrite(&a, 4, 1, f);
        fwrite(&n, 4, 1, f);
        for (uint32_t k = 0; k < n; k++) fputc((int)mem_r8(&M.cpu, a + k), f);
    }
}
uint32_t killcam_state_hash(void)
{
    uint32_t h = cars_hash();
    h = (h ^ mem_r32(&M.cpu, 0x80009010u)) * 16777619u; /* (the game's rand() seed) */
    dmd_refresh();
    for (int i = 0; i < dmd_n; i++) {
        uint32_t a = mem_r32(&M.cpu, dmd_buf + 8u * (uint32_t)i), n = mem_r32(&M.cpu, dmd_buf + 8u * (uint32_t)i + 4u);
        for (uint32_t k = 0; k < n; k++) h = (h ^ mem_r8(&M.cpu, a + k)) * 16777619u;
    }
    return h;
}
static int state;                    /* 0 recording, 1 a kill seen, 2 replaying, 3 done (until the round ends) */
static int rw;                       /* photo mode rewind: 0 off, 1 seeking (fast, hidden), 2 playing */
static uint64_t rw_frame;            /* rewinding: the frame about to run */
#define RW_SNAPS 16                  /* photo mode rewind's snapshots (below) */
#define RW_EVERY 120                 /* VBlanks of the gameplay clock between them (2 s) */
static Snap rw_snaps[RW_SNAPS], rw_now;
static uint64_t rw_last_gv, rw_target, rw_frame_gv[REC]; /* per frame: the gameplay clock at its start */
/* the shots: three of them picked for each kill cam */
enum { SHOT_ORBIT, SHOT_AERIAL, SHOT_CHASE, SHOT_FLIGHT, SHOT_LOW, SHOT_COUNT };
static const char *const shot_names[SHOT_COUNT] = {"around the wreck", "aerial", "chasing the killer",
                                                  "the shot's path (killer, its fire, the victim)", "low beside the victim"};
static int shots[3];
static uint64_t kill_frame, replay_frame, start_frame;
static int pass, victim_slot = -1, killer_slot = -1;
static uint64_t last_destroyed_frame;
int killcam_replaying;               /* the port's bookkeeping leaves the replay alone */
static int in_battle_now;

void audio_set_volumes(int music, int effects);
void audio_get_volumes(int *music, int *effects);
void killcam_banner(int on);         /* ui/options.c: "FINAL KILL" over the picture */

static void save(Snap *s)
{
    if (!s->ram && !(s->ram = malloc(RAM_SIZE))) return;
    memcpy(s->ram, M.ram, RAM_SIZE);
    memcpy(s->scratch, M.scratch, 1024);
    s->cpu = M.cpu;
    s->clock_acc = M.clock_acc;
    s->frame = frame_no;
    s->used = 1;
    s->gv = vp_gv();
    s->i_stat = M.i_stat; s->i_mask = M.i_mask; s->dma_pending = M.dma_pending; s->critical = M.critical;
    memcpy(s->events, M.events, sizeof s->events);
    s->clock_ticks = M.clock_ticks; s->last_vsync = M.last_vsync; s->offset = M.vblank_offset;
    hw_timer_counts(s->timers, NULL);
    if (!s->host && keep_bytes) s->host = malloc(keep_bytes);
    if (s->host)
        for (int i = 0, at = 0; i < keep_n; at += (int)keep[i].n, i++) memcpy(s->host + at, keep[i].p, keep[i].n);
}
static void load(const Snap *s)
{
    memcpy(M.ram, s->ram, RAM_SIZE);
    memcpy(M.scratch, s->scratch, 1024);
    uint8_t *ram = M.cpu.ram, *scr = M.cpu.scratch;
    M.cpu = s->cpu;
    M.cpu.ram = ram;
    M.cpu.scratch = scr;
    M.clock_acc = s->clock_acc;
    M.vblank_offset = M.vblanks - s->gv;
    M.i_stat = s->i_stat; M.i_mask = s->i_mask; M.dma_pending = s->dma_pending; M.critical = s->critical;
    memcpy(M.events, s->events, sizeof s->events);
    M.clock_ticks = s->clock_ticks;
    M.last_vsync = s->last_vsync + (M.vblank_offset - s->offset); /* (in the game's time, as it was) */
    hw_timer_counts(NULL, s->timers);
    void interp_snap(void); /* vplus/interp.c: another moment: nothing to blend from */
    interp_snap();
    if (s->host)
        for (int i = 0, at = 0; i < keep_n; at += (int)keep[i].n, i++) memcpy(keep[i].p, s->host + at, keep[i].n);
}
/* The machine's state as bytes, for another machine (net/game_tm2.c: online, the host's state at a match's start):
 * what a snapshot holds (save, load), the same build on both. Taken and put back at the same place, the frame's
 * start (hle_SetFrameStart), as the kill cam's own. */
enum { NET_STATE_MAGIC = 0x53324D54u, NET_STATE_VERSION = 1 }; /* "TM2S" */
#define NET_STATE_FIXED (8 + RAM_SIZE + 1024 + sizeof(CPU) + sizeof(double) + 3 * 8 + 4 * 4 + sizeof(((Snap *)0)->events) + 8 * 3 + 4 * 3)
size_t killcam_state_size(void) { return NET_STATE_FIXED + keep_bytes; }
static uint8_t *put(uint8_t *w, const void *p, size_t n) { memcpy(w, p, n); return w + n; }
static const uint8_t *get(const uint8_t *r, void *p, size_t n) { memcpy(p, r, n); return r + n; }
size_t killcam_state_save(uint8_t *out, size_t cap)
{
    static Snap s;
    if (cap < killcam_state_size()) return 0;
    save(&s);
    if (!s.ram || (keep_bytes && !s.host)) return 0;
    uint32_t magic = NET_STATE_MAGIC, version = NET_STATE_VERSION;
    uint8_t *w = out;
    w = put(w, &magic, 4); w = put(w, &version, 4);
    w = put(w, s.ram, RAM_SIZE); w = put(w, s.scratch, 1024); w = put(w, &s.cpu, sizeof s.cpu);
    w = put(w, &s.clock_acc, sizeof s.clock_acc); w = put(w, &s.frame, 8); w = put(w, &s.gv, 8);
    uint64_t used = (uint64_t)s.used; w = put(w, &used, 8);
    w = put(w, &s.i_stat, 4); w = put(w, &s.i_mask, 4); w = put(w, &s.dma_pending, 4); w = put(w, &s.critical, 4);
    w = put(w, s.events, sizeof s.events);
    w = put(w, &s.clock_ticks, 8); w = put(w, &s.last_vsync, 8); w = put(w, &s.offset, 8);
    w = put(w, s.timers, 4 * 3);
    if (keep_bytes) w = put(w, s.host, keep_bytes);
    return (size_t)(w - out);
}
int killcam_state_load(const uint8_t *in, size_t len)
{
    static Snap s;
    uint32_t magic, version;
    if (len != killcam_state_size()) return 0;
    const uint8_t *r = get(in, &magic, 4);
    r = get(r, &version, 4);
    if (magic != NET_STATE_MAGIC || version != NET_STATE_VERSION) return 0;
    if (!s.ram && !(s.ram = malloc(RAM_SIZE))) return 0;
    if (keep_bytes && !s.host && !(s.host = malloc(keep_bytes))) return 0;
    r = get(r, s.ram, RAM_SIZE); r = get(r, s.scratch, 1024); r = get(r, &s.cpu, sizeof s.cpu);
    r = get(r, &s.clock_acc, sizeof s.clock_acc); r = get(r, &s.frame, 8); r = get(r, &s.gv, 8);
    uint64_t used;
    r = get(r, &used, 8);
    r = get(r, &s.i_stat, 4); r = get(r, &s.i_mask, 4); r = get(r, &s.dma_pending, 4); r = get(r, &s.critical, 4);
    r = get(r, s.events, sizeof s.events);
    r = get(r, &s.clock_ticks, 8); r = get(r, &s.last_vsync, 8); r = get(r, &s.offset, 8);
    r = get(r, s.timers, 4 * 3);
    if (keep_bytes) r = get(r, s.host, keep_bytes);
    load(&s);
    frame_no = s.frame;
    return 1;
}
static void reset(void)
{
    state = 0;
    frame_no = 0;
    for (int i = 0; i < SNAPS; i++) snaps[i].used = 0;
    for (int i = 0; i < RW_SNAPS; i++) rw_snaps[i].used = 0;
    rw_last_gv = 0;
    victim_slot = killer_slot = -1;
    killcam_replaying = 0;
}

/* the buttons: recorded while playing, given back while replaying */
uint32_t netplay_input(int kind, int player, uint32_t live); /* net/game_tm2.c */
uint32_t killcam_input(int kind, int player, uint32_t live)
{
    live = netplay_input(kind, player, live); /* (online: every machine's agreed input) */
    if (player < 0 || player > 15 || kind < 0 || kind > 3 || !in_battle_now) return live;
    uint32_t i = (uint32_t)((rw ? rw_frame : state == 2 ? replay_frame : frame_no - (rerun_back > 0 ? (uint64_t)rerun_back : 0)) % REC);
    if (state == 2 || rw) {
        uint32_t v = rec_set[i][player][kind] ? rec[i][player][kind] : live;
        /* Start never pressed again in a replay: a pause made then opens only when it is pressed (active low: set) */
        if (kind == 0) v |= 0x0008u;
        return v;
    }
    rec[i][player][kind] = live;
    rec_set[i][player][kind] = 1;
    return live;
}

void killcam_destroyed(int own, int killer) /* feat_hooks.c: a car destroyed */
{
    if (state >= 2 || rw) return;
    victim_slot = own;
    killer_slot = killer;
    last_destroyed_frame = frame_no - (rerun_back > 0 ? (uint64_t)rerun_back : 0); /* (a step run again: its own frame) */
}
/* Rollback (net/game_tm2.c, net/rb_state.c): a step run again is `back` frames before the present one (its input at that
 * frame, the cars' hash it ends with, a kill in it); the snapshots taken after the step the rollback went back to hold
 * the guessed steps: forgotten (a replay starts from an older one, with the inputs as they were) */
void killcam_rerun_step(int back)
{
    if (state >= 2 || rw || back < 0 || (uint64_t)back > frame_no) return;
    if (rerun_back < 0) { /* (the batch's first step: from it on, the snapshots are of guessed steps) */
        uint64_t from = frame_no - (uint64_t)back;
        if (getenv("TM2PC_NET_DEBUG")) fprintf(stderr, "kill cam: frames from %llu run again\n", (unsigned long long)from);
        for (int i = 0; i < SNAPS; i++) if (snaps[i].used && snaps[i].frame > from) snaps[i].used = 0;
        for (int i = 0; i < RW_SNAPS; i++) if (rw_snaps[i].used && rw_snaps[i].frame > from) rw_snaps[i].used = 0;
    }
    rerun_back = back;
}
int killcam_catching_up; /* (net/game_tm2.c catch_up_step) the step running is one caught up, not one run again */
void killcam_rerun_frame(void) /* (hle.c: a step run again ends) the cars' hash its frame ends with */
{
    if (rerun_back >= 0 && state < 2 && !rw) rec_hash[(frame_no - (uint64_t)rerun_back) % REC] = cars_hash();
    else if (killcam_catching_up && state < 2 && !rw && in_battle_now) { /* a step caught up (net/game_tm2.c catch_up_step: no
                                         * rollback, no drawing): a frame of its own as killcam_frame makes one, the frame
                                         * before it ended, its inputs in a new one (else they took the live step's slot and
                                         * the replay ran a step short from there on) */
        rec_hash[frame_no % REC] = cars_hash();
        frame_no++;
        rw_frame_gv[frame_no % REC] = vp_gv();
    }
}
void killcam_rerun_end(void) { rerun_back = -1; }
void killcam_new_round(void) /* fixes.c: a round starts (a rematch too): the kill cam ready again */
{
    if (!rw && (state == 3 || state == 1)) reset();
    void rbs_new_round(void); /* net/rb_state.c: no rollback into the round before */
    rbs_new_round();
}
void killcam_forget(void) { if (!rw) victim_slot = -1; } /* fixes.c: the round ends without a kill (time up): no kill cam */
void killcam_round_end(void) /* fixes.c: the round's end scheduled */
{
    if (rw) return; /* (a rewind's replay: it was seen when played) */
    if (state != 0|| !vp_on(VP_FINAL_KILL_CAM) || victim_slot < 0 || frame_no > last_destroyed_frame + 120) {
        if (vp_on(VP_FINAL_KILL_CAM) && !state) /* (Survival asks every frame of a wave's end: told once) */
            fprintf(stderr, "final kill cam: none (%s)\n", victim_slot < 0 ? "no kill" : "the last kill too long before");
        return;
    }
    state = 1;
    kill_frame = last_destroyed_frame;
    if (getenv("TM2PC_KILLCAM_KILLER")) killer_slot = atoi(getenv("TM2PC_KILLCAM_KILLER")); /* tests: a killer for a test's kill */
    fprintf(stderr, "final kill cam: the kill at frame %llu\n", (unsigned long long)kill_frame);
}

static double ground_at(double x, double y, double z);
/* the sounds playing stopped, as the pause stops them (the game's sound object at gp+500, taskMgrStopAll): the
 * battle's loops (a turbo's, a machine gun's, the engines) when the kill cam starts, a pass's at its end. Else a loop
 * keyed on in the battle played on over the whole kill cam (nothing in the replay's machine knew of it: the user's
 * turbo, 2026-11-03). The battle goes on after it as after a pause: its loops started again by what the cars do. */
/* The battle's sounds stopped by the port (the kill cam, the results): the game's taskMgrStopAll on the sound object
 * (gp+500), its task manager's own word (taskMgrStopNext, taskMgrStopCurrent: retail 80181158, hashed online) put back after: each machine
 * stops them at its own moment (its results' time), and online the machines' states parted at every stage's end */
static void player_loops_stop(void);
void port_sounds_stop(void)
{
    uint32_t cur = GA(0x80181158u), keep[5]; /* (taskMgrStopNext, taskMgrStopCurrent: retail 80181158..8018116C) */
    for (int i = 0; i < 5; i++) keep[i] = mem_r32(&M.cpu, cur + 4u * (uint32_t)i);
    guest_call(GA(0x801605B4u), mem_r32(&M.cpu, M.cpu.r[28] + 500), 0, 0, 0);
    for (int i = 0; i < 5; i++) mem_w32(&M.cpu, cur + 4u * (uint32_t)i, keep[i]);
    player_loops_stop();
}
static void player_loops_stop(void)
{
    /* the players' own loops too (turbo 0, gun 9, burn / lava 32): kept in the sound object's per-player loop lists
     * and the port's per-slot instances, which taskMgrStopAll never reached: a player's turbo held at the round's end
     * played on over the whole kill cam (the user's, 2026-11-06; the trace: its voice keyed on through it).
     * portHumanLoopStop makes the game's own stops (the loop flags first; those flags are the sound side's, not hashed) */
    static uint32_t stop = 1;
    if (stop == 1) stop = port_sym("portHumanLoopStop");
    int n = (int)mem_r32(&M.cpu, GA(0x80181014u));
    static const int loops[3] = {0, 9, 32};
    uint32_t before = mem_r32(&M.cpu, GA(0x80180A04u)), before2 = mem_r32(&M.cpu, GA(0x80180A08u));
    for (int h = 0; stop && h < n && h < CARS_HUMANS_MAX; h++)
        for (int k = 0; k < 3; k++)
            guest_call(stop, (uint32_t)loops[k], (uint32_t)h, (uint32_t)(int16_t)mem_r16(&M.cpu, HUMAN_REC(h) + 4), 0);
    if (getenv("TM2PC_SOUND_TRACE")) /* (the loop flags: soundLoop32ActiveP1 80180A06, turbo A08, gun A0A; P2 the next byte) */
        fprintf(stderr, "sound %u: the players' loops stopped (flags %08X %08X, now %08X %08X)\n", (unsigned)M.vblanks,
                before, before2, mem_r32(&M.cpu, GA(0x80180A04u)), mem_r32(&M.cpu, GA(0x80180A08u)));
}
static void sounds_stop(void) { port_sounds_stop(); }
static uint32_t task_word[5]; /* (the kill cam: the sound task manager's word as the battle had it) */
static void start_pass(void)
{
    const Snap *s = NULL;
    for (int i = 0; i < SNAPS; i++) /* the newest snapshot at least BEFORE frames before the kill */
        if (snaps[i].used && snaps[i].frame + BEFORE <= kill_frame && (!s || snaps[i].frame > s->frame)) s = &snaps[i];
    if (!s) for (int i = 0; i < SNAPS; i++) if (snaps[i].used && (!s || snaps[i].frame < s->frame)) s = &snaps[i];
    if (!s) { state = 3; return; }
    load(s);
    /* the snapshot brought back the sound manager's loops of its moment with the rest of the memory (a turbo's flame,
     * loop 32, on then): the sound side re-triggered it through the whole pass, no start call needed; stopped here,
     * the replay's own sounds start again from what the cars do */
    player_loops_stop();
    replay_frame = start_frame = s->frame;
    /* (the views stay the battle's: split screen and the mirror are read by the game's own logic, mines and flames tick
     * per view; the shots are drawn full screen over each frame from the host, interp.c's killcam_present) */
}
int killcam_showing(void) { return state == 2 || rw == 2; } /* interp.c: the kill cam's or rewind's playback, its own view */

/* ---- Photo mode rewind (VP_PHOTO_REWIND) ----
 * The same machine kept every 2 seconds of the battle's gameplay clock, the
 * last 16 (30 seconds back), with the buttons above. Photo mode (opened from
 * the pause menu) asks for a moment: the pause ends with the photo still on
 * the screen, and at the next SetFrameStart the machine is kept as it is (the
 * present), the snapshot before the moment comes back and the frames up to it
 * play again with their buttons, as fast as they run, unseen and unheard
 * (rt_fast_forward). There photo mode opens again (from here, between two
 * frames). Play runs the frames on at their speed, seen from photo mode's
 * camera, up to the present (or until stopped). Back (Circle) puts the present
 * back and opens the pause menu again. While rewinding, the port's
 * bookkeeping leaves the replays alone (killcam_replaying), as in the kill cam. */
static int rw_requested, rw_effects = -1, rw_music;
extern int rt_fast_forward;          /* main.c */
int photo_rewind_session(void);     /* ui/photo.c: photo mode at a past moment: 0 back, 1 a moment asked for, 2 play */
int photo_camera_put(uint32_t cam); /* ui/photo.c: its camera on cam (the play's) */
void photo_hold(int on);            /* ui/photo.c: the last picture kept on the screen (seeking) */
void photo_play_overlay(void);      /* ui/photo.c: the timeline while playing */
void ui_pause_again(void);          /* ui/options.c: the pause menu at the next frame */

static int rw_enabled(void) { return vp_on(VP_PHOTO_REWIND) || getenv("TM2PC_REWIND_TEST"); } /* (tests: on) */
int rewind_range(uint64_t *oldest, uint64_t *now, uint64_t *at) /* photo mode: the frames it can show */
{
    if (!rw_enabled() || !in_battle_now) return 0; /* (after the final kill too: its kill cam ends for it, the user's) */
    uint64_t o = (uint64_t)-1;
    for (int i = 0; i < RW_SNAPS; i++)
        if (rw_snaps[i].used && rw_snaps[i].frame < o) o = rw_snaps[i].frame;
    if (o == (uint64_t)-1) return 0;
    *now = rw ? rw_now.frame : frame_no + 1; /* (paused: the frame being played ends first) */
    *at = rw ? rw_frame : *now;
    *oldest = o;
    return 1;
}
double rewind_seconds(uint64_t frame) /* how far a frame is before the present */
{
    uint64_t now = rw ? rw_now.frame : frame_no + 1, g = rw ? rw_now.gv : vp_gv();
    if (frame >= now || now - frame >= REC) return 0;
    return (double)(g - rw_frame_gv[frame % REC]) / 59.83;
}
void rewind_request(uint64_t frame) { rw_target = frame; rw_requested = 1; } /* photo mode: this moment */

static void rw_seek(uint64_t target)
{
    if (target >= rw_now.frame) { load(&rw_now); rw_frame = rw_now.frame; return; }
    if (target < rw_frame) { /* back: the snapshot before it */
        const Snap *s = NULL;
        for (int i = 0; i < RW_SNAPS; i++)
            if (rw_snaps[i].used && rw_snaps[i].frame <= target && (!s || rw_snaps[i].frame > s->frame)) s = &rw_snaps[i];
        if (!s) for (int i = 0; i < RW_SNAPS; i++) if (rw_snaps[i].used && (!s || rw_snaps[i].frame < s->frame)) s = &rw_snaps[i];
        if (s) { load(s); rw_frame = s->frame; }
    }
    rw_target = target;
}
static int rw_back_now; /* O pressed while playing */
static void rw_end(void) /* back to the present, the pause menu again */
{
    rw_requested = 0;
    load(&rw_now);
    rw = 0;
    rt_fast_forward = 0;
    killcam_replaying = 0;
    audio_set_volumes(rw_music, rw_effects);
    rw_effects = -1;
    ui_pause_again();
    fprintf(stderr, "photo rewind: back at the present\n");
}
/* at a moment (rw_frame): photo mode, then what it asked for */
static void rw_moment(void)
{
    rt_fast_forward = 0;
    if (rw_back_now) { rw_back_now = 0; rw_end(); return; } /* (O while playing: straight back) */
    for (;;) {
        /* the sounds the replay started (engines, a machine gun's loop) stopped as the pause stops them (its sound
         * object at gp+500): else they loop on through photo mode until the battle resumes */
        port_sounds_stop();
        /* the moment kept whole and put back after photo mode: its VBlanks (the sound driver's, the clocks, the root
         * counter the game times its steps by) went on while it was up, and the battle played on 5 VBlanks' worth in
         * one step, drifting (the user's: a weapon kill played back as a push off the edge) */
        static Snap look;
        save(&look);
        int r = photo_rewind_session();
        /* its moment asked for is taken here (rw_target): the flag left up started a rewind again at the next Resume (the
         * user's 2026-11-17: a second of muted replay, then the pause menu again) */
        rw_requested = 0;
        load(&look);
        if (r == 0) { rw_end(); return; }
        if (r == 2 && rw_frame < rw_now.frame) { /* play: at the game's speed, heard, from photo mode's camera */
            rw = 2;
            void photo_play_reset(void);
            photo_play_reset();
            audio_set_volumes(rw_music, rw_effects); /* (shown full screen from photo mode's camera: interp.c) */
            fprintf(stderr, "photo rewind: playing from frame %llu\n", (unsigned long long)rw_frame);
            return;
        }
        if (r == 1 && rw_target != rw_frame) {
            rw = 1;
            audio_set_volumes(rw_music, 0);
            rw_seek(rw_target);
            if (rw_frame == rw_target) continue; /* (a snapshot's own frame, or the present) */
            rt_fast_forward = 1;
            return;
        }
    }
}
static void rw_step(void) /* each SetFrameStart while rewinding: a frame played */
{
    static uint64_t checked_to; /* the replay against the battle's own frames (as the kill cam checks) */
    if (rw_frame < rw_now.frame && rw_frame >= checked_to && rec_hash[rw_frame % REC] != cars_hash()) {
        fprintf(stderr, "photo rewind: the replay differed from the battle at frame %llu\n", (unsigned long long)rw_frame);
        if (getenv("TM2PC_KILLCAM_DEBUG")) dbg_diff(rw_frame);
        checked_to = rw_now.frame; /* (once) */
    }
    if (rw_frame + 1 >= rw_now.frame && checked_to < rw_now.frame) {
        fprintf(stderr, "photo rewind: the replay matched the battle up to the present\n");
        checked_to = rw_now.frame;
    }
    rw_frame++;
    int stop = 0;
    if (rw == 2) { /* playing: X stops it there; O back to the present (and the pause menu) at once */
        uint16_t input_pad_buttons(void);
        static uint16_t prev = 0xFFFF;
        uint16_t live = M.live_input ? input_pad_buttons() : 0, pr = (uint16_t)(live & ~prev);
        prev = live;
        stop = (pr & 0x6000) != 0;
        if (pr & 0x2000) rw_back_now = 1;
        if (getenv("TM2PC_REWIND_TEST")) stop = 0;
        photo_play_overlay();
    }
    if (rw_frame >= rw_now.frame) { load(&rw_now); rw_frame = rw_now.frame; stop = 1; } /* the present */
    if (stop || (rw == 1 && rw_frame >= rw_target)) rw_moment();
}

static int music_saved = -1, effects_saved;
void killcam_keep_init(void) /* the port's gameplay blocks registered (once; net/game_tm2.c sizes its states by them) */
{
    static int registered;
    if (!registered) { registered = 1; vp_keep_register(); }
}
void killcam_frame(int battle) /* each SetFrameStart (hle.c), before the game's */
{
    killcam_keep_init();
    in_battle_now = battle;
    static int seeded;
    if (!battle) {
        if (state || frame_no) reset();
        frame_no = 0;
        /* the port's random numbers start somewhere new for each battle (tests: TM2PC_SEED seeds SDL's) */
        if (!seeded) { seeded = 1; mem_w32(&M.cpu, RAND_STATE, (uint32_t)SDL_rand_bits()); }
        return;
    }
    seeded = 0;
    /* (a battle's first frames' guard: vplus/fixes.c vp_MoveCodes, in every step the game runs, a caught-up one too) */

    { /* the battle's first 10 s: player 1's shots, energy attacks and jumps in the log, with the buttons then (the
       * user's weapon or jump at a level's start, 2026-11-04: what made it) */
        static int start_lines;
        if (frame_no == 0) start_lines = 0;
        int fire = mem_r8(&M.cpu, HUMAN_REC(0) + 0xEA), go = mem_r8(&M.cpu, HUMAN_REC(0) + 0xEC);
        if (frame_no < 300 && !state && !rw && (fire || go) && start_lines < 6) {
            start_lines++;
            fprintf(stderr, "battle start: frame %d player 1 %s%s (special %d) pad %04X held %04X\n", (int)frame_no,
                    fire ? "fire " : "", go ? "energy attack" : "", mem_r8(&M.cpu, HUMAN_REC(0) + 0x7D4), M.pad_buttons[0],
                    humans_held(0));
        }
    }
    if (frame_no % 30 == 0 && !state && !rw) { /* tests: the effect sprites in use (150) and the
                                                                          * explosions' sound cooldown (the user's missing
                                                                          * explosions, 2026-11-04) */
        int live = 0, looping = 0;
        uint32_t fx_sprites_base(void), fx_sprites_n(void); /* vplus/interp.c */
        uint32_t fx_n = fx_sprites_n();
        for (uint32_t i = 0; i < fx_n; i++) {
            uint32_t r = fx_sprites_base() + 48u * i;
            if ((int8_t)mem_r8(&M.cpu, r + 10) >= 0) { live++; looping += (int16_t)mem_r16(&M.cpu, r + 12) > 0; }
        }
        static int fx_log = -1;
        static uint64_t full_told;
        if (fx_log < 0) fx_log = getenv("TM2PC_FX_LOG") != NULL;
        if (fx_log)
            fprintf(stderr, "fx log: frame %d sprites %d (looping %d) cooldown %d\n", (int)frame_no, live, looping,
                    (int32_t)mem_r32(&M.cpu, GA(0x80180B5Cu)));
        else if (live >= (int)fx_n && (!full_told || frame_no >= full_told + 600 || frame_no < full_told)) { /* (every build: at
                                                                * most once in 10 s) a new explosion is skipped while full */
            full_told = frame_no;
            fprintf(stderr, "effects: all %d effect sprites in use (%d looping) at frame %d: new explosions are skipped\n",
                    live, looping, (int)frame_no);
        }
    }
    if (getenv("TM2PC_FIRE_LOG") && (frame_no < 6 || (frame_no < 120 && frame_no % 10 == 0)) && !state && !rw) /* tests: player 1's fire bytes as a battle starts */
        fprintf(stderr, "fire log: frame %d vblank %llu fire %d ready %d held %d rear %d fire missiles %d homing %d pad %04X special %d go %d\n", (int)frame_no,
                (unsigned long long)M.vblanks, mem_r8(&M.cpu, HUMAN_REC(0) + 0xEA), mem_r8(&M.cpu, HUMAN_REC(0) + 0xED),
                mem_r8(&M.cpu, HUMAN_REC(0) + 0xEE), mem_r8(&M.cpu, HUMAN_REC(0) + 0x7CD), (int16_t)mem_r16(&M.cpu, HUMAN_REC(0) + 0x7C8 + 0x10),
                (int16_t)mem_r16(&M.cpu, HUMAN_REC(0) + 0x7C8 + 0x12), M.pad_buttons[0], mem_r8(&M.cpu, HUMAN_REC(0) + 0x7D4),
                mem_r8(&M.cpu, HUMAN_REC(0) + 0xEC));
    if (getenv("TM2PC_KILLCAM_OBJLOG") && last_destroyed_frame && victim_slot >= 0 && victim_slot < 15) { /* tests: the victim's
         * object (drawn +8, model +0x11C, visible +0x124) after the kill, in the battle and in each pass */
        uint64_t f = state == 2 ? (uint64_t)replay_frame : frame_no;
        uint32_t o = mem_r32(&M.cpu, CAR_SLOT_OBJECTS + 4u * (uint32_t)victim_slot);
        if (o && f >= last_destroyed_frame && f <= last_destroyed_frame + AFTER)
            fprintf(stderr, "kc obj: %s %d frame +%d draw %d model %08x visible %d z %d\n", state == 2 ? "pass" : "battle", state == 2 ? pass : -1,
                    (int)(f - last_destroyed_frame), (int)mem_r32(&M.cpu, o + 8), mem_r32(&M.cpu, o + 0x11C), (int)mem_r32(&M.cpu, o + 0x124),
                    (int32_t)mem_r32(&M.cpu, o + 0xB0));
    }
    static long long ground_map = -1; /* tests: TM2PC_GROUND_MAP=V, at VBlank V the floor's height every 400 units */
    if (ground_map < 0) ground_map = getenv("TM2PC_GROUND_MAP") ? atoll(getenv("TM2PC_GROUND_MAP")) : 0;
    if (ground_map && (long long)M.vblanks == ground_map) {
        for (int y = -4000; y <= 8000; y += 400) {
            fprintf(stderr, "ground y %6d:", y);
            for (int x = -8000; x <= 9000; x += 400) fprintf(stderr, " %5.0f", ground_at(x, y, 20000));
            fputc('\n', stderr);
        }
    }
    if (rw) { rw_step(); return; }
    if (state == 2 && rw_requested) { /* photo mode rewind asked for during the kill cam: the battle back, then the moment */
        sounds_stop();
        load(&now_snap);
        killcam_replaying = 0;
        if (music_saved >= 0) audio_set_volumes(music_saved, effects_saved);
        killcam_banner(0);
        state = 3;
        fprintf(stderr, "final kill cam: ended for photo mode rewind\n");
        return; /* (the next frame starts the rewind) */
    }
    if (state == 2) {
        /* X (a local game: any controller here) skips the rest of it */
        uint16_t input_pad_buttons(void); /* runtime/input.c: every gamepad and the keyboard, set = held */
        static uint16_t skip_prev = 0xFFFF;
        uint16_t live = M.live_input ? input_pad_buttons() : 0, skip = (uint16_t)(live & ~skip_prev & 0x4000);
        skip_prev = live;
        if (getenv("TM2PC_TEST_KILLCAM_SKIP")) skip = 1; /* tests: every shot skipped at once (the others still watching: their wait) */
        if (skip) replay_frame = kill_frame + AFTER; /* this shot ends: the next one (the last: the battle back) */
        if (pass == 0) dbg_dump("replay", replay_frame);
        if (getenv("TM2PC_KILLCAM_SHOT") && getenv("TM2PC_STATE_LOG")) { /* tests: each pass at the kill, or at the frames
             * after it listed (TM2PC_KILLCAM_SHOT=0,10,20: frames from the kill) */
            const char *s = getenv("TM2PC_KILLCAM_SHOT");
            int listed = strchr(s, ',') != NULL, hit = 0;
            for (const char *p = s; listed && *p; p++)
                if ((p == s || p[-1] == ',') && kill_frame + (uint64_t)atoi(p) == (uint64_t)replay_frame) hit = 1;
            if (listed ? hit : replay_frame == kill_frame) {
                char path[1024];
                if (listed) snprintf(path, sizeof path, "%s-killcam%d-%02d.bmp", getenv("TM2PC_STATE_LOG"), pass, (int)(replay_frame - kill_frame));
                else snprintf(path, sizeof path, "%s-killcam%d.bmp", getenv("TM2PC_STATE_LOG"), pass);
                gpu_backend->capture(path, 0);
            }
        }
        if (pass == 0 && !diverged_at && replay_frame <= kill_frame && rec_hash[replay_frame % REC] != cars_hash()) {
            diverged_at = replay_frame;
            if (getenv("TM2PC_KILLCAM_DEBUG")) dbg_diff(replay_frame);
        }
        replay_frame++;
        if (replay_frame >= kill_frame + AFTER) { /* a pass done */
            sounds_stop(); /* (the pass's) */
            if (++pass < PASSES) { start_pass(); return; }
            load(&now_snap); /* the battle as it was */
            /* and the sound task manager's word (taskMgrStopCurrent, hashed online; not in the snapshot): the passes'
             * sounds left it as the replay had it, and a machine's state parted from the others' at a stage's end */
            for (int i = 0; i < 5; i++) mem_w32(&M.cpu, GA(0x80181158u) + 4u * (uint32_t)i, task_word[i]);
            killcam_replaying = 0;
            if (music_saved >= 0) audio_set_volumes(music_saved, effects_saved);
            killcam_banner(0);
            state = 3;
            if (diverged_at) fprintf(stderr, "final kill cam: the replay differed from frame %llu\n", (unsigned long long)diverged_at);
            else fprintf(stderr, "final kill cam: the replay matched the battle frame for frame\n");
            fprintf(stderr, "final kill cam: done\n");
        }
        return;
    }
    dbg_dump("battle", frame_no);
    rec_hash[frame_no % REC] = cars_hash(); /* (as the frame ends: frame_no's) */
    if (getenv("TM2PC_KILLCAM_DEBUG")) {
        if (!dbg_rec) dbg_rec = calloc(REC, sizeof *dbg_rec);
        if (dbg_rec) dbg_copy(dbg_rec[frame_no % REC]);
    }
    frame_no++;
    rw_frame_gv[frame_no % REC] = vp_gv();
    if (rw_enabled() && state != 2 && (vp_gv() >= rw_last_gv + RW_EVERY || vp_gv() < rw_last_gv)) { /* photo mode rewind's */
        int oldest = 0;
        for (int i = 0; i < RW_SNAPS; i++) {
            if (!rw_snaps[i].used) { oldest = i; break; }
            if (rw_snaps[i].frame < rw_snaps[oldest].frame) oldest = i;
        }
        save(&rw_snaps[oldest]);
        rw_last_gv = vp_gv();
    }
    if (rw_requested) { /* photo mode asked for a moment: the present kept, the replay to it */
        rw_requested = 0;
        if (state != 2) {
            save(&rw_now);
            audio_get_volumes(&rw_music, &rw_effects);
            killcam_replaying = 1;
            rw = 1;
            rw_frame = rw_now.frame;
            fprintf(stderr, "photo rewind: from frame %llu to %llu\n", (unsigned long long)rw_now.frame, (unsigned long long)rw_target);
            uint64_t t = rw_target;
            rw_target = rw_frame;
            audio_set_volumes(rw_music, 0);
            rw_seek(t);
            if (rw_frame >= rw_target) rw_moment();
            else rt_fast_forward = 1;
            return;
        }
    }
    if (state <= 1 && frame_no % SNAP_EVERY == 0) {
        int oldest = 0;
        for (int i = 0; i < SNAPS; i++) {
            if (!snaps[i].used) { oldest = i; break; }
            if (snaps[i].frame < snaps[oldest].frame) oldest = i;
        }
        save(&snaps[oldest]);
    }
    if (state == 1 && frame_no >= kill_frame + 50) { /* a second after it: the replay */
        sounds_stop(); /* (the battle's: kept stopped, as after a pause) */
        save(&now_snap);
        for (int i = 0; i < 5; i++) task_word[i] = mem_r32(&M.cpu, GA(0x80181158u) + 4u * (uint32_t)i); /* (below) */
        audio_get_volumes(&music_saved, &effects_saved); /* (the effects as loud as in the battle) */
        killcam_replaying = 1;
        killcam_banner(1);
        pass = 0;
        diverged_at = 0;
        state = 2;
        for (int i = 0; i < 3; i++) { /* three different shots, in a random order */
            int s;
            do s = (int)SDL_rand(SHOT_COUNT); while ((i > 0 && s == shots[0]) || (i > 1 && s == shots[1]));
            shots[i] = s;
        }
        if (getenv("TM2PC_KILLCAM_SHOTS")) sscanf(getenv("TM2PC_KILLCAM_SHOTS"), "%d,%d,%d", &shots[0], &shots[1], &shots[2]); /* tests */
        fprintf(stderr, "final kill cam: shots %s, %s, %s\n", shot_names[shots[0] % SHOT_COUNT], shot_names[shots[1] % SHOT_COUNT],
                shot_names[shots[2] % SHOT_COUNT]);
        fprintf(stderr, "final kill cam: replaying frames %llu..%llu\n", (unsigned long long)(kill_frame - BEFORE),
                (unsigned long long)(kill_frame + AFTER));
        start_pass();
    }
}

/* the camera (ui_ViewProc, before viewProc(0)): its parent at the origin, it at the pass's place, looking at the wreck */
static void obj_pos(int slot, double p[3])
{
    uint32_t o = slot >= 0 && slot < 15 ? mem_r32(&M.cpu, CAR_SLOT_OBJECTS + 4u * (uint32_t)slot) : 0;
    for (int i = 0; i < 3; i++) p[i] = o ? (int32_t)mem_r32(&M.cpu, o + 0xA8 + 4u * (uint32_t)i) : 0;
}
#define POS_BUF 0x80DF6900u /* a position for worldGroundHeightAt (high RAM) */
static double ground_at(double x, double y, double z) /* the floor under (x, y), from height z down (800EF340) */
{
    mem_w32(&M.cpu, POS_BUF, (uint32_t)(int32_t)x);
    mem_w32(&M.cpu, POS_BUF + 4, (uint32_t)(int32_t)y);
    mem_w32(&M.cpu, POS_BUF + 8, (uint32_t)(int32_t)z);
    return (int32_t)guest_call(GA(0x800EF340u), POS_BUF, 0, 0, 0);
}
static void place_camera(uint32_t cam, int shot, int sign, const double v[3], const double k[3], double u);
int vp_cinema_watch(int view, uint32_t *watched, uint32_t *other); /* vplus/fixes.c: a view watching with cinematic: the cars' objects */
int vp_freecam(void);                       /* vplus/fixes.c: spectating with the free camera */
int photo_freecam_put(uint32_t cam);        /* ui/photo.c */
int killcam_camera(int view)
{
    if (view < 0 || view > 3 || (view != 0 && (rw || state == 2))) return 0; /* (split screen: each watching view's cinematic) */
    uint32_t cam = mem_r32(&M.cpu, RB_viewCameraObjects + 4u * (uint32_t)view), parent = mem_r32(&M.cpu, RB_viewCameraObjectTable + 4u * (uint32_t)view);
    uint32_t wo = 0, oo = 0;
    if (rw == 1) return 0;
    int fc = view == 0 && rw != 2 && state != 2 && vp_freecam();
    if (rw != 2 && state != 2 && !fc && !vp_cinema_watch(view, &wo, &oo)) return 0;
    if (!cam || !parent) return 0;
    static const int16_t ident[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
    for (int i = 0; i < 9; i++) mem_w16(&M.cpu, parent + 0x80 + 2u * (uint32_t)i, (uint16_t)ident[i]);
    for (uint32_t i = 0; i < 12; i++) mem_w8(&M.cpu, parent + 0x8C + i, 0);
    for (uint32_t i = 0; i < 20; i++) mem_w8(&M.cpu, parent + 0xA0 + i, 0);
    int photo_play_camera_put(uint32_t cam); /* ui/photo.c: flown by the player while it plays */
    if (rw == 2) return photo_play_camera_put(cam); /* photo mode rewind playing: from photo mode's camera */
    if (fc) return photo_freecam_put(cam);     /* spectating: the free camera */
    double v[3], k[3];
    if (state != 2) { /* spectating cinematic: the kill cam's shots on the car watched and the one it is after, a new one every 4 s */
        static int shots_v[4] = {-1, -1, -1, -1}; /* (each view its own shot) */
        static uint64_t shot_at_v[4];
        static uint32_t was_v[4];
        int *shotp = &shots_v[view];
        uint64_t *atp = &shot_at_v[view], now = M.vblanks;
        if (*shotp < 0 || now >= *atp + 240 || wo != was_v[view]) {
            int s;
            do s = (int)SDL_rand(SHOT_COUNT); while (s == *shotp);
            *shotp = s;
            *atp = now;
            was_v[view] = wo;
        }
        int shot = *shotp;
        uint64_t shot_at = *atp;
        for (int i = 0; i < 3; i++) {
            v[i] = (int32_t)mem_r32(&M.cpu, wo + 0xA8 + 4u * (uint32_t)i);
            k[i] = oo ? (int32_t)mem_r32(&M.cpu, oo + 0xA8 + 4u * (uint32_t)i) : v[i];
        }
        /* (the shots look at the first car, with the second coming at it: here the one watched and its target) */
        place_camera(cam, shot, (int)(shot_at / 240) & 1 ? -1 : 1, v, k, (double)(now - shot_at) / 240 * 0.8);
        return 1;
    }
    double interp_step_offset(void); /* vplus/interp.c: an in-between frame's place, in steps from this one */
    double t = (double)(replay_frame - start_frame) + interp_step_offset();
    double span = (double)(kill_frame > start_frame ? kill_frame - start_frame : 1), u = t / span; /* 0 at the start, 1 at the kill */
    /* the cars where they are drawn: an in-between frame shows them between two steps (interp.c), so the camera follows
     * their places then, not this step's (the shots after a moving killer shook at 60 FPS, the user's) */
    enum { HIST = 8 };
    static double hv[HIST][3], hk[HIST][3];
    static int hf[HIST] = {-1, -1, -1, -1, -1, -1, -1, -1};
    obj_pos(victim_slot, v);
    obj_pos(killer_slot >= 0 && killer_slot != victim_slot ? killer_slot : victim_slot, k);
    int at = replay_frame & (HIST - 1);
    if (interp_step_offset() == 0 || getenv("TM2PC_KILLCAM_STEPPOS")) { /* a step's own frame: its places kept (tests: always, as before) */
        hf[at] = replay_frame;
        for (int i = 0; i < 3; i++) { hv[at][i] = v[i]; hk[at][i] = k[i]; }
    } else {
        double when = (double)replay_frame + interp_step_offset();
        int f0 = (int)floor(when), a0 = f0 & (HIST - 1), a1 = (f0 + 1) & (HIST - 1);
        double w = when - f0;
        if (getenv("TM2PC_KILLCAM_CAMLOG")) /* tests: the killer as drawn (the blend in memory) against the history's */
            fprintf(stderr, "kc drawn: %.0f %.0f  hist %.0f %.0f (ok %d) frame %d off %.3f\n", k[0], k[1],
                    hf[a0] == f0 && hf[a1] == f0 + 1 ? hk[a0][0] + (hk[a1][0] - hk[a0][0]) * w : -1,
                    hf[a0] == f0 && hf[a1] == f0 + 1 ? hk[a0][1] + (hk[a1][1] - hk[a0][1]) * w : -1, hf[a0] == f0 && hf[a1] == f0 + 1, replay_frame, interp_step_offset());
        if (hf[a0] == f0 && hf[a1] == f0 + 1)
            for (int i = 0; i < 3; i++) {
                v[i] = hv[a0][i] + (hv[a1][i] - hv[a0][i]) * w;
                k[i] = hk[a0][i] + (hk[a1][i] - hk[a0][i]) * w;
            }
    }
    place_camera(cam, shots[pass % 3] % SHOT_COUNT, pass & 1 ? -1 : 1, v, k, u);
    if (getenv("TM2PC_KILLCAM_CAMLOG")) /* tests: each frame's camera (pass, shot, time, place) */
        fprintf(stderr, "kc cam: %d %d %.3f %d %d %d\n", pass, shots[pass % 3] % SHOT_COUNT, t,
                (int32_t)mem_r32(&M.cpu, cam + 0xA8), (int32_t)mem_r32(&M.cpu, cam + 0xAC), (int32_t)mem_r32(&M.cpu, cam + 0xB0));
    if (getenv("TM2PC_KILLCAM_DEBUG") && (replay_frame - start_frame) % 30 == 0)
        fprintf(stderr, "kill cam: pass %d victim %d at %.0f %.0f %.0f\n", pass, victim_slot, v[0], v[1], v[2]);
    return 1;
}
/* Spectating: a new car watched (X, the one watched destroyed, the spectating's start, the player back) cut to its
 * chase camera at once, the user's "jumpy effect every time it goes to a new car" (2026-11-03). Now the camera flies
 * there, eased, 0.35 s and up to 1 s for a far car. The chase camera is a world camera (its parent at the origin; its
 * eye at +0xA8, pitch +0xA0, yaw +0xA4, as the kill cam and photo mode put theirs): ui_ViewProc calls this before
 * viewProc(0) (phase 0: the camera put between the one shown when the car changed and the game's of this frame, the yaw
 * the shorter way round and following the game's own turn) and after it (phase 1: the game's put back, so its chase
 * camera eases on from its own). Drawing only. One view only (no split screen). Tests: TM2PC_SPECT_BLEND_LOG. */
int vp_spectate_chase_target(void); /* vplus/fixes.c */
int vp_spectating_any(void);
static struct { int target, have, flying, saved; uint64_t t0; double dur, arc, e[3], yaw, pitch, roll, le[3], lyaw, lpitch, lroll, ty;
                uint8_t keep[20]; uint32_t cam; } sb;
static double wrap_turn(double a) { while (a > 2048) a -= 4096; while (a < -2048) a += 4096; return a; }
void killcam_spectate_blend(int phase)
{
    uint32_t cam = mem_r32(&M.cpu, RB_viewCameraObjects), parent = mem_r32(&M.cpu, RB_viewCameraObjectTable);
    if (phase) { /* after viewProc: the game's camera back */
        if (sb.saved && sb.cam == cam) for (uint32_t i = 0; i < 20; i++) mem_w8(&M.cpu, cam + 0xA0 + i, sb.keep[i]);
        sb.saved = 0;
        return;
    }
    sb.saved = 0;
    int tg = vp_spectating_any() ? vp_spectate_chase_target() : -1;
    int world = cam && parent && !mem_r32(&M.cpu, parent + 0xA8) && !mem_r32(&M.cpu, parent + 0xAC) && !mem_r32(&M.cpu, parent + 0xB0) &&
                (int16_t)mem_r16(&M.cpu, parent + 0x80) == 4096 && (int16_t)mem_r16(&M.cpu, parent + 0x88) == 4096; /* (a world camera) */
    if (!in_battle_now || state == 2 || rw || !tg || !world || mem_r32(&M.cpu, GA(0x801809E4u)) || getenv("TM2PC_NO_SPECT_BLEND")) {
        sb.have = sb.flying = 0;
        sb.target = tg;
        return;
    }
    double e[3], pitch = (int16_t)mem_r16(&M.cpu, cam + 0xA0), roll = (int16_t)mem_r16(&M.cpu, cam + 0xA2), yaw = (int16_t)mem_r16(&M.cpu, cam + 0xA4);
    for (int i = 0; i < 3; i++) e[i] = (int32_t)mem_r32(&M.cpu, cam + 0xA8 + 4u * (uint32_t)i);
    if (sb.have && tg != sb.target) { /* a new car: fly from what was shown */
        double d = 0;
        for (int i = 0; i < 3; i++) { sb.e[i] = sb.le[i]; d += (e[i] - sb.le[i]) * (e[i] - sb.le[i]); }
        sb.yaw = sb.lyaw, sb.pitch = sb.lpitch, sb.roll = sb.lroll;
        sb.dur = 0.35 + sqrt(d) / 6000;
        sb.arc = sqrt(d) / 6 < 800 ? sqrt(d) / 6 : 800;
        if (sb.dur > 1) sb.dur = 1;
        sb.t0 = SDL_GetTicksNS();
        sb.flying = 1;
        sb.ty = sb.yaw + wrap_turn(yaw - sb.yaw); /* (the shorter way round) */
    }
    sb.target = tg;
    sb.have = 1;
    if (sb.flying) sb.ty += wrap_turn(yaw - sb.ty), yaw = sb.ty; /* (the game's own turn followed, never the long way round) */
    double s = sb.flying ? (double)(SDL_GetTicksNS() - sb.t0) / 1e9 / sb.dur : 1;
    if (s >= 1) {
        sb.flying = 0;
        memcpy(sb.le, e, sizeof e), sb.lyaw = wrap_turn(yaw), sb.lpitch = pitch, sb.lroll = roll;
        return;
    }
    double w = s * s * (3 - 2 * s), o[3];
    for (int i = 0; i < 3; i++) o[i] = sb.e[i] + (e[i] - sb.e[i]) * w;
    o[2] += sin(M_PI * w) * sb.arc; /* (up and over, not through, what stands between: a sixth of the way, 800 at most) */
    double oy = sb.yaw + (yaw - sb.yaw) * w, op = sb.pitch + (pitch - sb.pitch) * w, orl = sb.roll + wrap_turn(roll - sb.roll) * w;
    memcpy(sb.le, o, sizeof o), sb.lyaw = oy, sb.lpitch = op, sb.lroll = orl;
    for (uint32_t i = 0; i < 20; i++) sb.keep[i] = mem_r8(&M.cpu, cam + 0xA0 + i);
    sb.cam = cam;
    sb.saved = 1;
    mem_w16(&M.cpu, cam + 0xA0, (uint16_t)(int16_t)lrint(op));
    mem_w16(&M.cpu, cam + 0xA2, (uint16_t)(int16_t)lrint(orl));
    mem_w16(&M.cpu, cam + 0xA4, (uint16_t)((int)lrint(oy) & 4095));
    for (int i = 0; i < 3; i++) mem_w32(&M.cpu, cam + 0xA8 + 4u * (uint32_t)i, (uint32_t)(int32_t)lrint(o[i]));
    if (getenv("TM2PC_SPECT_BLEND_LOG"))
        fprintf(stderr, "spect blend: s %.2f eye %.0f %.0f %.0f (game's %.0f %.0f %.0f) yaw %.0f (%.0f) pitch %.0f (%.0f)\n", s, o[0], o[1],
                o[2], e[0], e[1], e[2], oy, yaw, op, pitch);
}

/* Spectating at 60 FPS and uncapped: the in-between frames (interp.c) show the battle between the two steps before the
 * one just drawn, so a step's own frame is seen two steps after it was drawn. The cameras the port puts while spectating
 * (the free camera, the cinematic shots, the flight to a new car) are placed by the clock at the moment they are drawn:
 * an in-between frame's camera was ahead of the step frames around it, and the picture jumped back every third frame
 * (the user's 2026-11-18 take: "shaking like crazy" moving the free camera or going to another car). Each step's own
 * frame keeps the camera it was drawn with (each view its own); an in-between frame takes them blended, as the cars.
 * Called before viewProc, the camera placed: put, a camera of the port's (killcam_camera or the flight). */
double interp_step_offset(void); /* vplus/interp.c (0: a step's own frame) */
int interp_redrawing(void);
enum { SH_N = 8 };
static struct { int n; int have[SH_N]; int32_t e[SH_N][3]; int16_t a[SH_N][3]; uint32_t cam; } sh[4];
int killcam_spectate_flying(void) { return sb.saved; } /* (ui_ViewProc: the flight's camera put, this frame) */
void killcam_spectate_steps(int view, int put)
{
    if (view < 0 || view > 3) return;
    uint32_t cam = mem_r32(&M.cpu, RB_viewCameraObjects + 4u * (uint32_t)view);
    if (!in_battle_now || state == 2 || rw || !cam || !vp_spectating_any()) { sh[view].n = 0; memset(sh[view].have, 0, sizeof sh[view].have); return; }
    if (sh[view].cam != cam) { sh[view].cam = cam; memset(sh[view].have, 0, sizeof sh[view].have); }
    if (!interp_redrawing()) { /* a step's own frame: its camera kept (whoever put it) */
        int at = ++sh[view].n & (SH_N - 1);
        uint32_t parent = mem_r32(&M.cpu, RB_viewCameraObjectTable + 4u * (uint32_t)view);
        int world = parent && !mem_r32(&M.cpu, parent + 0xA8) && !mem_r32(&M.cpu, parent + 0xAC) && !mem_r32(&M.cpu, parent + 0xB0);
        sh[view].have[at] = world ? 1 + put : 0; /* (1 the game's, 2 the port's; 0 not a world camera: not blended) */
        for (int i = 0; i < 3; i++) {
            sh[view].e[at][i] = (int32_t)mem_r32(&M.cpu, cam + 0xA8 + 4u * (uint32_t)i);
            sh[view].a[at][i] = (int16_t)mem_r16(&M.cpu, cam + 0xA0 + 2u * (uint32_t)i);
        }
        return;
    }
    double when = sh[view].n + interp_step_offset(); /* (an in-between frame: offset -2 .. -1) */
    int f0 = (int)floor(when), a0 = f0 & (SH_N - 1), a1 = (f0 + 1) & (SH_N - 1);
    if (f0 < 1 || f0 + 1 > sh[view].n || sh[view].n - f0 >= SH_N || !sh[view].have[a0] || !sh[view].have[a1]) return;
    /* (the game's own chase camera on both sides: blended between the steps already, interp.c; the port's on either side,
     * or now: the steps' own, so the hand-over at a flight's end is as smooth) */
    if (!put && sh[view].have[a0] == 1 && sh[view].have[a1] == 1) return;
    double w = when - f0;
    for (int i = 0; i < 3; i++) {
        double e = sh[view].e[a0][i] + ((double)sh[view].e[a1][i] - sh[view].e[a0][i]) * w;
        mem_w32(&M.cpu, cam + 0xA8 + 4u * (uint32_t)i, (uint32_t)(int32_t)lrint(e));
        double d = wrap_turn((double)sh[view].a[a1][i] - sh[view].a[a0][i]); /* (angles of 4096 a turn: the shorter way) */
        int v = (int)lrint(sh[view].a[a0][i] + d * w);
        mem_w16(&M.cpu, cam + 0xA0 + 2u * (uint32_t)i, (uint16_t)(i == 2 ? v & 4095 : v));
    }
}

/* a shot's camera: v the car looked at, k the other (coming at it), u 0 .. 1 over the shot (1: the kill) */
static void place_camera(uint32_t cam, int shot, int sign, const double v[3], const double k[3], double u)
{
    double e[3], look[3];
    double dx = k[0] - v[0], dy = k[1] - v[1], d = sqrt(dx * dx + dy * dy);
    if (d < 1) { dx = 0; dy = 1; d = 1; }
    dx /= d; dy /= d; /* from the victim toward the killer */
    /* the action in frame: the wide shots (around, aerial, low) stand on the far side of the victim from
     * the killer, so the killer is behind it in the picture, coming at it; they look at the victim, a
     * little toward the killer (by then at the victim) */
    double f = u < 0 ? 0 : u > 1 ? 1 : u;
    f = f * f * (3 - 2 * f);
    double toward = (d < 1500 ? d : 1500) * 0.15 * (1 - f); /* the look's lead toward the killer */
    double T[3] = {v[0] + dx * toward, v[1] + dy * toward, v[2] + (k[2] - v[2]) * 0.1 * (1 - f)};
    double back = atan2(-dx, -dy); /* the far side's direction (sin, cos as below) */
    for (int i = 0; i < 3; i++) look[i] = v[i];
    look[2] += 30;
    switch (shot) {
    case SHOT_ORBIT: { /* around the wreck, a little above it: a sweep ending on the far side at the kill */
        double a = back + 1.1 * (1 - f) * sign;
        e[0] = v[0] + 460 * sin(a); e[1] = v[1] + 460 * cos(a); e[2] = v[2] + 160;
        break;
    }
    case SHOT_AERIAL: /* high above the far side, coming down slowly, looking down at it and past it */
        e[0] = v[0] + sin(back) * 450; e[1] = v[1] + cos(back) * 450; e[2] = v[2] + 1400 - 500 * f;
        break;
    case SHOT_CHASE: /* behind the killer (on the far side from the victim), following it, at the victim */
        e[0] = k[0] + dx * 380; e[1] = k[1] + dy * 380; e[2] = k[2] + 120;
        break;
    case SHOT_FLIGHT: { /* the shot's path: from behind the killer along the line to the victim, there at the kill; then by it */
        double w = u < 0.15 ? 0 : u > 1 ? 1 : (u - 0.15) / 0.85;
        w = w * w * (3 - 2 * w);
        double sx = k[0] + dx * 300, sy = k[1] + dy * 300, sz = k[2] + 110; /* behind the killer */
        double ex = v[0] + dx * 260, ey = v[1] + dy * 260, ez = v[2] + 90;  /* by the victim, on the killer's side */
        e[0] = sx + (ex - sx) * w; e[1] = sy + (ey - sy) * w; e[2] = sz + (ez - sz) * w;
        break;
    }
    default: { /* low, beside the victim: on the far side, a little off the line */
        double a = back + 0.45;
        e[0] = v[0] + 340 * sin(a); e[1] = v[1] + 340 * cos(a); e[2] = v[2] + 40;
        break;
    }
    }
    if (shot == SHOT_ORBIT || shot == SHOT_AERIAL || shot == SHOT_LOW)
        for (int i = 0; i < 3; i++) look[i] = T[i] + (i == 2 ? 30 : 0);
    /* the close shots 12% farther from what they look at (the user's 2026-11-03: "a bit too close", 10-15%); the aerial
     * one is far already */
    if (shot != SHOT_AERIAL)
        for (int i = 0; i < 3; i++) e[i] = look[i] + (e[i] - look[i]) * 1.12;
    /* in the arena: over a floor near the victim's (not over a drop or outside the map: nearer then), above it; the pull
     * and the lift eased over a few frames, not jumped: over lava's and paths' edges the floor test flipped from frame
     * to frame and the shot shook (the user's chase shot, 2026-10-29) */
    double pull = 1, lift = 0, p0[2] = {e[0], e[1]};
    for (int tries = 0; tries < 4; tries++) {
        double x = look[0] + (p0[0] - look[0]) * pull, y = look[1] + (p0[1] - look[1]) * pull;
        double g = ground_at(x, y, e[2] + 400);
        if (g > v[2] - 250) { if (e[2] < g + 70) lift = g + 70 - e[2]; break; } /* (lower: the view's bottom in the ground) */
        pull *= 0.5;
    }
    static double pull_s = 1, lift_s = 0;
    static int eased_shot = -1;
    static uint64_t eased_at;
    int same = eased_shot == shot && M.vblanks - eased_at < 30 && !getenv("TM2PC_KILLCAM_NOEASE"); /* (a new shot, or after a gap: at once; tests: never) */
    pull_s = same ? pull_s + (pull - pull_s) * 0.15 : pull;
    lift_s = same ? lift_s + (lift - lift_s) * 0.15 : lift;
    eased_shot = shot, eased_at = M.vblanks;
    e[0] = look[0] + (p0[0] - look[0]) * pull_s;
    e[1] = look[1] + (p0[1] - look[1]) * pull_s;
    e[2] += lift_s;
    /* and not behind a wall: the eye in front of the level's first wall between the car and it (the decomp's chase
     * camera's, portVisualCameraWall, its bit 5 on for the call). A shot ran into Suicide Swamp's canal wall: the
     * wall's inside filled the picture and the shot jumped out of it (the user's, 2026-10-29). In front at once, back
     * out eased, as the pull */
    static uint32_t wall_fn = 1, visual;
    if (wall_fn == 1) { wall_fn = port_sym("portVisualCameraWall"); visual = port_sym("portVisual"); }
    static double wall_s = 1;
    double wall = 1;
    if (wall_fn && visual) {
        const uint32_t L = POS_BUF + 0x10, E = POS_BUF + 0x20;
        for (int i = 0; i < 3; i++) {
            mem_w32(&M.cpu, L + 4u * (uint32_t)i, (uint32_t)(int32_t)look[i]);
            mem_w32(&M.cpu, E + 4u * (uint32_t)i, (uint32_t)(int32_t)e[i]);
        }
        uint32_t was = mem_r32(&M.cpu, visual);
        mem_w32(&M.cpu, visual, was | 0x20u);
        guest_call(wall_fn, L, E, 0, 0);
        mem_w32(&M.cpu, visual, was);
        double full = 0, got = 0;
        for (int i = 0; i < 3; i++) {
            double a = e[i] - look[i], b = (int32_t)mem_r32(&M.cpu, E + 4u * (uint32_t)i) - look[i];
            full += a * a, got += b * b;
        }
        if (full > 1) wall = sqrt(got / full);
        if (wall > 1) wall = 1;
        if (wall < 0.99 && getenv("TM2PC_KILLCAM_WALL_LOG")) fprintf(stderr, "kc wall: shot %d eye pulled to %.2f\n", shot, wall);
    }
    wall_s = !same || wall < wall_s ? wall : wall_s + (wall - wall_s) * 0.15;
    for (int i = 0; i < 3; i++) e[i] = look[i] + (e[i] - look[i]) * wall_s;
    double fx = look[0] - e[0], fy = look[1] - e[1], fz = look[2] - e[2], h = sqrt(fx * fx + fy * fy);
    int yaw = (int)(atan2(fx, fy) * 4096 / (2 * M_PI)) & 4095, pitch = (int)(atan2(fz, h > 1 ? h : 1) * 4096 / (2 * M_PI));
    mem_w16(&M.cpu, cam + 0xA0, (uint16_t)(int16_t)-pitch); /* (the camera's x rotation: down positive) */
    mem_w16(&M.cpu, cam + 0xA2, 0);
    mem_w16(&M.cpu, cam + 0xA4, (uint16_t)(int16_t)yaw);
    for (int i = 0; i < 3; i++) mem_w32(&M.cpu, cam + 0xA8 + 4u * (uint32_t)i, (uint32_t)(int32_t)e[i]);
}
