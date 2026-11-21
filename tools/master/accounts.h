/* The master server's players, friends and stats (accounts.c; the messages: src/net/master.h). */
#pragma once
#include <stdint.h>
#include "master.h"
#include "net_udp.h"

/* a server's name by its address (the master's listings): into out (size bytes), "" if none */
typedef void (*AccountsServerName)(const char *addr, uint16_t port, char *out, int size);
void accounts_load(const char *path, AccountsServerName names);
void accounts_tick(uint32_t now);  /* written to disk now and then while changed */
void accounts_flush(void);         /* written now (changed) */
void accounts_id(NetUdp *u, const uint8_t *d, int n, const char *addr, uint16_t port, uint32_t now);
void accounts_friend_add(const uint8_t *d, int n);
void accounts_friend_answer(const uint8_t *d, int n);
void accounts_result(const uint8_t *d, int n, uint32_t now);
