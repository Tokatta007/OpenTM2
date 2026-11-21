/* Mod arenas (docs/ARENA_LOADING.md): a mod's arena plays as level 13, one number for every mod arena; the port
 * chooses which mod. The game's own code then does the rest:
 * - the level's DMD and TMS names (settingsLevelDmdFileName / TmsFileName, vplus/fixes.c) are rewritten to
 *   DMD\MODARENA.DMD and TMS\MODARENA.TMS, and fileioLoadFileIntoRam (vp_FileLoad) reads those from the mod's folder
 *   (arena.dmd, arena.tms) to the game's place, as the disc's;
 * - arenaSetup's switch has no case 13: its default, arenaLoadHolland (80119EFC), is wrapped and loads the mod's
 *   arena.pts / .grp / .rmp, its sky and its start points (arena.ini) as an arenaLoad<Name> does;
 * - gameGetLevel (80129AF8) answers 7 (Holland, the level no code switch singles out) for level 13: the AI's level
 *   formulas (3 * level, (8 - level) * 60) and every per-level rule read it, so a mod arena gets the plain rules;
 *   the level events (LA's quakes, Moscow's bridges, Paris's tower...) read their own levels and never start;
 * - worldWarpsSetup (80115310): the teleporters' pairs from arena.ini (warp1 .. warp3).
 * arena.ini ([arena]): name, sky = r, g, b, points (route points, at most 140), starts (route points, up to 15),
 * sections (GRP, at most 35), surfaces (RMP, at most 225), surfaces_extra, short_chase (0 or 1), and
 * warpN = x, y, z, to x, y, z, its pair (0..2), fall_limit and fall_death (the heights a car is lost and destroyed
 * at). Its music: music.wav / .mp3 / .flac / .ogg in its folder (music/music.c), else another arena's track. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "mods.h"
#include "mod_arena.h"
#include "music/music.h"
#include "ui/draw.h"

#define SETTINGS GA(0x8016475Cu)            /* sessionSettings (+0x18 the level) */
#define ROUTE_NODES GA(0x801B1DE8u)         /* aiRouteNodes: 140 x 24 bytes */
#define ROUTE_COUNT GA(0x80180AD8u)         /* aiRouteNodeCount */
#define START_LIST GA(0x801B2B28u)          /* startNodeList: 15 s16 nodes, 15 used bytes, the count */
#define GRP_FILE GA(0x801A9848u)            /* worldSectionGrpFile: 6036 bytes */
#define SECTION_COUNT GA(0x80180AD4u)       /* terrainSectionCount */
#define SURFACES GA(0x801B0418u)            /* worldSurfaces: 225 x 28 bytes */
#define SURFACE_COUNT GA(0x80180ACCu)       /* terrainSurfaceCount */
#define SURFACE_EXTRA GA(0x80180AD0u)       /* (set with the RMP: Hong Kong's 2, the rest 0) */
#define CHASE_RANGE GA(0x80181030u)         /* arenaShortChaseRange */
#define WARPS GA(0x80187934u)               /* worldWarps: 3 x 40 bytes */
#define F_VIEW_BG GA(0x8015659Cu)           /* viewSetBgColor(r, g, b) */
#define F_START_CLEAR GA(0x801019DCu)       /* startNodesClearUsed */

#define PTS_BYTES 0xD20
#define GRP_BYTES 6036
#define RMP_BYTES 6300

static struct {
    char dir[600];
    char name[64];
    int sky[3], points, starts[15], nstarts, sections, surfaces, extra, chase;
    int warps, warp[3][7];
    int fall_limit, fall_death, has_fall_limit, has_fall_death;
    int heal, heal_x, heal_z;          /* health_spot = x, z (heal 1), none without it (2) */
    int exits[3][2], has_exit[3];      /* warpN_exit = spawn node, target node: a computer car leaving teleporter N */
    char music[700]; /* its own music file ("": none) */
} ma;
static int chosen; /* a mod arena is chosen (ma filled) */

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static int numbers(const char *v, int *out, int max)
{
    int n = 0;
    while (*v && n < max) {
        char *end;
        long x = strtol(v, &end, 10);
        if (end == v) { v++; continue; }
        out[n++] = (int)x;
        v = end;
    }
    return n;
}
static int read_ini(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512], section[32] = "";
    ma.sky[0] = 0x8F, ma.sky[1] = 0x6F, ma.sky[2] = 0x50;
    ma.chase = 1;
    while (fgets(line, sizeof line, f)) {
        char *semi = strchr(line, ';');
        if (semi) *semi = 0;
        trim(line);
        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close) *close = 0;
            snprintf(section, sizeof section, "%s", line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq || SDL_strcasecmp(section, "arena")) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key), trim(val);
        int one[1];
        if (!SDL_strcasecmp(key, "name")) snprintf(ma.name, sizeof ma.name, "%s", val);
        else if (!SDL_strcasecmp(key, "sky")) numbers(val, ma.sky, 3);
        else if (!SDL_strcasecmp(key, "points") && numbers(val, one, 1)) ma.points = one[0];
        else if (!SDL_strcasecmp(key, "starts")) ma.nstarts = numbers(val, ma.starts, 15);
        else if (!SDL_strcasecmp(key, "sections") && numbers(val, one, 1)) ma.sections = one[0];
        else if (!SDL_strcasecmp(key, "surfaces") && numbers(val, one, 1)) ma.surfaces = one[0];
        else if (!SDL_strcasecmp(key, "surfaces_extra") && numbers(val, one, 1)) ma.extra = one[0];
        else if (!SDL_strcasecmp(key, "short_chase") && numbers(val, one, 1)) ma.chase = one[0] != 0;
        else if (!SDL_strcasecmp(key, "fall_limit") && numbers(val, one, 1)) ma.fall_limit = one[0], ma.has_fall_limit = 1;
        else if (!SDL_strcasecmp(key, "fall_death") && numbers(val, one, 1)) ma.fall_death = one[0], ma.has_fall_death = 1;
        else if (!SDL_strcasecmp(key, "health_spot")) { int xz[2]; if (numbers(val, xz, 2) == 2) ma.heal = 1, ma.heal_x = xz[0], ma.heal_z = xz[1]; }
        else if (!SDL_strncasecmp(key, "warp", 4) && key[4] >= '1' && key[4] <= '3' && !SDL_strcasecmp(key + 5, "_exit")) {
            int k = key[4] - '1';
            if (numbers(val, ma.exits[k], 2) == 2) ma.has_exit[k] = 1;
        }
        else if (!SDL_strncasecmp(key, "warp", 4) && key[4] >= '1' && key[4] <= '3' && !key[5]) {
            int k = key[4] - '1';
            if (numbers(val, ma.warp[k], 7) == 7 && k + 1 > ma.warps) ma.warps = k + 1;
        }
    }
    fclose(f);
    if (ma.points < 1 || ma.points > 140) ma.points = ma.points < 1 ? 1 : 140;
    if (ma.sections < 1 || ma.sections > 35) ma.sections = ma.sections < 1 ? 1 : 35;
    if (ma.surfaces < 0 || ma.surfaces > 225) ma.surfaces = ma.surfaces < 0 ? 0 : 225;
    if (ma.nstarts < 1) ma.starts[0] = 0, ma.nstarts = 1;
    return 1;
}

int mod_arena_choose(const char *id)
{
    chosen = 0;
    if (!id || !id[0]) return 0;
    for (int i = 0; i < mods_count(); i++) {
        const Mod *m = mods_get(i);
        if (m->kind != MOD_ARENA || !m->ok || (strcmp(m->id, id) && strcmp(m->uid, id))) continue;
        memset(&ma, 0, sizeof ma);
        snprintf(ma.dir, sizeof ma.dir, "%s", m->dir);
        snprintf(ma.name, sizeof ma.name, "%s", m->name);
        char path[700];
        snprintf(path, sizeof path, "%sarena.ini", m->dir);
        if (!read_ini(path)) { fprintf(stderr, "mod arena: %s has no arena.ini\n", id); return 0; }
        static const char *const exts[] = {"wav", "mp3", "flac", "ogg"};
        for (int k = 0; k < 4 && !ma.music[0]; k++) { /* its own music: music.<ext> in its folder */
            snprintf(path, sizeof path, "%smusic.%s", m->dir, exts[k]);
            FILE *f = fopen(path, "rb");
            if (f) fclose(f), snprintf(ma.music, sizeof ma.music, "%s", path);
        }
        chosen = 1;
        fprintf(stderr, "mod arena: %s (%s): %d route points, %d starts, %d sections, %d surfaces, %d teleporters\n", ma.name, m->id,
                ma.points, ma.nstarts, ma.sections, ma.surfaces, ma.warps);
        return 1;
    }
    fprintf(stderr, "mod arena: no arena mod \"%s\" (or it has problems: --check-mod)\n", id);
    return 0;
}
const char *mod_arena_name(void) { return chosen ? ma.name : ""; }
const char *mod_arena_music(void) { return chosen ? ma.music : ""; }
int mod_arena_on(void) { return chosen && mem_r8(&M.cpu, SETTINGS + 24) == MOD_ARENA_LEVEL; }

/* tests: TM2PC_TEST_MOD_ARENA=<id>: a Challenge Match's arena is that mod's (read once) */
int mod_arena_test_level(void)
{
    static int state = -1;
    if (state < 0) state = getenv("TM2PC_TEST_MOD_ARENA") && mod_arena_choose(getenv("TM2PC_TEST_MOD_ARENA"));
    return state ? MOD_ARENA_LEVEL : 0;
}

/* a mod file into guest RAM at `at` (at most max bytes): its size, -1 if missing */
static int load_file(const char *name, uint32_t at, int max)
{
    char path[700];
    snprintf(path, sizeof path, "%s%s", ma.dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "mod arena: %s missing\n", path); return -1; }
    int n = 0, ch;
    while ((max < 0 || n < max) && (ch = fgetc(f)) != EOF) mem_w8(&M.cpu, at + (uint32_t)n++, (uint8_t)ch);
    fclose(f);
    return n;
}
/* fileioLoadFileIntoRam's name: DMD\MODARENA.DMD or TMS\MODARENA.TMS read from the mod (vp_FileLoad): 1 if so,
 * the size in r2 */
/* the game's line-up picture for level 13 (SCREEN\SHOWOPS13.TIM, none on the disc: the game retried 5 s, decomp
 * MOD_ARENAS.md R4): the mod's lineup.bmp as the disc's are, a 16-bit 320 x 240 TIM for VRAM (640, 0) */
static int lineup_tim(CPU *c, uint32_t dest)
{
    char path[700];
    snprintf(path, sizeof path, "%slineup.bmp", ma.dir);
    Image im = image_load_bmp(path);
    const int W = 320, H = 240;
    uint32_t at = dest;
    mem_w32(&M.cpu, at, 0x10), mem_w32(&M.cpu, at + 4, 2);
    mem_w32(&M.cpu, at + 8, 12 + W * H * 2);
    mem_w16(&M.cpu, at + 12, 640), mem_w16(&M.cpu, at + 14, 0), mem_w16(&M.cpu, at + 16, W), mem_w16(&M.cpu, at + 18, H);
    at += 20;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++, at += 2) {
            uint16_t v = 0x8000; /* (black, drawn: not the see-through 0) */
            if (im.px) {
                const uint8_t *p = im.px + ((size_t)(y * im.h / H) * im.w + (size_t)(x * im.w / W)) * 3;
                v = (uint16_t)(p[0] >> 3 | (p[1] >> 3) << 5 | (p[2] >> 3) << 10);
                if (!v) v = 0x8000;
            }
            mem_w16(&M.cpu, at, v);
        }
    free(im.px);
    c->r[2] = 20 + W * H * 2;
    fprintf(stderr, "mod arena: lineup.bmp as SHOWOPS13.TIM%s\n", im.px ? "" : " (missing: black)");
    return 1;
}
const char *mod_arena_dir(void) { return chosen ? ma.dir : ""; }
int mod_arena_file(CPU *c, uint32_t dest, const char *name)
{
    if (chosen && !SDL_strcasecmp(name, "SCREEN\\SHOWOPS13.TIM")) return lineup_tim(c, dest);
    if (!chosen || SDL_strncasecmp(name + 4, "MODARENA.", 9)) return 0;
    const char *file = !SDL_strncasecmp(name, "DMD", 3) ? "arena.dmd" : !SDL_strncasecmp(name, "TMS", 3) ? "arena.tms" : NULL;
    if (!file) return 0;
    int n = load_file(file, dest, 0x200000);
    c->r[2] = (uint32_t)(n > 0 ? n : 0);
    fprintf(stderr, "mod arena: %s, %d bytes at %08X\n", file, n, dest);
    return 1;
}
/* the level's file names (settingsLevelDmdFileName / TmsFileName, after the game's): level 13's, the mod's */
void mod_arena_file_name(CPU *c, const char *dir, const char *ext)
{
    if (!mod_arena_on()) return;
    char name[32];
    snprintf(name, sizeof name, "%s\\MODARENA.%s", dir, ext);
    for (int i = 0;; i++) {
        mem_w8(&M.cpu, c->r[2] + (uint32_t)i, (uint8_t)name[i]);
        if (!name[i]) break;
    }
}

/* arenaSetup's default (no case 13): the mod's arena as an arenaLoad<Name> */
void F_80119EFC_orig(CPU *c);
void ma_ArenaLoadHolland(CPU *c)
{
    if (!mod_arena_on()) { F_80119EFC_orig(c); return; }
    mem_w32(&M.cpu, CHASE_RANGE, (uint32_t)ma.chase);
    guest_call(F_VIEW_BG, (uint32_t)ma.sky[0], (uint32_t)ma.sky[1], (uint32_t)ma.sky[2], 0);
    /* the routes: the count, the start points, the points */
    mem_w32(&M.cpu, ROUTE_COUNT, (uint32_t)ma.points);
    guest_call(F_START_CLEAR, 0, 0, 0, 0);
    mem_w8(&M.cpu, START_LIST + 0x2D, 15); /* (as every arena's: 15, the start picks wrap round the list) */
    for (int i = 0; i < 15; i++)
        mem_w16(&M.cpu, START_LIST + 2u * (uint32_t)i, (uint16_t)ma.starts[i < ma.nstarts ? i : i % ma.nstarts]);
    for (uint32_t i = 0; i < PTS_BYTES; i++) mem_w8(&M.cpu, ROUTE_NODES + i, 0);
    load_file("arena.pts", ROUTE_NODES, PTS_BYTES);
    /* the sections and the surfaces */
    mem_w32(&M.cpu, SECTION_COUNT, (uint32_t)ma.sections);
    for (uint32_t i = 0; i < GRP_BYTES; i++) mem_w8(&M.cpu, GRP_FILE + i, 0);
    load_file("arena.grp", GRP_FILE, GRP_BYTES);
    mem_w32(&M.cpu, SURFACE_COUNT, (uint32_t)ma.surfaces);
    mem_w32(&M.cpu, SURFACE_EXTRA, (uint32_t)ma.extra);
    for (uint32_t i = 0; i < RMP_BYTES; i++) mem_w8(&M.cpu, SURFACES + i, 0);
    load_file("arena.rmp", SURFACES, RMP_BYTES);
    music_mod_arena_loaded(); /* (its round music: its own file, or another arena's track) */
    fprintf(stderr, "mod arena: %s loaded\n", ma.name);
    c->r[2] = 1;
}

/* gameGetLevel: level 13 as 7 (the plain rules); and inside a computer car's movement (vp_AiMovementUpdate) every
 * level past 8 as 7: aiMovementUpdate's two fire timers, (8 - level) * 60 + 180 and + 120 (decomp ai_steps.c 526,
 * 1451), go to 0 or below there, so on Roof Tops (12) a lined-up car cleared its cooldown and fired every frame (the
 * user's 16-car match 2026-11-17: Sweet Tooth's ~20 specials in a row, Axel's 10; Cyburbia's 11 now and then). Retail's
 * own (the secret arenas' AI); Holland's 240 / 180 frames, and nothing in that function singles out 7 */
static int ai_rank; /* (set only for the call) */
void ma_ai_rank(int on) { ai_rank = on; }
void F_80129AF8_orig(CPU *c);
void ma_GameGetLevel(CPU *c)
{
    F_80129AF8_orig(c);
    if (c->r[2] == MOD_ARENA_LEVEL && chosen) c->r[2] = MOD_ARENA_PLAYS_AS;
    else if (ai_rank && (int32_t)c->r[2] > 8) c->r[2] = MOD_ARENA_PLAYS_AS;
}

/* roundStartMusic(level) 80128DB4: the music mode is level + 1 (13 none at all): a mod arena's is Holland's, the
 * level it plays as; music.c then plays its own file, the Custom Music row, or another arena's track */
void F_80128DB4_orig(CPU *c);
void ma_RoundStartMusic(CPU *c)
{
    if (c->r[4] == MOD_ARENA_LEVEL && chosen) c->r[4] = MOD_ARENA_PLAYS_AS;
    F_80128DB4_orig(c);
}

/* worldFallLimitZ(car, human) and worldFallDeathZ: arena.ini's fall_limit and fall_death when it has them (else
 * Holland's, as the level it plays as) */
void F_8011EC0C_orig(CPU *c);
void ma_WorldFallLimitZ(CPU *c)
{
    if (ma.has_fall_limit && mod_arena_on()) { c->r[2] = (uint32_t)ma.fall_limit; return; }
    F_8011EC0C_orig(c);
}
void F_8011ECC4_orig(CPU *c);
void ma_WorldFallDeathZ(CPU *c)
{
    if (ma.has_fall_death && mod_arena_on()) { c->r[2] = (uint32_t)ma.fall_death; return; }
    F_8011ECC4_orig(c);
}

/* The decomp's portArenaRules (request 114, TM2_PORT_MOD_ARENA; decomp docs/MOD_ARENAS.md): the hidden health spot
 * and the computer cars' teleporter exits as the arena's data. Written at every arena load: a mod arena's from its
 * arena.ini (no health_spot: none; a teleporter without warpN_exit: no exit for computer cars), zeros (retail's own)
 * for the game's arenas. */
static void arena_rules(int mod)
{
    static uint32_t at = 1;
    if (at == 1) at = port_sym("portArenaRules");
    if (!at) return; /* (a build without request 114: Holland's) */
    for (uint32_t i = 0; i < 0x1C; i++) mem_w8(&M.cpu, at + i, 0);
    if (!mod) return;
    mem_w32(&M.cpu, at + 0x00, ma.heal ? 1u : 2u);
    mem_w32(&M.cpu, at + 0x04, (uint32_t)ma.heal_x);
    mem_w32(&M.cpu, at + 0x08, (uint32_t)ma.heal_z);
    mem_w32(&M.cpu, at + 0x0C, 1);
    for (int k = 0; k < 3; k++) {
        mem_w16(&M.cpu, at + 0x10 + 4u * (uint32_t)k, (uint16_t)(int16_t)(ma.has_exit[k] ? ma.exits[k][0] : -1));
        mem_w16(&M.cpu, at + 0x12 + 4u * (uint32_t)k, (uint16_t)(int16_t)(ma.has_exit[k] ? ma.exits[k][1] : -1));
    }
    fprintf(stderr, "mod arena: health spot %s (%d, %d), teleporter exits %d %d %d\n", ma.heal ? "at" : "none", ma.heal_x, ma.heal_z,
            ma.has_exit[0], ma.has_exit[1], ma.has_exit[2]);
}

/* worldWarpsSetup(level): the mod's teleporters (each a pad's place, where it sends a car, its pair); every warp record
 * written (the game's resets only some of each: an unused one kept the last arena's places, decomp MOD_ARENAS.md R2) */
void F_80115310_orig(CPU *c);
void ma_WorldWarpsSetup(CPU *c)
{
    int level = (int)c->r[4];
    F_80115310_orig(c);
    int mod = level == MOD_ARENA_LEVEL && chosen;
    arena_rules(mod);
    if (!mod) return;
    for (int k = 0; k < 3; k++) {
        uint32_t w = WARPS + 40u * (uint32_t)k;
        for (int j = 0; j < 6; j++) mem_w32(&M.cpu, w + 8 + 4u * (uint32_t)j, 0);
        mem_w8(&M.cpu, w + 2, 0);
    }
    for (int k = 0; k < ma.warps; k++) {
        uint32_t w = WARPS + 40u * (uint32_t)k;
        for (int j = 0; j < 3; j++) {
            mem_w32(&M.cpu, w + 8 + 4u * (uint32_t)j, (uint32_t)ma.warp[k][j]);
            mem_w32(&M.cpu, w + 20 + 4u * (uint32_t)j, (uint32_t)ma.warp[k][3 + j]);
        }
        mem_w8(&M.cpu, w + 2, (uint8_t)ma.warp[k][6]);
    }
}
