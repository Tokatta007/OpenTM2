/* The game's side of a network match (docs/ONLINE_PLAN.md; decomp ONLINE_ROLLBACK.md): the session (session.c) and its
 * engine (rollback.c) driven by the game's own loop.
 *
 * Rollback (netplay_uses_rollback): at the end of each main-loop iteration (net/rb_state.c's boundary) this machine
 * keeps the step's state, sends its players' inputs and runs on with the others' predicted; an input that comes late
 * and differs puts the state of its step back and runs the steps since again, nothing drawn (rb_state.c). Each step's
 * inputs reach every human's word through the kill cam's input seam (killcam_input); every 8th step the state hashes
 * are compared, and a machine that differs gets the host's state. Lockstep (a build without the decomp's rollback
 * pieces; tests: TM2PC_NET_LOCKSTEP=1) runs a step only once every input for it is here, waiting in the frame hook.
 * Every machine starts a match from the host's state.
 *
 * A match starts from a server's lobby (net/lobby.c: the host's Start, each machine's players numbered in node order:
 * netplay_match_begin), its packets on the lobby's socket; or, for tests, TM2PC_NET=host:PORT (this machine: node 0,
 * player 1) or TM2PC_NET=join:HOST:PORT (node 1, player 2), a 2-player Challenge Match on both. TM2PC_NET_DELAY=N
 * steps of input delay (rollback 1, lockstep 2); TM2PC_NET_LOCAL=1 binds 127.0.0.1 only (two copies on one PC: no
 * firewall prompt). */
#include "../runtime/runtime.h"
#include "../profile/feats.h"
#include "../profile/battle.h"
#include "rb_state.h"
#include "session.h"
#include "net_udp.h"
#include "lobby.h"
#include "lobby_mods.h"
#include "friends.h"
#include "../profile/profile.h"
#include "master.h"
#include "voice_io.h"
#include "settings.h"
#include "../runtime/gpu_backend.h"
#include <psyz/spu.h>
#include <psyz/audio.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NG_KINDS = 3, NG_PLAYERS = 16, NG_LOCAL = 4 };
enum { SEND_EVERY_MS = 4,             /* while a step waits: a send this often at most (plenty at 20 steps a second) */
       RELAY_RATE = 600000 };           /* the reliable bytes a second to the relayed machines, all of them together (the
                                         * master relays 1 MB/s a sender: the rest for the inputs, voice and resends) */
static Lobby *lob; /* (the server: below) */
static int ses_host_of_lobby(void) { return lobby_host_node(lob); }
void ui_net_wait_notice(const char *title, const char *line); /* ui/options.c: the game held, why */
void vp_net_notice(const char *text);                          /* vplus/fixes.c: a line over every view a while */
void vp_human_left(int h);                                     /* vplus/fixes.c: a human's car out (left) */
/* a notice over the views (the HUD's message line: 30 letters at most, vplus/fixes.c MSG_MAX): the name and the long words,
 * else the short ones, else the name cut */
static void notice_fit(char *t, size_t n, const char *name, const char *longer, const char *shorter)
{
    snprintf(t, n, "%s%s", name, longer);
    if (strlen(t) > 30) snprintf(t, n, "%s%s", name, shorter);
    if (strlen(t) > 30) snprintf(t, n, "%.*s%s", (int)(30 - strlen(shorter)), name, shorter);
}
int vp_human_handover(int h);                                 /* vplus/fixes.c: a computer driver takes it over */
void vp_human_back(int h);                                     /* vplus/fixes.c: in again (came back) */
int vp_human_join(int h, int fixed);                          /* vplus/fixes.c: back (1), or dropping in (2) */
int vp_dropin_room(int fixed);                                /* vplus/fixes.c: how many could drop in now */
void humans_add(int h, int slot, const uint8_t t[128]);       /* vplus/humans.c: a human dropping in */
void humans_set_table(int h, const uint8_t t[128]);           /* vplus/humans.c */
void humans_tables_again(void);                               /* vplus/humans.c */
int ui_net_fixed_cpus(void);                                  /* ui/options.c: the match's fixed computer cars */
int ui_net_dropin_game(void);                                 /* ui/options.c: a game a player may drop into */
int ui_net_watch_game(void);                                  /* ui/options.c: a match a machine may come in to watch */
int ui_net_join_next_stage(void);                             /* ui/options.c: a Versus stage on, a stage after it */
int ui_net_versus(void);                                      /* ui/options.c: a Versus Tournament on */
void ui_net_starts_caught_up(void);                           /* ui/options.c: the lobby's starts seen (no launch) */
int ui_net_spot_cpu(int humans);
int ui_net_menu_holds(int slot);                               /* ui/options.c: this PC's player in the Players menu */                               /* ui/options.c: the spot a player leaves: CPU / OPEN? */
static struct {
    int tried, on, node, host;            /* (host: the match host's node; another than 0 once a host left the lobby) */
    int first, count, humans;            /* this machine's players: humans first..first+count-1 of the match's humans */
    uint32_t nodes;                      /* the machines in the match (bits) */
    NetUdp *udp;                         /* (TM2PC_NET's own socket; a server's match: the lobby's) */
    Session *ses;
    uint32_t live[NG_LOCAL][NG_KINDS];   /* this machine's players' inputs as read this step (by its slot: 0..count-1) */
    uint32_t cur[NG_PLAYERS][NG_KINDS];  /* the agreed inputs of the step running (by human) */
    int advanced;                        /* the engine released a step */
    int32_t steps;
    int desyncs_seen, packets_in;
    int pushed, loaded;                  /* the match's start: the host's state sent (host), put in place (joiner) */
    int want_base;                       /* (a joiner) its base owed to the host: its battle's blocks' hashes (delta_make) */
    int left_done;                       /* this battle's leavers out already (the host's, in the state it sent) */
    int saw_match_on, gave_up;                    /* (waiting for the host's state) the host was seen in the match */
    int migrations_seen;                 /* the session's host changes (the host left: another took over) */
    uint32_t migrated_ms;                /* the last one (the notice) */
    uint32_t last_ms;                    /* the last step's end (a gap: a kill cam's replay) */
    uint32_t gone;                       /* the humans whose machine left (bits): their cars out */
    uint32_t back;                       /* the humans whose machine came back (bits; leaving again clears it) */
    int quiet_gone;                      /* (a machine coming back) the players gone already: known, not told again */
    uint32_t join_ask;                   /* (a machine coming back) its slots, asked for once its battle has begun */
    int join_wait;                       /* (its battle's own start first: the frames before the ask) */
    uint32_t mid[NG_PLAYERS];            /* each human's machine's own number (a machine back: known again) */
    uint32_t offered;                    /* (host) the humans a machine was asked back for */
    uint32_t offered_ms;                 /* (host) when */
    int offered_node;                    /* (host) the machine asked (asked again once another is there: the one leaving */
    uint32_t offered_id;                 /* may be asked first, its node then given to the one coming back) */
    int watch;                           /* this machine watches the match: no players of its own (in at the next start) */
    int wait_stage;                      /* (a Versus Tournament) its players in, from the next stage: watching until then */
    uint32_t watch_id[SES_MAX_NODES];    /* (host) each machine asked in to watch (its number: a node given again is new) */
    int each_battle;                     /* a match of several battles (a tournament's levels): each from the host's state */
    int out_of_battle;                   /* the battle left (between levels): the next one starts from the host's state */
    int32_t battle_steps;                /* this battle's steps (its first: a minute for every machine to get there) */
    int in_frame;                        /* netplay_frame's wait (its own sends) */
    int at_step;                         /* in netplay_frame: the game between two steps (a state may be taken) */
    int rollback;                        /* rollback (netplay_uses_rollback; lockstep without it): steps on with predicted
                                          * inputs, a late one that differs runs the steps since again (net/rb_state.c) */
    int resim, resim_ended;              /* (rollback) in the engine's batch of steps run again; the round ended in it */
    int32_t resim_step;                  /* (rollback) the next step the batch runs */
    int window;                          /* (rollback) the furthest a step runs ahead of the inputs (steps) */
    uint16_t tag;                        /* this match's (from its seed): its session's packets only */
    uint32_t sent_ms;                    /* the last send while waiting (SEND_EVERY_MS apart: no packet a millisecond) */
    char names[NG_PLAYERS][LOBBY_NAME];          /* every human's name, as the match began (a machine gone: still known) */
    uint8_t *raw;                        /* a state as killcam_state_save writes it */
    uint8_t *pend;                       /* the host's state as it came (packed), kept until this machine's battle step */
    int pend_len;
    size_t raw_size;
} ng;

extern int killcam_replaying;          /* vplus/killcam.c */
extern int rt_fast_forward;            /* main.c */
uint32_t killcam_state_hash(void);     /* vplus/killcam.c: the cars' state (the kill cam's replay check) */
size_t killcam_state_size(void);       /* vplus/killcam.c: the machine's state as bytes */
void killcam_keep_init(void);
size_t killcam_state_save(uint8_t *out, size_t cap);
int killcam_state_load(const uint8_t *in, size_t len);

static uint32_t now_ms(void) { return (uint32_t)SDL_GetTicks(); }
void netplay_poll(void);

/* an input: the pad's buttons (active low), the analog steering, the easy energy buttons: 2 bytes each */
static RbInput pack(const uint32_t v[NG_KINDS])
{
    RbInput in;
    memset(&in, 0, sizeof in);
    for (int k = 0; k < NG_KINDS; k++) { in.b[2 * k] = (uint8_t)v[k]; in.b[2 * k + 1] = (uint8_t)(v[k] >> 8); }
    return in;
}
static void unpack(const RbInput *in, uint32_t v[NG_KINDS])
{
    for (int k = 0; k < NG_KINDS; k++) v[k] = (uint32_t)in->b[2 * k] | (uint32_t)in->b[2 * k + 1] << 8;
}

/* the engine's callbacks: lockstep never rolls back (save / load unused). With rollback, a state at every step's start
 * (between two iterations of the main loop: net/rb_state.c), and a rollback's batch: the state of its first step put
 * back, each step run again with its input (nothing drawn, no sound), then the live step as always */
void humans_frame(void); /* vplus/humans.c: every human's input words from the agreed inputs */
static void slot_events(int32_t f);
void killcam_rerun_end(void); /* vplus/killcam.c: a batch of steps run again done */
static int32_t saved_step; /* (the step whose state is being kept: the engine hashes it next, cb_checksum) */
static void cb_save(void *ctx, int32_t step)
{
    (void)ctx;
    saved_step = step;
    if (!ng.rollback) return;
    if (ng.resim && step >= rb_now(ses_rollback(ng.ses))) { /* (the batch done: the live step's state) */
        rbs_batch_end();
        killcam_rerun_end();
        ng.resim = 0;
    }
    rbs_save(step);
}
static void cb_load(void *ctx, int32_t step)
{
    (void)ctx;
    if (!ng.rollback) return;
    if (!ng.resim) rbs_batch_begin();
    ng.resim = 1;
    ng.resim_ended = 0;
    ng.resim_step = step;
    if (!rbs_load(step)) { /* (a step before a boss phase's start: its states gone with the old round. Not run again on
                            * top of the new phase (the user's 2026-11-09 playtest: Minion's phase, steps run twice, the
                            * host's picture broken to the level's end); the joiner takes the host's state anyway) */
        fprintf(stderr, "network: rollback: no state for step %d (the steps not run again)\n", (int)step);
        ng.resim_ended = 1;
    }
}
/* (rollback) the next step ends the round (portRollbackEndDue: a scheduled event fires at its top, decomp
 * ONLINE_ROLLBACK.md 15.4) and a step before it ran on a predicted input */
static int end_due_predicted(void)
{
    static uint32_t a_due = 1;
    if (a_due == 1) a_due = port_sym("portRollbackEndDue");
    const Rollback *rb = ses_rollback(ng.ses);
    if (!a_due || rb_confirmed(rb) >= rb_now(rb) - 1) return 0;
    return guest_call(a_due, 0, 0, 0, 0) != 0;
}
/* (rollback) a machine behind the host runs the step just released at once, nothing drawn, so the next one is the
 * loop's: one step more this iteration. Not a step that ends the round (the loop's own top takes that). The gameplay
 * clock moves the unseen step's 3 VBlanks (FIXED_TICKS: 20 steps a second): the next step runs at the loop's top with
 * no pacing before it, as every machine runs it 3 VBlanks after the one before. */
static int catch_up_step(void)
{
    static uint32_t a_run = 1, a_due = 1;
    if (a_run == 1) { a_run = port_sym("portRunStep"); a_due = port_sym("portRollbackEndDue"); }
    if (!a_run || !a_due || guest_call(a_due, 0, 0, 0, 0)) return 0;
    /* (run as a step run again: the frame hook's own work only, no sound, the palettes after; the live hook's
     * presentation (the in-between frames' positions, the kill cam's frame, the HUD) expects a drawing before the next
     * step, and a machine catching up runs two steps with none between) */
    extern int killcam_catching_up; /* vplus/killcam.c: the step a frame of its own */
    rbs_batch_begin();
    killcam_catching_up = 1;
    int ended = (int)guest_call(a_run, 0, 0, 0, 0);
    killcam_catching_up = 0;
    rbs_batch_end();
    if (ended) return 0; /* (it ended the round after all: the loop leaves at its next top) */
    M.vblank_offset -= 3;
    ng.steps++;
    ng.battle_steps++;
    if (getenv("TM2PC_NET_DEBUG")) fprintf(stderr, "network: caught up a step (%d)\n", (int)rb_now(ses_rollback(ng.ses)) - 1);
    return 1;
}
static void end_due_told(void) /* tests (TM2PC_NET_DEBUG): the step that ends the round, released (every machine the same) */
{
    static uint32_t a_due = 1;
    if (a_due == 1) a_due = port_sym("portRollbackEndDue");
    if (a_due && getenv("TM2PC_NET_DEBUG") && guest_call(a_due, 0, 0, 0, 0))
        fprintf(stderr, "network: the round ends at step %d (confirmed %d)\n", (int)rb_now(ses_rollback(ng.ses)) - 1,
                (int)rb_confirmed(ses_rollback(ng.ses)));
}
static void resim_close(void) /* (a batch the session ran without a live step after it: ended) */
{
    if (ng.resim) { rbs_batch_end(); killcam_rerun_end(); ng.resim = 0; }
}
static void cb_advance(void *ctx, const RbInput *inputs)
{
    (void)ctx;
    for (int p = 0; p < ng.humans && p < NG_PLAYERS; p++) {
        unpack(&inputs[p], ng.cur[p]);
        if (!(inputs[p].b[0] | inputs[p].b[1])) { /* a neutral input (all zero): no button held (active low), no steering */
            ng.cur[p][0] = 0xFFFF;
            ng.cur[p][1] = 4096;
            ng.cur[p][2] = 0;
        }
        ng.cur[p][0] |= 0x0008;                                         /* (no Start online: nobody pauses everyone) */
    }
    if (ng.resim) { /* (a step run again: its input written, then the step) */
        int32_t f = ng.resim_step++;
        if (ng.resim_ended) return;
        extern int32_t rbs_step_running;
        rbs_step_running = f;
        void killcam_rerun_step(int back); /* vplus/killcam.c: the frame it was */
        killcam_rerun_step((int)(rb_now(ses_rollback(ng.ses)) - f));
        slot_events(f);
        humans_frame();
        if (rbs_rerun_step(f)) ng.resim_ended = 1; /* (the round ended in it: the loop leaves at its next top) */
        return;
    }
    ng.advanced = 1;
    ng.at_step = 0; /* (a step released: the game is not at its boundary any more until it has run) */
    if (ng.rollback) { /* (the next iteration runs it: who is in at its start, its input written now) */
        extern int32_t rbs_step_running;
        rbs_step_running = rb_now(ses_rollback(ng.ses));
        slot_events(rb_now(ses_rollback(ng.ses)));
        humans_frame();
    }
}
static uint32_t ds_kc[64], ds_kc_h[64]; /* (tests: TM2PC_NET_DESYNC_DUMP, the kill cam's part by step) */
static uint32_t cb_checksum(void *ctx)
{
    (void)ctx;
    static int from = -2;
    if (from == -2) from = getenv("TM2PC_NET_BLOCKS") ? atoi(getenv("TM2PC_NET_BLOCKS")) : -1;
    int32_t f = saved_step; /* (in a batch the engine's now is the present: the step is the one just kept) */
    if (from >= 0 && f >= from && f % 8 == 0) rbs_debug_blocks(f); /* tests: every kept block's hash (two machines diffed) */
    if (getenv("TM2PC_NET_HASHCARS") && getenv("TM2PC_STATE_LOG") && f % 8 == 0) { /* tests: the hashed cars as hashed (a
                                                                                      * step hashed again after a rollback:
                                                                                      * a later record, the final one) */
        static FILE *cf;
        void killcam_cars_dump(FILE *f, int32_t step);
        if (!cf) {
            char path[1024];
            snprintf(path, sizeof path, "%s.hcars", getenv("TM2PC_STATE_LOG"));
            cf = fopen(path, "wb");
        }
        if (cf) { killcam_cars_dump(cf, f); fflush(cf); }
        static FILE *nf; /* (and the seed and the models' nodes: the rest of the hash) */
        void killcam_dmd_dump(FILE *f, int32_t step);
        if (!nf) {
            char path[1024];
            snprintf(path, sizeof path, "%s.hdmd", getenv("TM2PC_STATE_LOG"));
            nf = fopen(path, "wb");
        }
        if (nf) { killcam_dmd_dump(nf, f); fflush(nf); }
    }
    if (getenv("TM2PC_NET_SPANHASH") && getenv("TM2PC_STATE_LOG") && f % 8 == 0 &&
        f >= atoi(getenv("TM2PC_NET_SPANHASH"))) { /* tests: each span's hash, every 8th (from the step given: 1, all) */
        static FILE *sf;
        if (!sf) {
            char path[1024];
            snprintf(path, sizeof path, "%s.spans", getenv("TM2PC_STATE_LOG"));
            sf = fopen(path, "wb");
        }
        if (sf) { rbs_span_hashes(sf, f); fflush(sf); }
    }
    if (getenv("TM2PC_NET_HASHDUMP") && f == atoi(getenv("TM2PC_NET_HASHDUMP")) && getenv("TM2PC_STATE_LOG")) {
        char path[1024]; /* tests: every hashed word at that step (two machines diffed) */
        snprintf(path, sizeof path, "%s.hash", getenv("TM2PC_STATE_LOG"));
        rbs_hash_dump(path);
    }
    /* the decomp's whole table (its hash rows: the game's state, each machine's own words out since requests 57-58;
     * rbs_hash) and the cars as the kill cam hashes them (their records less each machine's camera bytes), the game's
     * seed and the models' nodes; without the decomp's rollback pieces the latter alone */
    extern int32_t rbs_ds_step;
    rbs_ds_step = f; /* (tests: TM2PC_NET_DESYNC_DUMP's pieces, by step) */
    uint32_t kc = killcam_state_hash();
    if (getenv("TM2PC_NET_DESYNC_DUMP")) ds_kc[(uint32_t)f % 64] = (uint32_t)f, ds_kc_h[(uint32_t)f % 64] = kc;
    uint32_t sum = rbs_available() ? rbs_hash() ^ kc * 0x9E3779B1u : kc;
    rbs_ds_step = -1;
    return sum;
}
/* tests (TM2PC_NET_DESYNC_DUMP=file): at a desync the host writes its pieces of that step, a machine (as the host's
 * state comes again) its last 64 steps': two files diffed (spandiff), and the kill cam's cars hash of each */
void net_desync_dump(int32_t step, int host)
{
    const char *base = getenv("TM2PC_NET_DESYNC_DUMP");
    if (!base) return;
    char path[1024];
    snprintf(path, sizeof path, "%s.%s", base, host ? "host" : "join");
    void rbs_ds_dump(const char *path, int32_t only);
    rbs_ds_dump(path, host ? step : -1);
    snprintf(path, sizeof path, "%s.%s.kc", base, host ? "host" : "join");
    FILE *f = fopen(path, "a");
    for (int i = 0; f && i < 64; i++) if (!host || ds_kc[i] == (uint32_t)step) fprintf(f, "%u %08x\n", ds_kc[i], ds_kc_h[i]);
    if (f) fclose(f);
}
/* a state packed: runs of zero bytes (most of the 16 MB) and the bytes between as they are: a tag (0 bytes, 1 zeros),
 * the length (7 bits a byte, low first), the bytes */
enum { ZERO_RUN_MIN = 16 };
static uint8_t *put_len(uint8_t *w, size_t n)
{
    do { *w++ = (uint8_t)((n & 0x7F) | (n > 0x7F ? 0x80 : 0)); n >>= 7; } while (n);
    return w;
}
static size_t pack_state(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    uint8_t *w = out, *end = out + cap;
    size_t i = 0, lit = 0;
    while (i <= n) {
        size_t z = 0;
        if (i < n && !in[i]) while (i + z < n && !in[i + z]) z++;
        if (i == n || z >= ZERO_RUN_MIN) { /* the bytes before it, then the run */
            size_t from = i - lit;
            if (lit) {
                if ((size_t)(end - w) < lit + 12) return 0;
                *w++ = 0;
                w = put_len(w, lit);
                memcpy(w, in + from, lit);
                w += lit;
                lit = 0;
            }
            if (i == n) break;
            if (end - w < 12) return 0;
            *w++ = 1;
            w = put_len(w, z);
            i += z;
        } else {
            size_t k = z ? z : 1; /* (a short run of zeros: kept with the bytes) */
            lit += k;
            i += k;
        }
    }
    return (size_t)(w - out);
}
static size_t unpack_state(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    const uint8_t *r = in, *end = in + n;
    size_t at = 0;
    while (r < end) {
        int tag = *r++;
        size_t len = 0;
        for (int sh = 0; r < end; sh += 7) {
            uint8_t b = *r++;
            len |= (size_t)(b & 0x7F) << sh;
            if (!(b & 0x80)) break;
        }
        if (at + len > cap || (tag == 0 && (size_t)(end - r) < len) || tag > 1) return 0;
        if (tag) memset(out + at, 0, len);
        else { memcpy(out + at, r, len); r += len; }
        at += len;
    }
    return at;
}
/* Beside the machine's RAM: what the match loaded into the GPU and the SPU (the cars' textures, the arena's, their
 * sounds), which a machine whose menus went another way has not: the VRAM, each texture bank (1..31, the cars' with
 * the decomp's TEXTURE_BANKS: unused ones are zeros, packed to nothing), the sound RAM */
enum { VRAM_BYTES = 1024 * 512 * 2, BANKS_MAX = 31 };
static int gpu_banks(void) { return gpu_backend->banks && gpu_backend->banks() && gpu_backend->download_bank ? BANKS_MAX : 0; }
/* The banks: only those holding anything (a mask), so a match with few car textures carries 1-3 MB of them, not 31
 * MB of zeros (the user's 2026-11-13: copied, hashed and packed at every state). Every machine at the same step has
 * the same banks filled, so the hosts' and the joiners' states keep the same layout (the bases' blocks match). */
static size_t devices_size(void) { return 4 + 8 + (size_t)(1 + gpu_banks()) * VRAM_BYTES + PSYZ_SPU_RAM_SIZE; } /* (the most) */
static int bank_empty(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)p;
    for (size_t i = 0; i < VRAM_BYTES / 8; i++) if (q[i]) return 0;
    return 1;
}
static size_t devices_save(uint8_t *w)
{
    uint32_t n = (uint32_t)gpu_banks();
    uint64_t mask = 0;
    memcpy(w, &n, 4);
    uint8_t *at = w + 12;
    gpu_backend->download(0, 0, 1024, 512, (uint16_t *)at);
    at += VRAM_BYTES;
    for (uint32_t b = 1; b <= n; b++) { /* (each into its place; kept only if it holds anything) */
        gpu_backend->download_bank((int)b, 0, 0, 1024, 512, (uint16_t *)at);
        if (!bank_empty(at)) mask |= 1ull << b, at += VRAM_BYTES;
    }
    memcpy(w + 4, &mask, 8);
    Psyz_AudioLock();
    Psyz_SpuMemRead(0, at, PSYZ_SPU_RAM_SIZE);
    Psyz_AudioUnlock();
    return (size_t)(at + PSYZ_SPU_RAM_SIZE - w);
}
static int devices_load(const uint8_t *r, size_t len)
{
    uint32_t n;
    uint64_t mask;
    memcpy(&n, r, 4);
    memcpy(&mask, r + 4, 8);
    size_t kept = 0;
    for (uint32_t b = 1; b <= n && b < 64; b++) kept += (mask >> b) & 1;
    if (n != (uint32_t)gpu_banks() || len != 12 + (1 + kept) * VRAM_BYTES + PSYZ_SPU_RAM_SIZE) return 0;
    const uint8_t *at = r + 12;
    gpu_backend->upload(0, 0, 1024, 512, (const uint16_t *)at);
    void hdtex_vram_written(int x, int y, int w, int h);
    hdtex_vram_written(0, 0, 1024, 512); /* (the host's VRAM: the sharper textures kept where it is the same) */
    at += VRAM_BYTES;
    static uint8_t *mine; /* (a bank the host had empty: emptied here too, if it is not already) */
    for (uint32_t b = 1; b <= n; b++) {
        if ((mask >> b) & 1) {
            gpu_backend->upload_bank((int)b, 0, 0, 1024, 512, (const uint16_t *)at);
            at += VRAM_BYTES;
            continue;
        }
        if (!mine && !(mine = malloc(VRAM_BYTES))) return 0;
        gpu_backend->download_bank((int)b, 0, 0, 1024, 512, (uint16_t *)mine);
        if (!bank_empty(mine)) {
            memset(mine, 0, VRAM_BYTES);
            gpu_backend->upload_bank((int)b, 0, 0, 1024, 512, (const uint16_t *)mine);
        }
    }
    Psyz_AudioLock();
    unsigned t = Psyz_SpuGetTransferAddr();
    Psyz_SpuSetTransferAddr(0);
    Psyz_SpuFifoWriteBulk(at, PSYZ_SPU_RAM_SIZE);
    Psyz_SpuSetTransferAddr(t);
    Psyz_AudioUnlock();
    return 1;
}
/* A machine's start, only what differs (the user's: 3.5 MB to each joiner at every start, 16 players 53 MB from the
 * host): each machine loads the battle itself, so most of the host's state is its own already (measured: 65 of 3,105
 * blocks of 16 KB differ, the menus' last frames in VRAM and some of RAM). A joiner keeps its own state as its battle
 * loads (its base) and sends the host each block's hash; the host sends the blocks that differ, deflated (about 300 KB);
 * the joiner puts them over its base: the host's state, byte for byte. A base not come (an old build, a host just
 * moved): the whole state after BASE_WAIT_MS; a delta made from another base (another battle's): the whole one asked */
#include "miniz.h"
enum { BASE_BLOCK = 16384, BASE_WAIT_MS = 15000 };
static const uint32_t BASE_MAGIC = 0x31424D54u; /* "TMB1" */
static uint8_t *base_raw, *work;  /* (a joiner) its own state as its battle loaded; (both) the delta's bytes, unpacked */
static size_t base_len, work_cap;
static uint32_t base_id;          /* (a joiner) its bases so far: a delta names the one it was made from */
static uint64_t block_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull; /* (FNV-1a, 64 bits: a chance block alike by mistake is 1 in 2^64) */
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static int work_room(size_t n)
{
    if (work_cap >= n) return 1;
    uint8_t *w = realloc(work, n);
    if (!w) return 0;
    work = w;
    work_cap = n;
    return 1;
}
static void put_u32(uint8_t *w, uint32_t v) { for (int i = 0; i < 4; i++) w[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u32(const uint8_t *r) { return (uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24; }
static int base_send(void) /* (a joiner, its battle loaded) its own state kept, its blocks' hashes to the host; 0: not now */
{
    if (!ng.ses || !ng.raw_size || (!base_raw && !(base_raw = malloc(ng.raw_size)))) return 0;
    size_t n = killcam_state_save(base_raw, ng.raw_size);
    if (!n) return 0;
    n += devices_save(base_raw + n);
    uint32_t nb = (uint32_t)((n + BASE_BLOCK - 1) / BASE_BLOCK);
    size_t len = 12 + 8 * (size_t)nb;
    if (len > 65536 || !work_room(len) || getenv("TM2PC_NET_NO_BASE")) { /* (too big: the whole state; tests: always) */
        ses_send_base(ng.ses, NULL, 0);
        return 1;
    }
    base_len = n;
    base_id++;
    put_u32(work, BASE_MAGIC);
    put_u32(work + 4, base_id);
    put_u32(work + 8, (uint32_t)n);
    for (uint32_t b = 0; b < nb; b++) {
        size_t at = (size_t)b * BASE_BLOCK, k = n - at < BASE_BLOCK ? n - at : BASE_BLOCK;
        uint64_t h = block_hash(base_raw + at, k);
        put_u32(work + 12 + 8 * b, (uint32_t)h);
        put_u32(work + 16 + 8 * b, (uint32_t)(h >> 32));
    }
    ses_send_base(ng.ses, work, (int)len);
    fprintf(stderr, "network: this machine's base sent (%u blocks, %u bytes)\n", nb, (unsigned)len);
    return 1;
}
/* (host) the state for a machine with a base: the blocks that differ, deflated; 0: no usable base */
static size_t delta_make(size_t n, uint8_t *out, size_t cap)
{
    int bl;
    const uint8_t *base = ses_state_base(ng.ses, &bl);
    uint32_t nb = (uint32_t)((n + BASE_BLOCK - 1) / BASE_BLOCK), map = (nb + 7) / 8;
    if (!base || bl != (int)(12 + 8 * (size_t)nb) || get_u32(base) != BASE_MAGIC || get_u32(base + 8) != (uint32_t)n) return 0;
    if (!work_room(map + n) || cap < 32) return 0;
    memset(work, 0, map);
    size_t w = map;
    uint32_t differ = 0;
    for (uint32_t b = 0; b < nb; b++) {
        size_t at = (size_t)b * BASE_BLOCK, k = n - at < BASE_BLOCK ? n - at : BASE_BLOCK;
        uint64_t h = block_hash(ng.raw + at, k), theirs = get_u32(base + 12 + 8 * b) | (uint64_t)get_u32(base + 16 + 8 * b) << 32;
        if (h == theirs) continue;
        work[b / 8] |= (uint8_t)(1u << (b % 8));
        memcpy(work + w, ng.raw + at, k);
        w += k;
        differ++;
    }
    mz_ulong z = (mz_ulong)(cap - 13);
    if (mz_compress2(out + 13, &z, work, (mz_ulong)w, 6) != MZ_OK) return 0;
    out[0] = 'D';
    put_u32(out + 1, get_u32(base + 4));
    put_u32(out + 5, (uint32_t)n);
    put_u32(out + 9, (uint32_t)w);
    fprintf(stderr, "network: the state as a delta: %u of %u blocks differ (%u bytes deflated)\n", differ, nb, (unsigned)(13 + z));
    return 13 + (size_t)z;
}
/* the host's state for a machine (at a match's start); lockstep keeps no state of earlier steps: the one now */
static int cb_serialize(void *ctx, int32_t step, uint8_t *out, int cap)
{
    (void)ctx;
    if (!ng.at_step || killcam_replaying || !feats_in_battle()) return 0; /* the state only between two steps (not from
                                                           * a frame's middle, a replay, between battles): the session asks
                                                           * again at its next send */
    if (ng.rollback && step != rb_now(ses_rollback(ng.ses))) return 0; /* (rollback: past the step asked for with predicted
                                                                         * inputs: not yet; frame_body stops predicting
                                                                         * while a state is owed, the inputs catch up) */
    uint64_t t0 = SDL_GetTicksNS();
    size_t n = killcam_state_save(ng.raw, ng.raw_size);
    if (n) n += devices_save(ng.raw + n);
    if (n && getenv("TM2PC_NET_STATE_DUMP")) { /* tests: the raw state, to compare with a joiner's own */
        FILE *f = fopen(getenv("TM2PC_NET_STATE_DUMP"), "wb");
        if (f) { fwrite(ng.raw, 1, n, f); fclose(f); }
    }
    size_t packed = n ? delta_make(n, out, (size_t)cap) : 0;
    if (n && !packed && work_room(n + n / 64 + 4096)) { /* (the whole: its zero runs, then deflated, quick: a resync's) */
        size_t runs = pack_state(ng.raw, n, work, work_cap);
        mz_ulong z = (mz_ulong)((size_t)cap - 5);
        if (runs && mz_compress2(out + 5, &z, work, (mz_ulong)runs, 3) == MZ_OK) { /* (3: a fifth smaller than 1, a few
                                                                                  * hundredths of a second more) */
            out[0] = 'Z';
            put_u32(out + 1, (uint32_t)runs);
            packed = 5 + (size_t)z;
        }
    }
    uint64_t vp_gv(void);
    fprintf(stderr, "network: the state of step %d sent (%u bytes, %u packed in %.0f ms; gv %llu, now %d)\n", (int)step, (unsigned)n, (unsigned)packed,
            (SDL_GetTicksNS() - t0) / 1e6,
            (unsigned long long)vp_gv(), (int)rb_now(ses_rollback(ng.ses)));
    return (int)packed;
}
static void state_apply(void) /* the host's state kept: in place now (this machine at its battle's step) */
{
    if (!ng.pend_len) return;
    if (ng.loaded) net_desync_dump(-1, 0); /* (tests: a resync's: this machine's last steps, before the host's state) */
    if (getenv("TM2PC_NET_STATE_DUMP")) { /* tests: this machine's own state, before the host's */
        size_t own = killcam_state_save(ng.raw, ng.raw_size);
        if (own) own += devices_save(ng.raw + own);
        FILE *f = fopen(getenv("TM2PC_NET_STATE_DUMP"), "wb");
        if (f) { fwrite(ng.raw, 1, own, f); fclose(f); }
    }
    size_t n = 0, ks = killcam_state_size();
    const uint8_t *in = ng.pend;
    if (ng.pend_len > 5 && in[0] == 'Z') { /* the whole state */
        mz_ulong runs = get_u32(in + 1);
        if (work_room(runs) && mz_uncompress(work, &runs, in + 5, (mz_ulong)(ng.pend_len - 5)) == MZ_OK)
            n = unpack_state(work, runs, ng.raw, ng.raw_size);
    } else if (ng.pend_len > 13 && in[0] == 'D') { /* what differs from this machine's base */
        uint32_t id = get_u32(in + 1), size = get_u32(in + 5);
        mz_ulong w = get_u32(in + 9);
        if (!base_raw || id != base_id || size != base_len || size > ng.raw_size) { /* (another base's: the whole, please) */
            fprintf(stderr, "network: the host's state was made for another base (%u, this machine's %u): the whole asked\n", id, base_id);
            ses_send_base(ng.ses, NULL, 0);
            ng.pend_len = 0;
            return;
        }
        uint32_t nb = (uint32_t)((size + BASE_BLOCK - 1) / BASE_BLOCK), map = (nb + 7) / 8;
        if (work_room(w) && mz_uncompress(work, &w, in + 13, (mz_ulong)(ng.pend_len - 13)) == MZ_OK && w >= map) {
            memcpy(ng.raw, base_raw, size);
            size_t r = map;
            int ok = 1;
            for (uint32_t b = 0; b < nb && ok; b++) {
                if (!((work[b / 8] >> (b % 8)) & 1)) continue;
                size_t at = (size_t)b * BASE_BLOCK, k = size - at < BASE_BLOCK ? size - at : BASE_BLOCK;
                if (r + k > w) { ok = 0; break; }
                memcpy(ng.raw + at, work + r, k);
                r += k;
            }
            if (ok) n = size;
        }
    }
    ng.loaded = n > ks && killcam_state_load(ng.raw, ks) && devices_load(ng.raw + ks, n - ks);
    if (ng.loaded) rbs_dmd_again(); /* (the host's models: the list made again) */
    if (ng.loaded) rb_resum_restart(ses_rollback(ng.ses)); /* (its step's start ran before it was here: hashed again) */
    void humans_tables_again(void); /* vplus/humans.c */
    if (ng.loaded) humans_tables_again(); /* (the button tables, as every machine has them: the state has the host's) */
    void vp_watch_again(void); /* vplus/fixes.c */
    if (ng.loaded && (ng.watch || ng.wait_stage)) vp_watch_again(); /* (watching: its view on the car watched again, the host's view bytes in) */
    if (ng.loaded && ng.rollback) { /* (rollback: the state's step released before the state was put in place (the
                                     * session restarts at it as the state arrives): its state kept again, and who is in
                                     * and its input written into the host's state, as the release did into this
                                     * machine's own) */
        Rollback *rb = ses_rollback(ng.ses);
        int32_t f = rb_floor(rb);
        if (rb_now(rb) == f + 1) {
            rbs_save(f);
            slot_events(f);
            humans_frame();
        }
    }
    uint64_t vp_gv(void);
    fprintf(stderr, "network: the host's state %s (%d bytes packed; gv %llu, now %d)\n", ng.loaded ? "in place" : "NOT USABLE", ng.pend_len,
            (unsigned long long)vp_gv(), (int)rb_now(ses_rollback(ng.ses)));
    ng.pend_len = 0;
}
static void cb_deserialize(void *ctx, const uint8_t *in, int len)
{
    (void)ctx;
    /* (it may come while this machine is still loading or between a tournament's levels: kept for its battle's step) */
    uint8_t *p = realloc(ng.pend, (size_t)len);
    if (!p) return;
    ng.pend = p;
    memcpy(ng.pend, in, (size_t)len);
    ng.pend_len = len;
}

static void neutral_all(void)
{
    for (int p = 0; p < NG_PLAYERS; p++) {
        ng.cur[p][0] = 0xFFFF;
        ng.cur[p][1] = 4096;
        ng.cur[p][2] = 0;
    }
    for (int p = 0; p < NG_LOCAL; p++) {
        ng.live[p][0] = 0xFFFF;
        ng.live[p][1] = 4096;
        ng.live[p][2] = 0;
    }
}
/* The netcode: rollback (the user's, 2026-10-22: it replaces lockstep, no option) whenever the build has the decomp's
 * rollback pieces; lockstep only without them (tests: TM2PC_NET_LOCKSTEP=1, to compare). Input delay: rollback 1 step
 * (50 ms: predicted inputs cover the rest), lockstep 2. */
int netplay_uses_rollback(void) { return rbs_available() && !getenv("TM2PC_NET_LOCKSTEP"); }
int netplay_input_delay(void) { return getenv("TM2PC_NET_DELAY") ? atoi(getenv("TM2PC_NET_DELAY")) : netplay_uses_rollback() ? 1 : 2; }
static int match_open(const NetTransport *net, const uint32_t slots[SES_MAX_NODES], int delay, int rejoin)
{
    static NetGame game;
    game.rb.ctx = NULL;
    game.rb.save = cb_save;
    game.rb.load = cb_load;
    game.rb.advance = cb_advance;
    game.rb.checksum = cb_checksum;
    game.serialize = cb_serialize;
    game.deserialize = cb_deserialize;
    killcam_keep_init(); /* (the port's blocks in the state: its size final) */
    ng.raw_size = killcam_state_size() + devices_size();
    if (!ng.raw) ng.raw = malloc(ng.raw_size);
    if (!ng.raw) return 0;
    game.state_max = (int)(ng.raw_size + ng.raw_size / 64 + 4096); /* (packed: never much more than the bytes) */
    ng.ses = rejoin ? ses_join_slots(ng.node, ses_host_of_lobby(), net, &game, delay, now_ms(), ng.watch ? SES_WATCH : slots[ng.node]) /* (back in) */
                    : ses_new(ng.node, ng.host, ng.nodes, slots, net, &game, delay);
    if (!ng.ses) return 0;
    ng.rollback = netplay_uses_rollback();
    static int kept;
    if (!kept) { kept = 1; rbs_keep(&ng.gone, sizeof ng.gone); rbs_keep(&ng.back, sizeof ng.back); } /* (who is out, per step) */
    ng.resim = ng.resim_ended = 0;
    ses_set_lockstep(ng.ses, !ng.rollback);
    ses_set_one_step(ng.ses, ng.rollback);
    ng.window = getenv("TM2PC_NET_WINDOW") ? atoi(getenv("TM2PC_NET_WINDOW")) : 4; /* (rollback: the furthest a machine
                                                       * runs ahead of the inputs it has; past it, it waits) */
    if (ng.rollback) rb_set_window(ses_rollback(ng.ses), ng.window);
    ses_set_tag(ng.ses, ng.tag); /* (this match's: the last one's packets still coming are dropped) */
    if (ng.node != ng.host) ses_await_state(ng.ses); /* a joiner starts from the host's state (its menus need not be the same) */
    ses_set_base_wait(ng.ses, getenv("TM2PC_NET_NO_DELTA") ? 0 : BASE_WAIT_MS); /* (tests: the whole state, as before) */
    ng.want_base = ng.node != ng.host && !rejoin && !getenv("TM2PC_NET_NO_DELTA"); /* (back in: after its ask, frame_body) */
    ng.pushed = ng.loaded = ng.left_done = 0;
    ng.pend_len = 0;
    ng.migrations_seen = ses_stats(ng.ses)->migrations;
    ng.migrated_ms = ng.last_ms = 0;
    ng.gone = ng.back = ng.offered = 0;
    memset(ng.watch_id, 0, sizeof ng.watch_id); /* (host: a new match, every machine asked again) */
    ng.quiet_gone = rejoin;
    ng.join_ask = ng.watch ? SES_WATCH : rejoin ? slots[ng.node] : 0;
    ng.join_wait = 30;
    ng.battle_steps = ng.out_of_battle = ng.in_frame = 0;
    ng.steps = 0;
    ng.desyncs_seen = 0;
    ng.on = 1;
    neutral_all();
    fprintf(stderr, "network: the match: machine %d, players %d-%d of %d, input delay %d (%s)\n", ng.node, ng.first + 1,
            ng.first + ng.count, ng.humans, delay, ng.rollback ? "rollback" : "lockstep");
    return 1;
}
static void start(void)
{
    ng.tried = 1;
    const char *e = getenv("TM2PC_NET");
    if (!e) return;
    char host[128] = "";
    int port = 0;
    if (!strncmp(e, "host:", 5)) { ng.node = 0; port = atoi(e + 5); }
    else if (sscanf(e, "join:%127[^:]:%d", host, &port) == 2) ng.node = 1;
    else { fprintf(stderr, "network: TM2PC_NET: host:PORT or join:HOST:PORT\n"); return; }
    ng.first = ng.node;
    ng.count = 1;
    ng.humans = 2;
    ng.nodes = 0x3;
    int local = getenv("TM2PC_NET_LOCAL") != NULL; /* (tests: this PC only, 127.0.0.1) */
    ng.udp = udp_open(ng.node == 0 ? (uint16_t)port : 0, local ? UDP_LOOPBACK : 0);
    if (!ng.udp) { fprintf(stderr, "network: no socket\n"); return; }
    if (ng.node == 1 && !udp_set_node(ng.udp, 0, host, (uint16_t)port)) { fprintf(stderr, "network: %s not found\n", host); return; }
    static NetTransport net;
    net.ctx = ng.udp;
    net.send = udp_send;
    uint32_t slots[SES_MAX_NODES] = {1u << 0, 1u << 1};
    int delay = getenv("TM2PC_NET_DELAY") ? atoi(getenv("TM2PC_NET_DELAY")) : netplay_input_delay();
    match_open(&net, slots, delay, 0);
}

/* tests: a bad connection, on the match's packets in (both ways in: the direct one and a server's match through the
 * lobby): TM2PC_NET_LAG_MS=N each held N ms, TM2PC_NET_JITTER_MS=J plus 0-J more each (so some overtake others),
 * TM2PC_NET_LOSS=P P% of them dropped. Its own random numbers: never the game's. */
enum { LAG_MAX = 4096, LAG_PACKET = 2600 };
static struct LagPacket { uint32_t due; int from, n; uint8_t b[LAG_PACKET]; } *lagq; /* (made on the first one held) */
static int lag_n, lag_on = -1;
static uint32_t lag_ms, jitter_ms, loss_pct, lag_seed = 2463534242u;
static uint32_t lag_rand(void) { lag_seed ^= lag_seed << 13; lag_seed ^= lag_seed >> 17; lag_seed ^= lag_seed << 5; return lag_seed; }
static void net_in(int from, const uint8_t *data, int len)
{
    if (lag_on < 0) {
        lag_ms = getenv("TM2PC_NET_LAG_MS") ? (uint32_t)atoi(getenv("TM2PC_NET_LAG_MS")) : 0;
        jitter_ms = getenv("TM2PC_NET_JITTER_MS") ? (uint32_t)atoi(getenv("TM2PC_NET_JITTER_MS")) : 0;
        loss_pct = getenv("TM2PC_NET_LOSS") ? (uint32_t)atoi(getenv("TM2PC_NET_LOSS")) : 0;
        lag_seed ^= (uint32_t)SDL_GetTicksNS();
        lag_on = lag_ms || jitter_ms || loss_pct;
    }
    if (lag_on) {
        if (loss_pct && lag_rand() % 100 < loss_pct) return;
        if (!lagq) lagq = malloc(sizeof *lagq * LAG_MAX);
        if (lagq && lag_n < LAG_MAX && len <= LAG_PACKET) {
            struct LagPacket *q = &lagq[lag_n++];
            q->due = now_ms() + lag_ms + (jitter_ms ? lag_rand() % (jitter_ms + 1) : 0);
            q->from = from;
            q->n = len;
            memcpy(q->b, data, (size_t)len);
            return;
        }
    }
    if (ng.ses) { ses_receive(ng.ses, from, data, len, now_ms()); ng.packets_in++; }
}
static void lag_pump(void) /* (each frame) the held packets now due, the earliest first */
{
    for (;;) {
        int k = -1;
        uint32_t t = now_ms();
        for (int i = 0; i < lag_n; i++)
            if ((int32_t)(t - lagq[i].due) >= 0 && (k < 0 || (int32_t)(lagq[i].due - lagq[k].due) < 0)) k = i;
        if (k < 0) return;
        struct LagPacket q = lagq[k];
        lagq[k] = lagq[--lag_n];
        if (ng.ses) { ses_receive(ng.ses, q.from, q.b, q.n, t); ng.packets_in++; }
    }
}
static void receive(void)
{
    if (!ng.udp) return; /* (a server's match: its packets come through the lobby, netplay_poll) */
    uint8_t buf[2048];
    char addr[48];
    uint16_t port;
    int from, n;
    while ((n = udp_poll(ng.udp, &from, addr, &port, buf, sizeof buf)) > 0) {
        if (from < 0 && ng.node == 0) { /* (the host: the machine joining, by its first packet) */
            udp_set_node(ng.udp, 1, addr, port);
            from = 1;
        }
        if (from < 0) continue;
        net_in(from, buf, n);
    }
    lag_pump();
}

static void battle_left_check(void) /* (each frame hook) the battle left: a tournament's next level waits for the host's state */
{
    /* a round started without the battle left (a co-op level's boss phase: the arena again, the boss's car loaded, the
     * players placed anew), as a battle again: every machine from the host's state (each made its own: the user's take,
     * 48 pieces parted, every machine its own hash) */
    int vp_round_serial(void);
    static int seen_round = -1;
    int r = vp_round_serial();
    if (r != seen_round) {
        int fresh = seen_round >= 0;
        seen_round = r;
        if (fresh && ng.on && feats_in_battle() && !ng.out_of_battle && ng.each_battle && ng.steps > 0) {
            ng.out_of_battle = 1;
            fprintf(stderr, "network: a round again in the battle (a boss phase): from the host's state\n");
            if (ses_host(ng.ses) != ng.node) ses_await_state(ng.ses);
        }
    }
    if (ng.on && !feats_in_battle() && !ng.out_of_battle) {
        ng.out_of_battle = 1;
        if (getenv("TM2PC_NET_DEBUG")) fprintf(stderr, "network: the battle left after step %d\n", (int)rb_now(ses_rollback(ng.ses)) - 1);
        if (ng.each_battle && ng.steps > 0 && ses_host(ng.ses) != ng.node) ses_await_state(ng.ses); /* (the next level's) */
    }
}
/* each SetFrameStart (hle.c): in a battle, the step's inputs agreed before it runs */
static void frame_body(void);
void netplay_frame(void)
{
    if (ng.rollback && ng.on) { /* (rollback: the steps at the iteration's end, netplay_point; a battle left is seen
                                  * here, its frames out of a battle: the next level's from the host's state) */
        battle_left_check();
        return;
    }
    ng.at_step = 1;
    frame_body();
    ng.at_step = 0;
}
/* with rollback, the iteration's end (net/rb_state.c: the step and its drawing done): the inputs, a rollback, the next
 * step released */
void netplay_point(void)
{
    if (!ng.rollback || !ng.on) return;
    ng.at_step = 1;
    frame_body();
    ng.at_step = 0;
}
int netplay_rollback(void) { return ng.on && ng.rollback; }
/* The humans whose slot starts or ends at step f (a machine gone, back, or dropping in): at a step every machine has,
 * before it runs. Lockstep after the step is released; rollback for each step it runs, live or again (a rewind past
 * one undoes it: ng.gone and ng.back are rollback's state, rbs_keep) */
static void slot_events(int32_t f)
{
    const Rollback *rb = ses_rollback(ng.ses);
    for (int h = 0; h < ng.humans && h < RB_MAX_SLOTS; h++) {
        int32_t a, b;
        rb_slot_get_interval(rb, h, &a, &b);
        if (a > 0 && b == RB_NEVER && f >= a && !((ng.back >> h) & 1)) { /* back: from its step on, its car in again */
            ng.back |= 1u << h;
            ng.gone &= ~(1u << h);
            ng.offered &= ~(1u << h);
            if (lob) { /* (its button table as the lobby has it now: one copied as it was told of may be older) */
                const LobbyView *lv = lobby_view(lob);
                for (int n = 0; n < SES_MAX_NODES; n++) {
                    int first = lv->start.first[n];
                    if (n != ng.node && first != 0xFF && h >= first && h < first + lv->m[n].count) humans_set_table(h, lv->m[n].p[h - first].table);
                }
            }
            int how = vp_human_join(h, ui_net_fixed_cpus()); /* (a new record: dropping in; 3: a Versus Tournament's
                                                              * next stage) */
            rbs_dmd_again(); /* (its car's models: the list again, every machine at this step) */
            if (how == 2) humans_tables_again(); /* (every table written again: the newcomer's too, the same everywhere) */
            if (rbs_resimulating) continue; /* (a step run again: told the first time) */
            fprintf(stderr, "network: player %d (%s) %s at step %d\n", h + 1, ng.names[h],
                    how == 3 ? "joins from the next stage" : how == 2 ? "drops in" : "is back", (int)f);
            char t[40];
            notice_fit(t, sizeof t, ng.names[h][0] ? ng.names[h] : "A PLAYER",
                       how == 3 ? " JOINS NEXT STAGE" : how == 2 ? " JOINED THE MATCH" : " IS BACK", how == 3 ? " JOINS" : how == 2 ? " JOINED" : " BACK");
            for (char *q = t; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
            vp_net_notice(t);
            continue;
        }
        if ((ng.gone >> h) & 1 || b == RB_NEVER || f < b) continue;
        ng.gone |= 1u << h;
        ng.back &= ~(1u << h);
        if (ng.quiet_gone) continue; /* (a machine coming back: gone before it came, its state has it so) */
        if (!rbs_resimulating) fprintf(stderr, "network: player %d (%s) left the match at step %d\n", h + 1, ng.names[h], (int)f);
        int in = 0;
        for (int o = 0; o < ng.humans; o++) in += !((ng.gone >> o) & 1);
        int cpu = ui_net_spot_cpu(in) && vp_human_handover(h); /* (the user's rule: a CPU / OPEN spot a computer car takes over) */
        if (!cpu)
            vp_human_left(h);
        rbs_dmd_again();
        if (rbs_resimulating) continue; /* (a step run again: told the first time) */
        if (cpu) fprintf(stderr, "network: player %d's spot was CPU / OPEN: a computer car drives on\n", h + 1);
        char t[40];
        notice_fit(t, sizeof t, ng.names[h][0] ? ng.names[h] : "A PLAYER", cpu ? " LEFT: CPU DRIVES ON" : " LEFT THE MATCH", cpu ? ": CPU" : " LEFT");
        for (char *q = t; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        vp_net_notice(t);
    }
}
int ui_net_quit_take(void); /* ui/options.c: the online menu's QUIT chosen */
static void frame_body(void)
{
    if (ng.gave_up) return; /* (the match ended before this machine was in: its battle quitting, then the lobby) */
    if (ng.on && feats_in_battle() && !rbs_resimulating && ui_net_quit_take()) { /* QUIT: the battle quits here, between
                                                                               * steps; the others see this machine leave */
        ng.gave_up = 1;
        guest_call(GA(0x800D181Cu), 2, 0, 0, 0); /* (as the pause menu's Quit) */
        return;
    }
    if (!ng.tried) start();
    battle_left_check();
    if (!ng.on || !feats_in_battle() || killcam_replaying || rt_fast_forward) return;
    if (ng.out_of_battle) { /* a battle again (a tournament's next level): every machine from the host's state */
        ng.out_of_battle = 0;
        if (ng.wait_stage) { /* (joined during the last stage: playing in this one, in their own car) */
            ng.wait_stage = 0;
            fprintf(stderr, "network: this stage with this machine's players in it\n");
        }
        ng.left_done = 0;
        if (ng.each_battle && ng.steps > 0) {
            ng.battle_steps = 0;
            /* the players who left out of this battle in the state that goes (the host's, before its first step): the
             * machines take it as it is, their own first step's removal not again (it ran there before the state came,
             * then the state undid it, and the host's came after the state it sent: a Versus Tournament's stage 5 parted
             * the host from both machines, 39 pieces, the leaver's car and what it did) */
            ng.left_done = 1;
            if (ses_host(ng.ses) == ng.node) {
                for (int h = 0; h < ng.humans && h < NG_PLAYERS; h++)
                    if ((ng.gone >> h) & 1) vp_human_left(h);
                rbs_dmd_again();
                for (int n = 0; n < SES_MAX_NODES; n++)
                    if (n != ng.node && ses_present(ng.ses, n)) ses_push_state(ng.ses, n, now_ms()); /* (every machine in it: one in since, watching or playing, too) */
            }
            fprintf(stderr, "network: the next battle, from the host's state\n");
            ng.want_base = ses_host(ng.ses) != ng.node && !getenv("TM2PC_NET_NO_DELTA"); /* (its base for it) */
        }
    }
    if (ng.join_ask) { /* coming back: the battle's own start runs first (its tables set as a battle's first frames
                        * set them), then the ask; the host's state from its next steps over all of it */
        if (ng.join_wait-- > 0) return;
        ses_ask_join(ng.ses, ng.join_ask);
        ng.join_ask = 0;
        ng.want_base = !getenv("TM2PC_NET_NO_DELTA"); /* (after the ask: the host takes nothing before it) */
    }
    if (ng.want_base && ses_host(ng.ses) != ng.node && base_send()) ng.want_base = 0; /* (this machine's battle loaded) */
    state_apply(); /* (a state that came early: this is its step) */
    RbInput local[RB_MAX_SLOTS];
    memset(local, 0, sizeof local);
    for (int j = 0; j < ng.count && ng.first + j < RB_MAX_SLOTS; j++) local[ng.first + j] = pack(ng.live[j]);
    ng.advanced = 0;
    if (ng.node == ng.host && !ng.pushed) { /* the match's first step: the host's state to every machine */
        ng.pushed = 1;
        for (int n = 0; n < SES_MAX_NODES; n++)
            if (n != ng.node && ses_present(ng.ses, n)) ses_push_state(ng.ses, n, now_ms()); /* (every machine in it: one in since, watching or playing, too) */
    }
    uint32_t t0 = now_ms(), told = t0, drawn = 0, heard_at = t0;
    int heard_n = ng.packets_in;
    int noticed = 0;
    int caught = 0; /* (rollback: a step caught up in this call already) */
    ng.in_frame = 1;
    ses_set_away(ng.ses, SES_AWAY_NONE); /* (stepping: any wait now is the others') */
    if (!ng.last_ms || t0 - ng.last_ms > 1000) ses_forgive_quiet(ng.ses, t0); /* (back from a replay, or the battle's
                                                                                 * load: no one gone for the quiet) */
    for (;;) {
        const SesStats *ms = ses_stats(ng.ses);
        if (ms->migrations != ng.migrations_seen) { /* the host left: the machine there longest carries on as the host */
            ng.migrations_seen = ms->migrations;
            ng.migrated_ms = now_ms() ? now_ms() : 1;
            fprintf(stderr, "network: the host left the match; machine %d is the host now\n", ses_host(ng.ses));
            if (ses_host(ng.ses) == ng.node && !ses_ready(ng.ses)) { /* (it left before its state came: this machine's
                                                                       * battle, at the same step, is the match's now) */
                ses_ready_self(ng.ses);
                for (int n = 0; n < SES_MAX_NODES; n++)
                    if (n != ng.node && ses_present(ng.ses, n)) ses_push_state(ng.ses, n, now_ms()); /* (every machine in it: one in since, watching or playing, too) */
                fprintf(stderr, "network: no state came from the old host: this machine's battle goes to the others\n");
            }
        }
        if (now_ms() - t0 >= 700 && now_ms() - drawn >= 100 && ng.battle_steps >= 8) { /* (a hold the players see: why) */
            drawn = now_ms();
            noticed = 1;
            char line[64] = "";
            int h = ses_host(ng.ses);
            const LobbyView *v = lob ? lobby_view(lob) : NULL;
            if (h != ng.node && ses_quiet_ms(ng.ses, h, now_ms()) >= 2500) { /* the host quiet a while (it may be a moment's stall: a new
                                                                               * host only after 5 s): soon another */
                ui_net_wait_notice("HOST DROPPED", "Finding a new host...");
            } else if (ng.migrated_ms && now_ms() - ng.migrated_ms < 5000) {
                if (h == ng.node) snprintf(line, sizeof line, "You are the host now.");
                else snprintf(line, sizeof line, "Transferring host to %s...", v && v->m[h].count ? v->m[h].p[0].name : "another player");
                ui_net_wait_notice("HOST DROPPED", line);
            } else { /* why: the machines whose inputs are missing, each heard lately and saying it is in a replay or
                      * between screens (the user's 2026-11-07: "Connection trouble" when someone was only still
                      * watching the kill cam), else the connection */
                int why = -1, who = -1, many = 0;
                int32_t at = rb_now(ses_rollback(ng.ses));
                for (int n = 0; v && n < SES_MAX_NODES; n++) {
                    if (n == ng.node || !v->m[n].present || v->start.first[n] == 0xFF) continue;
                    int behind = 0;
                    for (int j = 0; j < v->m[n].count && v->start.first[n] + j < RB_MAX_SLOTS; j++)
                        behind |= rb_known_through(ses_rollback(ng.ses), v->start.first[n] + j) < at;
                    if (!behind) continue;
                    int aw = ses_away(ng.ses, n), heard = ses_quiet_ms(ng.ses, n == h || h == ng.node ? n : h, now_ms()) < 1000;
                    if (!aw || !heard) { why = 0; break; } /* (one machine with no reason: the connection) */
                    if (who >= 0) many = 1;
                    who = n;
                    why = why == SES_AWAY_REPLAY || aw == SES_AWAY_REPLAY ? SES_AWAY_REPLAY : aw;
                }
                if (why > 0 && who >= 0) {
                    const char *name = v->m[who].count ? v->m[who].p[0].name : "a player";
                    if (why == SES_AWAY_REPLAY) {
                        if (many) snprintf(line, sizeof line, "Waiting for the others to finish the replay.");
                        else snprintf(line, sizeof line, "Waiting for %s to finish the replay.", name);
                    } else if (many) snprintf(line, sizeof line, "Waiting for the others to catch up.");
                    else snprintf(line, sizeof line, "Waiting for %s to catch up.", name);
                } else snprintf(line, sizeof line, "Connection trouble. Match will resume in a moment.");
                ui_net_wait_notice("WAITING FOR PLAYERS", line);
                static char told_line[64];
                if (strcmp(told_line, line)) { snprintf(told_line, sizeof told_line, "%s", line); fprintf(stderr, "network: wait notice: %s\n", line); }
            }
        }
        if (now_ms() - told >= 2000) { /* (a wait: where it stands, every 2 s) */
            told = now_ms();
            int other = 0; /* (the host: the first other machine; a machine: the host) */
            while (other < SES_MAX_NODES - 1 && (other == ng.node || !((ng.nodes >> other) & 1))) other++;
            int got, pending = ses_rel_pending(ng.ses, ng.node == ng.host ? other : ses_host(ng.ses), &got);
            fprintf(stderr, "network: waiting at step %d (confirmed %d), packets in %d, reliable out %d, state in %d\n",
                    (int)rb_now(ses_rollback(ng.ses)), (int)rb_confirmed(ses_rollback(ng.ses)), ng.packets_in, pending, got);
            char kn[160] = "";
            for (int i = 0, at = 0; i < ng.humans && i < RB_MAX_SLOTS && at < (int)sizeof kn - 12; i++)
                at += snprintf(kn + at, sizeof kn - (size_t)at, " %d", (int)rb_known_through(ses_rollback(ng.ses), i));
            fprintf(stderr, "network: inputs known through, by player:%s\n", kn);
            if (lob) fprintf(stderr, "network: the host in a match: %d (seen %d; steps %d)\n", lobby_view(lob)->match_on, ng.saw_match_on, ng.battle_steps);
        }
        receive();
        netplay_poll(); /* (the server too: its machines stay in) */
        if (now_ms() - ng.sent_ms >= SEND_EVERY_MS) { /* (first: a state held for a machine goes from this boundary) */
            ng.sent_ms = now_ms();
            ses_send(ng.ses, ng.sent_ms);
        }
        if (ng.rollback) /* (no step ahead of the inputs, a moment's wait on this machine: while a state is owed to a
                          * machine (made at a confirmed step), and while the next step ends the round with an input
                          * before it still a prediction (every machine ends it at the same step: decomp 15.4)) */
            rb_set_window(ses_rollback(ng.ses), ses_state_owed(ng.ses) || end_due_predicted() ? 0 : ng.window);
        ses_step(ng.ses, local, now_ms());
        resim_close();
        state_apply(); /* (the host's state, come while waiting: before the step runs, and before its hash goes out) */
        if (ng.advanced || now_ms() - ng.sent_ms >= SEND_EVERY_MS) { /* (a step out: its input now; waiting: now and then) */
            ng.sent_ms = now_ms();
            ses_send(ng.ses, ng.sent_ms);
        }
        if (ng.advanced) {
            if (ng.rollback) end_due_told();
            if (ng.rollback && !caught && ses_behind(ng.ses) && !getenv("TM2PC_NET_NO_CATCHUP") && catch_up_step()) { /* (behind the host: the step just
                                                                                       * released run now, unseen; the
                                                                                       * next released for the loop) */
                caught = 1;
                ng.advanced = 0;
                continue;
            }
            break;
        }
        if (lob && ng.battle_steps < 8 && !lobby_is_host(lob)) { /* (still waiting for the host's state: the match over
                                                                  * meanwhile, no state to come: back to the lobby) */
            int on = lobby_view(lob)->match_on;
            if (on) ng.saw_match_on = 1;
            else if (ng.saw_match_on) {
                fprintf(stderr, "network: the match ended before this machine was in: back to the lobby\n");
                ng.gave_up = 1; /* (still a server's match: the menus take it back to its lobby) */
                guest_call(GA(0x800D181Cu), 2, 0, 0, 0); /* (the battle quits, as the pause menu's Quit) */
                return;
            }
        }
        if (ng.packets_in != heard_n) { heard_n = ng.packets_in; heard_at = now_ms(); } /* (the others still there) */
        if (now_ms() - (ng.battle_steps < 8 ? t0 : heard_at) > (ng.battle_steps < 8 ? (ng.each_battle ? 180000u : 60000u) : 10000u) ||
            now_ms() - t0 > 60000u) { /* the other machine silent (a minute to load the match; in a battle, nothing from
                                       * anyone for 10 s, or a minute's wait while they talk: a state through the relay
                                       * to a machine coming back takes some seconds): on without it */
            fprintf(stderr, "network: the other machine is silent, the match goes on alone\n");
            ng.on = 0;
            return;
        }
        SDL_PumpEvents(); /* (the window stays responsive while waiting) */
        SDL_Delay(1);
    }
    ng.in_frame = 0;
    if (noticed) ui_net_wait_notice(NULL, NULL);
    ng.last_ms = now_ms();
    if (!ng.rollback) slot_events(rb_now(ses_rollback(ng.ses)) - 1);
    ng.quiet_gone = 0;
    if (ng.migrated_ms && now_ms() - ng.migrated_ms < 3000) { /* (the new host, a moment over the game) */
        static uint32_t told_ms;
        if (told_ms != ng.migrated_ms) {
            told_ms = ng.migrated_ms;
            int h = ses_host(ng.ses);
            const LobbyView *v = lob ? lobby_view(lob) : NULL;
            char t[40];
            snprintf(t, sizeof t, "NEW HOST: %s", h == ng.node ? "YOU" : v && v->m[h].count ? v->m[h].p[0].name : "?");
            vp_net_notice(t);
        }
    }
    ng.steps++;
    ng.battle_steps++;
    if (getenv("TM2PC_NET_DEBUG") && ng.battle_steps <= 4) { void killcam_keep_debug(int step); killcam_keep_debug((int)rb_now(ses_rollback(ng.ses)) - 1); }
    if (getenv("TM2PC_NET_DEBUG") && ((rb_now(ses_rollback(ng.ses)) - 1) % 25 == 0 || ng.battle_steps < 40)) { /* tests: the gameplay clock by step */
        uint64_t vp_gv(void);
        fprintf(stderr, "network: step %d gv %llu vblanks %llu\n", (int)rb_now(ses_rollback(ng.ses)) - 1, (unsigned long long)vp_gv(),
                (unsigned long long)M.vblanks);
    }
    if (ng.battle_steps == 1 && !ng.left_done) /* (a battle's first step: the players who left stay out, every machine at
                                                 * this step; a battle from the host's state has it already) */
        for (int h = 0; h < ng.humans && h < NG_PLAYERS; h++)
            if ((ng.gone >> h) & 1) vp_human_left(h);
    if (getenv("TM2PC_NET_DMDDUMP") && getenv("TM2PC_STATE_LOG")) { /* tests: the hashed model fields, every 8th step
                                                                     * from the given one for 400 (a file) */
        static FILE *df;
        void killcam_dmd_dump(FILE *f, int32_t step);
        int32_t s = rb_now(ses_rollback(ng.ses)) - 1, from = atoi(getenv("TM2PC_NET_DMDDUMP"));
        if (!df) {
            char path[1024];
            snprintf(path, sizeof path, "%s.dmd", getenv("TM2PC_STATE_LOG"));
            df = fopen(path, "wb");
        }
        if (df && s >= from && s < from + 400 && s % 8 == 0) { killcam_dmd_dump(df, s); fflush(df); }
    }
    if (getenv("TM2PC_NET_CARDUMP") && getenv("TM2PC_STATE_LOG")) { /* tests: every car record, every step (a file) */
        static FILE *cf;
        void killcam_cars_dump(FILE *f, int32_t step);
        if (!cf) {
            char path[1024];
            snprintf(path, sizeof path, "%s.cars", getenv("TM2PC_STATE_LOG"));
            cf = fopen(path, "wb");
        }
        if (cf) killcam_cars_dump(cf, rb_now(ses_rollback(ng.ses)) - 1);
    }
    if (getenv("TM2PC_NET_DUMP_AT") && getenv("TM2PC_STATE_LOG")) { /* tests: the RAM at match steps from..to by every */
        int from = 0, to = -1, every = 1;
        sscanf(getenv("TM2PC_NET_DUMP_AT"), "%d-%d/%d", &from, &to, &every);
        int32_t ran = rb_now(ses_rollback(ng.ses)) - 1;
        if (ran >= from && ran <= to && every > 0 && (ran - from) % every == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s.at%d.ram", getenv("TM2PC_STATE_LOG"), (int)ran);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(M.ram, 1, GUEST_RAM_BYTES, f); fclose(f); }
        }
    }
    if (getenv("TM2PC_NET_DUMP") && getenv("TM2PC_STATE_LOG")) { /* tests: the RAM at these steps of each match (a list) */
        char want[16];
        snprintf(want, sizeof want, ",%d,", (int)ng.steps);
        char list[256];
        snprintf(list, sizeof list, ",%s,", getenv("TM2PC_NET_DUMP"));
        if (strstr(list, want)) {
            char path[1024];
            snprintf(path, sizeof path, "%s.step%d.ram", getenv("TM2PC_STATE_LOG"), (int)ng.steps);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(M.ram, 1, GUEST_RAM_BYTES, f); fclose(f); }
        }
    }
    const SesStats *st = ses_stats(ng.ses);
    if (st->desyncs_seen != ng.desyncs_seen) {
        ng.desyncs_seen = st->desyncs_seen;
        fprintf(stderr, "network: DESYNC at step %d (%d so far)\n", (int)ng.steps, st->desyncs_seen);
    }
    if (ng.steps % 600 == 0) {
        const RbStats *rs = rb_stats(ses_rollback(ng.ses));
        fprintf(stderr, "network: step %d, hashes checked %d, desyncs %d, stalls %d", (int)ng.steps, st->hashes_checked,
                st->desyncs_seen, rs->stalls);
        if (ng.rollback) fprintf(stderr, ", rollbacks %d (%d steps again, the longest %d)", rs->rollbacks, rs->resimulated, rs->longest);
        if (getenv("TM2PC_NET_DEBUG")) /* tests: each player's slot (its steps) and the inputs known */
            for (int h = 0; h < ng.humans && h < RB_MAX_SLOTS; h++) {
                int32_t a, b;
                rb_slot_get_interval(ses_rollback(ng.ses), h, &a, &b);
                fprintf(stderr, "\n   slot %d: steps %d-%d, known through %d", h, (int)a, b == RB_NEVER ? -1 : (int)b,
                        (int)rb_known_through(ses_rollback(ng.ses), h));
            }
        fprintf(stderr, "\n");
    }
}

int netplay_on(void) { return ng.on && feats_in_battle() && !killcam_replaying && !rt_fast_forward; }

/* the input seam (killcam_input, by human: vplus/humans.c): this machine's humans' live inputs kept for the next step,
 * every human's the agreed */
uint32_t netplay_input(int kind, int human, uint32_t live)
{
    if (!netplay_on() || kind < 0 || kind >= NG_KINDS || human < 0 || human >= NG_PLAYERS) return live;
    if (human >= ng.first && human < ng.first + ng.count && human - ng.first < NG_LOCAL) /* (in the Players menu: still) */
        ng.live[human - ng.first][kind] = ui_net_menu_holds(human - ng.first) ? (kind == 0 ? 0xFFFFu : kind == 1 ? 4096u : 0u) : live;
    return human < ng.humans ? ng.cur[human][kind] : live;
}

/* ---- the server (net/lobby.c): hosting one or joined, from Network Play's menus (ui/options.c) ---- */
static Lobby *lob;
Lobby *netplay_lobby(void) { return lob; }
static int net_loopback(void) { return getenv("TM2PC_NET_LOCAL") != NULL; } /* (tests: this PC only, no firewall prompt) */
static uint16_t net_port(void) { return getenv("TM2PC_NET_PORT") ? (uint16_t)atoi(getenv("TM2PC_NET_PORT")) : LOBBY_PORT; }
void netplay_leave(void)
{
    voice_io_close();
    if (!lob) return;
    lobby_mods_detach(); /* (Options > Mods' switches again) */
    lobby_free(lob);
    lob = NULL;
    fprintf(stderr, "network: left the server\n");
}
/* the internet: the master servers ([network] masters in the ini; TM2PC_MASTER for tests; none: no internet list,
 * LAN and addresses only) */
static int internet;
static const char *net_masters(void)
{
    const char *e = getenv("TM2PC_MASTER");
    return e && e[0] ? e : settings_masters[0] ? settings_masters : MASTERS_DEFAULT; /* (the ini's own list, else the game's) */
}
int netplay_master_known(void) { char a[MASTERS_MAX][48]; uint16_t p[MASTERS_MAX]; return masters_parse(net_masters(), a, p, MASTERS_MAX); }
/* friends (net/friends.h): player 1's profile online (a guest: none), through the masters (online only) */
static int friends_page;
const char *profile_name(void);
static void friends_setup(void)
{
    uint8_t id[8], secret[16];
    if (profile_online_identity(id, secret)) friends_identity(id, secret, profile_name());
    else friends_identity(NULL, NULL, NULL);
    friends_masters(internet && netplay_master_known() ? net_masters() : "", net_loopback());
}
void netplay_friends_page(int on) { friends_page = on; if (on) friends_setup(); } /* (ui/options.c: the Friends page, the browser) */
void netplay_masters_changed(void) { friends_setup(); } /* (Online Services: another list) */
void netplay_internet(int on) { internet = on || getenv("TM2PC_NET_INTERNET"); friends_setup(); } /* (tests: always) */
static void use_master(void)
{
    if (lob && friends_my_id()) lobby_set_online_id(lob, 0, friends_my_id()); /* (player 1's: the others add them as friends) */
    if (!lob || !internet || !netplay_master_known()) return;
    lobby_set_masters(lob, net_masters());
    fprintf(stderr, "network: the master servers %s\n", net_masters());
}
int netplay_host(const char *server, int count, const char (*names)[LOBBY_NAME])
{
    netplay_leave();
    lob = lobby_host(net_port(), net_loopback(), server, count, names);
    fprintf(stderr, lob ? "network: hosting \"%s\" on port %u\n" : "network: could not host \"%s\" (port %u taken?)\n", server,
            (unsigned)net_port());
    if (lob) lobby_mods_attach(lob); /* (its mods that are on: the server's) */
    use_master();
    return lob != NULL;
}
int netplay_join(const char *address, int count, const char (*names)[LOBBY_NAME]) /* "host" or "host:port" */
{
    netplay_leave();
    char host[128];
    snprintf(host, sizeof host, "%s", address);
    uint16_t port = net_port();
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
    lob = host[0] ? lobby_join(host, port, net_loopback(), count, names) : NULL;
    fprintf(stderr, lob ? "network: joining %s port %u\n" : "network: %s port %u not found\n", host, (unsigned)port);
    if (lob) lobby_mods_attach(lob); /* (the server's list comes: the pop-up if some are missing) */
    use_master();
    return lob != NULL;
}
static LanSearch *lan; /* the LAN browser's question (its answers timed as they come: each frame) */
static void rejoin_check(void) /* (host, a match on) a machine in the lobby, not in the match: its players come back? */
{
    const LobbyView *v = lobby_view(lob);
    if (ng.offered && (!v->m[ng.offered_node].present || v->m[ng.offered_node].id != ng.offered_id || now_ms() - ng.offered_ms > 60000))
        ng.offered = 0; /* (the machine asked was the one still leaving: gone now; or it never came in) */
    for (int k = 0; k < SES_MAX_NODES; k++) {
        if (k == ng.node || !v->m[k].present || !v->m[k].count) continue;
        int f = v->start.first[k]; /* (in the match: its humans there and not gone; a node given again is a new machine) */
        if (f != 0xFF && f < NG_PLAYERS && !((ng.gone >> f) & 1)) continue;
        for (int h0 = 0; h0 + v->m[k].count <= ng.humans; h0++) {
            int match = 1;
            for (int j = 0; j < v->m[k].count && match; j++) {
                int h = h0 + j;
                match = ((ng.gone >> h) & 1) && !((ng.offered >> h) & 1) &&
                        (ng.mid[h] == v->m[k].id || !strcmp(ng.names[h], v->m[k].p[j].name)); /* (its number, or its names) */
            }
            if (!match) continue;
            uint32_t mask = ((1u << v->m[k].count) - 1u) << h0;
            ng.offered |= mask;
            ng.offered_ms = now_ms();
            ng.offered_node = k;
            ng.offered_id = v->m[k].id;
            ses_host_offer_slots(ng.ses, mask);
            lobby_rejoin(lob, k, h0);
            fprintf(stderr, "network: %s is back: players %d-%d again\n", v->m[k].p[0].name, h0 + 1, h0 + v->m[k].count);
            break;
        }
        /* a machine new to the match: in to watch it until the next one starts (the user's 2026-11-11: joining
         * mid-match never puts a fresh car in, with full health and lives, against the players there; the lobby's room
         * counts it already: a full server takes no one). Asked once (its number: a node given again is new) */
        /* a machine watching a Versus Tournament: its players in during the stage it watches, playing from the next one
         * in their own cars (new records, after the others, every machine loading them; the user's 2026-11-11); one
         * machine at a time */
        if (f == 0xFF && ng.watch_id[k] == v->m[k].id && !ng.offered && ui_net_join_next_stage() && ng.battle_steps >= 8 &&
            ses_present(ng.ses, k) && /* (in, watching, first: its view on the cars meanwhile) */
            ng.humans + v->m[k].count <= NG_PLAYERS) {
            int h0 = ng.humans, ok = 1;
            for (int n = 0; n < SES_MAX_NODES; n++) ok &= v->start.first[n] == 0xFF || v->start.first[n] < h0; /* (none pending) */
            if (!ok) continue;
            uint32_t mask = ((1u << v->m[k].count) - 1u) << h0;
            ng.offered |= mask;
            ng.offered_ms = now_ms();
            ng.offered_node = k;
            ng.offered_id = v->m[k].id;
            ses_host_offer_slots(ng.ses, mask);
            lobby_set_humans(lob, h0 + v->m[k].count);
            for (int j = 0; j < v->m[k].count; j++) /* (their own pick, else the first player's: the stages after) */
                lobby_set_car(lob, h0 + j, v->m[k].p[j].car >= 0 ? v->m[k].p[j].car : v->start.car[0]);
            lobby_rejoin(lob, k, h0);
            fprintf(stderr, "network: %s joins at this stage: players %d-%d\n", v->m[k].p[0].name, h0 + 1, h0 + v->m[k].count);
            continue;
        }
        int picked = v->m[k].count > 0; /* (their cars chosen in the lobby, ready: then in to watch, the user's) */
        for (int j = 0; j < v->m[k].count; j++) picked &= v->m[k].p[j].ready;
        if (f == 0xFF && ui_net_watch_game() && ng.watch_id[k] != v->m[k].id && picked) {
            ng.watch_id[k] = v->m[k].id;
            lobby_watch(lob, k);
            fprintf(stderr, "network: %s watches the match: in from the next one\n", v->m[k].p[0].name);
        }
    }
}
/* (every machine, a match on) the host's start with more players in it: those dropping in, known (names, machines,
 * button tables, teams) before their slots begin */
static void dropin_learn(void)
{
    const LobbyView *v = lobby_view(lob);
    if (v->start.humans <= ng.humans || v->start.humans > NG_PLAYERS) return;
    int from = ng.humans;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        int f = v->start.first[n];
        if (f == 0xFF) continue;
        for (int i = 0; i < v->m[n].count && f + i < v->start.humans; i++) {
            int h = f + i;
            if (h < from) continue;
            snprintf(ng.names[h], sizeof ng.names[h], "%s", v->m[n].p[i].name);
            ng.mid[h] = v->m[n].id;
            humans_add(h, n == ng.node ? i : -1, v->m[n].p[i].table);
            if (v->m[n].p[i].team >= 0) battle_match()->player_team[h & 15] = (uint8_t)v->m[n].p[i].team;
        }
    }
    ng.humans = v->start.humans;
    fprintf(stderr, "network: players %d-%d drop in\n", from + 1, ng.humans);
    if (ng.watch && v->start.first[ng.node] != 0xFF) { /* this machine watching: its players are in now (their slots
                                                        * asked; their cars from the step the host gives) */
        ng.watch = 0;
        ng.wait_stage = ui_net_versus(); /* (a Versus Tournament: playing from the next stage, watching this one out) */
        ng.first = v->start.first[ng.node];
        ng.count = v->m[ng.node].count;
        uint32_t mask = 0;
        for (int j = 0; j < ng.count && ng.first + j < RB_MAX_SLOTS; j++) mask |= 1u << (ng.first + j);
        lobby_rejoin_done(lob);
        ui_net_starts_caught_up(); /* (the lobby's start moved on for it: not a match to launch) */
        ses_ask_join(ng.ses, mask);
        fprintf(stderr, "network: watching no more: players %d-%d in\n", ng.first + 1, ng.first + ng.count);
        stats_online(1); /* (the Stats screen: a match joined while it was on, for each of this machine's players) */
        for (int j = 0; j < ng.count; j++) stat_add(ng.first + j, STAT_JOINS, 1);
        feats_flush();
    }
}
void netplay_poll(void) /* each VBlank and each menu frame: the server's packets (a machine silent 5 s has left) */
{
    friends_poll(now_ms(), friends_page, lob && internet);
    static int friend_all = -1; /* tests: TM2PC_TEST_FRIEND_ALL, every other player asked, every request accepted */
    if (friend_all < 0) friend_all = getenv("TM2PC_TEST_FRIEND_ALL") != NULL;
    if (friend_all && lob && friends_my_id()) {
        static uint32_t asked_ms;
        if (now_ms() - asked_ms >= 3000) {
            asked_ms = now_ms();
            const LobbyView *v = lobby_view(lob);
            static const uint8_t none[8];
            for (int n = 0; n < SES_MAX_NODES; n++)
                for (int i = 0; n != lobby_node(lob) && v->m[n].present && i < v->m[n].count; i++)
                    if (memcmp(v->m[n].p[i].online_id, none, 8) && !friends_is(v->m[n].p[i].online_id)) friends_add(v->m[n].p[i].online_id);
            for (int i = 0; i < friends_count(1); i++) friends_answer(friends_get(1, i)->id, 1);
        }
    }
    if (lob) lobby_poll(lob, now_ms());
    if (lob) lobby_mods_frame(); /* (host: the list to newcomers, the mods' files to who asked) */
    if (lob) lag_pump(); /* (tests: a bad connection's held packets) */
    if (lob && ng.on && ng.ses) /* a machine through the relay: its state paced under the relay's share */
    {
        int relayed = 0; /* (the relay's share split between them: two machines' states at once stayed over the cap) */
        for (int n = 0; n < SES_MAX_NODES; n++) relayed += lobby_node_relayed(lob, n) && lobby_view(lob)->m[n].present;
        for (int n = 0; n < SES_MAX_NODES; n++)
            ses_set_rate(ng.ses, n, lobby_node_relayed(lob, n) ? (uint32_t)(RELAY_RATE / (relayed ? relayed : 1)) : 0);
    }
    if (lob && ng.on && ng.ses) dropin_learn();
    if (lob && ng.on && ng.ses && lobby_is_host(lob) && feats_in_battle() && !killcam_replaying)
        rejoin_check(); /* (only while the battle runs: not over its results, its final replay) */
    int input_ptt_held(void);                    /* runtime/input.c */
    unsigned ui_net_muted(void);                 /* ui/options.c: the machines this PC muted */
    int ui_net_voice_on(void);                   /* ui/options.c: the server's voice chat on */
    voice_io_frame(lob, lob && ui_net_voice_on() ? input_ptt_held() : -1, ui_net_muted());
    if (ng.on && ng.ses && !ng.in_frame) { /* a match on, this machine not stepping (between a tournament's levels, a kill
                                            * cam's replay, loading): heard by the others (none takes it for gone), the
                                            * host's state acknowledged as it comes */
        static uint32_t sent;
        if (killcam_replaying || now_ms() - ng.last_ms > 250) /* (why, for the others' wait notice: not between two steps) */
            ses_set_away(ng.ses, killcam_replaying ? SES_AWAY_REPLAY : SES_AWAY_BETWEEN);
        if (now_ms() - sent >= 100) { sent = now_ms(); ses_send(ng.ses, sent); }
    }
    if (lan) lobby_lan_results(lan, NULL, 0);
}
void netplay_lan_search(void)
{
    lobby_lan_search_free(lan);
    if (internet) lan = lobby_master_search(net_masters(), net_loopback()); /* (the internet's list: every master) */
    else lan = lobby_lan_search(net_loopback());
}
int netplay_lan_results(LobbyListing *out, int max) { return lan ? lobby_lan_results(lan, out, max) : 0; }
static void match_packet(void *ctx, int from, const uint8_t *data, int len, uint32_t t)
{
    (void)ctx;
    (void)t;
    net_in(from, data, len);
}
/* the host's Start arrived (or was given): this machine's part of the match, its packets on the lobby's socket */
int netplay_match_begin(void)
{
    if (!lob || ng.on) return 0;
    const LobbyView *v = lobby_view(lob);
    ng.node = lobby_node(lob);
    ng.host = lobby_host_node(lob);
    ng.tag = (uint16_t)(v->start.seed ^ (v->start.seed >> 16));
    int watch = ng.node >= 0 && v->start.first[ng.node] == 0xFF && lobby_rejoining(lob); /* (in to watch: no players) */
    if (ng.node < 0 || (v->start.first[ng.node] == 0xFF && !watch)) return 0;
    uint32_t slots[SES_MAX_NODES] = {0};
    ng.nodes = 0;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        if (v->start.first[n] == 0xFF || !v->m[n].count) continue;
        ng.nodes |= 1u << n;
        for (int j = 0; j < v->m[n].count && v->start.first[n] + j < RB_MAX_SLOTS; j++) slots[n] |= 1u << (v->start.first[n] + j);
    }
    ng.watch = watch;
    ng.first = watch ? 0 : v->start.first[ng.node];
    ng.count = watch ? 0 : v->m[ng.node].count;
    memset(ng.names, 0, sizeof ng.names);
    for (int n = 0; n < SES_MAX_NODES; n++)
        for (int j = 0; v->start.first[n] != 0xFF && j < v->m[n].count && v->start.first[n] + j < NG_PLAYERS; j++)
            snprintf(ng.names[v->start.first[n] + j], sizeof ng.names[0], "%s", v->m[n].p[j].name);
    ng.humans = v->start.humans;
    static NetTransport net;
    net = lobby_match_transport(lob);
    lobby_on_match(lob, match_packet, NULL);
    ng.tried = 1; /* (TM2PC_NET's way not taken) */
    int rejoin = lobby_rejoining(lob);
    lobby_rejoin_done(lob);
    for (int n = 0; n < SES_MAX_NODES; n++) /* (each human's machine's number: a machine back is known by it) */
        for (int j = 0; v->start.first[n] != 0xFF && j < v->m[n].count && v->start.first[n] + j < NG_PLAYERS; j++)
            ng.mid[v->start.first[n] + j] = v->m[n].id;
    if (watch) fprintf(stderr, "network: in to watch the match on (playing from the next one)\n");
    else if (rejoin) fprintf(stderr, "network: back into the match on: players %d-%d\n", ng.first + 1, ng.first + ng.count);
    if (lobby_is_host(lob)) lobby_set_match_on(lob, 1);
    ng.saw_match_on = rejoin || lobby_view(lob)->match_on; /* (let back in: a match was on; the end may come before
                                                            * this machine's battle has even loaded) */
    return match_open(&net, slots, v->start.delay ? v->start.delay : netplay_input_delay(), rejoin);
}
/* back in the lobby: the match over on this machine */
void netplay_match_end(void)
{
    if (!ng.ses) return;
    if (lob) lobby_on_match(lob, NULL, NULL);
    if (lob && lobby_is_host(lob)) lobby_set_match_on(lob, 0);
    ng.gave_up = ng.watch = ng.wait_stage = 0;
    ses_free(ng.ses);
    ng.ses = NULL;
    ng.on = 0;
    fprintf(stderr, "network: the match is over (%d steps)\n", (int)ng.steps);
}
int netplay_in_match(void) { return ng.on && lob != NULL; }
int netplay_first_local(void) { return ng.on ? ng.first : 0; } /* this machine's first player (the cars' slot), 0 offline */
int netplay_watching(void) { return ng.on && (ng.watch || ng.wait_stage); } /* this machine watches the match on: no players of its own */
void netplay_each_battle(int on) { ng.each_battle = on; } /* (ui/options.c: a tournament online, each level from the host's state) */
/* human h's name as the match began (a machine gone since: still its player's) */
const char *netplay_human_name(int h) { return ng.on && lob && h >= 0 && h < NG_PLAYERS && ng.names[h][0] ? ng.names[h] : NULL; }
