/* The rollback engine (docs/ONLINE_PLAN.md 2): steps a deterministic simulation with every slot's input, predicting the
 * inputs not here yet (a slot's last known one) and, when a real input differs from what was predicted, going back to
 * that step and stepping forward again (resimulation). It knows nothing of the game or the network: the game gives it
 * save / load / advance / checksum, the session (session.c) gives it the inputs that arrive.
 *
 * Steps are numbered from 0. Every slot is active over an interval of steps [start, end): outside it its input is the
 * neutral one (all zero), known everywhere without being sent (a computer car's slot, a player who has not joined yet or
 * has left). A slot's input for step f is "known" once it arrived (or is neutral); step f is confirmed when every
 * slot's inputs up to f are known. The state at the start of step f is final once step f - 1 is confirmed: its checksum
 * is then the same on every machine (or there is a desync). */
#ifndef TM2_NET_ROLLBACK_H
#define TM2_NET_ROLLBACK_H

#include <stdint.h>

enum {
    RB_MAX_SLOTS = 16,
    RB_WINDOW = 8,      /* the furthest a step may run ahead of the confirmed one (steps: 400 ms at 20 a second) */
    RB_RING = 1024,     /* inputs and checksums kept per slot (steps: 51 s; a machine whose state took long to come
                         * starts that far back and needs every input since: 128 ran out over the relay, 2026-11-14) */
    RB_NEVER = 0x7FFFFFFF,
};

typedef struct { uint8_t b[8]; } RbInput; /* one slot's input for a step (buttons, analog axes: the game's packing) */

typedef struct {
    void *ctx;
    void (*save)(void *ctx, int32_t step);             /* keep the current state as the state at the start of step */
    void (*load)(void *ctx, int32_t step);             /* make the state kept for step the current one */
    void (*advance)(void *ctx, const RbInput *inputs);  /* one step, inputs[RB_MAX_SLOTS] */
    uint32_t (*checksum)(void *ctx);                    /* the current state's hash (render-only parts left out) */
} RbCallbacks;

typedef struct {
    int rollbacks;       /* times a real input differed from the prediction */
    int resimulated;     /* steps stepped again */
    int stalls;          /* steps waited for (too far ahead of the confirmed step) */
    int longest;         /* the longest rollback (steps) */
} RbStats;

typedef struct Rollback Rollback;

/* local: the slots this machine reads; delay: input delay in steps (its inputs are for the step `delay` ahead);
 * checksum_every: a checksum kept at the start of every Nth step (1: each) */
Rollback *rb_new(const RbCallbacks *cb, uint32_t local, int delay, int checksum_every);
void rb_free(Rollback *rb);

void rb_slot_interval(Rollback *rb, int slot, int32_t start, int32_t end); /* active over [start, end) (RB_NEVER: open) */
void rb_slot_get_interval(const Rollback *rb, int slot, int32_t *start, int32_t *end);
void rb_set_local(Rollback *rb, uint32_t local);

int32_t rb_local_input(Rollback *rb, int slot, RbInput in); /* a local slot's input, for step now + delay (returned) */
int rb_remote_input(Rollback *rb, int slot, int32_t step, RbInput in); /* 1 new, 0 known already or out of range */
int rb_input(const Rollback *rb, int slot, int32_t step, RbInput *out); /* 1 known (out set) */
int32_t rb_known_through(const Rollback *rb, int slot); /* the last step whose input (and all before) is known */

int rb_step(Rollback *rb);          /* rolls back if needed, then one step: 1 stepped, 0 stalled (wait for inputs) */
int rb_catch_up(Rollback *rb);      /* the pending rollback alone (no new step) */
int32_t rb_now(const Rollback *rb);          /* the next step to run */
/* how far a step may run ahead of the confirmed one (RB_WINDOW, the default); 0: lockstep, each step only once every
 * slot's input for it is here (no prediction, nothing ever rolled back: the game's own loop runs the steps) */
void rb_set_window(Rollback *rb, int steps);
int32_t rb_confirmed(const Rollback *rb);    /* the last confirmed step (-1: none) */
int rb_checksum(const Rollback *rb, int32_t step, uint32_t *out); /* 1: final checksum of the state at step's start */

/* After the game loaded a state from outside (a resync, a late join) as the state at the start of step: carry on from
 * there (the inputs from step on are kept; nothing before step can be rolled back to). */
void rb_restart_at(Rollback *rb, int32_t step);
/* the restart step's checksum again, when its state was put in place after that step's start ran (lockstep: the
 * state kept for the battle's step) */
void rb_resum_restart(Rollback *rb);
int32_t rb_floor(const Rollback *rb); /* the last restart's step (nothing before it rolled back to) */

const RbStats *rb_stats(const Rollback *rb);

#endif
