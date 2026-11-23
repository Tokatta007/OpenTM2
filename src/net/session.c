/* A network match (session.h, docs/ONLINE_PLAN.md 1-3, 9). */
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAGIC = 0x4E54, P_INPUT = 1, P_REL = 2, P_HASH = 3, P_VOICE = 4, P_BYE = 5 }; /* packets */
enum { R_SLOT = 1, R_NODE = 2, R_STATE = 3, R_JOIN = 4, R_CHAT = 5, R_VOTE_START = 6, R_VOTE = 7, R_VOTE_TALLY = 8,
       R_KICK_VOTE = 9, R_KICK_STATE = 10, R_KICKED = 11, R_APP = 12, R_BASE = 13 }; /* reliable messages */
enum { BASE_MAX = 65536 }; /* a machine's base (ses_send_base): the game's block hashes of its own state */
enum { RUN_MAX = 32, ACKS_MAX = 128, ACK_QUEUE = 1024, HOLD_MAX = 256, CHUNK = 1000, RTO_MS = 150, RTO_MAX = 1500,
       PACKET_MAX = 1400, PEND_MAX = 128,
       REL_WINDOW = HOLD_MAX, /* reliable messages in flight at most past the oldest not acked: what the other side can
                               * hold behind a lost one (more is sent only to be dropped and sent again) */
       REL_BURST = 64 /* reliable messages sent a ses_send, a machine (a state: thousands of chunks) */ };

typedef struct { uint16_t id; uint8_t *data; int len; uint32_t sent_ms; uint8_t tries; } RelOut;
typedef struct { uint16_t id; uint8_t *data; int len; } RelIn;

typedef struct {
    int present;
    uint32_t slots;
    uint32_t heard_ms;
    int32_t known[RB_MAX_SLOTS];   /* what it reported knowing (inputs through, per slot) */
    int32_t now;                   /* its step, when last heard */
    uint32_t now_ms;
    RelOut *out;                   /* the reliable messages not acked yet */
    int out_n, out_cap;
    uint16_t out_id, in_id;
    RelIn hold[HOLD_MAX];          /* arrived ahead of their turn */
    int hold_n;
    uint16_t acks[ACK_QUEUE];      /* (what arrived, to ack: ACKS_MAX a packet, the rest in the next) */
    int ack_n;
    uint32_t srtt;                 /* the time from a reliable message to its ack, smoothed (0: not yet known) */
    uint32_t echo_ts, echo_at;     /* its last packet's clock, and when it came (sent back: its round trip) */
    uint32_t rtt;                  /* the round trip to it, smoothed, from the input packets (0: not yet known) */
    uint32_t resync_ms;            /* (host) a state sent to it then: no other for a while */
    int resync_wait;               /* (host) a resync's state on its way to it: the match held until its last part is
                                    * acked (resync_id), so it starts again where the others are (see resync_held) */
    uint16_t resync_id;
    int state_due;                 /* (host) a state it waits for that the game could not make yet (a replay showing) */
    int32_t state_sent;            /* (host) the step of the last state sent to it (-1 none): its hashes count only when
                                    * made in the world that state began (one made before it arrived: a resync's
                                    * hashes of the steps it had already run, judged again, set off another) */
    int32_t hash_from;             /* (host) its hashes from this step on: the earlier ones came from its own world, before
                                    * the host's state replaced it (a machine coming back, a resync) */
    uint32_t rate;                 /* reliable bytes a second to it at most (0: no limit; a relayed machine's: the relay's) */
    uint32_t budget, budget_ms;    /* (its pacing: bytes it may send now, refilled with time) */
    uint32_t voice_ms;             /* (host) its last voice frame (the 8 speakers at once) */
    uint32_t kick_ms;              /* (host) removed then (gone a moment later, once told) */
    uint32_t load_ms;              /* (host) no input of its own yet (loading the match) since then (0: in, or not looked) */
    int played;                    /* it runs the match's steps (its step moved on) */
    int32_t first_now;             /* its first step said above 0 (0: none yet) */
    uint8_t *app;                  /* an app message arriving (R_APP pieces, in order) */
    int app_got;
    uint8_t *base;                 /* (host) its base: the hashes of its own state's blocks (the game's), arriving */
    int base_len, base_got;
    int base_new;                  /* (host) a whole base not yet used: the next state to it only what differs from it */
    int want_full;                 /* (host) it asked for the whole state (its base gone: another battle's) */
    uint32_t due_ms;               /* (host) a state owed to it since then, waiting for its base (base_wait_ms at most) */
    int away;                      /* (host) what it said it is doing: SES_AWAY_* (not stepping: a replay, between screens) */
} Peer;

struct Session {
    int node, host, delay, ready, migrating, lockstep, one_step, idle;
    int behind;                    /* (one_step) this machine 2 steps or more behind the host's: the game may catch up */
    uint16_t magic;                /* MAGIC, or with a match's tag (ses_set_tag): another match's packets dropped */
    uint32_t migrate_ms, departed; /* (a new host) the slots of the host that left: ended once the inputs are in */
    uint32_t send_all_until;       /* (a machine, the host moved) every slot's inputs to the new host until then */
    uint32_t offer;                /* (host) slots a joining machine may take */
    Rollback *rb;
    NetTransport net;
    NetGame game;
    Peer peer[SES_MAX_NODES];
    int32_t next_hash;             /* (clients) the next step whose hash goes to the host */
    int32_t state_based;           /* (clients) the step of the host's state last put in place (-1 none): its hashes say so */
    struct { int node; int32_t step; uint32_t sum; } pend[PEND_MAX]; /* (host) hashes waiting for the host's own */
    int pend_n;
    uint8_t *rx;                   /* a state arriving */
    uint32_t rx_total, rx_got;
    int32_t rx_step;
    uint8_t *tx;                   /* a state to send */
    uint32_t base_wait_ms;         /* (host) a state owed waits this long for the machine's base (0: never: whole) */
    const uint8_t *cur_base;       /* (host) while the game makes a state: the machine's base (ses_state_base) */
    int cur_base_len;
    SesVoiceFn voice;
    void *voice_ctx;
    uint32_t voice_ms;             /* (host) its own last voice frame */
    SesEvents ev;
    struct { int round, n, result; uint8_t options[SES_VOTE_MAX]; int8_t choice[SES_MAX_NODES]; uint32_t until_ms, sig; } vote;
    struct { int target; uint32_t yes, started_ms, sig; } kick; /* (host) the kick vote running (target -1: none) */
    uint32_t banned;               /* (host) machines kicked: no rejoining this session */
    int kicked;                    /* this machine was removed */
    int away_self;                 /* this machine: SES_AWAY_* (ses_set_away) */
    uint32_t away_all;             /* (a machine) every machine's, as the host last said: 2 bits each */
    SesStats st;
};

/* ---- bytes ---- */
static uint8_t *p8(uint8_t *p, uint32_t v) { *p++ = (uint8_t)v; return p; }
static uint8_t *p16(uint8_t *p, uint32_t v) { p = p8(p, v); return p8(p, v >> 8); }
static uint8_t *p32(uint8_t *p, uint32_t v) { p = p16(p, v); return p16(p, v >> 16); }
static uint32_t g8(const uint8_t **p) { return *(*p)++; }
static uint32_t g16(const uint8_t **p) { uint32_t v = g8(p); return v | g8(p) << 8; }
static uint32_t g32(const uint8_t **p) { uint32_t v = g16(p); return v | g16(p) << 16; }

static uint8_t *head(const Session *s, uint8_t *p, int type, int from) /* (the session's own magic: a packet of another match fails it) */
{
    p = p16(p, s->magic);
    p = p8(p, (uint32_t)type);
    return p8(p, (uint32_t)from);
}

/* ---- the reliable channel ---- */
static void rel_send(Session *s, int to, const uint8_t *msg, int len)
{
    Peer *p = &s->peer[to];
    if (p->out_n == p->out_cap) {
        int cap = p->out_cap ? p->out_cap * 2 : 16;
        RelOut *o = realloc(p->out, sizeof *o * (size_t)cap);
        if (!o) return;
        p->out = o;
        p->out_cap = cap;
    }
    RelOut *o = &p->out[p->out_n++];
    o->id = p->out_id++;
    o->data = malloc((size_t)len);
    if (!o->data) { p->out_n--; return; }
    memcpy(o->data, msg, (size_t)len);
    o->len = len;
    o->sent_ms = 0; /* (sent at the next ses_send) */
    o->tries = 0;
}

static void rel_acked(Peer *p, uint16_t id, uint32_t now_ms)
{
    for (int i = 0; i < p->out_n; i++)
        if (p->out[i].id == id) {
            if (p->out[i].tries == 1) { /* (sent once: its round trip, a sample) */
                uint32_t rtt = now_ms - p->out[i].sent_ms;
                if (rtt < 10000) p->srtt = p->srtt ? (p->srtt * 7 + rtt) / 8 : rtt;
            }
            free(p->out[i].data);
            memmove(&p->out[i], &p->out[i + 1], sizeof *p->out * (size_t)(p->out_n - i - 1)); /* (kept in order: the oldest
                                                                                            * sent first, REL_BURST) */
            p->out_n--;
            return;
        }
}

static void rel_reset(Peer *p)
{
    for (int i = 0; i < p->out_n; i++) free(p->out[i].data);
    for (int i = 0; i < p->hold_n; i++) free(p->hold[i].data);
    p->out_n = p->hold_n = p->ack_n = 0;
    p->out_id = p->in_id = 0;
    p->srtt = 0;
}

static void handle_rel(Session *s, int from, const uint8_t *m, int len, uint32_t now_ms);

static void rel_receive(Session *s, int from, uint16_t id, const uint8_t *m, int len, uint32_t now_ms)
{
    Peer *p = &s->peer[from];
    int16_t ahead = (int16_t)(uint16_t)(id - p->in_id);
    if (ahead > 0) { /* (early: held; acked only once kept: one dropped for want of room comes again) */
        int had = 0;
        for (int i = 0; i < p->hold_n; i++) if (p->hold[i].id == id) had = 1;
        if (!had) {
            if (p->hold_n == HOLD_MAX) return;
            RelIn *h = &p->hold[p->hold_n];
            h->data = malloc((size_t)len);
            if (!h->data) return;
            memcpy(h->data, m, (size_t)len);
            h->len = len;
            h->id = id;
            p->hold_n++;
        }
    }
    if (p->ack_n < ACK_QUEUE) p->acks[p->ack_n++] = id;
    if (ahead != 0) return; /* (had it, or held) */
    handle_rel(s, from, m, len, now_ms);
    p->in_id++;
    for (int again = 1; again;) { /* the held ones now in turn */
        again = 0;
        for (int i = 0; i < p->hold_n; i++)
            if (p->hold[i].id == p->in_id) {
                RelIn h = p->hold[i];
                p->hold[i] = p->hold[--p->hold_n];
                handle_rel(s, from, h.data, h.len, now_ms);
                free(h.data);
                p->in_id++;
                again = 1;
                break;
            }
    }
}

/* ---- messages ---- */
static void send_slot(Session *s, int to, int slot)
{
    int32_t a, b;
    rb_slot_get_interval(s->rb, slot, &a, &b);
    uint8_t m[16], *q = m;
    q = p8(q, R_SLOT); q = p8(q, (uint32_t)slot); q = p32(q, (uint32_t)a); q = p32(q, (uint32_t)b);
    rel_send(s, to, m, (int)(q - m));
}

static void send_node(Session *s, int to, int node)
{
    uint8_t m[8], *q = m;
    int present = node == s->node || s->peer[node].present;
    q = p8(q, R_NODE); q = p8(q, (uint32_t)node); q = p8(q, (uint32_t)present); q = p32(q, s->peer[node].slots);
    rel_send(s, to, m, (int)(q - m));
}

static void broadcast_slot(Session *s, int slot)
{
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != s->node && s->peer[n].present) send_slot(s, n, slot);
}

/* (host) the state of the step after the confirmed one, to one machine: a resync, or a joining machine's start */
static void send_state(Session *s, int to, uint32_t now_ms)
{
    Peer *pt = &s->peer[to];
    int resync = pt->resync_wait;
    /* a machine's start (or a battle's): its base first, so only what differs goes (its own load is most of it): a
     * while at most, then the whole state; a resync goes at once, whole (its base is a battle's start, gone by now) */
    if (!resync && s->base_wait_ms && !pt->base_new && !pt->want_full) {
        if (!pt->due_ms) pt->due_ms = now_ms ? now_ms : 1;
        if (now_ms - pt->due_ms < s->base_wait_ms) { pt->state_due = 1; return; }
    }
    if (!s->one_step) rb_catch_up(s->rb); /* (a game rolls back only between two steps of its own: ses_step) */
    int32_t step = rb_confirmed(s->rb) + 1;
    int use_base = !resync && pt->base_new && !pt->want_full;
    s->cur_base = use_base ? pt->base : NULL;
    s->cur_base_len = use_base ? pt->base_len : 0;
    int len = s->game.serialize(s->game.rb.ctx, step, s->tx, s->game.state_max);
    s->cur_base = NULL;
    s->cur_base_len = 0;
    pt->state_due = len <= 0; /* (not now: again at the next send) */
    if (len <= 0) return;
    if (!resync) pt->base_new = pt->want_full = 0, pt->due_ms = 0; /* (a base: once) */
    if (s->peer[to].resync_wait) s->peer[to].resync_id = (uint16_t)(s->peer[to].out_id + (len + CHUNK - 1) / CHUNK - 1);
    for (int off = 0; off < len || off == 0; off += CHUNK) {
        int n = len - off < CHUNK ? len - off : CHUNK;
        uint8_t m[CHUNK + 16], *q = m;
        q = p8(q, R_STATE); q = p32(q, (uint32_t)step); q = p32(q, (uint32_t)len); q = p32(q, (uint32_t)off);
        memcpy(q, s->tx + off, (size_t)n);
        rel_send(s, to, m, (int)(q - m) + n);
        if (n == 0) break;
    }
    s->peer[to].resync_ms = now_ms;
    s->peer[to].hash_from = step + 1;
    s->peer[to].state_sent = step;
}

static void node_left(Session *s, int n, uint32_t now_ms);

/* (host) the kick vote on target: started (one at a time), or the one running; 0: another runs, or no such machine */
static int kick_begin(Session *s, int target, uint32_t now_ms)
{
    if (target < 0 || target >= SES_MAX_NODES || target == s->node || !s->peer[target].present) return 0;
    if (s->kick.target == target) return 1;
    if (s->kick.target >= 0) return 0;
    s->kick.target = target;
    s->kick.yes = 0;
    s->kick.sig = ~0u;
    s->kick.started_ms = now_ms;
    return 1;
}

static void handle_rel(Session *s, int from, const uint8_t *m, int len, uint32_t now_ms)
{
    const uint8_t *q = m;
    int kind = (int)g8(&q);
    if (kind == R_SLOT && len >= 10) {
        int slot = (int)g8(&q);
        int32_t a = (int32_t)g32(&q), b = (int32_t)g32(&q);
        rb_slot_interval(s->rb, slot, a, b);
    } else if (kind == R_NODE && len >= 7) {
        int n = (int)g8(&q), present = (int)g8(&q);
        uint32_t slots = g32(&q);
        if (n < 0 || n >= SES_MAX_NODES) return;
        if (n == s->node) { s->peer[n].slots = slots; rb_set_local(s->rb, slots); return; }
        s->peer[n].present = present;
        s->peer[n].slots = slots;
        s->peer[n].heard_ms = now_ms;
    } else if (kind == R_STATE && len >= 13) {
        int32_t step = (int32_t)g32(&q);
        uint32_t total = g32(&q), off = g32(&q), n = (uint32_t)len - 13;
        if (total > (uint32_t)s->game.state_max || off + n > total) return;
        if (step != s->rx_step || off == 0) { s->rx_step = step; s->rx_total = total; s->rx_got = 0; }
        memcpy(s->rx + off, q, n);
        s->rx_got += n;
        if (s->rx_got >= s->rx_total) { /* all here: the host's state, from its step on */
            s->game.deserialize(s->game.rb.ctx, s->rx, (int)s->rx_total);
            rb_restart_at(s->rb, step);
            s->next_hash = (step + SES_HASH_EVERY - 1) / SES_HASH_EVERY * SES_HASH_EVERY;
            s->state_based = step;
            if (s->ready) s->st.resyncs_done++;
            s->ready = 1;
            s->rx_step = -1;
        }
    } else if (kind == R_BASE && len >= 9 && from >= 0 && from < SES_MAX_NODES) { /* (host) a machine's base, in pieces */
        Peer *pb = &s->peer[from];
        uint32_t total = g32(&q), off = g32(&q), n = (uint32_t)len - 9;
        if (total == 0) { pb->want_full = 1; pb->base_new = 0; return; } /* (none: the whole state, please) */
        if (total > BASE_MAX || off + n > total) return;
        if (!pb->base && !(pb->base = malloc(BASE_MAX))) return;
        if (off == 0) pb->base_got = 0, pb->base_new = 0;
        memcpy(pb->base + off, q, n);
        pb->base_got += (int)n;
        if (pb->base_got >= (int)total) { pb->base_len = (int)total; pb->base_new = 1; pb->want_full = 0; }
    } else if (kind == R_CHAT && len >= 3) {
        int speaker = (int)g8(&q), n = (int)g8(&q);
        if (n > len - 3 || n > SES_CHAT_MAX) return;
        if (s->host == s->node) { /* (host) from its machine, to everyone */
            speaker = from;
            uint8_t out[SES_CHAT_MAX + 4], *w = out;
            w = p8(w, R_CHAT); w = p8(w, (uint32_t)speaker); w = p8(w, (uint32_t)n);
            memcpy(w, q, (size_t)n);
            for (int o = 0; o < SES_MAX_NODES; o++)
                if (o != s->node && o != from && s->peer[o].present) rel_send(s, o, out, 3 + n);
        }
        char text[SES_CHAT_MAX + 1];
        memcpy(text, q, (size_t)n);
        text[n] = 0;
        if (s->ev.chat) s->ev.chat(s->ev.ctx, speaker, text);
    } else if (kind == R_VOTE_START && len >= 5) { /* the host's vote: show it */
        s->vote.round = (int)g16(&q);
        s->vote.n = (int)g8(&q);
        if (s->vote.n > SES_VOTE_MAX || len < 5 + s->vote.n) return;
        for (int i = 0; i < s->vote.n; i++) s->vote.options[i] = (uint8_t)g8(&q);
        int seconds = (int)g8(&q);
        s->vote.result = -1;
        uint8_t zero[SES_VOTE_MAX] = {0};
        if (s->ev.vote) s->ev.vote(s->ev.ctx, s->vote.round, s->vote.n, s->vote.options, zero, -1, seconds);
    } else if (kind == R_VOTE && len >= 4 && s->host == s->node) {
        int round = (int)g16(&q), choice = (int)g8(&q);
        if (round == s->vote.round && s->vote.result < 0 && choice < s->vote.n) s->vote.choice[from] = (int8_t)choice;
    } else if (kind == R_VOTE_TALLY && len >= 5) {
        int round = (int)g16(&q), n = (int)g8(&q);
        uint8_t counts[SES_VOTE_MAX] = {0};
        if (n > SES_VOTE_MAX || len < 6 + n) return;
        for (int i = 0; i < n; i++) counts[i] = (uint8_t)g8(&q);
        int result = (int8_t)g8(&q), left = (int)g8(&q);
        if (round == s->vote.round) s->vote.result = result;
        if (s->ev.vote) s->ev.vote(s->ev.ctx, round, n, s->vote.options, counts, result, left);
    } else if (kind == R_KICK_VOTE && len >= 2 && s->host == s->node) {
        int target = (int)g8(&q);
        if (kick_begin(s, target, now_ms)) s->kick.yes |= 1u << from;
    } else if (kind == R_KICK_STATE && len >= 5) {
        int target = (int)g8(&q), yes = (int)g8(&q), needed = (int)g8(&q), state = (int)g8(&q);
        if (s->ev.kick) s->ev.kick(s->ev.ctx, target, yes, needed, state);
    } else if (kind == R_APP && len >= 5) { /* a piece of the game's own message (lobby.c): whole -> the callback */
        int total = (int)g16(&q), off = (int)g16(&q), n = len - 5;
        Peer *p = &s->peer[from];
        if (total > SES_APP_MAX || off + n > total) return;
        if (!p->app && !(p->app = malloc(SES_APP_MAX))) return;
        if (off == 0) p->app_got = 0;
        if (off != p->app_got) return; /* (in order: a piece missing means a message lost: never on the reliable channel) */
        memcpy(p->app + off, q, (size_t)n);
        p->app_got += n;
        if (p->app_got >= total) {
            p->app_got = 0;
            if (s->ev.app) s->ev.app(s->ev.ctx, from, p->app, total);
        }
    } else if (kind == R_KICKED) {
        s->kicked = 1;
        s->ready = 0;
        if (s->ev.kicked) s->ev.kicked(s->ev.ctx);
    } else if (kind == R_JOIN && s->host == s->node) { /* (host) a machine joins: a free slot from the next steps on */
        if ((s->banned >> from) & 1) return;
        uint32_t free_slots = s->offer;
        for (int n = 0; n < SES_MAX_NODES; n++) if (s->peer[n].present) free_slots &= ~s->peer[n].slots;
        free_slots &= ~s->peer[s->node].slots;
        uint32_t asked = len >= 5 ? g32(&q) : 0; /* (a machine coming back: its own slots, all of them) */
        int watch = (asked & SES_WATCH) != 0; /* (a machine watching the match: no slot, every input and the state) */
        if (watch) asked = 0;
        if (!watch && asked && (asked & free_slots) != asked) return;
        if (!watch && !free_slots) return;
        int slot = 0;
        while (!watch && !((free_slots >> slot) & 1)) slot++;
        uint32_t given = watch ? 0 : asked ? asked : 1u << slot;
        int32_t from_step = rb_now(s->rb) + s->delay + 1; /* past every input any machine knows */
        for (int i = 0; i < RB_MAX_SLOTS; i++) {
            int32_t t = rb_known_through(s->rb, i), a, b;
            rb_slot_get_interval(s->rb, i, &a, &b);
            /* (lockstep: a slot never used counts as known half the ring ahead; the join from now on, everyone waiting
             * while its state goes; rollback keeps that much room for the state to arrive while the others play on) */
            if (a == RB_NEVER && s->lockstep) continue;
            if (t + 1 > from_step && t < b) from_step = t + 1;
        }
        from_step += 2;
        for (int i = 0; i < RB_MAX_SLOTS; i++) {
            if (!((given >> i) & 1)) continue;
            rb_slot_interval(s->rb, i, from_step, RB_NEVER);
            for (int o = 0; o < SES_MAX_NODES; o++) /* (what each said it knew of it: neutral far ahead while it was
                                                      * out; its inputs again from its step) */
                if (s->peer[o].known[i] >= from_step) s->peer[o].known[i] = from_step - 1;
        }
        Peer *p = &s->peer[from];
        p->present = 1;
        p->slots = given;
        p->heard_ms = (asked || watch) && s->lockstep ? 0 : now_ms; /* (a machine coming back loads the match: timed once heard) */
        p->played = 0; /* (in once its own steps run: SES_LOAD_MS) */
        p->first_now = 0;
        p->load_ms = 0;
        for (int i = 0; i < RB_MAX_SLOTS; i++) p->known[i] = -1;
        for (int n = 0; n < SES_MAX_NODES; n++) /* everyone: the new machine and its slots */
            if (n != s->node && s->peer[n].present && n != from) {
                send_node(s, n, from);
                for (int i = 0; i < RB_MAX_SLOTS; i++) if ((given >> i) & 1) send_slot(s, n, i);
            }
        for (int n = 0; n < SES_MAX_NODES; n++) /* the new machine: who is there, every slot, the state */
            if (s->peer[n].present || n == s->node) send_node(s, from, n);
        for (int i = 0; i < RB_MAX_SLOTS; i++) send_slot(s, from, i);
        send_state(s, from, now_ms);
        s->st.joins++;
    }
    (void)node_left;
}

/* ---- the machine's start ---- */
static Session *alloc_session(int node, int host, const NetTransport *net, const NetGame *game, int delay)
{
    Session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->node = node;
    s->host = host;
    s->delay = delay;
    s->magic = MAGIC;
    s->net = *net;
    s->game = *game;
    s->rx_step = -1;
    s->state_based = -1;
    for (int n = 0; n < SES_MAX_NODES; n++) s->peer[n].state_sent = -1;
    s->rx = malloc((size_t)game->state_max);
    s->tx = malloc((size_t)game->state_max);
    s->rb = rb_new(&game->rb, 0, delay, SES_HASH_EVERY);
    if (!s->rx || !s->tx || !s->rb) { ses_free(s); return NULL; }
    for (int n = 0; n < SES_MAX_NODES; n++)
        for (int i = 0; i < RB_MAX_SLOTS; i++) s->peer[n].known[i] = -1;
    s->kick.target = -1;
    memset(s->vote.choice, -1, sizeof s->vote.choice);
    return s;
}

Session *ses_new(int node, int host, uint32_t nodes, const uint32_t slots[SES_MAX_NODES], const NetTransport *net,
                 const NetGame *game, int delay)
{
    Session *s = alloc_session(node, host, net, game, delay);
    if (!s) return NULL;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        s->peer[n].present = (int)((nodes >> n) & 1) && n != node;
        s->peer[n].slots = slots[n];
        if (!((nodes >> n) & 1)) continue;
        for (int i = 0; i < RB_MAX_SLOTS; i++)
            if ((slots[n] >> i) & 1) rb_slot_interval(s->rb, i, 0, RB_NEVER);
    }
    rb_set_local(s->rb, slots[node]);
    s->ready = 1;
    return s;
}

Session *ses_join(int node, int host, const NetTransport *net, const NetGame *game, int delay, uint32_t now_ms)
{
    return ses_join_slots(node, host, net, game, delay, now_ms, 0);
}
Session *ses_join_slots(int node, int host, const NetTransport *net, const NetGame *game, int delay, uint32_t now_ms,
                        uint32_t slots)
{
    Session *s = alloc_session(node, host, net, game, delay);
    if (!s) return NULL;
    s->peer[host].present = 1;
    s->peer[host].heard_ms = slots ? 0 : now_ms; /* (coming back into a match: loading it; the host timed only once heard) */
    if (!slots) ses_ask_join(s, 0); /* (a machine coming back asks once it has loaded the match: ses_ask_join) */
    return s;
}
void ses_ask_join(Session *s, uint32_t slots)
{
    uint8_t m[5] = {R_JOIN}, *q = m + 1;
    if (slots) q = p32(q, slots);
    rel_send(s, s->host, m, (int)(q - m));
}

/* This machine leaves: the others told at once (three copies, no answer wanted). Without it a machine leaving (Escape,
 * the window closed, Quit) held every other machine for the timeout's 5 s: each leave and rejoin froze the match
 * (Phoenix's report 2026-11-22). */
void ses_bye(Session *s)
{
    if (!s) return;
    uint8_t pkt[8], *q = head(s, pkt, P_BYE, s->node);
    for (int k = 0; k < 3; k++)
        for (int n = 0; n < SES_MAX_NODES; n++)
            if (n != s->node && s->peer[n].present) s->net.send(s->net.ctx, n, pkt, (int)(q - pkt));
}

void ses_free(Session *s)
{
    if (!s) return;
    for (int n = 0; n < SES_MAX_NODES; n++) { rel_reset(&s->peer[n]); free(s->peer[n].out); free(s->peer[n].app); free(s->peer[n].base); }
    rb_free(s->rb);
    free(s->rx);
    free(s->tx);
    free(s);
}

/* ---- leaving and the host moving ---- */
static void end_slots(Session *s, uint32_t slots) /* (host) the slots of a machine that left: neutral after its last input */
{
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        if (!((slots >> i) & 1)) continue;
        int32_t a, b, end = rb_known_through(s->rb, i) + 1;
        rb_slot_get_interval(s->rb, i, &a, &b);
        if (end < a) end = a;
        if (end >= b) continue;
        rb_slot_interval(s->rb, i, a, end);
        broadcast_slot(s, i);
    }
}

static void node_left(Session *s, int n, uint32_t now_ms)
{
    Peer *p = &s->peer[n];
    p->present = 0;
    rel_reset(p);
    if (s->host == s->node) {
        end_slots(s, p->slots);
        for (int o = 0; o < SES_MAX_NODES; o++) if (o != s->node && s->peer[o].present) send_node(s, o, n);
        return;
    }
    if (n != s->host) return;
    /* the host left: the machine there longest (the lowest number) carries on as the host */
    int next = s->node;
    for (int o = 0; o < SES_MAX_NODES; o++)
        if (o != n && (o == s->node || s->peer[o].present)) { next = o; break; }
    s->host = next;
    s->st.migrations++;
    if (next == s->node) {
        s->departed |= p->slots;
        s->migrating = 1;
        s->migrate_ms = now_ms;
        for (int o = 0; o < SES_MAX_NODES; o++) {
            if (o == s->node || !s->peer[o].present) continue;
            s->peer[o].heard_ms = now_ms;
            for (int i = 0; i < RB_MAX_SLOTS; i++) s->peer[o].known[i] = rb_now(s->rb) - RB_WINDOW * 2;
        }
    } else {
        Peer *h = &s->peer[next];
        rel_reset(h);
        h->heard_ms = now_ms;
        s->send_all_until = now_ms + 2 * SES_MIGRATE_MS;
        for (int i = 0; i < RB_MAX_SLOTS; i++) h->known[i] = rb_now(s->rb) - RB_WINDOW * 2;
    }
}

/* ---- receiving ---- */
void ses_receive(Session *s, int from, const void *data, int len, uint32_t now_ms)
{
    const uint8_t *q = data, *end = q + len;
    if (len < 4 || from < 0 || from >= SES_MAX_NODES || from == s->node) return;
    if (g16(&q) != s->magic) return;
    int type = (int)g8(&q);
    if ((int)g8(&q) != from) return;
    Peer *p = &s->peer[from];
    int is_host = s->host == s->node;
    if (!p->present && !(type == P_REL && is_host)) return; /* (a machine gone, or not in yet) */
    p->heard_ms = now_ms;
    if (type == P_BYE) { /* it leaves now: gone at once, not after SES_TIMEOUT_MS of quiet with every machine held */
        if (is_host || from == s->host) node_left(s, from, now_ms);
        return;
    }
    if (type == P_INPUT && end - q >= 4 + 10 + 4 + 4 * RB_MAX_SLOTS + 2) {
        int32_t its_now = (int32_t)g32(&q);
        uint32_t its_ts = g32(&q), echo = g32(&q), held = g16(&q), away = g32(&q);
        if (is_host) p->away = (int)(away >> (2 * from)) & 3; /* (a machine: its own) */
        else if (from == s->host) s->away_all = away;         /* (the host: everyone's) */
        p->echo_ts = its_ts;
        p->echo_at = now_ms;
        if (echo) { /* (our clock back: the round trip, less the time it held it) */
            uint32_t rtt = now_ms - echo - held;
            if (rtt < 10000) p->rtt = p->rtt ? (p->rtt * 7 + rtt) / 8 : (rtt ? rtt : 1);
        }
        int newer = its_now >= p->now; /* (a packet as new as the last: what it knows now; an older one's: no lower) */
        p->now = newer ? its_now : p->now;
        p->now_ms = now_ms;
        if (!p->played) { /* (in once its own steps run: its step 2 past the first it said (a joiner's: the host's state's),
                           * not the state's step alone (a game stopped after its load still takes the state)) */
            if (p->now > 0 && !p->first_now) p->first_now = p->now;
            else if (p->first_now && p->now >= p->first_now + 2) p->played = 1;
        }
        for (int i = 0; i < RB_MAX_SLOTS; i++) {
            int32_t k = (int32_t)g32(&q);
            if (newer || k > p->known[i]) p->known[i] = k; /* (the latest: it falls when a slot comes back in) */
        }
        int na = (int)g8(&q);
        for (int i = 0; i < na && end - q >= 2; i++) rel_acked(p, (uint16_t)g16(&q), now_ms);
        int runs = end - q >= 1 ? (int)g8(&q) : 0;
        for (int r = 0; r < runs && end - q >= 6; r++) {
            int slot = (int)g8(&q);
            int32_t start = (int32_t)g32(&q);
            int count = (int)g8(&q);
            for (int k = 0; k < count && end - q >= (int)sizeof(RbInput); k++) {
                RbInput in;
                memcpy(&in, q, sizeof in);
                q += sizeof in;
                rb_remote_input(s->rb, slot, start + k, in);
            }
        }
    } else if (type == P_REL && end - q >= 4) {
        uint16_t id = (uint16_t)g16(&q);
        int n = (int)g16(&q);
        if (n > end - q) return;
        if (!p->present) { /* (host) only a join from a machine not in yet */
            if (n < 1 || q[0] != R_JOIN || id != 0) return;
            rel_reset(p);
        }
        rel_receive(s, from, id, q, n, now_ms);
    } else if (type == P_VOICE && end - q >= 5) {
        const uint8_t *frame = q;
        int speaker = (int)g8(&q);
        uint16_t seq = (uint16_t)g16(&q);
        int n = (int)g16(&q);
        if (n > end - q || speaker < 0 || speaker >= SES_MAX_NODES || speaker == s->node) return;
        if (is_host) { /* a speaker more only while fewer than 8 talk */
            if (speaker != from) return;
            if ((!p->voice_ms || now_ms - p->voice_ms > 300)) {
                int talking = s->voice_ms && now_ms - s->voice_ms <= 300;
                for (int o = 0; o < SES_MAX_NODES; o++)
                    if (o != s->node && s->peer[o].present && s->peer[o].voice_ms && now_ms - s->peer[o].voice_ms <= 300) talking++;
                if (talking >= SES_VOICE_TALKERS) return;
            }
            p->voice_ms = now_ms ? now_ms : 1;
            uint8_t pkt[PACKET_MAX], *w = head(s, pkt, P_VOICE, s->node);
            if (5 + n > PACKET_MAX - 4) return;
            memcpy(w, frame, (size_t)(5 + n));
            for (int o = 0; o < SES_MAX_NODES; o++) /* to everyone else, as it came */
                if (o != s->node && o != from && s->peer[o].present) s->net.send(s->net.ctx, o, pkt, (int)(w - pkt) + 5 + n);
        }
        if (s->voice) s->voice(s->voice_ctx, speaker, seq, q, n);
    } else if (type == P_HASH && end - q >= 12 && is_host) {
        int32_t step = (int32_t)g32(&q);
        uint32_t sum = g32(&q);
        int32_t based = (int32_t)g32(&q);
        if (p->state_sent >= 0 && based != p->state_sent) return; /* (made before the last state it was sent arrived) */
        if (s->pend_n < PEND_MAX) { s->pend[s->pend_n].node = from; s->pend[s->pend_n].step = step; s->pend[s->pend_n].sum = sum; s->pend_n++; }
    }
}

/* ---- each tick ---- */
static int32_t host_now_estimate(const Session *s, uint32_t now_ms)
{
    const Peer *h = &s->peer[s->host];
    return h->now + (int32_t)((now_ms - h->now_ms) / SES_STEP_MS);
}

/* (host) a resync's state still on its way to a machine. Held meanwhile: the state is of the step after the confirmed
 * one when it went, and over the internet its 3-4 MB take seconds; played on, the machine went back that far when it
 * came, and the inputs since were never sent again (its reports from an earlier step count as old ones) or already
 * out of the ring: every machine waiting, for good (the user's online playtest 2026-11-01). Held, it goes back a few
 * steps at most, the inputs since still in its ring. (A machine joining or coming back: the others play on, as before.) */
static int resync_held(Session *s)
{
    int held = 0;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        Peer *p = &s->peer[n];
        if (n == s->node || !p->resync_wait || p->state_due) continue; /* (not made yet: held once it goes, of its step then) */
        int out = 0;
        for (int i = 0; p->present && i < p->out_n && !out; i++) out = (int16_t)(p->out[i].id - p->resync_id) <= 0;
        if (out) held = 1;
        else p->resync_wait = 0;
    }
    return held;
}

int ses_step(Session *s, const RbInput local[RB_MAX_SLOTS], uint32_t now_ms)
{
    /* who has gone quiet */
    for (int n = 0; n < SES_MAX_NODES; n++) {
        Peer *p = &s->peer[n];
        if (n == s->node || !p->present) continue;
        if (s->host == s->node && (s->lockstep || s->one_step) && p->slots) { /* (host) a player's machine that never comes
                * in: not heard, or heard (loading and between screens it says it is there) but never a step run. Its game
                * stopped while loading (Skull Servant's v1.0.12 log: every machine held at step 5, one player's inputs
                * never came, its packets did; no end): gone after a while, as one that left (its car a computer car) */
            if (!p->played) {
                if (!p->load_ms) p->load_ms = now_ms ? now_ms : 1;
                else if (now_ms - p->load_ms > SES_LOAD_MS) {
                    fprintf(stderr, "session: machine %d never came into the match (%u s): gone\n", n, SES_LOAD_MS / 1000u);
                    p->load_ms = 0;
                    node_left(s, n, now_ms);
                    continue;
                }
            } else p->load_ms = 0;
        }
        if ((s->lockstep || s->one_step) && !p->heard_ms) continue; /* (a game's match: a machine not heard yet is still loading
                                                                      * the match) */
        if (s->host == s->node || n == s->host) /* (waiting for the host's state: its sending, longer) */
            if (now_ms - p->heard_ms > ((s->lockstep || s->one_step) && !s->ready && n == s->host ? SES_AWAIT_MS : SES_TIMEOUT_MS))
                node_left(s, n, now_ms);
    }
    if (s->migrating && now_ms - s->migrate_ms >= SES_MIGRATE_MS) { /* (new host) the inputs gathered: end the slots */
        s->migrating = 0;
        end_slots(s, s->departed);
        s->departed = 0;
    }
    if (s->host == s->node) {
        for (int n = 0; n < SES_MAX_NODES; n++) /* a kicked machine: gone once told (a second later) */
            if (s->peer[n].present && s->peer[n].kick_ms && now_ms - s->peer[n].kick_ms > 1000) node_left(s, n, now_ms);
        if (s->vote.n && s->vote.result < 0) { /* the vote: everyone voted, or the time is up */
            uint8_t counts[SES_VOTE_MAX] = {0};
            int voters = 0, voted = 0;
            for (int n = 0; n < SES_MAX_NODES; n++) {
                if (n != s->node && !s->peer[n].present) continue;
                voters++;
                if (s->vote.choice[n] >= 0) { voted++; counts[s->vote.choice[n]]++; }
            }
            int done = voted == voters || (int32_t)(now_ms - s->vote.until_ms) >= 0, best = 0;
            for (int i = 1; i < s->vote.n; i++) if (counts[i] > counts[best]) best = i; /* (a tie: the first offered) */
            int left = done ? 0 : (int)((s->vote.until_ms - now_ms + 999) / 1000);
            uint32_t sig = (uint32_t)counts[0] | (uint32_t)counts[1] << 8 | (uint32_t)counts[2] << 16 | (uint32_t)left << 24;
            if (done || sig != s->vote.sig) { /* (the tally to everyone when it changes) */
                s->vote.sig = sig;
                if (done) s->vote.result = best;
                uint8_t m[16], *w = m;
                w = p8(w, R_VOTE_TALLY); w = p16(w, (uint32_t)s->vote.round); w = p8(w, (uint32_t)s->vote.n);
                for (int i = 0; i < s->vote.n; i++) w = p8(w, counts[i]);
                w = p8(w, (uint32_t)(done ? best : -1)); w = p8(w, (uint32_t)left);
                for (int n = 0; n < SES_MAX_NODES; n++) if (n != s->node && s->peer[n].present) rel_send(s, n, m, (int)(w - m));
                if (s->ev.vote) s->ev.vote(s->ev.ctx, s->vote.round, s->vote.n, s->vote.options, counts, done ? best : -1, left);
            }
        }
        if (s->kick.target >= 0) { /* the kick vote: more than half of the others (the target not counted) */
            int others = 0, yes = 0;
            for (int n = 0; n < SES_MAX_NODES; n++) {
                if (n == s->kick.target || (n != s->node && !s->peer[n].present)) continue;
                others++;
                yes += (int)((s->kick.yes >> n) & 1);
            }
            int needed = others / 2 + 1, state = yes >= needed ? SES_KICK_PASSED
                                                : now_ms - s->kick.started_ms > SES_KICK_MS ? SES_KICK_FAILED : SES_KICK_RUNNING;
            uint32_t sig = (uint32_t)yes | (uint32_t)needed << 8 | (uint32_t)state << 16 | (uint32_t)s->kick.target << 24;
            if (sig != s->kick.sig || state != SES_KICK_RUNNING) {
                s->kick.sig = sig;
                uint8_t m[8], *w = m;
                w = p8(w, R_KICK_STATE); w = p8(w, (uint32_t)s->kick.target); w = p8(w, (uint32_t)yes);
                w = p8(w, (uint32_t)needed); w = p8(w, (uint32_t)state);
                for (int n = 0; n < SES_MAX_NODES; n++) if (n != s->node && s->peer[n].present) rel_send(s, n, m, (int)(w - m));
                if (s->ev.kick) s->ev.kick(s->ev.ctx, s->kick.target, yes, needed, state);
            }
            if (state == SES_KICK_PASSED) ses_kick(s, s->kick.target, now_ms);
            if (state != SES_KICK_RUNNING) s->kick.target = -1;
        }
    }
    if (!s->ready || s->idle) return 0;
    if (s->host == s->node && resync_held(s)) return 0;
    int steps = 1;
    if (!s->lockstep && s->host != s->node && s->peer[s->host].now_ms) { /* keep with the host's step */
        int32_t ahead = rb_now(s->rb) - host_now_estimate(s, now_ms);
        steps = ahead > 1 ? 0 : ahead < -RB_WINDOW ? 3 : ahead < -1 ? 2 : 1;
        s->behind = s->one_step && steps > 1;
        if (s->one_step && steps > 1) steps = 1; /* (a game: one step an iteration of its loop; it catches up itself) */
    }
    int done = 0;
    for (int k = 0; k < steps; k++) {
        for (int i = 0; i < RB_MAX_SLOTS; i++)
            if ((ses_local_slots(s) >> i) & 1) rb_local_input(s->rb, i, local[i]);
        if (!rb_step(s->rb)) break;
        done++;
    }
    if (!steps) rb_catch_up(s->rb);
    return done;
}

static uint8_t *put_runs(Session *s, uint8_t *q, uint8_t *limit, const Peer *p, uint32_t slots)
{
    uint8_t *count_at = q++;
    int runs = 0;
    for (int i = 0; i < RB_MAX_SLOTS && runs < 255; i++) {
        if (!((slots >> i) & 1)) continue;
        /* (as far back as the ring holds: a machine coming back starts from a state some seconds old, the others played
         * on meanwhile up to its slot's start, and it needs every input since) */
        int32_t a, b, from = p->known[i] + 1, lo = rb_now(s->rb) - RB_RING + 2;
        rb_slot_get_interval(s->rb, i, &a, &b);
        if (a == RB_NEVER || from >= b) continue; /* (never active, or over) */
        if (from < a + s->delay) from = a + s->delay;
        if (from < lo) from = lo;
        RbInput in[RUN_MAX];
        int n = 0;
        while (n < RUN_MAX && from + n < b && rb_input(s->rb, i, from + n, &in[n])) n++;
        if (!n || q + 6 + n * (int)sizeof(RbInput) > limit) continue;
        q = p8(q, (uint32_t)i); q = p32(q, (uint32_t)from); q = p8(q, (uint32_t)n);
        memcpy(q, in, sizeof(RbInput) * (size_t)n);
        q += sizeof(RbInput) * (size_t)n;
        runs++;
    }
    *count_at = (uint8_t)runs;
    return q;
}

static void send_to(Session *s, int to, uint32_t now_ms)
{
    Peer *p = &s->peer[to];
    uint8_t pkt[PACKET_MAX], *q = head(s, pkt, P_INPUT, s->node);
    q = p32(q, (uint32_t)rb_now(s->rb));
    q = p32(q, now_ms ? now_ms : 1); /* (the ping: our clock, its clock back and how long we held it) */
    q = p32(q, p->echo_ts);
    q = p16(q, p->echo_ts ? (now_ms - p->echo_at > 0xFFFF ? 0xFFFF : now_ms - p->echo_at) : 0);
    uint32_t away = (uint32_t)(s->away_self & 3) << (2 * s->node); /* (what each machine is doing: the host tells all) */
    if (s->host == s->node)
        for (int n = 0; n < SES_MAX_NODES; n++) if (n != s->node && s->peer[n].present) away |= (uint32_t)(s->peer[n].away & 3) << (2 * n);
    q = p32(q, away);
    for (int i = 0; i < RB_MAX_SLOTS; i++) q = p32(q, (uint32_t)rb_known_through(s->rb, i));
    int na = p->ack_n > ACKS_MAX ? ACKS_MAX : p->ack_n;
    q = p8(q, (uint32_t)na);
    for (int i = 0; i < na; i++) q = p16(q, p->acks[i]);
    p->ack_n -= na; /* (the rest in the next packet) */
    memmove(p->acks, p->acks + na, sizeof *p->acks * (size_t)p->ack_n);
    /* the inputs it lacks: the host sends every slot but its own; a machine its own slots (to a new host: all) */
    uint32_t slots = s->host == s->node ? ~p->slots : ses_local_slots(s);
    if (s->host != s->node && (int32_t)(s->send_all_until - now_ms) > 0) slots = ~0u;
    q = put_runs(s, q, pkt + sizeof pkt, p, slots);
    s->net.send(s->net.ctx, to, pkt, (int)(q - pkt));
    if (p->rate) { /* paced: its share of the time since last (at most a tenth of a second's) */
        uint32_t add = (uint32_t)((uint64_t)p->rate * (now_ms - p->budget_ms) / 1000);
        p->budget = p->budget + add > p->rate / 10 ? p->rate / 10 : p->budget + add;
        p->budget_ms = now_ms;
    }
    /* the reliable messages not acked: again after the round trip and a half (RTO_MS before one is known; a slow
     * connection's acks take longer: sooner only floods it with copies), within REL_WINDOW of the oldest */
    uint32_t rto = p->srtt ? p->srtt + p->srtt / 2 + 30 : RTO_MS;
    rto = rto < RTO_MS ? RTO_MS : rto > RTO_MAX ? RTO_MAX : rto;
    for (int i = 0, burst = 0; i < p->out_n && burst < REL_BURST; i++) {
        RelOut *o = &p->out[i];
        if ((uint16_t)(o->id - p->out[0].id) >= REL_WINDOW) break;
        if (o->sent_ms && now_ms - o->sent_ms < (o->tries > 1 ? rto * 2 : rto)) continue; /* (again: longer) */
        if (p->rate && p->budget < (uint32_t)o->len + 16) break; /* (paced: the rest at the next send) */
        if (p->rate) p->budget -= (uint32_t)o->len + 16;
        burst++;
        uint8_t m[PACKET_MAX + CHUNK], *w = head(s, m, P_REL, s->node);
        w = p16(w, o->id); w = p16(w, (uint32_t)o->len);
        memcpy(w, o->data, (size_t)o->len);
        s->net.send(s->net.ctx, to, m, (int)(w - m) + o->len);
        o->sent_ms = now_ms ? now_ms : 1;
        if (o->tries < 255) o->tries++;
    }
}

void ses_send(Session *s, uint32_t now_ms)
{
    if (s->host == s->node) {
        for (int n = 0; n < SES_MAX_NODES; n++) {
            if (n != s->node && s->peer[n].present && s->peer[n].state_due) send_state(s, n, now_ms); /* (held: now?) */
            if (n != s->node && s->peer[n].present) send_to(s, n, now_ms);
        }
        /* the hashes the machines sent, against the host's own: a difference -> the host's state to that machine */
        for (int k = 0; k < s->pend_n;) {
            uint32_t mine;
            int n = s->pend[k].node;
            int32_t step = s->pend[k].step;
            if (step < s->peer[n].hash_from) { /* (made before the state it was sent: not compared) */
                s->pend[k] = s->pend[--s->pend_n];
                continue;
            }
            if (rb_checksum(s->rb, step, &mine)) {
                s->st.hashes_checked++;
                if (mine != s->pend[k].sum) {
                    s->st.desyncs_seen++;
                    if (getenv("TM2PC_NET_DEBUG"))
                        fprintf(stderr, "session: machine %d's hash of step %d %08x, the host's %08x (now %d)\n", n, (int)step,
                                s->pend[k].sum, mine, (int)rb_now(s->rb));
                    if (!s->lockstep && s->peer[n].present && now_ms - s->peer[n].resync_ms > 1000) { /* (lockstep keeps no
                                                                                                         * step states: logged only) */
                        s->peer[n].resync_wait = 1; /* (held from its sending: now, or when the game can make it) */
                        send_state(s, n, now_ms);
                        s->st.resyncs_sent++;
                        void net_desync_dump(int32_t step, int host); /* net/game_tm2.c: tests */
                        net_desync_dump(step, 1);
                        fprintf(stderr, "session: machine %d's step %d differs (%08x, the host's %08x): its state sent again\n", n,
                                (int)step, s->pend[k].sum, mine); /* (the players' logs: when it happened) */

                    }
                }
            } else if (step >= rb_now(s->rb) - RB_RING / 4 && step <= rb_now(s->rb) + RB_RING / 4) {
                k++;
                continue; /* (not final here yet) */
            }
            s->pend[k] = s->pend[--s->pend_n];
        }
    } else if (s->peer[s->host].present && !s->kicked) {
        send_to(s, s->host, now_ms);
        if (s->ready) /* the hashes of the steps final here */
            for (uint32_t sum; rb_checksum(s->rb, s->next_hash, &sum); s->next_hash += SES_HASH_EVERY) {
                uint8_t pkt[24], *q = head(s, pkt, P_HASH, s->node);
                static long bad_at = -2; /* tests: TM2PC_NET_TEST_BAD_HASH=N, this machine's hash of the first step from
                                          * N on reported wrong once (the host's resync, as after a real difference) */
                if (bad_at == -2) bad_at = getenv("TM2PC_NET_TEST_BAD_HASH") ? atol(getenv("TM2PC_NET_TEST_BAD_HASH")) : -1;
                if (bad_at >= 0 && s->next_hash >= bad_at) { sum ^= 1; bad_at = -1; }
                q = p32(q, (uint32_t)s->next_hash); q = p32(q, sum); q = p32(q, (uint32_t)s->state_based);
                s->net.send(s->net.ctx, s->host, pkt, (int)(q - pkt));
            }
    }
}

void ses_on_events(Session *s, const SesEvents *ev) { s->ev = *ev; }
void ses_set_away(Session *s, int away) { s->away_self = away & 3; }
int ses_resync_node(const Session *s)
{
    if (s->host != s->node) return -1;
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != s->node && s->peer[n].present && s->peer[n].resync_wait) return n;
    return -1;
}
int ses_away(const Session *s, int node)
{
    if (node < 0 || node >= SES_MAX_NODES) return 0;
    if (node == s->node) return s->away_self;
    if (s->host == s->node) return s->peer[node].present ? s->peer[node].away : 0;
    return (int)(s->away_all >> (2 * node)) & 3;
}

void ses_chat(Session *s, const char *text)
{
    int n = (int)strlen(text);
    if (n > SES_CHAT_MAX) n = SES_CHAT_MAX;
    uint8_t m[SES_CHAT_MAX + 4], *w = m;
    w = p8(w, R_CHAT); w = p8(w, (uint32_t)s->node); w = p8(w, (uint32_t)n);
    memcpy(w, text, (size_t)n);
    if (s->host == s->node) {
        for (int o = 0; o < SES_MAX_NODES; o++) if (o != s->node && s->peer[o].present) rel_send(s, o, m, 3 + n);
    } else rel_send(s, s->host, m, 3 + n);
}

void ses_vote_start(Session *s, int n, const uint8_t *options, int seconds, uint32_t now_ms)
{
    if (s->host != s->node || n < 1 || n > SES_VOTE_MAX) return;
    s->vote.round++;
    s->vote.n = n;
    s->vote.result = -1;
    s->vote.until_ms = now_ms + (uint32_t)seconds * 1000;
    memcpy(s->vote.options, options, (size_t)n);
    memset(s->vote.choice, -1, sizeof s->vote.choice);
    uint8_t m[16], *w = m;
    w = p8(w, R_VOTE_START); w = p16(w, (uint32_t)s->vote.round); w = p8(w, (uint32_t)n);
    for (int i = 0; i < n; i++) w = p8(w, options[i]);
    w = p8(w, (uint32_t)seconds);
    for (int o = 0; o < SES_MAX_NODES; o++) if (o != s->node && s->peer[o].present) rel_send(s, o, m, (int)(w - m));
}

void ses_vote(Session *s, int round, int choice)
{
    if (s->host == s->node) {
        if (round == s->vote.round && s->vote.result < 0 && choice >= 0 && choice < s->vote.n) s->vote.choice[s->node] = (int8_t)choice;
        return;
    }
    uint8_t m[8], *w = m;
    w = p8(w, R_VOTE); w = p16(w, (uint32_t)round); w = p8(w, (uint32_t)choice);
    rel_send(s, s->host, m, (int)(w - m));
}

void ses_kick_vote(Session *s, int target, uint32_t now_ms)
{
    if (target < 0 || target >= SES_MAX_NODES || target == s->host) return; /* (the host: not by vote) */
    if (s->host != s->node) {
        uint8_t m[4] = {R_KICK_VOTE, (uint8_t)target};
        rel_send(s, s->host, m, 2);
        return;
    }
    if (kick_begin(s, target, now_ms)) s->kick.yes |= 1u << s->node;
}

void ses_kick(Session *s, int target, uint32_t now_ms)
{
    if (s->host != s->node || target == s->node || target < 0 || target >= SES_MAX_NODES || !s->peer[target].present) return;
    if (s->peer[target].kick_ms) return;
    uint8_t m[2] = {R_KICKED};
    rel_send(s, target, m, 1);
    s->peer[target].kick_ms = now_ms ? now_ms : 1;
    s->banned |= 1u << target;
}

int ses_present(const Session *s, int node) { return node == s->node ? !s->kicked : s->peer[node].present; }

void ses_on_voice(Session *s, SesVoiceFn fn, void *ctx)
{
    s->voice = fn;
    s->voice_ctx = ctx;
}

void ses_send_voice(Session *s, uint16_t seq, const uint8_t *data, int len, uint32_t now_ms)
{
    uint8_t pkt[PACKET_MAX], *q = head(s, pkt, P_VOICE, s->node);
    if (len <= 0 || len > PACKET_MAX - 16) return;
    q = p8(q, (uint32_t)s->node); q = p16(q, seq); q = p16(q, (uint32_t)len);
    memcpy(q, data, (size_t)len);
    int n = (int)(q - pkt) + len;
    if (s->host == s->node) { /* (the host: straight to everyone, while fewer than 8 talk) */
        if ((!s->voice_ms || now_ms - s->voice_ms > 300)) {
            int talking = 0;
            for (int o = 0; o < SES_MAX_NODES; o++)
                if (o != s->node && s->peer[o].present && s->peer[o].voice_ms && now_ms - s->peer[o].voice_ms <= 300) talking++;
            if (talking >= SES_VOICE_TALKERS) return;
        }
        s->voice_ms = now_ms ? now_ms : 1;
        for (int o = 0; o < SES_MAX_NODES; o++)
            if (o != s->node && s->peer[o].present) s->net.send(s->net.ctx, o, pkt, n);
    } else if (s->peer[s->host].present) s->net.send(s->net.ctx, s->host, pkt, n);
}

int ses_host(const Session *s) { return s->host; }
int ses_ready(const Session *s) { return s->ready; }
uint32_t ses_local_slots(const Session *s) { return s->peer[s->node].slots; }
Rollback *ses_rollback(Session *s) { return s->rb; }
const SesStats *ses_stats(const Session *s) { return &s->st; }
void ses_host_offer_slots(Session *s, uint32_t free_slots) { s->offer = free_slots; }

void ses_set_one_step(Session *s, int on) { s->one_step = on != 0; }
int ses_behind(const Session *s) { return s->behind && s->host != s->node; }
int ses_state_owed(const Session *s)
{
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != s->node && s->peer[n].present && s->peer[n].state_due) return 1;
    return 0;
}
void ses_set_lockstep(Session *s, int on)
{
    s->lockstep = on != 0;
    rb_set_window(s->rb, on ? 0 : RB_WINDOW);
}

void ses_await_state(Session *s) { s->ready = 0; }
void ses_ready_self(Session *s) { s->ready = 1; }
void ses_set_tag(Session *s, uint16_t tag) { s->magic = (uint16_t)(MAGIC ^ (tag ? tag : 1)); }
void ses_set_rate(Session *s, int node, uint32_t bytes_per_s)
{
    if (node < 0 || node >= SES_MAX_NODES || s->peer[node].rate == bytes_per_s) return;
    s->peer[node].rate = bytes_per_s;
    s->peer[node].budget = 0;
}
uint32_t ses_quiet_ms(const Session *s, int node, uint32_t now_ms)
{
    if (node < 0 || node >= SES_MAX_NODES || node == s->node || !s->peer[node].present || !s->peer[node].heard_ms) return 0;
    return now_ms - s->peer[node].heard_ms;
}
void ses_forgive_quiet(Session *s, uint32_t now_ms)
{
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != s->node && s->peer[n].present && s->peer[n].heard_ms) s->peer[n].heard_ms = now_ms;
}

void ses_set_idle(Session *s, int on) { s->idle = on != 0; }

void ses_app_send(Session *s, int to, const void *data, int len)
{
    if (to < 0 || to >= SES_MAX_NODES || to == s->node || len <= 0 || len > SES_APP_MAX) return;
    if (s->host != s->node && to != s->host) return; /* (a machine talks to the host only) */
    const uint8_t *d = data;
    for (int off = 0; off < len; off += CHUNK) {
        int n = len - off < CHUNK ? len - off : CHUNK;
        uint8_t m[CHUNK + 8], *q = m;
        q = p8(q, R_APP); q = p16(q, (uint32_t)len); q = p16(q, (uint32_t)off);
        memcpy(q, d + off, (size_t)n);
        rel_send(s, to, m, (int)(q - m) + n);
    }
}

int ses_rtt(const Session *s, int node) { return node >= 0 && node < SES_MAX_NODES && s->peer[node].present ? (int)s->peer[node].rtt : 0; }
int ses_rel_pending(const Session *s, int node, int *state_got)
{
    if (state_got) *state_got = s->rx_step >= 0 ? (int)s->rx_got : -1;
    return node >= 0 && node < SES_MAX_NODES ? s->peer[node].out_n : 0;
}

void ses_set_base_wait(Session *s, uint32_t ms) { s->base_wait_ms = ms; }
const uint8_t *ses_state_base(const Session *s, int *len) { if (len) *len = s->cur_base_len; return s->cur_base; }
void ses_send_base(Session *s, const uint8_t *base, int len) /* (a machine) to the host, in pieces (len 0: the whole state) */
{
    if (s->host == s->node || s->host < 0 || len < 0 || len > BASE_MAX) return;
    for (int off = 0; off < len || off == 0; off += CHUNK) {
        int n = len - off < CHUNK ? len - off : CHUNK;
        uint8_t m[CHUNK + 16], *q = m;
        q = p8(q, R_BASE); q = p32(q, (uint32_t)len); q = p32(q, (uint32_t)off);
        if (n > 0) memcpy(q, base + off, (size_t)n);
        rel_send(s, s->host, m, (int)(q - m) + (n > 0 ? n : 0));
        if (n <= 0) break;
    }
}
void ses_push_state(Session *s, int to, uint32_t now_ms)
{
    if (s->host != s->node || to < 0 || to >= SES_MAX_NODES || to == s->node || !s->peer[to].present) return;
    send_state(s, to, now_ms);
}
