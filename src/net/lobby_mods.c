/* A server's mods (docs/MODDING.md 10; the user's, 2026-10-25): the host's mods that are on are the server's list;
 * a machine joining gets the list, and the mods it lacks (by uid and fingerprint: the same files) are downloaded from
 * the host once the player agrees (the UI's pop-up, up to their download limit), checked (mods/mod_files.c: names,
 * contents, hashes, sizes) and put in mods/. While in the server, its mods are on and every other mod is off
 * (mods_online_set); leaving puts Options > Mods' switches back.
 *
 * On the lobby's reliable channel (lobby_ext_send): LM_LIST (host -> machine: each mod's kind, uid, fingerprint,
 * size, id, name), LM_ASK (machine -> host: the mods it wants, a bit each), LM_FILES (host: one mod's file list),
 * LM_PART (host: 8 KB of a file, in order), LM_FAIL (host: it cannot send that mod). A player's skin travels as an
 * index in the list (lobby_set_skin, LobbyStart.skin), the same on every machine whatever order their mods are in. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lobby.h"
#include "lobby_mods.h"
#include "../mods/mods.h"
#include "../mods/mod_files.h"

enum { LM_LIST = LOBBY_EXT_FIRST, LM_ASK, LM_FILES, LM_PART, LM_FAIL };
enum { PART_BYTES = 8192 };

static Lobby *bound;
static LobbyMod list[LOBBY_MODS_MAX];
static int list_n, list_known;
static struct {              /* (host) each machine's downloads */
    int listed;              /* the list sent since it came */
    uint32_t want;           /* the mods it asked for, a bit each */
    int mod, file, nfiles;   /* the one on its way (mod -1: none) */
    long off;
    ModFile files[MOD_FILES_MAX];
    char dir[700];
} serve[SES_MAX_NODES];
static ModReceive rx;        /* (a machine) the mod coming in */
static int rx_mod = -1, state;
static uint32_t asked;
static long done_bytes, total_bytes, limit_bytes;
static char why[160];

static uint8_t *put32(uint8_t *w, uint32_t v) { w[0] = (uint8_t)v; w[1] = (uint8_t)(v >> 8); w[2] = (uint8_t)(v >> 16); w[3] = (uint8_t)(v >> 24); return w + 4; }
static uint32_t get32(const uint8_t *r) { return r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24; }
static uint8_t *put64(uint8_t *w, uint64_t v) { w = put32(w, (uint32_t)v); return put32(w, (uint32_t)(v >> 32)); }
static uint64_t get64(const uint8_t *r) { return get32(r) | (uint64_t)get32(r + 4) << 32; }
static uint8_t *put_str(uint8_t *w, const char *s, int max)
{
    int n = (int)strlen(s);
    if (n > max) n = max;
    *w++ = (uint8_t)n;
    memcpy(w, s, (size_t)n);
    return w + n;
}
static const uint8_t *get_str(const uint8_t *r, const uint8_t *end, char *out, int size)
{
    if (r >= end) return NULL;
    int n = *r++;
    if (n >= size || end - r < n) return NULL;
    memcpy(out, r, (size_t)n);
    out[n] = 0;
    return r + n;
}

static void set_online(void) /* the server's mods on, every other off */
{
    static char uids[LOBBY_MODS_MAX][40];
    for (int i = 0; i < list_n; i++) snprintf(uids[i], sizeof uids[i], "%s", list[i].uid);
    mods_online_set(uids, list_n);
}
static int have(int i) { return mods_find(list[i].uid, list[i].fingerprint) >= 0; }

/* ---- a machine: the list, the download ---- */
static void ask_next_check(void)
{
    if (asked) return;
    mods_rescan(); /* (the new ones in mods/) */
    for (int i = 0; i < list_n; i++)
        if (!have(i)) {
            state = LOBBY_MODS_FAILED;
            snprintf(why, sizeof why, "%s arrived but cannot be used", list[i].name);
            return;
        }
    set_online();
    state = LOBBY_MODS_READY;
    fprintf(stderr, "mods: the server's %d mod%s ready\n", list_n, list_n == 1 ? "" : "s");
}
static void fail(const char *text)
{
    if (rx.open) mod_receive_cancel(&rx);
    state = LOBBY_MODS_FAILED;
    snprintf(why, sizeof why, "%s", text);
    fprintf(stderr, "mods: download failed: %s\n", why);
}
static void got_list(const uint8_t *d, int len)
{
    const uint8_t *r = d + 2, *end = d + len;
    int n = len >= 2 ? d[1] : 0;
    list_n = 0;
    for (int i = 0; i < n && i < LOBBY_MODS_MAX; i++) {
        LobbyMod *m = &list[list_n];
        if (end - r < 1 + 32 + 8 + 4) break;
        m->kind = *r++;
        memcpy(m->uid, r, 32);
        m->uid[32] = 0;
        r += 32;
        m->fingerprint = get64(r);
        r += 8;
        m->bytes = get32(r);
        r += 4;
        if (!(r = get_str(r, end, m->id, sizeof m->id)) || !(r = get_str(r, end, m->name, sizeof m->name))) break;
        list_n++;
    }
    list_known = 1;
    long missing = 0;
    int count = lobby_mods_missing(&missing);
    if (!count) { set_online(); state = LOBBY_MODS_READY; }
    else state = LOBBY_MODS_ASK;
    fprintf(stderr, "mods: the server uses %d mod%s, %d missing (%ld bytes)\n", list_n, list_n == 1 ? "" : "s", count, missing);
}
static void got_files(const uint8_t *d, int len)
{
    const uint8_t *r = d + 3, *end = d + len;
    if (len < 3 || d[1] >= list_n || !(asked & 1u << d[1]) || state != LOBBY_MODS_DOWNLOADING) return;
    int i = d[1], n = d[2];
    ModFile files[MOD_FILES_MAX];
    for (int k = 0; k < n && k < MOD_FILES_MAX; k++) {
        if (!(r = get_str(r, end, files[k].name, sizeof files[k].name)) || end - r < 12) { fail("a damaged file list"); return; }
        files[k].size = (long)get32(r);
        files[k].hash = get64(r + 4);
        r += 12;
    }
    const LobbyMod *m = &list[i];
    if (!mod_receive_begin(&rx, mods_game_folder(), m->kind, m->id, m->uid, m->fingerprint, files, n, limit_bytes)) {
        char t[160];
        snprintf(t, sizeof t, "%s: %s", m->name, rx.why);
        fail(t);
        return;
    }
    rx_mod = i;
}
static void got_part(const uint8_t *d, int len)
{
    if (len < 9 || d[1] != rx_mod || !rx.open || state != LOBBY_MODS_DOWNLOADING) return;
    int file = d[2], n = d[7] | d[8] << 8;
    long off = (long)get32(d + 3);
    if (len < 9 + n || !mod_receive_chunk(&rx, file, off, d + 9, n)) {
        char t[160];
        snprintf(t, sizeof t, "%s: %s", list[rx_mod].name, rx.why);
        fail(t);
        return;
    }
    done_bytes += n;
    if (rx.done < rx.total) return;
    char where[700];
    if (!mod_receive_finish(&rx, where, sizeof where)) {
        char t[160];
        snprintf(t, sizeof t, "%s: %s", list[rx_mod].name, rx.why);
        fail(t);
        return;
    }
    fprintf(stderr, "mods: %s downloaded (%s)\n", list[rx_mod].name, where);
    asked &= ~(1u << rx_mod);
    rx_mod = -1;
    ask_next_check();
}

/* ---- the host ---- */
static void got_ask(int from, const uint8_t *d, int len)
{
    if (len < 5 || from < 0 || from >= SES_MAX_NODES) return;
    uint32_t want = get32(d + 1);
    if (list_n < 32) want &= (1u << list_n) - 1;
    serve[from].want |= want;
}
static void send_list(int to)
{
    static uint8_t m[SES_APP_MAX];
    uint8_t *w = m;
    *w++ = LM_LIST;
    *w++ = (uint8_t)list_n;
    for (int i = 0; i < list_n; i++) {
        *w++ = list[i].kind;
        memcpy(w, list[i].uid, 32);
        w += 32;
        w = put64(w, list[i].fingerprint);
        w = put32(w, list[i].bytes);
        w = put_str(w, list[i].id, 47);
        w = put_str(w, list[i].name, 63);
    }
    lobby_ext_send(bound, to, m, (int)(w - m));
}
static void serve_one(int n) /* (host) the next piece for machine n */
{
    if (serve[n].mod < 0) {
        int i = 0;
        while (i < list_n && !(serve[n].want & 1u << i)) i++;
        if (i >= list_n) { serve[n].want = 0; return; }
        int local = mods_find(list[i].uid, list[i].fingerprint);
        uint64_t fp;
        serve[n].nfiles = local < 0 ? -1 : mod_package(mods_get(local)->dir, list[i].kind, serve[n].files, MOD_FILES_MAX, &fp);
        if (serve[n].nfiles <= 0 || fp != list[i].fingerprint) { /* (changed or gone since the server started) */
            uint8_t m[2] = {LM_FAIL, (uint8_t)i};
            lobby_ext_send(bound, n, m, 2);
            serve[n].want &= ~(1u << i);
            return;
        }
        snprintf(serve[n].dir, sizeof serve[n].dir, "%s", mods_get(local)->dir);
        static uint8_t m[SES_APP_MAX];
        uint8_t *w = m;
        *w++ = LM_FILES;
        *w++ = (uint8_t)i;
        *w++ = (uint8_t)serve[n].nfiles;
        for (int k = 0; k < serve[n].nfiles; k++) {
            w = put_str(w, serve[n].files[k].name, MOD_FILE_NAME - 1);
            w = put32(w, (uint32_t)serve[n].files[k].size);
            w = put64(w, serve[n].files[k].hash);
        }
        lobby_ext_send(bound, n, m, (int)(w - m));
        serve[n].mod = i;
        serve[n].file = 0;
        serve[n].off = 0;
        return;
    }
    int i = serve[n].mod, k = serve[n].file;
    if (k >= serve[n].nfiles) { serve[n].want &= ~(1u << i); serve[n].mod = -1; return; }
    static uint8_t m[9 + PART_BYTES];
    char path[800];
    snprintf(path, sizeof path, "%s%s", serve[n].dir, serve[n].files[k].name);
    FILE *f = fopen(path, "rb");
    long left = serve[n].files[k].size - serve[n].off;
    int len = left < PART_BYTES ? (int)left : PART_BYTES;
    if (!f || fseek(f, serve[n].off, SEEK_SET) || fread(m + 9, 1, (size_t)len, f) != (size_t)len) {
        if (f) fclose(f);
        uint8_t fm[2] = {LM_FAIL, (uint8_t)i};
        lobby_ext_send(bound, n, fm, 2);
        serve[n].want &= ~(1u << i);
        serve[n].mod = -1;
        return;
    }
    fclose(f);
    m[0] = LM_PART;
    m[1] = (uint8_t)i;
    m[2] = (uint8_t)k;
    put32(m + 3, (uint32_t)serve[n].off);
    m[7] = (uint8_t)len;
    m[8] = (uint8_t)(len >> 8);
    lobby_ext_send(bound, n, m, 9 + len);
    serve[n].off += len;
    if (serve[n].off >= serve[n].files[k].size) { serve[n].file++; serve[n].off = 0; }
}

static void on_ext(void *ctx, int from, const uint8_t *d, int len)
{
    (void)ctx;
    if (!bound || len < 1) return;
    int host = lobby_is_host(bound), host_node = lobby_host_node(bound);
    if (!host && from == host_node && d[0] == LM_LIST) got_list(d, len);
    else if (!host && from == host_node && d[0] == LM_FILES) got_files(d, len);
    else if (!host && from == host_node && d[0] == LM_PART) got_part(d, len);
    else if (!host && from == host_node && d[0] == LM_FAIL && len >= 2 && d[1] < list_n) {
        char t[160];
        snprintf(t, sizeof t, "the server could not send %s", list[d[1]].name);
        fail(t);
    } else if (host && d[0] == LM_ASK) got_ask(from, d, len);
}

/* ---- the UI's ---- */
void lobby_mods_attach(Lobby *l)
{
    lobby_mods_detach();
    bound = l;
    lobby_on_ext(l, on_ext, NULL);
    for (int n = 0; n < SES_MAX_NODES; n++) serve[n].mod = -1;
    if (!lobby_is_host(l)) { state = LOBBY_MODS_WAITING; return; }
    for (int i = 0; i < mods_count() && list_n < LOBBY_MODS_MAX; i++) { /* the host's skins and characters that are on
                                                                           * (arenas later) */
        const Mod *m = mods_get(i);
        if ((m->kind != MOD_SKIN && m->kind != MOD_CHARACTER) || !mod_on(m)) continue;
        LobbyMod *e = &list[list_n++];
        e->kind = (uint8_t)m->kind;
        snprintf(e->id, sizeof e->id, "%s", m->id);
        snprintf(e->uid, sizeof e->uid, "%s", m->uid);
        snprintf(e->name, sizeof e->name, "%s", m->name);
        e->fingerprint = m->fingerprint;
        e->bytes = (uint32_t)m->bytes;
    }
    list_known = 1;
    set_online();
    state = LOBBY_MODS_READY;
    if (list_n) fprintf(stderr, "mods: this server uses %d mod%s\n", list_n, list_n == 1 ? "" : "s");
}
void lobby_mods_detach(void)
{
    if (rx.open) mod_receive_cancel(&rx);
    if (bound) lobby_on_ext(bound, NULL, NULL);
    bound = NULL;
    list_n = list_known = 0;
    rx_mod = -1;
    asked = 0;
    done_bytes = total_bytes = 0;
    why[0] = 0;
    state = LOBBY_MODS_NONE;
    memset(serve, 0, sizeof serve);
    if (mods_online()) mods_online_set(NULL, 0);
}
void lobby_mods_frame(void)
{
    if (!bound || !lobby_is_host(bound)) return;
    const LobbyView *v = lobby_view(bound);
    int me = lobby_node(bound);
    for (int n = 0; n < SES_MAX_NODES; n++) {
        if (n == me || !v->m[n].present) { serve[n].listed = 0; serve[n].want = 0; serve[n].mod = -1; continue; }
        if (!serve[n].listed) { send_list(n); serve[n].listed = 1; }
        if (serve[n].want) serve_one(n);
    }
}
int lobby_mods_count(void) { return list_n; }
const LobbyMod *lobby_mods_get(int i) { return i >= 0 && i < list_n ? &list[i] : NULL; }
int lobby_mods_missing(long *bytes)
{
    int n = 0;
    long b = 0;
    for (int i = 0; i < list_n; i++)
        if (!have(i)) { n++; b += list[i].bytes; }
    if (bytes) *bytes = b;
    return n;
}
void lobby_mods_download(long limit)
{
    if (!bound || lobby_is_host(bound) || state != LOBBY_MODS_ASK) return;
    asked = 0;
    total_bytes = done_bytes = 0;
    for (int i = 0; i < list_n; i++)
        if (!have(i)) { asked |= 1u << i; total_bytes += list[i].bytes; }
    limit_bytes = limit;
    if (limit > 0 && total_bytes > limit) {
        char t[160];
        snprintf(t, sizeof t, "%.1f MB, over your download limit (Online Services)", total_bytes / 1048576.0);
        fail(t);
        return;
    }
    uint8_t m[5];
    m[0] = LM_ASK;
    put32(m + 1, asked);
    lobby_ext_send(bound, lobby_host_node(bound), m, 5);
    state = LOBBY_MODS_DOWNLOADING;
}
int lobby_mods_state(long *done, long *total, const char **reason)
{
    if (done) *done = done_bytes;
    if (total) *total = total_bytes;
    if (reason) *reason = why;
    return state;
}
/* a player's skin across machines: the list's index of a local car number's skin, and back */
int lobby_mods_skin_index(int number)
{
    int mod_car_mod(int number); /* vplus/mod_cars.c: a skin's mod index */
    if (number == LOOK_DT2) return LOBBY_SKIN_DT2; /* (Dark Tooth's second phase: no mod, every machine's) */
    int k = mod_car_mod(number);
    const Mod *m = k >= 0 ? mods_get(k) : NULL;
    for (int i = 0; m && i < list_n; i++)
        if (!strcmp(list[i].uid, m->uid) && list[i].fingerprint == m->fingerprint) return i;
    return -1;
}
int lobby_mods_skin_number(int index)
{
    int mod_car_number_of(int mod_index); /* vplus/mod_cars.c */
    if (index == LOBBY_SKIN_DT2) return LOOK_DT2;
    if (index < 0 || index >= list_n) return -1;
    int k = mods_find(list[index].uid, list[index].fingerprint);
    return k >= 0 ? mod_car_number_of(k) : -1;
}
