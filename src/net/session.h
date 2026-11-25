/* A network match (docs/ONLINE_PLAN.md 1-3, 9): the host relays every machine's inputs to every machine (a star),
 * each machine runs the rollback engine (rollback.h) with them. Also: the state hashes (a desync: the host's state sent
 * to that machine), a player joining a running match (the host's state, then the inputs), and the host leaving (the
 * machine that joined first carries on as the host).
 *
 * Machines are nodes 0..SES_MAX_NODES-1 (the host gives the numbers); each owns some slots (its local players). The
 * transport is the caller's: real UDP (net_udp.c) or a simulated network (tools/net_sim). Times are milliseconds. */
#ifndef TM2_NET_SESSION_H
#define TM2_NET_SESSION_H

#include <stdint.h>
#include "rollback.h"

enum {
    SES_MAX_NODES = 16,
    SES_STEP_MS = 50,          /* the game's 20 steps a second */
    SES_HASH_EVERY = 8,        /* a state hash sent every Nth step */
    SES_TIMEOUT_MS = 5000,     /* a machine silent this long has left (2 s dropped one whose PC stalled a moment; the others
                                * see WAITING FOR PLAYERS from 0.7 s: the user's, 2026-11-05) */
    SES_AWAIT_MS = 20000,      /* (a machine waiting for the host's state: the host silent this long has left; before
                                * the host takes a joining machine in, it says nothing, the more so through the relay) */
    SES_LOAD_MS = 40000,       /* (host) a player's machine with no step run this long after the host's battle began never
                                * came in: it has left (its game stopped loading; the others otherwise wait on it for
                                * good). Under the game's minute (game_tm2.c: a wait that long, the match alone) */
    SES_MIGRATE_MS = 400,     /* a new host gathers the inputs this long before going on */
};

typedef struct {
    void *ctx;
    void (*send)(void *ctx, int to_node, const void *data, int len);
} NetTransport;

typedef struct {
    RbCallbacks rb;
    int (*serialize)(void *ctx, int32_t step, uint8_t *out, int cap); /* the kept state of step's start (bytes, 0: none) */
    void (*deserialize)(void *ctx, const uint8_t *in, int len);       /* that state made the current one */
    int state_max;                                                    /* the largest serialized state (bytes) */
} NetGame;

typedef struct {
    int resyncs_sent, resyncs_done, desyncs_seen, migrations, joins;
    int hashes_checked;
} SesStats;

typedef struct Session Session;

/* node: this machine; host: the host's node; nodes: the machines there at the start (bits), slots[node] each one's
 * slots; delay: the input delay (steps, the same everywhere). A joining machine: ses_join instead. */
Session *ses_new(int node, int host, uint32_t nodes, const uint32_t slots[SES_MAX_NODES], const NetTransport *net,
                 const NetGame *game, int delay);
Session *ses_join(int node, int host, const NetTransport *net, const NetGame *game, int delay, uint32_t now_ms);
/* a machine coming back to a match: these slots again (its own: the host offers them, ses_host_offer_slots), the
 * host's state from the next steps on */
Session *ses_join_slots(int node, int host, const NetTransport *net, const NetGame *game, int delay, uint32_t now_ms,
                        uint32_t slots);
/* (a machine coming back, its match loaded) the ask itself: the host's state from now on (the host keeps only the
 * last steps' inputs: an ask before a long load would leave a gap) */
void ses_ask_join(Session *s, uint32_t slots);
/* (ses_join_slots / ses_ask_join's slots) a machine watching a match on: no slot of its own, every input and the
 * host's state (the user's 2026-11-11: one joining mid-match watches until the next one starts) */
#define SES_WATCH 0x80000000u
void ses_free(Session *s);
void ses_bye(Session *s); /* this machine leaving: the others told at once */

void ses_receive(Session *s, int from, const void *data, int len, uint32_t now_ms);
int ses_step(Session *s, const RbInput local[RB_MAX_SLOTS], uint32_t now_ms); /* each step tick: steps run (0-3) */
void ses_send(Session *s, uint32_t now_ms); /* after each tick: the packets for every machine */

/* lockstep: one step a call, only once every slot's input is here (the game's loop is the clock; no prediction) */
void ses_set_lockstep(Session *s, int on);
/* a game's rollback: one step a call at most (one an iteration of its loop; a machine behind stays behind, the others
 * wait in their window), and a machine still loading or waiting for the host's state timed as lockstep times it */
void ses_set_one_step(Session *s, int on);
int ses_behind(const Session *s); /* (one_step) 2 steps or more behind the host: a step more this iteration helps */
int ses_state_owed(const Session *s); /* (host) a machine waits for a state the game could not make yet */
/* a machine that starts from the host's state: no step runs until it is here (R_STATE) */
void ses_await_state(Session *s);
/* (a new host whose state never came: its own is the match's from here; it sends it to the others) */
void ses_ready_self(Session *s);
/* a match's own tag (set before its first send): packets of another match (the one before, still in flight) dropped */
void ses_set_tag(Session *s, uint16_t tag);
/* that machine's reliable data (a state's chunks) paced to this many bytes a second (0: none; a machine reached
 * through the master's relay: under the relay's share) */
void ses_set_rate(Session *s, int node, uint32_t bytes_per_s);
/* this machine stopped stepping a while (a kill cam's replay): every machine counted as heard now (none taken as gone
 * for the quiet it could not hear) */
void ses_forgive_quiet(Session *s, uint32_t now_ms);
uint32_t ses_quiet_ms(const Session *s, int node, uint32_t now_ms); /* how long since that machine was heard (0: not there) */
/* idle: no steps run (a server's lobby: the machines, the messages, nobody playing); ses_step only keeps who is there */
void ses_set_idle(Session *s, int on);
/* the game's own messages (net/lobby.c), whole and in order on the reliable channel, up to SES_APP_MAX bytes: the host
 * to any machine, a machine to the host only; they arrive at SesEvents.app */
enum { SES_APP_MAX = 16384 };
void ses_app_send(Session *s, int to, const void *data, int len);
/* the reliable messages not acked yet to that machine; *state_got: the bytes of a state arriving (-1: none) */
int ses_rtt(const Session *s, int node); /* the round trip to a machine, ms (0: not known yet) */
int ses_rel_pending(const Session *s, int node, int *state_got);
/* (host) its state to that machine now (game.serialize of the next step; a match's start in lockstep) */
void ses_push_state(Session *s, int to, uint32_t now_ms);
/* (a machine) the host asked for its base (a resync: only what differs from it comes): once, then 0 */
int ses_base_asked(Session *s);
int ses_host(const Session *s);
/* a machine's start from the host's state, only what differs from its own (the game's base: its state's block hashes,
 * made as its battle loaded): the host waits up to ms for a machine's base, then sends the whole state */
void ses_set_base_wait(Session *s, uint32_t ms);
/* the machines (bits) whose players only watch: chosen as the new host last (the lobby's list, the same on every machine) */
void ses_set_watchers(Session *s, uint32_t nodes);
void ses_send_base(Session *s, const uint8_t *base, int len); /* (a machine) its base to the host (len 0: the whole, please) */
const uint8_t *ses_state_base(const Session *s, int *len);  /* (host, in the game's serialize) the machine's base, or NULL */
int ses_ready(const Session *s);              /* in the match (a joiner: once the host's state arrived) */
uint32_t ses_local_slots(const Session *s);
Rollback *ses_rollback(Session *s);
const SesStats *ses_stats(const Session *s);

/* voice (voice.h): a machine's encoded frames go through the host to everyone (the host never decodes what it
 * relays; 8 speakers at once at most); each machine gets every other speaker's frames from the callback (muting: the
 * listener drops them) */
enum { SES_VOICE_TALKERS = 8 };
typedef void (*SesVoiceFn)(void *ctx, int speaker, uint16_t seq, const uint8_t *data, int len);
void ses_on_voice(Session *s, SesVoiceFn fn, void *ctx);
void ses_send_voice(Session *s, uint16_t seq, const uint8_t *data, int len, uint32_t now_ms);

/* The server's own messages (docs/ONLINE_PLAN.md 6-7), all through the host on the reliable channel:
 * chat; the playlist vote (the host offers 2-3 games, everyone votes, the most votes wins, a tie: the first offered);
 * vote kick (more than half of the other players: the host removes the player; the host can kick at once). Muting is
 * the listener's own (it drops the speaker's chat and voice). */
enum { SES_CHAT_MAX = 120, SES_VOTE_MAX = 3, SES_KICK_MS = 30000 };
enum { SES_KICK_RUNNING, SES_KICK_PASSED, SES_KICK_FAILED };
typedef struct {
    void *ctx;
    void (*chat)(void *ctx, int speaker, const char *text);
    /* the vote: options (the host's playlist entries), the counts so far, result -1 until it ends */
    void (*vote)(void *ctx, int round, int n, const uint8_t *options, const uint8_t *counts, int result, int seconds_left);
    void (*kick)(void *ctx, int target, int yes, int needed, int state); /* a kick vote's progress */
    void (*kicked)(void *ctx);                                            /* this machine was removed */
    void (*app)(void *ctx, int from, const uint8_t *data, int len);      /* the game's own message (ses_app_send) */
} SesEvents;
void ses_on_events(Session *s, const SesEvents *ev);
/* what a machine is doing while it does not step (sent with the inputs; the host passes everyone's on), so a wait can
 * say why: SES_AWAY_NONE stepping, SES_AWAY_REPLAY a kill cam's replay, SES_AWAY_BETWEEN its results, a load */
enum { SES_AWAY_NONE, SES_AWAY_REPLAY, SES_AWAY_BETWEEN, SES_AWAY_RESYNC }; /* (RESYNC: the host, holding the match while
                                                                            * a machine takes its state again) */
int ses_resync_node(const Session *s); /* (host) a machine the match is held for, its state on the way again; -1 none */
void ses_set_away(Session *s, int away);
int ses_away(const Session *s, int node);
void ses_chat(Session *s, const char *text);
void ses_vote_start(Session *s, int n, const uint8_t *options, int seconds, uint32_t now_ms); /* (host) */
void ses_vote(Session *s, int round, int choice);
void ses_kick_vote(Session *s, int target, uint32_t now_ms);
void ses_kick(Session *s, int target, uint32_t now_ms);                                       /* (host) */
int ses_present(const Session *s, int node);

/* tests: a joining machine asks for slots; the host gives it the first free of them */
void ses_host_offer_slots(Session *s, uint32_t free_slots);

#endif
