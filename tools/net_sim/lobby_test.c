/* lobby_test: a server's lobby (src/net/lobby.c) over real UDP on 127.0.0.1 (no firewall prompt), without the game:
 * a host and three machines in one process. Joining (node numbers), the players and their choices, the host's
 * settings, chat, the match's start, a machine leaving, a kick and the kicked machine refused. Prints each check;
 * exits 1 if one fails. */
#include "../../src/net/lobby.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uint32_t now_ms(void) { return (uint32_t)GetTickCount64(); }
static void nap(void) { Sleep(1); }
#else
#include <time.h>
#include <unistd.h>
static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void nap(void) { usleep(1000); }
#endif

#include <stdint.h>
void net_desync_dump(int32_t step, int host); /* (the game's, net/game_tm2.c: a test dump; none here) */
void net_desync_dump(int32_t step, int host) { (void)step; (void)host; }

enum { PORT = 47100, N = 4 };
static Lobby *lb[N];
static int fails;

static void poll_all(void) { for (int i = 0; i < N; i++) if (lb[i]) lobby_poll(lb[i], now_ms()); }
typedef int (*Cond)(void);
static int wait_for(Cond c, int ms)
{
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < (uint32_t)ms) {
        poll_all();
        if (c()) return 1;
        nap();
    }
    return c();
}
static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) fails++;
}

static int all_in(void)
{
    for (int i = 1; i < 3; i++) if (!lb[i] || lobby_status(lb[i]) != LOBBY_IN) return 0;
    const LobbyView *h = lobby_view(lb[0]);
    return h->m[1].present && h->m[2].present;
}
static int same_lobby(const LobbyView *a, const LobbyView *b)
{
    if (a->settings_len != b->settings_len || memcmp(a->settings, b->settings, (size_t)a->settings_len)) return 0;
    if (strcmp(a->server, b->server)) return 0;
    for (int n = 0; n < SES_MAX_NODES; n++) {
        const LobbyMachine *x = &a->m[n], *y = &b->m[n];
        if (x->present != y->present || x->count != y->count) return 0;
        for (int i = 0; i < x->count; i++)
            if (strcmp(x->p[i].name, y->p[i].name) || x->p[i].car != y->p[i].car || x->p[i].team != y->p[i].team ||
                x->p[i].ready != y->p[i].ready)
                return 0;
    }
    return 1;
}
static int everyone_same(void)
{
    for (int i = 1; i < N; i++) if (lb[i] && lobby_status(lb[i]) == LOBBY_IN && !same_lobby(lobby_view(lb[0]), lobby_view(lb[i]))) return 0;
    return 1;
}
static int choices_in(void)
{
    const LobbyView *h = lobby_view(lb[0]);
    return h->m[1].p[1].car == 5 && h->m[1].p[1].team == 1 && h->m[1].p[1].ready && h->m[2].p[0].car == 7 &&
           h->settings_len == 7 && everyone_same();
}
static int chat_everywhere(void)
{
    for (int i = 0; i < 3; i++) {
        int sp, k = 0;
        const char *t = NULL;
        while (lobby_chat_lines(lb[i], k, &sp, &t)) k++;
        if (!k || !lobby_chat_lines(lb[i], k - 1, &sp, &t) || strcmp(t, "hello from B") || sp != 2) return 0;
    }
    return 1;
}
static int chat_past_full(void) /* (Tegu's: after 32 lines the newest showed nowhere) */
{
    for (int i = 0; i < 3; i++) {
        int sp, k = lobby_chat_first(lb[i]);
        const char *t = NULL;
        while (lobby_chat_lines(lb[i], k, &sp, &t)) k++;
        if (k < 41 || !lobby_chat_lines(lb[i], k - 1, &sp, &t) || strcmp(t, "line forty")) return 0;
    }
    return 1;
}
static int started_everywhere(void)
{
    for (int i = 0; i < 3; i++) if (lobby_view(lb[i])->starts != 1) return 0;
    for (int i = 1; i < 3; i++) if (memcmp(&lobby_view(lb[i])->start, &lobby_view(lb[0])->start, sizeof(LobbyStart))) return 0;
    return 1;
}
static int third_in(void) { return lb[3] && lobby_status(lb[3]) == LOBBY_IN && lobby_view(lb[0])->m[3].present && everyone_same(); }
static int vote_in(void)
{
    for (int i = 0; i < 3; i++) {
        const LobbyView *v = lobby_view(lb[i == 0 ? 0 : i + 1 < N ? i + 1 : 0]);
        if (v->m[1].p[1].vote != 2 || v->m[1].p[1].vote_round != 1 || v->m[0].p[0].vote != 0) return 0;
    }
    return 1;
}
static int a_gone(void) { return !lobby_view(lb[0])->m[1].present && everyone_same(); }
static int b_kicked(void) { return lobby_status(lb[2]) == LOBBY_KICKED; }
static int b_gone(void) { return !lobby_view(lb[0])->m[2].present; }
static int b_refused(void) { return lobby_status(lb[2]) != LOBBY_CONNECTING; }

/* host migration (lb[0] gone: lb[1] the host, lb[2] a machine, lb[3] joins later) */
static int addrs_known(void)
{
    const LobbyView *v = lobby_view(lb[2]);
    return v->m[1].addr[0] && v->m[1].port && v->m[2].addr[0];
}
static int migrated(void)
{
    return lobby_is_host(lb[1]) && lobby_host_node(lb[1]) == 1 && lobby_host_node(lb[2]) == 1 && lobby_migrated_at(lb[2], NULL) &&
           lobby_status(lb[2]) == LOBBY_IN;
}
static int moved_same(void)
{
    const LobbyView *h = lobby_view(lb[1]);
    return !h->m[0].present && h->m[1].present && h->m[2].present && h->m[2].p[0].car == 5 && same_lobby(h, lobby_view(lb[2]));
}
static int joined_moved(void)
{
    if (!lb[3] || lobby_status(lb[3]) != LOBBY_IN) return 0;
    const LobbyView *h = lobby_view(lb[1]);
    return h->m[lobby_node(lb[3])].present && same_lobby(h, lobby_view(lb[2])) && same_lobby(h, lobby_view(lb[3]));
}
static int chat_moved(void)
{
    for (int i = 1; i < 4; i++) {
        int sp, k = 0;
        const char *t = NULL;
        while (lobby_chat_lines(lb[i], k, &sp, &t)) k++;
        if (!k || !lobby_chat_lines(lb[i], k - 1, &sp, &t) || strcmp(t, "hello new host") || sp != lobby_node(lb[3])) return 0;
    }
    return 1;
}

static int moved_started(void) /* the new host's Start: on every machine, the same, every machine's players numbered */
{
    const LobbyView *h = lobby_view(lb[1]);
    if (h->starts != 1) return 0;
    for (int i = 2; i < 4; i++)
        if (!lb[i] || lobby_view(lb[i])->starts != 1 || memcmp(&lobby_view(lb[i])->start, &h->start, sizeof(LobbyStart))) return 0;
    return h->start.humans == 3 && h->start.first[1] != 0xFF && h->start.first[2] != 0xFF && h->start.first[lobby_node(lb[3])] != 0xFF;
}

int main(void)
{
    const char host_names[1][LOBBY_NAME] = {"Host"};
    const char a_names[2][LOBBY_NAME] = {"A1", "A2"};
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0); /* (tests: every line out, a crash too) */
    const char b_names[1][LOBBY_NAME] = {"B1"};
    const char c_names[1][LOBBY_NAME] = {"C1"};
    lb[0] = lobby_host(PORT, 1, "Test Server", 1, host_names);
    check("the host's lobby open", lb[0] != NULL);
    if (!lb[0]) return 1;
    lb[1] = lobby_join("127.0.0.1", PORT, 1, 2, a_names);
    lb[2] = lobby_join("127.0.0.1", PORT, 1, 1, b_names);
    check("two machines join (nodes given, the lobby arrives)", wait_for(all_in, 5000));
    check("their node numbers 1 and 2", lobby_node(lb[1]) == 1 && lobby_node(lb[2]) == 2);
    check("everyone's lobby the host's (names, server)", wait_for(everyone_same, 3000) &&
          !strcmp(lobby_view(lb[2])->m[1].p[1].name, "A2") && !strcmp(lobby_view(lb[1])->server, "Test Server"));

    lobby_set_player(lb[1], 1, 5, 1, 1);
    lobby_set_player(lb[2], 0, 7, 0, 0);
    lobby_set_settings(lb[0], "arena=8", 7);
    check("cars, teams, ready and the host's settings everywhere", wait_for(choices_in, 3000));

    lobby_chat(lb[2], "hello from B");
    check("a chat line reaches every machine", wait_for(chat_everywhere, 3000));
    for (int i = 0; i < 40; i++) lobby_chat(lb[0], i == 39 ? "line forty" : "more"); /* (past the 32 kept) */
    check("chat past the 32 lines kept still reaches every machine", wait_for(chat_past_full, 3000));

    LobbyListing found[8];
    LanSearch *search = lobby_lan_search(1);
    int nf = 0;
    for (uint32_t t0 = now_ms(); now_ms() - t0 < 500;) { poll_all(); nf = lobby_lan_results(search, found, 8); nap(); }
    lobby_lan_search_free(search);
    if (nf != 1 || found[0].players != 4) printf("  (found %d%s%s, players %d, port %u, build %d)\n", nf, nf ? ": " : "", nf ? found[0].name : "",
                                                 nf ? found[0].players : 0, nf ? found[0].port : 0, nf ? found[0].same_build : 0);
    check("the LAN browser finds the server (name, players, port)",
          nf == 1 && !strcmp(found[0].name, "Test Server") && found[0].players == 4 && found[0].port == PORT && found[0].same_build &&
          !strcmp(found[0].host, "Host"));

    lobby_set_max_players(lb[0], 5); /* (host 1, A 2, B 1: room for one more player) */
    lb[3] = lobby_join("127.0.0.1", PORT, 1, 1, c_names);
    check("a third machine joins later and gets the lobby", wait_for(third_in, 5000));

    const int8_t cars[SES_MAX_NODES] = {3, 5, 9, 7, 1};
    lobby_start(lb[0], 1234, 2, cars);
    check("the start reaches everyone, the same", wait_for(started_everywhere, 3000));
    const LobbyStart *s = &lobby_view(lb[1])->start;
    check("the players numbered by machine (host 0, A 1-2, B 3, C 4)",
          s->humans == 5 && s->first[0] == 0 && s->first[1] == 1 && s->first[2] == 3 && s->first[3] == 4 && s->seed == 1234 && s->car[2] == 9);

    {   /* a machine of two players where one fits: refused (full) */
        const char d_names[2][LOBBY_NAME] = {"D1", "D2"};
        Lobby *full = lobby_join("127.0.0.1", PORT, 1, 2, d_names);
        uint32_t t0 = now_ms();
        while (now_ms() - t0 < 3000 && lobby_status(full) == LOBBY_CONNECTING) { poll_all(); lobby_poll(full, now_ms()); nap(); }
        check("a machine whose players do not fit is refused (full)", lobby_status(full) == LOBBY_REFUSED_FULL);
        lobby_free(full);
    }
    lobby_set_vote(lb[1], 1, 1, 2); /* (A's second player votes card 3 of round 1) */
    lobby_set_vote(lb[0], 0, 1, 0);
    check("the votes, a player each, in every lobby", wait_for(vote_in, 3000));

    lobby_free(lb[1]);
    lb[1] = NULL;
    check("a machine that leaves is gone from every lobby", wait_for(a_gone, 6000));

    lobby_kick(lb[0], 2);
    check("the host kicks a machine: it is told", wait_for(b_kicked, 3000));
    check("and it is gone from the lobby", wait_for(b_gone, 4000));
    lobby_free(lb[2]);
    lb[2] = lobby_join("127.0.0.1", PORT, 1, 1, b_names);
    check("its address cannot join again", wait_for(b_refused, 4000) && lobby_status(lb[2]) == LOBBY_REFUSED_BANNED);

    for (int i = 0; i < N; i++) { lobby_free(lb[i]); lb[i] = NULL; }

    /* host migration: the host leaves, the machine there longest carries on as the host; a new machine can join it */
    lb[0] = lobby_host(PORT + 1, 1, "Moving Server", 1, host_names);
    lb[1] = lobby_join("127.0.0.1", PORT + 1, 1, 1, a_names);
    lb[2] = lobby_join("127.0.0.1", PORT + 1, 1, 1, b_names);
    check("host migration: two machines in", wait_for(all_in, 5000) && wait_for(addrs_known, 3000));
    lobby_set_player(lb[2], 0, 5, 1, 1);
    wait_for(everyone_same, 2000);
    lobby_free(lb[0]);
    lb[0] = NULL;
    check("the host leaves: machine 1 takes over (both agree)", wait_for(migrated, 6000));
    check("the new host's lobby reaches the other (old host gone)", wait_for(moved_same, 3000));
    {
        LobbyListing found[8];
        LanSearch *search = lobby_lan_search(1);
        int nf = 0;
        for (uint32_t t0 = now_ms(); now_ms() - t0 < 500;) { poll_all(); nf = lobby_lan_results(search, found, 8); nap(); }
        lobby_lan_search_free(search);
        check("the LAN browser finds the new host's server", nf == 1 && !strcmp(found[0].name, "Moving Server") && found[0].players == 2);
        lb[3] = nf == 1 ? lobby_join("127.0.0.1", found[0].port, 1, 1, c_names) : NULL;
        check("a new machine joins the new host", lb[3] && wait_for(joined_moved, 5000));
        check("and gets a node of its own", lb[3] && lobby_node(lb[3]) != 1 && lobby_node(lb[3]) != 2);
        lobby_chat(lb[3], "hello new host");
        check("its chat reaches the others", wait_for(chat_moved, 3000));
        int8_t cars2[SES_MAX_NODES];
        memset(cars2, 3, sizeof cars2);
        lobby_start(lb[1], 99, 2, cars2);
        check("the new host's Start reaches every machine", wait_for(moved_started, 3000));
    }

    for (int i = 0; i < N; i++) { lobby_free(lb[i]); lb[i] = NULL; }

    /* the master server (tools/master: TM2PC_TEST_MASTER=host:port, one running): listed, found, joined through it */
    const char *ms = getenv("TM2PC_TEST_MASTER");
    if (ms) {
        lb[0] = lobby_host(PORT + 2, 1, "Internet Server", 1, host_names);
        lobby_set_masters(lb[0], ms);
        LobbyListing li;
        memset(&li, 0, sizeof li);
        snprintf(li.name, sizeof li.name, "Internet Server");
        snprintf(li.host, sizeof li.host, "Host");
        lobby_set_listing(lb[0], &li);
        for (uint32_t t0 = now_ms(); now_ms() - t0 < 500;) { poll_all(); nap(); } /* (its listing to the master) */
        LobbyListing found[8];
        LanSearch *search = lobby_master_search(ms, 1);
        int nf = 0;
        for (uint32_t t0 = now_ms(); now_ms() - t0 < 600;) { poll_all(); nf = lobby_lan_results(search, found, 8); nap(); }
        lobby_lan_search_free(search);
        check("the masters list the server once (its name, its public port)",
              nf == 1 && !strcmp(found[0].name, "Internet Server") && found[0].port == PORT + 2 && found[0].same_build);
        lb[1] = nf == 1 ? lobby_join(found[0].addr, found[0].port, 1, 1, a_names) : NULL;
        if (lb[1]) lobby_set_masters(lb[1], ms);
        lb[2] = nf == 1 ? lobby_join(found[0].addr, found[0].port, 1, 1, b_names) : NULL;
        if (lb[2]) lobby_set_masters(lb[2], ms);
        check("two machines join through it", lb[1] && lb[2] && wait_for(all_in, 5000));
        for (uint32_t t0 = now_ms(); now_ms() - t0 < 5000;) { poll_all(); nap(); } /* (keep-alives, introductions) */
        { /* the browser's details: the players there and the host's continent, through the master (v1.0.20) */
            LanSearch *again = lobby_master_search(ms, 1);
            int n2 = 0;
            for (uint32_t t0 = now_ms(); now_ms() - t0 < 600;) { poll_all(); n2 = lobby_lan_results(again, found, 8); nap(); }
            lobby_lan_search_free(again);
            int names = 0;
            for (const char *q = n2 == 1 ? found[0].who : ""; *q; q++) names += *q == ',';
            printf("(the listing's players: \"%s\", region \"%s\")\n", n2 == 1 ? found[0].who : "", n2 == 1 ? found[0].region : "");
            check("the master's listing names the players there and the host's continent",
                  n2 == 1 && found[0].who[0] && names + 1 == found[0].players && !strcmp(found[0].region, "NA"));
        }
        lobby_free(lb[0]);
        lb[0] = NULL;
        check("the host leaves: machine 1 takes over (internet)", wait_for(migrated, 6000));
        for (uint32_t t0 = now_ms(); now_ms() - t0 < 4500;) { poll_all(); nap(); } /* (its first listing) */
        LanSearch *s2 = lobby_master_search(ms, 1);
        nf = 0;
        for (uint32_t t0 = now_ms(); now_ms() - t0 < 600;) { poll_all(); nf = lobby_lan_results(s2, found, 8); nap(); }
        lobby_lan_search_free(s2);
        int listed_new = 0;
        for (int i = 0; i < nf; i++) listed_new |= found[i].port == lobby_port(lb[1]);
        check("the new host is listed by the master", listed_new);
        for (int i = 0; i < N; i++) { lobby_free(lb[i]); lb[i] = NULL; }
    }
    printf(fails ? "%d checks failed\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
