/* The master server (src/net/master.h): the internet's list of servers and the introductions for hole punching, the
 * players' friends and stats (accounts.c). It carries no match traffic. One UDP port (MASTER_PORT, or the first
 * argument); Windows or Linux.
 *
 *     tm2_master [port] [data file]   (TM2PC_NET_LOCAL=1: this PC only, for tests; the data: tm2_master.db)
 *
 * It keeps every machine it hears from (a host's listing, a member's keep-alive) by its public address for
 * MASTER_EXPIRE_MS. The players, friends and stats are written to the data file. */
#include "master.h"
#include "accounts.h"
#include "net_udp.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static uint32_t now_ms(void) { return (uint32_t)GetTickCount64(); }
static void nap(void) { Sleep(2); }
#else
#include <time.h>
#include <unistd.h>
static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void nap(void) { usleep(2000); }
#endif

enum { MACHINES = 4096, LISTING_MAX = 160 };
typedef struct {
    char addr[16];
    uint16_t port;
    uint32_t heard;               /* 0: a free entry */
    uint32_t listed;              /* (an asker) its last LIST: one a second (the answers are many packets: no flood) */
    uint32_t relay_second, relay_bytes; /* (a sender through the relay) this second's bytes */
    uint32_t build;               /* (a host) its build's id */
    int listing_len;              /* (a host) its listing's bytes; 0: a member (KEEP only) */
    uint8_t listing[LISTING_MAX];
} Machine;
static Machine m[MACHINES];

static Machine *find(const char *addr, uint16_t port, int make, uint32_t now)
{
    Machine *free_one = NULL;
    for (int i = 0; i < MACHINES; i++) {
        if (m[i].heard && now - m[i].heard > MASTER_EXPIRE_MS) m[i].heard = 0; /* (expired) */
        if (m[i].heard && m[i].port == port && !strcmp(m[i].addr, addr)) return &m[i];
        if (!m[i].heard && !free_one) free_one = &m[i];
    }
    if (!make || !free_one) return NULL;
    memset(free_one, 0, sizeof *free_one);
    snprintf(free_one->addr, sizeof free_one->addr, "%s", addr);
    free_one->port = port;
    return free_one;
}
static uint32_t get32(const uint8_t *r) { return r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24; }
static void listing_name(const char *addr, uint16_t port, char *out, int size) /* (accounts.c: a friend's server) */
{
    out[0] = 0;
    Machine *h = find(addr, port, 0, now_ms());
    if (h && h->listing_len >= 8 + 32) snprintf(out, (size_t)size, "%.31s", (const char *)h->listing + 8); /* (magic, build, name) */
}

static volatile sig_atomic_t stopping; /* (Ctrl+C, docker stop: written first, then out) */
static void on_stop(int sig) { (void)sig; stopping = 1; }
int main(int argc, char **argv)
{
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : MASTER_PORT;
    NetUdp *u = udp_open(port, getenv("TM2PC_NET_LOCAL") ? UDP_LOOPBACK : 0);
    if (!u) { fprintf(stderr, "master: port %u taken\n", (unsigned)port); return 1; }
    fprintf(stderr, "master: listening on UDP %u\n", (unsigned)port);
    setvbuf(stderr, NULL, _IONBF, 0);
    accounts_load(argc > 2 ? argv[2] : "tm2_master.db", listing_name);
    uint8_t buf[4096], out[4096];
    char addr[48];
    uint16_t from_port;
    int from;
    uint32_t relayed = 0, relayed_bytes = 0, told = 0; /* (the relay's traffic, told every 10 s while there is some) */
    for (;;) {
        int n = udp_poll(u, &from, addr, &from_port, buf, sizeof buf);
        if (relayed && now_ms() - told >= 10000) {
            fprintf(stderr, "master: relayed %u packets (%u KB) in the last 10 s\n", relayed, relayed_bytes / 1024);
            relayed = relayed_bytes = 0;
            told = now_ms();
        }
        if (stopping) { accounts_flush(); fprintf(stderr, "master: stopped (written)\n"); return 0; }
        accounts_tick(now_ms());
        if (n <= 0) { nap(); continue; }
        if (n < 2 || buf[0] != MASTER_CHANNEL) continue;
        uint32_t now = now_ms() ? now_ms() : 1;
        if (buf[1] == MASTER_REGISTER && n >= 2 + 8) { /* a host: its listing (the LAN answer: magic, build, ...) */
            Machine *h = find(addr, from_port, 1, now);
            if (!h) continue;
            int fresh = !h->listing_len;
            h->heard = now;
            h->build = get32(buf + 2 + 4);
            h->listing_len = n - 2 > LISTING_MAX ? LISTING_MAX : n - 2;
            memcpy(h->listing, buf + 2, (size_t)h->listing_len);
            if (fresh) fprintf(stderr, "master: a server at %s:%u\n", addr, (unsigned)from_port);
        } else if (buf[1] == MASTER_BYE) { /* leaving: off the list now */
            Machine *b = find(addr, from_port, 0, now);
            if (b) { if (b->listing_len) fprintf(stderr, "master: the server at %s:%u closed\n", addr, (unsigned)from_port); b->heard = 0; }
        } else if (buf[1] == MASTER_KEEP) { /* a member of an internet server */
            Machine *k = find(addr, from_port, 1, now);
            if (k) k->heard = now;
        } else if (buf[1] == MASTER_LIST && n >= 6) { /* the browser: every server of its build */
            uint32_t build = get32(buf + 2);
            Machine *asker = find(addr, from_port, 1, now);
            if (!asker || (asker->listed && now - asker->listed < 1000)) continue;
            if (!asker->heard) asker->heard = now;
            asker->listed = now;
            int sent = 0;
            for (int i = 0; i < MACHINES; i++) {
                if (!m[i].heard || now - m[i].heard > MASTER_EXPIRE_MS || !m[i].listing_len || m[i].build != build) continue;
                uint8_t *w = out;
                *w++ = MASTER_CHANNEL;
                *w++ = MASTER_ENTRY;
                w = master_put_addr(w, m[i].addr, m[i].port);
                memcpy(w, m[i].listing, (size_t)m[i].listing_len);
                w += m[i].listing_len;
                udp_send_to(u, addr, from_port, out, (int)(w - out));
                sent++;
            }
            fprintf(stderr, "master: %s:%u asked: %d servers\n", addr, (unsigned)from_port, sent);
        } else if (buf[1] == MASTER_RELAY && n > 2 + MASTER_ADDR_BYTES) { /* a packet passed on (hole punching failed) */
            char to[16];
            uint16_t to_port;
            master_get_addr(buf + 2, to, &to_port);
            Machine *sender = find(addr, from_port, 1, now), *target = find(to, to_port, 0, now);
            if (!sender || !target) continue; /* (only to a machine the master knows) */
            sender->heard = now;
            if (now / 1000 != sender->relay_second) { sender->relay_second = now / 1000; sender->relay_bytes = 0; }
            if ((sender->relay_bytes += (uint32_t)n) > MASTER_RELAY_BYTES) continue; /* (over its share: dropped) */
            out[0] = MASTER_CHANNEL;
            out[1] = MASTER_RELAYED;
            master_put_addr(out + 2, addr, from_port);
            memcpy(out + 2 + MASTER_ADDR_BYTES, buf + 2 + MASTER_ADDR_BYTES, (size_t)(n - 2 - MASTER_ADDR_BYTES));
            udp_send_to(u, to, to_port, out, n);
            if (!relayed && now_ms() - told >= 10000) told = now_ms();
            relayed++;
            relayed_bytes += (uint32_t)n;
        } else if (buf[1] == MASTER_ID) { /* a profile online: where it is; its friends and requests back */
            accounts_id(u, buf + 2, n - 2, addr, from_port, now);
        } else if (buf[1] == MASTER_FRIEND_ADD) {
            accounts_friend_add(buf + 2, n - 2);
        } else if (buf[1] == MASTER_FRIEND_ANSWER) {
            accounts_friend_answer(buf + 2, n - 2);
        } else if (buf[1] == MASTER_RESULT) {
            accounts_result(buf + 2, n - 2, now);
        } else if (buf[1] == MASTER_PUNCH && n >= 2 + MASTER_ADDR_BYTES) { /* introduce the asker and its target */
            char to[16];
            uint16_t to_port;
            master_get_addr(buf + 2, to, &to_port);
            Machine *asker = find(addr, from_port, 1, now), *target = find(to, to_port, 0, now);
            if (asker) asker->heard = now;
            if (!target) continue; /* (only machines the master knows: it sends nothing to an address it was only told of) */
            uint8_t *w = out;
            *w++ = MASTER_CHANNEL;
            *w++ = MASTER_INTRO;
            w = master_put_addr(w, addr, from_port);
            udp_send_to(u, to, to_port, out, (int)(w - out)); /* (to the target: the asker) */
            w = out + 2;
            w = master_put_addr(w, to, to_port);
            udp_send_to(u, addr, from_port, out, (int)(w - out)); /* (to the asker: the target) */
        }
    }
}
