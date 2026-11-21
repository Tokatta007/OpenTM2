/* UDP for network matches (docs/ONLINE_PLAN.md 1): a socket, the machines' addresses by node number (session.h's
 * transport), LAN discovery by broadcast. Windows (Winsock) and POSIX sockets. */
#ifndef TM2_NET_UDP_H
#define TM2_NET_UDP_H

#include <stdint.h>

enum { UDP_GAME_PORT = 27520, UDP_LAN_PORT = 27521 }; /* (the server's port; LAN discovery's) */

typedef struct NetUdp NetUdp;

enum { UDP_BROADCAST = 1, UDP_LOOPBACK = 2 };            /* udp_open's flags (loopback: this PC only, no firewall prompt) */
NetUdp *udp_open(uint16_t port, int flags);               /* port 0: any free one */
void udp_close(NetUdp *u);
uint16_t udp_port(const NetUdp *u);

int udp_set_node(NetUdp *u, int node, const char *host, uint16_t port); /* 1: the address resolved */
void udp_forget_node(NetUdp *u, int node);
int udp_node_addr(const NetUdp *u, int node, char addr[48]); /* a node's IP address (no port); 0: unknown */
uint16_t udp_node_port(const NetUdp *u, int node);
int udp_node_known(const NetUdp *u, int node);
void udp_send(void *u, int to_node, const void *data, int len);         /* (session.h's NetTransport.send) */
int udp_send_to(NetUdp *u, const char *host, uint16_t port, const void *data, int len);
int udp_broadcast(NetUdp *u, uint16_t port, const void *data, int len); /* LAN discovery */

/* the relay (a master server: master.h) for the peers no introduction gets through to: the masters that may relay
 * (their RELAYED packets unwrapped, answers back through them), a peer to reach through the first, and whether a
 * node is reached that way */
int udp_add_relay(NetUdp *u, const char *host, uint16_t port);
int udp_relay_to(NetUdp *u, const char *host, uint16_t port);
int udp_relayed(const NetUdp *u, int node);

/* the next datagram (0: none): from_node -1 when the sender is no known machine (a join, a LAN query): its address in
 * addr (a.b.c.d) and port, for udp_set_node */
int udp_poll(NetUdp *u, int *from_node, char addr[48], uint16_t *port, void *buf, int cap);

#endif
