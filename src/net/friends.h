/* Friends and presence through the master servers (src/net/master.h: ID, FRIENDS, FRIEND_ADD, FRIEND_ANSWER, RESULT;
 * docs/ONLINE_PLAN.md 7, 11). A profile's id is 5 random bytes (its friend code: 8 letters and digits), its secret 16
 * (made the first time it goes online: profile.c). */
#pragma once
#include <stdint.h>
#include "master.h"

typedef struct {
    uint8_t id[MASTER_ID_BYTES];
    char name[MASTER_NAME_BYTES];
    int online, where;                 /* where: 0 the menus, 1 hosting, 2 in a server */
    char addr[16];                     /* (where 1-2) the server's address and port, its name */
    uint16_t port;
    char server[MASTER_SERVER_BYTES];
} Friend;

void friends_identity(const uint8_t id[MASTER_ID_BYTES], const uint8_t secret[MASTER_SECRET_BYTES], const char *name);
void friends_masters(const char *list, int loopback); /* "host:port, ..." (none: off) */
/* each menu frame while Network Play's online pages are up (or a server is joined: the lobby's socket says where) */
void friends_poll(uint32_t now_ms, int active, int in_server);
void friends_packet(const uint8_t *d, int n); /* a master's FRIENDS (from any socket) */
int friends_count(int requests);              /* 0 friends, 1 the requests to this profile */
const Friend *friends_get(int requests, int i);
int friends_is(const uint8_t id[MASTER_ID_BYTES]); /* a friend of this profile */
void friends_add(const uint8_t id[MASTER_ID_BYTES]);
void friends_answer(const uint8_t id[MASTER_ID_BYTES], int how); /* 1 accept, 0 decline, 2 remove */
void friends_code(const uint8_t id[MASTER_ID_BYTES], char out[10]); /* "XXXX-XXXX" */
int friends_parse_code(const char *code, uint8_t id[MASTER_ID_BYTES]); /* 0: not a code */
const uint8_t *friends_my_id(void); /* NULL: no identity yet */
/* the lobby's socket: the ID with where it is (1 hosting, 2 in a server: the host's address), for lobby.c */
int friends_id_packet(uint8_t *out, int where, const char *host_addr, uint16_t host_port);
/* a match's result (every machine; counted when two agree): players' ids, cars, wins, kills, deaths, damage */
typedef struct { uint8_t id[MASTER_ID_BYTES]; uint8_t car, won; uint16_t kills, deaths; uint32_t damage; } FriendResult;
void friends_report(const uint8_t key[8], int vanilla, int arena, int mode, const FriendResult *p, int n);
