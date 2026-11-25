/* Friends and presence through the master servers (friends.h). */
#include "friends.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_udp.h"

int masters_parse(const char *list, char addr[][48], uint16_t port[], int max); /* lobby.c */
enum { MASTERS = 4 };

static struct {
    int have;
    uint8_t id[MASTER_ID_BYTES], secret[MASTER_SECRET_BYTES];
    char name[MASTER_NAME_BYTES];
    char master[MASTERS][48];
    uint16_t master_port[MASTERS];
    int masters, loopback;
    NetUdp *u;
    uint32_t sent_ms;
    Friend list[2][MASTER_FRIENDS_MAX], next[2][MASTER_FRIENDS_MAX];
    int n[2], next_n[2], parts_seen[2];
} F;

void friends_identity(const uint8_t id[MASTER_ID_BYTES], const uint8_t secret[MASTER_SECRET_BYTES], const char *name)
{
    if (!id) { F.have = 0; return; }
    if (!F.have || memcmp(F.id, id, MASTER_ID_BYTES)) { F.n[0] = F.n[1] = 0; } /* (another profile: another list) */
    F.have = 1;
    memcpy(F.id, id, MASTER_ID_BYTES);
    memcpy(F.secret, secret, MASTER_SECRET_BYTES);
    memset(F.name, 0, sizeof F.name);
    snprintf(F.name, sizeof F.name, "%s", name ? name : "");
}
void friends_masters(const char *list, int loopback)
{
    F.masters = list && list[0] ? masters_parse(list, F.master, F.master_port, MASTERS) : 0;
    F.loopback = loopback;
}
const uint8_t *friends_my_id(void) { return F.have ? F.id : NULL; }

static uint8_t *head(uint8_t *w, int kind)
{
    *w++ = MASTER_CHANNEL;
    *w++ = (uint8_t)kind;
    memcpy(w, F.id, MASTER_ID_BYTES); w += MASTER_ID_BYTES;
    memcpy(w, F.secret, MASTER_SECRET_BYTES); w += MASTER_SECRET_BYTES;
    return w;
}
static void send_all(const uint8_t *m, int len)
{
    if (!F.u) F.u = udp_open(0, F.loopback ? UDP_LOOPBACK : 0);
    for (int i = 0; F.u && i < F.masters; i++) udp_send_to(F.u, F.master[i], F.master_port[i], m, len);
}
int friends_id_packet(uint8_t *out, int where, const char *host_addr, uint16_t host_port)
{
    if (!F.have) return 0;
    uint8_t *w = head(out, MASTER_ID);
    memcpy(w, F.name, MASTER_NAME_BYTES); w += MASTER_NAME_BYTES;
    *w++ = (uint8_t)where;
    if (where == 2) w = master_put_addr(w, host_addr ? host_addr : "", host_port);
    return (int)(w - out);
}

void friends_packet(const uint8_t *d, int n)
{
    if (n < 6 || d[0] != MASTER_CHANNEL || d[1] != MASTER_FRIENDS) return;
    int part = d[2], parts = d[3], what = d[4] & 1, count = d[5];
    if (n < 6 + count * MASTER_FRIEND_BYTES) return;
    if (part == 0) { F.next_n[what] = 0; F.parts_seen[what] = 0; }
    const uint8_t *r = d + 6;
    for (int k = 0; k < count && F.next_n[what] < MASTER_FRIENDS_MAX; k++, r += MASTER_FRIEND_BYTES) {
        Friend *f = &F.next[what][F.next_n[what]++];
        memcpy(f->id, r, MASTER_ID_BYTES);
        memcpy(f->name, r + MASTER_ID_BYTES, MASTER_NAME_BYTES);
        f->name[MASTER_NAME_BYTES - 1] = 0;
        f->online = r[MASTER_ID_BYTES + MASTER_NAME_BYTES];
        f->where = r[MASTER_ID_BYTES + MASTER_NAME_BYTES + 1];
        master_get_addr(r + MASTER_ID_BYTES + MASTER_NAME_BYTES + 2, f->addr, &f->port);
        memcpy(f->server, r + MASTER_ID_BYTES + MASTER_NAME_BYTES + 2 + MASTER_ADDR_BYTES, MASTER_SERVER_BYTES);
        f->server[MASTER_SERVER_BYTES - 1] = 0;
    }
    if (++F.parts_seen[what] >= parts) { /* (whole: shown) */
        int changed = F.n[what] != F.next_n[what];
        for (int i = 0; i < F.next_n[what] && !changed; i++)
            changed = memcmp(&F.list[what][i], &F.next[what][i], sizeof(Friend)) != 0;
        memcpy(F.list[what], F.next[what], sizeof(Friend) * (size_t)F.next_n[what]);
        F.n[what] = F.next_n[what];
        if (changed) {
            fprintf(stderr, "friends: %d %s:", F.n[what], what ? "requests" : "friends");
            for (int i = 0; i < F.n[what]; i++)
                fprintf(stderr, " %s (%s)", F.list[what][i].name, !F.list[what][i].online ? "offline" : F.list[what][i].where == 1 ? "hosting"
                        : F.list[what][i].where == 2 ? "in a server" : "online");
            fprintf(stderr, "\n");
        }
    }
}

void friends_poll(uint32_t now_ms, int active, int in_server)
{
    if (!F.have || !F.masters) return;
    if (active && !in_server && (!F.sent_ms || now_ms - F.sent_ms >= MASTER_REGISTER_MS)) { /* (in a server: the lobby's) */
        F.sent_ms = now_ms ? now_ms : 1;
        uint8_t m[128];
        send_all(m, friends_id_packet(m, 0, NULL, 0));
    }
    if (!F.u) return;
    uint8_t buf[2048];
    char addr[48];
    uint16_t port;
    int from, n;
    while ((n = udp_poll(F.u, &from, addr, &port, buf, sizeof buf)) > 0) friends_packet(buf, n);
}

static int test_requests(void) /* tests: TM2PC_TEST_FRIEND_REQUESTS=N, N made-up requests (the notice's pictures) */
{
    static int n = -1;
    if (n < 0) n = getenv("TM2PC_TEST_FRIEND_REQUESTS") ? atoi(getenv("TM2PC_TEST_FRIEND_REQUESTS")) : 0;
    return n;
}
int friends_count(int requests) { return (requests & 1) && test_requests() ? test_requests() : F.n[requests & 1]; }
const Friend *friends_get(int requests, int i)
{
    if ((requests & 1) && test_requests()) {
        static Friend fake;
        snprintf(fake.name, sizeof fake.name, "TESTER %d", i + 1);
        return i >= 0 && i < test_requests() ? &fake : NULL;
    }
    return i >= 0 && i < F.n[requests & 1] ? &F.list[requests & 1][i] : NULL;
}
int friends_is(const uint8_t id[MASTER_ID_BYTES])
{
    static int all = -1; /* tests (TM2PC_TEST_ALL_FRIENDS): every other player a friend (the marks' pictures) */
    if (all < 0) all = getenv("TM2PC_TEST_ALL_FRIENDS") != NULL;
    static const uint8_t none[MASTER_ID_BYTES];
    if (all) return memcmp(id, none, MASTER_ID_BYTES) && !(F.have && !memcmp(id, F.id, MASTER_ID_BYTES));
    for (int i = 0; i < F.n[0]; i++) if (!memcmp(F.list[0][i].id, id, MASTER_ID_BYTES)) return 1;
    return 0;
}
void friends_add(const uint8_t id[MASTER_ID_BYTES])
{
    if (!F.have || !memcmp(id, F.id, MASTER_ID_BYTES)) return;
    uint8_t m[64], *w = head(m, MASTER_FRIEND_ADD);
    memcpy(w, id, MASTER_ID_BYTES); w += MASTER_ID_BYTES;
    send_all(m, (int)(w - m));
    F.sent_ms = 0; /* (the list again soon) */
}
void friends_answer(const uint8_t id[MASTER_ID_BYTES], int how)
{
    if (!F.have) return;
    uint8_t m[64], *w = head(m, MASTER_FRIEND_ANSWER);
    memcpy(w, id, MASTER_ID_BYTES); w += MASTER_ID_BYTES;
    *w++ = (uint8_t)how;
    send_all(m, (int)(w - m));
    F.sent_ms = 0;
}

/* a friend code: the id's 5 bytes (40 bits) in 8 of Crockford's base 32 (no I, L, O, U), "XXXX-XXXX" */
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
void friends_code(const uint8_t id[MASTER_ID_BYTES], char out[10])
{
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = v << 8 | id[i];
    char c[8];
    for (int i = 7; i >= 0; i--) { c[i] = B32[v & 31]; v >>= 5; }
    snprintf(out, 10, "%.4s-%.4s", c, c + 4);
}
int friends_parse_code(const char *code, uint8_t id[MASTER_ID_BYTES])
{
    uint64_t v = 0;
    int n = 0;
    for (const char *p = code; *p && n < 9; p++) {
        char ch = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        if (ch == '-' || ch == ' ') continue;
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        const char *at = strchr(B32, ch);
        if (!at || !ch) return 0;
        v = v << 5 | (uint64_t)(at - B32);
        n++;
    }
    if (n != 8) return 0;
    memset(id, 0, MASTER_ID_BYTES);
    for (int i = 4; i >= 0; i--) { id[i] = (uint8_t)v; v >>= 8; }
    return 1;
}

void friends_report(const uint8_t key[8], int vanilla, int arena, int mode, const FriendResult *p, int n)
{
    if (!F.have || !F.masters || n < 2 || n > MASTER_RESULT_PLAYERS) return;
    uint8_t m[64 + MASTER_RESULT_PLAYERS * MASTER_RESULT_PLAYER_BYTES], *w = head(m, MASTER_RESULT);
    memcpy(w, key, 8); w += 8;
    *w++ = (uint8_t)vanilla; *w++ = (uint8_t)arena; *w++ = (uint8_t)mode; *w++ = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        memcpy(w, p[i].id, MASTER_ID_BYTES); w += MASTER_ID_BYTES;
        *w++ = p[i].car; *w++ = p[i].won;
        *w++ = (uint8_t)p[i].kills; *w++ = (uint8_t)(p[i].kills >> 8);
        *w++ = (uint8_t)p[i].deaths; *w++ = (uint8_t)(p[i].deaths >> 8);
        for (int k = 0; k < 4; k++) *w++ = (uint8_t)(p[i].damage >> (8 * k));
    }
    send_all(m, (int)(w - m));
}
