/* Rollback's states and re-runs (docs/ONLINE_PLAN.md 2; decomp ONLINE_ROLLBACK.md 12, 14): what the rollback engine
 * (rollback.c) keeps for a step and how a step is run again with nothing drawn.
 *
 * With the decomp's STEP_SPLIT (portRollbackOptions bit 8) the main loop's iteration runs the whole step first
 * (portStepBody), then draws; its pacing wait (the first waiting VSync after the step, hle.c) is between two steps.
 * There a state is the game's state at the start of the next step:
 * - the guest RAM of the build's rollback_regions.tsv (the snapshot rows: the game's and the port variant's statics,
 *   minus what is not state), resolved by the recompiler (rc_rb_snap, rc_rb_hash), with the BIOS rand() seed;
 * - the models' mutable node fields (portDmdMutableList: the animations' frames, selects, transforms), listed again
 *   for every state (a car out, back in or swapped changes the models);
 * - the port's own: its random numbers (vp_rand, high RAM), its gameplay blocks (the kill cam's keep list), every
 *   human's input of the step (humans.c), the gameplay clock (vp_gv) the step ran with.
 * A re-run loads a state and calls portRunStep once a step with portResimulating set (the decomp's sound gate and
 * palette shadow: no sound, no upload), then portSoundReconcile and portClutAnimUpload. While re-running, the
 * frame hook (hle_SetFrameStart) runs only the step's own work (rbs_resimulating). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_timer.h>
#include "runtime/runtime.h"
#include "rollback.h"
#include "rb_state.h"
#include "runtime/rebase_table.h"
#include "vplus/cars.h"

extern const struct { uint32_t addr, size; } rc_rb_snap[], rc_rb_hash[];
extern const unsigned rc_rb_snap_count, rc_rb_hash_count;
int killcam_keep_block(int i, void **p, size_t *n); /* vplus/killcam.c */
void killcam_keep_init(void);
size_t humans_inputs_size(void);                    /* vplus/humans.c */
void humans_inputs_get(uint8_t *out);
void humans_inputs_put(const uint8_t *in);

enum { SLOTS = RB_WINDOW + 4, DMD_PAIRS = 49152 };
#define DMD_BUF 0x80E00000u   /* the node list (address, bytes pairs; 384 KB): the top of the guest RAM, which nothing
                               * else uses (psx_cpu.h's map) */
#define PORT_RAND 0x80DF6FF0u /* vp_rand's state (vplus/killcam.c) */

typedef struct {
    int32_t step;     /* the step it is the start of; -1 none */
    uint8_t *b;
    size_t len, cap;
} State;
static State st[SLOTS];
int rbs_resimulating;
/* the gameplay clock (vp_gv: VBlanks) each step ran with, kept by its first run (live): a re-run step runs with the same
 * (nothing passes a VBlank between the iteration's end and the next step) */
static int32_t gv_tag[SLOTS];
static uint64_t gv_at[SLOTS];
static uint32_t a_run, a_resim, a_reconcile, a_clut, a_dmd;

static int syms(void)
{
    static int done;
    if (!done) {
        done = 1;
        a_run = port_sym("portRunStep");
        a_resim = port_sym("portResimulating");
        a_reconcile = port_sym("portSoundReconcile");
        a_clut = port_sym("portClutAnimUpload");
        a_dmd = port_sym("portDmdMutableList");
        for (int i = 0; i < SLOTS; i++) st[i].step = gv_tag[i] = -1;
    }
    return a_run && a_resim && a_reconcile && a_clut && a_dmd && rc_rb_snap_count > 1;
}
int rbs_available(void) { return syms(); }
/* The port's state that only a rollback puts back (the kill cam's replays keep their own "counted once" rule): who hit
 * whom (the kills' attribution), the match's tally (Gun Game's levels come from it), the stats counted this battle */
enum { RB_KEEP_MAX = 48 };
static struct { void *p; size_t n; } rkeep[RB_KEEP_MAX];
static int rkeep_n;
void rbs_keep(void *p, size_t n)
{
    if (rkeep_n < RB_KEEP_MAX) { rkeep[rkeep_n].p = p; rkeep[rkeep_n].n = n; rkeep_n++; }
    else fprintf(stderr, "rollback: RB_KEEP_MAX (%d) blocks: one more left out\n", RB_KEEP_MAX);
}
/* Host state that stays live across a batch (real time, not the step's: put back after the re-runs as it was before
 * the state was loaded), as the clock's accumulator below */
enum { RB_LIVE_MAX = 64 };
static struct { void *p; size_t n; uint8_t was[8]; } rlive[RB_LIVE_MAX];
static int rlive_n;
void rbs_live(void *p, size_t n)
{
    if (rlive_n < RB_LIVE_MAX && n <= sizeof rlive[0].was) { rlive[rlive_n].p = p; rlive[rlive_n].n = n; rlive_n++; }
    else fprintf(stderr, "rollback: a live block left out (%u bytes, %d of %d)\n", (unsigned)n, rlive_n, RB_LIVE_MAX);
}
static int kc_n; /* the kill cam's blocks (registered once) */
static void keep_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    killcam_keep_init();
    void *p;
    size_t n;
    while (killcam_keep_block(kc_n, &p, &n)) kc_n++;
    void feats_rb_register(void), humans_rb_register(void); /* profile/feat_hooks.c, profile/feats.c, vplus/humans.c */
    feats_rb_register();
    humans_rb_register();
    /* the one-view mirror, this machine's own (a local hotkey; the table's rows snapshot them): as it was before a
     * batch, not the loaded state's (vplus/four.c, mirrors_rear_pair) */
    rbs_live(mem_direct(&M.cpu, GA(0x801809DCu), 4), 4); /* rtRearViewActive */
    rbs_live(mem_direct(&M.cpu, GA(0x801809E0u), 4), 4); /* rtRearViewAllowed */
}
static int keep_block(int i, void **p, size_t *n) /* the kill cam's blocks, then rollback's own */
{
    if (i < kc_n) return killcam_keep_block(i, p, n);
    if (i - kc_n >= rkeep_n) return 0;
    *p = rkeep[i - kc_n].p;
    *n = rkeep[i - kc_n].n;
    return 1;
}
static struct { int k, every, on; int32_t step, base; uint8_t in[SLOTS][96]; int checks, bad, cut; } chk; /* (the re-run check, below) */
static int dmd_n = -1; /* (the models' list: its pairs; -1 to be made, dmd_list below) */
void rbs_new_round(void) /* (killcam.c: a round starts) the states kept are the round before's: none to go back to */
{
    for (int i = 0; i < SLOTS; i++) st[i].step = gv_tag[i] = -1;
    chk.base = 0;
    dmd_n = -1; /* (the models' list made again: a load) */
}

uint64_t vp_gv(void);
static void set_gv(uint64_t gv) { M.vblank_offset = M.vblanks - gv; }
static uint64_t gv_of(int32_t step) { return gv_tag[(uint32_t)step % SLOTS] == step ? gv_at[(uint32_t)step % SLOTS] : vp_gv(); }
static int step_ran; /* a live step ran in this iteration of the main loop */
extern int32_t rbs_step_running;
void rbs_live_step(void) /* (hle_SetFrameStart: a live step's) */
{
    if (!rbs_resimulating) step_ran = 1;
    static long poke_at = -2; /* tests: TM2PC_NET_TEST_POKE=N, at this machine's live step N human 1's health 10 lower
                               * (a real difference: the resync's log names it) */
    if (poke_at == -2) poke_at = getenv("TM2PC_NET_TEST_POKE") ? atol(getenv("TM2PC_NET_TEST_POKE")) : -1;
    if (poke_at >= 0 && !rbs_resimulating && rbs_step_running == poke_at) {
        mem_w16(&M.cpu, HUMAN_REC(0) + 0x1F0, (uint16_t)(mem_r16(&M.cpu, HUMAN_REC(0) + 0x1F0) - 10));
        fprintf(stderr, "test: human 1's health 10 lower at step %ld\n", poke_at);
        poke_at = -1;
    }
}

/* The models' mutable node fields (portDmdMutableList, decomp ONLINE_ROLLBACK.md 15.1): every root of every model
 * file loaded for the match, so the list changes only with the loads (the level file at LEVEL_FILE, the installed
 * files noted in portDmdImages: a car group loaded or unloaded by a drop-in). Made again when those differ from the
 * last list's, or a round starts. */
#define LEVEL_FILE 0x80019C40u
static uint32_t dmd_key;
static int dmd_list(void)
{
    static uint32_t a_images = 1;
    if (a_images == 1) a_images = port_sym("portDmdImages");
    /* (the loads as they are: where each file is (portDmdImages), and the start of the level file and of each file,
     * which tells another file loaded where one was: a player coming back, their car's group loaded again) */
    uint32_t key = 2166136261u;
    for (uint32_t i = 0; i < 0x104; i++) key = (key ^ (a_images ? mem_r8(&M.cpu, a_images + i) : 0)) * 16777619u;
    for (uint32_t i = 0; i < 128; i++) key = (key ^ mem_r8(&M.cpu, LEVEL_FILE + i)) * 16777619u;
    for (uint32_t g = 0; a_images && g < 64; g++) {
        uint32_t base = mem_r32(&M.cpu, a_images + 4u * g);
        if ((base & 0x1FFFFFFFu) >= GUEST_RAM_BYTES - 128 || !base) continue;
        for (uint32_t i = 0; i < 128; i++) key = (key ^ mem_r8(&M.cpu, base + i)) * 16777619u;
    }
    static uint32_t kept[2 * DMD_PAIRS]; /* (the list as made: the guest buffer is the port's own RAM, which other
                                          * code may use between two calls) */
    if (dmd_n >= 0 && key == dmd_key) {
        memcpy(mem_direct(&M.cpu, DMD_BUF, 8 * dmd_n), kept, 8u * (uint32_t)dmd_n);
        return dmd_n;
    }
    dmd_key = key;
    uint64_t t0 = SDL_GetPerformanceCounter();
    dmd_n = (int)guest_call(a_dmd, DMD_BUF, DMD_PAIRS, 0, 0);
    if (getenv("TM2PC_DMD_TIME")) fprintf(stderr, "rollback: the model list made: %d pairs, %.3f ms\n", dmd_n,
                                          (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    if (dmd_n < 0) {
        static int told;
        if (!told++) fprintf(stderr, "rollback: the model list needs %d pairs (room for %d): left out\n", -dmd_n, DMD_PAIRS);
        dmd_n = 0;
    }
    memcpy(kept, mem_direct(&M.cpu, DMD_BUF, 8 * dmd_n), 8u * (uint32_t)dmd_n);
    return dmd_n;
}
void rbs_dmd_again(void) { dmd_n = -1; } /* (net/game_tm2.c: a player left, came back or dropped in: a car's models) */
int rbs_dmd_list(uint32_t *buf) /* (vplus/killcam.c's hash too) the pairs, at *buf */
{
    *buf = DMD_BUF;
    return syms() ? dmd_list() : 0;
}
static uint8_t *room(State *s, size_t need)
{
    if (s->cap < need) {
        uint8_t *b = realloc(s->b, need + need / 8);
        if (!b) return NULL;
        s->b = b;
        s->cap = need + need / 8;
    }
    return s->b;
}

static void save_to(State *s, int32_t step);
void rbs_save(int32_t step)
{
    if (!syms()) return;
    keep_init();
    /* (tests: TM2PC_DMD_REBUILD8=1, the model list made again at every 8th step. Before decomp request 106 the walk read
     * a taken pickup's child count (0), so a list made at another moment differed: a drop-in's machine, its list made
     * from the host's state, 1,477 ranges, the host's from the match's start 1,481, and the hashes parted. The walk
     * takes a pickup holder's one child now: the list is the same at any step, made once after each load) */
    static int rebuild8 = -1;
    if (rebuild8 < 0) rebuild8 = getenv("TM2PC_DMD_REBUILD8") != NULL;
    if (rebuild8 && step % 8 == 0) dmd_n = -1;
    save_to(&st[(uint32_t)step % SLOTS], step);
    if (!rbs_resimulating) { gv_tag[(uint32_t)step % SLOTS] = step; gv_at[(uint32_t)step % SLOTS] = vp_gv(); }
}
static void save_to(State *s, int32_t step)
{
    int n = dmd_list();
    size_t need = 4 + humans_inputs_size() + 4;
    for (unsigned i = 0; i < rc_rb_snap_count; i++) need += rc_rb_snap[i].size;
    for (int i = 0; i < n; i++) need += 8 + mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i + 4u);
    void *p;
    size_t k;
    for (int i = 0; keep_block(i, &p, &k); i++) need += k;
    uint8_t *w = room(s, need);
    if (!w) { s->step = -1; return; }
    for (unsigned i = 0; i < rc_rb_snap_count; i++) {
        memcpy(w, mem_direct(&M.cpu, rc_rb_snap[i].addr, (int)rc_rb_snap[i].size), rc_rb_snap[i].size);
        w += rc_rb_snap[i].size;
    }
    uint32_t r = mem_r32(&M.cpu, PORT_RAND);
    memcpy(w, &r, 4); w += 4;
    humans_inputs_get(w); w += humans_inputs_size();
    memcpy(w, &n, 4); w += 4;
    for (int i = 0; i < n; i++) {
        uint32_t a = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i), len = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i + 4u);
        memcpy(w, &a, 4); memcpy(w + 4, &len, 4); w += 8;
        for (uint32_t b = 0; b < len; b++) *w++ = (uint8_t)mem_r8(&M.cpu, a + b);
    }
    for (int i = 0; keep_block(i, &p, &k); i++) { memcpy(w, p, k); w += k; }
    s->len = (size_t)(w - s->b);
    s->step = step;
}

static void load_from(const State *s);
int rbs_load(int32_t step)
{
    State *s = &st[(uint32_t)step % SLOTS];
    if (!syms() || s->step != step) return 0;
    load_from(s);
    set_gv(gv_of(step));
    return 1;
}
static void load_from(const State *s)
{
    const uint8_t *r = s->b;
    for (unsigned i = 0; i < rc_rb_snap_count; i++) {
        memcpy(mem_direct(&M.cpu, rc_rb_snap[i].addr, (int)rc_rb_snap[i].size), r, rc_rb_snap[i].size);
        r += rc_rb_snap[i].size;
    }
    uint32_t v;
    memcpy(&v, r, 4); r += 4;
    mem_w32(&M.cpu, PORT_RAND, v);
    humans_inputs_put(r); r += humans_inputs_size();
    int n;
    memcpy(&n, r, 4); r += 4;
    for (int i = 0; i < n; i++) {
        uint32_t a, len;
        memcpy(&a, r, 4); memcpy(&len, r + 4, 4); r += 8;
        for (uint32_t b = 0; b < len; b++) mem_w8(&M.cpu, a + b, *r++);
    }
    void *p;
    size_t k;
    for (int i = 0; keep_block(i, &p, &k); i++) { memcpy(p, r, k); r += k; }
}

/* Each machine's own, left out of the hash beside the table's "hash no" rows (decomp requests 57, 58): the objects its
 * `ptr` rows name (the views' camera objects, their pose objects and shared root, the objects drawn at each view's
 * camera: the sky), read through their pointer words at each hash. In a human's record its camera and view bytes
 * (human_rec_state_word; the table's field rows have them too). */
extern const struct { uint32_t addr, size; } rc_rb_ptrs[];
extern const unsigned rc_rb_ptrs_count;
enum { OBJ_MASKS = 42 }; /* (the table's ptr rows, then the port's own: the last three) */
static uint32_t cam_lo[OBJ_MASKS], cam_hi[OBJ_MASKS];
static void masks_now(void) /* (the objects as the pointers name them now) */
{
    for (unsigned k = 0; k < OBJ_MASKS; k++) {
        uint32_t p = k < rc_rb_ptrs_count && rc_rb_ptrs[k].size ? mem_r32(&M.cpu, rc_rb_ptrs[k].addr) : 0;
        cam_lo[k] = p;
        cam_hi[k] = p ? p + rc_rb_ptrs[k].size : 0;
    }
    /* and the task manager's stop loop's two words (taskMgrStopNext, taskMgrStopCurrent: retail 80181158..8018116C, the
     * loop's own iteration, nothing of the game's play; rtScheduledEvents after them stays in): the port stops the battle's
     * sounds at each machine's own moment (the kill cam, the results), and the states parted at a stage's end over them */
    cam_lo[OBJ_MASKS - 1] = GA(0x80181158u);
    cam_hi[OBJ_MASKS - 1] = GA(0x80181158u) + 0x14;
    /* each machine's views: the view-mode table's rows (the port sets them by this machine's views and widescreen: read
     * through viewModes, itself each machine's) and the port's settings block's first word (+1 humans, +2 left: this
     * machine's views; the block is the host's settings, written again at every frame's start). A machine with 2-4
     * players of its own (split screen online) has other rows and views than one with one */
    cam_lo[OBJ_MASKS - 2] = RB_viewModeTable;
    cam_hi[OBJ_MASKS - 2] = RB_viewModeTable + 0x600u;
    cam_lo[OBJ_MASKS - 3] = rc_port_config;
    cam_hi[OBJ_MASKS - 3] = rc_port_config ? rc_port_config + 4u : 0;
}
static int hashed_word(uint32_t a, uint32_t *w) /* 0: left out; else *w as hashed */
{
    for (int v = 0; v < OBJ_MASKS; v++) if (a >= cam_lo[v] && a < cam_hi[v]) return 0;
    *w = mem_r32(&M.cpu, a);
    if (a >= HUMAN_REC(0) && a < HUMAN_REC(CARS_HUMANS_MAX)) *w = human_rec_state_word((a - HUMAN_REC(0)) % HUMAN_REC_SIZE, *w);
    return 1;
}
/* the hash rows less the masks, as spans (made again when a camera object changes: the masks' only moving part) */
static struct { uint32_t a, n; } *hspan;
static int hspan_n, hspan_cap;
static uint32_t hspan_key[OBJ_MASKS] = {1};
static void hspans_now(void)
{
    masks_now();
    if (!memcmp(hspan_key, cam_lo, sizeof hspan_key)) return;
    memcpy(hspan_key, cam_lo, sizeof hspan_key);
    hspan_n = 0;
    uint32_t lo[OBJ_MASKS], hi[OBJ_MASKS];
    int m = 0;
    for (int k = 0; k < OBJ_MASKS; k++) if (cam_hi[k]) { lo[m] = cam_lo[k]; hi[m] = cam_hi[k]; m++; }
    for (unsigned i = 0; i < rc_rb_hash_count; i++) {
        uint32_t at = rc_rb_hash[i].addr, end = at + (rc_rb_hash[i].size & ~3u);
        while (at < end) {
            int in = -1; /* (inside a mask: past it) */
            for (int k = 0; k < m && in < 0; k++) if (lo[k] <= at && at < hi[k]) in = k;
            if (in >= 0) { at = (hi[in] + 3u) & ~3u; continue; }
            uint32_t next = end; /* (else up to the next mask) */
            for (int k = 0; k < m; k++) if (lo[k] > at && lo[k] < next) next = lo[k];
            next &= ~3u;
            if (next > at) {
                if (hspan_n == hspan_cap) {
                    hspan_cap = hspan_cap ? 2 * hspan_cap : 256;
                    hspan = realloc(hspan, (size_t)hspan_cap * sizeof *hspan);
                }
                hspan[hspan_n].a = at;
                hspan[hspan_n].n = next - at;
                hspan_n++;
            }
            at = next > at ? next : at + 4;
        }
    }
}
/* tests (TM2PC_NET_DESYNC_DUMP): each hashed step's 4 KB pieces' own hashes, kept in memory for the last DS_RING steps
 * (no file while playing: the timing as without it), written out at a desync (net/game_tm2.c): two machines diffed */
enum { DS_RING = 64, DS_CHUNKS = 16384 };
typedef struct { int32_t step; int n; uint32_t a[DS_CHUNKS], h[DS_CHUNKS]; uint32_t rnd, dmd; } DsEntry;
static DsEntry *ds;
int32_t rbs_ds_step = -1; /* (set by the caller: the step hashed next) */
static void ds_put(DsEntry *e, uint32_t a, uint32_t h) { if (e && e->n < DS_CHUNKS) { e->a[e->n] = a; e->h[e->n] = h; e->n++; } }
void rbs_ds_dump(const char *path, int32_t only) /* the steps kept (only: that one, -1 all), appended */
{
    if (!ds) return;
    FILE *f = fopen(path, "ab");
    if (!f) return;
    for (int i = 0; i < DS_RING; i++) {
        const DsEntry *e = &ds[i];
        if (e->step < 0 || (only >= 0 && e->step != only)) continue;
        fwrite(&e->step, 4, 1, f);
        int n = e->n + 2;
        fwrite(&n, 4, 1, f);
        for (int k = 0; k < e->n; k++) { uint32_t sz = 4096; fwrite(&e->a[k], 4, 1, f); fwrite(&sz, 4, 1, f); fwrite(&e->h[k], 4, 1, f); }
        uint32_t ra = 1, rd = 2, z = 0; /* (the port's random numbers and the nodes: pseudo addresses 1 and 2) */
        fwrite(&ra, 4, 1, f); fwrite(&z, 4, 1, f); fwrite(&e->rnd, 4, 1, f);
        fwrite(&rd, 4, 1, f); fwrite(&z, 4, 1, f); fwrite(&e->dmd, 4, 1, f);
    }
    fclose(f);
}
uint32_t rbs_hash(void) /* the state as the machines compare it: the table's hash rows (less each machine's own words),
                         * the nodes, the port's random numbers (not the kept blocks: some are each machine's own) */
{
    if (!syms()) return 0;
    hspans_now();
    static int ds_on = -1;
    if (ds_on < 0) { ds_on = getenv("TM2PC_NET_DESYNC_DUMP") != NULL; if (ds_on && (ds = calloc(DS_RING, sizeof *ds))) for (int i = 0; i < DS_RING; i++) ds[i].step = -1; }
    DsEntry *e = ds && rbs_ds_step >= 0 ? &ds[(uint32_t)rbs_ds_step % DS_RING] : NULL;
    if (e) { e->step = rbs_ds_step; e->n = 0; }
    uint32_t h = 2166136261u, hum_lo = HUMAN_REC(0), hum_hi = HUMAN_REC(CARS_HUMANS_MAX);
    for (int i = 0; i < hspan_n; i++) {
        uint32_t a = hspan[i].a, n = hspan[i].n, c = 2166136261u;
        const uint8_t *b = mem_direct(&M.cpu, a, (int)n);
        int hum = a < hum_hi && a + n > hum_lo; /* (a human's record less its camera and view bytes) */
        for (uint32_t k = 0; k < n; k += 4) {
            uint32_t w;
            memcpy(&w, b + k, 4);
            if (hum && a + k >= hum_lo && a + k < hum_hi) w = human_rec_state_word((a + k - hum_lo) % HUMAN_REC_SIZE, w);
            h = (h ^ w) * 16777619u;
            if (e) {
                c = (c ^ w) * 16777619u;
                if (((k + 4) & 4095) == 0 || k + 4 >= n) { ds_put(e, a + (k & ~4095u), c); c = 2166136261u; }
            }
        }
    }
    h = (h ^ mem_r32(&M.cpu, PORT_RAND)) * 16777619u;
    if (e) e->rnd = mem_r32(&M.cpu, PORT_RAND);
    uint32_t hd = 2166136261u;
    int n = dmd_list();
    for (int i = 0; i < n; i++) {
        uint32_t a = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i), len = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i + 4u);
        const uint8_t *b = mem_direct(&M.cpu, a, (int)len);
        uint32_t he = 2166136261u;
        for (uint32_t k = 0; k < len; k++) { h = (h ^ b[k]) * 16777619u; hd = (hd ^ b[k]) * 16777619u; he = (he ^ b[k]) * 16777619u; }
        if (e) ds_put(e, a | 1u, he ^ len); /* (each node range on its own: its address, odd, marks it) */
    }
    if (e) e->dmd = hd;
    return h;
}

/* the clock's interrupt accumulator (timerTicsAccum: SysClkIntHandler adds a period per root-counter wrap, GetCurTics
 * reads it): live time, not the step's (a re-run passes no VBlank), so a batch leaves it as it was; the main loop's
 * pacing measures from it (put back to an older step's, its next wait ended at once: the clocks of the machines parted).
 * In the regions' snapshot and hash rows (decomp request 54, ONLINE_ROLLBACK.md) */
#define TICS_ACCUM 0x801FA3B8u /* (retail) */
static uint64_t live_gv;
static uint32_t live_accum;
void rbs_batch_begin(void)
{
    live_gv = vp_gv();
    live_accum = mem_r32(&M.cpu, GA(TICS_ACCUM));
    for (int i = 0; i < rlive_n; i++) memcpy(rlive[i].was, rlive[i].p, rlive[i].n);
    mem_w32(&M.cpu, a_resim, 1);
    rbs_resimulating = 1;
}
int rbs_rerun_step(int32_t step)
{
    set_gv(gv_of(step));
    return (int)guest_call(a_run, 0, 0, 0, 0);
}
void rbs_batch_end(void)
{
    mem_w32(&M.cpu, a_resim, 0);
    rbs_resimulating = 0;
    guest_call(a_reconcile, 0, 0, 0, 0);
    guest_call(a_clut, 0, 0, 0, 0);
    set_gv(live_gv);
    mem_w32(&M.cpu, GA(TICS_ACCUM), live_accum);
    for (int i = 0; i < rlive_n; i++) memcpy(rlive[i].p, rlive[i].was, rlive[i].n);
}
int rbs_rerun(int32_t from, int32_t to, void (*inputs)(void *ctx, int32_t step), void *ctx)
{
    if (!syms() || st[(uint32_t)from % SLOTS].step != from) return -1;
    rbs_batch_begin();
    rbs_load(from);
    int ran = 0;
    for (int32_t f = from; f < to; f++) {
        if (inputs) inputs(ctx, f);
        ran++;
        if (rbs_rerun_step(f)) break; /* (the round ended in it: the loop leaves at its next top) */
    }
    rbs_batch_end();
    return ran;
}

/* The re-run check (tests: TM2PC_TEST_RERUN=K[,EVERY]): every EVERY steps (default 100) in a battle, the state kept
 * and K steps (1..SLOTS-1) played on with each step's input kept; then those K steps run again from the kept state
 * with the same inputs, nothing drawn, and the state compared with the first run's (the regions' hash rows; on a
 * difference, every snapshot byte and node byte that differs, by address). The battle goes on from the re-run. */
int feats_in_battle(void);
extern int killcam_replaying;
static void chk_inputs(void *ctx, int32_t f) { (void)ctx; humans_inputs_put(chk.in[(f - chk.base) % SLOTS]); }
static uint8_t *ram_copy;
/* the play's state, as the machines compare it online (killcam_state_hash: the cars, the game's seed, the model nodes),
 * with the port's random numbers and its gameplay blocks: the regions' hash rows still hold the drawing's state (its
 * order tables, matrices, the objects' screen positions), which a re-run does not make (decomp request 54 (ONLINE_ROLLBACK.md)); and
 * rollback's own blocks (the tally, the kills' attribution, the stats: this machine's) */
uint32_t killcam_state_hash(void);
static uint32_t play_hash(void)
{
    uint32_t h = killcam_state_hash();
    h = (h ^ mem_r32(&M.cpu, PORT_RAND)) * 16777619u;
    void *p;
    size_t k;
    for (int i = 0; keep_block(i, &p, &k); i++)
        for (size_t b = 0; b < k; b++) h = (h ^ ((uint8_t *)p)[b]) * 16777619u;
    return h;
}
int rbs_rand_log; /* (runtime/bios.c: rand()'s callers logged, the first check's window: TM2PC_TEST_RERUN_RAND) */
int32_t rbs_step_running = -1; /* (tests: the network step running, net/game_tm2.c; bios.c's TM2PC_RAND_STEPS) */
static void point(void);
void netplay_point(void); /* net/game_tm2.c: rollback's step */
void rbs_iteration_end(void) /* (ui/options.c's pauseMenu, the iteration's last call: drawn, the next step not begun) */
{
    if (!step_ran) return;
    step_ran = 0;
    netplay_point();
    point();
}
static void point(void)
{
    if (!chk.on) {
        const char *e = getenv("TM2PC_TEST_RERUN");
        chk.on = e ? 1 : -1;
        if (e && sscanf(e, "%d,%d", &chk.k, &chk.every) < 2) chk.every = 100;
        if (chk.k < 1) chk.k = 1;
        if (chk.k > SLOTS - 1) chk.k = SLOTS - 1;
        if (chk.every <= chk.k) chk.every = chk.k + 1;
    }
    static uint32_t a_options = 1;
    if (a_options == 1) a_options = port_sym("portRollbackOptions");
    if (chk.on < 0 || !syms() || !a_options || !(mem_r32(&M.cpu, a_options) & 0x100u) || !feats_in_battle() || killcam_replaying)
        return;
    if (humans_inputs_size() > sizeof chk.in[0]) return;
    int32_t f = ++chk.step;
    if (f % chk.every == 0) chk.base = f;
    int32_t at = f - chk.base;
    if (chk.base <= 0 || at < 0 || at > chk.k) return;
    if (at == 0 && !chk.checks && getenv("TM2PC_TEST_RERUN_RAND")) { rbs_rand_log = 1; fprintf(stderr, "rand: the live window\n"); }
    if (getenv("TM2PC_TEST_RERUN_NODE") && (at == 0 || at == chk.k) && !chk.checks) { /* tests: the node list around an address */
        uint32_t want = (uint32_t)strtoul(getenv("TM2PC_TEST_RERUN_NODE"), NULL, 16);
        int n = dmd_list();
        for (int i = 0; i < n; i++) {
            uint32_t a = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i), len = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i + 4u);
            if (a + len > want - 0x40 && a < want + 0x40) fprintf(stderr, "rerun: step %d node list %08X +%u\n", (int)f, a, len);
        }
        for (uint32_t o = 0; o < 0x20; o += 4) fprintf(stderr, "rerun: step %d node %08X: %08x\n", (int)f, want + o, mem_r32(&M.cpu, want + o));
    }
    if (at < chk.k) { /* in the window: the state and the step's input kept */
        rbs_save(f);
        humans_inputs_get(chk.in[at]);
        return;
    }
    uint32_t first = play_hash(), first_all = rbs_hash();
    enum { BLK_MAX = 256 };
    static uint32_t blk_first[BLK_MAX];
    void *bp;
    size_t bn;
    int nblk = 0;
    for (; nblk < BLK_MAX && keep_block(nblk, &bp, &bn); nblk++) { /* (each kept block's hash: the one that differs named) */
        uint32_t h = 2166136261u;
        for (size_t b = 0; b < bn; b++) h = (h ^ ((uint8_t *)bp)[b]) * 16777619u;
        blk_first[nblk] = h;
    }
    if (!ram_copy) ram_copy = malloc(GUEST_RAM_BYTES);
    if (ram_copy) memcpy(ram_copy, M.ram, GUEST_RAM_BYTES);
    uint8_t in_now[96];
    humans_inputs_get(in_now);
    if (rbs_rand_log) fprintf(stderr, "rand: the re-run\n");
    int ran = rbs_rerun(chk.base, f, chk_inputs, NULL);
    if (ran < 0) return; /* (no state: a round began in the window) */
    rbs_rand_log = 0;
    humans_inputs_put(in_now); /* (the next step's input, as the live hook wrote it) */
    uint32_t again = play_hash(), again_all = rbs_hash();
    chk.checks++;
    if (ran != chk.k) { /* (the round ended in the first run's window: the loop leaves at its next top; nothing to compare) */
        if (!chk.cut++) fprintf(stderr, "rerun: steps %d-%d: the round ended in them (%d run again): not compared\n", (int)chk.base, (int)f - 1, ran);
        return;
    }
    if (first == again) {
        if (chk.checks <= 3 || chk.checks % 20 == 0)
            fprintf(stderr, "rerun: steps %d-%d again: equal (%d checks, %d differed; the regions' hash rows %s)\n", (int)chk.base, (int)f - 1,
                    chk.checks, chk.bad, first_all == again_all ? "equal too" : "differ");
        static int hash_told;
        if (first_all != again_all && ram_copy && hash_told++ < 3) /* (which hashed words: the table's rows to fix) */
            for (unsigned i = 0, shown = 0; i < rc_rb_hash_count && shown < 40; i++) {
                uint32_t a0 = rc_rb_hash[i].addr & 0x1FFFFFFFu;
                for (uint32_t o = 0; o < rc_rb_hash[i].size && shown < 40; o += 4)
                    if (memcmp(ram_copy + a0 + o, M.ram + a0 + o, 4)) {
                        uint32_t x, y;
                        memcpy(&x, ram_copy + a0 + o, 4); memcpy(&y, M.ram + a0 + o, 4);
                        fprintf(stderr, "rerun:   hashed %08X first %08x again %08x\n", 0x80000000u | (a0 + o), x, y);
                        shown++;
                    }
            }
        return;
    }
    chk.bad++;
    fprintf(stderr, "rerun: steps %d-%d again (%d run): DIFFER (%08x first, %08x again)\n", (int)chk.base, (int)f - 1, ran, first, again);
    for (int i = 0; i < nblk && keep_block(i, &bp, &bn); i++) {
        uint32_t h = 2166136261u;
        for (size_t b = 0; b < bn; b++) h = (h ^ ((uint8_t *)bp)[b]) * 16777619u;
        if (h != blk_first[i])
            fprintf(stderr, "rerun:   kept block %d (%s %d, %u bytes) differs\n", i, i < kc_n ? "the kill cam's" : "rollback's", i < kc_n ? i : i - kc_n, (unsigned)bn);
    }
    if (!ram_copy) return;
    int shown = 0;
    for (unsigned i = 0; i < rc_rb_hash_count && shown < 24; i++) { /* (the hashed bytes: what must be the same) */
        uint32_t a0 = rc_rb_hash[i].addr & 0x1FFFFFFFu;
        for (uint32_t o = 0; o < rc_rb_hash[i].size && shown < 24; o += 4)
            if (memcmp(ram_copy + a0 + o, M.ram + a0 + o, 4)) {
                uint32_t x, y;
                memcpy(&x, ram_copy + a0 + o, 4); memcpy(&y, M.ram + a0 + o, 4);
                fprintf(stderr, "rerun:   %08X first %08x again %08x\n", 0x80000000u | (a0 + o), x, y);
                shown++;
            }
    }
    int n = dmd_list();
    for (int i = 0; i < n && shown < 32; i++) {
        uint32_t a = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i), len = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i + 4u);
        for (uint32_t b = 0; b < len && shown < 32; b++)
            if (ram_copy[(a & 0x1FFFFFFFu) + b] != M.ram[(a & 0x1FFFFFFFu) + b]) {
                fprintf(stderr, "rerun:   node byte %08X first %02x again %02x\n", a + b, ram_copy[(a & 0x1FFFFFFFu) + b], M.ram[(a & 0x1FFFFFFFu) + b]);
                shown++;
            }
    }
    if (!shown) fprintf(stderr, "rerun:   the guest RAM is the same: a kept block of the port differs\n");
}

void rbs_debug_blocks(int32_t step) /* (tests: TM2PC_NET_BLOCKS) a line: the cars' and regions' hashes, then each kept block's */
{
    if (!syms()) return;
    keep_init();
    uint32_t killcam_state_hash(void);
    fprintf(stderr, "blocks %d: cars %08x regions %08x |", (int)step, killcam_state_hash(), rbs_hash());
    void *p;
    size_t n;
    for (int i = 0; keep_block(i, &p, &n); i++) {
        uint32_t h = 2166136261u;
        for (size_t b = 0; b < n; b++) h = (h ^ ((uint8_t *)p)[b]) * 16777619u;
        fprintf(stderr, " %d:%08x", i, h);
    }
    fprintf(stderr, "\n");
}

void rbs_hash_dump(const char *path) /* tests (TM2PC_NET_HASHDUMP): every hashed word as hashed (address, value), the
                                      * nodes' words, to diff machines */
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    masks_now();
    uint32_t w;
    for (unsigned i = 0; i < rc_rb_hash_count; i++)
        for (uint32_t o = 0; o + 4 <= rc_rb_hash[i].size; o += 4) {
            uint32_t a = rc_rb_hash[i].addr + o;
            if (!hashed_word(a, &w)) continue;
            fwrite(&a, 4, 1, f);
            fwrite(&w, 4, 1, f);
        }
    int n = dmd_list();
    for (int i = 0; i < n; i++) {
        uint32_t a = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i), len = mem_r32(&M.cpu, DMD_BUF + 8u * (uint32_t)i + 4u);
        for (uint32_t b = 0; b < len; b++) {
            uint32_t at = a + b, v = mem_r8(&M.cpu, at) | 0x100u; /* (a node byte: 0x1xx) */
            fwrite(&at, 4, 1, f);
            fwrite(&v, 4, 1, f);
        }
    }
    fclose(f);
}

void rbs_span_hashes(FILE *f, int32_t step) /* tests (TM2PC_NET_SPANHASH): each hashed span's own hash at a step */
{
    if (!syms()) return;
    hspans_now();
    uint32_t hum_lo = HUMAN_REC(0), hum_hi = HUMAN_REC(CARS_HUMANS_MAX);
    int chunks = 0; /* (256-byte pieces of the spans: a piece that differs names its symbol) */
    for (int i = 0; i < hspan_n; i++) chunks += (int)((hspan[i].n + 255) / 256);
    fwrite(&step, 4, 1, f);
    fwrite(&chunks, 4, 1, f);
    for (int i = 0; i < hspan_n; i++)
        for (uint32_t c = 0; c < hspan[i].n; c += 256) {
            uint32_t a = hspan[i].a + c, n = hspan[i].n - c < 256 ? hspan[i].n - c : 256, h = 2166136261u;
            for (uint32_t k = 0; k < n; k += 4) {
                uint32_t w = mem_r32(&M.cpu, a + k);
                if (a + k >= hum_lo && a + k < hum_hi) w = human_rec_state_word((a + k - hum_lo) % HUMAN_REC_SIZE, w);
                h = (h ^ w) * 16777619u;
            }
            fwrite(&a, 4, 1, f);
            fwrite(&n, 4, 1, f);
            fwrite(&h, 4, 1, f);
        }
}

/* tests (TM2PC_NET_POSECHK, decomp ONLINE_ROLLBACK.md 20.2): at a step's start every car object's position (+0xA8)
 * against its record's (a human's +0x6FC, a computer car's +0x73C): a difference is a write between two steps (a
 * seeker aims at the object's) */
void rbs_pose_check(int resim)
{
    static int from = -2; /* (TM2PC_NET_POSECHK=S: from network step S on) */
    if (from == -2) from = getenv("TM2PC_NET_POSECHK") ? atoi(getenv("TM2PC_NET_POSECHK")) : -1;
    if (from < 0 || rbs_step_running < from || !syms()) return;
    static int lines;
    for (int k = 0; k < CARS_HUMANS_MAX + cars_ai_max() && lines < 3000; k++) {
        int human = k < CARS_HUMANS_MAX, i = human ? k : k - CARS_HUMANS_MAX;
        uint32_t obj = mem_r32(&M.cpu, human ? GA(0x80180FECu) + 4u * (uint32_t)i : GA(0x80188B38u) + 4u * (uint32_t)i);
        if (!obj) continue;
        uint32_t rec = human ? HUMAN_REC(i) + 0x6FCu : AI_REC(i) + 0x73Cu;
        int differs = 0;
        for (int c = 0; c < 3; c++) differs |= mem_r32(&M.cpu, obj + 0xA8u + 4u * (uint32_t)c) != mem_r32(&M.cpu, rec + 4u * (uint32_t)c);
        if (!differs) continue;
        lines++;
        fprintf(stderr, "pose: step %d%s %s %d object %08X at %d %d %d, its record %d %d %d\n", (int)rbs_step_running,
                resim ? " (run again)" : "", human ? "human" : "computer car", i, obj,
                (int32_t)mem_r32(&M.cpu, obj + 0xA8u), (int32_t)mem_r32(&M.cpu, obj + 0xACu), (int32_t)mem_r32(&M.cpu, obj + 0xB0u),
                (int32_t)mem_r32(&M.cpu, rec), (int32_t)mem_r32(&M.cpu, rec + 4u), (int32_t)mem_r32(&M.cpu, rec + 8u));
    }
    /* each live projectile's target (+0x38): which pool object it is in (20.2's second window: one outside the hash) */
    uint32_t fx = GA(0x801A5B08u);
    for (int k = 0; k < 16 && lines < 3000; k++) {
        uint32_t r = fx + 0x3D4u * (uint32_t)k, t = mem_r32(&M.cpu, r + 0x38);
        int16_t type = (int16_t)mem_r16(&M.cpu, r + 2), life = (int16_t)mem_r16(&M.cpu, r + 4);
        if (!type || !t) continue;
        lines++;
        fprintf(stderr, "seeker: step %d%s record %d type %d life %d object %08X target %08X (%d %d %d)\n", (int)rbs_step_running,
                resim ? " (run again)" : "", k, type, life, mem_r32(&M.cpu, r + 0x34), t, (int32_t)mem_r32(&M.cpu, t),
                (int32_t)mem_r32(&M.cpu, t + 4), (int32_t)mem_r32(&M.cpu, t + 8));
    }
}

/* ---- What differs from the host's state (a resync: Terror's and Blues's "Connection trouble" every few steps) ----
 * Before the host's state is put in place, this machine at the same step: its hashed words (the spans, less each
 * machine's own words, as rbs_hash takes them), the port's random number and the models' node fields kept; after it,
 * compared, and the words that differ logged by name (a car record and its offset, else the nearest symbol), a few
 * a resync and only the first resyncs of a match, so a player's log says what went another way on this machine. */
extern const struct { const char *name; uint32_t addr; } rc_port_symbols[];
extern const unsigned rc_port_symbol_count;
static uint8_t *dif_mine;
static size_t dif_len, dif_cap;
static int dif_on;
static void dif_name(uint32_t a, char *out, size_t n)
{
    if (a >= HUMAN_REC(0) && a < HUMAN_REC(CARS_HUMANS_MAX)) {
        snprintf(out, n, "human %u +0x%X", (a - HUMAN_REC(0)) / HUMAN_REC_SIZE + 1, (a - HUMAN_REC(0)) % HUMAN_REC_SIZE);
        return;
    }
    uint32_t ai0 = AI_REC(0), ai1 = AI_REC(cars_ai_max());
    if (a >= ai0 && a < ai1) { snprintf(out, n, "computer car %u +0x%X", (a - ai0) / AI_REC_SIZE, (a - ai0) % AI_REC_SIZE); return; }
    const char *best = NULL;
    uint32_t at = 0;
    for (unsigned i = 0; i < rc_port_symbol_count; i++)
        if (rc_port_symbols[i].name && rc_port_symbols[i].addr <= a && rc_port_symbols[i].addr >= at) best = rc_port_symbols[i].name, at = rc_port_symbols[i].addr;
    if (best && a - at < 0x10000u) snprintf(out, n, "%s +0x%X", best, a - at);
    else snprintf(out, n, "%08X", a);
}
/* step: the host's state's (this machine's own of that step, from the states kept for rollback; it may be steps on) */
void rbs_diff_begin(int32_t step)
{
    dif_on = 0;
    if (!syms()) return;
    const State *s = &st[(uint32_t)step % SLOTS];
    if (s->step != step) { fprintf(stderr, "network: step %d: this machine's own state of it is gone (not compared)\n", (int)step); return; }
    static uint8_t *img; /* (this machine's RAM at that step's start, where the kept regions say) */
    if (!img && !(img = calloc(1, GUEST_RAM_BYTES))) return;
    const uint8_t *r = s->b;
    for (unsigned i = 0; i < rc_rb_snap_count; i++) {
        uint32_t a0 = rc_rb_snap[i].addr & 0x00FFFFFFu;
        if (a0 + rc_rb_snap[i].size <= GUEST_RAM_BYTES) memcpy(img + a0, r, rc_rb_snap[i].size);
        r += rc_rb_snap[i].size;
    }
    uint32_t rnd;
    memcpy(&rnd, r, 4);
    hspans_now();
    size_t need = 8;
    for (int i = 0; i < hspan_n; i++) need += hspan[i].n;
    if (need > dif_cap) {
        uint8_t *p = realloc(dif_mine, need);
        if (!p) return;
        dif_mine = p, dif_cap = need;
    }
    size_t o = 0;
    for (int i = 0; i < hspan_n; i++) {
        uint32_t a0 = hspan[i].a & 0x00FFFFFFu;
        if (a0 + hspan[i].n <= GUEST_RAM_BYTES) memcpy(dif_mine + o, img + a0, hspan[i].n);
        o += hspan[i].n;
    }
    memcpy(dif_mine + o, &rnd, 4), o += 4;
    dif_len = o;
    dif_on = 1;
}
void rbs_diff_end(int step)
{
    static int told;
    if (!dif_on || told >= 6) return; /* (the first resyncs of the match: enough to see what goes another way) */
    dif_on = 0;
    hspans_now();
    /* by the table's hash rows (one line each: how many words, the first), the cars' records by field */
    static int *row_n;
    static uint32_t *row_first, *row_mine, *row_host;
    if (!row_n) {
        row_n = calloc(rc_rb_hash_count, sizeof *row_n);
        row_first = calloc(rc_rb_hash_count, 4), row_mine = calloc(rc_rb_hash_count, 4), row_host = calloc(rc_rb_hash_count, 4);
        if (!row_n || !row_first || !row_mine || !row_host) return;
    }
    memset(row_n, 0, rc_rb_hash_count * sizeof *row_n);
    int total = 0, cars = 0;
    uint32_t hum_lo = HUMAN_REC(0), hum_hi = HUMAN_REC(CARS_HUMANS_MAX), ai0 = AI_REC(0), ai1 = AI_REC(cars_ai_max());
    size_t o = 0;
    char name[96];
    unsigned row = 0;
    for (int i = 0; i < hspan_n && o + hspan[i].n <= dif_len; i++) {
        const uint8_t *b = mem_direct(&M.cpu, hspan[i].a, (int)hspan[i].n);
        for (uint32_t k = 0; k < hspan[i].n; k += 4) {
            uint32_t a = hspan[i].a + k, mine, host;
            memcpy(&mine, dif_mine + o + k, 4), memcpy(&host, b + k, 4);
            if (a >= hum_lo && a < hum_hi) {
                uint32_t off = (a - hum_lo) % HUMAN_REC_SIZE;
                mine = human_rec_state_word(off, mine), host = human_rec_state_word(off, host);
            }
            if (mine == host) continue;
            if (!total) fprintf(stderr, "network: step %d, what differed from the host's state here (build %s):\n", step, TM2PC_VERSION);
            total++;
            if (((a >= hum_lo && a < hum_hi) || (a >= ai0 && a < ai1)) && cars < 16) { /* (each car field: the likeliest start) */
                dif_name(a, name, sizeof name);
                fprintf(stderr, "network:   %s: this machine %08x, the host %08x\n", name, mine, host);
                cars++;
            }
            if (!(rc_rb_hash[row].addr <= a && a < rc_rb_hash[row].addr + rc_rb_hash[row].size))
                for (row = 0; row < rc_rb_hash_count; row++)
                    if (rc_rb_hash[row].addr <= a && a < rc_rb_hash[row].addr + rc_rb_hash[row].size) break;
            if (row >= rc_rb_hash_count) { row = 0; continue; }
            if (!row_n[row]++) row_first[row] = a, row_mine[row] = mine, row_host[row] = host;
        }
        o += hspan[i].n;
    }
    int lines = 0;
    for (unsigned r = 0; r < rc_rb_hash_count && lines < 40; r++) {
        if (!row_n[r]) continue;
        dif_name(row_first[r], name, sizeof name);
        fprintf(stderr, "network:   %08X (%u bytes): %d words, the first %s (%08X) this machine %08x, the host %08x\n",
                rc_rb_hash[r].addr, rc_rb_hash[r].size, row_n[r], name, row_first[r], row_mine[r], row_host[r]);
        lines++;
    }
    uint32_t r_mine;
    if (o + 4 <= dif_len) {
        memcpy(&r_mine, dif_mine + o, 4);
        if (r_mine != mem_r32(&M.cpu, PORT_RAND)) { total++; fprintf(stderr, "network:   the port's random number: this machine %08x, the host %08x\n", r_mine, mem_r32(&M.cpu, PORT_RAND)); }
    }
    if (total) { told++; fprintf(stderr, "network: step %d: %d words differed\n", step, total); }
    else fprintf(stderr, "network: step %d: the host's state the same as this machine's here (what differed was before, or outside the step)\n", step);
}
