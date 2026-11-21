/* The master's friends and stats (tools/master/accounts.c) over real UDP on 127.0.0.1: two profiles, a request and its
 * acceptance, the lists with presence, a match result counted once two machines send it, a wrong secret refused, and
 * all of it kept on disk (the master started again).
 *
 *     master_friends_test <tm2_master.exe>        (runs it on port 27531 with a data file in the temp folder) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "master.h"
#include "net_udp.h"
#ifdef _WIN32
#include <windows.h>
static void nap(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void nap(int ms) { usleep((useconds_t)ms * 1000); }
#endif

enum { PORT = 27531 };
static NetUdp *ua, *ub;
static int failures;
static void check(const char *what, int ok) { printf("%-58s %s\n", what, ok ? "ok" : "FAIL"); failures += !ok; }

typedef struct { uint8_t id[MASTER_ID_BYTES], secret[MASTER_SECRET_BYTES]; const char *name; } Who;
static Who A = {{1, 2, 3, 4, 5}, {11, 12, 13}, "Alice"}, B = {{9, 8, 7, 6, 5}, {21, 22, 23}, "Bob"};

static int head(uint8_t *m, int kind, const Who *w)
{
    m[0] = MASTER_CHANNEL;
    m[1] = (uint8_t)kind;
    memcpy(m + 2, w->id, MASTER_ID_BYTES);
    memcpy(m + 2 + MASTER_ID_BYTES, w->secret, MASTER_SECRET_BYTES);
    return 2 + MASTER_ID_BYTES + MASTER_SECRET_BYTES;
}
static void send_id(NetUdp *u, const Who *w, int where)
{
    uint8_t m[128];
    int n = head(m, MASTER_ID, w);
    memset(m + n, 0, MASTER_NAME_BYTES);
    snprintf((char *)m + n, MASTER_NAME_BYTES, "%s", w->name);
    n += MASTER_NAME_BYTES;
    m[n++] = (uint8_t)where;
    udp_send_to(u, "127.0.0.1", PORT, m, n);
}
/* the lists the master sent u (after an ID): friends' names (",") into out, requests' into req */
static void lists(NetUdp *u, char *out, char *req, int *online)
{
    out[0] = req[0] = 0;
    *online = -1;
    uint8_t buf[2048];
    char addr[48];
    uint16_t port;
    int from, n;
    for (int t = 0; t < 50; t++, nap(10))
        while ((n = udp_poll(u, &from, addr, &port, buf, sizeof buf)) > 0) {
            if (n < 6 || buf[1] != MASTER_FRIENDS) continue;
            if (buf[2] == 0) (buf[4] ? req : out)[0] = 0; /* (a list's first part: an older one replaced) */
            const uint8_t *r = buf + 6;
            for (int k = 0; k < buf[5]; k++, r += MASTER_FRIEND_BYTES) {
                char *dst = buf[4] ? req : out;
                strcat(dst, (const char *)r + MASTER_ID_BYTES);
                strcat(dst, ",");
                if (!buf[4]) *online = r[MASTER_ID_BYTES + MASTER_NAME_BYTES];
            }
        }
}
static void send_kind(NetUdp *u, int kind, const Who *w, const Who *other, int how)
{
    uint8_t m[64];
    int n = head(m, kind, w);
    memcpy(m + n, other->id, MASTER_ID_BYTES);
    n += MASTER_ID_BYTES;
    if (kind == MASTER_FRIEND_ANSWER) m[n++] = (uint8_t)how;
    udp_send_to(u, "127.0.0.1", PORT, m, n);
}
static void send_result(NetUdp *u, const Who *w, int kills_a)
{
    uint8_t m[256];
    int n = head(m, MASTER_RESULT, w);
    memcpy(m + n, "MATCH001", 8); n += 8;
    m[n++] = 1; m[n++] = 3; m[n++] = 1; m[n++] = 2; /* vanilla, arena 3, mode 1, 2 players */
    const Who *p[2] = {&A, &B};
    for (int i = 0; i < 2; i++) {
        memcpy(m + n, p[i]->id, MASTER_ID_BYTES); n += MASTER_ID_BYTES;
        m[n++] = (uint8_t)(i ? 4 : 7); m[n++] = (uint8_t)(i == 0);
        int kills = i ? 1 : kills_a;
        m[n++] = (uint8_t)kills; m[n++] = 0; m[n++] = (uint8_t)(i ? 2 : 1); m[n++] = 0;
        m[n++] = 100; m[n++] = 0; m[n++] = 0; m[n++] = 0;
    }
    udp_send_to(u, "127.0.0.1", PORT, m, n);
}
static void run_master(const char *exe, const char *db, int start)
{
    static char cmd[1400];
#ifdef _WIN32
    static PROCESS_INFORMATION pi;
    if (!start) { TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 3000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return; }
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    snprintf(cmd, sizeof cmd, "\"%s\" %d \"%s\"", exe, PORT, db);
    SetEnvironmentVariableA("TM2PC_NET_LOCAL", "1");
    SetEnvironmentVariableA("TM2_MASTER_SAVE_MS", "100"); /* (written at once: the test stops it hard) */
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { printf("cannot start %s\n", exe); exit(2); }
#else
    (void)exe; (void)db; (void)start; (void)cmd;
#endif
    nap(1000); /* (started: its file read, its port open) */
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "master_friends_test <tm2_master>\n"); return 2; }
    char db[1024];
    snprintf(db, sizeof db, "%s/tm2_master_test.db", getenv("TEMP") ? getenv("TEMP") : ".");
    remove(db);
    run_master(argv[1], db, 1);
    ua = udp_open(0, UDP_LOOPBACK);
    ub = udp_open(0, UDP_LOOPBACK);
    char fr[512], rq[512];
    int on;
    send_id(ua, &A, 0);
    send_id(ub, &B, 0);
    lists(ua, fr, rq, &on);
    check("a new profile: no friends, no requests", !fr[0] && !rq[0]);
    send_kind(ua, MASTER_FRIEND_ADD, &A, &B, 0);
    nap(100);
    send_id(ub, &B, 0);
    lists(ub, fr, rq, &on);
    check("Alice's request reaches Bob", !strcmp(rq, "Alice,"));
    send_kind(ub, MASTER_FRIEND_ANSWER, &B, &A, 1);
    nap(100);
    send_id(ua, &A, 0);
    lists(ua, fr, rq, &on);
    check("accepted: Bob is Alice's friend, and online", !strcmp(fr, "Bob,") && on == 1);
    send_id(ub, &B, 0);
    lists(ub, fr, rq, &on);
    check("and Alice is Bob's (the request gone)", !strcmp(fr, "Alice,") && !rq[0]);
    Who fake = B;
    fake.secret[0] = 99;
    fake.name = "Mallory";
    send_kind(ua, MASTER_FRIEND_ANSWER, &fake, &A, 2); /* (Bob's id, another secret: removing Alice) */
    nap(100);
    send_id(ua, &A, 0);
    lists(ua, fr, rq, &on);
    check("a wrong secret is refused (still friends)", !strcmp(fr, "Bob,"));
    send_result(ua, &A, 3);
    send_result(ua, &A, 3); /* (the same machine twice: once) */
    send_result(ub, &B, 5); /* (another view: not agreeing) */
    nap(100);
    send_result(ub, &fake, 3); /* (a wrong secret: not counted) */
    nap(300);
    run_master(argv[1], db, 0);
    run_master(argv[1], db, 1); /* (a new run: this time B sends the same as A) */
    send_id(ua, &A, 0);
    send_id(ub, &B, 0);
    nap(100);
    send_result(ua, &A, 3);
    send_result(ub, &B, 3);
    nap(500);
    run_master(argv[1], db, 0);
    FILE *f = fopen(db, "r");
    char line[512], text[4096] = "";
    while (f && fgets(line, sizeof line, f)) if (strlen(text) + strlen(line) < sizeof text) strcat(text, line);
    if (f) fclose(f);
    check("kept on disk: both players", strstr(text, "Alice") && strstr(text, "Bob"));
    check("kept on disk: their friendship", strstr(text, "\nF ") != NULL);
    check("a result counted once two machines agree (Alice, car 7)", strstr(text, "S 0102030405000000 7 1 1 3 1 100") != NULL);
    check("and only once (Bob, car 4: one match)", strstr(text, "S 0908070605000000 4 1 0 1 2 100") != NULL);
    check("the totals: one match, 4 kills, 3 deaths, 200 damage", strstr(text, "\nG 1 4 3 200\n") != NULL);
    check("per car: car 7 (one win), car 4", strstr(text, "\nC 4 1 0 1 2 100\n") && strstr(text, "\nC 7 1 1 3 1 100\n"));
    { /* the public stats beside it (the website's): the totals and the cars, no id, no secret */
        char pub[1100], js[4096] = "";
        snprintf(pub, sizeof pub, "%s", db);
        char *sl = strrchr(pub, '/');
        snprintf(sl ? sl + 1 : pub, sizeof pub - (size_t)(sl ? sl + 1 - pub : 0), "tm2_stats.json");
        FILE *pf = fopen(pub, "r");
        size_t n = pf ? fread(js, 1, sizeof js - 1, pf) : 0;
        js[n] = 0;
        if (pf) fclose(pf);
        check("the public stats: totals and Roadkill's win, no ids", strstr(js, "\"matches\": 1,") &&
              strstr(js, "\"name\": \"Roadkill\", \"matches\": 1, \"wins\": 1") && !strstr(js, "0102030405"));
    }
    run_master(argv[1], db, 1); /* started again: read back */
    send_id(ua, &A, 0);
    lists(ua, fr, rq, &on);
    if (strcmp(fr, "Bob,")) printf("(read back: friends \"%s\", requests \"%s\", online %d)\n", fr, rq, on);
    check("started again: Alice's friends read back", !strcmp(fr, "Bob,"));
    run_master(argv[1], db, 0);
    printf(failures ? "%d FAILED\n" : "all checks passed\n", failures);
    return failures != 0;
}
