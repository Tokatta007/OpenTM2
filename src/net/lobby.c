/* A server's lobby: the handshake, the lobby's session and its messages (lobby.h). */
#include "lobby.h"
#include "net_udp.h"
#include "master.h"
#include "friends.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

enum { CH_HELLO = 0x48, CH_LOBBY = 0x4C, CH_MATCH = 0x4D }; /* the first byte of a packet: 'H', 'L', 'M' */
enum { HELLO = 1, WELCOME = 2, REFUSED = 3 };                 /* the handshake */
enum { RELAY_AFTER_MS = 3000 }; /* a joining machine unanswered this long: through the relay */
enum { L_INFO = 1, L_SET = 2, L_STATE = 3, L_START = 4, L_VOTE = 5, L_TABLE = 6, L_REJOIN = 7, L_PINGS = 8, L_SKIN = 9,
       L_BACK = 10, L_BACK_STATE = 11 }; /* (L_BACK: a machine's vote to go back to the lobby; L_BACK_STATE: the host's tally) */     /* the lobby's messages (ses_app_send) */
enum { HELLO_EVERY_MS = 500, HELLO_GIVE_UP_MS = 10000, PENDING_MS = 10000, STATE_EVERY_MS = 100, BANNED_MAX = 16, PINGS_EVERY_MS = 2000 };
static const uint8_t MAGIC[4] = {'T', 'M', '2', 'L'};
enum { PLAYER_BYTES = LOBBY_NAME + 5 + 128 + 8 + 1 + 2 };

struct Lobby {
    struct { /* the vote to go back to the lobby (a match on): the host's tally, every machine's copy (lobby_back_progress) */
        uint8_t round, caller, yes, no, needed, state; /* state: LOBBY_BACK_* */
        uint8_t total, secs;                    /* the machines that may vote; the seconds left (the host's, sent) */
        uint8_t vote[SES_MAX_NODES];            /* (host) each machine's: 0 none, 1 yes, 2 no */
        uint32_t at, ended;                     /* (host) its start; its end (the result shown a while) */
        uint8_t mine_round;                     /* (each machine) the round this machine voted in */
    } back;
    SesVoiceFn voice;                           /* the voices as they come (voice_io.c) */
    void *voice_ctx;
    uint32_t pending_id[SES_MAX_NODES];         /* (host) the number each machine given a node said in its hello */
    int rejoin;                                 /* (a machine) its start: a match on, to come back into */
    char master[MASTERS_MAX][48];               /* the master servers (an internet server's): their addresses, ports */
    uint16_t master_port[MASTERS_MAX];
    int masters;
    uint32_t master_ms;                         /* the last REGISTER / KEEP */
    struct { int target, yes, needed, state; uint32_t at; } kick; /* the last kick vote's progress (at: when; 0 none) */
    int host, node, status;
    int loopback;                               /* this PC only (the LAN socket too, once this machine hosts) */
    int host_node;                              /* the host's node (0 until a host leaves and another takes over) */
    uint32_t migrated_ms;                       /* when another machine took over as the host (0: never) */
    NetUdp *udp;
    Session *ses;
    LobbyView view;
    int count;                                  /* this machine's players */
    int spectate;                               /* which of this machine's players only watch (bit i; lobby_set_spectate) */
    LobbyPlayer mine[LOBBY_LOCAL_MAX];
    uint32_t hello_ms, hello_first_ms;          /* (a machine) the handshake's tries */
    uint32_t pending_ms[SES_MAX_NODES];         /* (host) nodes given out, not in the session yet */
    int last_node;                              /* (host) the node given out last: the next after it (join order) */
    int was_present[SES_MAX_NODES];
    int dirty;                                  /* (host) the lobby changed: everyone gets it */
    uint32_t sent_ms;
    uint32_t pings_ms;                          /* (host) the pings sent then (every PINGS_EVERY_MS) */
    char banned[BANNED_MAX][48];                /* (host) addresses kicked: no rejoining */
    int banned_n;
    struct { int speaker; char text[SES_CHAT_MAX + 1]; } chat[LOBBY_CHAT_LINES];
    int chat_n, chat_total; /* (chat_total: every line ever, the kept ones its last chat_n) */
    void (*match)(void *ctx, int from, const uint8_t *data, int len, uint32_t now_ms);
    void *match_ctx;
    uint32_t now;
    NetUdp *lan;                                /* (host) the LAN browser's questions */
    int max_players;                            /* (host) the players' room */
    LobbyListing listing;
    char password[LOBBY_PASSWORD];              /* a private server's (lobby_set_password) */
    void (*ext)(void *ctx, int from, const uint8_t *data, int len); /* (net/lobby_mods.c) */
    void *ext_ctx;
};
static uint16_t lan_port(void) { return getenv("TM2PC_NET_LAN_PORT") ? (uint16_t)atoi(getenv("TM2PC_NET_LAN_PORT")) : UDP_LAN_PORT; }
static const uint8_t ASK[4] = {'T', 'M', '2', 'Q'}, ANSWER[4] = {'T', 'M', '2', 'A'};
enum { LISTING_BYTES = 4 + 4 + 32 + 24 + 32 + 2 + 6, LISTING_MODS = LISTING_BYTES + 1 + 48, /* (the mods after the rest:
                                     * the masters keep 160 bytes of a listing as they are, tools/master/master.c) */
       LISTING_LOCKED = LISTING_MODS + 1 }; /* (then whether it needs a password: no master change) */
typedef char listing_fits_masters[LISTING_LOCKED <= 160 ? 1 : -1];

/* ---- the build ---- */
uint32_t lobby_build_id(void)
{
    static uint32_t id;
    if (id) return id;
    char path[1024] = "";
#ifdef _WIN32
    GetModuleFileNameA(NULL, path, sizeof path);
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n > 0) path[n] = 0;
#endif
    uint32_t h = 2166136261u;
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (f) {
        static uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
        fclose(f);
    }
    return id = h ? h : 1;
}

/* ---- the channels on the socket ---- */
static void send_ch(Lobby *l, int ch, int to, const void *data, int len)
{
    uint8_t buf[2600];
    if (len + 1 > (int)sizeof buf) return;
    buf[0] = (uint8_t)ch;
    memcpy(buf + 1, data, (size_t)len);
    udp_send(l->udp, to, buf, len + 1);
}
static void lobby_send(void *ctx, int to, const void *data, int len) { send_ch(ctx, CH_LOBBY, to, data, len); }
static void match_send(void *ctx, int to, const void *data, int len) { send_ch(ctx, CH_MATCH, to, data, len); }
NetTransport lobby_match_transport(Lobby *l) { NetTransport t = {l, match_send}; return t; }
void lobby_on_match(Lobby *l, void (*fn)(void *, int, const uint8_t *, int, uint32_t), void *ctx) { l->match = fn; l->match_ctx = ctx; }

/* ---- the lobby's messages ---- */
static uint8_t *put_player(uint8_t *w, const LobbyPlayer *p)
{
    memcpy(w, p->name, LOBBY_NAME);
    w[LOBBY_NAME] = (uint8_t)p->car;
    w[LOBBY_NAME + 1] = (uint8_t)p->team;
    w[LOBBY_NAME + 2] = p->ready;
    w[LOBBY_NAME + 3] = (uint8_t)p->vote;
    w[LOBBY_NAME + 4] = p->vote_round;
    memcpy(w + LOBBY_NAME + 5, p->table, 128);
    memcpy(w + LOBBY_NAME + 5 + 128, p->online_id, 8);
    w[LOBBY_NAME + 5 + 128 + 8] = (uint8_t)p->skin;
    w[LOBBY_NAME + 5 + 128 + 9] = (uint8_t)p->picture;
    w[LOBBY_NAME + 5 + 128 + 10] = (uint8_t)((uint16_t)p->picture >> 8);
    return w + PLAYER_BYTES;
}
static const uint8_t *get_player(const uint8_t *r, LobbyPlayer *p)
{
    memcpy(p->name, r, LOBBY_NAME);
    p->name[LOBBY_NAME - 1] = 0;
    p->car = (int8_t)r[LOBBY_NAME];
    p->team = (int8_t)r[LOBBY_NAME + 1];
    p->ready = r[LOBBY_NAME + 2] != 0;
    p->vote = (int8_t)r[LOBBY_NAME + 3];
    p->vote_round = r[LOBBY_NAME + 4];
    memcpy(p->table, r + LOBBY_NAME + 5, 128);
    memcpy(p->online_id, r + LOBBY_NAME + 5 + 128, 8);
    p->skin = (int8_t)r[LOBBY_NAME + 5 + 128 + 8];
    p->picture = (int16_t)(r[LOBBY_NAME + 5 + 128 + 9] | r[LOBBY_NAME + 5 + 128 + 10] << 8);
    return r + PLAYER_BYTES;
}
static uint8_t *put32(uint8_t *w, uint32_t v) { w[0] = (uint8_t)v; w[1] = (uint8_t)(v >> 8); w[2] = (uint8_t)(v >> 16); w[3] = (uint8_t)(v >> 24); return w + 4; }
static uint32_t get32(const uint8_t **r) { const uint8_t *p = *r; *r += 4; return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void master_punch(Lobby *l, const char *addr, uint16_t port); /* (below) */
static void master_send(Lobby *l, const uint8_t *m, int len);         /* (below) */
static void get_where(const uint8_t *r, LobbyMachine *mc); /* (below) */
static void send_state(Lobby *l) /* (host) the whole lobby to every machine */
{
    static uint8_t m[SES_APP_MAX];
    uint8_t *w = m;
    l->view.seq++;
    *w++ = L_STATE;
    w = put32(w, l->view.seq);
    memcpy(w, l->view.server, sizeof l->view.server);
    w += sizeof l->view.server;
    w = put32(w, (uint32_t)l->view.settings_len);
    memcpy(w, l->view.settings, (size_t)l->view.settings_len);
    w += l->view.settings_len;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        const LobbyMachine *mc = &l->view.m[n];
        *w++ = mc->present;
        *w++ = mc->count;
        for (int i = 0; i < mc->count; i++) w = put_player(w, &mc->p[i]);
        memcpy(w, mc->addr, 16);
        w += 16;
        *w++ = (uint8_t)mc->port;
        *w++ = (uint8_t)(mc->port >> 8);
        w = put32(w, mc->id);
        for (int i = 0; i < 2; i++) *w++ = (uint8_t)mc->region[i];
        for (int i = 0; i < 3; i++) *w++ = (uint8_t)mc->plat[i];
        *w++ = mc->spectate;
    }
    *w++ = l->view.match_on;
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != l->node && l->view.m[n].present) ses_app_send(l->ses, n, m, (int)(w - m));
    l->dirty = 0;
    l->sent_ms = l->now;
}
static void got_state(Lobby *l, const uint8_t *r, int len) /* (a machine) the host's lobby */
{
    const uint8_t *end = r + len;
    if (len < 4 + 32 + 4) return;
    LobbyView v = l->view; /* (starts kept) */
    v.seq = get32(&r);
    memcpy(v.server, r, sizeof v.server);
    v.server[sizeof v.server - 1] = 0;
    r += sizeof v.server;
    v.settings_len = (int)get32(&r);
    if (v.settings_len < 0 || v.settings_len > LOBBY_SETTINGS_MAX || end - r < v.settings_len) return;
    memcpy(v.settings, r, (size_t)v.settings_len);
    r += v.settings_len;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        if (end - r < 2) return;
        LobbyMachine *mc = &v.m[n];
        uint16_t ping = mc->ping; /* (the pings come on their own: kept) */
        memset(mc, 0, sizeof *mc);
        mc->ping = ping;
        mc->present = *r++;
        mc->count = *r++;
        if (mc->count > LOBBY_LOCAL_MAX || end - r < mc->count * PLAYER_BYTES + 28) return;
        for (int i = 0; i < mc->count; i++) r = get_player(r, &mc->p[i]);
        memcpy(mc->addr, r, 16);
        mc->addr[15] = 0;
        r += 16;
        mc->port = (uint16_t)(r[0] | r[1] << 8);
        r += 2;
        mc->id = get32(&r);
        get_where(r, mc);
        r += 5;
        mc->spectate = *r++;
    }
    v.match_on = end - r >= 1 ? *r++ : 0;
    if (getenv("TM2PC_NET_DEBUG") && v.match_on != l->view.match_on) fprintf(stderr, "lobby: the host in a match: %d (%d bytes left)\n", v.match_on, (int)(end - r));
    for (int n = 0; n < SES_MAX_NODES; n++) /* (every other machine's address: any may become the host) */
        if (n != l->node && n != l->host_node && v.m[n].present && v.m[n].addr[0] && !udp_node_known(l->udp, n)) {
            udp_set_node(l->udp, n, v.m[n].addr, v.m[n].port);
            master_punch(l, v.m[n].addr, v.m[n].port);
        }
    l->view = v;
    if (l->status == LOBBY_CONNECTING) l->status = LOBBY_IN;
}
/* ---- where a machine plays from: a continent, never more (the lobby cards, the user's 2026-11-25) ----
 * The country of Windows' region setting (GetUserGeoID: what the player chose, no network lookup), as its continent. */
static const char *const continents[6][2] = {
    {"NA", "US CA MX GT BZ SV HN NI CR PA CU JM HT DO BS BB TT AG DM GD KN LC VC PR VI VG AI MS KY TC BM GL PM AW CW SX BQ "
           "MQ GP BL MF UM"},
    {"SA", "BR AR CL PE CO VE EC BO PY UY GY SR GF FK GS"},
    {"EU", "GB IE FR DE NL BE LU CH AT IT ES PT AD MC SM VA MT DK NO SE FI IS FO AX PL CZ SK HU SI HR BA RS ME MK AL GR BG "
           "RO MD UA BY LT LV EE RU XK LI GI GG JE IM SJ"},
    {"AF", "EG LY TN DZ MA EH SD SS ET ER DJ SO KE UG TZ RW BI CD CG CF CM TD NE NG BJ TG GH CI LR SL GN GW SN GM ML BF MR "
           "CV ST GQ GA AO ZM ZW MW MZ MG MU SC KM YT RE NA BW ZA LS SZ SH IO"},
    {"AS", "CN JP KR KP TW HK MO MN IN PK BD LK NP BT MV AF IR IQ SY LB JO IL PS SA YE OM AE QA BH KW TR CY GE AM AZ KZ UZ "
           "TM KG TJ TH VN LA KH MM MY SG ID PH BN TL"},
    {"OC", "AU NZ PG FJ SB VU NC PF WS TO TV KI NR MH FM PW CK NU TK WF AS GU MP NF PN CC CX HM"},
};
const char *lobby_region(void)
{
    static char got[3];
    static int done;
    if (done) return got;
    done = 1;
    const char *test = getenv("TM2PC_TEST_REGION"); /* tests: a country's 2 letters */
    char iso[8] = "";
#ifdef _WIN32
    if (test) snprintf(iso, sizeof iso, "%s", test);
    else {
        GEOID g = GetUserGeoID(GEOCLASS_NATION);
        if (g != GEOID_NOT_AVAILABLE) GetGeoInfoA(g, GEO_ISO2, iso, (int)sizeof iso, 0);
    }
#else
    if (test) snprintf(iso, sizeof iso, "%s", test);
#endif
    if (strlen(iso) != 2) return got;
    for (int c = 0; c < 6; c++)
        for (const char *s = continents[c][1]; *s; s += s[2] ? 3 : 2)
            if (s[0] == iso[0] && s[1] == iso[1]) { memcpy(got, continents[c][0], 3); return got; }
    return got;
}
const char *lobby_platform(void) { return "PC"; }
static uint8_t *put_where(uint8_t *w) /* (2 bytes the continent, 3 the platform) */
{
    const char *rg = lobby_region(), *pl = lobby_platform();
    for (int i = 0; i < 2; i++) *w++ = (uint8_t)rg[i < (int)strlen(rg) ? i : 2];
    for (int i = 0; i < 3; i++) *w++ = (uint8_t)(i < (int)strlen(pl) ? pl[i] : 0);
    return w;
}
static void get_where(const uint8_t *r, LobbyMachine *mc)
{
    for (int i = 0; i < 2; i++) mc->region[i] = (r[i] >= 'A' && r[i] <= 'Z') ? (char)r[i] : 0;
    mc->region[2] = 0;
    if (!mc->region[0] || !mc->region[1]) mc->region[0] = 0;
    for (int i = 0; i < 3; i++) mc->plat[i] = (r[2 + i] >= 'A' && r[2 + i] <= 'Z') ? (char)r[2 + i] : 0;
    mc->plat[3] = 0;
}
static void send_info(Lobby *l) /* (a machine) its players, once in */
{
    uint8_t m[2 + LOBBY_LOCAL_MAX * PLAYER_BYTES + 6], *w = m;
    *w++ = L_INFO;
    *w++ = (uint8_t)l->count;
    for (int i = 0; i < l->count; i++) w = put_player(w, &l->mine[i]);
    w = put_where(w); /* (where it plays from) */
    *w++ = (uint8_t)l->spectate; /* (which of its players only watch) */
    ses_app_send(l->ses, l->host_node, m, (int)(w - m));
}
/* ---- the vote to go back to the lobby (a match on; the user's 2026-11-25) ----
 * Any machine calls it (its players' vote: yes); every other machine yes or no; more than half the match's machines
 * yes: passed (the host's game ends the match at one step on every machine: net/game_tm2.c); half or more no, or 30 s:
 * failed. The result shown a few seconds, then none. One vote a machine (as the kick vote's). */
enum { BACK_SECS = 30, BACK_SHOWN_MS = 3000 };
static void back_send(Lobby *l)
{
    uint8_t m[9] = {L_BACK_STATE, l->back.round, l->back.caller, l->back.yes, l->back.no, l->back.needed, l->back.state,
                    l->back.total, l->back.secs};
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != l->node && l->view.m[n].present) ses_app_send(l->ses, n, m, (int)sizeof m);
}
static void back_tally(Lobby *l)
{
    int total = 0, yes = 0, no = 0;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        if (!l->view.m[n].present || !l->view.m[n].count) continue;
        total++;
        yes += l->back.vote[n] == 1, no += l->back.vote[n] == 2;
    }
    l->back.yes = (uint8_t)yes, l->back.no = (uint8_t)no, l->back.needed = (uint8_t)(total / 2 + 1), l->back.total = (uint8_t)total;
    uint32_t gone = l->now - l->back.at;
    l->back.secs = (uint8_t)(gone >= BACK_SECS * 1000u ? 0 : (BACK_SECS * 1000u - gone + 999) / 1000);
    if (l->back.state == LOBBY_BACK_RUNNING) {
        if (yes >= l->back.needed) l->back.state = LOBBY_BACK_PASSED;
        else if (no * 2 >= total || l->now - l->back.at >= BACK_SECS * 1000u) l->back.state = LOBBY_BACK_FAILED;
        if (l->back.state != LOBBY_BACK_RUNNING) {
            l->back.ended = l->now ? l->now : 1;
            fprintf(stderr, "lobby: the vote to go back to the lobby %s (%d yes, %d no of %d)\n",
                    l->back.state == LOBBY_BACK_PASSED ? "passed" : "failed", yes, no, total);
        }
    }
    back_send(l);
}
static void back_take(Lobby *l, int node, int round, int choice) /* (host) a machine's call or vote */
{
    if (node < 0 || node >= SES_MAX_NODES || (choice != 1 && choice != 2)) return;
    if (l->back.state != LOBBY_BACK_RUNNING) { /* (none running: a yes calls one) */
        if (choice != 1 || (l->back.state != LOBBY_BACK_NONE && l->back.state != LOBBY_BACK_FAILED)) return;
        memset(l->back.vote, 0, sizeof l->back.vote);
        l->back.round++;
        if (!l->back.round) l->back.round = 1;
        l->back.caller = (uint8_t)node;
        l->back.state = LOBBY_BACK_RUNNING;
        l->back.at = l->now ? l->now : 1;
        fprintf(stderr, "lobby: machine %d called a vote to go back to the lobby\n", node);
    } else if (round != l->back.round || l->back.vote[node]) return; /* (another round's, or voted already) */
    l->back.vote[node] = (uint8_t)choice;
    back_tally(l);
}
void lobby_back_vote(Lobby *l, int yes)
{
    if (!l || l->node < 0) return;
    int round = l->back.state == LOBBY_BACK_RUNNING ? l->back.round : 0;
    l->back.mine_round = l->back.state == LOBBY_BACK_RUNNING ? l->back.round : 0; /* (a call: the caller's, below) */
    if (l->host) back_take(l, l->node, round, yes ? 1 : 2);
    else {
        uint8_t m[3] = {L_BACK, (uint8_t)round, (uint8_t)(yes ? 1 : 2)};
        ses_app_send(l->ses, l->host_node, m, (int)sizeof m);
    }
}
int lobby_back_counts(const Lobby *l, int *total, int *secs)
{
    if (!l || l->back.state == LOBBY_BACK_NONE) return 0;
    if (total) *total = l->back.total;
    if (secs) *secs = l->back.secs;
    return 1;
}
int lobby_back_progress(const Lobby *l, int *caller, int *yes, int *no, int *needed, int *mine)
{
    if (!l || l->back.state == LOBBY_BACK_NONE) return LOBBY_BACK_NONE;
    if (caller) *caller = l->back.caller;
    if (yes) *yes = l->back.yes;
    if (no) *no = l->back.no;
    if (needed) *needed = l->back.needed;
    if (mine) *mine = l->back.mine_round == l->back.round || l->back.caller == l->node;
    return l->back.state;
}
void lobby_back_clear(Lobby *l) /* (the match over, back in the lobby: none) */
{
    if (!l) return;
    l->back.state = LOBBY_BACK_NONE;
    memset(l->back.vote, 0, sizeof l->back.vote);
    if (l->host) back_send(l);
}
static void back_tick(Lobby *l) /* (host) its time out; a result shown a while, then none */
{
    if (!l->host) return;
    if (l->back.state == LOBBY_BACK_RUNNING) {
        uint32_t gone = l->now - l->back.at; /* (each second: its countdown to every machine) */
        uint8_t secs = (uint8_t)(gone >= BACK_SECS * 1000u ? 0 : (BACK_SECS * 1000u - gone + 999) / 1000);
        if (gone >= BACK_SECS * 1000u || secs != l->back.secs) back_tally(l);
    } else if (l->back.state == LOBBY_BACK_FAILED && l->now - l->back.ended >= BACK_SHOWN_MS) {
        l->back.state = LOBBY_BACK_NONE;
        back_send(l);
    }
}
static void on_app(void *ctx, int from, const uint8_t *d, int len)
{
    Lobby *l = ctx;
    if (len < 1) return;
    const uint8_t *r = d + 1;
    if (l->host && d[0] == L_INFO && len >= 2 && from != l->node && from < SES_MAX_NODES) {
        int count = d[1] > LOBBY_LOCAL_MAX ? LOBBY_LOCAL_MAX : d[1];
        if (len < 2 + count * PLAYER_BYTES) return;
        LobbyMachine *mc = &l->view.m[from];
        memset(mc, 0, sizeof *mc);
        mc->present = 1;
        mc->count = (uint8_t)count;
        r = d + 2;
        for (int i = 0; i < count; i++) r = get_player(r, &mc->p[i]);
        if (len >= 2 + count * PLAYER_BYTES + 5) get_where(r, mc);
        if (len >= 2 + count * PLAYER_BYTES + 6) mc->spectate = r[5];
        l->dirty = 1;
    } else if (l->host && d[0] == L_SET && len >= 5 && from != l->node && from < SES_MAX_NODES) {
        LobbyMachine *mc = &l->view.m[from];
        int i = d[1];
        if (i >= mc->count) return;
        mc->p[i].car = (int8_t)d[2];
        mc->p[i].team = (int8_t)d[3];
        mc->p[i].ready = d[4] != 0;
        l->dirty = 1;
    } else if (l->host && d[0] == L_SKIN && len >= 3 && from != l->node && from < SES_MAX_NODES) {
        LobbyMachine *mc = &l->view.m[from];
        if (d[1] >= mc->count) return;
        mc->p[d[1]].skin = (int8_t)d[2];
        l->dirty = 1;
    } else if (l->host && d[0] == L_BACK && len >= 3 && from != l->node && from < SES_MAX_NODES) {
        back_take(l, from, d[1], d[2]);
    } else if (!l->host && from == l->host_node && d[0] == L_BACK_STATE && len >= 7) {
        l->back.round = d[1], l->back.caller = d[2], l->back.yes = d[3], l->back.no = d[4], l->back.needed = d[5];
        l->back.state = d[6];
        if (len >= 9) l->back.total = d[7], l->back.secs = d[8];
    } else if (d[0] >= LOBBY_EXT_FIRST) {
        if (l->ext) l->ext(l->ext_ctx, from, d, len);
    } else if (l->host && d[0] == L_TABLE && len >= 2 + 128 && from != l->node && from < SES_MAX_NODES) {
        LobbyMachine *mc = &l->view.m[from];
        if (d[1] >= mc->count) return;
        memcpy(mc->p[d[1]].table, d + 2, 128);
        l->dirty = 1;
    } else if (l->host && d[0] == L_VOTE && len >= 4 && from != l->node && from < SES_MAX_NODES) {
        LobbyMachine *mc = &l->view.m[from];
        int i = d[1];
        if (i >= mc->count) return;
        mc->p[i].vote_round = d[2];
        mc->p[i].vote = (int8_t)d[3];
        l->dirty = 1;
    } else if (!l->host && from == l->host_node && d[0] == L_STATE) {
        got_state(l, r, len - 1);
    } else if (!l->host && from == l->host_node && d[0] == L_PINGS && len >= 1 + 2 * SES_MAX_NODES) {
        for (int n = 0; n < SES_MAX_NODES; n++) l->view.m[n].ping = (uint16_t)(r[2 * n] | r[2 * n + 1] << 8);
    } else if (!l->host && from == l->host_node && d[0] == L_REJOIN && len >= 2 + 4 + 2 + 3 * SES_MAX_NODES) {
        int target = *r++; /* (the start with a machine back in it: everyone's; that machine's to go in) */
        LobbyStart s;
        s.seed = get32(&r);
        s.delay = *r++;
        s.humans = *r++;
        memcpy(s.first, r, SES_MAX_NODES);
        memcpy(s.car, r + SES_MAX_NODES, SES_MAX_NODES);
        memcpy(s.skin, r + 2 * SES_MAX_NODES, SES_MAX_NODES);
        r += 3 * SES_MAX_NODES;
        s.spec = len >= 2 + 4 + 2 + 3 * SES_MAX_NODES + 4 ? get32(&r) : 0;
        l->view.start = s;
        if (target == l->node) { l->rejoin = 1; l->view.starts++; }
    } else if (!l->host && from == l->host_node && d[0] == L_START && len >= 1 + 4 + 2 + 3 * SES_MAX_NODES) {
        LobbyStart s;
        s.seed = get32(&r);
        s.delay = *r++;
        s.humans = *r++;
        memcpy(s.first, r, SES_MAX_NODES);
        memcpy(s.car, r + SES_MAX_NODES, SES_MAX_NODES);
        memcpy(s.skin, r + 2 * SES_MAX_NODES, SES_MAX_NODES);
        r += 3 * SES_MAX_NODES;
        s.spec = len >= 1 + 4 + 2 + 3 * SES_MAX_NODES + 4 ? get32(&r) : 0;
        l->view.start = s;
        l->view.starts++;
    }
}
static void on_chat(void *ctx, int speaker, const char *text)
{
    Lobby *l = ctx;
    if (l->chat_n == LOBBY_CHAT_LINES) { memmove(&l->chat[0], &l->chat[1], sizeof l->chat[0] * (LOBBY_CHAT_LINES - 1)); l->chat_n--; }
    l->chat[l->chat_n].speaker = speaker;
    snprintf(l->chat[l->chat_n].text, sizeof l->chat[0].text, "%s", text);
    l->chat_n++;
    l->chat_total++;
}
static void on_kicked(void *ctx) { ((Lobby *)ctx)->status = LOBBY_KICKED; }
static void on_voice(void *ctx, int speaker, uint16_t seq, const uint8_t *data, int len)
{
    Lobby *l = ctx;
    if (l->voice) l->voice(l->voice_ctx, speaker, seq, data, len);
}
static void on_kick(void *ctx, int target, int yes, int needed, int state)
{
    Lobby *l = ctx;
    l->kick.target = target;
    l->kick.yes = yes;
    l->kick.needed = needed;
    l->kick.state = state;
    l->kick.at = l->now ? l->now : 1;
}

/* the lobby's session keeps no game state (idle: no steps) */
static void nop_step(void *ctx, int32_t f) { (void)ctx; (void)f; }
static void nop_advance(void *ctx, const RbInput *in) { (void)ctx; (void)in; }
static uint32_t nop_sum(void *ctx) { (void)ctx; return 0; }
static int nop_ser(void *ctx, int32_t f, uint8_t *o, int cap) { (void)ctx; (void)f; (void)o; (void)cap; return 0; }
static void nop_des(void *ctx, const uint8_t *in, int len) { (void)ctx; (void)in; (void)len; }
static Session *make_session(Lobby *l, uint32_t now_ms)
{
    NetTransport t = {l, lobby_send};
    NetGame g;
    memset(&g, 0, sizeof g);
    g.rb.ctx = l;
    g.rb.save = nop_step;
    g.rb.load = nop_step;
    g.rb.advance = nop_advance;
    g.rb.checksum = nop_sum;
    g.serialize = nop_ser;
    g.deserialize = nop_des;
    g.state_max = 64;
    Session *s;
    if (l->host) {
        uint32_t slots[SES_MAX_NODES] = {1u};
        s = ses_new(0, 0, 1u, slots, &t, &g, 2);
        if (s) ses_host_offer_slots(s, ~1u); /* (a slot each: the lobby's session only counts machines) */
    } else {
        s = ses_join(l->node, l->host_node, &t, &g, 2, now_ms);
    }
    if (!s) return NULL;
    ses_set_idle(s, 1);
    SesEvents ev;
    memset(&ev, 0, sizeof ev);
    ev.ctx = l;
    ev.chat = on_chat;
    ev.kicked = on_kicked;
    ev.kick = on_kick;
    ses_on_voice(s, on_voice, l);
    ev.app = on_app;
    ses_on_events(s, &ev);
    return s;
}

static Lobby *make(int host, int count, const char (*names)[LOBBY_NAME])
{
    Lobby *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->host = host;
    l->count = count < 1 ? 1 : count > LOBBY_LOCAL_MAX ? LOBBY_LOCAL_MAX : count;
    for (int i = 0; i < l->count; i++) {
        snprintf(l->mine[i].name, LOBBY_NAME, "%s", names ? names[i] : "Player");
        l->mine[i].car = -1;
        l->mine[i].team = -1;
        l->mine[i].vote = -1;
        l->mine[i].skin = -1;
        l->mine[i].picture = -1;
    }
    return l;
}

Lobby *lobby_host(uint16_t port, int loopback, const char *server, int count, const char (*names)[LOBBY_NAME])
{
    Lobby *l = make(1, count, names);
    if (!l) return NULL;
    l->loopback = loopback;
    l->udp = udp_open(port, loopback ? UDP_LOOPBACK : 0);
    if (!l->udp || !(l->ses = make_session(l, 0))) { lobby_free(l); return NULL; }
    l->node = 0;
    l->status = LOBBY_IN;
    snprintf(l->view.server, sizeof l->view.server, "%s", server ? server : "");
    l->view.m[0].present = 1;
    l->view.m[0].count = (uint8_t)l->count;
    l->lan = udp_open(lan_port(), loopback ? UDP_LOOPBACK : 0); /* (taken: another server on this PC answers; no listing) */
    l->listing.port = port;
    snprintf(l->listing.name, sizeof l->listing.name, "%s", server ? server : "");
    snprintf(l->listing.host, sizeof l->listing.host, "%s", l->mine[0].name);
    memcpy(l->view.m[0].p, l->mine, sizeof l->mine);
    l->was_present[0] = 1;
    return l;
}

Lobby *lobby_join(const char *host, uint16_t port, int loopback, int count, const char (*names)[LOBBY_NAME])
{
    Lobby *l = make(0, count, names);
    if (!l) return NULL;
    l->node = -1;
    l->loopback = loopback;
    l->status = LOBBY_CONNECTING;
    l->spectate = getenv("TM2PC_TEST_SPECTATE") ? (int)strtol(getenv("TM2PC_TEST_SPECTATE"), NULL, 0) : 0; /* tests: this
                                    * machine's players spectators from the start (1: all of them; a mask: 2 player 2's) */
    if (getenv("TM2PC_TEST_SPECTATE") && l->spectate == 1) l->spectate = 0xFF;
    l->udp = udp_open(0, loopback ? UDP_LOOPBACK : 0);
    if (!l->udp || !udp_set_node(l->udp, 0, host, port)) { lobby_free(l); return NULL; }
    return l;
}

void lobby_free(Lobby *l)
{
    if (!l) return;
    static const uint8_t bye[2] = {MASTER_CHANNEL, MASTER_BYE};
    if (l->udp) master_send(l, bye, 2); /* (off the master's list at once) */
    ses_bye(l->ses); /* (the server's machines: this one gone now) */
    ses_free(l->ses);
    udp_close(l->udp);
    udp_close(l->lan);
    free(l);
}

int lobby_status(const Lobby *l) { return l->status; }
int lobby_node(const Lobby *l) { return l->node; }
int lobby_is_host(const Lobby *l) { return l->host; }
uint16_t lobby_port(const Lobby *l) { return udp_port(l->udp); }
int lobby_host_node(const Lobby *l) { return l->host_node; }
uint32_t lobby_migrated_at(const Lobby *l, int *new_host)
{
    if (new_host) *new_host = l->host_node;
    return l->migrated_ms;
}
const LobbyView *lobby_view(const Lobby *l) { return &l->view; }

/* ---- the handshake ---- */
static void hello_send(Lobby *l)
{
    uint8_t m[14 + LOBBY_PASSWORD];
    memcpy(m, MAGIC, 4);
    m[4] = HELLO;
    put32(m + 5, lobby_build_id());
    m[9] = (uint8_t)l->count;
    put32(m + 10, lobby_machine_id()); /* (this machine's own number: known again if it drops and comes back) */
    memcpy(m + 14, l->password, LOBBY_PASSWORD); /* (a private server's: the one typed) */
    send_ch(l, CH_HELLO, 0, m, (int)sizeof m);
}
static int banned(const Lobby *l, const char *addr)
{
    for (int i = 0; i < l->banned_n; i++) if (!strcmp(l->banned[i], addr)) return 1;
    return 0;
}
static void host_hello(Lobby *l, int from, const char *addr, uint16_t port, const uint8_t *d, int len)
{
    if (len < 10 || memcmp(d, MAGIC, 4) || d[4] != HELLO) return;
    const uint8_t *r = d + 5;
    uint32_t id = get32(&r);
    uint8_t reply[8];
    memcpy(reply, MAGIC, 4);
    int node = from;
    int wrong_password = l->password[0] && (len < 14 + LOBBY_PASSWORD || strncmp((const char *)d + 14, l->password, LOBBY_PASSWORD));
    if (id != lobby_build_id() || banned(l, addr) || wrong_password) {
        reply[4] = REFUSED;
        reply[5] = id != lobby_build_id() ? 1 : banned(l, addr) ? 3 : 4;
    } else {
        int players = 0, asking = d[9] ? d[9] : 1;
        for (int k = 0; k < SES_MAX_NODES; k++) if (l->view.m[k].present && k != from) players += l->view.m[k].count;
        if (node < 0 && l->max_players && players + asking > l->max_players) node = -2; /* (full: each player one of the room) */
        if (node == -1) { /* a new machine: the next node free after the one given last, so the nodes (the lobby's order,
                           * the match's players) follow the order the machines came in (the user's: a newcomer had the
                           * place of one who left, in the middle); round to 1 at the end, node 0 last (its host's) */
            for (int k = 1; k <= SES_MAX_NODES && node < 0; k++) {
                int n = (l->last_node + k) % SES_MAX_NODES;
                if (n == 0) continue;
                if (n != l->node && !ses_present(l->ses, n) && !l->pending_ms[n]) node = n;
            }
            if (node < 0 && l->node != 0 && !ses_present(l->ses, 0) && !l->pending_ms[0]) node = 0;
            if (node >= 0) l->last_node = node;
            if (node >= 0) {
                udp_set_node(l->udp, node, addr, port);
                l->pending_ms[node] = l->now ? l->now : 1;
                const uint8_t *ri = d + 10;
                l->pending_id[node] = len >= 14 ? get32(&ri) : 0; /* (its number: the machine's when it comes in) */
            }
        }
        if (node < -1) node = -1;
        reply[4] = node >= 0 ? WELCOME : REFUSED;
        reply[5] = node >= 0 ? (uint8_t)node : 2;
    }
    reply[6] = (uint8_t)l->node; /* (the host's node: another than 0 once a host left) */
    /* (to that address: a refused machine has no node) */
    uint8_t buf[9];
    buf[0] = CH_HELLO;
    memcpy(buf + 1, reply, 7);
    udp_send_to(l->udp, addr, port, buf, 8);
}
static void client_hello(Lobby *l, const uint8_t *d, int len)
{
    if (len < 6 || memcmp(d, MAGIC, 4) || l->node >= 0 || l->status != LOBBY_CONNECTING) return;
    if (d[4] == REFUSED) {
        l->status = d[5] == 1 ? LOBBY_REFUSED_BUILD : d[5] == 3 ? LOBBY_REFUSED_BANNED : d[5] == 4 ? LOBBY_REFUSED_PASSWORD
                  : LOBBY_REFUSED_FULL;
        return;
    }
    int hn = len >= 7 ? d[6] : 0;
    if (d[4] != WELCOME || d[5] == hn || d[5] >= SES_MAX_NODES || hn >= SES_MAX_NODES) return;
    if (hn != 0) { /* (the host there is another node than 0: a host left before; its address under its node) */
        char a[48];
        if (!udp_node_addr(l->udp, 0, a)) return;
        uint16_t p = udp_node_port(l->udp, 0);
        udp_forget_node(l->udp, 0);
        udp_set_node(l->udp, hn, a, p);
    }
    l->host_node = hn;
    l->node = d[5];
    if (!(l->ses = make_session(l, l->now))) { l->status = LOBBY_LOST; return; }
    send_info(l);
}

/* ---- LAN: the browser's question, the host's listing ---- */
static int put_listing(uint8_t *w, const LobbyListing *s)
{
    uint8_t *w0 = w;
    memcpy(w, ANSWER, 4); w += 4;
    w = put32(w, lobby_build_id());
    memcpy(w, s->name, 32); w += 32;
    memcpy(w, s->host, 24); w += 24;
    memcpy(w, s->type, 32); w += 32;
    *w++ = (uint8_t)s->port; *w++ = (uint8_t)(s->port >> 8);
    *w++ = s->players; *w++ = s->max; *w++ = s->game; *w++ = s->arena; *w++ = s->mode; *w++ = s->playing;
    *w++ = s->mods;
    memcpy(w, s->mod_names, 48); w += 48;
    *w++ = s->locked;
    return (int)(w - w0);
}
static int get_listing(const uint8_t *r, int len, LobbyListing *s)
{
    if (len < LISTING_BYTES || memcmp(r, ANSWER, 4)) return 0;
    r += 4;
    s->same_build = get32(&r) == lobby_build_id();
    memcpy(s->name, r, 32); s->name[31] = 0; r += 32;
    memcpy(s->host, r, 24); s->host[23] = 0; r += 24;
    memcpy(s->type, r, 32); s->type[31] = 0; r += 32;
    s->port = (uint16_t)(r[0] | r[1] << 8); r += 2;
    s->players = *r++; s->max = *r++; s->game = *r++; s->arena = *r++; s->mode = *r++; s->playing = *r++;
    if (len >= LISTING_MODS) { s->mods = *r++; memcpy(s->mod_names, r, 48); s->mod_names[47] = 0; r += 48; }
    if (len >= LISTING_LOCKED) s->locked = *r++;
    return 1;
}
void lobby_set_password(Lobby *l, const char *password)
{
    if (!l) return;
    memset(l->password, 0, sizeof l->password);
    snprintf(l->password, sizeof l->password, "%s", password ? password : "");
}
void lobby_set_listing(Lobby *l, const LobbyListing *info)
{
    uint16_t port = l->listing.port;
    l->listing = *info;
    l->listing.port = port;
}
static int listing_now(const Lobby *l, uint8_t *out) /* (host) the listing, its players as they are now */
{
    LobbyListing s = l->listing;
    int players = 0;
    for (int k = 0; k < SES_MAX_NODES; k++) if (l->view.m[k].present) players += l->view.m[k].count;
    s.players = (uint8_t)players;
    s.locked = l->password[0] != 0;
    return put_listing(out, &s);
}
static void master_send(Lobby *l, const uint8_t *m, int len) /* to every master (each keeps the list on its own) */
{
    for (int i = 0; i < l->masters; i++) udp_send_to(l->udp, l->master[i], l->master_port[i], m, len);
}
static void master_punch(Lobby *l, const char *addr, uint16_t port) /* the master: introduce this machine and that one */
{
    uint8_t m[2 + MASTER_ADDR_BYTES] = {MASTER_CHANNEL, MASTER_PUNCH};
    master_put_addr(m + 2, addr, port);
    master_send(l, m, (int)sizeof m);
}
int masters_parse(const char *list, char addr[][48], uint16_t port[], int max) /* "host:port, host" -> each */
{
    int n = 0;
    for (const char *p = list ? list : ""; *p && n < max;) {
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        size_t len = strcspn(p, ",; ");
        if (!len) break;
        char one[64];
        snprintf(one, sizeof one, "%.*s", (int)(len < sizeof one ? len : sizeof one - 1), p);
        p += len;
        char *colon = strrchr(one, ':');
        port[n] = MASTER_PORT;
        if (colon) { *colon = 0; port[n] = (uint16_t)atoi(colon + 1); }
        if (one[0]) snprintf(addr[n++], 48, "%s", one);
    }
    return n;
}
void lobby_set_masters(Lobby *l, const char *list)
{
    l->masters = masters_parse(list, l->master, l->master_port, MASTERS_MAX);
    l->master_ms = 0;
    for (int i = 0; i < l->masters; i++) udp_add_relay(l->udp, l->master[i], l->master_port[i]); /* (their relayed packets) */
    if (l->masters && !l->host && getenv("TM2PC_NET_FORCE_RELAY")) { /* tests: through the relay from the start */
        char a[48];
        if (udp_node_addr(l->udp, 0, a)) udp_relay_to(l->udp, a, udp_node_port(l->udp, 0));
    }
}
static void master_tick(Lobby *l) /* (an internet server) the host's listing, the others' keep-alive, now and then */
{
    if (!l->masters || (l->master_ms && l->now - l->master_ms < MASTER_REGISTER_MS)) return;
    l->master_ms = l->now ? l->now : 1;
    uint8_t m[2 + 160] = {MASTER_CHANNEL, MASTER_REGISTER};
    if (l->host) master_send(l, m, 2 + listing_now(l, m + 2));
    else if (l->status == LOBBY_IN) { m[1] = MASTER_KEEP; master_send(l, m, 2); }
    /* this machine's profile online, where it is (friends see the server): from this socket, the master's view of it */
    uint8_t id[128];
    char host[48] = "";
    if (!l->host) udp_node_addr(l->udp, l->host_node, host);
    int len = friends_id_packet(id, l->host ? 1 : 2, host, l->host ? 0 : udp_node_port(l->udp, l->host_node));
    if (len && (l->host || l->status == LOBBY_IN)) master_send(l, id, len);
}
static void master_packet(Lobby *l, const uint8_t *d, int len) /* the master's INTRO: a few PROBEs open the way */
{
    if (len >= 2 && d[1] == MASTER_FRIENDS) { friends_packet(d, len); return; } /* (the friends page's, also in a server) */
    if (len < 2 + MASTER_ADDR_BYTES || d[1] != MASTER_INTRO) return; /* (a PROBE: nothing to do) */
    char addr[16];
    uint16_t port;
    master_get_addr(d + 2, addr, &port);
    static const uint8_t probe[2] = {MASTER_CHANNEL, MASTER_PROBE};
    for (int i = 0; i < 3; i++) udp_send_to(l->udp, addr, port, probe, 2);
}
static void lan_answer(Lobby *l)
{
    uint8_t buf[64], out[LISTING_LOCKED];
    char addr[48];
    uint16_t port;
    int from, n;
    while (l->lan && (n = udp_poll(l->lan, &from, addr, &port, buf, sizeof buf)) > 0) {
        if (n < 4 || memcmp(buf, ASK, 4)) continue;
        udp_send_to(l->lan, addr, port, out, listing_now(l, out));
    }
}
struct LanSearch {
    NetUdp *u;
    uint32_t t0;
    int n;
    LobbyListing found[32];
};
static uint32_t ms_now(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
#endif
}
LanSearch *lobby_lan_search(int loopback)
{
    LanSearch *s = calloc(1, sizeof *s);
    if (!s || !(s->u = udp_open(0, loopback ? UDP_LOOPBACK : UDP_BROADCAST))) { free(s); return NULL; }
    uint8_t ask[8];
    memcpy(ask, ASK, 4);
    put32(ask + 4, lobby_build_id());
    s->t0 = ms_now();
    if (!loopback) udp_broadcast(s->u, lan_port(), ask, 8);
    udp_send_to(s->u, "127.0.0.1", lan_port(), ask, 8); /* (a server on this PC: the broadcast may not come back to it) */
    return s;
}
LanSearch *lobby_master_search(const char *masters, int loopback)
{
    char addr[MASTERS_MAX][48];
    uint16_t port[MASTERS_MAX];
    int n = masters_parse(masters, addr, port, MASTERS_MAX);
    if (!n) return NULL;
    LanSearch *s = calloc(1, sizeof *s);
    if (!s || !(s->u = udp_open(0, loopback ? UDP_LOOPBACK : 0))) { free(s); return NULL; }
    uint8_t ask[6] = {MASTER_CHANNEL, MASTER_LIST};
    put32(ask + 2, lobby_build_id());
    s->t0 = ms_now();
    for (int i = 0; i < n; i++) udp_send_to(s->u, addr[i], port[i], ask, 6); /* (every master: a server on several, once) */
    return s;
}
static int addr_private(const char *a) /* a private network's address (10/8, 172.16/12, 192.168/16, 127/8) */
{
    unsigned b0 = 0, b1 = 0;
    if (sscanf(a, "%u.%u", &b0, &b1) != 2) return 0;
    return b0 == 10 || b0 == 127 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168);
}
int lobby_lan_results(LanSearch *s, LobbyListing *out, int max)
{
    if (!s) return 0;
    uint8_t buf[256];
    char addr[48];
    uint16_t port;
    int from, len;
    while ((len = udp_poll(s->u, &from, addr, &port, buf, sizeof buf)) > 0) {
        LobbyListing f;
        memset(&f, 0, sizeof f);
        if (len > 2 + MASTER_ADDR_BYTES && buf[0] == MASTER_CHANNEL && buf[1] == MASTER_ENTRY) { /* the master's: its public address */
            char at[16];
            uint16_t at_port;
            master_get_addr(buf + 2, at, &at_port);
            if (!get_listing(buf + 2 + MASTER_ADDR_BYTES, len - 2 - MASTER_ADDR_BYTES, &f)) continue;
            snprintf(f.addr, sizeof f.addr, "%s", at);
            f.port = at_port;
        } else {
            if (!get_listing(buf, len, &f)) continue;
            snprintf(f.addr, sizeof f.addr, "%s", addr);
        }
        f.ping = (int)(ms_now() - s->t0);
        int dup = 0; /* (a server heard twice: by the broadcast and on this PC, or from two masters) */
        for (int i = 0; i < s->n; i++) {
            LobbyListing *e = &s->found[i];
            if (e->port != f.port || strcmp(e->name, f.name) || strcmp(e->host, f.host)) continue;
            dup = 1;
            /* two masters' addresses: the public one. A master on the host's own network (the user's NAS) sees the host
             * through the router's loopback, as 192.168.0.1, and answered first: joining that, the router, failed */
            if (addr_private(e->addr) && !addr_private(f.addr)) { int ping = e->ping; *e = f; e->ping = ping; }
        }
        if (!dup && s->n < 32) s->found[s->n++] = f;
    }
    int n = s->n < max ? s->n : max < 0 ? 0 : max;
    if (out && n > 0) memcpy(out, s->found, sizeof *out * (size_t)n);
    return n;
}
void lobby_lan_search_free(LanSearch *s)
{
    if (!s) return;
    udp_close(s->u);
    free(s);
}

/* ---- each frame ---- */
void lobby_poll(Lobby *l, uint32_t now_ms)
{
    l->now = now_ms;
    back_tick(l);
    if (l->host) lan_answer(l);
    uint8_t buf[2600];
    char addr[48];
    uint16_t port;
    int from, n;
    while ((n = udp_poll(l->udp, &from, addr, &port, buf, sizeof buf)) > 0) {
        if (buf[0] == MASTER_CHANNEL) {
            master_packet(l, buf, n);
        } else if (buf[0] == CH_HELLO) {
            if (l->host) host_hello(l, from, addr, port, buf + 1, n - 1);
            else if (from == 0 && l->node < 0) client_hello(l, buf + 1, n - 1);
        } else if (from >= 0 && buf[0] == CH_LOBBY && l->ses) {
            ses_receive(l->ses, from, buf + 1, n - 1, now_ms);
        } else if (from >= 0 && buf[0] == CH_MATCH && l->match) {
            l->match(l->match_ctx, from, buf + 1, n - 1, now_ms);
        }
    }
    if (!l->host && l->status == LOBBY_CONNECTING && l->node < 0) { /* asking in */
        if (!l->hello_first_ms) l->hello_first_ms = now_ms ? now_ms : 1;
        if (now_ms - l->hello_first_ms > HELLO_GIVE_UP_MS) { l->status = LOBBY_LOST; return; }
        if (!l->hello_ms || now_ms - l->hello_ms >= HELLO_EVERY_MS) {
            l->hello_ms = now_ms ? now_ms : 1;
            char a[48];
            /* no answer a while (introductions too): the host's router lets nothing through; through the relay */
            if (l->masters && now_ms - l->hello_first_ms > RELAY_AFTER_MS && !udp_relayed(l->udp, 0) && udp_node_addr(l->udp, 0, a)) {
                udp_relay_to(l->udp, a, udp_node_port(l->udp, 0));
                fprintf(stderr, "lobby: no answer from %s directly: through the master's relay\n", a);
            }
            if (l->masters && udp_node_addr(l->udp, 0, a)) master_punch(l, a, udp_node_port(l->udp, 0)); /* (behind a router) */
            hello_send(l);
        }
        return;
    }
    master_tick(l);
    if (!l->ses) return;
    RbInput none[RB_MAX_SLOTS];
    memset(none, 0, sizeof none);
    ses_step(l->ses, none, now_ms); /* (idle: who is there) */
    if (l->host) {
        if (l->node >= 0 && l->node < SES_MAX_NODES && !l->view.m[l->node].plat[0]) { /* (the host's own: where it plays) */
            uint8_t wb[5];
            put_where(wb);
            get_where(wb, &l->view.m[l->node]);
            l->dirty = 1;
        }
        for (int n2 = 0; n2 < SES_MAX_NODES; n2++) {
            if (n2 == l->node) continue;
            int present = ses_present(l->ses, n2);
            if (present && !l->view.m[n2].addr[0]) { /* (its address for every machine: host migration) */
                if (udp_node_addr(l->udp, n2, l->view.m[n2].addr)) { l->view.m[n2].port = udp_node_port(l->udp, n2); l->dirty = 1; }
            }
            if (present && l->pending_ms[n2]) { l->view.m[n2].id = l->pending_id[n2]; l->dirty = 1; }
            if (present) l->pending_ms[n2] = 0;
            else if (l->pending_ms[n2] && now_ms - l->pending_ms[n2] > PENDING_MS) { /* given out, never came */
                l->pending_ms[n2] = 0;
                udp_forget_node(l->udp, n2);
            }
            if (l->was_present[n2] && !present) { /* left */
                memset(&l->view.m[n2], 0, sizeof l->view.m[n2]);
                udp_forget_node(l->udp, n2);
                l->dirty = 1;
            }
            l->was_present[n2] = present;
        }
        if (l->dirty && now_ms - l->sent_ms >= STATE_EVERY_MS) send_state(l);
        if (now_ms - l->pings_ms >= PINGS_EVERY_MS) { /* each machine's round trip to the host (the host's own: 0) */
            l->pings_ms = now_ms;
            uint8_t m[1 + 2 * SES_MAX_NODES];
            m[0] = L_PINGS;
            for (int n2 = 0; n2 < SES_MAX_NODES; n2++) {
                int ms = n2 == l->node ? 0 : ses_rtt(l->ses, n2);
                l->view.m[n2].ping = (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms);
                m[1 + 2 * n2] = (uint8_t)l->view.m[n2].ping;
                m[2 + 2 * n2] = (uint8_t)(l->view.m[n2].ping >> 8);
            }
            for (int n2 = 0; n2 < SES_MAX_NODES; n2++)
                if (n2 != l->node && l->view.m[n2].present) ses_app_send(l->ses, n2, m, (int)sizeof m);
        }
    } else if (l->status == LOBBY_IN || l->status == LOBBY_CONNECTING) {
        uint32_t watchers = 0; /* (the spectators' machines: the next host only if no player's is left, session.c) */
        for (int n2 = 0; n2 < SES_MAX_NODES; n2++)
            if (l->view.m[n2].present && lobby_machine_watches(&l->view.m[n2])) watchers |= 1u << n2;
        ses_set_watchers(l->ses, watchers);
        int h = ses_host(l->ses);
        if (h != l->host_node) { /* the host left: the machine there longest carries on as the host (session.c) */
            int old = l->host_node;
            memset(&l->view.m[old], 0, sizeof l->view.m[old]);
            l->host_node = h;
            l->migrated_ms = now_ms ? now_ms : 1;
            fprintf(stderr, "lobby: the host (machine %d) left; machine %d is the host now%s\n", old, h, h == l->node ? " (this one)" : "");
            if (h == l->node) { /* this machine: the host now */
                l->host = 1;
                l->view.m[l->node].present = 1;
                ses_host_offer_slots(l->ses, ~1u); /* (machines joining from now on: a slot each, as the first host's) */
                if (!l->lan) l->lan = udp_open(lan_port(), l->loopback ? UDP_LOOPBACK : 0);
                l->listing.port = udp_port(l->udp);
                snprintf(l->listing.name, sizeof l->listing.name, "%s", l->view.server);
                snprintf(l->listing.host, sizeof l->listing.host, "%s", l->mine[0].name);
                for (int n = 0; n < SES_MAX_NODES; n++) l->was_present[n] = l->view.m[n].present;
                l->dirty = 1;
            }
        }
    }
    ses_send(l->ses, now_ms);
}

/* ---- this machine's choices; the host's ---- */
static void spectate_changed(Lobby *l);
void lobby_set_spectate(Lobby *l, int on) { l->spectate = on ? 0xFF : 0; spectate_changed(l); }
void lobby_set_spectate_player(Lobby *l, int i, int on)
{
    if (i < 0 || i >= LOBBY_LOCAL_MAX) return;
    if (on) l->spectate |= 1 << i;
    else l->spectate &= ~(1 << i);
    spectate_changed(l);
}
int lobby_spectate_mask(const Lobby *l) { return l->spectate & ((1 << l->count) - 1); }
static void spectate_changed(Lobby *l)
{
    if (l->host) {
        if (l->node >= 0 && l->node < SES_MAX_NODES) l->view.m[l->node].spectate = (uint8_t)l->spectate;
        l->dirty = 1;
    } else if (l->ses && l->node >= 0) {
        if (l->node < SES_MAX_NODES) l->view.m[l->node].spectate = (uint8_t)l->spectate; /* (shown at once) */
        send_info(l);
    }
}
int lobby_spectating(const Lobby *l) { return l->count && (l->spectate & ((1 << l->count) - 1)) == (1 << l->count) - 1; }
void lobby_set_player(Lobby *l, int i, int car, int team, int ready)
{
    if (i < 0 || i >= l->count) return;
    LobbyPlayer *p = &l->mine[i];
    p->car = (int8_t)car;
    p->team = (int8_t)team;
    p->ready = ready != 0;
    if (l->host) {
        l->view.m[l->node].p[i] = *p;
        l->dirty = 1;
    } else if (l->ses && l->node >= 0) {
        uint8_t m[5] = {L_SET, (uint8_t)i, (uint8_t)car, (uint8_t)team, (uint8_t)(ready != 0)};
        ses_app_send(l->ses, l->host_node, m, 5);
        l->view.m[l->node].p[i] = *p; /* (shown at once; the host's lobby confirms it) */
    }
}

void lobby_set_skin(Lobby *l, int i, int skin)
{
    if (i < 0 || i >= l->count || l->mine[i].skin == (int8_t)skin) return;
    l->mine[i].skin = (int8_t)skin;
    if (l->node >= 0 && l->node < SES_MAX_NODES) l->view.m[l->node].p[i].skin = (int8_t)skin;
    if (l->host) l->dirty = 1;
    else if (l->ses && l->node >= 0) {
        uint8_t m[3] = {L_SKIN, (uint8_t)i, (uint8_t)skin};
        ses_app_send(l->ses, l->host_node, m, 3);
    }
}
void lobby_on_ext(Lobby *l, void (*fn)(void *, int, const uint8_t *, int), void *ctx) { l->ext = fn; l->ext_ctx = ctx; }
void lobby_ext_send(Lobby *l, int to, const void *data, int len) { if (l->ses && to >= 0 && to != l->node) ses_app_send(l->ses, to, data, len); }

void lobby_set_max_players(Lobby *l, int max) { l->max_players = max; }
void lobby_set_picture(Lobby *l, int i, int picture) /* (before joining: it goes with this machine's players) */
{
    if (!l || i < 0 || i >= l->count) return;
    l->mine[i].picture = (int16_t)picture;
    if (l->node >= 0 && l->node < SES_MAX_NODES) l->view.m[l->node].p[i].picture = (int16_t)picture;
    if (l->host) l->dirty = 1;
}
void lobby_set_online_id(Lobby *l, int i, const uint8_t id[8]) /* (before joining: it goes with this machine's players) */
{
    if (!l || i < 0 || i >= l->count) return;
    memcpy(l->mine[i].online_id, id, 8);
    if (l->node >= 0 && l->node < SES_MAX_NODES) memcpy(l->view.m[l->node].p[i].online_id, id, 8);
    if (l->host) l->dirty = 1;
}
void lobby_set_table(Lobby *l, int i, const uint8_t table[128])
{
    if (i < 0 || i >= l->count || !memcmp(l->mine[i].table, table, 128)) return;
    memcpy(l->mine[i].table, table, 128);
    int me = l->node;
    if (me >= 0 && me < SES_MAX_NODES) memcpy(l->view.m[me].p[i].table, table, 128);
    if (l->host) l->dirty = 1;
    else if (l->ses && l->node >= 0) {
        uint8_t m[2 + 128] = {L_TABLE, (uint8_t)i};
        memcpy(m + 2, table, 128);
        ses_app_send(l->ses, l->host_node, m, (int)sizeof m);
    }
}
void lobby_set_vote(Lobby *l, int i, int round, int choice)
{
    if (i < 0 || i >= l->count) return;
    l->mine[i].vote = (int8_t)choice;
    l->mine[i].vote_round = (uint8_t)round;
    int me = l->node;
    if (me >= 0 && me < SES_MAX_NODES) { l->view.m[me].p[i].vote = (int8_t)choice; l->view.m[me].p[i].vote_round = (uint8_t)round; }
    if (l->host) l->dirty = 1;
    else if (l->ses && l->node >= 0) {
        uint8_t m[4] = {L_VOTE, (uint8_t)i, (uint8_t)round, (uint8_t)choice};
        ses_app_send(l->ses, l->host_node, m, 4);
    }
}
void lobby_set_team(Lobby *l, int node, int i, int team)
{
    if (!l->host || node < 0 || node >= SES_MAX_NODES || i < 0 || i >= l->view.m[node].count) return;
    l->view.m[node].p[i].team = (int8_t)team;
    if (node == l->node) l->mine[i].team = (int8_t)team;
    l->dirty = 1;
}

void lobby_set_settings(Lobby *l, const void *data, int len)
{
    if (!l->host || len < 0 || len > LOBBY_SETTINGS_MAX) return;
    if (len == l->view.settings_len && !memcmp(l->view.settings, data, (size_t)len)) return;
    memcpy(l->view.settings, data, (size_t)len);
    l->view.settings_len = len;
    l->dirty = 1;
}

void lobby_start(Lobby *l, uint32_t seed, int delay, const int8_t car[SES_MAX_NODES])
{
    if (!l->host) return;
    if (l->dirty) send_state(l); /* (the lobby as it starts: everyone the same) */
    LobbyStart s;
    memset(&s, 0, sizeof s);
    s.seed = seed;
    s.delay = (uint8_t)delay;
    memset(s.first, 0xFF, sizeof s.first);
    int h = 0;
    for (int n = 0; n < SES_MAX_NODES; n++) /* (a spectators' machine: none of the start's players; it watches) */
        if (l->view.m[n].present && l->view.m[n].count && !lobby_machine_watches(&l->view.m[n])) {
            for (int i = 0; i < l->view.m[n].count; i++) /* (a split screen's spectators: a car each, parked out) */
                if (lobby_player_spectates(&l->view.m[n], i) && h + i < 32) s.spec |= 1u << (h + i);
            s.first[n] = (uint8_t)h;
            h += l->view.m[n].count;
        }
    s.humans = (uint8_t)h;
    memcpy(s.car, car, SES_MAX_NODES);
    memset(s.skin, -1, sizeof s.skin);
    for (int n = 0; n < SES_MAX_NODES; n++) /* (each human's skin: their choice, kept only with their own car) */
        for (int i = 0; s.first[n] != 0xFF && i < l->view.m[n].count && s.first[n] + i < SES_MAX_NODES; i++)
            if (l->view.m[n].p[i].car >= 0 && l->view.m[n].p[i].car == car[s.first[n] + i]) s.skin[s.first[n] + i] = l->view.m[n].p[i].skin;
    uint8_t m[1 + 4 + 2 + 3 * SES_MAX_NODES + 4], *w = m;
    *w++ = L_START;
    w = put32(w, s.seed);
    *w++ = s.delay;
    *w++ = s.humans;
    memcpy(w, s.first, SES_MAX_NODES);
    w += SES_MAX_NODES;
    memcpy(w, s.car, SES_MAX_NODES);
    w += SES_MAX_NODES;
    memcpy(w, s.skin, SES_MAX_NODES);
    w += SES_MAX_NODES;
    w = put32(w, s.spec);
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != l->node && l->view.m[n].present) ses_app_send(l->ses, n, m, (int)(w - m));
    l->view.start = s;
    l->view.starts++;
}

void lobby_rejoin(Lobby *l, int node, int first)
{
    if (!l->host || node < 0 || node >= SES_MAX_NODES || node == l->node) return;
    LobbyStart *s = &l->view.start;
    for (int n = 0; n < SES_MAX_NODES; n++) if (s->first[n] == (uint8_t)first) s->first[n] = 0xFF; /* (its old node's) */
    s->first[node] = (uint8_t)first;
    uint8_t m[2 + 4 + 2 + 3 * SES_MAX_NODES + 4], *w = m;
    *w++ = L_REJOIN;
    *w++ = (uint8_t)node;
    w = put32(w, s->seed);
    *w++ = s->delay;
    *w++ = s->humans;
    memcpy(w, s->first, SES_MAX_NODES);
    w += SES_MAX_NODES;
    memcpy(w, s->car, SES_MAX_NODES);
    w += SES_MAX_NODES;
    memcpy(w, s->skin, SES_MAX_NODES);
    w += SES_MAX_NODES;
    w = put32(w, s->spec);
    for (int n = 0; n < SES_MAX_NODES; n++)
        if (n != l->node && l->view.m[n].present) ses_app_send(l->ses, n, m, (int)(w - m));
}
void lobby_watch(Lobby *l, int node) { lobby_rejoin(l, node, 0xFF); } /* (first 0xFF: none of the start's players) */
int lobby_rejoining(const Lobby *l) { return l->rejoin; }
void lobby_set_humans(Lobby *l, int humans) /* (host) the match's players, more of them (one dropping in): sent with
                                             * lobby_rejoin */
{
    if (l->host && humans > 0 && humans <= SES_MAX_NODES) l->view.start.humans = (uint8_t)humans;
}
void lobby_set_car(Lobby *l, int h, int car) /* (host) human h's car in the match's start (one dropping in) */
{
    if (l->host && h >= 0 && h < SES_MAX_NODES) l->view.start.car[h] = (int8_t)car, l->view.start.skin[h] = -1;
}
int lobby_node_relayed(const Lobby *l, int node) { return udp_relayed(l->udp, node); }
void lobby_on_voice(Lobby *l, SesVoiceFn fn, void *ctx) { l->voice = fn; l->voice_ctx = ctx; }
void lobby_voice_send(Lobby *l, uint16_t seq, const uint8_t *data, int len) { if (l->ses) ses_send_voice(l->ses, seq, data, len, l->now); }
void lobby_rejoin_done(Lobby *l) { l->rejoin = 0; }
void lobby_set_match_on(Lobby *l, int on)
{
    if (!l || l->view.match_on == (on != 0)) return;
    l->view.match_on = on != 0;
    l->dirty = 1;
    if (getenv("TM2PC_NET_DEBUG")) fprintf(stderr, "lobby: the host in a match: %d\n", on);
}
uint32_t lobby_machine_id(void)
{
    static uint32_t id;
    if (!id) id = ((uint32_t)ms_now() * 2654435761u) ^ (uint32_t)(uintptr_t)&id ^ 0x9E3779B9u;
    return id ? id : 1;
}
void lobby_kick(Lobby *l, int node)
{
    if (!l->host || node < 0 || node == l->node || node >= SES_MAX_NODES || !ses_present(l->ses, node)) return;
    char addr[48] = "";
    if (udp_node_addr(l->udp, node, addr) && l->banned_n < BANNED_MAX) snprintf(l->banned[l->banned_n++], 48, "%s", addr);
    ses_kick(l->ses, node, l->now);
}

void lobby_chat(Lobby *l, const char *text)
{
    if (!l->ses) return;
    ses_chat(l->ses, text);
    on_chat(l, l->node, text); /* (the session sends it to the others: this machine's own line shown here) */
}
void lobby_kick_vote(Lobby *l, int node) { if (l->ses) ses_kick_vote(l->ses, node, l->now); }
int lobby_kick_progress(const Lobby *l, int *target, int *yes, int *needed, int *state, uint32_t *at)
{
    if (!l->kick.at) return 0;
    if (target) *target = l->kick.target;
    if (yes) *yes = l->kick.yes;
    if (needed) *needed = l->kick.needed;
    if (state) *state = l->kick.state;
    if (at) *at = l->kick.at;
    return 1;
}
/* line i of every line ever said (0 the first): kept only the last LOBBY_CHAT_LINES (lobby_chat_first). By position in
 * the kept ones, a full list gave nothing new: after 32 lines no one's chat showed (Tegu's, 2026-11-23: the countdown
 * alone says 6 a match) */
int lobby_chat_lines(const Lobby *l, int i, int *speaker, const char **text)
{
    int k = i - (l->chat_total - l->chat_n);
    if (k < 0 || k >= l->chat_n) return 0;
    if (speaker) *speaker = l->chat[k].speaker;
    if (text) *text = l->chat[k].text;
    return 1;
}
int lobby_chat_first(const Lobby *l) { return l->chat_total - l->chat_n; } /* (the oldest line kept) */
