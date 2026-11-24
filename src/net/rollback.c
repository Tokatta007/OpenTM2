/* The rollback engine (rollback.h, docs/ONLINE_PLAN.md 2). */
#include "rollback.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t start, end;            /* active over [start, end) */
    int32_t through;               /* cache: inputs known through this step */
    RbInput in[RB_RING];           /* the inputs that arrived (or were read), by step % RB_RING */
    int32_t tag[RB_RING];          /* the step each holds (-1: none) */
    RbInput used[RB_RING];         /* what each step was last run with (the prediction, or the input) */
    int32_t used_tag[RB_RING];
} Slot;

struct Rollback {
    RbCallbacks cb;
    uint32_t local;
    int delay, every;
    int window;                    /* the furthest a step runs ahead of the confirmed one (RB_WINDOW; 0: lockstep) */
    int32_t now;                   /* the next step to run */
    int32_t floor;                 /* no step before it can be loaded (a restart from outside) */
    int32_t rollback_to;           /* the earliest step run with a wrong prediction (RB_NEVER: none) */
    Slot slot[RB_MAX_SLOTS];
    uint32_t sum[RB_RING];
    int32_t sum_tag[RB_RING];
    RbStats st;
};

static const RbInput NEUTRAL;

static int active(const Slot *s, int32_t f) { return f >= s->start && f < s->end; }

/* the slot's input for step f if it is known: neutral outside its interval, before the restart point, and over the
 * first `delay` steps of its interval (no machine reads an input for those) */
static int known(const Rollback *rb, const Slot *s, int32_t f, RbInput *out)
{
    if (f < rb->floor || !active(s, f) || f < s->start + rb->delay) {
        if (out) *out = NEUTRAL;
        return 1;
    }
    if (s->tag[f % RB_RING] != f) return 0;
    if (out) *out = s->in[f % RB_RING];
    return 1;
}

static int32_t through(Rollback *rb, int i)
{
    Slot *s = &rb->slot[i];
    int32_t limit = rb->now + RB_RING / 2;
    while (s->through + 1 < limit && known(rb, s, s->through + 1, NULL)) s->through++;
    return s->through;
}

static void forget_from(Rollback *rb, int i, int32_t f) /* the cache may not reach f any more */
{
    if (rb->slot[i].through >= f) rb->slot[i].through = f - 1;
}

Rollback *rb_new(const RbCallbacks *cb, uint32_t local, int delay, int checksum_every)
{
    Rollback *rb = calloc(1, sizeof *rb);
    if (!rb) return NULL;
    rb->cb = *cb;
    rb->local = local;
    rb->delay = delay < 0 ? 0 : delay;
    rb->every = checksum_every < 1 ? 1 : checksum_every;
    rb->window = RB_WINDOW;
    rb->rollback_to = RB_NEVER;
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        Slot *s = &rb->slot[i];
        s->start = s->end = RB_NEVER; /* (inactive until set) */
        s->through = -1;
        for (int k = 0; k < RB_RING; k++) s->tag[k] = s->used_tag[k] = -1;
    }
    for (int k = 0; k < RB_RING; k++) rb->sum_tag[k] = -1;
    return rb;
}

void rb_free(Rollback *rb) { free(rb); }

void rb_set_local(Rollback *rb, uint32_t local) { rb->local = local; }

void rb_slot_interval(Rollback *rb, int i, int32_t start, int32_t end)
{
    if (i < 0 || i >= RB_MAX_SLOTS) return;
    Slot *s = &rb->slot[i];
    /* the steps confirmed as they ran, before the change: with the new interval in place a slot ended at step e counts
     * every step from e as known (neutral), so the confirmed step leapt past e and the rollback to e was refused (a
     * player leaving at a match's first steps: the host ended the slot at 1, a machine at step 3 kept the car in for
     * steps 1-2, and the next hash differed) */
    int32_t c = rb_confirmed(rb) + 1;
    int32_t from = RB_NEVER;
    if (start != s->start) from = start < s->start ? start : s->start;
    if (end != s->end) {
        int32_t e = end < s->end ? end : s->end;
        if (e < from) from = e;
    }
    s->start = start;
    s->end = end;
    if (from == RB_NEVER) return;
    /* (no rollback into confirmed steps: their inputs were every slot's as they stood; a slot given again to a player
     * coming back moves its start from the old one, whose steps stay as they ran) */
    if (from < c) from = c;
    forget_from(rb, i, from);
    if (from < rb->now && from < rb->rollback_to) rb->rollback_to = from < rb->floor ? rb->floor : from;
}

void rb_slot_get_interval(const Rollback *rb, int i, int32_t *start, int32_t *end)
{
    *start = rb->slot[i].start;
    *end = rb->slot[i].end;
}

int rb_remote_input(Rollback *rb, int i, int32_t f, RbInput in)
{
    if (i < 0 || i >= RB_MAX_SLOTS || f < 0) return 0;
    Slot *s = &rb->slot[i];
    if (known(rb, s, f, NULL) || f >= rb->now + RB_RING / 2) return 0; /* (known, neutral, or too far ahead) */
    if (f <= rb->now - RB_RING / 2) return 0;                          /* (too old: a step long gone) */
    if (f < rb->floor) return 0;                                       /* (before this machine's start: its state has it) */
    s->in[f % RB_RING] = in;
    s->tag[f % RB_RING] = f;
    if (f < rb->now) { /* already run: with a prediction; wrong -> back to it */
        int same = s->used_tag[f % RB_RING] == f && !memcmp(&s->used[f % RB_RING], &in, sizeof in);
        if (!same && f < rb->rollback_to) rb->rollback_to = f;
    }
    return 1;
}

int32_t rb_local_input(Rollback *rb, int i, RbInput in)
{
    int32_t f = rb->now + rb->delay;
    rb_remote_input(rb, i, f, in);
    return f;
}

int rb_input(const Rollback *rb, int i, int32_t f, RbInput *out)
{
    if (i < 0 || i >= RB_MAX_SLOTS) return 0;
    return known(rb, &rb->slot[i], f, out);
}

int32_t rb_known_through(const Rollback *rb, int i) { return through((Rollback *)rb, i); }

/* the prediction for a slot's unknown input: its last known one */
static RbInput predict(Rollback *rb, int i, int32_t f)
{
    Slot *s = &rb->slot[i];
    int32_t t = through(rb, i);
    RbInput p = NEUTRAL;
    for (int32_t g = f - 1; g >= t && g > f - RB_RING / 2; g--) /* the latest known before f (beyond the run) */
        if (known(rb, s, g, &p)) return p;
    if (t >= 0) known(rb, s, t, &p);
    return p;
}

static void run(Rollback *rb, int32_t f)
{
    rb->cb.save(rb->cb.ctx, f);
    if (f % rb->every == 0) {
        rb->sum[f % RB_RING] = rb->cb.checksum(rb->cb.ctx);
        rb->sum_tag[f % RB_RING] = f;
    }
    RbInput in[RB_MAX_SLOTS];
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        Slot *s = &rb->slot[i];
        if (!known(rb, s, f, &in[i])) in[i] = predict(rb, i, f);
        s->used[f % RB_RING] = in[i];
        s->used_tag[f % RB_RING] = f;
    }
    rb->cb.advance(rb->cb.ctx, in);
}

int rb_catch_up(Rollback *rb)
{
    if (rb->rollback_to >= rb->now) {
        rb->rollback_to = RB_NEVER;
        return 0;
    }
    int32_t from = rb->rollback_to < rb->floor ? rb->floor : rb->rollback_to;
    rb->rollback_to = RB_NEVER;
    rb->cb.load(rb->cb.ctx, from);
    for (int32_t f = from; f < rb->now; f++) run(rb, f);
    int n = (int)(rb->now - from);
    rb->st.rollbacks++;
    rb->st.resimulated += n;
    if (n > rb->st.longest) rb->st.longest = n;
    return n;
}

int32_t rb_confirmed(const Rollback *rb)
{
    int32_t c = rb->now - 1;
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        int32_t t = through((Rollback *)rb, i);
        if (t < c) c = t;
    }
    return c;
}

int rb_step(Rollback *rb)
{
    rb_catch_up(rb);
    int32_t known_to = RB_NEVER; /* (every slot's inputs known through this step; rb_confirmed's is capped at now - 1, so
                                  * the window 0 of lockstep could never be met with it) */
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        int32_t t = through(rb, i);
        if (t < known_to) known_to = t;
    }
    if (rb->now - known_to > rb->window) { /* too far ahead of what is known: wait */
        rb->st.stalls++;
        return 0;
    }
    run(rb, rb->now);
    rb->now++;
    return 1;
}

int32_t rb_now(const Rollback *rb) { return rb->now; }
void rb_set_window(Rollback *rb, int steps) { rb->window = steps < 0 ? 0 : steps > RB_WINDOW ? RB_WINDOW : steps; }

int rb_checksum(const Rollback *rb, int32_t f, uint32_t *out)
{
    if (f > rb->now || f - 1 > rb_confirmed(rb) || rb->rollback_to < f || rb->sum_tag[f % RB_RING] != f) return 0;
    *out = rb->sum[f % RB_RING];
    return 1;
}

void rb_restart_at(Rollback *rb, int32_t f)
{
    rb->now = f;
    rb->floor = f;
    rb->rollback_to = RB_NEVER;
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        Slot *s = &rb->slot[i];
        for (int k = 0; k < RB_RING; k++) {
            s->used_tag[k] = -1;
            if (s->tag[k] < f) s->tag[k] = -1;
        }
        s->through = f - 1;
    }
    for (int k = 0; k < RB_RING; k++) rb->sum_tag[k] = -1;
    /* this machine's inputs for the steps its delay skips from here (each made at the step `delay` before it, which
     * this machine may not have run: a state for the step after the host's confirmed one, come while this machine was
     * one behind): neutral, sent as any, else no machine ever has them and every machine waits (the user's online
     * playtest 2026-11-01: a resync at step 12, both machines stuck with inputs through 11) */
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        Slot *s = &rb->slot[i];
        if (!((rb->local >> i) & 1)) continue;
        for (int32_t g = f; g < f + rb->delay; g++)
            if (!known(rb, s, g, NULL)) {
                s->in[g % RB_RING] = NEUTRAL;
                s->tag[g % RB_RING] = g;
            }
    }
}

void rb_resum_restart(Rollback *rb)
{
    int32_t f = rb->floor;
    if (rb->now == f + 1 && rb->sum_tag[f % RB_RING] == f) rb->sum[f % RB_RING] = rb->cb.checksum(rb->cb.ctx);
}

int32_t rb_floor(const Rollback *rb) { return rb->floor; }
const RbStats *rb_stats(const Rollback *rb) { return &rb->st; }
