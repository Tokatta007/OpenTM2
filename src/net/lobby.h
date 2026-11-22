/* A server's lobby (docs/ONLINE_PLAN.md, docs/NETWORK_MENUS.md): the host and the machines that joined it, over one UDP
 * socket a machine. The host owns the lobby: the game's settings, every machine's players with their cars, teams and
 * ready marks; it sends the whole lobby to everyone when it changes. A machine sends only its own players' choices
 * (car, team, ready), and leaves; the host starts the match (L_START: the machines, each one's players, the seed),
 * and every machine then runs it (net/game_tm2.c).
 *
 * On the socket, the first byte of every packet is its channel: the handshake (a machine asks in and gets its node
 * number, or a refusal: another build, full, banned), the lobby's session (session.c, idle: presence, chat, vote
 * kick, the lobby's own messages through ses_app_send) and the match's session (game_tm2.c's, passed through). */
#ifndef TM2_NET_LOBBY_H
#define TM2_NET_LOBBY_H
#include <stdint.h>
#include "session.h"

enum {
    LOBBY_PORT = 27520,      /* the default port (LAN, and the port a host forwards) */
    LOBBY_LOCAL_MAX = 4,     /* players on one machine (split screen) */
    LOBBY_NAME = 17, /* (a profile name's 16 letters, PROFILE_NAME_MAX, and its end: 16 had room for 15) */
    LOBBY_SETTINGS_MAX = 8192,
    LOBBY_CHAT_LINES = 32,
};
enum { LOBBY_CONNECTING, LOBBY_IN, LOBBY_REFUSED_BUILD, LOBBY_REFUSED_FULL, LOBBY_REFUSED_BANNED, LOBBY_LOST, LOBBY_KICKED,
       LOBBY_REFUSED_PASSWORD };

typedef struct {
    char name[LOBBY_NAME];
    int8_t car;     /* -1: not chosen (random) */
    int8_t team;    /* -1: none */
    uint8_t ready;
    int8_t vote;    /* the next match's vote: a card (-1 none) */
    uint8_t vote_round; /* of that vote (an older one's does not count) */
    uint8_t table[128]; /* their button table (action -> button: their layout and own buttons; every machine uses it) */
    uint8_t online_id[8]; /* their profile online (net/friends.h: a friend code; all 0: a guest) */
    int8_t skin;    /* their car's skin: an index in the server's mod list (net/lobby_mods.c), -1 the car's own */
    int16_t picture; /* their profile's picture (the lobby's portrait), -1 none (their car's) */
} LobbyPlayer;
typedef struct {
    uint8_t present, count;
    LobbyPlayer p[LOBBY_LOCAL_MAX];
    char addr[16];      /* where the host hears it (a.b.c.d) and its port: every machine knows every other's, so any of */
    uint16_t port;      /* them can take over as the host (host migration) */
    uint32_t id;        /* the machine's own number (this run of the game): a machine dropped and back is known again */
    uint16_t ping;      /* its round trip to the host, ms, as the host measures it (the host: 0; 0 too: not known yet) */
} LobbyMachine;
typedef struct {
    uint32_t seed;
    uint8_t delay;                       /* input delay (steps) */
    uint8_t humans;                      /* every machine's players, in node order */
    uint8_t first[SES_MAX_NODES];        /* each machine's first player's human index (0xFF: none) */
    int8_t car[SES_MAX_NODES];           /* each human's car (the host's pick for a random one) */
    int8_t skin[SES_MAX_NODES];          /* each human's skin (the server's mod list's index, -1 none) */
} LobbyStart;
typedef struct {
    uint32_t seq;                        /* changes with every update the host sends */
    char server[32];
    int settings_len;
    uint8_t settings[LOBBY_SETTINGS_MAX];/* the game's settings, as the UI packs them (opaque here) */
    LobbyMachine m[SES_MAX_NODES];       /* by node; node 0 the host */
    uint32_t starts;                     /* matches started: a new value = L_START arrived, `start` holds it */
    uint8_t match_on;                    /* the host is in a match (a machine waiting to come back: over, the lobby) */
    LobbyStart start;
} LobbyView;

typedef struct Lobby Lobby;
/* names: count of LOBBY_NAME-byte names (this machine's players); loopback: this PC only (tests: no firewall prompt) */
Lobby *lobby_host(uint16_t port, int loopback, const char *server, int count, const char (*names)[LOBBY_NAME]);
Lobby *lobby_join(const char *host, uint16_t port, int loopback, int count, const char (*names)[LOBBY_NAME]);
void lobby_free(Lobby *l);
/* each frame (menus, and while a match waits): the packets in (the match's to its handler), the lobby's out */
void lobby_poll(Lobby *l, uint32_t now_ms);
int lobby_status(const Lobby *l);
int lobby_node(const Lobby *l);          /* this machine's node (-1 before it is in) */
int lobby_is_host(const Lobby *l);
uint16_t lobby_port(const Lobby *l);       /* this machine's game port (its socket's) */
int lobby_host_node(const Lobby *l);     /* the host's node (it changes when the host leaves: the machine there longest) */
/* the host left and another took over: the moment it did (0: never), the new host's node; the UI's notice */
uint32_t lobby_migrated_at(const Lobby *l, int *new_host);
const LobbyView *lobby_view(const Lobby *l);
/* this machine's players' choices (the host's go straight into the lobby) */
void lobby_set_player(Lobby *l, int local, int car, int team, int ready);
void lobby_set_skin(Lobby *l, int local, int skin); /* their car's skin (the server's mod list's index, -1 none) */
/* messages of other modules (net/lobby_mods.c) on the lobby's reliable channel: first byte LOBBY_EXT_FIRST.. */
enum { LOBBY_EXT_FIRST = 64 };
void lobby_on_ext(Lobby *l, void (*fn)(void *ctx, int from, const uint8_t *data, int len), void *ctx);
void lobby_ext_send(Lobby *l, int to, const void *data, int len);
/* (host) */
void lobby_set_settings(Lobby *l, const void *data, int len);
void lobby_set_team(Lobby *l, int node, int i, int team); /* a player's team (a joiner's: the host's balancing; it adopts it) */
void lobby_set_max_players(Lobby *l, int max); /* the players' room: a machine whose players do not fit is refused (full) */
/* this machine's player i votes for card `choice` of vote `round` (every player one vote; the tally: from the lobby) */
void lobby_set_vote(Lobby *l, int i, int round, int choice);
void lobby_set_table(Lobby *l, int i, const uint8_t table[128]); /* this machine's player i's button table */
void lobby_start(Lobby *l, uint32_t seed, int delay, const int8_t car[SES_MAX_NODES]);
/* (host) a machine back in the match on: its players the humans from `first` again (the start as it is, theirs
 * moved to its node; every machine told, that one goes in: its starts move on and lobby_rejoining is 1) */
void lobby_rejoin(Lobby *l, int node, int first);
/* (host) a machine new to the match on: in to watch it (no players of its own: its start's first stays 0xFF; it plays
 * from the next match's start, the user's 2026-11-11); lobby_rejoining is 1 there too */
void lobby_watch(Lobby *l, int node);
int lobby_rejoining(const Lobby *l);
void lobby_set_humans(Lobby *l, int humans);
void lobby_set_car(Lobby *l, int h, int car);
int lobby_node_relayed(const Lobby *l, int node); /* that machine reached through a master's relay */
/* voice chat (voice_io.c): this machine's frames out through the server's session; everyone else's in */
void lobby_on_voice(Lobby *l, SesVoiceFn fn, void *ctx);
void lobby_voice_send(Lobby *l, uint16_t seq, const uint8_t *data, int len);    /* (a machine) the start it has is a match on, to come back into */
void lobby_rejoin_done(Lobby *l);
void lobby_set_match_on(Lobby *l, int on);
void lobby_set_picture(Lobby *l, int i, int picture); /* this machine's player i's profile picture (before joining) */
void lobby_set_online_id(Lobby *l, int i, const uint8_t id[8]); /* this machine's player i's profile online (net/friends.h) */ /* (host) in a match or back in the lobby: every machine told */
uint32_t lobby_machine_id(void);        /* this run of the game's own number */
void lobby_kick(Lobby *l, int node);
/* chat and the vote to kick (session.c); the chat's lines kept for the UI, the newest last */
void lobby_chat(Lobby *l, const char *text);
void lobby_kick_vote(Lobby *l, int node);
/* the last kick vote's progress (SES_KICK_RUNNING / PASSED / FAILED, the votes for it and needed; at: when it was
 * last heard of); 0: none yet */
int lobby_kick_progress(const Lobby *l, int *target, int *yes, int *needed, int *state, uint32_t *at);
int lobby_chat_lines(const Lobby *l, int i, int *speaker, const char **text); /* i from 0 (the oldest kept); 0: none */
/* the match's channel: its transport, and its packets as they arrive */
NetTransport lobby_match_transport(Lobby *l);
void lobby_on_match(Lobby *l, void (*fn)(void *ctx, int from, const uint8_t *data, int len, uint32_t now_ms), void *ctx);
/* the build's identity: machines play together only with the same build (lockstep: the same code) */
uint32_t lobby_build_id(void);

/* LAN: a hosting machine answers the browser's broadcast (UDP_LAN_PORT) with its listing; the browser asks and
 * gathers the answers for wait_ms (loopback: this PC only, tests) */
typedef struct {
    char name[32], host[24], type[32];  /* the server, its host player, its game type's name */
    char addr[48];                      /* (found) where it answered from */
    uint16_t port;                      /* its game port */
    uint8_t players, max, game, arena, mode, playing;
    uint8_t mods;                       /* the server's mods (its skins and characters), how many */
    char mod_names[48];                 /* their names, ", " between, cut with "..." when longer */
    uint8_t same_build;                 /* (found) its build is this one (else it cannot be joined: an update) */
    uint8_t locked;                     /* a private server: joining needs its password (set by the lobby, lobby_set_password) */
    int ping;                           /* (found) ms */
} LobbyListing;
void lobby_set_listing(Lobby *l, const LobbyListing *info); /* (host) what the LAN browser shows; players kept by the lobby */
/* a private server's password (LOBBY_PASSWORD - 1 letters at most; "" none): the host's, needed to join it (else
 * LOBBY_REFUSED_PASSWORD); a joining machine's, sent with its hello (set right after lobby_join). Kept on every machine
 * that got in, so a new host (the old one left) asks the same. */
enum { LOBBY_PASSWORD = 24 };
void lobby_set_password(Lobby *l, const char *password);
typedef struct LanSearch LanSearch;
LanSearch *lobby_lan_search(int loopback);                       /* the question sent */
int lobby_lan_results(LanSearch *s, LobbyListing *out, int max); /* each frame: the servers that answered so far */
/* the internet's browser: the master server's list (net/master.h), its answers read as the LAN's (lobby_lan_results) */
LanSearch *lobby_master_search(const char *masters, int loopback); /* every master of the list ("host:port, ...") */
/* an internet server: the master server it is listed on (host) or known to (the others), introductions through it */
void lobby_set_masters(Lobby *l, const char *list); /* "host:port, host:port" (each listed on all of them) */
enum { MASTERS_MAX = 8 };
int masters_parse(const char *list, char addr[][48], uint16_t port[], int max);
void lobby_lan_search_free(LanSearch *s);
#endif
