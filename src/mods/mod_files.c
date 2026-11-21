/* A mod's files for online play (docs/MODDING.md 10): its fingerprint, and receiving it safely from a server.
 *
 * Mods are data only. A file travels only if its kind allows its name (mod_file_allowed), and it is kept only if its
 * contents look like what the name says (mod_file_sniff: a model starts with DXSP, a texture pack with TXSP, a picture
 * is a BMP, text has no control bytes...): a program renamed car.dmd is refused. The game never runs anything from a
 * mod; model data is read by the game's own code inside the emulated PlayStation memory, which cannot reach the PC's.
 *
 * Receiving (ModReceive): the server's list for a mod (its id, uid, kind, and each file's name, size and FNV-1a 64
 * hash) is checked first (names, sizes, the totals against the player's download limit); the chunks go into a staging
 * folder mods/<kind>/<id>.part/ at checked offsets; at the end every file's hash and contents are checked, and only
 * then is the folder renamed into place (a folder of that name already there: <id>~<the uid's first 8>). Nothing is
 * written outside the staging folder, and a failed mod leaves no folder in mods/. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "mods.h"
#include "mod_files.h"

/* ---- what may travel ---- */
static const char *const skin_files[] = {"mod.ini", "car.dmd", "car.tms", "carselect.dmd", "carselect.tms", "portrait_1.bmp",
                                         "portrait_2.bmp", "carinfo.bmp", "plate.bmp", NULL};
static const char *const car_files[] = {"mod.ini", "car.dmd", "car.tms", "carselect.dmd", "carselect.tms", "portrait_1.bmp",
                                        "portrait_2.bmp", "carinfo.bmp", "plate.bmp", "bio.txt", "ending.txt", "ending.bmp", "ending.mpg", "engine.wav", "special.wav", NULL};
static const char *const arena_files[] = {"mod.ini", "arena.dmd", "arena.tms", "arena.grp", "arena.rmp", "arena.pts", "arena.ini",
                                          "card.bmp", "wide.bmp", "lineup.bmp", "radar.bmp", "music.wav", "music.ogg",
                                          "music.flac", "music.mp3", NULL};
static int name_chars_ok(const char *n) /* lowercase letters, digits, '_', '-', one '.' inside; no path */
{
    int dots = 0;
    if (!n[0] || n[0] == '.' || strlen(n) > MOD_FILE_NAME - 1) return 0;
    for (const char *p = n; *p; p++) {
        if (*p == '.') { dots++; continue; }
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == '-')) return 0;
    }
    return dots == 1 && n[strlen(n) - 1] != '.';
}
int mod_file_allowed(int kind, const char *name)
{
    if (!name_chars_ok(name)) return 0;
    const char *const *list = kind == MOD_SKIN ? skin_files : kind == MOD_CHARACTER ? car_files : kind == MOD_ARENA ? arena_files : NULL;
    if (list) {
        for (int i = 0; list[i]; i++)
            if (!strcmp(list[i], name)) return 1;
        return 0;
    }
    /* a tournament pack: mod.ini, and its texts and pictures between levels (level1.txt, level1.bmp, ending.bmp ...) */
    const char *dot = strrchr(name, '.');
    return !strcmp(name, "mod.ini") || (dot && (!strcmp(dot, ".txt") || !strcmp(dot, ".bmp")));
}
static long file_limit(const char *name) /* bytes */
{
    const char *dot = strrchr(name, '.');
    if (dot && (!strcmp(dot, ".ini") || !strcmp(dot, ".txt"))) return 64L * 1024;
    if (dot && (!strcmp(dot, ".dmd") || !strcmp(dot, ".tms") || !strcmp(dot, ".grp") || !strcmp(dot, ".rmp") || !strcmp(dot, ".pts")))
        return 4L * 1024 * 1024;
    if (dot && !strcmp(dot, ".mpg")) return MOD_FILE_MAX; /* (an ending's video: MODDING.md 6) */
    return 16L * 1024 * 1024;
}

/* ---- contents ---- */
static uint32_t u32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int mod_file_sniff(const char *name, const uint8_t *d, long n)
{
    const char *dot = strrchr(name, '.');
    if (!dot || n <= 0) return 0;
    if (!strcmp(dot, ".dmd")) return n >= 0x20 && !memcmp(d, "DXSP", 4);
    if (!strcmp(dot, ".tms")) return n >= 0x10 && !memcmp(d, "TXSP", 4);
    if (!strcmp(dot, ".bmp")) { /* a BMP whose header fits the file */
        if (n < 54 || d[0] != 'B' || d[1] != 'M') return 0;
        uint32_t off = u32le(d + 10);
        int32_t w = (int32_t)u32le(d + 18), h = (int32_t)u32le(d + 22);
        int bpp = d[28] | d[29] << 8;
        if (h < 0) h = -h;
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (bpp != 8 && bpp != 24 && bpp != 32) || off > (uint32_t)n) return 0;
        long row = ((long)w * bpp / 8 + 3) & ~3L;
        return off + row * h <= (unsigned long)n;
    }
    if (!strcmp(dot, ".wav")) return n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4);
    if (!strcmp(dot, ".ogg")) return n >= 4 && !memcmp(d, "OggS", 4);
    if (!strcmp(dot, ".mpg")) /* MPEG-1: a program stream's pack header, or a video's sequence header */
        return n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && (d[3] == 0xBA || d[3] == 0xB3);
    if (!strcmp(dot, ".flac")) return n >= 4 && !memcmp(d, "fLaC", 4);
    if (!strcmp(dot, ".mp3")) return n >= 3 && (!memcmp(d, "ID3", 3) || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0));
    if (!strcmp(dot, ".ini") || !strcmp(dot, ".txt")) { /* text: printable, tabs and line ends, UTF-8 bytes */
        for (long i = 0; i < n; i++)
            if (d[i] < 32 && d[i] != '\t' && d[i] != '\r' && d[i] != '\n') return 0;
        return 1;
    }
    if (!strcmp(dot, ".grp") || !strcmp(dot, ".rmp") || !strcmp(dot, ".pts")) /* the level's tables: data the game reads */
        return !(n >= 2 && d[0] == 'M' && d[1] == 'Z');   /* (never a Windows program) */
    return 0;
}

/* ---- the fingerprint: FNV-1a 64 over each allowed file's name, size and bytes, in name order ---- */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
static uint8_t *read_all(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = *n >= 0 && *n <= MOD_FILE_MAX ? malloc((size_t)*n + 1) : NULL;
    if (d && fread(d, 1, (size_t)*n, f) != (size_t)*n) { free(d); d = NULL; }
    fclose(f);
    return d;
}
static int by_name(const void *a, const void *b) { return strcmp(((const ModFile *)a)->name, ((const ModFile *)b)->name); }
/* a mod folder's allowed files (name, size, hash) and its fingerprint; the count, -1 if a file cannot be read */
int mod_package(const char *dir, int kind, ModFile *files, int max, uint64_t *fingerprint)
{
    int n = 0, count = 0;
    char **list = SDL_GlobDirectory(dir, "*", 0, &count);
    for (int i = 0; list && i < count && n < max; i++) {
        if (!mod_file_allowed(kind, list[i])) continue; /* (other files in the folder: not sent, not counted) */
        char path[700];
        snprintf(path, sizeof path, "%s%s", dir, list[i]);
        long size;
        uint8_t *d = read_all(path, &size);
        if (!d) { SDL_free(list); return -1; }
        ModFile *f = &files[n++];
        snprintf(f->name, sizeof f->name, "%s", list[i]);
        f->size = size;
        f->hash = fnv(0xCBF29CE484222325ull, d, (size_t)size);
        free(d);
    }
    SDL_free(list);
    qsort(files, (size_t)n, sizeof files[0], by_name);
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < n; i++) {
        h = fnv(h, files[i].name, strlen(files[i].name) + 1);
        h = fnv(h, &files[i].size, sizeof files[i].size);
        h = fnv(h, &files[i].hash, sizeof files[i].hash);
    }
    *fingerprint = h;
    return n;
}

/* ---- receiving ---- */
static int id_ok(const char *id)
{
    if (!id[0] || strlen(id) > 40) return 0;
    for (const char *p = id; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-')) return 0;
    return 1;
}
static void staging_path(const ModReceive *r, char *out, size_t size)
{
    snprintf(out, size, "%smods/%s/%s.part/", r->game_folder, mod_kind_folders[r->kind], r->id);
}
static void empty_staging(const ModReceive *r) /* the staging folder's files whose names a mod of its kind may have */
{
    char dir[700];
    staging_path(r, dir, sizeof dir);
    int count = 0;
    char **list = SDL_GlobDirectory(dir, "*", 0, &count);
    for (int i = 0; list && i < count; i++) {
        if (!mod_file_allowed(r->kind, list[i])) continue;
        char p[800];
        snprintf(p, sizeof p, "%s%s", dir, list[i]);
        SDL_RemovePath(p);
    }
    SDL_free(list);
}
/* the server's list for one mod, checked before anything is written; 0 and a reason if refused */
int mod_receive_begin(ModReceive *r, const char *game_folder, int kind, const char *id, const char *uid, uint64_t fingerprint,
                      const ModFile *files, int count, long limit_bytes)
{
    memset(r, 0, sizeof *r);
    snprintf(r->game_folder, sizeof r->game_folder, "%s", game_folder);
    if (kind < 0 || kind >= MOD_KINDS) { snprintf(r->why, sizeof r->why, "an unknown kind of mod"); return 0; }
    if (!id_ok(id)) { snprintf(r->why, sizeof r->why, "a bad id"); return 0; }
    if (strlen(uid) != 32 || strspn(uid, "0123456789abcdef") != 32) { snprintf(r->why, sizeof r->why, "a bad uid"); return 0; }
    if (count <= 0 || count > MOD_FILES_MAX) { snprintf(r->why, sizeof r->why, "%d files", count); return 0; }
    r->kind = kind;
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->uid, sizeof r->uid, "%s", uid);
    r->fingerprint = fingerprint;
    long total = 0;
    int has_ini = 0;
    for (int i = 0; i < count; i++) {
        if (!mod_file_allowed(kind, files[i].name)) { snprintf(r->why, sizeof r->why, "a file that is not allowed (%.40s)", files[i].name); return 0; }
        for (int k = 0; k < i; k++)
            if (!strcmp(files[k].name, files[i].name)) { snprintf(r->why, sizeof r->why, "%s twice", files[i].name); return 0; }
        if (files[i].size <= 0 || files[i].size > file_limit(files[i].name)) {
            snprintf(r->why, sizeof r->why, "%s is too big", files[i].name);
            return 0;
        }
        total += files[i].size;
        has_ini |= !strcmp(files[i].name, "mod.ini");
        r->files[i] = files[i];
    }
    if (!has_ini) { snprintf(r->why, sizeof r->why, "no mod.ini"); return 0; }
    if (total > MOD_TOTAL_MAX || (limit_bytes > 0 && total > limit_bytes)) {
        snprintf(r->why, sizeof r->why, "%.1f MB, over the download limit", total / 1048576.0);
        return 0;
    }
    r->count = count;
    r->total = total;
    char dir[700];
    staging_path(r, dir, sizeof dir);
    char kind_dir[700];
    snprintf(kind_dir, sizeof kind_dir, "%smods/%s", game_folder, mod_kind_folders[kind]);
    SDL_CreateDirectory(kind_dir); /* (makes mods/ and mods/<kind>/ as needed) */
    empty_staging(r);              /* (an earlier try's) */
    if (!SDL_CreateDirectory(dir)) { snprintf(r->why, sizeof r->why, "cannot write %s", dir); return 0; }
    r->open = 1;
    return 1;
}

/* a chunk of file i at offset (each file in order: the reliable channel keeps it); 0 refused (the mod is dropped) */
int mod_receive_chunk(ModReceive *r, int i, long offset, const uint8_t *data, int len)
{
    if (!r->open) return 0;
    if (i < 0 || i >= r->count || len < 0 || offset != r->got[i] || offset + len > r->files[i].size) {
        snprintf(r->why, sizeof r->why, "a piece out of place");
        mod_receive_cancel(r);
        return 0;
    }
    char path[800];
    staging_path(r, path, sizeof path);
    strncat(path, r->files[i].name, sizeof path - strlen(path) - 1);
    FILE *f = fopen(path, offset ? "ab" : "wb");
    if (!f || fwrite(data, 1, (size_t)len, f) != (size_t)len) {
        if (f) fclose(f);
        snprintf(r->why, sizeof r->why, "cannot write %s", r->files[i].name);
        mod_receive_cancel(r);
        return 0;
    }
    fclose(f);
    r->got[i] += len;
    r->done += len;
    return 1;
}

/* every file in: each one's hash and contents, the fingerprint; then the folder into place. 1 installed (out: its
 * folder), 0 refused (nothing left in mods/) */
int mod_receive_finish(ModReceive *r, char *installed, int size)
{
    if (!r->open) return 0;
    char dir[700];
    staging_path(r, dir, sizeof dir);
    for (int i = 0; i < r->count; i++) {
        if (r->got[i] != r->files[i].size) { snprintf(r->why, sizeof r->why, "%s did not all arrive", r->files[i].name); goto fail; }
        char path[800];
        snprintf(path, sizeof path, "%s%s", dir, r->files[i].name);
        long n;
        uint8_t *d = read_all(path, &n);
        int good = d && n == r->files[i].size && fnv(0xCBF29CE484222325ull, d, (size_t)n) == r->files[i].hash;
        int looks = good && mod_file_sniff(r->files[i].name, d, n);
        free(d);
        if (!good) { snprintf(r->why, sizeof r->why, "%s arrived damaged", r->files[i].name); goto fail; }
        if (!looks) { snprintf(r->why, sizeof r->why, "%s is not what its name says", r->files[i].name); goto fail; }
    }
    ModFile check[MOD_FILES_MAX];
    uint64_t fp;
    if (mod_package(dir, r->kind, check, MOD_FILES_MAX, &fp) != r->count || fp != r->fingerprint) {
        snprintf(r->why, sizeof r->why, "its fingerprint does not match the server's");
        goto fail;
    }
    char final[700];
    snprintf(final, sizeof final, "%smods/%s/%s", r->game_folder, mod_kind_folders[r->kind], r->id);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(final, &info)) /* (another mod or version by that name: kept; this one beside it) */
        snprintf(final, sizeof final, "%smods/%s/%s~%.8s", r->game_folder, mod_kind_folders[r->kind], r->id, r->uid);
    char from[700];
    snprintf(from, sizeof from, "%s", dir);
    from[strlen(from) - 1] = 0; /* (no trailing slash) */
    if (SDL_GetPathInfo(final, &info) || !SDL_RenamePath(from, final)) {
        snprintf(r->why, sizeof r->why, "cannot put it in mods/%s", mod_kind_folders[r->kind]);
        goto fail;
    }
    r->open = 0;
    if (installed) snprintf(installed, (size_t)size, "%s/", final);
    return 1;
fail:
    mod_receive_cancel(r);
    return 0;
}

/* the staging folder's files removed and the folder too (only our own files: the names on the list) */
void mod_receive_cancel(ModReceive *r)
{
    empty_staging(r);
    char dir[700];
    staging_path(r, dir, sizeof dir);
    dir[strlen(dir) - 1] = 0;
    SDL_RemovePath(dir);
    r->open = 0;
}

/* ---- the self-test (tm2pc --test-mod-download) ---- */
static int send_files(const char *game, int kind, const char *id, const char *uid, uint64_t fp, const ModFile *files, int n,
                      uint8_t *const *data, long limit, char *installed, char *why)
{
    ModReceive *r = malloc(sizeof *r);
    if (!r) return 0;
    int ok = mod_receive_begin(r, game, kind, id, uid, fp, files, n, limit);
    for (int i = 0; ok && i < n; i++)
        for (long off = 0; ok && off < files[i].size; off += 1000)
            ok = mod_receive_chunk(r, i, off, data[i] + off, (int)(files[i].size - off < 1000 ? files[i].size - off : 1000));
    if (ok) ok = mod_receive_finish(r, installed, 700);
    snprintf(why, 120, "%s", r->why);
    free(r);
    return ok;
}
static int expect(const char *what, int got, int want, const char *why)
{
    printf("  %-44s %s%s%s\n", what, got == want ? "as expected" : "WRONG", got ? "" : ": ", got ? "" : why);
    return got == want ? 0 : 1;
}
int mod_files_selftest(const char *mod_dir, const char *game)
{
    char dir[700], id[48] = "", uid[40] = "", line[300];
    size_t dl = strlen(mod_dir);
    snprintf(dir, sizeof dir, "%s%s", mod_dir, dl && (mod_dir[dl - 1] == '/' || mod_dir[dl - 1] == '\\') ? "" : "/");
    int kind = strstr(dir, "characters") ? MOD_CHARACTER : strstr(dir, "arenas") ? MOD_ARENA : strstr(dir, "tournaments") ? MOD_TOURNAMENT : MOD_SKIN;
    snprintf(line, sizeof line, "%smod.ini", dir);
    FILE *f = fopen(line, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *c = strchr(line, ';');
        if (c) *c = 0;
        if (!eq) continue;
        *eq = 0;
        char key[32] = "", val[64] = "";
        sscanf(line, "%31s", key);
        sscanf(eq + 1, "%63s", val);
        if (!strcmp(key, "id")) snprintf(id, sizeof id, "%s", val);
        if (!strcmp(key, "uid")) snprintf(uid, sizeof uid, "%s", val);
    }
    if (f) fclose(f);
    ModFile files[MOD_FILES_MAX];
    uint64_t fp;
    int n = mod_package(dir, kind, files, MOD_FILES_MAX, &fp), bad = 0;
    if (n <= 0) { printf("cannot read %s\n", dir); return 1; }
    printf("%s: %d files, fingerprint %016llx\n", id, n, (unsigned long long)fp);
    uint8_t *data[MOD_FILES_MAX];
    for (int i = 0; i < n; i++) {
        char p[800];
        long size;
        snprintf(p, sizeof p, "%s%s", dir, files[i].name);
        data[i] = read_all(p, &size);
    }
    char installed[700] = "", why[120];
    /* 1. as a server sends it: installed, the same fingerprint */
    int ok = send_files(game, kind, id, uid, fp, files, n, data, 50L * 1024 * 1024, installed, why);
    bad += expect("the mod as the server sends it: installed", ok, 1, why);
    if (ok) {
        ModFile again[MOD_FILES_MAX];
        uint64_t fp2;
        int n2 = mod_package(installed, kind, again, MOD_FILES_MAX, &fp2);
        bad += expect("  its files the same", n2 == n && fp2 == fp, 1, "the fingerprint differs");
        printf("  (in %s)\n", installed);
    }
    /* 2. a byte changed on the way */
    data[0][0] ^= 0x55;
    bad += expect("a byte changed on the way: refused", send_files(game, kind, id, uid, fp, files, n, data, 0, NULL, why), 0, why);
    data[0][0] ^= 0x55;
    /* 3. a file name with a path */
    ModFile evil[MOD_FILES_MAX];
    memcpy(evil, files, sizeof files[0] * (size_t)n);
    snprintf(evil[n - 1].name, sizeof evil[n - 1].name, "../../evil.dmd");
    bad += expect("a name with a path: refused", send_files(game, kind, id, uid, fp, evil, n, data, 0, NULL, why), 0, why);
    /* 4. a program sent as the car's model */
    static uint8_t program[4096] = {'M', 'Z'};
    for (int i = 0; i < n; i++)
        if (strstr(files[i].name, ".dmd")) {
            memcpy(evil, files, sizeof files[0] * (size_t)n);
            evil[i].size = sizeof program;
            evil[i].hash = fnv(0xCBF29CE484222325ull, program, sizeof program);
            uint8_t *swapped[MOD_FILES_MAX];
            memcpy(swapped, data, sizeof data[0] * (size_t)n);
            swapped[i] = program;
            uint64_t h = 0xCBF29CE484222325ull; /* (its fingerprint: as a lying server would list it) */
            for (int k = 0; k < n; k++) {
                h = fnv(h, evil[k].name, strlen(evil[k].name) + 1);
                h = fnv(h, &evil[k].size, sizeof evil[k].size);
                h = fnv(h, &evil[k].hash, sizeof evil[k].hash);
            }
            bad += expect("a program as the model: refused", send_files(game, kind, "evil-mod", uid, h, evil, n, swapped, 0, NULL, why), 0, why);
            break;
        }
    /* 5. over the player's limit */
    bad += expect("over the download limit: refused", send_files(game, kind, id, uid, fp, files, n, data, 10, NULL, why), 0, why);
    for (int i = 0; i < n; i++) free(data[i]);
    printf(bad ? "%d checks WRONG\n" : "all as expected\n", bad);
    return bad;
}
