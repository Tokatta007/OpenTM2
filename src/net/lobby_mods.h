/* A server's mods: the list, downloading the ones a machine lacks, a player's skin across machines (lobby_mods.c). */
#ifndef TM2_NET_LOBBY_MODS_H
#define TM2_NET_LOBBY_MODS_H
#include <stdint.h>
#include "lobby.h"

enum { LOBBY_MODS_MAX = 32 };
enum { LOBBY_MODS_NONE, LOBBY_MODS_WAITING /* for the host's list */, LOBBY_MODS_ASK /* some missing: ask the player */,
       LOBBY_MODS_DOWNLOADING, LOBBY_MODS_READY, LOBBY_MODS_FAILED };
typedef struct {
    uint8_t kind;            /* MOD_* (mods/mods.h) */
    char id[48], uid[40], name[64];
    uint64_t fingerprint;
    uint32_t bytes;
} LobbyMod;

void lobby_mods_attach(Lobby *l); /* after hosting or joining (the host's mods that are on become the server's) */
void lobby_mods_detach(void);     /* leaving: Options > Mods' switches again */
void lobby_mods_frame(void);      /* each frame while in a server (the host sends the list and the files) */
int lobby_mods_count(void);
const LobbyMod *lobby_mods_get(int i);
int lobby_mods_missing(long *bytes);  /* the server's mods this machine lacks (count, their size) */
void lobby_mods_download(long limit); /* the player agreed: get them (limit: bytes, 0 none) */
int lobby_mods_state(long *done, long *total, const char **reason);
int lobby_mods_skin_index(int number); /* a local car number's skin: its index in the server's list, -1 */
int lobby_mods_skin_number(int index); /* that index's skin here: a local car number, -1 */
#endif
