/* UDP for network matches (net_udp.h). */
#include "net_udp.h"
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#endif

enum { RELAYS_MAX = 8, RELAYED_MAX = 32 };
struct NetUdp {
    sock_t s;
    uint16_t port;
    struct sockaddr_in node[SES_MAX_NODES];
    int known[SES_MAX_NODES];
    struct sockaddr_in relay[RELAYS_MAX];   /* the masters that can relay (their packets unwrapped) */
    int relays;
    struct { struct sockaddr_in peer; int via; } relayed[RELAYED_MAX]; /* the peers reached through a relay */
    int relayed_n;
};
static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static int relayed_via(const NetUdp *u, const struct sockaddr_in *peer) /* the relay a peer is reached through, -1 direct */
{
    for (int i = 0; i < u->relayed_n; i++) if (same_addr(&u->relayed[i].peer, peer)) return u->relayed[i].via;
    return -1;
}
static void mark_relayed(NetUdp *u, const struct sockaddr_in *peer, int via)
{
    for (int i = 0; i < u->relayed_n; i++) if (same_addr(&u->relayed[i].peer, peer)) { u->relayed[i].via = via; return; }
    if (u->relayed_n < RELAYED_MAX) { u->relayed[u->relayed_n].peer = *peer; u->relayed[u->relayed_n].via = via; u->relayed_n++; }
}
static int send_raw(NetUdp *u, const struct sockaddr_in *to, const void *data, int len) /* direct, or wrapped for its relay */
{
    int via = relayed_via(u, to);
    if (via < 0 || via >= u->relays) return (int)sendto(u->s, (const char *)data, len, 0, (const struct sockaddr *)to, sizeof *to) == len;
    uint8_t w[2 + 18 + 4096];
    if (len > (int)sizeof w - 20) return 0;
    w[0] = 0x53; /* MASTER_CHANNEL, MASTER_RELAY (master.h) */
    w[1] = 9;
    memset(w + 2, 0, 16);
    inet_ntop(AF_INET, &to->sin_addr, (char *)w + 2, 16);
    w[18] = (uint8_t)ntohs(to->sin_port);
    w[19] = (uint8_t)(ntohs(to->sin_port) >> 8);
    memcpy(w + 20, data, (size_t)len);
    return (int)sendto(u->s, (const char *)w, len + 20, 0, (const struct sockaddr *)&u->relay[via], sizeof u->relay[via]) == len + 20;
}

static int started;

static int net_start(void)
{
#ifdef _WIN32
    if (!started) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    }
#endif
    started = 1;
    return 1;
}

NetUdp *udp_open(uint16_t port, int flags)
{
    if (!net_start()) return NULL;
    NetUdp *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u->s == BAD_SOCK) { free(u); return NULL; }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(u->s, FIONBIO, &nb);
    /* (Windows: a datagram sent to a port no one has any more comes back as an error on the next receive, which
     * stopped the reading for the frame: a machine gone, every packet still sent to it cost one; a master relaying to
     * it fell behind and the others lost each other. Off.) */
    BOOL report = FALSE;
    DWORD got = 0;
    WSAIoctl(u->s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report, sizeof report, NULL, 0, &got, NULL, NULL);
#else
    fcntl(u->s, F_SETFL, fcntl(u->s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int on = 1;
    if (flags & UDP_BROADCAST) setsockopt(u->s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(flags & UDP_LOOPBACK ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(u->s, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "network: port %u is taken\n", port);
        close_sock(u->s);
        free(u);
        return NULL;
    }
    socklen_t n = sizeof a;
    getsockname(u->s, (struct sockaddr *)&a, &n);
    u->port = ntohs(a.sin_port);
    return u;
}

void udp_close(NetUdp *u)
{
    if (!u) return;
    close_sock(u->s);
    free(u);
}

uint16_t udp_port(const NetUdp *u) { return u->port; }

static int resolve(const char *host, uint16_t port, struct sockaddr_in *out)
{
    struct addrinfo hint, *res = NULL;
    memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hint, &res) || !res) return 0;
    memcpy(out, res->ai_addr, sizeof *out);
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return 1;
}

int udp_set_node(NetUdp *u, int node, const char *host, uint16_t port)
{
    if (node < 0 || node >= SES_MAX_NODES) return 0;
    u->known[node] = resolve(host, port, &u->node[node]);
    return u->known[node];
}

int udp_node_addr(const NetUdp *u, int node, char addr[48])
{
    if (node < 0 || node >= SES_MAX_NODES || !u->known[node]) return 0;
    inet_ntop(AF_INET, &u->node[node].sin_addr, addr, 48);
    return 1;
}

uint16_t udp_node_port(const NetUdp *u, int node)
{
    return node >= 0 && node < SES_MAX_NODES && u->known[node] ? ntohs(u->node[node].sin_port) : 0;
}
int udp_node_known(const NetUdp *u, int node) { return node >= 0 && node < SES_MAX_NODES && u->known[node]; }

void udp_forget_node(NetUdp *u, int node)
{
    if (node >= 0 && node < SES_MAX_NODES) u->known[node] = 0;
}

void udp_send(void *ctx, int to, const void *data, int len)
{
    NetUdp *u = ctx;
    if (to < 0 || to >= SES_MAX_NODES || !u->known[to]) return;
    send_raw(u, &u->node[to], data, len);
}

int udp_send_to(NetUdp *u, const char *host, uint16_t port, const void *data, int len)
{
    struct sockaddr_in a;
    if (!resolve(host, port, &a)) return 0;
    return send_raw(u, &a, data, len);
}

int udp_add_relay(NetUdp *u, const char *host, uint16_t port)
{
    if (u->relays >= RELAYS_MAX || !resolve(host, port, &u->relay[u->relays])) return 0;
    for (int i = 0; i < u->relays; i++) if (same_addr(&u->relay[i], &u->relay[u->relays])) return 1; /* (known already) */
    u->relays++;
    return 1;
}
int udp_relay_to(NetUdp *u, const char *host, uint16_t port)
{
    struct sockaddr_in a;
    if (!u->relays || !resolve(host, port, &a)) return 0;
    mark_relayed(u, &a, 0); /* (through the first master: the main one) */
    return 1;
}
int udp_relayed(const NetUdp *u, int node)
{
    return node >= 0 && node < SES_MAX_NODES && u->known[node] && relayed_via(u, &u->node[node]) >= 0;
}

int udp_broadcast(NetUdp *u, uint16_t port, const void *data, int len)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    a.sin_port = htons(port);
    return sendto(u->s, (const char *)data, len, 0, (struct sockaddr *)&a, sizeof a) == len;
}

int udp_poll(NetUdp *u, int *from_node, char addr[48], uint16_t *port, void *buf, int cap)
{
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    int len = (int)recvfrom(u->s, (char *)buf, cap, 0, (struct sockaddr *)&a, &n);
#ifdef _WIN32
    for (int tries = 0; len < 0 && WSAGetLastError() == WSAECONNRESET && tries < 64; tries++) { /* (one such error: on to the next) */
        n = sizeof a;
        len = (int)recvfrom(u->s, (char *)buf, cap, 0, (struct sockaddr *)&a, &n);
    }
#endif
    if (len <= 0) return 0;
    if (len > 20 && ((uint8_t *)buf)[0] == 0x53 && ((uint8_t *)buf)[1] == 10) /* a relay's RELAYED: unwrapped, from its sender */
        for (int r = 0; r < u->relays; r++) {
            if (!same_addr(&a, &u->relay[r])) continue;
            uint8_t *b = buf;
            char at[17];
            memcpy(at, b + 2, 16);
            at[16] = 0;
            struct sockaddr_in from;
            memset(&from, 0, sizeof from);
            from.sin_family = AF_INET;
            if (inet_pton(AF_INET, at, &from.sin_addr) != 1) return udp_poll(u, from_node, addr, port, buf, cap);
            from.sin_port = htons((uint16_t)(b[18] | b[19] << 8));
            mark_relayed(u, &from, r); /* (its answers back the same way) */
            len -= 20;
            memmove(b, b + 20, (size_t)len);
            a = from;
            break;
        }
    *from_node = -1;
    for (int i = 0; i < SES_MAX_NODES; i++)
        if (u->known[i] && u->node[i].sin_addr.s_addr == a.sin_addr.s_addr && u->node[i].sin_port == a.sin_port) {
            *from_node = i;
            break;
        }
    if (addr) inet_ntop(AF_INET, &a.sin_addr, addr, 48);
    if (port) *port = ntohs(a.sin_port);
    return len;
}
