/* The master server's protocol (docs/ONLINE_PLAN.md 1, tools/master/master.c): the internet's server list and the
 * introductions that let two machines behind home routers reach each other (hole punching).
 *
 * Every packet starts with MASTER_CHANNEL ('S'), then its kind. A machine talks to the master from its game socket
 * (the lobby's), so the master sees the public address and port its router gave that socket: the one the others must
 * send to.
 * - REGISTER (a hosting machine, every MASTER_REGISTER_MS): its listing (the LAN answer's bytes). The master keeps it
 *   MASTER_EXPIRE_MS, under the address it came from.
 * - KEEP (every other machine of an internet server, as often): known to the master, so it can be introduced.
 * - LIST (the browser, from any socket): the build's id; the master answers an ENTRY a server of that build.
 * - ENTRY (the master): the server's public address and port, its listing.
 * - PUNCH (a machine that wants to reach another: a joining machine, a peer after a host migration): that one's public
 *   address and port. The master sends both sides an INTRO with the other's address.
 * - INTRO (the master): another machine's address and port: send it a few PROBEs (the router then lets its packets in).
 * - PROBE: nothing to do (it opened the way).
 * - BYE (a machine leaving its server): forgotten at once (a host: off the list).
 * - RELAY (a machine whose router lets no introduction through: hole punching failed): another machine's address and
 *   port, then a packet for it. The master sends it on as RELAYED, the sender's address and port first; the other
 *   answers the same way. Only to a machine the master has heard from; MASTER_RELAY_BYTES a second a sender. */
/* Friends and stats (docs/ONLINE_PLAN.md 7, 11; tools/master/accounts.c keeps them on disk):
 * - ID (a machine with a profile online, every MASTER_REGISTER_MS): the profile's id, its secret (a profile's own,
 *   made the first time it goes online: the master keeps the first one it sees for that id), its name, where it is
 *   (0 the menus, 1 hosting a server, 2 in a server: then the host's address as it joined it). From the lobby's socket
 *   in a server, else from its own. The master answers FRIENDS (the friends, then the requests to it).
 * - FRIENDS (the master): a part of the list: part, parts, what (0 friends, 1 requests to this profile), count, then
 *   each: id, name, online (1: heard within MASTER_EXPIRE_MS), where, the server's address and its name.
 * - FRIEND_ADD: id, secret, the other's id (a friend code): a request; both asked: friends.
 * - FRIEND_ANSWER: id, secret, the other's id, 1 accept, 0 decline, 2 remove (a friend no more).
 * - RESULT (each machine at a match's end, a vanilla ruleset only): id, secret, the match's key (8 bytes: the same on
 *   every machine), the ruleset (1 vanilla), arena, mode, the count, then each player: id, car, won, kills (16 bits),
 *   deaths (16), damage (32). Counted once two machines agree (stats per player and car). */
#ifndef TM2_NET_MASTER_H
#define TM2_NET_MASTER_H

#include <stdint.h>
#include <string.h>

enum {
    MASTER_PORT = 27522,
    MASTER_CHANNEL = 0x53, /* 'S' */
    MASTER_REGISTER = 1, MASTER_KEEP = 2, MASTER_LIST = 3, MASTER_ENTRY = 4, MASTER_PUNCH = 5, MASTER_INTRO = 6,
    MASTER_PROBE = 7, MASTER_BYE = 8, MASTER_RELAY = 9, MASTER_RELAYED = 10,
    MASTER_RELAY_BYTES = 1000000, /* a machine's relayed bytes a second at most (a state's join: a few seconds of it) */
    MASTER_REGISTER_MS = 4000, /* a host's listing, a member's keep-alive: this often */
    MASTER_EXPIRE_MS = 15000,  /* not heard this long: gone from the list */
    MASTER_ADDR_BYTES = 18,    /* an address: a.b.c.d (16 bytes, 0-padded), then the port (2 bytes, low first) */
    MASTER_ID = 11, MASTER_FRIENDS = 12, MASTER_FRIEND_ADD = 13, MASTER_FRIEND_ANSWER = 14, MASTER_RESULT = 15,
    MASTER_ID_BYTES = 8, MASTER_SECRET_BYTES = 16, MASTER_NAME_BYTES = 24, MASTER_SERVER_BYTES = 32,
    MASTER_FRIENDS_MAX = 64,       /* a profile's friends (and requests to it) at most */
    MASTER_FRIENDS_PART = 12,      /* entries a FRIENDS packet */
    MASTER_FRIEND_BYTES = MASTER_ID_BYTES + MASTER_NAME_BYTES + 2 + MASTER_ADDR_BYTES + MASTER_SERVER_BYTES,
    MASTER_RESULT_PLAYERS = 16,
    MASTER_RESULT_PLAYER_BYTES = MASTER_ID_BYTES + 1 + 1 + 2 + 2 + 4
};

static inline uint8_t *master_put_addr(uint8_t *w, const char *addr, uint16_t port)
{
    memset(w, 0, 16);
    strncpy((char *)w, addr, 15);
    w[16] = (uint8_t)port;
    w[17] = (uint8_t)(port >> 8);
    return w + MASTER_ADDR_BYTES;
}
static inline const uint8_t *master_get_addr(const uint8_t *r, char addr[16], uint16_t *port)
{
    memcpy(addr, r, 16);
    addr[15] = 0;
    *port = (uint16_t)(r[16] | r[17] << 8);
    return r + MASTER_ADDR_BYTES;
}

#endif
