/* net_sim: the online code (src/net) proven without the game (docs/ONLINE_PLAN.md 12 step A).
 *
 * Several machines in one program, each with its own session and its own copy of a small deterministic test game
 * (16 cars on a field: steering, shots, a random stream), over a simulated network: delay, jitter and lost packets.
 * Each scenario checks that every machine ends with the same game (their state hashes at the same steps agree), that
 * the game kept moving, and what it was meant to show (a desync caught and repaired, a machine joining, the host
 * leaving). Times are simulated: a 3-minute match runs in a moment.
 *
 *   net_sim [scenario...]     (none: all)   exit code 0: every scenario passed */
#include "../../src/net/session.h"
#include "../../src/net/voice.h"

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>
void net_desync_dump(int32_t step, int host); /* (the game's, net/game_tm2.c: a test dump; none here) */
void net_desync_dump(int32_t step, int host) { (void)step; (void)host; }

/* ---- the test game ---- */
typedef struct {
    int32_t x[RB_MAX_SLOTS], y[RB_MAX_SLOTS], vx[RB_MAX_SLOTS], vy[RB_MAX_SLOTS], hp[RB_MAX_SLOTS], score[RB_MAX_SLOTS];
    uint32_t rng;
    int32_t steps;
} Toy;

typedef struct {
    Toy now;
    Toy kept[RB_RING];
    int32_t kept_step[RB_RING];
} Game;

static uint32_t toy_rand(Toy *t) { t->rng = t->rng * 1103515245u + 12345u; return (t->rng >> 16) & 0x7FFF; }

static void g_advance(void *ctx, const RbInput *in)
{
    Toy *t = &((Game *)ctx)->now;
    for (int i = 0; i < RB_MAX_SLOTS; i++) {
        uint8_t b = in[i].b[0];
        t->vx[i] += (b & 1) - ((b >> 1) & 1);
        t->vy[i] += ((b >> 2) & 1) - ((b >> 3) & 1);
        t->vx[i] = t->vx[i] * 7 / 8;
        t->vy[i] = t->vy[i] * 7 / 8;
        t->x[i] = (t->x[i] + t->vx[i] + 4096) % 4096;
        t->y[i] = (t->y[i] + t->vy[i] + 4096) % 4096;
        if (b & 0x10) { /* a shot at a random car near enough */
            int target = (int)(toy_rand(t) % RB_MAX_SLOTS);
            int32_t dx = t->x[target] - t->x[i], dy = t->y[target] - t->y[i];
            if (target != i && dx * dx + dy * dy < 1500 * 1500) {
                t->hp[target]--;
                t->score[i]++;
                if (t->hp[target] <= 0) { t->hp[target] = 100; t->x[target] = (int32_t)(toy_rand(t) % 4096); }
            }
        }
        t->x[i] += in[i].b[1] - 128; /* (an analog axis) */
    }
    t->steps++;
}

static void g_save(void *ctx, int32_t step)
{
    Game *g = ctx;
    g->kept[step % RB_RING] = g->now;
    g->kept_step[step % RB_RING] = step;
}

static void g_load(void *ctx, int32_t step)
{
    Game *g = ctx;
    if (g->kept_step[step % RB_RING] != step) { fprintf(stderr, "load of step %d not kept\n", step); abort(); }
    g->now = g->kept[step % RB_RING];
}

static uint32_t g_checksum(void *ctx)
{
    const uint8_t *p = (const uint8_t *)&((Game *)ctx)->now;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(Toy); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int g_serialize(void *ctx, int32_t step, uint8_t *out, int cap)
{
    Game *g = ctx;
    if (g->kept_step[step % RB_RING] != step || cap < (int)sizeof(Toy)) return 0;
    memcpy(out, &g->kept[step % RB_RING], sizeof(Toy));
    return (int)sizeof(Toy);
}

static void g_deserialize(void *ctx, const uint8_t *in, int len)
{
    if (len == (int)sizeof(Toy)) memcpy(&((Game *)ctx)->now, in, sizeof(Toy));
}

/* a player: changes what it presses every few steps (so predictions are often wrong) */
static RbInput player_input(int slot, int32_t step)
{
    uint32_t h = (uint32_t)(step / 3) * 2654435761u ^ (uint32_t)slot * 40503u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    RbInput in = {{0}};
    in.b[0] = (uint8_t)(h & 0x1F);
    in.b[1] = (uint8_t)(120 + (h >> 8) % 17);
    return in;
}

/* ---- the network ---- */
enum { MACHINES = SES_MAX_NODES };
typedef struct { uint32_t at; int from, to, len; uint8_t data[2600]; } Packet;

typedef struct {
    int latency, jitter, loss_pct;  /* one way: ms, +- ms, percent lost */
    uint32_t rng;
    Packet *q;
    int n, cap;
    long sent, lost, bytes;
} Net;

static Net net;
static uint32_t clock_ms;
static int alive[MACHINES], cut[MACHINES];
static Session *ses[MACHINES];
static Game games[MACHINES];
static uint32_t next_tick[MACHINES];
/* every machine's final hash of every 8th step, for the whole match (checked against the majority at the end) */
enum { HASHES_MAX = 4096 };
static uint32_t seen[HASHES_MAX][MACHINES];
static uint8_t seen_set[HASHES_MAX][MACHINES];
static int32_t seen_next[MACHINES];
static int32_t resynced_at[MACHINES]; /* the step a machine's resync took effect (-1: none) */
static int tick_ms[MACHINES];

static uint32_t net_rand(void) { net.rng = net.rng * 1664525u + 1013904223u; return net.rng >> 8; }

typedef struct { int node; } Ctx;
static Ctx ctx[MACHINES];

static void net_send(void *c, int to, const void *data, int len)
{
    int from = ((Ctx *)c)->node;
    net.sent++;
    net.bytes += len;
    if (cut[from] || to < 0 || to >= MACHINES || (int)(net_rand() % 100) < net.loss_pct || len > (int)sizeof net.q[0].data) {
        net.lost++;
        return;
    }
    if (net.n == net.cap) {
        net.cap = net.cap ? net.cap * 2 : 1024;
        net.q = realloc(net.q, sizeof *net.q * (size_t)net.cap);
    }
    Packet *p = &net.q[net.n++];
    int j = net.jitter ? (int)(net_rand() % (uint32_t)(2 * net.jitter + 1)) - net.jitter : 0;
    p->at = clock_ms + (uint32_t)(net.latency + j > 1 ? net.latency + j : 1);
    p->from = from;
    p->to = to;
    p->len = len;
    memcpy(p->data, data, (size_t)len);
}

static void deliver(void)
{
    for (int i = 0; i < net.n;) {
        Packet *p = &net.q[i];
        if (p->at > clock_ms) { i++; continue; }
        if (alive[p->to] && ses[p->to] && !cut[p->to]) ses_receive(ses[p->to], p->from, p->data, p->len, clock_ms);
        net.q[i] = net.q[--net.n];
    }
}

static NetGame game_of(int m)
{
    NetGame g = {{&games[m], g_save, g_load, g_advance, g_checksum}, g_serialize, g_deserialize, (int)sizeof(Toy)};
    return g;
}

/* ---- scenarios ---- */
typedef struct {
    const char *name;
    int machines;                /* at the start */
    int slots_each;              /* each machine's slots (consecutive) */
    int latency, jitter, loss;
    int seconds;
    int desync_at, desync_node;  /* a corrupted state (s), on that machine */
    int join_at;                 /* a machine joining (s) */
    int host_leaves_at;          /* the host gone (s) */
    int talkers;                 /* machines talking (voice), each its own pitch */
    int lobby;                   /* chat, a playlist vote, a vote kick */
} Scenario;

static const Scenario scenarios[] = {
    {"four", 4, 1, 60, 10, 2, 120, 0, 0, 0, 0, 0},
    {"sixteen", 8, 2, 80, 20, 5, 120, 0, 0, 0, 0, 0},
    {"split", 4, 4, 50, 10, 3, 90, 0, 0, 0, 0, 0},
    {"desync", 4, 1, 60, 10, 2, 90, 30, 2, 0, 0, 0},
    {"join", 3, 1, 60, 10, 2, 90, 0, 0, 20, 0, 0},
    {"migrate", 4, 1, 60, 10, 2, 90, 0, 0, 0, 30, 0},
    {"bad_net", 4, 1, 150, 60, 15, 90, 0, 0, 0, 0, 0},
    {"voice", 6, 1, 60, 15, 3, 40, 0, 0, 0, 0, 4},
    {"voice_cap", 12, 1, 60, 15, 3, 30, 0, 0, 0, 0, 12},
    {"lobby", 6, 1, 70, 15, 5, 60, 0, 0, 0, 0, 0, 1},
};

/* ---- the server's messages: chat, the playlist vote, a vote kick ---- */
static int chat_got[MACHINES][MACHINES], vote_result[MACHINES], vote_total[MACHINES], kick_seen[MACHINES], kicked_me[MACHINES];

static void ev_chat(void *c, int speaker, const char *text)
{
    int me = ((Ctx *)c)->node;
    char want[32];
    snprintf(want, sizeof want, "hello %d #%d", speaker, chat_got[me][speaker]);
    if (!strcmp(text, want)) chat_got[me][speaker]++; /* (in order, none lost) */
}

static void ev_vote(void *c, int round, int n, const uint8_t *options, const uint8_t *counts, int result, int left)
{
    (void)round; (void)left;
    int me = ((Ctx *)c)->node;
    if (result < 0) return;
    vote_result[me] = options[result];
    vote_total[me] = 0;
    for (int i = 0; i < n; i++) vote_total[me] += counts[i];
}

static void ev_kick(void *c, int target, int yes, int needed, int state)
{
    (void)yes; (void)needed;
    if (state == SES_KICK_PASSED && target == 5) kick_seen[((Ctx *)c)->node] = 1;
}

static void ev_kicked(void *c) { kicked_me[((Ctx *)c)->node] = 1; }

static void lobby_events(const Scenario *sc)
{
    (void)sc;
    if (clock_ms % 1000 == 0 && clock_ms >= 5000 && clock_ms < 8000) /* three lines from everyone */
        for (int m = 0; m < MACHINES; m++) {
            if (!alive[m] || !ses[m]) continue;
            static int said[MACHINES];
            if (clock_ms == 5000) said[m] = 0;
            char line[32];
            snprintf(line, sizeof line, "hello %d #%d", m, said[m]++);
            ses_chat(ses[m], line);
        }
    if (clock_ms == 10000) { /* the playlist vote: games 4, 7, 9 of the playlist */
        static const uint8_t games[3] = {4, 7, 9};
        ses_vote_start(ses[0], 3, games, 10, clock_ms);
    }
    if (clock_ms == 12000) /* the votes: 3 for the first, 2 the second, 1 the third */
        for (int m = 0; m < 6; m++) ses_vote(ses[m], 1, m == 0 || m == 3 || m == 5 ? 0 : m == 1 || m == 4 ? 1 : 2);
    if (clock_ms == 25000 || clock_ms == 26000 || clock_ms == 27000) /* three of the five others vote to kick machine 5 */
        ses_kick_vote(ses[1 + (int)(clock_ms - 25000) / 1000], 5, clock_ms);
    if (kicked_me[5] && alive[5]) alive[5] = 0; /* (it leaves) */
}

/* ---- voice: each talking machine a tone of its own pitch; each listener hears it back through Opus ---- */
static VoiceEnc *enc[MACHINES];
static VoiceDec *dec[MACHINES][MACHINES];
static int vsent[MACHINES], vplayed[MACHINES][MACHINES], vmatched[MACHINES][MACHINES], vdecoded[MACHINES][MACHINES];
static uint16_t vseq[MACHINES];
static int vconcurrent, vover;          /* the most speakers a listener played at once; ticks with more than 8 */
static long vticks;
static double phase[MACHINES];

static double pitch(int m) { return 220.0 + 70.0 * m; }

static void on_voice(void *c, int speaker, uint16_t seq, const uint8_t *data, int len)
{
    int me = ((Ctx *)c)->node;
    if (!dec[me][speaker]) dec[me][speaker] = voice_dec_new();
    voice_dec_put(dec[me][speaker], seq, data, len);
}

static void voice_tick(const Scenario *sc)
{
    for (int m = 0; m < MACHINES; m++) {
        if (!alive[m] || !ses[m]) continue;
        if (m < sc->talkers && (clock_ms / 1000) % 4 != 3) { /* (talk 3 s, then 1 s quiet) */
            int16_t pcm[VOICE_FRAME];
            for (int i = 0; i < VOICE_FRAME; i++) {
                pcm[i] = (int16_t)(8000 * sin(phase[m]));
                phase[m] += 2 * 3.14159265358979 * pitch(m) / VOICE_RATE;
            }
            uint8_t out[VOICE_PACKET_MAX];
            int n = voice_encode(enc[m], pcm, out, sizeof out);
            if (n > 0) { ses_send_voice(ses[m], vseq[m]++, out, n, clock_ms); vsent[m]++; }
        }
        int now_playing = 0;
        for (int sp = 0; sp < MACHINES; sp++) { /* the listener plays every speaker */
            if (!dec[m][sp]) continue;
            int16_t pcm[VOICE_FRAME];
            if (!voice_dec_pull(dec[m][sp], pcm)) continue;
            now_playing++;
            vplayed[m][sp]++;
            int cross = 0; /* the pitch heard: zero crossings */
            for (int i = 1; i < VOICE_FRAME; i++) cross += (pcm[i - 1] < 0) != (pcm[i] < 0);
            double heard = cross / 2.0 / 0.020;
            vdecoded[m][sp]++;
            if (fabs(heard - pitch(sp)) < pitch(sp) * 0.15) vmatched[m][sp]++;
        }
        if (now_playing > vconcurrent) vconcurrent = now_playing;
        vover += now_playing > SES_VOICE_TALKERS;
        vticks++;
    }
}

static int run(const Scenario *sc)
{
    memset(&net, 0, sizeof net);
    net.rng = 12345;
    net.latency = sc->latency;
    net.jitter = sc->jitter;
    net.loss_pct = sc->loss;
    clock_ms = 0;
    memset(alive, 0, sizeof alive);
    memset(cut, 0, sizeof cut);
    memset(games, 0, sizeof games);
    uint32_t slots[SES_MAX_NODES] = {0}, nodes = 0;
    int joiner = sc->join_at ? sc->machines : -1, delay = 2;
    for (int m = 0; m < sc->machines; m++) {
        nodes |= 1u << m;
        for (int k = 0; k < sc->slots_each; k++) slots[m] |= 1u << (m * sc->slots_each + k);
    }
    for (int m = 0; m < MACHINES; m++) {
        ctx[m].node = m;
        for (int k = 0; k < RB_RING; k++) games[m].kept_step[k] = -1;
        for (int i = 0; i < RB_MAX_SLOTS; i++) games[m].now.hp[i] = 100;
        games[m].now.rng = 99;
        tick_ms[m] = SES_STEP_MS + (m % 3) - 1; /* (each machine's clock a little off) */
        next_tick[m] = (uint32_t)(m * 7);
        ses[m] = NULL;
    }
    for (int m = 0; m < sc->machines; m++) {
        NetTransport t = {&ctx[m], net_send};
        NetGame g = game_of(m);
        ses[m] = ses_new(m, 0, nodes, slots, &t, &g, delay);
        alive[m] = 1;
        if (sc->talkers) ses_on_voice(ses[m], on_voice, &ctx[m]);
        if (sc->lobby) {
            SesEvents ev = {&ctx[m], ev_chat, ev_vote, ev_kick, ev_kicked};
            ses_on_events(ses[m], &ev);
        }
    }
    if (joiner >= 0) ses_host_offer_slots(ses[0], 1u << (joiner * sc->slots_each));
    memset(seen_set, 0, sizeof seen_set);
    memset(vsent, 0, sizeof vsent);
    memset(vplayed, 0, sizeof vplayed);
    memset(vmatched, 0, sizeof vmatched);
    memset(vdecoded, 0, sizeof vdecoded);
    memset(vseq, 0, sizeof vseq);
    vconcurrent = vover = 0;
    memset(chat_got, 0, sizeof chat_got);
    memset(vote_result, -1, sizeof vote_result);
    memset(vote_total, 0, sizeof vote_total);
    memset(kick_seen, 0, sizeof kick_seen);
    memset(kicked_me, 0, sizeof kicked_me);
    vticks = 0;
    memset(phase, 0, sizeof phase);
    for (int m = 0; m < MACHINES; m++) {
        if (sc->talkers && m < sc->talkers) enc[m] = voice_enc_new();
        for (int sp = 0; sp < MACHINES; sp++) dec[m][sp] = NULL;
    }
    for (int m = 0; m < MACHINES; m++) { seen_next[m] = 0; resynced_at[m] = -1; }
    for (clock_ms = 0; clock_ms < (uint32_t)sc->seconds * 1000; clock_ms++) {
        if (sc->join_at && clock_ms == (uint32_t)sc->join_at * 1000) {
            NetTransport t = {&ctx[joiner], net_send};
            NetGame g = game_of(joiner);
            ses[joiner] = ses_join(joiner, 0, &t, &g, delay, clock_ms);
            alive[joiner] = 1;
        }
        if (sc->desync_at && clock_ms == (uint32_t)sc->desync_at * 1000)
            games[sc->desync_node].now.score[5] += 7; /* (a bug on one machine) */
        if (sc->host_leaves_at && clock_ms == (uint32_t)sc->host_leaves_at * 1000) {
            cut[0] = 1; /* (the host's machine off the network: gone) */
            alive[0] = 0;
        }
        deliver();
        if (sc->talkers && clock_ms % 20 == 0) voice_tick(sc);
        if (sc->lobby) lobby_events(sc);
        for (int m = 0; m < MACHINES; m++) {
            if (!alive[m] || !ses[m] || clock_ms < next_tick[m]) continue;
            next_tick[m] += (uint32_t)tick_ms[m];
            RbInput local[RB_MAX_SLOTS] = {{{0}}};
            int32_t step = rb_now(ses_rollback(ses[m])) + delay;
            for (int i = 0; i < RB_MAX_SLOTS; i++) local[i] = player_input(i, step);
            int had = ses_stats(ses[m])->resyncs_done;
            ses_step(ses[m], local, clock_ms);
            ses_send(ses[m], clock_ms);
            Rollback *rb = ses_rollback(ses[m]);
            if (ses_stats(ses[m])->resyncs_done != had) resynced_at[m] = rb_now(rb);
            if (seen_next[m] < rb_now(rb) - RB_RING / 2) /* (a restart: from where its states begin) */
                seen_next[m] = (rb_now(rb) - RB_RING / 2 + SES_HASH_EVERY) / SES_HASH_EVERY * SES_HASH_EVERY;
            for (uint32_t sum; seen_next[m] / SES_HASH_EVERY < HASHES_MAX && rb_checksum(rb, seen_next[m], &sum);
                 seen_next[m] += SES_HASH_EVERY) {
                seen[seen_next[m] / SES_HASH_EVERY][m] = sum;
                seen_set[seen_next[m] / SES_HASH_EVERY][m] = 1;
            }
        }
    }
    /* the whole match: at every 8th step the machines' hashes against the majority's (a desynced machine allowed to
     * differ from the corruption until its resync) */
    int ref = -1, ok = 1, compared = 0, mismatched = 0, steps_checked = 0;
    for (int m = 0; m < MACHINES; m++) if (alive[m] && ses[m] && ses_ready(ses[m]) && ref < 0) ref = m;
    for (int k = 0; k < HASHES_MAX; k++) {
        int n = 0;
        uint32_t v[MACHINES];
        int who[MACHINES];
        for (int m = 0; m < MACHINES; m++) if (seen_set[k][m]) { v[n] = seen[k][m]; who[n++] = m; }
        if (n < 2) continue;
        steps_checked++;
        uint32_t major = v[0];
        for (int a = 0, best = 0; a < n; a++) {
            int c = 0;
            for (int b = 0; b < n; b++) c += v[b] == v[a];
            if (c > best) { best = c; major = v[a]; }
        }
        for (int a = 0; a < n; a++) {
            compared++;
            if (v[a] == major) continue;
            int32_t step = k * SES_HASH_EVERY;
            int excused = sc->desync_at && who[a] == sc->desync_node && step >= sc->desync_at * 20 - 40 &&
                          (resynced_at[who[a]] < 0 || step < resynced_at[who[a]]);
            if (!excused) mismatched++;
        }
    }
    if (steps_checked < 50 || mismatched) ok = 0;
    printf("%-8s %s: ", sc->name, ok ? "PASS" : "FAIL");
    printf("%d steps checked across the match (%d hashes, %d differ); ", steps_checked, compared, mismatched);
    int host = ref >= 0 ? ses_host(ses[ref]) : -1;
    for (int m = 0; m < MACHINES; m++) {
        if (!alive[m] || !ses[m]) continue;
        const RbStats *rs = rb_stats(ses_rollback(ses[m]));
        const SesStats *ss = ses_stats(ses[m]);
        int32_t expected = (int32_t)(clock_ms / SES_STEP_MS);
        int32_t now = rb_now(ses_rollback(ses[m]));
        if (m == ref)
            printf("step %d of ~%d, rollbacks %d (longest %d, %d steps redone), stalls %d", now, expected, rs->rollbacks,
                   rs->longest, rs->resimulated, rs->stalls);
        if (now < expected * 8 / 10 - (sc->join_at && m == joiner ? sc->join_at * 20 : 0) - (sc->host_leaves_at ? 60 : 0)) {
            ok = 0;
            printf(" [machine %d fell behind: step %d]", m, now);
        }
        if (m == host) {
            printf("; host %d: %d hashes checked, %d desyncs seen, %d resyncs sent, %d joins, %d host moves", m,
                   ss->hashes_checked, ss->desyncs_seen, ss->resyncs_sent, ss->joins, ss->migrations);
            if (sc->desync_at && (!ss->desyncs_seen || !ss->resyncs_sent)) { ok = 0; printf(" [the desync was not caught]"); }
            if (!sc->desync_at && ss->desyncs_seen) { ok = 0; printf(" [a desync without a cause]"); }
            if (sc->join_at && !ss->joins) { ok = 0; printf(" [no join]"); }
            if (!ss->hashes_checked) { ok = 0; printf(" [no hashes checked]"); }
        }
    }
    if (sc->join_at && (!ses[joiner] || !ses_ready(ses[joiner]))) { ok = 0; printf(" [the joining machine never got in]"); }
    if (sc->host_leaves_at && host == 0) { ok = 0; printf(" [the host never moved]"); }
    if (sc->talkers) { /* every listener heard each speaker (8 at most), at its pitch */
        int pairs = 0, good = 0, heard_max = 0;
        long played = 0, matched = 0, decoded = 0;
        for (int l = 0; l < MACHINES; l++) {
            if (!alive[l]) continue;
            int heard = 0;
            for (int sp = 0; sp < sc->talkers; sp++) {
                if (sp == l || !vsent[sp]) continue;
                if (vplayed[l][sp] > vsent[sp] / 2) heard++;
                if (sc->talkers > SES_VOICE_TALKERS) continue;
                pairs++;
                played += vplayed[l][sp];
                matched += vmatched[l][sp];
                decoded += vdecoded[l][sp];
                if (vplayed[l][sp] >= vsent[sp] * 9 / 10 && vmatched[l][sp] >= vdecoded[l][sp] * 9 / 10) good++;
            }
            if (heard > heard_max) heard_max = heard;
        }
        printf("; voice: ");
        if (sc->talkers > SES_VOICE_TALKERS) {
            /* (at most 8 at once; a speaker's last buffered frames may overlap the next one's first for a moment) */
            printf("%d of %d talking, at most %d heard at once (%.2f%% of the time more than %d)", heard_max, sc->talkers,
                   vconcurrent, vticks ? 100.0 * vover / vticks : 0.0, SES_VOICE_TALKERS);
            if (vconcurrent > SES_VOICE_TALKERS + 1 || vover * 100 > vticks || vconcurrent < SES_VOICE_TALKERS - 1) {
                ok = 0;
                printf(" [the 8-speaker cap]");
            }
        } else {
            printf("%d of %d listener-speaker pairs clear, %ld frames played, %.1f%% at the right pitch", good, pairs, played,
                   decoded ? 100.0 * matched / decoded : 0.0);
            if (good < pairs) { ok = 0; printf(" [voice lost or garbled]"); }
        }
        for (int m = 0; m < MACHINES; m++) {
            voice_enc_free(enc[m]);
            enc[m] = NULL;
            for (int sp = 0; sp < MACHINES; sp++) { voice_dec_free(dec[m][sp]); dec[m][sp] = NULL; }
        }
    }
    if (sc->lobby) { /* every chat line everywhere in order; one vote result everywhere; machine 5 kicked */
        int chats = 0, votes = 0, kicks = 0;
        for (int l = 0; l < 6; l++) {
            for (int sp = 0; sp < 6; sp++) chats += sp != l && chat_got[l][sp] == 3;
            votes += vote_result[l] == 4 && vote_total[l] == 6;
            kicks += l == 5 ? kicked_me[5] : kick_seen[l] && !ses_present(ses[l], 5);
        }
        printf("; lobby: %d of 30 chat streams whole, %d of 6 saw the vote won by game 4 (3-2-1), kick: %d of 6", chats, votes, kicks);
        if (chats != 30 || votes != 6 || kicks != 6) { ok = 0; printf(" [lobby messages]"); }
    }
    printf("; network: %ld packets, %ld lost, %.1f KB/s\n", net.sent, net.lost, net.bytes / 1024.0 / sc->seconds);
    for (int m = 0; m < MACHINES; m++) { ses_free(ses[m]); ses[m] = NULL; }
    free(net.q);
    net.q = NULL;
    return ok;
}

int main(int argc, char **argv)
{
    int failed = 0, ran = 0;
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        int want = argc < 2;
        for (int a = 1; a < argc; a++) want |= !strcmp(argv[a], scenarios[i].name);
        if (!want) continue;
        ran++;
        failed += !run(&scenarios[i]);
    }
    printf("%d of %d scenarios passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
