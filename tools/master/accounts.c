/* The master server's players (src/net/master.h: ID, FRIENDS, FRIEND_ADD, FRIEND_ANSWER, RESULT): each profile by its
 * id (its secret checked), its name and where it is; the friends and the requests between them; the stats per player
 * and car from the matches two machines agreed on. Kept on disk (a text file, written now and then while changed:
 * a temporary file, then renamed), read at start. */
#include "accounts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { PLAYERS_MAX = 1 << 16, CARS = 16, PENDING = 256, VARIANTS = 4, SAVE_EVERY_MS = 30000, DONE_KEEP_MS = 600000 };

typedef struct { uint32_t matches, wins, kills, deaths; uint64_t damage; } CarStats;
typedef struct {
    int used;
    uint8_t id[MASTER_ID_BYTES], secret[MASTER_SECRET_BYTES];
    char name[MASTER_NAME_BYTES];
    uint32_t heard;                   /* (this run) last ID; 0 never */
    uint8_t where;
    char srv[16];                     /* the server it is in (where 1: its own address; 2: the host's) */
    uint16_t srv_port;
    int64_t seen;                     /* (on disk) its last ID, Unix time */
    int *friends, nf, *req, nr;       /* friends; requests to it (each the other's index) */
    CarStats *stats;                  /* (once it has some) per car */
} Player;
static Player *pl;
static int pl_n;
static int *slot;                     /* (an index of ids: PLAYERS_MAX * 2 open-addressing slots, -1 empty) */
static int dirty;
static uint32_t saved_ms;
static char path_db[1024];
static AccountsServerName server_name;

typedef struct {
    int used, done;
    uint8_t key[8];
    uint32_t at;
    int nv;
    struct { uint32_t hash; int votes; uint8_t reporters[VARIANTS][MASTER_ID_BYTES]; int n; uint8_t data[2 + MASTER_RESULT_PLAYERS * MASTER_RESULT_PLAYER_BYTES]; int len; } v[VARIANTS];
} Pending;
static Pending pend[PENDING];
/* the totals (the website's first stats: no players): every match counted, and per car (picked: a player drove it) */
static struct { uint64_t matches, kills, deaths, damage; } total;
static CarStats car_total[CARS];

static uint32_t hash_id(const uint8_t *id) { uint32_t h = 2166136261u; for (int i = 0; i < MASTER_ID_BYTES; i++) h = (h ^ id[i]) * 16777619u; return h; }
static int find(const uint8_t *id) /* its index, or -1 */
{
    for (uint32_t i = hash_id(id) % (PLAYERS_MAX * 2), k = 0; k < PLAYERS_MAX * 2; i = (i + 1) % (PLAYERS_MAX * 2), k++) {
        if (slot[i] < 0) return -1;
        if (!memcmp(pl[slot[i]].id, id, MASTER_ID_BYTES)) return slot[i];
    }
    return -1;
}
static int make(const uint8_t *id) /* a new player (its index), or -1 (full) */
{
    if (pl_n >= PLAYERS_MAX) return -1;
    uint32_t i = hash_id(id) % (PLAYERS_MAX * 2);
    while (slot[i] >= 0) i = (i + 1) % (PLAYERS_MAX * 2);
    slot[i] = pl_n;
    Player *p = &pl[pl_n];
    memset(p, 0, sizeof *p);
    p->used = 1;
    memcpy(p->id, id, MASTER_ID_BYTES);
    dirty = 1;
    return pl_n++;
}
static void add_to(int **list, int *n, int v)
{
    for (int i = 0; i < *n; i++) if ((*list)[i] == v) return;
    if (*n >= MASTER_FRIENDS_MAX) return;
    int *l = realloc(*list, sizeof(int) * (size_t)(*n + 1));
    if (!l) return;
    *list = l;
    (*list)[(*n)++] = v;
}
static void remove_from(int *list, int *n, int v)
{
    for (int i = 0; i < *n; i++) if (list[i] == v) { list[i] = list[--*n]; return; }
}
static int has(const int *list, int n, int v) { for (int i = 0; i < n; i++) if (list[i] == v) return 1; return 0; }

static void hex(char *out, const uint8_t *b, int n) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static int unhex(uint8_t *b, const char *s, int n)
{
    for (int i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0; b[i] = (uint8_t)v; }
    return 1;
}

void accounts_load(const char *path, AccountsServerName names)
{
    snprintf(path_db, sizeof path_db, "%s", path);
    server_name = names;
    pl = calloc(PLAYERS_MAX, sizeof *pl);
    slot = malloc(sizeof(int) * PLAYERS_MAX * 2);
    for (int i = 0; i < PLAYERS_MAX * 2; i++) slot[i] = -1;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "master: no %s yet (made at the first change)\n", path); return; }
    char line[512];
    int friends = 0, stats = 0;
    while (fgets(line, sizeof line, f)) {
        char a[64], b[64], name[64] = "";
        uint8_t ia[MASTER_ID_BYTES], ib[MASTER_ID_BYTES], sec[MASTER_SECRET_BYTES];
        long long seen;
        unsigned car, m1, w, k, d;
        unsigned long long dmg;
        if (line[0] == 'P' && sscanf(line, "P %16s %32s %lld %63[^\n]", a, b, &seen, name) >= 3 && unhex(ia, a, MASTER_ID_BYTES) &&
            unhex(sec, b, MASTER_SECRET_BYTES)) {
            int i = find(ia) >= 0 ? find(ia) : make(ia);
            if (i < 0) continue;
            memcpy(pl[i].secret, sec, sizeof sec);
            pl[i].seen = seen;
            snprintf(pl[i].name, sizeof pl[i].name, "%s", name);
        } else if ((line[0] == 'F' || line[0] == 'R') && sscanf(line + 2, "%16s %16s", a, b) == 2 && unhex(ia, a, MASTER_ID_BYTES) &&
                   unhex(ib, b, MASTER_ID_BYTES)) {
            int x = find(ia), y = find(ib);
            if (x < 0 || y < 0) continue;
            if (line[0] == 'F') { add_to(&pl[x].friends, &pl[x].nf, y); add_to(&pl[y].friends, &pl[y].nf, x); friends++; }
            else add_to(&pl[y].req, &pl[y].nr, x); /* (R from to) */
        } else if (line[0] == 'G' && sscanf(line, "G %u %u %u %llu", &m1, &k, &d, &dmg) == 4) {
            total.matches = m1, total.kills = k, total.deaths = d, total.damage = dmg;
        } else if (line[0] == 'C' && sscanf(line, "C %u %u %u %u %u %llu", &car, &m1, &w, &k, &d, &dmg) == 6 && car < CARS) {
            car_total[car] = (CarStats){m1, w, k, d, dmg};
        } else if (line[0] == 'S' && sscanf(line, "S %16s %u %u %u %u %u %llu", a, &car, &m1, &w, &k, &d, &dmg) == 7 &&
                   unhex(ia, a, MASTER_ID_BYTES) && car < CARS) {
            int x = find(ia);
            if (x < 0) continue;
            if (!pl[x].stats && !(pl[x].stats = calloc(CARS, sizeof(CarStats)))) continue;
            pl[x].stats[car] = (CarStats){m1, w, k, d, dmg};
            stats++;
        }
    }
    fclose(f);
    dirty = 0;
    fprintf(stderr, "master: %s: %d players, %d friendships, %d stats rows\n", path, pl_n, friends, stats);
}

/* The public stats (docs/STATS_FORMAT.md): the totals and each car's, no player, no id or secret, as JSON beside the
 * database (tm2_stats.json in its folder), written with it: the website reads this file, never the database. Cars by
 * name in car-select order (the game's src/profile/profile.c, then Dark Tooth). */
static const char *const car_names[CARS] = {
    "Hammerhead", "Outlaw 2", "Warthog", "Mr. Grimm", "Grasshopper", "Thumper", "Spectre", "Roadkill",
    "Twister", "Axel", "Mr. Slam", "Shadow", "Sweet Tooth", "Minion", "Dark Tooth", NULL,
};
static void save_public(void)
{
    char out[1100], tmp[1120];
    snprintf(out, sizeof out, "%s", path_db);
    char *slash = strrchr(out, '/'), *bslash = strrchr(out, '\\');
    if (bslash > slash) slash = bslash;
    snprintf(slash ? slash + 1 : out, sizeof out - (size_t)(slash ? slash + 1 - out : 0), "tm2_stats.json");
    snprintf(tmp, sizeof tmp, "%s.new", out);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "master: cannot write %s\n", tmp); return; }
    fprintf(f, "{\n  \"format\": 1,\n  \"updated\": %lld,\n", (long long)time(NULL));
    fprintf(f, "  \"matches\": %llu,\n  \"kills\": %llu,\n  \"deaths\": %llu,\n  \"damage\": %llu,\n",
            (unsigned long long)total.matches, (unsigned long long)total.kills, (unsigned long long)total.deaths,
            (unsigned long long)total.damage);
    fprintf(f, "  \"cars\": [");
    int first = 1;
    for (int c = 0; c < CARS; c++) {
        if (!car_total[c].matches) continue;
        fprintf(f, "%s\n    {\"car\": %d, \"name\": \"%s\", \"matches\": %u, \"wins\": %u, \"kills\": %u, \"deaths\": %u, "
                   "\"damage\": %llu}", first ? "" : ",", c, car_names[c] ? car_names[c] : "?", car_total[c].matches,
                car_total[c].wins, car_total[c].kills, car_total[c].deaths, (unsigned long long)car_total[c].damage);
        first = 0;
    }
    fprintf(f, "%s]\n}\n", first ? "" : "\n  ");
    if (fclose(f)) return;
    remove(out);
    if (rename(tmp, out)) fprintf(stderr, "master: cannot write %s\n", out);
}
static void save(void)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", path_db);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "master: cannot write %s\n", tmp); return; }
    fprintf(f, "# tm2_master: P id secret seen name / F a b (friends) / R from to (a request) / S id car matches wins kills deaths damage\n"
               "# G matches kills deaths damage (every match) / C car picked wins kills deaths damage (per car, car-select order)\n");
    fprintf(f, "G %llu %llu %llu %llu\n", (unsigned long long)total.matches, (unsigned long long)total.kills,
            (unsigned long long)total.deaths, (unsigned long long)total.damage);
    for (int c = 0; c < CARS; c++)
        if (car_total[c].matches)
            fprintf(f, "C %d %u %u %u %u %llu\n", c, car_total[c].matches, car_total[c].wins, car_total[c].kills, car_total[c].deaths,
                    (unsigned long long)car_total[c].damage);
    char a[2 * MASTER_ID_BYTES + 1], b[2 * MASTER_SECRET_BYTES + 1];
    for (int i = 0; i < pl_n; i++) {
        hex(a, pl[i].id, MASTER_ID_BYTES);
        hex(b, pl[i].secret, MASTER_SECRET_BYTES);
        fprintf(f, "P %s %s %lld %s\n", a, b, (long long)pl[i].seen, pl[i].name);
    }
    for (int i = 0; i < pl_n; i++) {
        hex(a, pl[i].id, MASTER_ID_BYTES);
        for (int k = 0; k < pl[i].nf; k++) if (pl[i].friends[k] > i) { hex(b, pl[pl[i].friends[k]].id, MASTER_ID_BYTES); fprintf(f, "F %s %s\n", a, b); }
        for (int k = 0; k < pl[i].nr; k++) { hex(b, pl[pl[i].req[k]].id, MASTER_ID_BYTES); fprintf(f, "R %s %s\n", b, a); }
        for (int c = 0; pl[i].stats && c < CARS; c++) {
            const CarStats *s = &pl[i].stats[c];
            if (s->matches) fprintf(f, "S %s %d %u %u %u %u %llu\n", a, c, s->matches, s->wins, s->kills, s->deaths, (unsigned long long)s->damage);
        }
    }
    if (fclose(f)) return;
    remove(path_db);
    if (rename(tmp, path_db)) fprintf(stderr, "master: cannot write %s\n", path_db);
    dirty = 0;
    save_public();
}
void accounts_tick(uint32_t now)
{
    static uint32_t every;
    if (!every) every = getenv("TM2_MASTER_SAVE_MS") ? (uint32_t)atoi(getenv("TM2_MASTER_SAVE_MS")) : SAVE_EVERY_MS; /* (tests: sooner) */
    if (dirty && now - saved_ms >= every) { saved_ms = now; save(); }
    for (int i = 0; i < PENDING; i++) if (pend[i].used && now - pend[i].at > DONE_KEEP_MS) pend[i].used = 0;
}
void accounts_flush(void) { if (dirty) save(); }

/* the sender, by its id and secret (a new id: its secret kept); -1 if the secret is not the one kept */
static int who(const uint8_t *d)
{
    int i = find(d);
    if (i < 0) {
        if ((i = make(d)) < 0) return -1;
        memcpy(pl[i].secret, d + MASTER_ID_BYTES, MASTER_SECRET_BYTES);
        return i;
    }
    return memcmp(pl[i].secret, d + MASTER_ID_BYTES, MASTER_SECRET_BYTES) ? -1 : i;
}
static uint8_t *put_entry(uint8_t *w, const Player *p, uint32_t now)
{
    memcpy(w, p->id, MASTER_ID_BYTES); w += MASTER_ID_BYTES;
    memcpy(w, p->name, MASTER_NAME_BYTES); w += MASTER_NAME_BYTES;
    int online = p->heard && now - p->heard <= MASTER_EXPIRE_MS;
    *w++ = (uint8_t)online;
    *w++ = online ? p->where : 0;
    w = master_put_addr(w, online && p->where ? p->srv : "", online && p->where ? p->srv_port : 0);
    memset(w, 0, MASTER_SERVER_BYTES);
    if (online && p->where && server_name) server_name(p->srv, p->srv_port, (char *)w, MASTER_SERVER_BYTES);
    return w + MASTER_SERVER_BYTES;
}
static void send_lists(NetUdp *u, const Player *p, const char *addr, uint16_t port, uint32_t now)
{
    for (int what = 0; what < 2; what++) {
        const int *list = what ? p->req : p->friends;
        int n = what ? p->nr : p->nf, parts = n ? (n + MASTER_FRIENDS_PART - 1) / MASTER_FRIENDS_PART : 1;
        for (int part = 0; part < parts; part++) {
            uint8_t out[6 + MASTER_FRIENDS_PART * MASTER_FRIEND_BYTES], *w = out;
            *w++ = MASTER_CHANNEL; *w++ = MASTER_FRIENDS; *w++ = (uint8_t)part; *w++ = (uint8_t)parts; *w++ = (uint8_t)what;
            uint8_t *count = w++;
            *count = 0;
            for (int k = part * MASTER_FRIENDS_PART; k < n && k < (part + 1) * MASTER_FRIENDS_PART; k++, (*count)++)
                w = put_entry(w, &pl[list[k]], now);
            udp_send_to(u, addr, port, out, (int)(w - out));
        }
    }
}

void accounts_id(NetUdp *u, const uint8_t *d, int n, const char *addr, uint16_t port, uint32_t now)
{
    if (n < MASTER_ID_BYTES + MASTER_SECRET_BYTES + MASTER_NAME_BYTES + 1) return;
    int i = who(d);
    if (i < 0) return;
    Player *p = &pl[i];
    const uint8_t *r = d + MASTER_ID_BYTES + MASTER_SECRET_BYTES;
    char name[MASTER_NAME_BYTES];
    memcpy(name, r, MASTER_NAME_BYTES);
    name[MASTER_NAME_BYTES - 1] = 0;
    for (char *q = name; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
    if (strcmp(name, p->name)) { memcpy(p->name, name, sizeof name); dirty = 1; }
    r += MASTER_NAME_BYTES;
    p->where = *r++;
    if (p->where == 1) { snprintf(p->srv, sizeof p->srv, "%s", addr); p->srv_port = port; }
    else if (p->where == 2 && n >= (int)(r - d) + MASTER_ADDR_BYTES) master_get_addr(r, p->srv, &p->srv_port);
    else p->where = 0;
    if (!p->heard) fprintf(stderr, "master: %s is online\n", p->name);
    p->heard = now;
    int64_t t = (int64_t)time(NULL);
    if (t - p->seen > 3600) { p->seen = t; dirty = 1; } /* (on disk: to the hour) */
    send_lists(u, p, addr, port, now);
}

void accounts_friend_add(const uint8_t *d, int n)
{
    if (n < MASTER_ID_BYTES * 2 + MASTER_SECRET_BYTES) return;
    int i = who(d), j = find(d + MASTER_ID_BYTES + MASTER_SECRET_BYTES);
    if (i < 0 || j < 0 || i == j || has(pl[i].friends, pl[i].nf, j)) return;
    if (has(pl[i].req, pl[i].nr, j)) { /* both asked: friends */
        remove_from(pl[i].req, &pl[i].nr, j);
        add_to(&pl[i].friends, &pl[i].nf, j);
        add_to(&pl[j].friends, &pl[j].nf, i);
        fprintf(stderr, "master: %s and %s are friends\n", pl[i].name, pl[j].name);
    } else {
        add_to(&pl[j].req, &pl[j].nr, i);
        fprintf(stderr, "master: %s asked %s to be friends\n", pl[i].name, pl[j].name);
    }
    dirty = 1;
}

void accounts_friend_answer(const uint8_t *d, int n)
{
    if (n < MASTER_ID_BYTES * 2 + MASTER_SECRET_BYTES + 1) return;
    int i = who(d), j = find(d + MASTER_ID_BYTES + MASTER_SECRET_BYTES), how = d[MASTER_ID_BYTES * 2 + MASTER_SECRET_BYTES];
    if (i < 0 || j < 0) return;
    if (how == 1 && has(pl[i].req, pl[i].nr, j)) {
        remove_from(pl[i].req, &pl[i].nr, j);
        add_to(&pl[i].friends, &pl[i].nf, j);
        add_to(&pl[j].friends, &pl[j].nf, i);
        fprintf(stderr, "master: %s and %s are friends\n", pl[i].name, pl[j].name);
    } else if (how == 0) remove_from(pl[i].req, &pl[i].nr, j);
    else if (how == 2) { remove_from(pl[i].friends, &pl[i].nf, j); remove_from(pl[j].friends, &pl[j].nf, i); }
    dirty = 1;
}

/* a match's result from one machine: counted once another machine sends the same */
void accounts_result(const uint8_t *d, int n, uint32_t now)
{
    int head = MASTER_ID_BYTES + MASTER_SECRET_BYTES + 8 + 4;
    if (n < head) return;
    int i = who(d);
    if (i < 0) return;
    const uint8_t *key = d + MASTER_ID_BYTES + MASTER_SECRET_BYTES, *r = key + 8;
    int vanilla = r[0], players = r[3];
    if (!vanilla || players < 2 || players > MASTER_RESULT_PLAYERS || n < head + players * MASTER_RESULT_PLAYER_BYTES) return;
    const uint8_t *data = r + 2; /* (the mode, the count, the players: what the machines must agree on) */
    int len = 2 + players * MASTER_RESULT_PLAYER_BYTES;
    uint32_t h = 2166136261u;
    for (int k = 0; k < len; k++) h = (h ^ data[k]) * 16777619u;
    Pending *pd = NULL, *free_one = NULL;
    for (int k = 0; k < PENDING; k++) {
        if (pend[k].used && !memcmp(pend[k].key, key, 8)) pd = &pend[k];
        if (!pend[k].used && !free_one) free_one = &pend[k];
    }
    if (!pd) {
        if (!free_one) return;
        pd = free_one;
        memset(pd, 0, sizeof *pd);
        pd->used = 1;
        memcpy(pd->key, key, 8);
        pd->at = now;
    }
    if (pd->done) return;
    int v = -1;
    for (int k = 0; k < pd->nv; k++) if (pd->v[k].hash == h && pd->v[k].len == len && !memcmp(pd->v[k].data, data, (size_t)len)) v = k;
    if (v < 0) {
        if (pd->nv >= VARIANTS) return;
        v = pd->nv++;
        pd->v[v].hash = h;
        pd->v[v].len = len;
        memcpy(pd->v[v].data, data, (size_t)len);
    }
    for (int k = 0; k < pd->v[v].n; k++) if (!memcmp(pd->v[v].reporters[k], d, MASTER_ID_BYTES)) return; /* (once a machine) */
    if (pd->v[v].n < VARIANTS) memcpy(pd->v[v].reporters[pd->v[v].n++], d, MASTER_ID_BYTES);
    if (pd->v[v].n < 2) return;
    pd->done = 1; /* two agree: counted */
    const uint8_t *q = data + 2;
    total.matches++;
    for (int k = 0; k < players; k++, q += MASTER_RESULT_PLAYER_BYTES) {
        int x = find(q), car = q[MASTER_ID_BYTES];
        if (car >= CARS) continue;
        const uint8_t *t = q + MASTER_ID_BYTES + 1; /* (the totals: every player of it) */
        uint32_t kills = (uint32_t)(t[1] | t[2] << 8), deaths = (uint32_t)(t[3] | t[4] << 8);
        uint32_t damage = (uint32_t)(t[5] | t[6] << 8 | t[7] << 16 | (uint32_t)t[8] << 24);
        total.kills += kills, total.deaths += deaths, total.damage += damage;
        car_total[car].matches++, car_total[car].wins += t[0], car_total[car].kills += kills, car_total[car].deaths += deaths;
        car_total[car].damage += damage;
        if (x < 0) continue;
        if (!pl[x].stats && !(pl[x].stats = calloc(CARS, sizeof(CarStats)))) continue;
        CarStats *s = &pl[x].stats[car];
        const uint8_t *z = q + MASTER_ID_BYTES + 1;
        s->matches++;
        s->wins += z[0];
        s->kills += (uint32_t)(z[1] | z[2] << 8);
        s->deaths += (uint32_t)(z[3] | z[4] << 8);
        s->damage += (uint32_t)(z[5] | z[6] << 8 | z[7] << 16 | (uint32_t)z[8] << 24);
    }
    dirty = 1;
    fprintf(stderr, "master: a match counted (%d players)\n", players);
}
