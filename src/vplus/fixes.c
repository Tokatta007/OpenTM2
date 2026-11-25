/* Vanilla Plus changes that hook game functions (src/overrides.json).
 * Each hook runs the original code and adds the change only when it is on;
 * vp_vblank runs once per VBlank for changes that need timing. */
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "hud_layout.h"
#include "cars.h" /* HUMAN_REC, AI_REC, the counts */
uint64_t vp_gv(void);    /* killcam.c: the gameplay clock (VBlanks; a kill cam replay sets it back) */
uint32_t vp_rand(void);  /* killcam.c: random numbers kept in guest RAM (a replay draws the same) */
void killcam_keep(void *p, size_t n);
uint32_t killcam_input(int kind, int player, uint32_t live); /* killcam.c */
void killcam_forget(void);
void killcam_round_end(void);
extern int killcam_replaying;
#include "vplus.h"
#include "profile/battle.h"
#include "profile/profile.h"
#include "ui/ui.h"
#include "settings.h"
#include "music/music.h"
#include "mods/mod_arena.h"
#include "mods/mods.h"
#define DM_TEXT 0x80DFF000u /* the port's spare guest RAM: texts, HUD copies (see the users) */
#include <psyz/video.h>

/* ---- VP_PAUSE_QUIT_BOOM ----
 * The confirm explosion (program 0x1D: sample 0x09320) sounds full in the
 * menus because a menu screen's music loop (program 0x1B) is playing under
 * it, or starts with it; its beat gives the bass. Resume and Quit in the
 * in-game pause menu play the explosion alone and sound thin. Borrowing the
 * loop brings its tune in, so instead the explosion is doubled one octave
 * lower: once the game has keyed the explosion on, its voice registers are
 * copied to a free voice at half the pitch and that voice is keyed on.
 *
 * 80160428 commits the sounds queued on a sound object. The pause menu
 * (80129528) calls it after queueing its explosion, from 801298CC (for
 * either choice: the cursor is in s1, 0 Resume, 1 Quit); the sound driver
 * keys the explosion on at its next tick, so the copy is made at the next
 * VBlank. */
#define PAUSE_CONFIRM_COMMIT_RA GA(0x801298D4u)
#define EXPLOSION_SAMPLE (0x09320u / 8)  /* SPU start address register value */
#define BOOM_VOLUME_PERCENT 50           /* of the explosion's volume */

#define SPU_VOICE(v, reg) ((v) < 24 ? 0x1F801C00u + (uint32_t)(v) * 16 + (reg) : 0x1F802C00u + (uint32_t)((v) - 24) * 16 + (reg))
#define SPU_KEY_ON 0x1F801D88u
/* voices 0..47 with the decomp's 48 (TM2_PORT_SPU48: portSpuExt in the build), its driver on 0..43 and 44..47 the
 * port's own; else the PS1's 24 */
static int spu_voices(void) { static int n = -1; if (n < 0) n = port_sym("portSpuExt") ? 48 : 24; return n; }

static uint64_t boom_at; /* VBlank to add the low explosion at; 0: none */

void F_80160428_orig(CPU *c);
void vp_SoundCommit(CPU *c)
{
    if (c->r[31] == PAUSE_CONFIRM_COMMIT_RA && vp_on(VP_PAUSE_QUIT_BOOM))
        boom_at = vp_gv() + 1;
    F_80160428_orig(c);
}

/* The port's own pause menu (ui/options.c) confirms without the game's call. */
void vp_pause_boom(void)
{
    if (vp_on(VP_PAUSE_QUIT_BOOM))
        boom_at = vp_gv() + 1;
}

static uint32_t spu_r(int v, uint32_t reg) { return rc_io_read(&M.cpu, SPU_VOICE(v, reg), 2) & 0xFFFF; }
static void spu_w(int v, uint32_t reg, uint32_t val) { rc_io_write(&M.cpu, SPU_VOICE(v, reg), val & 0xFFFF, 2); }

static void low_explosion(void)
{
    /* The envelope level (register 0xC) moves only as audio is mixed, so
     * right after key-on the explosion's voice may still read silent: any
     * voice set to the sample will do, one that sounds is preferred. */
    int src = -1, dst = -1;
    for (int v = 0; v < spu_voices(); v++) {
        int sounding = spu_r(v, 0xC) != 0;
        if (spu_r(v, 6) == EXPLOSION_SAMPLE) {
            if (src < 0 || sounding) src = v;
        } else if (!sounding && (spu_voices() == 24 || v >= 44)) {
            dst = v;                         /* silent: free (the highest one; with 48, one of the port's 44..47) */
        }
    }
    if (src < 0 || dst < 0) {
        if (M.trace) fprintf(stderr, "vplus: pause quit boom: no explosion voice (%d) or free voice (%d)\n", src, dst);
        return;
    }
    for (uint32_t reg = 0; reg < 4; reg += 2) { /* volume: fixed-level values only */
        uint32_t vol = spu_r(src, reg);
        spu_w(dst, reg, (vol & 0x8000) ? vol : (vol & 0x7FFF) * BOOM_VOLUME_PERCENT / 100);
    }
    spu_w(dst, 4, spu_r(src, 4) / 2);      /* pitch: one octave lower */
    spu_w(dst, 6, EXPLOSION_SAMPLE);
    spu_w(dst, 8, spu_r(src, 8));          /* ADSR */
    spu_w(dst, 0xA, spu_r(src, 0xA));
    spu_w(dst, 0xE, EXPLOSION_SAMPLE);     /* repeat address */
    spu_port_keyon = 1; /* not thinned */
    int w = dst < 24 ? dst : dst - 24;
    rc_io_write(&M.cpu, (dst < 24 ? SPU_KEY_ON : 0x1F802D88u) + (w >= 16 ? 2u : 0u), 1u << (w & 15), 2);
    spu_port_keyon = 0;
    if (M.trace) fprintf(stderr, "vplus: pause quit boom (explosion on voice %d, low copy on voice %d)\n", src, dst);
}

void hudlay_vblank(void); /* hud_layout.c */
static void carsel_sound_vblank(void);
void wide_vblank(void); /* wide.c */
int feats_in_battle(void);
int vp_dt_player_offered(void) /* a player may pick Dark Tooth (decomp request 36: portCarOptions bit 8, read live by car
                                * select): unlocked by every driver's ending (TM2PC_DT_PLAYER: tests) */
{
    int profile_dark_tooth_unlocked(void);
    static int test = -1; /* (cached: car select asks every frame) */
    if (test < 0) test = getenv("TM2PC_DT_PLAYER") != NULL;
    return test || profile_dark_tooth_unlocked();
}
/* portCarOptions bit 8, a player may pick Dark Tooth: set both ways (a profile signed in without him takes him out
 * again; the user's 2026-11-18, a new profile made after one with every ending had him). Each VBlank out of an online
 * match, and once at its start (net/game_tm2.c, every machine: Network Play offers him to everyone) */
void vp_dt_offer_apply(void)
{
    static uint32_t opt = 1;
    if (opt == 1) opt = port_sym("portCarOptions");
    if (!opt) return;
    uint32_t o = mem_r32(&M.cpu, opt), want = vp_dt_player_offered() ? (o | 0x100u) : (o & ~0x100u);
    if (want != o) mem_w32(&M.cpu, opt, want);
}
void ui_menu_music_vblank(void); /* ui/options.c: the menus' song on, if something silenced it */
void vp_watch_out_of_battle(void); /* (below: the spectators' watch ended between matches) */
void vp_vblank(void)
{
    vp_watch_out_of_battle();
    hudlay_vblank();
    ui_menu_music_vblank();
    wide_vblank();
    int netplay_in_match(void); /* net/game_tm2.c */
    if (!netplay_in_match()) vp_dt_offer_apply(); /* (not during an online match: the word is in the match's state, the
                * host's on every machine, and a joiner with every ending made it differ each step, as profile_hooks.c's
                * unlock flags did; vp_dt_offer_apply at the match's start instead) */
    /* portVisual bit 6 (decomp request 72, HIDDEN_AREAS.md): the hidden areas (Paris's church) drawn from anywhere, only while the depth
     * buffer is on (it hides what is behind walls; the PS1's order would paint the inside over them). Each machine's
     * own drawing (not in the online hash), read at every renderScene. */
    static uint32_t visual = 1;
    if (visual == 1) visual = port_sym("portVisual");
    if (visual) {
        int gpu_depth_enabled(void); /* runtime/gpu.c */
        uint32_t w = mem_r32(&M.cpu, visual), want = gpu_depth_enabled() ? w | 0x40u : w & ~0x40u;
        want = vp_on(VP_STEADY_SHADOW) ? want | 0x80u : want & ~0x80u; /* (bit 7, request 92, RENDER_ANSWERS.md: steady shadows, drawing only) */
        /* bit 8 (request 93, RENDER_ANSWERS.md, RENDER_ANSWERS R8): at Wobble None no polygon is cut by the near pass (its pieces' UVs are
         * averaged in screen space, the affine error fixed into them), the decomp's own switch for renderScene and
         * renderDrawModel. precise.c's hook around the emitter did it already; this no longer depends on that hook.
         * (Not the temple wall's sliding beams: PSY-Z's flipped-texture fix, patch 23) */
        extern int gte_precise_on; /* runtime/gte.c: 2 at Wobble None */
        want = gte_precise_on >= 2 ? want | 0x100u : want & ~0x100u;
        /* bit 9 (request 99, RENDER_ANSWERS R9): HUD > Mirror Height: Car, the mirror's eye on the car's roof (drawing only) */
        want = hud_of(0)->mirror_height ? want | 0x200u : want & ~0x200u; /* (split screen: each player's, mirrors.c) */
        /* bit 10 (request 102, RENDER_ANSWERS R10.2): each car's shadow sorted in front of the ground under it (New
         * York's roof tiles), with the depth test only (the car, nearer, still hides it); gpu.c's pull for it */
        extern int gpu_shadow_ground; /* runtime/gpu.c */
        gpu_shadow_ground = gpu_depth_enabled() && !getenv("TM2PC_NO_SHADOW_GROUND");
        want = gpu_shadow_ground ? want | 0x400u : want & ~0x400u;
        /* bit 11 (request 116, decomp PORT_NOTES fifth part): a polygon NCLIP culls on its whole-pixel corners tested
         * again on the exact projection, kept when it faces the eye (Hong Kong's crosswalk strips, under a pixel tall
         * far down the road with the scaled detail bands: holes in the road). Only undoes wrong culls; drawing only */
        want |= 0x800u;
        unsigned vp_visual_test_mask(void); /* vplus.c: tests */
        want &= vp_visual_test_mask();
        if (want != w) mem_w32(&M.cpu, visual, want);
    }

    carsel_sound_vblank();
    static const char *targets = (const char *)1; /* tests: TM2PC_TEST_TARGETS=V,x,y,r: at VBlank V every breakable within r
                                                    * of x, y destroyed (targetDamage: Paris's church door for request 72, HIDDEN_AREAS.md) */
    if (targets == (const char *)1) targets = getenv("TM2PC_TEST_TARGETS");
    long long tv = 0, tx = 0, ty = 0, tr = 0;
    if (targets && sscanf(targets, "%lld,%lld,%lld,%lld", &tv, &tx, &ty, &tr) == 4 && M.vblanks == (uint64_t)tv) {
        for (uint32_t r = GA(0x801909D0u); r < GA(0x801909D0u) + 50 * 20; r += 20) {
            int type = (int16_t)mem_r16(&M.cpu, r), inst = (int16_t)mem_r16(&M.cpu, r + 2), left = (int16_t)mem_r16(&M.cpu, r + 4);
            long long dx = (int32_t)mem_r32(&M.cpu, r + 8) - tx, dy = (int32_t)mem_r32(&M.cpu, r + 12) - ty;
            if (left <= 0 || dx * dx + dy * dy > tr * tr) continue;
            fprintf(stderr, "test: target %d/%d (counter %d) at %lld,%lld destroyed\n", type, inst, left, dx + tx, dy + ty);
            guest_call(GA(0x80125118u), (uint32_t)type, (uint32_t)inst, 10000, 0);
        }
    }
    if (boom_at && vp_gv() >= boom_at) {
        boom_at = 0;
        low_explosion();
    }
}

/* ---- VP_SECRET_CARS_LAST ----
 * Car select's carousel is a choice of 14 (the computer-opponent select has 12)
 * entries, the car-select indices 0-13, starting at Roadkill (7). Its arrows
 * step it with TM2FourWay::run (8012305C, the decomp's object_control_helpers.cpp):
 * mode 0 adds the step (+1/-1) modulo the entry count and repeats while the
 * accept callback (8012CC10: locked or taken) rejects the car. So Sweet Tooth
 * (12) and Minion (13) come between Shadow (11) and Hammerhead (0), in the
 * middle of the list as seen from Roadkill. With this change a 14-entry
 * choice steps along CAR_ORDER instead: the regular cars from Roadkill round
 * to Spectre, then Sweet Tooth and Minion, then Roadkill again.
 *
 * The screen opens by setting the carousel to the car before the start car
 * (D_80180D14, kept as "car - 1") with 80122078 and stepping it +1, so that
 * value is translated to the car before it in CAR_ORDER. */
#define SELECT_FIND_CHOICE GA(0x80123F34u)   /* object id -> its record (type 9: a choice) */
static const uint8_t CAR_ORDER[15] = {7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 12, 13, 14}; /* (14: Dark Tooth, last) */
int vp_dt_player_offered(void);
static int carousel_cars(void) { return vp_dt_player_offered() ? 15 : 14; } /* (the decomp's 15-child carousel, bit 8) */
int mod_cars_count(void), mod_car_base(int number); /* vplus/mod_cars.c: the mod cars (car numbers 0x20 + m) */
int mod_car_is_character(int number), mod_characters(void), mod_character(int i);
static int is_carousel(CPU *c, uint32_t ch) /* a car select carousel's choice (type 9, 14 or 15 entries; 0x20 + the skins) */
{
    int n = ch && mem_r8(c, ch) == 9 ? mem_r8(c, ch + 6) : 0;
    return n == 14 || n == 15 || (n > 0x20 && n == 0x20 + mod_cars_count());
}

/* the carousel's Left / Right order for a choice of n children: the cars (CAR_ORDER's with Vanilla Plus "secret cars
 * last", else their own), and the mods' characters that are on at the end of the portrait row (the user's): after
 * Dark Tooth with "secret cars last", else after Spectre (the game's order starts at Roadkill, 7, and wraps); no
 * skins (Select cycles those: vp_carousel_select) */
static int car_order(uint8_t out[160], int n)
{
    int k = 0, cars = n > 15 ? 15 : n, last = vp_on(VP_SECRET_CARS_LAST);
    for (int i = 0; i < cars; i++) {
        out[k++] = last ? CAR_ORDER[i] : (uint8_t)i;
        if (i == (last ? cars - 1 : 6))
            for (int j = 0, ch; n > 0x20 && (ch = mod_character(j)) >= 0 && k < 160; j++)
                if (ch < n) out[k++] = (uint8_t)ch;
    }
    return k;
}
static int car_order_pos(const uint8_t *order, int len, uint32_t car)
{
    if (car >= 0x20 && mod_car_base((int)car) >= 0 && !mod_car_is_character((int)car))
        car = (uint32_t)mod_car_base((int)car); /* (a skin: from its car) */
    int p = 0;
    while (p < len - 1 && order[p] != car) p++;
    return p;
}

/* ---- VP_DUPLICATE_CARS ----
 * Car select's accept callback (8012CC10) rejects a locked secret car, a car
 * a player already chose (settings +8.., the first +4) and an opponent's car
 * (+10.., the first +5). With duplicates allowed, the players' cars are left
 * out of the check while a player picks; the opponent draw (8012CD08) still
 * passes over the players' cars. */
#define SETTINGS_BLOCK GA(0x8016475Cu)
/* A font's color (fontSpriteTemplates, retail 80187238: 7 sprites of 20 bytes, r g b at +4), kept and put back: the port's
 * HUD lines set font 2 green and then reset it (128 gray), and drawn over a screen that had set its own (the globe's
 * level name, 255 161 0: menuTextInit) left it green, then gray (the user's 2026-11-09 co-op) */
uint32_t vp_font_color_get(int font)
{
    uint32_t a = GA(0x80187238u) + 20u * (uint32_t)font + 4;
    return (uint32_t)mem_r8(&M.cpu, a) | (uint32_t)mem_r8(&M.cpu, a + 1) << 8 | (uint32_t)mem_r8(&M.cpu, a + 2) << 16;
}
void vp_font_color_put(int font, uint32_t rgb)
{
    uint32_t a = GA(0x80187238u) + 20u * (uint32_t)font + 4;
    for (uint32_t i = 0; i < 3; i++) mem_w8(&M.cpu, a + i, (uint8_t)(rgb >> (8 * i)));
}
int four_humans(void);       /* vplus/four.c: 3-4 players */
int four_car_of(int player);
#define OPPONENT_DRAW_START GA(0x8012CD08u)
#define OPPONENT_DRAW_END GA(0x8012CE04u)
void F_8012CC10_orig(CPU *c);
void vp_CarAccept(CPU *c)
{
    uint32_t ra = c->r[31], car = c->r[4];
    int drawing = ra >= OPPONENT_DRAW_START && ra < OPPONENT_DRAW_END;
    if (!vp_on(VP_DUPLICATE_CARS) || drawing) {
        F_8012CC10_orig(c);
    } else {
        uint8_t humans = mem_r8(c, SETTINGS_BLOCK + 4);
        mem_w8(c, SETTINGS_BLOCK + 4, 0);
        F_8012CC10_orig(c);
        mem_w8(c, SETTINGS_BLOCK + 4, humans);
    }
    if (drawing) /* 3-4 players: their cars aren't drawn either */
        for (int h = 2; h < four_humans(); h++) if ((int)car == four_car_of(h)) c->r[2] = 0;
    /* a Challenge Match: only the drivers its game type allows */
    if (!drawing && ui_battle_live() && car < BATTLE_DRIVERS && !((battle_match_type()->drivers >> car) & 1))
        c->r[2] = 0;
}

/* settingsSetHumanCar(settings, human, number) (801378A4): every human 0-1's car handed to the game, by the game's car
 * select or the port. LOOK_DT2 (Dark Tooth's second phase, decomp request 118, DARK_TOOTH_PLAYER.md): Dark Tooth for the game, the human's
 * phase-2 bit for the port (mod_dt2_human); the game's own carousel hands 14 while it shows his second phase. */
void F_801378A4_orig(CPU *c);
int ui_carsel_game(void);
int vp_carousel_look(void); /* (below) */
void vp_SettingsSetHumanCar(CPU *c)
{
    int h = (int)c->r[5], number = (int)c->r[6];
    if (number == 14 && ui_carsel_game() && vp_carousel_look() == LOOK_DT2) number = LOOK_DT2;
    mod_dt2_human(h, number);
    if (number == LOOK_DT2) c->r[6] = 14;
    F_801378A4_orig(c);
}

/* ---- VP_LIVES_PER_LEVEL ----
 * Every tournament level starts through 801334C0 (decomp TOURNAMENT_FLOW.md:
 * the opponents, then the globe and the arena), from car select, a password
 * or load, and between levels. The lives (settings +20, the only counter;
 * the runner takes one for any human's death) are set there: 3 (2 more after
 * the first; the HUD shows the 2 to come), co-op too (one pool for now; each
 * player's own when the decomp shows how, a request to the decomp); 3 players 2, 4 players 1 (kept, the user's
 * 2026-10-27, though players 3-4 no longer bring computer cars more: vplus/four.c). */
#define LIVES_PER_LEVEL 3
void F_801334C0_orig(CPU *c);
void vp_LevelStart(CPU *c)
{
    uint8_t mode = mem_r8(c, SETTINGS_BLOCK);
    if (vp_on(VP_LIVES_PER_LEVEL) && (mode == 1 || mode == 2)) {
        /* 3 and 4 players: each player past 2 takes one of the extra lives (the user's): 3 players 1 extra life a
         * level, 4 players none */
        int humans = four_humans() > 2 ? four_humans() : 2;
        int lives = LIVES_PER_LEVEL - (mode == 2 ? humans - 2 : 0);
        mem_w8(c, SETTINGS_BLOCK + 20, (uint8_t)lives);
        if (mode == 2) mem_w8(c, SETTINGS_BLOCK + 21, (uint8_t)lives);
        if (M.trace) fprintf(stderr, "vplus: lives per level: %d\n", lives);
    }
    F_801334C0_orig(c);
}

/* ---- VP_GLACIER_FIRE (decomp request 120, RETAIL_BUGS.md 45) ----
 * Antarctica's fire pickup on floe 5 (node 80051B40, its LOD 80051B64) lies inside the hit-only mark 800561E8, which
 * the hit walk reaches first and which marks every car point in it: no car ever collects it. The decomp's
 * portGlacierFire (TM2_PORT_GLACIER_FIRE) is a build switch; the port does the same move here, so it can be an
 * option: the pickup and its LOD's center to floe-local (992, 119), z kept ((600, -800) turned by the floe's 60
 * degrees), before worldLoadStartObjects (800D1C70) registers the pickups. Every word is checked first. Level data,
 * the same on every machine: online always on (netplay_in_match). */
static int glacier_fire_move(CPU *c)
{
    const uint32_t pick = 0x80051B40u, lod = 0x80051B64u, floe = 0x80051B7Cu;
    if (mem_r8(c, pick) != 4 || mem_r16(c, pick + 2) != 401 || (int32_t)mem_r32(c, pick + 4) != 600 ||
        (int32_t)mem_r32(c, pick + 8) != -800 || (int32_t)mem_r32(c, pick + 12) != 4800)
        return 0;
    if (mem_r8(c, lod) != 2 || (int32_t)mem_r32(c, lod + 4) != 600 || (int32_t)mem_r32(c, lod + 8) != -800 ||
        (int32_t)mem_r32(c, lod + 12) != 4851)
        return 0;
    if (mem_r8(c, floe) != 5 || mem_r16(c, floe + 2) != 3100 || mem_r32(c, floe + 44) != lod) return 0;
    mem_w32(c, pick + 4, 992); mem_w32(c, lod + 4, 992);
    mem_w32(c, pick + 8, 119); mem_w32(c, lod + 8, 119);
    return 1;
}
void F_800D1C70_orig(CPU *c);
void vp_WorldLoadStartObjects(CPU *c)
{
    int netplay_in_match(void); /* net/game_tm2.c */
    if ((vp_on(VP_GLACIER_FIRE) || netplay_in_match()) && (int)guest_call(GA(0x80129AF8u), 0, 0, 0, 0) == 6) {
        uint32_t a0 = c->r[4], a1 = c->r[5]; /* (guest_call may use the argument registers) */
        int moved = glacier_fire_move(c);
        c->r[4] = a0, c->r[5] = a1;
        if (moved) fprintf(stderr, "vplus: Antarctica's fire missile out of its hidden box\n");
    }
    F_800D1C70_orig(c);
}

/* The shell's menu sounds (menuPlaySoundRun 80137D38: the select explosion 29, the cursor 28 through the
 * sound manager's object, not soundPlay): silent while the port hands the live car select's picks to
 * the game (each player's X given again: the port has played its own lock-in sound already). */
void F_80137D38_orig(CPU *c);
void vp_MenuPlaySoundRun(CPU *c)
{
    if (ui_mute_game_sounds()) return;
    F_80137D38_orig(c);
}

/* ---- VP_SPAWN_SPACING ----
 * A car starts (each round) and respawns at a start point (startNodePickRandom 800F636C: a random one
 * not taken this round; all taken: any, so cars land on each other: Los Angeles has 10 for up to 11
 * cars, and the humans are placed last). Start points are nodes of the arena's driving routes
 * (aiRouteNodes, 24 bytes: x, y s32, z s16, next s16[6], count s8). Here the game's pick stands
 * (its random number drawn as before) unless a car is near it; then the clearest start point not
 * taken, else the clearest route node: one still linked from another (a node of a section gone,
 * Antarctica's ice, is unlinked) with a way on. Clearest: the farthest from every car (the cars'
 * world objects, wrecks too). From the machine's state alone (the kill cam replays it the same). */
#define ROUTE_NODES GA(0x801B1DE8u)
#define ROUTE_NODE_COUNT GA(0x80180AD8u)
#define START_IDS GA(0x801B2B28u)      /* s16[15] */
#define START_TAKEN GA(0x801B2B46u)    /* u8[15] */
#define CAR_OBJECTS GA(0x80187AD4u)    /* each car slot's world object (position at +0xA8) */
#define SPAWN_CLEAR 700            /* nearer than this to a car: another place */
static void node_xy(int id, int32_t *x, int32_t *y)
{
    *x = (int32_t)mem_r32(&M.cpu, ROUTE_NODES + 24u * (uint32_t)id);
    *y = (int32_t)mem_r32(&M.cpu, ROUTE_NODES + 24u * (uint32_t)id + 4);
}
/* where cars are, or are about to be: the start points taken before this pick (the cars placed this
 * round: their objects show it only later), and every car's own position (humans +0x6FC, computer
 * cars +0x73C: their motion blocks) */
static uint8_t spawn_taken[15];
static int spawn_count;
/* the places picked lately (a round's start places every car in one frame, before any car's position shows it: the
 * route nodes picked past the start points were not counted, so 16 cars landed 7 or 8 on one node, the user's) */
enum { SPAWN_RECENT = 16, SPAWN_RECENT_VBLANKS = 30 };
static int32_t spawn_recent[SPAWN_RECENT][2];
static uint64_t spawn_recent_at[SPAWN_RECENT];
static int spawn_recent_n;
static int64_t nearest_car2(int32_t x, int32_t y)
{
    int64_t best = INT64_MAX;
    int32_t px, py;
    for (int k = 0; k < SPAWN_RECENT; k++) {
        if (!spawn_recent_at[k] || spawn_recent_at[k] + SPAWN_RECENT_VBLANKS < vp_gv()) continue;
        int64_t dx = spawn_recent[k][0] - x, dy = spawn_recent[k][1] - y, d = dx * dx + dy * dy;
        if (d < best) best = d;
    }
    for (int k = 0; k < spawn_count; k++) {
        if (!spawn_taken[k]) continue;
        int s = (int16_t)mem_r16(&M.cpu, START_IDS + 2u * (uint32_t)k);
        if (s < 0) continue;
        node_xy(s, &px, &py);
        int64_t dx = px - x, dy = py - y, d = dx * dx + dy * dy;
        if (d < best) best = d;
    }
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < humans + ais && k < CARS_HUMANS_MAX + cars_ai_max(); k++) {
        uint32_t pos = k < humans ? HUMAN_REC(k) + 0x6FC : AI_REC(k - humans) + 0x73C;
        if (k < humans ? k >= CARS_HUMANS_MAX : k - humans >= cars_ai_max()) continue;
        int64_t dx = (int32_t)mem_r32(&M.cpu, pos) - x, dy = (int32_t)mem_r32(&M.cpu, pos + 4) - y;
        if (dx == -(int64_t)x && dy == -(int64_t)y) continue; /* (at 0, 0: not placed yet) */
        int64_t d = dx * dx + dy * dy;
        if (d < best) best = d;
    }
    return best;
}
static void spawn_remember(int32_t x, int32_t y)
{
    int k = spawn_recent_n++ % SPAWN_RECENT;
    spawn_recent[k][0] = x;
    spawn_recent[k][1] = y;
    spawn_recent_at[k] = vp_gv() ? vp_gv() : 1;
}
void F_800F636C_orig(CPU *c);
void vp_StartNodePick(CPU *c) /* startNodePickRandom(count): a node id */
{
    int count = (int)c->r[4];
    spawn_count = count > 0 && count <= 15 ? count : 0;
    for (int k = 0; k < spawn_count; k++) spawn_taken[k] = mem_r8(c, START_TAKEN + (uint32_t)k); /* before the pick */
    F_800F636C_orig(c);
    int id = (int)(int16_t)c->r[2], n = (int)mem_r32(c, ROUTE_NODE_COUNT);
    static int dumped = -1;
    if (getenv("TM2PC_SPAWN_TRACE") && dumped != n) { /* tests: the arena's start points and route nodes */
        dumped = n;
        for (int k = 0; k < count && k < 15; k++) fprintf(stderr, "spawn: start point %d is node %d\n", k, (int16_t)mem_r16(c, START_IDS + 2u * (uint32_t)k));
        for (int i = 0; i < n && i < 400; i++)
            fprintf(stderr, "spawn: node %d at %d %d z %d, %d links\n", i, (int32_t)mem_r32(c, ROUTE_NODES + 24u * (uint32_t)i),
                    (int32_t)mem_r32(c, ROUTE_NODES + 24u * (uint32_t)i + 4), (int16_t)mem_r16(c, ROUTE_NODES + 24u * (uint32_t)i + 8),
                    (int8_t)mem_r8(c, ROUTE_NODES + 24u * (uint32_t)i + 22));
    }
    if (getenv("TM2PC_SPAWN_TRACE")) fprintf(stderr, "spawn: the game picked node %d\n", id);
    if (!vp_on(VP_SPAWN_SPACING) || id < 0 || n <= 0 || n > 400 || count <= 0 || count > 15) return;
    int32_t x, y;
    /* the decomp's start along a route link (request 85, portStartLink: set, x, y, ...): the car's start puts it there,
     * the pick's node its first; kept */
    static uint32_t start_link = 1;
    if (start_link == 1) start_link = port_sym("portStartLink");
    if (start_link && mem_r32(c, start_link)) {
        x = (int32_t)mem_r32(c, start_link + 4);
        y = (int32_t)mem_r32(c, start_link + 8);
        if (getenv("TM2PC_SPAWN_TRACE")) fprintf(stderr, "spawn: node %d, the decomp's link start at %d %d (nearest car %.0f)\n", id, x, y,
                                                 sqrt((double)nearest_car2(x, y)));
        spawn_remember(x, y);
        return;
    }
    node_xy(id, &x, &y);
    const int64_t clear2 = (int64_t)SPAWN_CLEAR * SPAWN_CLEAR;
    if (getenv("TM2PC_SPAWN_TRACE")) fprintf(stderr, "spawn: node %d nearest car %.0f (vblank %llu)\n", id, sqrt((double)nearest_car2(x, y)),
                                             (unsigned long long)vp_gv());
    if (nearest_car2(x, y) >= clear2) { spawn_remember(x, y); return; } /* the game's pick is clear */
    /* the clearest start point not taken */
    int best = -1, best_k = -1;
    int64_t best_d = -1;
    for (int k = 0; k < count; k++) {
        if (mem_r8(c, START_TAKEN + (uint32_t)k)) continue;
        int s = (int16_t)mem_r16(c, START_IDS + 2u * (uint32_t)k);
        if (s < 0 || s >= n) continue;
        node_xy(s, &x, &y);
        int64_t d = nearest_car2(x, y);
        if (d > best_d) { best_d = d; best = s; best_k = k; }
    }
    if (best >= 0 && best_d >= clear2) {
        mem_w8(c, START_TAKEN + (uint32_t)best_k, 1);
    } else { /* the clearest route node still in use */
        static uint8_t linked[400];
        memset(linked, 0, sizeof linked);
        for (int i = 0; i < n; i++) {
            uint32_t a = ROUTE_NODES + 24u * (uint32_t)i;
            int cnt = (int8_t)mem_r8(c, a + 22);
            for (int j = 0; j < cnt && j < 4; j++) { /* (next[4] at +0x0A, the count at +0x16: decomp TM2RouteNode) */
                int t = (int16_t)mem_r16(c, a + 10 + 2u * (uint32_t)j);
                if (t >= 0 && t < n) linked[t] = 1;
            }
        }
        /* only nodes on a floor many share (the ground, Paris's deck, a roof): not a ramp or a lift's path (Paris's
         * elevator, nodes 87-89: a car placed there started down its shaft, the user's 2026-10-29) */
        static int16_t zs[400];
        for (int i = 0; i < n; i++) zs[i] = (int16_t)mem_r16(c, ROUTE_NODES + 24u * (uint32_t)i + 8);
        best = -1;
        best_d = -1;
        /* (a hilly arena has hardly a floor 8 nodes share: Suicide Swamp found none and kept the game's pick, a car on
         * another, the user's 2026-10-29; then any linked node, the clearest) */
        for (int pass = 0; pass < 2 && best < 0; pass++)
            for (int i = 0; i < n; i++) {
                if (!linked[i] || (int8_t)mem_r8(c, ROUTE_NODES + 24u * (uint32_t)i + 22) <= 0) continue;
                int level = 0;
                for (int j = 0; j < n && level < 8 && pass == 0; j++) level += j != i && abs(zs[j] - zs[i]) <= 32;
                if (pass == 0 && level < 8) continue;
                node_xy(i, &x, &y);
                int64_t d = nearest_car2(x, y);
                if (d > best_d) { best_d = d; best = i; }
            }
        if (best < 0) return;
    }
    /* always logged (rare: a start point crowded), with where it puts the car: SupahFlyKev's and Slick!'s cars under
     * Amazonia and Antarctica, 2026-11-23, with no log of where they came in */
    fprintf(stderr, "spawn: node %d crowded (z %d), node %d instead (z %d, nearest car %.0f)\n", id,
                (int16_t)mem_r16(c, ROUTE_NODES + 24u * (uint32_t)id + 8), best, (int16_t)mem_r16(c, ROUTE_NODES + 24u * (uint32_t)best + 8),
                sqrt((double)best_d));
    node_xy(best, &x, &y);
    spawn_remember(x, y);
    c->r[2] = (uint32_t)best;
}

/* The game's sound call (800D3334: soundPlay(code, port, pan, volume)).
 * TM2PC_SOUND_TRACE=1 logs each. */
void F_800D3334_orig(CPU *c);
void vp_SoundPlay(CPU *c)
{
    static int trace = -1;
    if (trace < 0) trace = getenv("TM2PC_SOUND_TRACE") != NULL;
    if (trace) fprintf(stderr, "sound %llu: play %u port %u pan %d volume %u ra %08x\n", (unsigned long long)vp_gv(),
                       c->r[4], c->r[5], (int)c->r[6], c->r[7], c->r[31]);
    if (ui_mute_game_sounds()) { c->r[2] = 0; return; }
    /* Mr. Slam's lift (sound 57): weaponSlamSwingUpdate starts it each frame the claw closes (about 7 at once with the
     * port's voices, loud and distorted: the user's); one, its restarts within 12 VBlanks dropped */
    static uint64_t slam_lift_at;
    if (c->r[4] == 57 && slam_lift_at && M.vblanks - slam_lift_at < 12) { c->r[2] = 0; return; }
    if (c->r[4] == 57) slam_lift_at = M.vblanks;
    if (c->r[4] == 27 && getenv("TM2PC_TEST_NO_MENU_LOOP")) { c->r[2] = 0; return; } /* tests: the menus' loop off */
    int ui_menu_music_keeps(int code, int start); /* ui/options.c: the menus' song is the port's */
    if (ui_menu_music_keeps((int)c->r[4], 1)) { c->r[2] = 0; return; }
    uint32_t code = c->r[4];
    F_800D3334_orig(c);
    extern int rbs_resimulating, killcam_catching_up;
    if (trace) fprintf(stderr, "sound %llu: played %u -> %d%s%s\n", (unsigned long long)vp_gv(), code, (int)c->r[2],
                       rbs_resimulating ? " (a re-run)" : "", killcam_catching_up ? " (caught up)" : "");
}
/* The game's soundStop(code) (800D3464): the menus' song is the port's while it plays (ui/options.c) */
void F_800D3464_orig(CPU *c);
void vp_SoundStop(CPU *c)
{
    static int trace = -1;
    if (trace < 0) trace = getenv("TM2PC_SOUND_TRACE") != NULL;
    if (trace) fprintf(stderr, "sound %llu: stop %u ra %08x\n", (unsigned long long)vp_gv(), c->r[4], c->r[31]);
    int ui_menu_music_keeps(int code, int start);
    if (ui_menu_music_keeps((int)c->r[4], 0)) { c->r[2] = 0; return; }
    F_800D3464_orig(c);
}

/* ---- VP_LA_QUAKE_FINALE ----
 * laEventsUpdate (800D5AF0, decomp RETAIL_BUGS.md 2): once both debris lanes
 * of a quake are done, the event keeps its finale (explosions, a flash)
 * for 20 frames and retires; but the end time is kept in a signed byte, so a
 * quake whose lanes finish after frame 107 retires at once. Here the end time
 * is kept apart (s16) and the event is kept alive until it. */
#define LA_EVENT_GET GA(0x800D5560u)
void F_800D5AF0_orig(CPU *c);
void vp_LaEvents(CPU *c)
{
    static int16_t end[3];
    F_800D5AF0_orig(c);
    if (!vp_on(VP_LA_QUAKE_FINALE)) return;
    for (int k = 1; k < 3; k++) {
        uint32_t p = guest_call(LA_EVENT_GET, (uint32_t)k, 0, 0, 0);
        if (!p) continue;
        int16_t t = (int16_t)mem_r16(c, p + 10);
        int done = mem_r8(c, p + 6) && mem_r8(c, p + 7);
        if (!done) { end[k] = 0; continue; }       /* not at its finale (or a new quake) */
        if (end[k] < 0) continue;                 /* its finale is over */
        if (!end[k]) {
            if (mem_r8(c, p + 1) == 0 && t == 0) continue;
            end[k] = (int16_t)(t + 20);          /* the finale's first frame */
        }
        if (mem_r8(c, p + 1) == 0) {
            if (t < end[k]) mem_w8(c, p + 1, 1); /* retired early by the byte timer: the finale goes on */
            else end[k] = -1;
        }
    }
}

/* ---- VP_AXEL_EYES ----
 * Axel's body texture is the first TIM of TMS/AX.TMS (8 bits a pixel, 128
 * wide); the model mirrors its left half, so each eye is the 2 pixels at
 * (2, 6) and (3, 6): solid red. Fixed as the file is read from the disc:
 * the inner pixel the palette's color nearest dark brown, the outer one the
 * nearest white. */
/* The patch of Axel's texture drawn sharp (PSY-Z patch 10): texels u 0..7, v 4..8 (the eye, u 2-3 of row 6) */
enum { AXEL_U0 = 0, AXEL_V0 = 4, AXEL_PW = 8, AXEL_PH = 5, AXEL_SCALE = 16, AXEL_HD_SLOT = 2 };
static struct {
    int ready; uint32_t lba; uint32_t off; uint8_t idx[2];
    int img_h; uint8_t sig[256]; uint8_t clut[512]; uint8_t patch[AXEL_PH * 128]; /* rows AXEL_V0.. */
    int vx, vy, cx, cy, have_img, have_clut, hd_on; /* where its image and CLUT are in VRAM */
} axel;
static int nearest(const uint8_t *clut, int n, int r, int g, int b)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < n; i++) {
        int v = clut[i * 2] | clut[i * 2 + 1] << 8;
        int cr = (v & 31) << 3, cg = (v >> 5 & 31) << 3, cb = (v >> 10 & 31) << 3;
        int d = (cr - r) * (cr - r) + (cg - g) * (cg - g) + (cb - b) * (cb - b);
        if (v && d < bd) { bd = d; best = i; }
    }
    return best;
}
static void axel_find(void)
{
    uint32_t lba, size;
    uint8_t sec[2048 * 4];
    axel.ready = -1;
    if (!cd_find_file("\\TMS\\AX.TMS;1", &lba, &size) && !cd_find_file("TMS/AX.TMS", &lba, &size)) return;
    for (int i = 0; i < 4; i++) if (!cd_read_sector(lba + (uint32_t)i, sec + i * 2048)) return;
    const uint32_t tim = 0x30;
    uint32_t flags = sec[tim + 4] | sec[tim + 5] << 8;
    if (sec[tim] != 0x10 || (flags & 7) != 1 || !(flags & 8)) return; /* an 8-bit TIM with a CLUT */
    uint32_t clut_len = sec[tim + 8] | sec[tim + 9] << 8 | sec[tim + 10] << 16;
    const uint8_t *clut = sec + tim + 8 + 12;
    uint32_t img = tim + 8 + clut_len, w = (uint32_t)(sec[img + 8] | sec[img + 9] << 8) * 2;
    if (w != 128) return;
    axel.off = img + 12 + 6 * w + 2;
    /* for the HD eye: the image's first rows and the CLUT, to know them when they reach VRAM */
    axel.img_h = sec[img + 10] | sec[img + 11] << 8;
    memcpy(axel.sig, sec + img + 12, sizeof axel.sig);
    memcpy(axel.clut, clut, sizeof axel.clut);
    memcpy(axel.patch, sec + img + 12 + AXEL_V0 * w, sizeof axel.patch);
    axel.lba = lba;
    axel.idx[0] = (uint8_t)nearest(clut, 256, 74, 44, 22);    /* the inner pixel: dark brown */
    axel.idx[1] = (uint8_t)nearest(clut, 256, 232, 226, 214); /* the outer one: white */
    axel.ready = 1;
    if (M.trace || getenv("TM2PC_AXEL_DEBUG")) fprintf(stderr, "vplus: axel eyes at lba %u + %u, colors %u %u\n", axel.lba, axel.off, axel.idx[0], axel.idx[1]);
}
/* The sharp eye: the patch's texels (the CLUT's colors) at AXEL_SCALE, then
 * each eye (mirrored on the car: both are texels 2-3 of row 6) as white with
 * a round dark brown iris and a pupil, toward the inner (u 2) side. */
static void axel_hd_put(void)
{
    enum { W = AXEL_PW * AXEL_SCALE, H = AXEL_PH * AXEL_SCALE };
    static uint8_t rgba[W * H * 4];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int t = axel.patch[(y / AXEL_SCALE) * 128 + AXEL_U0 + x / AXEL_SCALE];
            uint16_t c = (uint16_t)(axel.clut[t * 2] | axel.clut[t * 2 + 1] << 8);
            uint8_t *o = rgba + ((size_t)y * W + x) * 4;
            o[0] = (uint8_t)((c & 31) * 255 / 31);
            o[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31);
            o[2] = (uint8_t)(((c >> 10) & 31) * 255 / 31);
            o[3] = c ? 255 : 0;
        }
    /* the old eye's texels (red) take the skin above them (row 5) first */
    for (int y = (6 - AXEL_V0) * AXEL_SCALE; y < (7 - AXEL_V0) * AXEL_SCALE; y++)
        for (int x = (2 - AXEL_U0) * AXEL_SCALE; x < (4 - AXEL_U0) * AXEL_SCALE; x++)
            memcpy(rgba + ((size_t)y * W + x) * 4, rgba + ((size_t)(y - AXEL_SCALE) * W + x) * 4, 4);
    /* the eye: an almond over texels 2-3 of row 6 (2 texels by 1, a little taller) */
    float ex = (2 - AXEL_U0 + 1.0f) * AXEL_SCALE, ey = (6 - AXEL_V0 + 0.5f) * AXEL_SCALE;
    float rx = 1.05f * AXEL_SCALE, ry = 0.62f * AXEL_SCALE;
    float ix = ex - 0.25f * AXEL_SCALE, ir = 0.5f * AXEL_SCALE, pr = 0.2f * AXEL_SCALE;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float d = (px - ex) * (px - ex) / (rx * rx) + (py - ey) * (py - ey) / (ry * ry);
            if (d > 1.0f) continue;
            uint8_t *o = rgba + ((size_t)y * W + x) * 4;
            float di = sqrtf((px - ix) * (px - ix) + (py - ey) * (py - ey));
            const uint8_t white[3] = {236, 230, 220}, brown[3] = {74, 44, 22}, pupil[3] = {12, 8, 6}, rim[3] = {40, 26, 16};
            const uint8_t *col = di < pr ? pupil : di < ir ? brown : white;
            if (d > 0.8f && col == white) col = rim; /* the lid's line */
            float a = d > 0.9f ? (1.0f - d) / 0.1f : 1.0f; /* soft edge */
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)(o[k] + (col[k] - o[k]) * a);
            o[3] = 255;
        }
    if (getenv("TM2PC_AXEL_DUMP")) { /* tests: the picture, raw RGBA */
        FILE *f = fopen(getenv("TM2PC_AXEL_DUMP"), "wb");
        if (f) { fwrite(rgba, 1, sizeof rgba, f); fclose(f); }
    }
    int page_x = axel.vx & ~63, page_y = axel.vy & ~255;
    int u0 = (axel.vx - page_x) * 2 + AXEL_U0, v0 = axel.vy - page_y + AXEL_V0;
    axel.hd_on = Psyz_VideoSetTextureHD(AXEL_HD_SLOT, page_x, page_y, axel.cy * 64 + axel.cx / 16, u0, v0,
                                        u0 + AXEL_PW, v0 + AXEL_PH, rgba, W, H);
    for (int k = 0; k < 4; k++) /* car select's team colors: the same eye with their palette rows (768, 40 + player: 1-4) */
        Psyz_VideoSetTextureHD(AXEL_HD_SLOT + 1 + k, page_x, page_y, (40 + k) * 64 + 768 / 16, u0, v0, u0 + AXEL_PW,
                               v0 + AXEL_PH, rgba, W, H);
    if (getenv("TM2PC_AXEL_DEBUG"))
        fprintf(stderr, "vplus: axel HD eye %s (image %d,%d, CLUT %d,%d)\n", axel.hd_on ? "on" : "not drawn", axel.vx,
                axel.vy, axel.cx, axel.cy);
}

/* Each image put in VRAM: Axel's texture and its CLUT are known by their
 * pixels (wherever the car's slot puts them); with both, the sharp eye. An
 * upload over its place (another car's texture) ends it. */
void vp_vram_upload(int x, int y, int w, int h, const uint16_t *pix)
{
    if (axel.ready <= 0 || !vp_on(VP_AXEL_EYES)) return;
    int was = axel.have_img && axel.have_clut;
    if (w == 64 && h == axel.img_h && !memcmp(pix, axel.sig, sizeof axel.sig)) {
        axel.vx = x; axel.vy = y; axel.have_img = 1;
    } else if (w == 256 && h == 1 && !memcmp(pix, axel.clut, sizeof axel.clut)) {
        axel.cx = x; axel.cy = y; axel.have_clut = 1;
    } else if (axel.have_img && x < axel.vx + 64 && x + w > axel.vx && y < axel.vy + axel.img_h && y + h > axel.vy) {
        axel.have_img = 0; /* overwritten */
    } else {
        return;
    }
    if (axel.have_img && axel.have_clut && (!was || !axel.hd_on)) axel_hd_put();
    else if (!(axel.have_img && axel.have_clut) && axel.hd_on) {
        for (int k = 0; k < 5; k++) Psyz_VideoClearTextureHD(AXEL_HD_SLOT + k);
        axel.hd_on = 0;
    }
}

/* Axel's eye entries keep their colors in a team's palette (VP_AXEL_EYES: white and dark brown, not team-tinted) */
static int axel_eye_entry(int car, int i)
{
    return car == 9 && axel.ready > 0 && vp_on(VP_AXEL_EYES) && (i == axel.idx[0] || i == axel.idx[1]);
}

/* Warthog's side (Vanilla Plus "Warthog's side trim"): his body texture has a 4-row gray strip above the side view's
 * windows (rows 84..87, a light gray to a dark one), drawn on the model above the side windows; the 1997 PC version's
 * texture has the body's camo there instead (a mistake the PC version fixed: Gibbers's, 2026-11-25). The strip's pixels
 * take the camo of rows 78..81 above them, in TMS\WH.TMS's body image and in the car selects' copies of it
 * (SHELLDB1\CARSEL.TMS, AICARSEL.TMS: the image whose palette is at 768, 25), as the files are read (as Axel's eyes).
 * Found by the strip's own colors, so another texture (a mod's, another disc's) is left alone. */
enum { WH_PATCH_MAX = 4 * 128 };
static struct { int ready; int n_files; struct { uint32_t lba, n; uint32_t off[WH_PATCH_MAX]; uint8_t val[WH_PATCH_MAX]; } f[3]; } wh;
static int wh_gray(const uint8_t *clut, int i, int r, int g, int b) /* the palette entry is (r, g, b) give or take a step */
{
    uint16_t c = (uint16_t)(clut[i * 2] | clut[i * 2 + 1] << 8);
    int cr = (c & 31) * 255 / 31, cg = ((c >> 5) & 31) * 255 / 31, cb = ((c >> 10) & 31) * 255 / 31;
    return abs(cr - r) <= 10 && abs(cg - g) <= 10 && abs(cb - b) <= 10;
}
static void wh_file(const char *iso_name, const char *name, int want_clut_y)
{
    uint32_t lba, size;
    if (wh.n_files >= 3 || (!cd_find_file(iso_name, &lba, &size) && !cd_find_file(name, &lba, &size))) return;
    if (size > 0x100000) return;
    uint8_t *data = malloc(size + 2048);
    if (!data) return;
    for (uint32_t s = 0; s * 2048 < size; s++)
        if (!cd_read_sector(lba + s, data + s * 2048)) { free(data); return; }
    for (uint32_t t = 0; t + 0x220 < size; t += 4) { /* each 8-bit TIM with a CLUT: magic 0x10, flags 9 */
        if (data[t] != 0x10 || data[t + 1] || data[t + 2] || data[t + 3] || (data[t + 4] & 0xF) != 9) continue;
        uint32_t clut_len = data[t + 8] | data[t + 9] << 8 | data[t + 10] << 16;
        int cy = data[t + 14] | data[t + 15] << 8;
        if (clut_len != 0x20C || (want_clut_y >= 0 && cy != want_clut_y)) continue;
        const uint8_t *clut = data + t + 20;
        uint32_t img = t + 8 + clut_len;
        if (img + 12 + 128 * 128 > size) continue;
        uint32_t w = (uint32_t)(data[img + 8] | data[img + 9] << 8) * 2, h = data[img + 10] | data[img + 11] << 8;
        if (w != 128 || h != 128) continue;
        const uint8_t *px = data + img + 12;
        /* the strip: row 84 light (131, 139, 156) to row 87 dark (65, 74, 74), the same entry along each row */
        int gray = px[84 * 128 + 40];
        if (!wh_gray(clut, gray, 131, 139, 156) || !wh_gray(clut, px[87 * 128 + 40], 65, 74, 74)) continue;
        int k = wh.n_files, n = 0;
        for (int x = 0; x < 128; x++) {
            if (px[84 * 128 + x] != gray) continue;
            for (int r = 0; r < 4 && n < WH_PATCH_MAX; r++) {
                wh.f[k].off[n] = img + 12 + (uint32_t)((84 + r) * 128 + x);
                wh.f[k].val[n++] = px[(78 + r) * 128 + x];
            }
        }
        wh.f[k].lba = lba, wh.f[k].n = (uint32_t)n;
        wh.n_files++;
        if (M.trace || getenv("TM2PC_WARTHOG_DEBUG")) fprintf(stderr, "vplus: warthog's strip in %s, %d pixels\n", name, n);
        break;
    }
    free(data);
}
static void wh_disc_sector(uint32_t lba, uint8_t *sec)
{
    if (!vp_on(VP_WARTHOG_TRIM)) return;
    if (!wh.ready) {
        wh.ready = 1;
        wh_file("\\TMS\\WH.TMS;1", "TMS/WH.TMS", -1);
        wh_file("\\SHELLDB1\\CARSEL.TMS;1", "SHELLDB1/CARSEL.TMS", 23 + 2);
        wh_file("\\SHELLDB1\\AICARSEL.TMS;1", "SHELLDB1/AICARSEL.TMS", 23 + 2);
    }
    for (int k = 0; k < wh.n_files; k++)
        for (uint32_t i = 0; i < wh.f[k].n; i++)
            if (lba == wh.f[k].lba + wh.f[k].off[i] / 2048) sec[wh.f[k].off[i] % 2048] = wh.f[k].val[i];
}
void vp_disc_sector(uint32_t lba, uint8_t *sec)
{
    wh_disc_sector(lba, sec);
    if (!vp_on(VP_AXEL_EYES)) return;
    if (!axel.ready) axel_find();
    if (axel.ready < 0) return;
    for (int k = 0; k < 2; k++) {
        uint32_t o = axel.off + (uint32_t)k;
        if (lba == axel.lba + o / 2048) {
            sec[o % 2048] = axel.idx[k];
            if (getenv("TM2PC_AXEL_DEBUG")) fprintf(stderr, "vplus: axel eye pixel %d set\n", k);
        }
    }
}

void F_80122078_orig(CPU *c);
void vp_ChoiceSet(CPU *c)
{
    /* car select (801307EC) setting its carousel (object 9005), by the function and the object, not the exact return
     * address (the decomp recompiled car select longer: the address no longer matched, the start untranslated, and back
     * from CAR INFO the carousel landed on another car, the user's): it opens on "car - 1" and steps once, so with the
     * port's order (vp_FourWayRun) the start is the car before in that order; a skin (the last car chosen) opens on its
     * car, a character on itself */
    if (gr_function(c->r[31]) == 0x801307ECu && mem_r16(c, c->r[4]) == 9005) {
        static uint32_t sel = 1;
        if (sel == 1) sel = port_sym("portCarSel");
        int n = sel ? (int)mem_r32(c, sel + 4) : 0, car = (int)c->r[5] + 1;
        if (n <= 0) n = carousel_cars();
        if (car >= 0x20 && mod_car_base(car) >= 0 && !mod_car_is_character(car)) car = mod_car_base(car);
        if (vp_on(VP_SECRET_CARS_LAST) || n > 0x20) {
            uint8_t order[160];
            int len = car_order(order, n), p = car_order_pos(order, len, (uint32_t)car % (uint32_t)(n > 0 ? n : 1));
            c->r[5] = order[(p + len - 1) % len];
        } else if (car != (int)c->r[5] + 1) {
            c->r[5] = (uint32_t)(car ? car - 1 : n - 1);
        }
    }
    F_80122078_orig(c);
}

/* Car select's car (the carousel, object 9005), placed higher with Vanilla
 * Plus "Car select portrait bar": nearer the middle of the screen, so a big
 * car (Minion) does not reach the name and the portraits below. Its height
 * is the third coordinate of sub_80123E1C (place) and sub_80123AC8 (move to);
 * +10 raises it about 10 lines. TM2PC_CAR_DY overrides (tests). */
#define CAROUSEL_ID 9005
#define CAR_RAISE 10
static int car_dy(void)
{
    const char *e = getenv("TM2PC_CAR_DY");
    return e ? atoi(e) : vp_on(VP_CARSEL_PORTRAITS) ? CAR_RAISE : 0;
}

void F_80123E1C_orig(CPU *c);
void vp_ObjectPlace(CPU *c) /* sub_80123E1C(object, x, y, z) */
{
    /* not from the actions (80122CA4 a move-to: its target is raised already;
     * 80122D58 a step: from where the car is), or it would climb each frame */
    if (mem_r16(c, c->r[4]) == CAROUSEL_ID && c->r[31] != GA(0x80122CA4u) && c->r[31] != GA(0x80122D58u))
        c->r[7] += (uint32_t)car_dy();
    F_80123E1C_orig(c);
}

void F_80123AC8_orig(CPU *c);
void vp_MoveTo(CPU *c) /* sub_80123AC8(action, target, x, y, z, frames) */
{
    if (mem_r16(c, c->r[5]) == CAROUSEL_ID) /* z, the fifth argument */
        mem_w32(c, c->r[29] + 16, mem_r32(c, c->r[29] + 16) + (uint32_t)car_dy());
    F_80123AC8_orig(c);
}

/* Car select's carousel (any 14-entry choice): its record, kept from its
 * steps (the screen opens with one), so the car it is on can be read. */
static uint32_t carousel;
int vp_carousel_car(void) /* (the car: a skin's base) */
{
    int car = carousel ? mem_r8(&M.cpu, carousel + 7) : -1;
    return car >= 0x20 ? mod_car_base(car) : car;
}
int vp_carousel_number(void) { return carousel ? mem_r8(&M.cpu, carousel + 7) : -1; } /* (a skin: its own number) */
/* Dark Tooth's second phase on the carousel (LOOK_DT2, decomp request 118, DARK_TOOTH_PLAYER.md): the carousel stays on Dark Tooth (14), its
 * model the head alone (mod_cars_carsel_frame); only while the carousel is on 14 */
static int carousel_dt2;
int vp_carousel_look(void) /* the look shown: a skin's number, LOOK_DT2, else the car */
{
    int n = vp_carousel_number();
    if (n != 14) carousel_dt2 = 0;
    return n == 14 && carousel_dt2 ? LOOK_DT2 : n;
}
/* Select in car select: the highlighted car's next look (its own, then its skins that are on); 1 if it changed */
int vp_carousel_select(void)
{
    int mod_next_look(int number);
    int now = vp_carousel_look(), next = now >= 0 ? mod_next_look(now) : now;
    if (next == now || !carousel) return 0;
    if (next == LOOK_DT2 || next == 14) { /* (on and off his second phase: the same child) */
        carousel_dt2 = next == LOOK_DT2;
        mem_w8(&M.cpu, carousel + 7, 14);
        return 1;
    }
    if (next >= mem_r8(&M.cpu, carousel + 6)) return 0;
    carousel_dt2 = 0;
    mem_w8(&M.cpu, carousel + 7, (uint8_t)next);
    return 1;
}
void vp_carousel_forget(void) { carousel = 0; carousel_dt2 = 0; }
/* the carousel on car number `number` (a skin's own too) as its player's turn begins, and the game's start car
 * (carSelectStartCar 80180D14, which CAR INFO's return also sets) with it; 0 before the carousel is known */
int vp_carousel_start(int number)
{
    carousel_dt2 = number == LOOK_DT2;
    if (number == LOOK_DT2) number = 14;
    if (!carousel || number < 0 || number >= mem_r8(&M.cpu, carousel + 6)) return 0;
    mem_w8(&M.cpu, carousel + 7, (uint8_t)number);
    mem_w32(&M.cpu, GA(0x80180D14u), (uint32_t)(number >= 0x20 ? mod_car_base(number) : number));
    return 1;
}

/* ---- VP_CARSEL_SOUNDS ----
 * The highlighted driver's special weapon sound, as weaponFireSpecial
 * (8010xxxx, decomp weapon_basic.c) plays it for a player, by car type
 * (carIndexToType 8012B044): Hammerhead's the crush (car_crash.c), Thumper's
 * the flame (weaponFlameStart), Mr. Slam's the arm (weapon_slam_swing.c),
 * Minion's 13. The one sound bank (UACORE.VAB) is loaded from boot, so they
 * play in car select too. The last one is stopped first. */
static const uint8_t special_sounds[16] = {0, 0x3D, 0x25, 0x27, 0x29, 0x2B, 0x2D, 0x2F,
                                           0x31, 0x33, 0x35, 0x37, 0x39, 0x3B, 13, 0};
int ui_carsel_live(void); /* ui/options.c: the port's live car select (2-4 players) */
/* The sound goes to the sound chip at once (soundUpdate, then soundFlush: the car select
 * commits nothing until its next move, so each driver's sound came one step late). The game's
 * sound bank builds some specials from several copies of one sample started together (Axel's
 * and Mr. Slam's 4-5, Hammerhead's crush 3): in a battle they are meant to be loud. Here every
 * copy plays, each at its share (the voices keyed on are noted and scaled), and all specials
 * quieter than the battle's (CARSEL_SOUND_LEVEL); a sound ends after 4 seconds (the flame loops
 * would go on). */
#define CARSEL_SOUND_LEVEL 55 /* percent of the effects volume, for one copy */
extern int spu_keyon_record;
extern uint64_t spu_keyed; /* (bit v: voice v, 0..47; 24..47 the second register block, hw.c) */
#define CARSEL_VOICES 48
static uint32_t voice_reg(int v) { return v < 24 ? 0x1F801C00u + 16u * (uint32_t)v : 0x1F802C00u + 16u * (uint32_t)(v - 24); }
void audio_voice_scale(int voice, int percent);
void audio_voice_scale_clear(void);
void audio_volumes_vblank(void);
static int carsel_playing;
static uint64_t carsel_voices; /* the voices its copies play on (keyed off at its end: a loop stops too) */
static uint64_t carsel_stop_at;
static int carsel_level; /* the copies' volume (percent): faded out over the last frames */
static uint16_t carsel_start[CARSEL_VOICES]; /* each copy's sample (voice +6): another sound on the voice is not ours */
#define CARSEL_FADE 12
static void carsel_commit(void)
{
    guest_call(GA(0x800D34C0u), 0, 0, 0, 0); /* soundUpdate */
    guest_call(GA(0x800D34E8u), 0, 0, 0, 0); /* soundFlush */
}
void vp_carsel_sound(int car)
{
    if (carsel_playing) {
        guest_call(GA(0x800D3464u), (uint32_t)carsel_playing, 0, 0, 0); /* soundStop */
        carsel_commit();
        if (carsel_voices & 0xFFFF) rc_io_write(&M.cpu, SPU_KEY_ON + 4, (uint32_t)(carsel_voices & 0xFFFF), 2);   /* key off */
        if ((carsel_voices >> 16) & 0xFF) rc_io_write(&M.cpu, SPU_KEY_ON + 6, (uint32_t)((carsel_voices >> 16) & 0xFF), 2);
        if ((carsel_voices >> 24) & 0xFFFF) rc_io_write(&M.cpu, 0x1F802D8Cu, (uint32_t)((carsel_voices >> 24) & 0xFFFF), 2); /* (24..47) */
        if (carsel_voices >> 40) rc_io_write(&M.cpu, 0x1F802D8Eu, (uint32_t)(carsel_voices >> 40), 2);
        carsel_voices = 0;
        audio_voice_scale_clear();
    }
    carsel_playing = 0;
    carsel_stop_at = 0;
    if (car < 0 || !vp_on(VP_CARSEL_SOUNDS)) return;
    int type = (int)guest_call(GA(0x8012B044u), GA(0x8016475Cu), (uint32_t)car, 0, 0);
    int code = type >= 0 && type < 256 ? special_sounds[type >> 4] : 0;
    if (getenv("TM2PC_SOUND_TRACE")) fprintf(stderr, "carsel sound: car %d type %d code %d\n", car, type, code);
    if (!code) return;
    spu_keyed = 0;
    spu_keyon_record = 1;
    guest_call(GA(0x800D3334u), (uint32_t)code, 0, 0, 99);
    carsel_commit();
    spu_keyon_record = 0;
    for (int v = 0; v < CARSEL_VOICES; v++) /* not the menu's cursor click, sent with it (sample 0x1A70) */
        if (((spu_keyed >> v) & 1) && (rc_io_read(&M.cpu, voice_reg(v) + 6, 2) & 0xFFFF) == 0x1A70u) spu_keyed &= ~(1ull << v);
    int n = 0;
    for (int v = 0; v < CARSEL_VOICES; v++) n += (int)((spu_keyed >> v) & 1);
    carsel_level = n ? CARSEL_SOUND_LEVEL * (n > 2 ? 2 : n) / n * (car == 0 ? 60 : car == 9 ? 48 : 100) / 100 : 0; /* Hammerhead, Axel: lower */
    for (int v = 0; v < CARSEL_VOICES && n; v++)
        if ((spu_keyed >> v) & 1) { /* the copies together: one or two copies' worth; Hammerhead's crush a loud sample */
            audio_voice_scale(v, carsel_level);
            carsel_start[v] = (uint16_t)rc_io_read(&M.cpu, voice_reg(v) + 6, 2);
        }
    audio_volumes_vblank();
    if (getenv("TM2PC_SOUND_TRACE")) fprintf(stderr, "carsel sound: %d voices, each at %d%%\n", n, n ? CARSEL_SOUND_LEVEL * (n > 2 ? 2 : n) / n : 0);
    carsel_playing = code;
    carsel_voices = spu_keyed;
    carsel_stop_at = vp_gv() + (car == 5 || car == 7 ? 96 : 240); /* Thumper's and Roadkill's flames loop: as long as the longest other */
}
static void carsel_sound_vblank(void) /* the special's end */
{
    /* a voice another sound took (Car Info's explosion, the menu's): its own volume again (it played at the special's
     * share, or faded to nothing: "very quiet", the user's) */
    for (int v = 0; carsel_playing && v < CARSEL_VOICES; v++)
        if (((carsel_voices >> v) & 1) && (uint16_t)rc_io_read(&M.cpu, voice_reg(v) + 6, 2) != carsel_start[v]) {
            carsel_voices &= ~(1ull << v);
            audio_voice_scale(v, 0); /* (0: not scaled) */
            if (getenv("TM2PC_SOUND_TRACE")) fprintf(stderr, "carsel sound: voice %d taken by another sound, its volume back\n", v);
            audio_volumes_vblank();
        }
    if (carsel_playing && carsel_stop_at && vp_gv() >= carsel_stop_at) { vp_carsel_sound(-1); return; }
    if (carsel_playing && carsel_stop_at && vp_gv() + CARSEL_FADE > carsel_stop_at) { /* a short fade: no hard cut */
        int left = (int)(carsel_stop_at - vp_gv());
        for (int v = 0; v < CARSEL_VOICES; v++)
            if ((carsel_voices >> v) & 1) audio_voice_scale(v, carsel_level * left / CARSEL_FADE);
        audio_volumes_vblank();
    }
}

void F_8012305C_orig(CPU *c);
void vp_FourWayRun(CPU *c)
{
    uint32_t cmd = c->r[4];
    if (mem_r32(c, cmd + 0x10) == 0) {
        uint32_t ch = guest_call(SELECT_FIND_CHOICE, mem_r32(c, cmd + 0x0C), 0, 0, 0);
        if (is_carousel(c, ch)) carousel = ch;
        static int started; /* tests: TM2PC_CARSEL_START=N, the carousel on car number N at its first step */
        if (!started && carousel && getenv("TM2PC_CARSEL_START")) { mem_w8(c, carousel + 7, (uint8_t)atoi(getenv("TM2PC_CARSEL_START"))); started = 1; }
    }
    if (mem_r32(c, cmd + 0x10) == 0) {  /* mode 0: step (with skins always: the game's own steps would go through them) */
        uint32_t choice = guest_call(SELECT_FIND_CHOICE, mem_r32(c, cmd + 0x0C), 0, 0, 0);
        if (is_carousel(c, choice) && (vp_on(VP_SECRET_CARS_LAST) || mem_r8(c, choice + 6) > 0x20)) {
            uint8_t order[160];
            int step = (int32_t)mem_r32(c, cmd + 0x14) < 0 ? -1 : 1, n = car_order(order, mem_r8(c, choice + 6));
            uint32_t accept = mem_r32(c, cmd + 0x18);
            int p = car_order_pos(order, n, mem_r8(c, choice + 7));
            for (int tries = 0; tries < n; tries++) {
                p = (p + n + step) % n;
                if (!accept || guest_call(accept, order[p], 0, 0, 0))
                    break;
            }
            mem_w8(c, choice + 7, order[p]);
            if (getenv("TM2PC_MODS_DEBUG")) fprintf(stderr, "car select: stepped to %d (%d in its order)\n", order[p], n);
            return;
        }
    }
    F_8012305C_orig(c);
}

/* ---- the car models above 2 MB ----
 * sessionLoadArenaAndCars (8012BAFC) packs the arena's model and then each
 * car's from 0x80019C40 up to the EXE (737,300 bytes): 6-10 cars by arena.
 * The guest has 8 MB (main.c), so the car models go from 0x80200000
 * instead; the arena stays (its pointers are absolute). A car model is
 * relocated wherever it is installed (assetInstallImage, decomp
 * SIXTEEN_CARS.md 3.5). Loads at 0x80019C40 (the textures, the arena) are
 * the game's. */
#define LOAD_BASE 0x80019C40u
#define CAR_MODEL_BASE (rc_addrmap_count ? 0x80210000u : 0x80200000u) /* (the port variant's arrays at 80200000) */
#define SESSION_LOAD_START GA(0x8012BAFCu)
#define SESSION_LOAD_END GA(0x8012BD94u)
static uint32_t car_model_next, car_model_last; /* (0: CAR_MODEL_BASE) */
void F_8014DA30_orig(CPU *c);
void vp_FileLoad(CPU *c) /* fileioLoadFileIntoRam(dest, name): the size */
{
    uint32_t ra = c->r[31], dest = c->r[4];
    if (mod_arena_on()) { /* a mod arena's DMD or TMS (its names: mod_arena_file_name): from the mod's folder */
        char name[24];
        for (int i = 0; i < 23; i++) if (!(name[i] = (char)mem_r8(c, c->r[5] + (uint32_t)i))) break;
        name[23] = 0;
        if (mod_arena_file(c, dest, name)) {
            if (dest == LOAD_BASE) car_model_next = CAR_MODEL_BASE; /* (as the game's: the cars start over) */
            return;
        }
    }
    static int own_area = -1; /* the build loads the car models in its own area (decomp request 32: portCarDmdArea) */
    if (own_area < 0) own_area = port_sym("portCarDmdArea") != 0;
    if (own_area || ra < SESSION_LOAD_START || ra >= SESSION_LOAD_END) {
        F_8014DA30_orig(c);
        static uint32_t area;
        if (own_area && !area) area = port_sym("portCarDmdArea");
        if (own_area && dest >= area && dest - area < 0x400000u && getenv("TM2PC_PACKET_PEAK")) /* tests: the car models' area */
            fprintf(stderr, "car models: %u bytes at +%u, the area filled to %u\n", c->r[2], dest - area, dest - area + c->r[2]);
        return;
    }
    if (dest == LOAD_BASE) { /* a texture file or the arena: the game's place; the cars start over */
        car_model_next = CAR_MODEL_BASE;
        F_8014DA30_orig(c);
        return;
    }
    if (!car_model_next) car_model_next = CAR_MODEL_BASE;
    c->r[4] = car_model_next;
    F_8014DA30_orig(c);
    car_model_last = car_model_next;
    car_model_next = (car_model_next + c->r[2] + 2048 + 3) & ~3u; /* whole sectors are read: room for the tail */
    if (M.trace || getenv("TM2PC_LOAD_DEBUG"))
        fprintf(stderr, "vplus: car model at %08X (%u bytes; the game's place %08X)\n", car_model_last, c->r[2], dest);
}
/* ---- the Antarctica Santa (decomp SANTA.md, way A) ----
 * The PC version's pedestrian 516 #2, a 3-frame sprite: tools/art/santa.py builds ui/santa.tms (his
 * three images) and ui/santa.dmd (his nodes) from the PC files. On Antarctica (level 6) they go in as
 * one more car's assets, just before the session's VRAM bookkeeping ends (bitmapDisableTracking
 * 801215EC from sessionLoadArenaAndCars): the container parsed with the next group, the model
 * installed with it (relocated, its primitives pointed at the images, walked: targetActorSpawn makes
 * him pedestrian 516 #2). The retail code places, runs and respawns him. Vanilla Plus: VP_SANTA. */
#define SANTA_TMS 0x80CF0000u /* free high RAM: under the frame's packet buffers (vplus/four.c) */
#define SANTA_DMD 0x80CFC000u
static int last_group; /* the last car group parsed in this session load */
const char *ui_folder(void);
static int load_guest_file(const char *name, uint32_t at, uint32_t max)
{
    char path[1200];
    snprintf(path, sizeof path, "%sui/%s", ui_folder(), name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = 0, ch;
    while ((ch = fgetc(f)) != EOF && (uint32_t)n < max) mem_w8(&M.cpu, at + (uint32_t)n++, (uint8_t)ch);
    fclose(f);
    return n;
}
void F_801215EC_orig(CPU *c);
void vp_BitmapDisableTracking(CPU *c)
{
    uint32_t ra = c->r[31];
    if (ra >= SESSION_LOAD_START && ra < SESSION_LOAD_END && mem_r8(c, GA(0x8016475Cu) + 24) == 6 && vp_on(VP_SANTA)) {
        int n = last_group + 1;
        if (!getenv("TM2PC_TEST_NO_SANTA") /* (tests: a copy set up from the PS1 disc alone) */
            && load_guest_file("santa.tms", SANTA_TMS, 0xC000) > 0 && load_guest_file("santa.dmd", SANTA_DMD, 0x1000) > 0) {
            guest_call(GA(0x80121A2Cu), SANTA_TMS, (uint32_t)n, 0, 0); /* bitmapParseContainer */
            guest_call(GA(0x80120EF4u), SANTA_DMD, (uint32_t)n, 0, 0); /* assetInstallImage */
            fprintf(stderr, "santa: added to Antarctica (group %d; pedestrians %u)\n", n, mem_r32(c, GA(0x80180BB8u)));
        } else fprintf(stderr, "santa: ui/santa.tms or santa.dmd missing (tools/art/santa.py)\n");
    }
    F_801215EC_orig(c);
}

void F_80120EF4_orig(CPU *c);
void vp_InstallImage(CPU *c) /* assetInstallImage(buf, group) */
{
    uint32_t ra = c->r[31];
    if (ra >= SESSION_LOAD_START && ra < SESSION_LOAD_END && c->r[4] != LOAD_BASE && car_model_last) c->r[4] = car_model_last;
    F_80120EF4_orig(c);
}

/* ---- a Challenge Match's game type in the battle ----
 * What the game type changes that the game already has a place for
 * (decomp GAME_EVENTS.md, pickup_apply.c): each weapon's ammo per pickup
 * (pickupAmmoTable, by weapon id, refilled by pickupSlotsReset), a turned-off
 * weapon's (or health or turbo) pickup giving one of the weapons on (none on:
 * nothing), how often a pickup comes back
 * (pickupRespawnPeriod: frames, 200 the game's), infinite weapons (the
 * cheat's flag, humans), and each energy attack (weaponFireEnergyAttack's
 * kinds 100-106; the jump, 104, always). */
#define PICKUP_AMMO_TABLE GA(0x8018429Cu)
#define PICKUP_RESPAWN_PERIOD GA(0x80180AE8u)
static const int WEAPON_ID[BATTLE_WEAPONS] = {2, 1, 4, 5, 3, 6, 7};             /* homing .. lightning: the game's ids */
static const int PICKUP_ID[BATTLE_WEAPONS] = {403, 401, 404, 411, 406, 413, 419}; /* their pickups */
#define PICKUP_NOTHING 402 /* an id pickupApply does nothing with (the pickup still goes) */
#define PICKUP_SPECIAL 400 /* the port's: a special (the game gets PICKUP_NOTHING, then +1 special) */

/* Survival's rules: the game's, its computer cars only ever hunting the players (and always hurt) */
static const GameType *survival_rules(void)
{
    static GameType t;
    if (!t.drivers) { battle_type_defaults(&t, MODE_LAST_CAR); t.ai_targets = AI_TARGETS_HUMANS; }
    return &t;
}
static const GameType *battle_rules(void)
{
    return !ui_battle_live() ? NULL : ui_survival() ? survival_rules() : battle_match_type();
}
static int vp_dm(void); /* a Deathmatch: respawns for all, kills win */
static int respawn_extra(void) /* the game type's respawn time, seconds after the burning (0: none, or not a custom match) */
{
    const GameType *t = battle_rules();
    return t && t->respawn > 0 ? t->respawn : 0;
}
/* the computer cars' retargeting clocks (gameplay: kept with the kill cam's machine, vp_keep_register) */
static uint64_t spect_retarget, team_next, equal_next, hill_retarget;
static int battle_lives(void) { return ui_survival() ? 1 : vp_dm() ? 99 : battle_match()->lives; }

static int roulette_w = -1; /* the roulette's weapon now (BATTLE_WEAPONS index; -1 none yet) */
int vp_pickup_id(int id)
{
    const GameType *t = battle_rules();
    if (!t) return id;
    int keep = id >= 441 ? 40 : 0, base = id - keep; /* 441..480: the same, not taken off the map */
    if (getenv("TM2PC_TEST_PICKUP_SPECIAL")) /* tests: every weapon pickup gives a special */
        for (int w = 0; w < BATTLE_WEAPONS; w++) if (base == PICKUP_ID[w]) return PICKUP_SPECIAL + keep;
    if (t->mode == MODE_GUN_GAME && !ui_survival()) /* Gun Game: the weapon is the level's; weapon pickups give none */
        for (int w = 0; w < BATTLE_WEAPONS; w++) if (base == PICKUP_ID[w]) return PICKUP_NOTHING + keep;
    if (t->roulette && !t->roulette_add && roulette_w >= 0) /* the roulette replacing: a weapon pickup gives more of its weapon */
        for (int w = 0; w < BATTLE_WEAPONS; w++) if (base == PICKUP_ID[w]) return PICKUP_ID[roulette_w] + keep;
    int off = ((base == 417 || base == 418) && !t->health) || (base == 430 && !t->turbo);
    for (int w = 0; w < BATTLE_WEAPONS; w++)
        if (base == PICKUP_ID[w] && !((t->weapons >> w) & 1)) off = 1;
    if (!off) return id;
    if (t->mode == MODE_GUN_GAME && !ui_survival()) return PICKUP_NOTHING + keep; /* (Gun Game: the level's weapon only) */
    int on[BATTLE_WEAPONS + 1], n = 0; /* turned off: one of the weapons on, at random (none on: nothing) */
    for (int k = 0; k < BATTLE_WEAPONS; k++) if ((t->weapons >> k) & 1) on[n++] = k;
    if (t->special_pickups && t->specials) on[n++] = -1; /* or a special */
    if (!n) return PICKUP_NOTHING + keep;
    int pick = on[(int)vp_rand() % n];
    return pick < 0 ? PICKUP_SPECIAL + keep : PICKUP_ID[pick] + keep;
}

/* the ammo per pickup, and the respawn period, as each arena starts */
void F_8010AE04_orig(CPU *c);
void vp_PickupReset(CPU *c)
{
    F_8010AE04_orig(c);
    const GameType *t = battle_rules();
    static const int periods[PICKUPS_COUNT] = {200, 90, 400, 0x7FFFFFFF};
    mem_w32(c, PICKUP_RESPAWN_PERIOD, (uint32_t)(t ? periods[t->pickup_respawn % PICKUPS_COUNT] : 200));
    if (!t) return;
    for (int w = 0; w < BATTLE_WEAPONS; w++)
        mem_w32(c, PICKUP_AMMO_TABLE + 4u * (uint32_t)WEAPON_ID[w], (uint32_t)t->ammo[w]);
}

/* at each round's start (ui_MainLoop): the HUD as customized; infinite
 * weapons for the humans (weapons +0, as humanCarSetInfiniteWeapons 8011A3C4
 * writes it: that function is never called in retail, so not recompiled) */
void vp_lives_round(void);
static void hud_round(void);
static void pickups_round(void);
static void dm_round_start(void);
void interp_snap(void); /* vplus/interp.c */
void render_stats_round(void); /* render_stats.c */
void render_stats_frame(void);
int render_stats_line(char *out, size_t n, int mirror);
static void hud_text_print(CPU *c, int x, int y, const char *text, int centered);
/* The draw distance in use: Options > Video's, or online always Farthest. It changes what the game does (the far values,
 * the far bands are game memory, in every machine's hashed state): two players with two settings, or one changing it
 * in a match, parted the states every step (the user's 2026-11-09 co-op playtest: "waiting for player" over and over) */
int vp_draw_distance(void)
{
    int netplay_in_match(void);
    static int test = -2; /* tests: TM2PC_DRAW_DISTANCE=0-2, online too */
    if (test == -2) test = getenv("TM2PC_DRAW_DISTANCE") ? atoi(getenv("TM2PC_DRAW_DISTANCE")) % 3 : -1;
    if (test >= 0) return test;
    return netplay_in_match() ? 2 : settings_video.draw_distance % 3;
}
void vp_win_reset(void);
void vp_battle_round(void)
{
    vp_win_reset(); /* (the winner's moment: the match's last round only) */
    render_stats_round();
    interp_snap(); /* a round's start: nothing to blend from */
    void interp_round_start(void);
    interp_round_start(); /* (the animators' random stream: from the same seed each round, in every build) */
    vp_lives_round();
    dm_round_start();
    hud_round();
    pickups_round();
    /* the draw distance (Options > Video): the far value D_80180DAC and its working copy
     * D_80180DA8 (decomp DRAW_DISTANCE.md; docs/RENDER_PLAN.md 2), 4800 in retail. Above
     * about 8000 the GTE's depth saturates (polygons sort and project wrongly). */
    static const uint32_t far_values[3] = {4800, 6000, 8000};
    uint32_t far = far_values[vp_draw_distance()];
    /* with the decomp's far distance (request 31, FAR_DISTANCE.md: TM2_PORT_FAR_DISTANCE): past the GTE's 8,191 through
     * its range pass, so Far 16,000 and Farthest 32,000 (Antarctica's far side from the other; the user's pop-in), the
     * game's own far value left at retail's (the menus' models use it) */
    static uint32_t far_word = 1;
    if (far_word == 1) far_word = port_sym("portFarDistance");
    if (far_word) {
        static const uint32_t port_far[3] = {0, 16000, 32000};
        uint32_t pf = getenv("TM2PC_NO_FAR_DISTANCE") ? 0 : port_far[vp_draw_distance()];
        mem_w32(&M.cpu, far_word, pf);
        if (pf) far = 4800;
        /* the rear-view mirror as far (request 46-9: retail's 500 for the level, 1,000 for the objects: the pop-in in it,
         * the road's far pieces missing): at Original the view's own distance, 4,800 */
        static uint32_t mirror_word = 1;
        if (mirror_word == 1) mirror_word = port_sym("portMirrorFar");
        uint32_t mf = pf ? pf : 4800;
        if (getenv("TM2PC_MIRROR_FAR")) mf = (uint32_t)atoi(getenv("TM2PC_MIRROR_FAR")); /* tests */
        if (mirror_word) mem_w32(&M.cpu, mirror_word, mf);
    }
    /* the sharp near bands kept farther (request 46-6, FAR_LOD.md 8: Hong Kong's buildings blurry until close): Far 2x,
     * Farthest 4x their distance (tests: TM2PC_NEAR_SCALE=N) */
    static uint32_t near_word = 1;
    if (near_word == 1) near_word = port_sym("portFarLodNearScale");
    if (near_word) {
        static const uint32_t scales[3] = {0, 2, 4};
        const char *e = getenv("TM2PC_NEAR_SCALE");
        /* (the same for every player count: one path, the user's) */
        mem_w32(&M.cpu, near_word, e ? (uint32_t)atoi(e) : scales[vp_draw_distance()]);
    }
    /* Farthest: each view's fine bands out to 16x as far as its own last frame's mesh queue and packet buffer allow
     * (request 98, FAR_LOD.md 14: Holland's fence at its finest; the floor stays the scale above; tests:
     * TM2PC_FAR_BUDGET=N, 0 off) */
    static uint32_t budget_word = 1;
    if (budget_word == 1) budget_word = port_sym("portFarLodBudget");
    if (budget_word) {
        const char *e = getenv("TM2PC_FAR_BUDGET");
        mem_w32(&M.cpu, budget_word, e ? (uint32_t)atoi(e) : vp_draw_distance() == 2 ? 16u : 0u);
    }
    mem_w32(&M.cpu, GA(0x80180DACu), far);
    mem_w32(&M.cpu, GA(0x80180DA8u), far);
    if (getenv("TM2PC_FAR")) { /* tests: any far value */
        far = (uint32_t)atoi(getenv("TM2PC_FAR"));
        mem_w32(&M.cpu, GA(0x80180DACu), far);
        mem_w32(&M.cpu, GA(0x80180DA8u), far);
    }
    if (M.trace || getenv("TM2PC_FAR")) fprintf(stderr, "draw distance: %u\n", far);
    if (getenv("TM2PC_ARENA_EXTENT")) { /* tests: the arena's size, from its route points (aiRouteNodes, x y z s32) */
        int n = (int)mem_r32(&M.cpu, GA(0x80180AD8u));
        int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
        for (int i = 0; i < n && i < 1000; i++)
            for (int a = 0; a < 2; a++) {
                int32_t v = (int32_t)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i + 4u * (uint32_t)a);
                if (v < lo[a]) lo[a] = v;
                if (v > hi[a]) hi[a] = v;
            }
        if (!strcmp(getenv("TM2PC_ARENA_EXTENT"), "all")) /* (=all: every point, to find a place to test) */
            for (int i = 0; i < n && i < 1000; i++)
                fprintf(stderr, "route point %d: %d %d %d\n", i, (int)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i),
                        (int)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i + 4), (int)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i + 8));
        if (getenv("TM2PC_TEST_HEIGHTMAP")) { /* tests: the ground's height on a grid (worldGroundHeightAt from high up) */
            int step = atoi(getenv("TM2PC_TEST_HEIGHTMAP"));
            if (step < 50) step = 50;
            const uint32_t pos = 0x80CA0300u; /* (world_marks.c's RAM, unused here) */
            for (int y = lo[1] - 1000; y <= hi[1] + 1000; y += step) {
                char line[4096];
                int at = snprintf(line, sizeof line, "height row %d:", y);
                for (int x = lo[0] - 1000; x <= hi[0] + 1000 && at < (int)sizeof line - 8; x += step) {
                    mem_w32(&M.cpu, pos, (uint32_t)x), mem_w32(&M.cpu, pos + 4, (uint32_t)y), mem_w32(&M.cpu, pos + 8, 12000);
                    at += snprintf(line + at, sizeof line - (size_t)at, " %d", (int)guest_call(GA(0x800EF340u), pos, 0, 0, 0));
                }
                fprintf(stderr, "%s\n", line);
            }
        }
        double dx = (double)hi[0] - lo[0], dy = (double)hi[1] - lo[1];
        fprintf(stderr, "arena extent: level %d, %d route points, x %d..%d, y %d..%d, diagonal %.0f\n",
                mem_r8(&M.cpu, GA(0x8016475Cu) + 24), n, lo[0], hi[0], lo[1], hi[1], sqrt(dx * dx + dy * dy));
    }
    const GameType *t = battle_rules();
    if (!t) return;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) mem_w32(&M.cpu, HUMAN_REC(i) + 0x7C8, (uint32_t)(t->infinite != 0)); /* (players 3-4 too) */
}

void F_801046FC_orig(CPU *c);
void vp_EnergyAttack(CPU *c) /* weaponFireEnergyAttack(car, human) */
{
    const GameType *t = battle_rules();
    if (t) {
        uint32_t car = c->r[4];
        int kind = (int8_t)mem_r8(c, car + (c->r[5] & 0xFF ? 0x7C8u : 0x808u) + 0x0C);
        static const int bit[7] = {ENERGY_FREEZE, ENERGY_MINES, ENERGY_NAPALM, ENERGY_SHIELD, -1, ENERGY_INVISIBILITY,
                                   ENERGY_VOLLEY};
        /* only a request refused (ctl->fire_special: a human's +0xD0 + 0x1C, a computer car's +0x44 + 0x1C), as the game
         * checks it first: the selected kind stays after its press, so the refusal (and its sound) came every step while
         * the kind was one the game type has off, on and on until another attack replaced it (shuriken8147's Minion
         * volley, 2026-11-24); the request then taken, as the game's own refusals do */
        uint32_t req = car + (c->r[5] & 0xFF ? 0xECu : 0x60u);
        /* a Custom Tournament: every energy attack, as the game's own tournament (the user's 2026-11-25); online still never
         * the volley or invisibility */
        int ui_custom_tour_on(void), netplay_in_match(void);
        unsigned allowed = ui_custom_tour_on() ? ~(netplay_in_match() ? BATTLE_ONLINE_NEVER_ENERGY : 0u) : t->energy;
        if (kind >= 100 && kind <= 106 && bit[kind - 100] >= 0 && !((allowed >> bit[kind - 100]) & 1) && mem_r8(c, req)) {
            if (c->r[5] & 0xFF) mem_w8(c, GA(0x80180A04u), 1); /* the "can't" sound, as the game's */
            mem_w8(c, req, 0);
            static int dbg = -1; /* (tests: TM2PC_MOVE_DEBUG, each refusal) */
            if (dbg < 0) dbg = getenv("TM2PC_MOVE_DEBUG") != NULL;
            if (dbg) fprintf(stderr, "controls: energy attack %d refused (off in the game type) at VBlank %llu\n", kind,
                             (unsigned long long)M.vblanks);
            return;
        }
    }
    uint32_t car = c->r[4];
    int human = (c->r[5] & 0xFF) != 0, asked = human && mem_r8(c, car + 0xEC), rear = mem_r8(c, car + 0x7CD);
    F_801046FC_orig(c);
    static int dbg = -1; /* (tests: TM2PC_MOVE_DEBUG, an energy attack fired behind: the rear fire flag taken by the launch) */
    if (dbg < 0) dbg = getenv("TM2PC_MOVE_DEBUG") != NULL;
    if (dbg && asked && rear && !mem_r8(c, car + 0xEC))
        fprintf(stderr, "controls: energy attack %d with the rear fire flag: %s\n", (int8_t)mem_r8(c, car + 0x7D4),
                mem_r8(c, car + 0x7CD) ? "not taken" : "fired behind");
    /* a freeze asked for and now done or refused: the rear fire flag it may have carried (the rear freeze, above) not left
     * for the next weapon (a launch clears it itself; a refusal, too little energy, did not) */
    if (asked && !mem_r8(c, car + 0xEC) && (int8_t)mem_r8(c, car + 0x7D4) == 100) mem_w8(c, car + 0x7CD, 0);
}

/* A special from a pickup: the game takes the pickup away (as an id it does
 * nothing with), then the car holds one more special, up to its most. */
int vp_pickup_special(uint32_t car, int human)
{
    uint32_t weap = car + (human ? 0x7C8u : 0x808u);
    const GameType *t = battle_rules();
    int held = (int8_t)mem_r8(&M.cpu, weap + 0x0E), most = (int8_t)mem_r8(&M.cpu, weap + 0x0B);
    held += t ? t->special_ammo : 1;
    mem_w8(&M.cpu, weap + 0x0E, (uint8_t)(held < most ? held : most));
    return PICKUP_NOTHING;
}

/* Each car's weapon block as it is set up (weaponBlockInit 800FDCAC; decomp
 * CAR_STATS.md 3): specials off (none held, none recharged) or recharged
 * faster or slower (+0x2C, frames per special). */
void F_800FDCAC_orig(CPU *c);
void vp_WeaponBlockInit(CPU *c) /* weaponBlockInit(record, weapons, human) */
{
    uint32_t weap = c->r[5];
    F_800FDCAC_orig(c);
    const GameType *t = battle_rules();
    if (!t) return;
    /* the start's 2 fire missiles and napalm (ammo[1], ammo[5]) only if the game type has them on (the user's 2026-10-29);
     * Gun Game never: the level's weapon only. Held (+0x22) counts them */
    static const int start_w[2] = {1, 3}; /* (BATTLE_WEAPONS indexes: fire, napalm) */
    for (int i = 0; i < 2; i++) {
        if (((t->weapons >> start_w[i]) & 1) && !(t->mode == MODE_GUN_GAME && !ui_survival())) continue;
        uint32_t a = weap + 0x0E + 2u * (uint32_t)WEAPON_ID[start_w[i]];
        int16_t n = (int16_t)mem_r16(c, a), held = (int16_t)mem_r16(c, weap + 0x22);
        if (n <= 0) continue;
        mem_w16(c, a, 0);
        mem_w16(c, weap + 0x22, (uint16_t)(held > n ? held - n : 0));
    }
    if (!t->specials) {
        mem_w8(c, weap + 0x0E, 0);
        mem_w8(c, weap + 0x0B, 0);
        return;
    }
    int k = t->special_recharge % SPECIAL_COUNT;
    if (t->roulette && !t->roulette_add) k = SPECIAL_NONE; /* the roulette replacing: its weapon only (the user's 2026-10-29) */
    if (k == SPECIAL_NONE) { /* none held, none recharged: specials come from pickups only */
        mem_w8(c, weap + 0x0E, 0);
        mem_w16(c, weap + 0x2C, 0x7FFF);
        return;
    }
    static const int num[SPECIAL_COUNT] = {1, 2, 1, 1, 1, 4, 1}, den[SPECIAL_COUNT] = {1, 1, 1, 2, 4, 1, 3}; /* the period: x4 .. /4 */
    int period = (int16_t)mem_r16(c, weap + 0x2C); /* s16: frames per special */
    if (period > 0) mem_w16(c, weap + 0x2C, (uint16_t)(period * num[k] / den[k]));
}

/* ---- timed rules: auto-refill and roulette (each frame of a battle) ----
 * The weapon block (human +0x7C8, AI +0x808; decomp cars.h TM2CarWeapons):
 * ammo[kind] s16 at +0x0E (kind = the weapon id, 0 the specials), held at
 * +0x22, capacity at +0x24, selected at +0x0D (8: none). */
static void each_weapon_block(void (*fn)(uint32_t weap, int arg), int arg)
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) fn(HUMAN_REC(i) + 0x7C8u, arg);
    for (int i = 0; i < ais && i < cars_ai_max(); i++) fn(AI_REC(i) + 0x808u, arg);
}
static void give_one(uint32_t weap, int kind)
{
    int16_t held = (int16_t)mem_r16(&M.cpu, weap + 0x22), cap = (int16_t)mem_r16(&M.cpu, weap + 0x24);
    if (held >= cap) return;
    uint32_t a = weap + 0x0E + 2u * (uint32_t)kind;
    mem_w16(&M.cpu, a, (uint16_t)((int16_t)mem_r16(&M.cpu, a) + 1));
    mem_w16(&M.cpu, weap + 0x22, (uint16_t)(held + 1));
    if ((int8_t)mem_r8(&M.cpu, weap + 0x0D) == 8) mem_w8(&M.cpu, weap + 0x0D, (uint8_t)kind);
}
static int roulette_count;
static void give_n(uint32_t weap, int kind) /* the roulette adding: its count of the weapon, as far as the bay holds */
{
    for (int i = 0; i < roulette_count; i++) give_one(weap, kind);
}
static void set_only(uint32_t weap, int kind)
{
    int16_t cap = (int16_t)mem_r16(&M.cpu, weap + 0x24);
    int n = roulette_count < cap ? roulette_count : cap;
    for (int k = 1; k < 8; k++) mem_w16(&M.cpu, weap + 0x0E + 2u * (uint32_t)k, 0);
    mem_w16(&M.cpu, weap + 0x0E + 2u * (uint32_t)kind, (uint16_t)n);
    mem_w16(&M.cpu, weap + 0x22, (uint16_t)n);
    mem_w8(&M.cpu, weap + 0x0D, (uint8_t)kind);
}
void vp_lives_frame(void);
/* the battle's clocks for refills and the roulette (gameplay: kept with the kill cam's machine) */
static uint64_t bf_last;
static uint32_t bf_ticks, bf_refill_at[BATTLE_WEAPONS], bf_roulette_at;
#define last bf_last
#define ticks bf_ticks
#define refill_at bf_refill_at
#define roulette_at bf_roulette_at
/* Each frame of a battle. It runs in the kill cam's replays and photo mode rewind's too (the computer cars'
 * moods and targets, respawns, refills: what plays out depends on them), its state kept with the machine
 * (vp_keep_register); the port's bookkeeping (the match's tally, feats, messages) leaves replays alone where
 * it is kept. */
int vp_spectating_any(void);
static void giveup_frame(void);
void vp_battle_frame(void)
{
    static int steer_trace = -1; /* tests: TM2PC_STEER_TRACE=1, player 1's steering each game frame */
    if (steer_trace < 0) steer_trace = getenv("TM2PC_STEER_TRACE") != NULL;
    if (steer_trace) {
        uint32_t r = HUMAN_REC(0);
        fprintf(stderr, "steer: gv %llu vb %llu pad %04X inc %d dec %d steer %d yawrate %d\n", (unsigned long long)vp_gv(),
                (unsigned long long)M.vblanks, (unsigned)(uint16_t)~M.pad_buttons[0], mem_r8(&M.cpu, r + 0xE0),
                mem_r8(&M.cpu, r + 0xDF), (int16_t)mem_r16(&M.cpu, r + 0x6DC), (int16_t)mem_r16(&M.cpu, r + 0x6DC + 0x14));
    }
    vp_lives_frame();
    render_stats_frame();
    if (battle_rules()) giveup_frame(); /* (a Challenge Match's or Survival's computer cars) */
    /* spectating (one view): the rear-view mirror off, back on with the player's car (the user's). Its flag only
     * (rtRearViewActive): rtToggleRearView also picks the mirror's frame buffer by the frame being made, and here,
     * outside the game's own moment for it, it picked the one being drawn: its order table linked into itself, a
     * freeze after a respawn (the user's 2026-11-01: Juggernaut, Hunted, King of the Hill, with the mirror on) */
    static int mirror_hidden;
    int one_view = !mem_r32(&M.cpu, GA(0x801809E4u)), mirror = mem_r32(&M.cpu, GA(0x801809DCu)) != 0;
    if (one_view && vp_spectating_any() && mirror) { mem_w32(&M.cpu, GA(0x801809DCu), 0); mirror_hidden = 1; }
    else if (mirror_hidden && (!vp_spectating_any() || !one_view)) {
        if (one_view && !mirror) mem_w32(&M.cpu, GA(0x801809DCu), 1);
        mirror_hidden = 0;
    }
    const GameType *t = battle_rules();
    uint64_t now = vp_gv(), d = now - last;
    last = now;
    if (!t) { ticks = roulette_at = 0; roulette_w = -1; memset(refill_at, 0, sizeof refill_at); return; }
    ticks += (uint32_t)(d < 8 ? d : 1); /* VBlanks of play (a pause or a load counts as one) */
    for (int w = 0; w < BATTLE_WEAPONS; w++) {
        if (!t->refill[w]) continue;
        if (!refill_at[w] || refill_at[w] > ticks + 60u * (uint32_t)t->refill[w]) refill_at[w] = ticks + 60u * (uint32_t)t->refill[w];
        if (ticks >= refill_at[w]) {
            each_weapon_block(give_one, WEAPON_ID[w]);
            refill_at[w] = ticks + 60u * (uint32_t)t->refill[w];
        }
    }
    if (t->roulette) {
        if (!roulette_at) roulette_at = ticks + 1;
        if (ticks >= roulette_at) {
            int on[BATTLE_WEAPONS], n = 0;
            for (int k = 0; k < BATTLE_WEAPONS; k++) if ((t->weapons >> k) & 1) on[n++] = k;
            if (n) {
                roulette_count = t->roulette_count;
                roulette_w = on[(int)vp_rand() % n];
                each_weapon_block(t->roulette_add ? give_n : set_only, WEAPON_ID[roulette_w]);
            }
            roulette_at = ticks + 60u * (uint32_t)t->roulette;
        }
    }
}
#undef last
#undef ticks
#undef refill_at
#undef roulette_at

/* ---- falling off the map ----
 * Below the death line a car's health is set to -1 in its tick (humanCarTick
 * 800DC88C, aiCarDrive 800E7EDC; decomp car_tick.c, ai_driver.c). With the
 * game type's Falling off map not Death, the car goes back on the map (a
 * start point: humanCarPlaceAtStart, aiPlaceOnRouteNode) with what health it
 * had, less the penalty (it can die of it). */
static void fall_rescue(CPU *c, uint32_t rec, int human, int32_t before)
{
    const GameType *t = battle_rules();
    uint32_t hp = rec + (human ? 0x1F0u : 0x5A0u), falling = rec + (human ? 0xBBu : 0x8Bu);
    if (!t || t->falloff == FALL_DEATH || before <= 0 || (int32_t)mem_r32(c, hp) != -1 || !mem_r8(c, falling)) return;
    static const int pct[FALL_COUNT] = {100, 10, 25, 50, 75, 0};
    int max = (int16_t)mem_r16(c, rec + (human ? 0x186u : 0x536u));
    int32_t left = before - max * pct[t->falloff % FALL_COUNT] / 100;
    if (left <= 0) { mem_w32(c, hp, (uint32_t)-1); return; } /* the fall finished it */
    guest_call(human ? GA(0x800FD6C4u) : GA(0x800F6518u), rec, 0, 0, 0);
    mem_w8(c, falling, 0);
    mem_w32(c, hp, (uint32_t)left);
    if (M.trace || getenv("TM2PC_FALL_DEBUG")) fprintf(stderr, "vplus: a car fell off: back on the map, health %d\n", left);
}
void F_800DC88C_orig(CPU *c);
void vp_HumanCarTick(CPU *c)
{
    uint32_t rec = c->r[4];
    int32_t before = (int32_t)mem_r32(c, rec + 0x1F0u);
    F_800DC88C_orig(c);
    static uint64_t dbg_next;
    if (getenv("TM2PC_HUMANS_DEBUG") && vp_gv() >= dbg_next) {
        dbg_next = vp_gv() + 300;
        for (int i = 0; i < (int)mem_r32(c, GA(0x80181014u)) && i < 4; i++) {
            uint32_t h = HUMAN_REC(i);
            fprintf(stderr, "humans debug v%llu: %d dead %d hp %d pos %d,%d pad %04x frozen %d timer %d missile %d shield %d ice %d\n",
                    (unsigned long long)vp_gv(), i, mem_r8(c, h + 0x9C), (int)mem_r32(c, h + 0x1F0), (int)mem_r32(c, h + 0x6FC),
                    (int)mem_r32(c, h + 0x700), i < 2 ? M.pad_buttons[i] : 0, mem_r8(c, h + 0xA9), (int16_t)mem_r16(c, h + 0x166),
                    (int16_t)mem_r16(c, h + 0x168), mem_r8(c, h + 0xAA),
                    mem_r8(c, GA(0x8018F950u) + 264u * (mem_r32(c, h + 0x90) >> 4) + 7)); /* (its model's ice step) */
        }
    }
    fall_rescue(c, rec, 1, before);
}
void F_800E7EDC_orig(CPU *c);
void vp_AiCarDrive(CPU *c)
{
    uint32_t rec = c->r[4];
    int32_t before = (int32_t)mem_r32(c, rec + 0x5A0u);
    F_800E7EDC_orig(c);
    fall_rescue(c, rec, 0, before);
}

/* ---- lives without a round restart ----
 * A human's death schedules the round's end (rtScheduleEvent(1, 420), or
 * 3/4 in Deathmatch; decomp COOP_LIVES.md 1.1), and the runner restarts
 * every human car. With a life left the event is not scheduled: after the
 * same delay that car alone comes back (2.2: its per-car resets, the start
 * point farthest from every other car, full health, its object shown, two
 * seconds of shield). In a Challenge Match always (its Lives); in a
 * tournament with Vanilla Plus "No round restarts" (its lives, settings
 * +20: the last one still ends the round, so the runner's GAME OVER comes).
 * The HUD's "Lives: N" shows settings +20 less one. */
static struct { int left; uint64_t at, shield_off; } life[CARS_HUMANS_MAX]; /* left: lives after this one (a Challenge Match's), by human */
static int lives_round;   /* 1 a Challenge Match's lives, 2 the tournament's (settings +20) */

/* A Challenge Match's start (and a rematch's): every player's lives */
static void ai_lives_start(void);
static int human_gone[CARS_HUMANS_MAX]; /* online: the player's machine left the match (its car out until it comes back) */
static int gone_left[CARS_HUMANS_MAX];  /* (their lives left as they went: theirs again when they come back) */
static int handed[CARS_HUMANS_MAX];     /* online: the computer record + 1 that drives the player's car since they left
                                         * (decomp request 50, portHumanHandOver; 0: none) */
static int ai_team_plus1[CARS_MAX];     /* (by computer record) a handed-over car's team + 1: its player's (0: the match's) */
static int dropped_bot[CARS_MAX];      /* online: computer records a player dropping in took the place of (gone) */
static int back_wait[CARS_HUMANS_MAX];  /* online: the player back while the computer driver's car is down: theirs once it
                                         * comes back (vp_ai_frame) */
void vp_lives_match_start(void)
{
    if (!battle_rules()) return;
    ai_lives_start();
    int ui_tour_lives(int p); /* ui/options.c: a custom tournament's lives for this stage (0: not one) */
    for (int i = 0; i < CARS_HUMANS_MAX; i++) life[i].left = (ui_tour_lives(i) > 0 ? ui_tour_lives(i) : battle_lives()) - 1;
    memset(human_gone, 0, sizeof human_gone);
    memset(dropped_bot, 0, sizeof dropped_bot); /* (a new match: every computer car its own again) */
}

/* A Challenge Match without "No round restarts": a lost life restarts the
 * round (as the game's tournament); 1 if player p had one to lose. */
int vp_lives_left(int p) { return p >= 0 && p < CARS_HUMANS_MAX ? life[p].left + 1 : 0; } /* the lives a player has, this one too */
int vp_lives_take(int p)
{
    if (p < 0 || p > 1 || life[p].left <= 0) return 0;
    life[p].left--;
    fprintf(stderr, "lives: player %d lost a life (the round restarts)\n", p + 1);
    return 1;
}

/* ---- spectating (a Challenge Match's player out, two or more computer cars left) ----
 * The round goes on: the player's camera follows a computer car (view 7,
 * as the attract demo: viewWatchedAiCar, carSetViewMode; decomp
 * COOP_LIVES.md 6.1), the next one when it is out or on X; one car left
 * ends the round (rtScheduleEvent(1, ...)). */
#define AI_REMAINING GA(0x80181020u)
#define VIEW_WATCHED_AI GA(0x80180FACu)
static int spectating; /* the only player out, watching: the rules' flag (the computer cars on each other, the round's end
                        * at one car left); what they see is their view's watching (wv[0]), as every player's */
/* A player out watches the drivers still in, each view its own (wv by view: this PC's screen L, its human
 * humans_of_slot(L); offline the same numbers; online a player's view whatever their number in the match): a computer
 * car (ai) or another player (human), with the chase camera (view 7, the D-pad's turn: orbit), cinematic (the kill
 * cam's shots on the car and its target, killcam_camera) or the free camera (photo mode's: one view only), Triangle the
 * HUD and the banner (hide). One spectate for every player count (the user's 2026-11-07: "why make it different?"). */
static struct { int on, ai, human, cinema, free, orbit, hide; uint16_t prev; uint64_t switch_at; } wv[4] = {{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
static int one_view(void) { return !mem_r32(&M.cpu, GA(0x801809E4u)); } /* (this PC's screen one view: the free camera, the flights) */
int vp_spectating_any(void) { return wv[0].on || wv[1].on || wv[2].on || wv[3].on; }
static int watched_pos(int p, int32_t *x, int32_t *y, int32_t *z, int *heading) /* view p's watched car: where, its heading */
{
    uint32_t r, o;
    if (wv[p].human >= 0 && wv[p].human < CARS_HUMANS_MAX) r = HUMAN_REC(wv[p].human), o = 0x6FC;
    else if (wv[p].ai >= 0 && wv[p].ai < cars_ai_max()) r = AI_REC(wv[p].ai), o = 0x73C;
    else return 0;
    *x = (int32_t)mem_r32(&M.cpu, r + o), *y = (int32_t)mem_r32(&M.cpu, r + o + 4);
    if (z) *z = (int32_t)mem_r32(&M.cpu, r + o + 8);
    if (heading) *heading = (int)(int16_t)mem_r16(&M.cpu, r + o + 0x14);
    return 1;
}
/* viewSetFollowObjPos(pos, view) (801563BC, worldUpdate each frame: view 1's sky placed at the player's car): while the
 * player spectates, at the car watched, or the free camera, as the attract demo places it at its watched car; at the
 * wreck the camera left the sky's dome far away and saw gray (the user's on Cyburbia, spectating) */
void photo_camera_xy(int32_t *px, int32_t *py); /* ui/photo.c */
void F_801563BC_orig(CPU *c);
void vp_SetFollowObjPos(CPU *c)
{
    int view = (int)c->r[5];
    if (view >= 0 && view < 4 && wv[view].on) {
        int32_t sx, sy;
        if (wv[view].free) photo_camera_xy(&sx, &sy);
        else if (!watched_pos(view, &sx, &sy, NULL, NULL)) { F_801563BC_orig(c); return; }
        mem_w32(c, c->r[4], (uint32_t)sx); /* (the caller's own target: written for this call) */
        mem_w32(c, c->r[4] + 4, (uint32_t)sy);
    }
    F_801563BC_orig(c);
}
/* one view watching: its banner across the screen (1 chase, 2 cinematic, 3 free); split screen: vp_watching's, each view's */
int vp_spectating(void) { return one_view() && wv[0].on && wv[0].hide < 2 ? (wv[0].free ? 3 : 1 + wv[0].cinema) : 0; }
int vp_freecam(void) { return one_view() && wv[0].on && wv[0].free && !killcam_replaying; }
/* killcam.c's spectator camera flight (one view): the car watched with the chase camera (a number of its own), 0 otherwise */
int vp_spectate_chase_target(void)
{
    if (!one_view() || !wv[0].on || wv[0].cinema || wv[0].free || killcam_replaying) return 0;
    if (wv[0].human >= 0) return 1000 + wv[0].human;
    return wv[0].ai >= 0 && wv[0].ai < cars_ai_max() ? wv[0].ai + 1 : 0;
}
static int ai_alive(int k) { return k < (int)mem_r32(&M.cpu, GA(0x8018101Cu)) && !mem_r8(&M.cpu, AI_REC(k) + 0x6C); }

/* ---- teams (a Challenge Match's Last Car Standing with 2-4 teams, the Match page) ----
 * Computer car k is opponent k (the match's team[k]); the players have
 * player_team[]. The computer cars hunt the nearest car of another team
 * (aiCarTargets, -1 the nearest human: so only when that is an enemy);
 * without friendly fire teammates' hits do nothing (feat_Damage); the
 * enemies count only the other teams' cars, so the round is won when they
 * are all out, by the player or their teammates (also while spectating);
 * it is lost when the player is out and no teammate is left. 1 player. */
int vp_duo(void);
int vp_versus_on(void);
static int duo_team_won(void);
static void duo_round_start(void);
int vp_port_event; /* the next rtScheduleEvent is the port's own decision: let it through */
static int teams_on(void)
{
    const GameType *t = battle_rules();
    uint32_t humans = mem_r32(&M.cpu, GA(0x80181014u));
    return t && !ui_survival() && battle_match_teams() >= 2 && (humans == 1 || (humans >= 2 && humans <= CARS_HUMANS_MAX && vp_duo()));
}
static int team_of_ai(int k)
{
    if (k >= 0 && k < CARS_MAX && ai_team_plus1[k]) return ai_team_plus1[k] - 1; /* (a player's car, handed over: their team) */
    return battle_match()->team[(unsigned)k % BATTLE_OPPONENT_SLOTS] % BATTLE_TEAM_COLORS;
}
static int my_team(void) { return battle_match()->player_team[0] % BATTLE_TEAM_COLORS; }
static int team_of_human(int i) { return battle_match()->player_team[i & 15] % BATTLE_TEAM_COLORS; }
/* humanCarNearest(pos, exclude): the game's "nearest player" for a computer car (its target -1: aiGetTargetCar, the
 * chase, the specials). With teams: the nearest player of another team than the car whose position pos is (its
 * record's or its object's), alive; none: the game's answer. */
void F_8011C8E4_orig(CPU *c);
void vp_HumanCarNearest(CPU *c)
{
    uint32_t pos = c->r[4];
    int exclude = (int)c->r[5], mine = -1;
    if (teams_on()) {
        int ais = (int)mem_r32(c, GA(0x8018101Cu)), humans = (int)mem_r32(c, GA(0x80181014u));
        for (int k = 0; k < ais && k < cars_ai_max() && mine < 0; k++) {
            uint32_t obj = mem_r32(c, GA(0x80188B38u) + 4u * (uint32_t)k);
            if (pos == AI_REC(k) + 0x73C || (obj && pos == obj + 0xA8)) mine = team_of_ai(k);
        }
        for (int i = 0; i < humans && i < CARS_HUMANS_MAX && mine < 0; i++)
            if (pos == HUMAN_REC(i) + 0x6FC) mine = team_of_human(i);
        if (mine >= 0) {
            /* an enemy player alive first; else any enemy player (a wreck, one out: harmless, never a teammate: the
             * game's answer, the nearest player of any team, sent teammates' missiles at the player while every enemy
             * computer car was down, KinggzZ's 2026-11-23 report) */
            for (int any = 0; any < 2; any++) {
                int best = -1;
                int32_t bd = 0;
                for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) {
                    if (i == exclude || team_of_human(i) == mine) continue;
                    if (!any && (mem_r8(c, HUMAN_REC(i) + 0x9C) || (int32_t)mem_r32(c, HUMAN_REC(i) + 0x1F0) <= 0)) continue;
                    int32_t dx = (int32_t)mem_r32(c, pos) - (int32_t)mem_r32(c, HUMAN_REC(i) + 0x6FC);
                    int32_t dy = (int32_t)mem_r32(c, pos + 4) - (int32_t)mem_r32(c, HUMAN_REC(i) + 0x700);
                    int32_t d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                    if (best < 0 || d < bd) { best = i; bd = d; }
                }
                if (best >= 0) { c->r[2] = (uint32_t)best; return; }
            }
        }
    }
    F_8011C8E4_orig(c);
}
int vp_teams_for_config(uint8_t human[16], uint8_t ai[16]) /* port_config.c: the teams for the variant's C */
{
    if (!teams_on()) return 0;
    for (int i = 0; i < 16; i++) human[i] = (uint8_t)team_of_human(i);
    for (int k = 0; k < 16; k++) ai[k] = (uint8_t)team_of_ai(k);
    return 1;
}
int vp_team_of_human(int i) { return teams_on() && i >= 0 && i < CARS_HUMANS_MAX ? team_of_human(i) : -1; } /* -1: teams off */
int vp_team_of_slot(int slot) /* a car's team by its object slot, -1 none (teams off) */
{
    if (!teams_on() || slot < 0) return -1;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++)
        if ((int16_t)mem_r16(&M.cpu, HUMAN_REC(i) + 4) == slot) return team_of_human(i);
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < n && k < cars_ai_max(); k++)
        if ((int16_t)mem_r16(&M.cpu, AI_REC(k) + 6) == slot) return team_of_ai(k);
    return -1;
}
/* feat_Damage: 1 when this hit is a teammate's and friendly fire is off (victim key: 0-1 humans, 2+ computer cars) */
int vp_team_blocks_damage(int victim_key, int attacker_slot)
{
    if (attacker_slot < 0 || !teams_on()) return 0; /* (Juggernaut: the others only don't aim at each other, team_aim;
                                                    * rams, guns and stray missiles still hurt: the user's 2026-10-29) */
    int victim_slot = victim_key < CARS_HUMANS_MAX ? (int16_t)mem_r16(&M.cpu, HUMAN_REC(victim_key) + 4) /* keys: 0-15 players, 16+ computer cars */
                                     : (int16_t)mem_r16(&M.cpu, AI_REC(victim_key - CARS_HUMANS_MAX) + 6);
    if (victim_slot == attacker_slot) return 0; /* your own weapon hurts you */
    int a = vp_team_of_slot(attacker_slot), v = vp_team_of_slot(victim_slot);
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("TM2PC_TEAM_DEBUG") != NULL;
    if (dbg && a >= 0 && a == v) fprintf(stderr, "team hit: slot %d hit teammate slot %d (team %d)\n", attacker_slot, victim_slot, a);
    return battle_match()->no_friendly_fire && a >= 0 && a == v; /* (Teams > Friendly fire) */
}
static int teammates_alive(void)
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), c = 0;
    for (int k = 0; k < n && k < cars_ai_max(); k++) c += ai_alive(k) && team_of_ai(k) == my_team();
    return c;
}
static int ai_life_pending(int k);
static int enemies_left(void) /* the other teams' cars alive or coming back */
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), c = 0;
    for (int k = 0; k < n && k < cars_ai_max(); k++)
        if (team_of_ai(k) != my_team()) c += ai_alive(k) || ai_life_pending(k);
    return c;
}
int vp_dm_team_winner(void);
int vp_teams_winner(void) /* the team with cars left when the round ended (-1: teams off or none) */
{
    if (!teams_on()) return -1;
    if (vp_dm()) return vp_dm_team_winner(); /* the modes with respawns: the most points (kills in Deathmatch) */
    if (vp_duo()) return duo_team_won();
    if (!enemies_left()) return my_team();
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < n && k < cars_ai_max(); k++)
        if (ai_alive(k) && team_of_ai(k) != my_team()) return team_of_ai(k);
    return -1;
}
/* ---- giving up an unreachable target (decomp PLAYTEST_ANSWERS.md 16.3) ----
 * A computer car whose target stands off its routes walks a loop around the nearest route point, turning 180 degrees
 * again and again (the user's on Cyburbia). Per car: the target it was given, the nearest it got, the route points it
 * passed (its route node, +0x464) since then. Past 8 points without getting 10% nearer or hurting the target, the
 * target is given up for 10 seconds: the port's target picks pass it over (unless it comes within 1,500). Deterministic
 * (positions, HP, counters); kept with the kill cam's machine. */
enum { GU_POINTS = 8, GU_VBLANKS = 600, GU_NEAR = 1500 };
static int32_t gu_target[CARS_MAX], gu_node[CARS_MAX], gu_points[CARS_MAX], gu_hp[CARS_MAX], gu_banned[CARS_MAX];
static double gu_best[CARS_MAX];
static uint64_t gu_until[CARS_MAX];
static int gu_target_pos(int tg, double *x, double *y, int *hp) /* tg: a computer car, or -1 the nearest player */
{
    if (tg >= 0) {
        if (tg >= cars_ai_max()) return 0;
        *x = (int32_t)mem_r32(&M.cpu, AI_REC(tg) + 0x73C);
        *y = (int32_t)mem_r32(&M.cpu, AI_REC(tg) + 0x740);
        *hp = (int16_t)mem_r16(&M.cpu, AI_REC(tg) + 0x5A0);
        return 1;
    }
    *x = (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x6FC);
    *y = (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x700);
    *hp = (int16_t)mem_r16(&M.cpu, HUMAN_REC(0) + 0x1F0);
    return 1;
}
static void giveup_frame(void)
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < n && k < cars_ai_max() && k < CARS_MAX; k++) {
        if (!ai_alive(k)) { gu_target[k] = -2; continue; }
        int tg = (int32_t)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k);
        double tx, ty, kx = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x73C), ky = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x740);
        int hp, node = (int16_t)mem_r16(&M.cpu, AI_REC(k) + 0x464);
        if (!gu_target_pos(tg, &tx, &ty, &hp)) continue;
        double dist = sqrt((tx - kx) * (tx - kx) + (ty - ky) * (ty - ky));
        if (tg != gu_target[k]) { gu_target[k] = tg; gu_best[k] = dist; gu_points[k] = 0; gu_node[k] = node; gu_hp[k] = hp; continue; }
        if (dist < gu_best[k] * 0.9 || hp < gu_hp[k]) { gu_best[k] = dist < gu_best[k] ? dist : gu_best[k]; gu_points[k] = 0; }
        gu_hp[k] = hp;
        if (node != gu_node[k]) { gu_node[k] = node; gu_points[k]++; }
        if (gu_points[k] >= GU_POINTS) {
            gu_banned[k] = tg;
            gu_until[k] = vp_gv() + GU_VBLANKS;
            gu_target[k] = -2; /* (the next target counted anew) */
            if (getenv("TM2PC_AI_TRACE")) fprintf(stderr, "ai %d: gave up target %d (passed %d route points, no nearer)\n", k, tg, GU_POINTS);
            equal_next = spect_retarget = team_next = 0; /* (a new one at once) */
        }
    }
}
static int gu_skip(int k, int j, double dist2) /* car k may not take j now (j -1: the players) */
{
    return k >= 0 && k < CARS_MAX && gu_until[k] > vp_gv() && gu_banned[k] == j && dist2 > (double)GU_NEAR * GU_NEAR;
}
static int enemy_player_alive(int k) /* a player of another team than computer car k's, alive */
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++)
        if (team_of_human(i) != team_of_ai(k) && !mem_r8(&M.cpu, HUMAN_REC(i) + 0x9C) && (int32_t)mem_r32(&M.cpu, HUMAN_REC(i) + 0x1F0) > 0)
            return 1;
    return 0;
}
/* a computer car whose target is no good now (gone, a teammate, or the nearest player with no enemy player alive): the
 * targets again at once, not at the next second (the game sets -1 itself when a target dies) */
static int teams_targets_bad(void)
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < n && k < cars_ai_max(); k++) {
        if (!ai_alive(k)) continue;
        int t = (int32_t)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k);
        if (t == -1 ? !enemy_player_alive(k) : t < 0 || t >= n || t == k || team_of_ai(t) == team_of_ai(k)) return 1;
        if (t >= 0 && !ai_alive(t)) {
            for (int j = 0; j < n && j < cars_ai_max(); j++) /* (dead, with an enemy alive to take instead) */
                if (j != k && ai_alive(j) && team_of_ai(j) != team_of_ai(k)) return 1;
            if (enemy_player_alive(k)) return 1;
        }
    }
    return 0;
}
static void teams_retarget(void) /* each computer car: the nearest car of another team */
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < n && k < cars_ai_max(); k++) {
        if (!ai_alive(k)) continue;
        mem_w8(&M.cpu, AI_REC(k) + 0x034, 1); /* hurt by any car, as when spectating */
        double kx = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x73C), ky = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x740), best_d = 1e30;
        int best = -1;
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), human = -1;
        for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) { /* a player of another team (aiCarTargets -1: the nearest player) */
            if (mem_r8(&M.cpu, HUMAN_REC(i) + 0x9C) || team_of_human(i) == team_of_ai(k)) continue;
            double dx = (int32_t)mem_r32(&M.cpu, HUMAN_REC(i) + 0x6FC) - kx, dy = (int32_t)mem_r32(&M.cpu, HUMAN_REC(i) + 0x700) - ky;
            if (gu_skip(k, -1, dx * dx + dy * dy)) continue;
            if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; human = i; }
        }
        /* (a computer car of another team; with no player of another team, one given up on too: -1 would be "the
         * nearest player", a teammate then, the user's 2026-10-29) */
        /* (pass 2: none alive, every enemy computer car down (respawning): one of them all the same, its record; -1
         * would be the nearest player, a teammate when no enemy player is in: KinggzZ's 2026-11-23 report) */
        for (int pass = 0; pass < 3 && best < 0 && (pass == 0 || human < 0); pass++)
            for (int j = 0; j < n && j < cars_ai_max(); j++) {
                if (j == k || (pass < 2 && !ai_alive(j)) || team_of_ai(j) == team_of_ai(k)) continue;
                double dx = (int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x73C) - kx, dy = (int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x740) - ky;
                if (!pass && gu_skip(k, j, dx * dx + dy * dy)) continue;
                if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; best = j; }
            }
        static int dbg = -1; /* tests: TM2PC_TARGET_DEBUG, each computer car's new target */
        if (dbg < 0) dbg = getenv("TM2PC_TARGET_DEBUG") != NULL;
        if (dbg && (int32_t)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k) != best)
            fprintf(stderr, "teams: computer car %d's target %d (frame %d)\n", k + 1, best, (int)(vp_gv() / 3));
        mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k, (uint32_t)best);
    }
}
void port_config_sync(void); /* vplus/port_config.c: the game's C sees a change of the port's settings at once */
static int spectate_possible(void) { return teams_on() ? teammates_alive() : (int)mem_r32(&M.cpu, AI_REMAINING) >= 2; }
static void watch_begin(int h);
static int spectate_begin(void)
{
    if (!spectate_possible()) return 0; /* teams: a teammate to watch */
    spectating = 1;
    port_config_sync(); /* (AI vs AI from now: the game's C sees it this frame, as the host's wraps do) */
    watch_begin(0); /* (the view: as every player's) */
    fprintf(stderr, "lives: player 1 is out: spectating (vblank %llu)\n", (unsigned long long)vp_gv());
    return 1;
}
/* killcam.c: view `view` watching with cinematic: the car watched and the one it is after (a computer car's target, else
 * the nearest other driver still in) */
int vp_player_out(int p);
int vp_cinema_watch(int view, uint32_t *watched, uint32_t *other)
{
    if (view < 0 || view > 3 || !wv[view].on || !wv[view].cinema || wv[view].free || killcam_replaying) return 0;
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), k = wv[view].ai, h = wv[view].human;
    int32_t x, y;
    *watched = *other = 0;
    if (!watched_pos(view, &x, &y, NULL, NULL)) return 0;
    if (h >= 0) *watched = guest_call(GA(0x8011CB50u), (uint32_t)h, 0, 0, 0); /* humanCarObject */
    else {
        if (k >= n) return 0;
        *watched = mem_r32(&M.cpu, GA(0x80188B38u) + 4u * (uint32_t)k);
        int t = (int)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k);
        if (t >= 0 && t < n && t < cars_ai_max() && t != k && ai_alive(t)) {
            *other = mem_r32(&M.cpu, GA(0x80188B38u) + 4u * (uint32_t)t);
            return *watched != 0;
        }
    }
    double best = 1e30;
    for (int o = 0; o < humans && o < CARS_HUMANS_MAX; o++) {
        if (o == h || vp_player_out(o) || mem_r8(&M.cpu, HUMAN_REC(o) + 0x9C)) continue;
        double dx = (int32_t)mem_r32(&M.cpu, HUMAN_REC(o) + 0x6FC) - (double)x, dy = (int32_t)mem_r32(&M.cpu, HUMAN_REC(o) + 0x700) - (double)y;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; *other = guest_call(GA(0x8011CB50u), (uint32_t)o, 0, 0, 0); }
    }
    for (int j = 0; j < n && j < cars_ai_max(); j++) {
        if ((h < 0 && j == k) || !ai_alive(j)) continue;
        double dx = (int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x73C) - (double)x, dy = (int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x740) - (double)y;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; *other = mem_r32(&M.cpu, GA(0x80188B38u) + 4u * (uint32_t)j); }
    }
    return *watched != 0;
}
static void spectate_frame(void)
{
    if (!spectating) return;
    /* one car (team) left: the end. Teams: the player's team out, or every other team (the game's own won test needs the
     * player alive: with the player watching, the teammates' win went on forever, the user's 2026-11-18 take) */
    if (teams_on() ? !teammates_alive() || !enemies_left() : (int)mem_r32(&M.cpu, AI_REMAINING) <= 1) {
        spectating = 0;
        port_config_sync();
        vp_port_event = 1;
        guest_call(GA(0x800D181Cu), 1, 210, 0, 0);
        return;
    }
    if (!teams_on() && vp_gv() >= spect_retarget) { /* the computer cars hunt each other (aiCarTargets, as in the attract demo) */
        spect_retarget = vp_gv() + 60;
        int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
        for (int k = 0; k < n && k < cars_ai_max(); k++) {
            if (!ai_alive(k)) continue;
            int best = -1;
            double best_d = 1e30;
            for (int j = 0; j < n && j < cars_ai_max(); j++) {
                if (j == k || !ai_alive(j)) continue;
                double dx = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x73C) - (double)(int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x73C);
                double dy = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x740) - (double)(int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x740);
                if (gu_skip(k, j, dx * dx + dy * dy)) continue;
                if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; best = j; }
            }
            mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k, (uint32_t)best);
        }
    }
    mem_w32(&M.cpu, VIEW_WATCHED_AI, (uint32_t)wv[0].ai); /* (the game's watched car, as before: the view's own) */
}

void vp_survival_start(void);
/* a player's lives where the game reads them (gameGetLives, the HUD's "Lives: N"): players 1-4 the settings' (+20..+23),
 * the others the decomp's portHumanLives */
static void lives_hud_put(int h)
{
    uint8_t v = (uint8_t)(life[h].left + 1);
    if (h < 4) mem_w8(&M.cpu, GA(0x8016475Cu) + 20 + (uint32_t)h, v);
    else {
        static uint32_t a = 1;
        if (a == 1) a = port_sym("portHumanLives");
        if (a) mem_w8(&M.cpu, a + (uint32_t)h, v);
    }
}
static int coop_out[CARS_HUMANS_MAX]; /* (by human) VP_COOP_SPECTATE: a player out of the tournament's lives, watching (3-4 players: out of a match's) */
int vp_player_out(int p) { return p >= 0 && p < CARS_HUMANS_MAX && coop_out[p]; } /* vplus/four.c: an out player 3-4 watches */
static int showdown_on; /* Final showdown music: switched this round */
static void showdown_reset(void) { showdown_on = 0; music_showdown_play(0); }
void killcam_new_round(void); /* killcam.c */
/* A player down first sees their wreck for the game's own delay (the tournament's, before its restart: the delay
 * of the death's event), then watches the others: out of lives, to the end; with a respawn timer (Deathmatch's),
 * until they come back, "RESPAWNING IN N" counting down. */
static uint64_t watch_at[CARS_HUMANS_MAX]; /* VBlank human i starts watching (0: not waiting) */
static uint64_t spectate_at;   /* 1 player, out: VBlank the spectating starts (0: not waiting) */
static uint32_t death_delay;   /* the death's event delay (VBlanks), for duo_player_out */
static void boss_round_start(void); static int boss_hold(void); static int boss_coming(void); static void boss_frame(void); static void survival_clock_stop(void); /* (below, with Survival's waves) */
static uint64_t test_round_gv;    /* the round's start on the gameplay clock (kept with the network states: a machine that
                                   * took the host's has the host's; tests: TM2PC_TEST_ROUND_KILLS) */
static uint32_t test_round_fired;
static int test_round_n;          /* (and the rounds so far: TM2PC_TEST_ROUND_ROTATE turns the players by it) */
/* the game's frames since the round started, the same on every machine (the kill cam's own count was each machine's:
 * online, a stage's first frames guarded on one and not on another, and the states parted at every Versus stage) */
int vp_round_frames(void)
{
    if (!test_round_gv) return 0x7FFFFFFF;
    uint64_t d = vp_gv() >= test_round_gv ? (vp_gv() - test_round_gv) / 3 : 0;
    return d > 0x7FFFFFFF ? 0x7FFFFFFF : (int)d;
}
static int round_serial; /* (net/game_tm2.c: a round started: a level's boss phase too, which never leaves the battle) */
int vp_round_serial(void) { return round_serial; }
void vp_lives_round(void) /* the round's start */
{
    round_serial++;
    test_round_gv = vp_gv(), test_round_fired = 0, test_round_n++;
    killcam_new_round();
    vp_survival_start();
    boss_round_start();
    if (battle_rules()) /* the computer cars hunt the players again (spectating turned them on each other) */
        for (int k = 0; k < cars_ai_max(); k++) mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k, (uint32_t)-1);
    spectating = 0;
    port_config_sync();
    for (int i = 0; i < CARS_HUMANS_MAX; i++) coop_out[i] = 0;
    if (vp_duo() || vp_versus_on()) /* "Enemies: N" again with every player in (it was made before this, with the last
                                     * round's players out: "Enemies: 3" of 10 until a car died, the user's 2026-11-11) */
        guest_call(GA(0x800FBB6Cu), 0, 0, 0, 0); /* hudMessageReset */
    duo_round_start();
    showdown_reset();
    int mode = mem_r8(&M.cpu, GA(0x8016475Cu));
    lives_round = 0;
    { /* (a load clears the decomp's vacant records: a hand-over of the last load is over) */
        static uint32_t vacant_at = 1;
        if (vacant_at == 1) vacant_at = port_sym("portHumanVacant");
        uint32_t mask = vacant_at ? mem_r32(&M.cpu, vacant_at) : 0;
        for (int h = 0; h < CARS_HUMANS_MAX; h++)
            if (handed[h] && !((mask >> h) & 1)) { ai_team_plus1[(handed[h] - 1) % CARS_MAX] = 0; handed[h] = back_wait[h] = 0; }
        /* online every human takes damage: a hand-over made the leaving player's record vacant with damageable (stats
         * +0xA8, human +0x1F4) 0, and only uaInit (the boot) sets it again, carStatsLoad never for a player's record:
         * whoever had that place next drove unhurt by anything, match after match until a restart (Sage's, Enough
         * Boxes' and Phoenix's reports 2026-11-22). Online the cheats are off, so 0 is never wanted there. */
        int humans_local_count(void); /* vplus/humans.c: -1 offline */
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
        if (humans_local_count() >= 0)
            for (int h = 0; h < humans && h < CARS_HUMANS_MAX; h++)
                if (!((mask >> h) & 1) && !mem_r32(&M.cpu, HUMAN_REC(h) + 0x1F4)) {
                    mem_w32(&M.cpu, HUMAN_REC(h) + 0x1F4, 1);
                    fprintf(stderr, "lives: player %d takes damage again (a hand-over's vacant record)\n", h + 1);
                }
    }
    vp_port_event = 0; /* (none left over from the last round: its next death would end this one) */
    for (int i = 0; i < CARS_HUMANS_MAX; i++) life[i].at = life[i].shield_off = watch_at[i] = 0;
    spectate_at = 0;
    if (battle_rules()) {
        int n = (int)mem_r32(&M.cpu, GA(0x80181014u));
        for (int h = 0; h < n && h < CARS_HUMANS_MAX; h++) lives_hud_put(h);
        lives_round = vp_on(VP_NO_ROUND_RESTARTS) || vp_dm() || vp_duo() ? 1 : 0; /* Deathmatch, 2 players: a respawn */
    } else if ((mode == 1 || mode == 2) && vp_on(VP_NO_ROUND_RESTARTS)) {
        lives_round = 2;
    }
}

int vp_survival_cleared(void);
int vp_survival_player_down(void);
static int coop_player_out(int i, uint32_t delay);

void F_800D181C_orig(CPU *c);
/* The winner's moment (the user's 2026-11-24): the match's last round end waits WIN_HOLD VBlanks more, the winner's name
 * and picture over the cars once the final kill cam is over (ui/options.c win_frame). The replay's end the same wait, so
 * the kill cam's replay schedules what the battle did. */
#define WIN_HOLD 120
int ui_round_ends_match(int value); /* ui/options.c */
static int win_value = -1;
static uint64_t win_end; /* (the round's end, VBlanks: the moment's overlay off 4 before it, so the results' picture of
                          * the last frame is the battle's alone) */
int vp_win_value(void) { return win_value >= 0 && vp_gv() + 4 < win_end ? win_value : -1; }
void vp_win_reset(void) { win_value = -1; }
static void win_hold(CPU *c, int replay)
{
    if (!ui_round_ends_match((int)c->r[4])) return;
    c->r[5] += WIN_HOLD;
    if (!replay) win_value = (int)c->r[4], win_end = vp_gv() + c->r[5];
}
static int duo_player_out(int i);
static uint32_t burn_wait = 180; /* the game's wait before a destroyed player comes back (VBlanks): a respawn's burning */
void vp_ScheduleEvent(CPU *c) /* rtScheduleEvent(value, delay) */
{
    int value = (int)c->r[4], mode = mem_r8(c, GA(0x8016475Cu));
    if (killcam_replaying) { vp_port_event = 0; win_hold(c, 1); F_800D181C_orig(c); return; } /* the kill cam's replay: the battle's end is the
                                                                               * original's (the port's decision replayed: used up) */
    if (vp_port_event) { vp_port_event = 0; killcam_round_end(); win_hold(c, 0); F_800D181C_orig(c); return; }
    int duo = vp_duo();
    if (duo && value == 3) return; /* 2 players and computer cars: the round's end is the port's (vp_duo_frame) */
    if (value == 3 && ui_survival() && vp_survival_cleared()) { killcam_round_end(); return; } /* a wave cleared (its kill cam): the next one */
    if (value == 3 && boss_hold()) { mem_w32(c, GA(0x80180FB0u), 0); /* (ROUND_WON_FLAG) */ killcam_round_end(); return; } /* the enemies out: the boss */
    if (value == 1 && ui_survival() && vp_survival_player_down()) return; /* co-op: one player out, the other plays on */
    if (ui_survival() && (value == 1 || value == 3)) survival_clock_stop(); /* the run is over: its time */
    int death = lives_round && (((mode == 1 || mode == 2 || mode == 3) && value == 1) || (mode == 4 && (value == 3 || value == 4)));
    if (death) {
        int humans = (int)mem_r32(c, GA(0x80181014u));
        for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) {
            if (!mem_r8(c, HUMAN_REC(i) + 0x9C) || life[i].at || coop_out[i]) continue; /* not dead, coming back, or out */
            if (human_gone[i]) { /* (left the match: no coming back) */
                if (duo) { death_delay = c->r[5]; duo_player_out(i); continue; }
                coop_out[i] = 1; /* (a tournament's shared lives: not one of them; the others play on, or the round is lost) */
                int other = 0;
                for (int j = 0; j < humans && j < CARS_HUMANS_MAX; j++) other |= j != i && !coop_out[j] && !mem_r8(c, HUMAN_REC(j) + 0x9C);
                if (other) return;
                break;
            }
            if (duo && lives_round == 1 && life[i].left <= 0) { death_delay = c->r[5]; duo_player_out(i); continue; } /* out: watches */
            if (lives_round == 1 && life[i].left <= 0 && mode == 3 && humans == 1 && !ui_survival() && spectate_possible()) {
                spectate_at = vp_gv() + c->r[5]; /* out: the wreck, then watch the rest (Survival alone: the run ends) */
                return;
            }
            if (lives_round == 2) { /* the tournament's pool: the last life ends the round as the game's */
                int pool = mem_r8(c, GA(0x8016475Cu) + 20);
                if (pool <= 1 && mode == 2 && humans >= 2 && coop_player_out(i, c->r[5])) return; /* the others play on */
                if (pool <= 1) break;
                mem_w8(c, GA(0x8016475Cu) + 20, (uint8_t)(pool - 1));
            } else {
                if (life[i].left <= 0) break;                         /* out of lives: the round ends */
                life[i].left--;
                lives_hud_put(i);
            }
            /* the game's delay (60 a second), then the game type's respawn time (a mode with respawns, or Last Car
             * Standing with lives: the user's 2026-10-30), watching */
            burn_wait = c->r[5]; /* (the computer cars wait it too) */
            life[i].at = vp_gv() + (uint64_t)c->r[5] + 60u * (uint64_t)respawn_extra();
            watch_at[i] = respawn_extra() > 0 ? vp_gv() + (uint64_t)c->r[5] : 0;
            fprintf(stderr, "lives: player %d lost a life (vblank %llu, back at %llu)\n", i + 1, (unsigned long long)vp_gv(),
                    (unsigned long long)life[i].at);
            if (duo) continue; /* the other may be down too */
            return; /* no round end */
        }
    }
    if (duo && value == 1) return; /* a player down: the port's rules go on */
    if (!(value == 1 && (mode == 1 || mode == 2))) killcam_round_end(); /* (not a tournament's lost life) */
    win_hold(c, 0);
    F_800D181C_orig(c);
}

/* The start point farthest from every other car: the others' entries are
 * marked taken for humanCarPlaceAtStart's pick (decomp COOP_LIVES.md 2.3). */
static void place_away(uint32_t rec, int self)
{
    const uint32_t list = GA(0x801B2B28u), taken = GA(0x801B2B46u), nodes = GA(0x801B1DE8u);
    int count = mem_r8(&M.cpu, GA(0x801B2B55u));
    if (count > 15) count = 15;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), best = -1;
    double best_d = -1;
    for (int e = 0; e < count; e++) {
        int n = (int16_t)mem_r16(&M.cpu, list + 2u * (uint32_t)e);
        double nx = (int32_t)mem_r32(&M.cpu, nodes + 24u * (uint32_t)n), ny = (int32_t)mem_r32(&M.cpu, nodes + 24u * (uint32_t)n + 4);
        double nearest = 1e18;
        for (int k = 0; k < humans + ais && (k < humans || k - humans < cars_ai_max()); k++) {
            uint32_t r = k < humans ? HUMAN_REC(k) : AI_REC(k - humans), pos = r + (k < humans ? 0x6FCu : 0x73Cu);
            if (k == self || mem_r8(&M.cpu, r + (k < humans ? 0x9Cu : 0x6Cu))) continue; /* itself, or a wreck */
            double dx = (int32_t)mem_r32(&M.cpu, pos) - nx, dy = (int32_t)mem_r32(&M.cpu, pos + 4) - ny, d = dx * dx + dy * dy;
            if (d < nearest) nearest = d;
        }
        if (nearest > best_d) { best_d = nearest; best = e; }
    }
    uint8_t saved[15];
    for (int e = 0; e < count; e++) {
        saved[e] = mem_r8(&M.cpu, taken + (uint32_t)e);
        mem_w8(&M.cpu, taken + (uint32_t)e, (uint8_t)(e != best));
    }
    guest_call(GA(0x800FD6C4u), rec, 0, 0, 0); /* a start point (the one left), full health */
    for (int e = 0; e < count; e++) mem_w8(&M.cpu, taken + (uint32_t)e, saved[e]);
}

/* A destroyed car is frozen (carDestroyed: the wreck, freeze_timer 540), and a freeze missile's ice is drawn
 * through its car model's freeze object (carFreezeSetState: 16 per model, not per car) until the freeze
 * countdown ends (car_tick.c: frozen, then the timer, then the ice off when frozen_by_missile). The placement
 * clears frozen, so the countdown never runs again: the ice off here, and the countdown's state, as the
 * computer cars' respawn does (ai_respawn). */
static uint32_t car_key_of(int slot, uint32_t type); /* (below) */
static void thaw_human(uint32_t rec)
{
    if (mem_r16(&M.cpu, rec + 0x168)) /* stats.frozen_by_missile */
        guest_call(GA(0x80124C00u), car_key_of((int16_t)mem_r16(&M.cpu, rec + 4), mem_r32(&M.cpu, rec + 0x90)), 0, 0, 0); /* no ice (its key) */
    mem_w16(&M.cpu, rec + 0x168, 0);
    mem_w16(&M.cpu, rec + 0x166, 0); /* stats.freeze_timer */
    mem_w8(&M.cpu, rec + 0xA9, 0);   /* state.frozen (the placement's too) */
}

/* The game's shield (its energy move 103) for a respawn: a free bubble of the shield pool (weaponShieldPool
 * 80184A68: N objects, then N records {timer, owner, the car's object, the bubble}; N rc_shield_count: 4 retail, 16 in
 * the 16-car variant) shown around the car;
 * weaponShieldsUpdate counts the timer down each frame, then hides it and ends the shield. 0: none free. */
static int shield_bubble(int i, int seconds)
{
    int slot = (int)guest_call(GA(0x80109F54u), 0, 0, 0, 0); /* weaponShieldSlotFind */
    if (slot < 0 || slot >= (int)rc_shield_count) return 0;
    const uint32_t pool = GA(0x80184A68u), obj = mem_r32(&M.cpu, pool + 4u * (uint32_t)slot);
    const uint32_t r = pool + 4u * rc_shield_count + 12u * (uint32_t)slot, car = guest_call(GA(0x8011CB50u), (uint32_t)i, 0, 0, 0);
    if (!obj || !car) return 0;
    int ticks = (int)guest_call(GA(0x8015531Cu), 0, 0, 0, 0); /* timerGetFrameTicks: VBlanks a frame */
    mem_w8(&M.cpu, HUMAN_REC(i) + 0xAA, 1);                /* state.energy_shield */
    mem_w32(&M.cpu, obj + 8, 1);                           /* the bubble shown */
    mem_w16(&M.cpu, r + 0, (uint16_t)(seconds * 60 / (ticks > 0 ? ticks : 3)));
    mem_w16(&M.cpu, r + 2, (uint16_t)i);
    mem_w32(&M.cpu, r + 4, car);
    mem_w32(&M.cpu, r + 8, obj);
    return 1;
}

static void watch_end_for(int i);
/* each human's respawn: where the car came in, and 2 seconds later where it is: far below that, or destroyed, logged
 * (the reports of cars spawning under the map: the next log names the spot). Log only, nothing else. */
static struct { uint64_t at; int32_t x, y, z; } spawn_check[CARS_HUMANS_MAX];
static void spawn_check_start(int i, uint32_t rec)
{
    if (i < 0 || i >= CARS_HUMANS_MAX) return;
    spawn_check[i].x = (int32_t)mem_r32(&M.cpu, rec + 0x6FC);
    spawn_check[i].y = (int32_t)mem_r32(&M.cpu, rec + 0x700);
    spawn_check[i].z = (int32_t)mem_r32(&M.cpu, rec + 0x704);
    spawn_check[i].at = vp_gv() + 120;
    fprintf(stderr, "spawn: player %d in at %d %d z %d (arena %d)\n", i + 1, spawn_check[i].x, spawn_check[i].y, spawn_check[i].z,
            (int)mem_r8(&M.cpu, GA(0x8016475Cu) + 24));
}
static void spawn_check_frame(void)
{
    /* a car under the arena may not fall: it can drive about down there, or a jump can put it back on top (the user's,
     * 2026-11-23). Twice a second, each player's height against the arena's lowest route point (the lowest place cars
     * drive): far below it, logged once (with where they last came in), and once when back above. Reads only (no game
     * call: the same on every machine and every re-run). */
    static int under[CARS_HUMANS_MAX];
    if (vp_gv() % 30 == 0) {
        int n = (int)mem_r32(&M.cpu, ROUTE_NODE_COUNT), humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
        int lowest = 32767;
        for (int k = 0; k < n && k < 400; k++) {
            int z = (int16_t)mem_r16(&M.cpu, ROUTE_NODES + 24u * (uint32_t)k + 8);
            if (z < lowest) lowest = z;
        }
        for (int i = 0; n > 0 && i < humans && i < CARS_HUMANS_MAX; i++) {
            uint32_t rec = HUMAN_REC(i);
            if (mem_r8(&M.cpu, rec + 0x9C)) continue; /* (a wreck) */
            int32_t z = (int32_t)mem_r32(&M.cpu, rec + 0x704);
            /* the lowest route point within 2000 of the car (a bridge over a tunnel: the tunnel's), else the arena's: a car on
             * its floor stands about 200 above it */
            int32_t cx = (int32_t)mem_r32(&M.cpu, rec + 0x6FC), cy = (int32_t)mem_r32(&M.cpu, rec + 0x700);
            int near = 32767;
            for (int k = 0; k < n && k < 400; k++) {
                int64_t dx = (int32_t)mem_r32(&M.cpu, ROUTE_NODES + 24u * (uint32_t)k) - cx;
                int64_t dy = (int32_t)mem_r32(&M.cpu, ROUTE_NODES + 24u * (uint32_t)k + 4) - cy;
                int nz = (int16_t)mem_r16(&M.cpu, ROUTE_NODES + 24u * (uint32_t)k + 8);
                if (dx * dx + dy * dy <= 2000LL * 2000 && nz < near) near = nz;
            }
            if (near == 32767) near = lowest;
            static int watch = -1; /* tests: TM2PC_SPAWN_WATCH, each check */
            if (watch < 0) watch = getenv("TM2PC_SPAWN_WATCH") != NULL;
            if (watch) fprintf(stderr, "spawn watch: player %d z %d, lowest route point near %d (arena %d)\n", i + 1, z, near, lowest);
            if (!under[i] && z < near - 300) {
                under[i] = 1;
                fprintf(stderr, "spawn: player %d UNDER the arena at %d %d z %d (the lowest route point near: z %d; last in at %d %d z %d)\n",
                        i + 1, cx, cy, z, near,
                        spawn_check[i].x, spawn_check[i].y, spawn_check[i].z);
            } else if (under[i] && z >= near - 100) {
                under[i] = 0;
                fprintf(stderr, "spawn: player %d back above at %d %d z %d\n", i + 1, (int32_t)mem_r32(&M.cpu, rec + 0x6FC),
                        (int32_t)mem_r32(&M.cpu, rec + 0x700), z);
            }
        }
    }
    for (int i = 0; i < CARS_HUMANS_MAX; i++) {
        if (!spawn_check[i].at || vp_gv() < spawn_check[i].at) continue;
        spawn_check[i].at = 0;
        uint32_t rec = HUMAN_REC(i);
        int32_t x = (int32_t)mem_r32(&M.cpu, rec + 0x6FC), y = (int32_t)mem_r32(&M.cpu, rec + 0x700), z = (int32_t)mem_r32(&M.cpu, rec + 0x704);
        int dead = mem_r8(&M.cpu, rec + 0x9C);
        if (z < spawn_check[i].z - 400 || dead)
            fprintf(stderr, "spawn: player %d 2 s after coming in at %d %d z %d: %s at %d %d z %d (%d below)\n", i + 1, spawn_check[i].x,
                    spawn_check[i].y, spawn_check[i].z, dead ? "DESTROYED" : "FELL", x, y, z, spawn_check[i].z - z);
    }
}
static void respawn_human(int i)
{
    uint32_t rec = HUMAN_REC(i);
    watch_end_for(i); /* (watching while waiting: their own view again) */
    /* carRestart's per-car stores (800FD550), not its global wipes */
    mem_w8(&M.cpu, rec + 0x14C, 1);
    mem_w32(&M.cpu, rec + 0x7CC, 0);
    mem_w32(&M.cpu, rec + 0x7FC, 0);
    mem_w8(&M.cpu, rec + 0xF1, 0);
    mem_w8(&M.cpu, rec + 0xF2, 0);
    mem_w8(&M.cpu, rec + 0xF4, 0);
    mem_w8(&M.cpu, rec + 0xC8, 0);
    mem_w8(&M.cpu, rec + 0x8C, 0);
    for (int k = 0x155; k <= 0x158; k++) mem_w8(&M.cpu, rec + (uint32_t)k, 0);
    mem_w8(&M.cpu, rec + 0x14D, 0);
    mem_w8(&M.cpu, rec + 0x235, 1);
    guest_call(GA(0x800FF764u), rec, 0, 0, 0);                             /* viewCameraOffsetsInit */
    guest_call(GA(0x800EC938u), rec + 0xD0, 0, 0, 0);                      /* aiClearControls */
    guest_call(GA(0x8011D6A4u), rec, mem_r32(&M.cpu, RB_padConfigs + 4u * (uint32_t)i), 0, 0); /* the pad config */
    guest_call(GA(0x800FDCACu), rec, rec + 0x7C8u, 1, 0);                  /* a fresh weapon block */
    place_away(rec, i);                                                /* away from the others, full health */
    guest_call(GA(0x8011CBB0u), (uint32_t)i, 0, 0, 0);                     /* the displayed speed */
    uint32_t obj = guest_call(GA(0x8011CB50u), (uint32_t)i, 0, 0, 0);      /* its object, shown again */
    if (obj) guest_call(GA(0x80149604u), obj, 1, 0, 0);
    mem_w8(&M.cpu, GA(0x80180F94u), 0);                                    /* the death flag */
    thaw_human(rec);
    int shield = battle_rules() && !ui_survival() ? battle_rules()->spawn_shield : 2; /* the game type's spawn shield */
    life[i].shield_off = 0;
    if (shield > 0 && !shield_bubble(i, shield)) { /* no bubble free: the shield alone, ended here */
        mem_w8(&M.cpu, rec + 0xAA, 1);
        life[i].shield_off = vp_gv() + 60u * (uint64_t)shield;
    }
    spawn_check_start(i, rec);
    fprintf(stderr, "lives: player %d is back (vblank %llu)\n", i + 1, (unsigned long long)vp_gv());
}

void vp_ai_frame(void);
static char flash_text[48];
static uint64_t flash_until;
static void kill_feed_add(int own, int killer, int attack);
static unsigned long long win_at, vs_at; /* (the tests' hooks below: kept with the kill cam's machine) */
static unsigned freeze_fired, fired;
int vp_versus_on(void);
int vp_versus_points(int h);
int vp_human_handover(int h);
void vp_human_back(int h);
static void test_handover(void) /* tests: TM2PC_TEST_HANDOVER="H@V": from VBlank V, once human H (1-based) has the spawn
                                 * shield on, a computer driver takes the car (as a player leaving online), 120 VBlanks
                                 * later H takes it back; each 60 VBlanks after, both records' shield flags */
{
    static int h = -2, step, j;
    static unsigned long long at, next;
    if (h == -2) {
        const char *e = getenv("TM2PC_TEST_HANDOVER");
        unsigned long long v = 0;
        h = e && sscanf(e, "%d@%llu", &h, &v) == 2 ? h - 1 : -1;
        at = v;
    }
    if (h < 0 || vp_gv() < at) return;
    if (step == 0 && mem_r8(&M.cpu, HUMAN_REC(h) + 0xAA)) {
        j = vp_human_handover(h) ? handed[h] - 1 : -1;
        fprintf(stderr, "test: handover of player %d (shield on) to computer record %d\n", h + 1, j);
        step = j >= 0 ? 1 : 9;
        at = vp_gv() + 120;
    } else if (step == 1) {
        vp_human_back(h);
        fprintf(stderr, "test: player %d back\n", h + 1);
        step = 2;
        next = vp_gv();
        at = vp_gv() + 900;
    } else if (step == 2) {
        if (vp_gv() >= next) {
            fprintf(stderr, "test: +%llu shield: player %d %d, computer record %d %d\n", vp_gv() + 900 - at, h + 1,
                    mem_r8(&M.cpu, HUMAN_REC(h) + 0xAA), j, mem_r8(&M.cpu, AI_REC(j) + 0x7A));
            next = vp_gv() + 60;
        }
        if (vp_gv() >= at) step = 9;
    }
}
static void test_kills(void) /* tests: TM2PC_TEST_KILL="1@2400,2@3000" destroys player P at VBlank V */
{
    test_handover();
    static int eiffel_at = -2; /* tests: TM2PC_TEST_EIFFEL=N, Paris's tower falls at the round's frame N (every machine the
                                * same, a step run again too: online desync tests, Boxes's 2026-11-25) */
    if (eiffel_at == -2) eiffel_at = getenv("TM2PC_TEST_EIFFEL") ? atoi(getenv("TM2PC_TEST_EIFFEL")) : -1;
    if (eiffel_at >= 0 && vp_round_frames() == eiffel_at && mem_r8(&M.cpu, GA(0x8016475Cu) + 24) == 3) { /* (Paris) */
        guest_call(GA(0x800D6710u), 0, 0, 0, 0); /* eiffelTowerStartFall (a second call does nothing) */
        fprintf(stderr, "test: the Eiffel tower falls (round frame %d)\n", vp_round_frames());
    }
    static int flashed;
    if (!flashed && getenv("TM2PC_TEST_FLASH") && vp_gv() >= (getenv("TM2PC_TEST_FLASH_AT") ? strtoull(getenv("TM2PC_TEST_FLASH_AT"), 0, 10) : 2000)) { /* tests: a port message at VBlank 2000 */
        flashed = 1;
        snprintf(flash_text, sizeof flash_text, "%s", getenv("TM2PC_TEST_FLASH"));
        flash_until = vp_gv() + 150;
    }
    static int win_every = -1;
    if (win_every < 0) win_every = getenv("TM2PC_TEST_WIN_EVERY") ? atoi(getenv("TM2PC_TEST_WIN_EVERY")) : 0;
    if (win_every > 0) { /* tests: every computer car destroyed this many VBlanks into each round (a tournament's flow) */
        int ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), any = 0;
        for (int k = 0; k < ais && k < cars_ai_max(); k++) any |= ai_alive(k);
        if (!any && vp_versus_on()) { /* a Versus Tournament: then the players, one by one, all but one (another each stage) */
            int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), in = 0, total = 0, last = -1;
            for (int h = 0; h < humans && h < CARS_HUMANS_MAX; h++) total += vp_versus_points(h);
            int spare = humans > 0 ? total % humans : 0;
            for (int h = 0; h < humans && h < CARS_HUMANS_MAX; h++)
                if (!mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C)) { in++; if (h != spare) last = h; }
            if (in <= 1 || last < 0) vs_at = 0;
            else if (!vs_at) vs_at = vp_gv() + (unsigned)win_every / 2;
            else if (vp_gv() >= vs_at) {
                fprintf(stderr, "test: destroying player %d\n", last + 1);
                guest_call5(GA(0x800E5628u), (uint32_t)(last + 1), 1000, 0, 0, (uint32_t)-1); /* carApplyDamage */
                vs_at = vp_gv() + (unsigned)win_every / 2;
            }
        }
        if (!any) win_at = 0;
        else if (!win_at) win_at = vp_gv() + (unsigned)win_every;
        else if (vp_gv() >= win_at) {
            fprintf(stderr, "test: destroying every computer car\n");
            for (int k = 0; k < ais && k < cars_ai_max(); k++)
                if (ai_alive(k)) mem_w32(&M.cpu, AI_REC(k) + 0x5A0, 0); /* its health: aiCarDamageUpdate destroys it */
            win_at = vp_gv() + (unsigned)win_every;
        }
    }
    static int ai_trace = -1; /* tests: TM2PC_AI_TRACE=N, every N VBlanks each computer car's place, section, route points (previous, current, next) and health */
    if (ai_trace < 0) ai_trace = getenv("TM2PC_AI_TRACE") ? atoi(getenv("TM2PC_AI_TRACE")) : 0;
    static unsigned long long ai_trace_next;
    if (ai_trace > 0 && vp_gv() >= ai_trace_next) {
        ai_trace_next = vp_gv() + (unsigned)ai_trace;
        int ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
        for (int k = 0; k < ais && k < cars_ai_max(); k++) {
            if (!ai_alive(k)) continue;
            uint32_t r = AI_REC(k); /* (its drive block: decomp TM2AIDrive at +0x448) */
            int tg = (int32_t)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k); /* aiCarTargets: -1 the nearest player */
            fprintf(stderr, "ai trace %llu: car %d at %d %d %d section %d nodes %d %d %d health %d target %d in %d\n",
                    (unsigned long long)vp_gv(), k,
                    (int32_t)mem_r32(&M.cpu, r + 0x73C), (int32_t)mem_r32(&M.cpu, r + 0x740), (int32_t)mem_r32(&M.cpu, r + 0x744),
                    (int16_t)mem_r16(&M.cpu, r + 0x46A), (int16_t)mem_r16(&M.cpu, r + 0x462), (int16_t)mem_r16(&M.cpu, r + 0x464),
                    (int16_t)mem_r16(&M.cpu, r + 0x466), (int32_t)mem_r32(&M.cpu, r + 0x5A0), tg,
                    tg >= 0 && tg < cars_ai_max() ? (int16_t)mem_r16(&M.cpu, AI_REC(tg) + 0x46A) : -1);
            if (getenv("TM2PC_MODS_DEBUG")) /* (and its stats block, +0x4FC: a mod character's own, decomp request 70, CAR_PROFILES.md) */
                fprintf(stderr, "ai trace: car %d top speed %d acceleration %d max hp %d mass %d track %d axles %d %d fire %d\n", k,
                        (int16_t)mem_r16(&M.cpu, r + 0x4FC + 0x52), (int32_t)mem_r32(&M.cpu, r + 0x4FC + 0xAC),
                        (int16_t)mem_r16(&M.cpu, r + 0x4FC + 0x3A), (int16_t)mem_r16(&M.cpu, r + 0x4FC + 0x7A),
                        (int16_t)mem_r16(&M.cpu, r + 0x4FC + 0x48), (int16_t)mem_r16(&M.cpu, r + 0x4FC + 0x4E),
                        (int16_t)mem_r16(&M.cpu, r + 0x4FC + 0x50),
                        mem_r8(&M.cpu, r + 0x808 + 0x0A)); /* (weapons.fire_chance: [ai] fire_chance, decomp request 87) */        }
    }
    static const char *freeze_list; /* tests: TM2PC_TEST_FREEZE="1@2900": player P frozen at VBlank V, as by a freeze missile */
    static int freeze_read;

    if (!freeze_read) freeze_read = 1, freeze_list = getenv("TM2PC_TEST_FREEZE");
    for (const char *p = freeze_list; p && *p; ) {
        int who = atoi(p), n = (int)(p - freeze_list);
        const char *at = strchr(p, '@');
        if (!at) break;
        if (who >= 1 && who <= 4 && vp_gv() >= strtoull(at + 1, NULL, 10) && !(freeze_fired & (1u << (n & 31)))) {
            uint32_t rec = HUMAN_REC(who - 1);
            freeze_fired |= 1u << (n & 31);
            mem_w8(&M.cpu, rec + 0xA9, 1);     /* weaponSlamSwing's freeze (type 14): frozen, */
            mem_w16(&M.cpu, rec + 0x166, 300); /* the countdown, */
            mem_w16(&M.cpu, rec + 0x168, 1);   /* by a missile, */
            guest_call(GA(0x80124C00u), mem_r32(&M.cpu, rec + 0x90), 1, 0, 0); /* the ice */
            fprintf(stderr, "test: freezing player %d\n", who);
        }
        if (who >= 50 && who - 50 < cars_ai_max() && vp_gv() >= strtoull(at + 1, NULL, 10) && !(freeze_fired & (1u << (n & 31)))) {
            uint32_t rec = AI_REC(who - 50); /* (computer car k = 50 + k: the same fields, at its blocks' offsets) */
            freeze_fired |= 1u << (n & 31);
            mem_w8(&M.cpu, rec + 0x79, 1);
            mem_w16(&M.cpu, rec + 0x516, 300);
            mem_w16(&M.cpu, rec + 0x518, 1);
            guest_call(GA(0x80124C00u), mem_r32(&M.cpu, rec + 0x30), 1, 0, 0);
            fprintf(stderr, "test: freezing computer car %d\n", who - 50);
        }
        p = strchr(at, ',');
        if (!p) break;
        p++;
    }
    static const char *list;
    static int read;
    if (!read) read = 1, list = getenv("TM2PC_TEST_KILL");
    if (!list) return;

    int n = 0;
    for (const char *p = list; *p; n++) {
        int who = atoi(p);
        const char *at = strchr(p, '@');
        if (!at) break;
        unsigned long long v = strtoull(at + 1, NULL, 10);
        int alive = who >= 1 && who <= 4 ? !mem_r8(&M.cpu, HUMAN_REC(who - 1) + 0x9C) : who >= 50 && who < 59 && ai_alive(who - 50);
        if (alive && vp_gv() >= v && !(fired & (1u << n))) { /* 1-2 a player, 50+k computer car k */
            fired |= 1u << n;
            fprintf(stderr, "test: destroying car %d at %llu\n", who, (unsigned long long)M.vblanks);
            if (getenv("TM2PC_TEST_FEED")) /* tests: a feed line as if player 1 had killed it with this attack (A_*) */
                for (const char *q = getenv("TM2PC_TEST_FEED"); *q; q += strcspn(q, ",") + (q[strcspn(q, ",")] ? 1 : 0))
                    if (getenv("TM2PC_TEST_FEED_P1")) /* (_P1: player 2's car, by player 1's) */
                        kill_feed_add((int16_t)mem_r16(&M.cpu, HUMAN_REC(1) + 4), (int16_t)mem_r16(&M.cpu, HUMAN_REC(0) + 4), atoi(q));
                    else
                        kill_feed_add(1, 0, atoi(q));
            const char *kb_ai = getenv("TM2PC_TEST_KILL_BY");
            if (who >= 50 && kb_ai && atoi(kb_ai) >= 1 && atoi(kb_ai) <= 4) /* tests: a computer car's kill credited to that player */
                guest_call5(GA(0x800E5628u), (uint32_t)who, 1000, 0, 0, /* (its id: 50 + k) */
                            (uint32_t)(int16_t)mem_r16(&M.cpu, HUMAN_REC(atoi(kb_ai) - 1) + 4));
            else if (who >= 50) mem_w32(&M.cpu, AI_REC(who - 50) + 0x5A0, 0); /* its health: aiCarDamageUpdate destroys it */
            else {
                const char *kb = getenv("TM2PC_TEST_KILL_BY"); /* tests: by this player (1-4) */
                uint32_t by = getenv("TM2PC_TEST_KILL_SELF") ? (uint32_t)(int16_t)mem_r16(&M.cpu, HUMAN_REC(who - 1) + 4) /* tests: */
                              : kb && atoi(kb) >= 1 && atoi(kb) <= 4 ? (uint32_t)(int16_t)mem_r16(&M.cpu, HUMAN_REC(atoi(kb) - 1) + 4)
                                                             : (uint32_t)-1;                                 /* its own weapons */
                guest_call5(GA(0x800E5628u), (uint32_t)who, 1000, 0, 0, by); /* carApplyDamage: victim id, ..., attacker */
            }
        }
        p = strchr(at, ',');
        if (!p) break;
        p++;
    }
}
/* tests, trailer footage: TM2PC_TEST_ROUND_KILLS="H@S:B,cK@S,...": each round, S seconds after its start, human H
 * (1-16) destroyed by human B (0 or none: by nothing), or computer car K (0..) destroyed. Every machine with the same
 * list does the same (a match's game state) */
static void round_test_kills(void)
{
    static const char *list = (const char *)1;
    if (list == (const char *)1) list = getenv("TM2PC_TEST_ROUND_KILLS");
    if (!list || !test_round_gv) return;
    int n = 0;
    for (const char *p = list; *p && n < 32; n++) {
        int ai = *p == 'c';
        int who = atoi(ai ? p + 1 : p), by = 0;
        const char *at = strchr(p, '@'), *end = strchr(p, ',');
        if (!at) break;
        double secs = strtod(at + 1, NULL);
        const char *colon = strchr(at, ':');
        if (colon && (!end || colon < end)) by = atoi(colon + 1);
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
        static int rotate = -1;
        if (rotate < 0) rotate = getenv("TM2PC_TEST_ROUND_ROTATE") != NULL;
        if (rotate && humans > 0 && !ai) { /* each round another order: the players turned by the round's number */
            who = (who - 1 + 3 * (test_round_n - 1)) % humans + 1;
            if (by >= 1) by = (by - 1 + 3 * (test_round_n - 1)) % humans + 1;
        }
        int alive = ai ? who >= 0 && who < cars_ai_max() && ai_alive(who)
                       : who >= 1 && who <= humans && who <= CARS_HUMANS_MAX && !mem_r8(&M.cpu, HUMAN_REC(who - 1) + 0x9C);
        if (alive && !(test_round_fired & (1u << n)) && vp_gv() >= test_round_gv + (uint64_t)(secs * 60)) {
            test_round_fired |= 1u << n;
            fprintf(stderr, "test: round kill %s%d by %d at %llu\n", ai ? "computer car " : "player ", who, by, (unsigned long long)vp_gv());
            if (ai) mem_w32(&M.cpu, AI_REC(who) + 0x5A0, 0); /* its health: aiCarDamageUpdate destroys it */
            else {
                int h = who - 1;
                uint32_t victim = h < 7 ? (uint32_t)h + 1 : h == 7 ? 9u : (uint32_t)h + 3; /* (its object id, as vp_human_left) */
                uint32_t killer = by >= 1 && by <= humans ? (uint32_t)(int16_t)mem_r16(&M.cpu, HUMAN_REC(by - 1) + 4) : (uint32_t)-1;
                guest_call5(GA(0x800E5628u), victim, 1000, 0, 1, killer); /* carApplyDamage: victim id, amount, dir, type 1 (a
                                                                           * weapon's: the explosion, the body flying apart), attacker */
            }
        }
        if (!end) break;
        p = end + 1;
    }
}
/* trailer footage: TM2PC_TEST_STAGE_CLEAR="S" (or "S:cpu", co-op: the computer cars only): each stage, S seconds after its
 * start, the other cars destroyed one at a time (every 0.7 s, the next one alive: a car back from a life goes again;
 * each stage starting at another car), player 1's kills, until the stage ends. Player 1 (the recorded copy's, the user)
 * drives the stage until then. The game state only: every machine the same */
static void stage_clear_test(void)
{
    static int secs = -2, cpu_only, no_boss;
    if (secs == -2) {
        const char *e = getenv("TM2PC_TEST_STAGE_CLEAR");
        secs = e ? atoi(e) : -1;
        cpu_only = e && strstr(e, "cpu") != NULL;
        no_boss = e && strstr(e, "noboss") != NULL;
    }
    if (secs < 0 || !test_round_gv) return;
    if (no_boss) /* ("S:cpu:noboss": Dark Tooth's phase left to play out, he alive (type 15)) */
        for (int k = 0; k < cars_ai_max() && k < (int)mem_r32(&M.cpu, GA(0x8018101Cu)); k++) {
            int type = (int)((mem_r32(&M.cpu, AI_REC(k) + 0x30) >> 4) & 15);
            if (type == 15 && ai_alive(k)) return;
        }
    uint64_t t0 = test_round_gv + (uint64_t)secs * 60;
    if (vp_gv() < t0 || (vp_gv() - t0) % 42) return;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    if (ais > cars_ai_max()) ais = cars_ai_max();
    extern int rbs_resimulating; /* (a step run again: told the first time) */
    int p1_down = mem_r8(&M.cpu, HUMAN_REC(0) + 0x9C) != 0; /* (player 1 down too: on, the kills no one's) */
    uint32_t by = p1_down ? (uint32_t)-1 : (uint32_t)(int16_t)mem_r16(&M.cpu, HUMAN_REC(0) + 4);
    int n = (cpu_only ? 0 : humans - 1) + ais;
    for (int t = 0; t < n; t++) {
        int i = (t + 3 * (test_round_n - 1)) % n, h = cpu_only ? -1 : i < humans - 1 ? i + 1 : -1, k = h >= 0 ? -1 : i - (cpu_only ? 0 : humans - 1);
        if (h >= 0) {
            if (h >= CARS_HUMANS_MAX || mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C) || human_gone[h] || coop_out[h]) continue;
            uint32_t victim = h < 7 ? (uint32_t)h + 1 : h == 7 ? 9u : (uint32_t)h + 3; /* (its object id, as vp_human_left) */
            if (!rbs_resimulating) fprintf(stderr, "test: stage clear: player %d at %llu\n", h + 1, (unsigned long long)vp_gv());
            guest_call5(GA(0x800E5628u), victim, 1000, 0, 1, by); /* carApplyDamage, type 1: a weapon's death (type 0 was
                                                                   * a quiet wreck, no explosion: the user's 16-player take 2026-11-17) */
            return;
        }
        if (k < 0 || !ai_alive(k) || !mem_r8(&M.cpu, AI_REC(k) + 0x34)) continue; /* (not active: a boss waiting hidden, its
                                                                                 * turn later; the next one instead) */
        if (!rbs_resimulating) fprintf(stderr, "test: stage clear: computer car %d at %llu\n", k + 1, (unsigned long long)vp_gv());
        if (by != (uint32_t)-1) guest_call5(GA(0x800E5628u), (uint32_t)(50 + k), 1000, 0, 1, by); /* (its id 50 + k; a weapon's death) */
        if ((int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x5A0) > 0) /* (no attacker, or the hit taken by nothing: a shield, a car
                                                               * not active; some took 80 hits on Amazonia, the user's co-op
                                                               * take 2026-11-17) its health: aiCarDamageUpdate destroys it */
            mem_w32(&M.cpu, AI_REC(k) + 0x5A0, 0);
        return;
    }
}
static void vp_duo_frame(void);
static void duo_round_start(void);
static void showdown_frame(void);
static void watch_frame(void); /* (below, with the watching) */
static void survival_watch_frame(void);
static void versus_frame(void);
/* Dark Tooth's ricochet look (decomp request 122, PLAYTEST_ANSWERS.md 36): in his BB.DMD, 15 animation frames whose
 * primitive pointer (+0x0C) something overwrites with an absolute value (FDA1F14C: Supr's and Mohamed Eissa's co-op Dark
 * Tooth phase, 2026-11-23; request 110 before): the emitter then drew garbage (missing textures, broken pictures, a
 * freeze). Each step, every loaded BB.DMD (known by its frame 5's vertex pointer, base + 0x1DD0): each frame's pointer
 * back to its own primitives (header + 0x2C) if it is not, the first time logged (the step says load or play: 36.4).
 * Every machine the same (the same files at the same places), so the same online. */
static void dt_ricochet_check(void)
{
    static uint32_t a_images = 1;
    if (a_images == 1) a_images = port_sym("portDmdImages");
    if (!a_images) return;
    static const uint32_t frames[15] = {0xE8DC, 0xE950, 0xE9C4, 0xEA38, 0xEAAC, 0xEB20, 0xEB94, 0xEC08,
                                        0xEC7C, 0xECF4, 0xED68, 0xEDDC, 0xEE50, 0xEEC4, 0xEF38};
    static int told;
    for (uint32_t g = 1; g < 64; g++) {
        uint32_t base = mem_r32(&M.cpu, a_images + 4u * g);
        if (!base || (base & 0x1FFFFFFFu) + 0xF0B8u >= GUEST_RAM_BYTES) continue;
        if (mem_r32(&M.cpu, base + 0xEB20u + 4) != base + 0x1DD0u) continue; /* (not a BB.DMD) */
        static uint32_t seen[8];
        int known = 0;
        for (int k = 0; k < 8; k++) known |= seen[k] == base;
        if (!known) { /* (once per file loaded: the check is on) */
            for (int k = 7; k > 0; k--) seen[k] = seen[k - 1];
            seen[0] = base;
            fprintf(stderr, "dark tooth: his model file (group %u at %08X) watched\n", g, base);
        }
        for (int f = 0; f < 15; f++) {
            uint32_t h = base + frames[f], want = h + 0x2C, got = mem_r32(&M.cpu, h + 0x0C);
            if (got == want) continue;
            if (told++ < 20)
                fprintf(stderr, "dark tooth: ricochet frame %d's primitives %08X (should be %08X): put back (group %u at %08X, "
                        "step %llu, frame %d of the round)\n", f, got, want, g, base, (unsigned long long)vp_gv(), vp_round_frames());
            mem_w32(&M.cpu, h + 0x0C, want);
        }
    }
}
void vp_messages_clock_check(void); /* (below, with the kill feed) */
void vp_lives_frame(void)
{
    vp_messages_clock_check();
    dt_ricochet_check();
    spawn_check_frame();
    versus_frame();
    test_kills();
    round_test_kills();
    stage_clear_test();
    { /* (any game: coop_out is co-op's only) a player out, watching (co-op, 3-4 players): their HUD's lives 0, not one below it (the
                           * game's byte at 0 drew "LIVES: ." under the spectating bar; the user's: out stays 0) */
        int n = (int)mem_r32(&M.cpu, GA(0x80181014u));
        int any_out = 0;
        for (int h = 0; h < n && h < CARS_HUMANS_MAX; h++) any_out |= coop_out[h];
        for (int h = 0; h < n && h < CARS_HUMANS_MAX; h++) {
            /* (a co-op tournament's lives are the team's, in player 1's byte: with anyone out, every view's lives 0 too) */
            if (!coop_out[h] && !(any_out && mem_r8(&M.cpu, GA(0x8016475Cu)) == 2)) continue;
            static uint32_t lv = 1; /* (gameGetLives: the settings' +20.. for players 1-4, or portHumanLives for every
                                     * player with the port's rules on, as online) */
            if (lv == 1) lv = port_sym("portHumanLives");
            uint32_t a[2] = {h < 4 ? GA(0x8016475Cu) + 20 + (uint32_t)h : 0, lv ? lv + (uint32_t)h : 0};
            for (int q = 0; q < 2; q++) /* (0 drew "/", one taken past it 255 drew ".") */
                if (a[q] && (mem_r8(&M.cpu, a[q]) == 0 || mem_r8(&M.cpu, a[q]) >= 0x80)) mem_w8(&M.cpu, a[q], 1);
        }
        /* a tournament (co-op): player 1 out, watching, and the others destroyed the last enemy: the level won, as the
         * game's own won test would with player 1 driving (worldUpdate's test needs humanCars[0] alive: the level went on
         * with no enemies, forever). The same rule and timing as the game's (event 3 after 420 ticks; Dark Tooth's 210) */
        int mode = mem_r8(&M.cpu, GA(0x8016475Cu)), other = 0;
        for (int h = 1; h < n && h < CARS_HUMANS_MAX; h++) other |= !coop_out[h] && !mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C);
        if ((mode == 1 || mode == 2) && n >= 2 && coop_out[0] && mem_r8(&M.cpu, HUMAN_REC(0) + 0x9C) && other &&
            (int32_t)mem_r32(&M.cpu, AI_REMAINING) <= 0 && !mem_r32(&M.cpu, GA(0x80180FB0u))) { /* (ROUND_WON_FLAG) */
            static uint32_t end = 1, dt = 1;
            if (end == 1) { end = port_sym("portRoundEnd"); dt = port_sym("portCarDarkToothRoundEnd"); }
            if (end) {
                int delay = dt && guest_call(dt, 0, 0, 0, 0) ? 210 : 420;
                extern int rbs_resimulating;
                mem_w32(&M.cpu, GA(0x80180F94u), 0); /* roundOverFlag: player 1's death set it (the round lost, an end
                                                        * the port held back for the others): the level's end is this one */
                if (guest_call(end, 3, (uint32_t)delay, 0, 1 /* TM2_ROUND_FREEZE */) && !rbs_resimulating)
                    fprintf(stderr, "lives: player 1 out, the others won the level\n");
            }
        }
    }
    watch_frame();
    /* the players out, watching: their buttons (X next, O back, Square cinematic) in every mode (it ran only from the
     * Challenge rules' frame: in a co-op tournament, online or with 3-4 players, the view stayed on its first car, the
     * user's 2026-11-05) */
    survival_watch_frame();
    showdown_frame();
    vp_duo_frame();
    spectate_frame();
    vp_ai_frame();
    for (int i = 0; i < CARS_HUMANS_MAX; i++) /* (a shield's end: also with no respawns, a car back from a computer driver) */
        if (life[i].shield_off && vp_gv() >= life[i].shield_off) {
            life[i].shield_off = 0;
            mem_w8(&M.cpu, HUMAN_REC(i) + 0xAA, 0);
        }
    if (!lives_round) return;
    for (int i = 0; i < CARS_HUMANS_MAX; i++)
        if (life[i].at && vp_gv() >= life[i].at) { life[i].at = 0; respawn_human(i); }
}

/* ---- computer cars: lives, and whom they hunt ----
 * Lives (decomp AI_LIVES.md): a computer car with a life left is dead as the
 * game's (its wreck, its explosion), still counted as an enemy
 * (aiCarsCountRemaining, aiCarsCountAlive), and after 7 seconds it alone
 * comes back (2.6: aiCarStart at the start point farthest from the other
 * cars, its damage model and body object, the object shown, 2 seconds of
 * shield). Dark Tooth as a boss has one life (his two-stage fight).
 * Whom they hunt (the game type's Computer cars): the game's (every one the
 * nearest human: aiCarTargets -1; a computer car is hurt only while in range
 * of its own target, +0x034); Everyone equal (the nearest car of any kind,
 * always hurt); Only target humans (never each other, as the game's, but
 * always hurt: their stray hits on each other count). */
static struct { int left; uint64_t at, shield_off; } ai_life[CARS_MAX];

int vp_ai_ignore_ai(void) { return 0; } /* accidental hits between computer cars count in every setting */

static void ai_lives_start(void)
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), lives = battle_rules() ? battle_lives() : 1;
    /* Dark Tooth: one life as a boss (his two-stage fight: Survival's, a tournament's level 8); an ordinary computer Dark
     * Tooth (a Challenge Match's or a custom tournament's: portCarOptions bit 10) the game type's lives as any car (decomp
     * request 121, PLAYTEST_ANSWERS.md 35.2; KinggzZ's report) */
    uint32_t opt = port_sym("portCarOptions");
    int dt_ordinary = opt && (mem_r32(&M.cpu, opt) & 0x400u);
    for (int k = 0; k < cars_ai_max(); k++) {
        ai_life[k].left = k < n && (mem_r32(&M.cpu, AI_REC(k) + 0x30) != 0xF0u || dt_ordinary) ? lives - 1 : 0;
        ai_life[k].at = ai_life[k].shield_off = 0;
    }
}

static int ai_life_pending(int k) { return k >= 0 && k < cars_ai_max() && ai_life[k].at != 0; }
static int ai_pending(void)
{
    int n = 0;
    for (int k = 0; k < cars_ai_max(); k++) n += ai_life[k].at != 0;
    return n;
}

void F_8011BBD4_orig(CPU *c);
void vp_AiCarDamageUpdate(CPU *c) /* aiCarDamageUpdate(i) */
{
    int k = (int)c->r[4];
    int was = k >= 0 && k < cars_ai_max() ? mem_r8(c, AI_REC(k) + 0x6C) : 1;
    F_8011BBD4_orig(c);
    void versus_ai_out(int k);
    if (!was && k >= 0 && k < cars_ai_max() && mem_r8(c, AI_REC(k) + 0x6C) && !(ai_life[k].left > 0)) versus_ai_out(k);
    if (was || k < 0 || k >= cars_ai_max() || !battle_rules() || !mem_r8(c, AI_REC(k) + 0x6C)) return;
    if (ai_life[k].left > 0 && !ai_life[k].at) { /* dead with a life left: back in 7 seconds */
        ai_life[k].left--;
        ai_life[k].at = vp_gv() + (vp_dm() ? burn_wait : 420) + 60u * (uint64_t)respawn_extra(); /* (the burning, then the extra) */
        fprintf(stderr, "lives: computer car %d lost a life (%d left)\n", k + 1, ai_life[k].left);
    }
}

int vp_survival_waiting(void);
void F_8011C248_orig(CPU *c);
void vp_AiCountRemaining(CPU *c) /* aiCarsRemaining = the cars with HP > 0: and those coming back */
{
    F_8011C248_orig(c);
    if (teams_on()) mem_w32(c, AI_REMAINING, (uint32_t)enemies_left()); /* teams: only the other teams' cars */
    else if (battle_rules())
        mem_w32(c, AI_REMAINING, mem_r32(c, AI_REMAINING) + (uint32_t)(ai_pending() + vp_survival_waiting() + boss_coming()));
}
void F_8011E16C_orig(CPU *c);
void vp_AiCountAlive(CPU *c) /* the HUD's "Enemies: N": the same */
{
    F_8011E16C_orig(c);
    if (teams_on()) c->r[2] = (uint32_t)enemies_left();
    else if (battle_rules()) c->r[2] += (uint32_t)(ai_pending() + vp_survival_waiting() + boss_coming());
    int vp_versus_on(void);
    int no_cpu = battle_rules() && !ui_survival() && mem_r8(c, GA(0x8016475Cu)) == 4; /* (the game's mode 4: 2 players and no
                                    * computer cars, not vp_duo's 3: a 1v1 online read "Enemies Toasted!" from its start,
                                    * the user's Hong Kong final dry run 2026-11-16) */
    if (!teams_on() && (vp_duo() || vp_versus_on() || no_cpu)) { /* a Challenge Match (or a Versus Tournament's stage) of 2 or more humans: the other players count as enemies too (with no
                                    * computer cars an online match read "Enemies Toasted!" from its start, the trailer's).
                                    * Every car still in but one: the same number on every machine (the HUD's state is the
                                    * game's, hashed online), so a player out sees one more than is left for them */
        int H = (int)mem_r32(c, GA(0x80181014u)), in = 0;
        for (int o = 0; o < H && o < CARS_HUMANS_MAX; o++) in += !coop_out[o];
        if (in > 0) c->r[2] += (uint32_t)(in - 1);
    }
    static int dbg = -1, last = -1; /* tests: TM2PC_ENEMIES_DEBUG, each change of the count */
    if (dbg < 0) dbg = getenv("TM2PC_ENEMIES_DEBUG") != NULL;
    if (dbg && (int)c->r[2] != last) {
        last = (int)c->r[2];
        int H = (int)mem_r32(c, GA(0x80181014u)), in = 0;
        for (int o = 0; o < H && o < CARS_HUMANS_MAX; o++) in += !coop_out[o];
        fprintf(stderr, "enemies: %d at gv %llu (ai remaining %d, humans %d in %d, duo %d versus %d)\n", last,
                (unsigned long long)vp_gv(), (int)mem_r32(c, AI_REMAINING), H, in, vp_duo(), vp_versus_on());
    }
}

/* a car's key (the 16-car variant: the second car of a type has its own key, decomp portCarKey; the damage stage, the
 * ice and the turbo flame are per key): its record's slot and type, else the type (no such function) */
static uint32_t car_key_of(int slot, uint32_t type)
{
    static uint32_t key_fn = 1;
    if (key_fn == 1) key_fn = port_sym("portCarKey");
    return key_fn ? guest_call(key_fn, (uint32_t)slot, type & 0xFF, 0, 0) : type;
}

/* the start entry farthest from every live car, the others marked taken for the pick */
static void ai_respawn(int k)
{
    uint32_t rec = AI_REC(k), t = mem_r32(&M.cpu, rec + 0x30);
    /* its own key: with the type, a twin (the second Axel, the second Sweet Tooth) came back as its wreck and the other
     * car of its type was reset instead (the user's single-player take 2026-11-17, until its health went yellow) */
    uint32_t key = car_key_of((int16_t)mem_r16(&M.cpu, rec + 0x006), t);
    if (mem_r8(&M.cpu, rec + 0x518)) { guest_call(GA(0x80124C00u), key, 0, 0, 0); mem_w8(&M.cpu, rec + 0x518, 0); } /* the ice look */
    if (mem_r8(&M.cpu, rec + 0x088)) guest_call(GA(0x80124384u), key + 6, 0, 0, 0);                              /* the turbo flame */
    if (t == 0xF0u) { /* Dark Tooth back: his shot and swoop timers, his swoop and stage flag reset, so he does not swoop the moment he
                       * is back (decomp request 121, PLAYTEST_ANSWERS.md 35.2) */
        static uint32_t reset = 1;
        if (reset == 1) reset = port_sym("portCarResetSpecials");
        if (reset) guest_call(reset, (uint32_t)(int16_t)mem_r16(&M.cpu, rec + 0x006), 0, 0, 0);
    }
    const uint32_t list = GA(0x801B2B28u), taken = GA(0x801B2B46u), nodes = GA(0x801B1DE8u);
    int count = mem_r8(&M.cpu, GA(0x801B2B55u)), humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    if (count > 15) count = 15;
    int best = -1;
    double best_d = -1;
    for (int e = 0; e < count; e++) {
        int n = (int16_t)mem_r16(&M.cpu, list + 2u * (uint32_t)e);
        double nx = (int32_t)mem_r32(&M.cpu, nodes + 24u * (uint32_t)n), ny = (int32_t)mem_r32(&M.cpu, nodes + 24u * (uint32_t)n + 4);
        double nearest = 1e18;
        for (int j = 0; j < humans + ais && (j < humans || j - humans < cars_ai_max()); j++) {
            int human = j < humans;
            uint32_t r = human ? HUMAN_REC(j) : AI_REC(j - humans);
            if ((!human && j - humans == k) || mem_r8(&M.cpu, r + (human ? 0x9Cu : 0x6Cu))) continue;
            uint32_t pos = r + (human ? 0x6FCu : 0x73Cu);
            double dx = (int32_t)mem_r32(&M.cpu, pos) - nx, dy = (int32_t)mem_r32(&M.cpu, pos + 4) - ny;
            if (dx * dx + dy * dy < nearest) nearest = dx * dx + dy * dy;
        }
        if (nearest > best_d) { best_d = nearest; best = e; }
    }
    uint8_t saved[15];
    for (int e = 0; e < count; e++) {
        saved[e] = mem_r8(&M.cpu, taken + (uint32_t)e);
        mem_w8(&M.cpu, taken + (uint32_t)e, (uint8_t)(e != best));
    }
    guest_call(GA(0x800FFEFCu), rec, t, 1, 0);                               /* aiCarStart: a full per-car reset */
    for (int e = 0; e < count; e++) mem_w8(&M.cpu, taken + (uint32_t)e, saved[e]);
    guest_call(GA(0x80124718u), key, 0, mem_r8(&M.cpu, rec + 0x500), 0);    /* the undamaged model */
    guest_call(GA(0x80114E10u), key, 0, 0, 0);                               /* its body object */
    mem_w8(&M.cpu, rec + 0x501, 0);                                      /* the wreck state */
    mem_w16(&M.cpu, rec + 0x010, 0);
    mem_w16(&M.cpu, rec + 0x00E, 0);
    uint32_t obj = mem_r32(&M.cpu, GA(0x80188B38u) + 4u * (uint32_t)k);
    if (obj) guest_call(GA(0x80149604u), obj, 1, 0, 0);                      /* shown and solid again */
    int shield = battle_rules() && !ui_survival() ? battle_rules()->spawn_shield : 2; /* the game type's spawn shield */
    mem_w8(&M.cpu, rec + 0x07A, (uint8_t)(shield > 0));
    ai_life[k].shield_off = vp_gv() + 60u * (uint64_t)shield;
    fprintf(stderr, "lives: computer car %d is back (car key %X)\n", k + 1, (unsigned)key);
}

/* each frame of a Challenge Match: respawns, shields, and whom the computer cars hunt */
void vp_survival_frame(void);
static int mode_is(int mode);
static uint8_t hill_goal[CARS_MAX]; /* King of the Hill: the computer cars going for the hill (below) */
void vp_ai_frame(void)
{
    const GameType *t = battle_rules();
    if (!t) return;
    vp_survival_frame();
    /* Moods (decomp AI_VS_AI.md 7): 0 the start, route ahead of the target; 1 hunt, the only one driving straight at it;
     * 2 away (the route node farthest from the target, mines); 3 a random route node. The game's pack director keeps
     * about three hunting and sends the rest away or roaming, for one player against the pack. In a Challenge Match and
     * Survival (not the tournament: the game's own) every car hunts, and backs off (2) only when badly hurt (under 30%);
     * Juggernaut and Hunted: mode_frame (always 1). King of the Hill: the cars going for the hill 0 (on the route, its
     * aim the hill: vp_AiFollowRouteTarget), the others 1. */
    if (!mode_is(MODE_JUGGERNAUT) && !mode_is(MODE_ODDBALL)) {
        int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
        for (int k = 0; k < n && k < cars_ai_max(); k++) {
            if (!ai_alive(k)) continue;
            int mood = mem_r8(&M.cpu, AI_REC(k) + 0x001), hp = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x5A0),
                max = (int16_t)mem_r16(&M.cpu, AI_REC(k) + 0x536);
            int hurt = max > 0 && hp * 10 < max * 3;
            if (mode_is(MODE_KING_OF_THE_HILL) && hill_goal[k]) { if (mood != 0) mem_w8(&M.cpu, AI_REC(k) + 0x001, 0); }
            else if (mood == 0 || mood == 3 || (mood == 2 && !hurt)) mem_w8(&M.cpu, AI_REC(k) + 0x001, 1);
        }
    }
    for (int k = 0; k < cars_ai_max(); k++) {
        if (ai_life[k].at && vp_gv() >= ai_life[k].at) {
            ai_life[k].at = 0;
            ai_respawn(k);
            void vp_human_back(int h);
            for (int h = 0; h < CARS_HUMANS_MAX; h++) /* (a player back waiting for this car: theirs now) */
                if (back_wait[h] && handed[h] == k + 1) vp_human_back(h);
        }
        if (ai_life[k].shield_off && vp_gv() >= ai_life[k].shield_off) {
            ai_life[k].shield_off = 0;
            mem_w8(&M.cpu, AI_REC(k) + 0x07A, 0);
        }
    }
    if (teams_on()) {
        if (vp_gv() >= team_next) {
            if (vp_gv() > team_next + 600) { /* a new round: who is on which team */
                int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
                for (int k = 0; k < n && k < cars_ai_max(); k++)
                    fprintf(stderr, "teams: computer car %d (type %02x) on team %d\n", k + 1, mem_r32(&M.cpu, AI_REC(k) + 0x30),
                            team_of_ai(k));
            }
            team_next = vp_gv() + 60;
            teams_retarget();
            static uint64_t dbg_next;
            if (getenv("TM2PC_TEAMS_DEBUG") && vp_gv() >= dbg_next) {
                dbg_next = vp_gv() + 600;
                int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
                for (int k = 0; k < n && k < cars_ai_max(); k++)
                    fprintf(stderr, "teams debug: car %d team %d dead %d hp %d target %d dist %d range %d mood %d\n", k + 1,
                            team_of_ai(k), mem_r8(&M.cpu, AI_REC(k) + 0x6C), (int)mem_r32(&M.cpu, AI_REC(k) + 0x5A0),
                            (int)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k), (int)mem_r32(&M.cpu, AI_REC(k) + 0x28),
                            mem_r8(&M.cpu, AI_REC(k) + 0x34), mem_r8(&M.cpu, AI_REC(k) + 1));
            }
        } else if (teams_targets_bad())
            teams_retarget();
        return;
    }
    if (t->ai_targets == AI_TARGETS_GAME) return;
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int k = 0; k < n && k < cars_ai_max(); k++) /* always hurt (not only in range of their own target) */
        if (ai_alive(k)) mem_w8(&M.cpu, AI_REC(k) + 0x034, 1);
    if (t->ai_targets != AI_TARGETS_EQUAL || spectating) return; /* spectating: they hunt each other already */
    if (vp_gv() < equal_next) return;
    equal_next = vp_gv() + 60;
    for (int k = 0; k < n && k < cars_ai_max(); k++) { /* the nearest car of any kind (a human: -1, the nearest human) */
        if (!ai_alive(k)) continue;
        double kx = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x73C), ky = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x740), best_d = 1e30;
        int best = -1;
        for (int j = 0; j < humans && j < 2; j++) {
            if (mem_r8(&M.cpu, HUMAN_REC(j) + 0x9C)) continue;
            double dx = (int32_t)mem_r32(&M.cpu, HUMAN_REC(j) + 0x6FC) - kx, dy = (int32_t)mem_r32(&M.cpu, HUMAN_REC(j) + 0x700) - ky;
            if (gu_skip(k, -1, dx * dx + dy * dy)) continue;
            if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; best = -1; }
        }
        for (int j = 0; j < n && j < cars_ai_max(); j++) {
            if (j == k || !ai_alive(j)) continue;
            double dx = (int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x73C) - kx, dy = (int32_t)mem_r32(&M.cpu, AI_REC(j) + 0x740) - ky;
            if (gu_skip(k, j, dx * dx + dy * dy)) continue;
            if (dx * dx + dy * dy < best_d) { best_d = dx * dx + dy * dy; best = j; }
        }
        mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k, (uint32_t)best);
    }
}

/* The chase camera (viewChaseCamera 80102CC4) aims from the followed car's
 * heading; spectating, the D-pad's turn is added to the watched computer
 * car's yaw (motion.rot[2], +0x750) for the camera alone. */
void F_80102CC4_orig(CPU *c);
/* A player out (2-4 players), each watching the drivers still in with their own view 7: a computer car (its index)
 * or another player, the chase or the cinematic, the D-pad's turn, Triangle. All may watch at once: the game's
 * one watched car (viewWatchedAiCar) is set to each player's while their view's camera runs (vp_watch_view). */
static int wv_view = -1; /* the player whose view's camera is being updated */
int humans_slot(int h);    /* vplus/humans.c: human -> this PC's view (-1: another machine's) */
int humans_of_slot(int L); /* view -> human */
static uint32_t view_rec(int v) { int h = humans_of_slot(v); return HUMAN_REC(h < 0 ? 0 : h); }
void vp_ChaseCamera(CPU *c)
{
    int p = wv_view;
    if (p >= 0 && wv[p].on && !wv[p].cinema && !wv[p].free && wv[p].orbit && wv[p].human < 0 && c->r[4] == view_rec(p)) {
        uint32_t wy = AI_REC(wv[p].ai % cars_ai_max()) + 0x750, ws = mem_r32(c, wy); /* the watcher's chase: turned */
        mem_w32(c, wy, ws + (uint32_t)wv[p].orbit);
        F_80102CC4_orig(c);
        mem_w32(c, wy, ws);
        return;
    }
    F_80102CC4_orig(c);
}

/* ---- Survival: rounds of computer cars ----
 * The run loads up to 9 computer cars (Minion the last). At its start every
 * one is put out of play as a broken-up wreck is (not drawn, not hit, not
 * driven, not counted: decomp AI_LIVES.md 1.3), then each round brings some
 * back (aiCarStart, as a respawn): round r the first r of the others, round
 * SURVIVAL_ROUNDS all of them. "ROUND N" shows for 2 seconds first, with the
 * explosion. A round is cleared when no enemy is left (the game's won test,
 * held back: its won flag cleared); the last one cleared wins the run. */
#define SURVIVAL_ROUNDS 10
#define ROUND_WON_FLAG GA(0x80180FB0u)
static int surv_round, surv_state; /* 0 off, 1 the banner (the round about to start), 2 the round on, 3 cleared (a pause) */
static uint64_t surv_clock_start, surv_clock_end; /* the run's clock (game VBlanks): from its start, stopped at its end */
int vp_survival_seconds(void) /* the run's time so far (or at its end) */
{
    if (!surv_clock_start) return 0;
    uint64_t end = surv_clock_end ? surv_clock_end : vp_gv();
    return end > surv_clock_start ? (int)((end - surv_clock_start) / 60) : 0;
}
static void survival_clock_stop(void) { if (surv_clock_start && !surv_clock_end) surv_clock_end = vp_gv(); }
static uint64_t surv_until;
int vp_survival_round(void) { return surv_round; }
int vp_survival_banner(void) { return ui_survival() && surv_state == 1 ? surv_round : 0; }

static int survival_wave(void) /* the cars of the round */
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    if (n > cars_ai_max()) n = cars_ai_max();
    return surv_round >= SURVIVAL_ROUNDS ? n : (surv_round < n - 1 ? surv_round : n - 1);
}
int vp_survival_waiting(void) /* the round's cars while "ROUND N" shows: counted as enemies */
{
    return ui_survival() && surv_state == 1 ? survival_wave() : 0;
}

static void ai_out_of_play(int k)
{
    uint32_t rec = AI_REC(k);
    mem_w32(&M.cpu, rec + 0x5A0, 0);   /* HP */
    mem_w8(&M.cpu, rec + 0x6C, 1);     /* dead */
    mem_w8(&M.cpu, rec + 0x501, 4);    /* broken up: not updated */
    mem_w8(&M.cpu, rec + 0x034, 0);    /* out of range: no engine sound, no damage (decomp SURVIVAL.md 3.1) */
    mem_w32(&M.cpu, rec + 0x744, (uint32_t)-1000000); /* far below: Mr. Slam, the tornado and fires test distance only */
    mem_w8(&M.cpu, rec + 0x086, 0);    /* not burning */
    uint32_t obj = mem_r32(&M.cpu, GA(0x80188B38u) + 4u * (uint32_t)k);
    if (obj) guest_call(GA(0x80149604u), obj, 0, 0, 0);
}
/* co-op Survival: a player who is out waits for the next round (the other plays
 * on); both out: the run is over. Their car stays a wreck until then. */
static int surv_down[CARS_HUMANS_MAX]; /* co-op: the players out until the next round (by human) */
/* A player out (wv[]) watches any driver still in: a computer car
 * (viewWatchedAiCar), or the other player (wv[].human). View 7's camera
 * reads the watched car through aiGetWatchedCar (8011D128) in the computer
 * cars' layout (motion +0x71C, state +0x06C, stats +0x4FC); for a player the
 * port returns a copy of their blocks in that layout (WATCH_SHADOW). */
#define WATCH_SHADOW 0x80DF0000u /* 0x854 bytes of spare RAM */
int vp_hud_hidden(int player) /* this player's HUD hidden while they watch (Triangle), and in the kill cam */
{
    if (killcam_replaying) return 1;
    return player >= 0 && player < 4 && wv[player].on && wv[player].hide >= 1;
}
/* a player spectating (out, watching another car): their HUD shows their own wreck's numbers, so the radar, the weapons
 * and the health, energy and turbo are left out (hud_layout.c, hud_styles.c); the message line ("RESPAWNING IN N"),
 * the score line and the kill feed stay */
int vp_watch_hud(int player)
{
    if (killcam_replaying) return 0;
    return player >= 0 && player < 4 && wv[player].on;
}
int vp_watching(int player) /* player watching in split screen (their banner): 0 no (or hidden), 1 chase, 2 cinematic */
{
    if (player < 0 || player > 3 || !wv[player].on || wv[player].hide >= 2 || one_view()) return 0; /* (one view: vp_spectating's) */
    return 1 + wv[player].cinema;
}
static int watch_eligible_human(int p, int o) /* player o, for view p's player to watch: driving */
{
    return o >= 0 && o < CARS_HUMANS_MAX && o != humans_of_slot(p) && (int)mem_r32(&M.cpu, GA(0x80181014u)) > o && !vp_player_out(o) &&
           !mem_r8(&M.cpu, HUMAN_REC(o) + 0x9C);
}
static void watch_step(int p, int dir) /* player p: the next (dir 1) or previous driver in */
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), cand[CARS_HUMANS_MAX + CARS_MAX], nc = 0, cur, at = -1;
    for (int o = 0; o < CARS_HUMANS_MAX; o++) if (watch_eligible_human(p, o)) cand[nc++] = 100 + o; /* the players first */
    for (int k = 0; k < n && k < cars_ai_max() && nc < (int)(sizeof cand / sizeof cand[0]); k++) if (ai_alive(k)) cand[nc++] = k;
    if (!nc) return;
    cur = wv[p].human >= 0 ? 100 + wv[p].human : wv[p].ai;
    for (int j = 0; j < nc; j++) if (cand[j] == cur) at = j;
    int next = at < 0 ? cand[dir > 0 ? 0 : nc - 1] : cand[((at + dir) % nc + nc) % nc];
    if (next >= 100) wv[p].human = next - 100;
    else { wv[p].human = -1; wv[p].ai = next; }
    if (getenv("TM2PC_WATCH_DEBUG")) fprintf(stderr, "watch: view %d now %s %d\n", p + 1, next >= 100 ? "player" : "computer car", (next >= 100 ? next - 100 : next) + 1);
}
static void watch_begin_view(int i, int h);
static void watch_begin(int h) /* player h out: their view follows another player first, else a computer car */
{
    watch_begin_view(humans_slot(h), h); /* (their view: none for another machine's player) */
}
static void watch_begin_view(int i, int h) /* view i (its car human h's record) follows another player, else a computer car */
{
    if (i < 0 || i > 3) return;
    wv[i].on = 1;
    wv[i].prev = 0xFFFF;
    wv[i].human = -1;
    wv[i].cinema = wv[i].free = wv[i].orbit = wv[i].hide = 0;
    wv[i].ai = 0;
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < n && k < cars_ai_max(); k++) if (ai_alive(k)) { wv[i].ai = k; break; }
    for (int o = 0; o < CARS_HUMANS_MAX && wv[i].human < 0; o++) if (watch_eligible_human(i, o)) wv[i].human = o;
    guest_call(GA(0x800E7380u), HUMAN_REC(h), 7, 0, 0); /* carSetViewMode: the watched car */
    fprintf(stderr, "watch: player %d (view %d) watches %s %d (at %llu)\n", h + 1, i + 1, wv[i].human >= 0 ? "player" : "computer car",
            (wv[i].human >= 0 ? wv[i].human : wv[i].ai) + 1, (unsigned long long)vp_gv());
}
/* online: a machine in to watch the match on (no players of its own, the user's 2026-11-11: in from the next match): its
 * one view (player 1's car's record, humans_view_human) watches the drivers as a player out does, from the battle's
 * first step here; the host's state (its view bytes: the host's own) puts the watched car's view back again */
int netplay_watching(void); /* net/game_tm2.c */
int humans_view_human(int view); /* vplus/humans.c */
static void watch_end_one(int p);
static void watcher_frame(void)
{
    static int was;
    if (netplay_watching() && !wv[0].on && feats_in_battle()) watch_begin_view(0, humans_view_human(0));
    if (netplay_watching()) was = 1;
    else if (was && humans_view_human(0) < (int)mem_r32(&M.cpu, GA(0x80181014u))) { /* (in now: its own car, once there) */
        was = 0;
        watch_end_one(0);
    }
}
void vp_watch_again(void) /* (net/game_tm2.c) a state from the host in place */
{
    if (netplay_watching() && wv[0].on) guest_call(GA(0x800E7380u), HUMAN_REC(humans_view_human(0)), 7, 0, 0); /* carSetViewMode */
}
/* four.c's viewCarCameraUpdate wrap, around player i's view's camera: the game's watched car is theirs */
void vp_watch_view(int i, int enter)
{
    static uint32_t saved;
    if (enter) {
        wv_view = i >= 0 && i < 4 ? i : -1;
        if (wv_view >= 0 && wv[i].on) {
            saved = mem_r32(&M.cpu, VIEW_WATCHED_AI);
            mem_w32(&M.cpu, VIEW_WATCHED_AI, (uint32_t)wv[i].ai);
        }
    } else {
        if (wv_view >= 0 && wv[wv_view].on) mem_w32(&M.cpu, VIEW_WATCHED_AI, saved);
        wv_view = -1;
    }
}
void F_8011D128_orig(CPU *c);
void vp_GetWatchedCar(CPU *c) /* aiGetWatchedCar(): the record view 7 follows */
{
    F_8011D128_orig(c);
    int p = wv_view;
    if (p < 0 || !wv[p].on || wv[p].human < 0) return;
    uint32_t h = HUMAN_REC(wv[p].human);
    for (uint32_t o = 0; o < 0xEC; o += 4) mem_w32(c, WATCH_SHADOW + 0x71C + o, mem_r32(c, h + 0x6DC + o)); /* motion */
    for (uint32_t o = 0; o < 0x30; o += 4) mem_w32(c, WATCH_SHADOW + 0x06C + o, mem_r32(c, h + 0x09C + o)); /* state */
    for (uint32_t o = 0; o < 0xE8; o += 4) mem_w32(c, WATCH_SHADOW + 0x4FC + o, mem_r32(c, h + 0x14C + o)); /* stats */
    mem_w32(c, WATCH_SHADOW + 0x30, mem_r32(c, h + 0x90)); /* type */
    if (!wv[p].cinema && !wv[p].free) mem_w32(c, WATCH_SHADOW + 0x750, mem_r32(c, WATCH_SHADOW + 0x750) + (uint32_t)wv[p].orbit); /* turned around it */
    c->r[2] = WATCH_SHADOW;
}
static void respawn_human(int i);
int vp_survival_player_down(void) /* co-op (2-4 players): a player out waits for the next round; all out: the run's end */
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (humans < 2 || humans > CARS_HUMANS_MAX) return 0;
    int alive = 0, out[CARS_HUMANS_MAX], nout = 0;
    for (int i = 0; i < humans; i++) {
        if (mem_r8(&M.cpu, HUMAN_REC(i) + 0x9C)) {
            if (!surv_down[i]) { surv_down[i] = 1; out[nout++] = i; fprintf(stderr, "survival: player %d is out until the next round\n", i + 1); }
        } else {
            alive++;
        }
    }
    for (int j = 0; alive && j < nout; j++) watch_begin(out[j]); /* their views follow the drivers in (X next, O back) */
    return alive > 0;
}
/* VP_COOP_SPECTATE: the co-op tournament's last life is gone and player i is
 * destroyed; while the other still drives, i watches the computer cars (as
 * co-op Survival: X next, O back) and the level goes on. 0: the round ends. */
static int coop_player_out(int i, uint32_t delay) /* (delay: the death's, the game's VBlanks: the wreck seen first) */
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), other = -1;
    for (int j = 0; j < humans && j < CARS_HUMANS_MAX && other < 0; j++) /* another player still driving */
        if (j != i && !coop_out[j] && !mem_r8(&M.cpu, HUMAN_REC(j) + 0x9C)) other = j;
    if (!vp_on(VP_COOP_SPECTATE) || other < 0) return 0;
    if ((int)mem_r32(&M.cpu, GA(0x8018101Cu)) < 1) return 0;
    coop_out[i] = 1;
    /* their wreck first, the game's delay, then the view follows the drivers in (watch_frame), as a life lost does (it
     * went to the watching at once: the user's online co-op take) */
    watch_at[i] = vp_gv() + (delay > 0 && delay < 600 ? delay : 180);
    fprintf(stderr, "lives: player %d is out of lives: watching (player %d plays on)\n", i + 1, other + 1);
    return 1;
}
static void watch_frame_one(int p);
static void survival_watch_frame(void) /* the players out: their buttons pick the car watched */
{
    for (int p = 0; p < 4; p++) if (wv[p].on) watch_frame_one(p);
}
static void watch_frame_one(int p)
{
    uint16_t input_player_bits(int p); /* runtime/input.c: players 3-4's PS1 buttons (set = held) */
    uint16_t pad = p < 2 ? (uint16_t)~M.pad_buttons[p] : input_player_bits(p), pressed = (uint16_t)(pad & ~wv[p].prev);
    wv[p].prev = pad;
    int lost = wv[p].human >= 0 ? !watch_eligible_human(p, wv[p].human) : !ai_alive(wv[p].ai);
    /* cinematic: the kill cam's shots on the car and the one it is after, a new shot every 4 seconds and a new car every
     * 8; the D-pad turns the chase camera around the car; Triangle the HUD, then the banner too, then both back */
    if (pressed & 0x1000) wv[p].hide = (wv[p].hide + 1) % 3;
    if (!wv[p].cinema && !wv[p].free) {
        if (pad & 0x0080) wv[p].orbit += 48;
        if (pad & 0x0020) wv[p].orbit -= 48;
        if (pressed & (0x4000 | 0x2000)) wv[p].orbit = 0;
    }
    /* Square cycles the cameras: chase, cinematic, free, chase (split screen: no free camera, chase and cinematic); X the
     * next driver, O the one before, in every camera (the free camera: back to the chase camera on that driver). The
     * user's 2026-11-24: cinematic to chase took Square, then X */
    if (wv[p].free && (pressed & (0x4000 | 0x2000 | 0x8000))) {
        wv[p].free = 0;
        wv[p].cinema = 0;
        wv[p].switch_at = vp_gv() + 480;
        pressed &= (uint16_t)~0x8000; /* (X / O: the driver below as well) */
    } else if (pressed & 0x8000) {
        if (!wv[p].cinema) wv[p].cinema = 1; /* (the view stays the chase view 7: killcam_camera puts the shots over it) */
        else if (!one_view()) wv[p].cinema = 0;
        else { /* the free camera: from behind the car watched */
            void photo_freecam_begin(double cx, double cy, double cz, int heading); /* ui/photo.c */
            int32_t x, y, z;
            int hd;
            if (watched_pos(p, &x, &y, &z, &hd)) {
                photo_freecam_begin(x, y, z, hd);
                wv[p].cinema = 0;
                wv[p].free = 1;
            } else wv[p].cinema = 0; /* (nothing to start it from: the chase camera) */
        }
        wv[p].switch_at = vp_gv() + 480;
        pressed = 0;
    }
    if (wv[p].free) return; /* (the free camera: the car watched stays as it was) */
    if (lost || (pressed & 0x4000) || (wv[p].cinema && vp_gv() >= wv[p].switch_at)) {
        watch_step(p, 1);
        wv[p].switch_at = vp_gv() + 480;
    } else if (pressed & 0x2000) {
        watch_step(p, -1);
        wv[p].switch_at = vp_gv() + 480;
    }
}
static void watch_end_one(int p) /* view p's player back: their own view (view_last) */
{
    if (p < 0 || p > 3 || !wv[p].on) return;
    uint32_t rec = view_rec(p);
    guest_call(GA(0x800E7380u), rec, mem_r32(&M.cpu, rec + 0x110), 0, 0);
    wv[p].on = 0;
    wv[p].human = -1;
}
static void survival_watch_end(void) { for (int p = 0; p < 4; p++) watch_end_one(p); }
int vp_versus_parked(int h); /* (below, with the Versus Tournament: h parked out of the boss round) */
static void watch_frame(void) /* the delayed starts of watching */
{
    watcher_frame();
    if (spectate_at && vp_gv() >= spectate_at) {
        spectate_at = 0;
        if (!spectate_begin()) { /* nobody left to watch meanwhile: the round's end, as the game's */
            vp_port_event = 1;
            guest_call(GA(0x800D181Cu), 1, 30, 0, 0);
        }
    }
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    /* a player of this machine parked out of a Versus boss round, not watching yet: watching now, once a boss round.
     * The host parks them and its next step clears their watch_at; a stage starts from the host's state, so the
     * machine of a parked player never saw it set and its view stayed where it was (the user's 2026-11-14 take) */
    static uint32_t parked_started;
    for (int i = 0; i < CARS_HUMANS_MAX && i < humans; i++) {
        int s = humans_slot(i);
        if (!vp_versus_parked(i)) { parked_started &= ~(1u << i); continue; }
        if (s < 0 || s >= 4 || wv[s].on || watch_at[i] || ((parked_started >> i) & 1)) continue;
        parked_started |= 1u << i;
        fprintf(stderr, "watch: player %d parked out of the boss round: watching\n", i + 1);
        watch_begin(i);
    }
    for (int i = 0; i < CARS_HUMANS_MAX && i < humans; i++) {
        if (!watch_at[i] || vp_gv() < watch_at[i]) continue;
        watch_at[i] = 0;
        if (coop_out[i] || life[i].at) { /* out (2 players), or a respawn to wait for */
            if (humans == 1) { if (life[i].at) spectate_begin(); }
            else if (humans_slot(i) >= 0 && humans_slot(i) < 4 && !wv[humans_slot(i)].on) watch_begin(i);
        }
    }
}
static void watch_end_one(int p);
static void spectate_stop(void) /* 1 player back (a respawn): their own view again */
{
    spectating = 0;
    port_config_sync();
    watch_end_one(0); /* (their own view: view_last) */
}
static void watch_end_for(int i)
{
    watch_at[i] = 0;
    if (i == 0 && spectating) spectate_stop();
    watch_end_one(humans_slot(i));
}
void killcam_new_round(void);
static void survival_banner(void)
{
    survival_watch_end();
    killcam_new_round(); /* each round's last kill its kill cam */
    for (int i = 0; i < CARS_HUMANS_MAX; i++) /* co-op: the players who were out come back */
        if (surv_down[i]) { surv_down[i] = 0; if (surv_round > 1) respawn_human(i); }
    surv_state = 1;
    surv_until = vp_gv() + (surv_round >= 2 ? 180 : 120); /* "ROUND N" 3 seconds before the cars (the first: 2) */
    guest_call(GA(0x800D3334u), 0x1D, 0, 0, 99); /* the explosion */
    fprintf(stderr, "survival: round %d (vblank %llu)\n", surv_round, (unsigned long long)vp_gv());
}
/* Out of a match (the globe, the cards, the menus): nobody watches. The watch was only ended at the next match's start,
 * so after a lost level's RESTART a player out watching with the cinematic camera still had it hooked into view 0: the
 * globe was drawn from the kill cam's shot (shifted left, the scope over empty space) and the CINEMATIC legend stayed over
 * the cards (the user's online co-op take 2026-11-16, 7:07). The views are the game's own again at the next load; no
 * guest call here (no match to call into). Local state only (not in the rollback's blocks). */
void vp_watch_out_of_battle(void)
{
    if (feats_in_battle() || !vp_spectating_any()) return;
    for (int p = 0; p < 4; p++) { wv[p].on = wv[p].cinema = wv[p].free = wv[p].hide = 0; wv[p].human = -1; }
    fprintf(stderr, "spectate: ended with the match\n");
}
void vp_survival_start(void) /* the run's (or a retry's) start */
{
    surv_state = 0;
    memset(surv_down, 0, sizeof surv_down);
    survival_watch_end(); /* (each watching player's own view back first: a round with no new load, Dark Tooth's second
                           * phase, left an out player's view on the game's watched computer car, him: player 4 seemed
                           * to drive Dark Tooth, the user's co-op take 2026-11-17) */
    for (int p = 0; p < 4; p++) { wv[p].on = wv[p].cinema = wv[p].free = wv[p].hide = 0; wv[p].human = -1; }
    if (!ui_survival()) return;
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < n && k < cars_ai_max(); k++) ai_out_of_play(k);
    surv_round = getenv("TM2PC_TEST_SURV_ROUND") ? atoi(getenv("TM2PC_TEST_SURV_ROUND")) : 1; /* (tests: from this round) */
    surv_clock_start = vp_gv() ? vp_gv() : 1;
    surv_clock_end = 0;
    survival_banner();
    guest_call(GA(0x800FBB6Cu), 0, 0, 0, 0); /* hudMessageReset: "Enemies: N" (not "Enemies Toasted!") */
}
static void dm_frame(void);
void vp_survival_frame(void)
{
    boss_frame();
    dm_frame();
    static int dbg = -1;
    static uint64_t dbg_next;
    if (dbg < 0) dbg = getenv("TM2PC_SURV_DEBUG") != NULL;
    if (dbg && ui_survival() && vp_gv() >= dbg_next && (dbg_next = vp_gv() + 300))
        for (int k = 0; k < (int)mem_r32(&M.cpu, GA(0x8018101Cu)) && k < cars_ai_max(); k++) {
            uint32_t r = AI_REC(k);
            fprintf(stderr, "survival debug: car %d type %X dead %d wreck %d/%d mood %d", k + 1, (unsigned)mem_r32(&M.cpu, r + 0x30),
                    mem_r8(&M.cpu, r + 0x6C), mem_r8(&M.cpu, r + 0x501), (int)(int16_t)mem_r16(&M.cpu, r + 0x522), mem_r8(&M.cpu, r + 1));
            fprintf(stderr, " left %d boss %d", (int)mem_r32(&M.cpu, GA(0x80181020u)), (int)mem_r32(&M.cpu, GA(0x8018102Cu)));
            fprintf(stderr, " dist %d hp %d range %d pos %d,%d target %d f000 %d\n",
                    (int)mem_r32(&M.cpu, r + 0x28), (int)mem_r32(&M.cpu, r + 0x5A0),
                    mem_r8(&M.cpu, r + 0x34), (int)mem_r32(&M.cpu, r + 0x73C), (int)mem_r32(&M.cpu, r + 0x740),
                    (int)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k), mem_r8(&M.cpu, r));
        }
    /* co-op: the game's won test (worldUpdate) asks for human 0 alive (!humanCars[0].f_09C): with player 1 out until the
     * next round, the others clearing the wave left "Enemies Toasted" up and no next round (Blues's and Slick!'s online
     * Survival, 2026-11-23). The round won here instead, as the game would: its flag, then rtScheduleEvent(3) (through
     * vp_ScheduleEvent: the next round, or the last round's win) */
    if (ui_survival() && surv_state == 2 && mem_r8(&M.cpu, HUMAN_REC(0) + 0x9C) && (int)mem_r32(&M.cpu, AI_REMAINING) <= 0 &&
        !mem_r32(&M.cpu, ROUND_WON_FLAG)) {
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), alive = 0;
        for (int i = 1; i < humans && i < CARS_HUMANS_MAX; i++) alive |= !mem_r8(&M.cpu, HUMAN_REC(i) + 0x9C);
        if (alive) {
            mem_w32(&M.cpu, ROUND_WON_FLAG, 1);
            fprintf(stderr, "survival: round %d cleared with player 1 out\n", surv_round);
            guest_call(GA(0x800D181Cu), 3, 420, 0, 0); /* rtScheduleEvent */
            return;
        }
    }
    if (ui_survival() && surv_state == 3 && vp_gv() >= surv_until) { /* the round cleared a while ago: the next one's banner */
        surv_round++;
        survival_banner();
        return;
    }
    if (!ui_survival() || surv_state != 1 || vp_gv() < surv_until) return;
    int count = survival_wave(), n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    if (n > cars_ai_max()) n = cars_ai_max();
    surv_state = 2;
    /* which: at random from the cars loaded (Minion, the last, only when all come: the last round) */
    int pick[CARS_MAX], m = surv_round >= SURVIVAL_ROUNDS ? n : n - 1;
    for (int k = 0; k < m; k++) pick[k] = k;
    for (int k = m - 1; k > 0; k--) { int j = (int)(vp_rand() % (uint32_t)(k + 1)), t = pick[k]; pick[k] = pick[j]; pick[j] = t; }
    for (int k = 0; k < count && k < m; k++) ai_respawn(pick[k]);
    guest_call(GA(0x800FBB6Cu), 0, 0, 0, 0); /* hudMessageReset: "Enemies: N" */
}
/* A custom tournament's bosses (ui/options.c): the opponents from boss_first on wait out of play until the others are
 * out; then, 2 seconds later, they come in (as Survival's waves, ai_respawn) and the stage is won once they are out too */
static int boss_first = -1, boss_state; /* state: 0 none, 1 waiting, 2 coming (boss_until), 3 in */
static uint64_t boss_until;
void vp_boss_wave(int first) { boss_first = first; } /* ui: -1 none */
static void boss_round_start(void)
{
    boss_state = 0;
    if (boss_first < 0) return;
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = boss_first; k < n && k < cars_ai_max(); k++) ai_out_of_play(k);
    boss_state = n > boss_first ? 1 : 0;
}
static int boss_hold(void) /* the others are out: 1 when the bosses come now (the round goes on) */
{
    if (boss_state != 1) return 0;
    boss_state = 2;
    boss_until = vp_gv() + 120;
    fprintf(stderr, "custom tournament: the boss is coming\n");
    return 1;
}
/* the bosses on their way in (the 2 seconds before boss_frame brings them): enemies still to come. Out of play until then,
 * they were not counted, so the game's own won test fired again the next frame and the stage ended a few seconds after
 * they came in (Kharmisle's and shuriken8147's custom tournaments, 2026-11-24) */
static int boss_coming(void)
{
    if (boss_state != 2 || boss_first < 0) return 0;
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    if (n > cars_ai_max()) n = cars_ai_max();
    return n > boss_first ? n - boss_first : 0;
}
static void boss_frame(void)
{
    if (boss_state != 2 || vp_gv() < boss_until) return;
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = boss_first; k < n && k < cars_ai_max(); k++) ai_respawn(k);
    boss_state = 3;
    snprintf(flash_text, sizeof flash_text, "BOSS BATTLE");
    flash_until = vp_gv() + 150;
    guest_call(GA(0x800FBB6Cu), 0, 0, 0, 0); /* hudMessageReset: "Enemies: N" */
    fprintf(stderr, "custom tournament: the boss is in\n");
}
int vp_survival_cleared(void) /* the won test fired: 1 held back */
{
    if (surv_state == 2 && surv_round >= SURVIVAL_ROUNDS) return 0; /* the last round: the run is won */
    mem_w32(&M.cpu, ROUND_WON_FLAG, 0);
    if (surv_state == 2) { /* 4 seconds of room, then the next round's banner (vp_survival_frame) */
        surv_state = 3;
        surv_until = vp_gv() + 240;
    }
    return 1;
}

/* "ROUND N" in the pause menu's font (6), printed by the game after the
 * one-view HUD (hudDrawSingleView 800F8370 ends with its DrawOTag), as the
 * pause menu prints "PAUSED": fontPrintCenteredXY 801136B8 into the frame. */
static void mode_status_print(CPU *c, int p, int x, int y);
static void score_line_print(CPU *c, int p, int x, int y, int split);
static int score_player; /* the player whose one-view HUD the game draws (vp_HudDraw): the score line's */
static int score_text(int p, char *out, size_t n);
static void score_dash_raise(uint32_t sprites, const char *text);
static void timer_print(CPU *c, int p, int x, int y);
static void hud_extra_print(CPU *c, int p, int x, int right, int big_y, int small);
void hudlay_prescale(int x, int y, float k); /* hud_layout.c */
void hudlay_force(int group);
/* "ROUND N" in the pause font (6), centred on x of the view's own places, its top at y, scaled k about that point:
 * its letters as sprites in an order table of our own, drawn through the layout (as the HUD's counts,
 * hud_styles.c number()), so in a split or quarter view it is the view's middle and smaller (fontPrint draws
 * straight to the screen, past the layout: in a quarter it came out too big and off its middle) */
#define BANNER_OT 0x80DF6C00u      /* (high RAM: an order table of 2, */
#define BANNER_SPRITES 0x80DF6C10u /*  then the draw mode and 20 bytes a letter) */
float hudlay_wk(void); /* hud_layout.c: widescreen's squeeze of the view's x */
static void survival_banner_print(CPU *c, int x, int y, float k)
{
    int round = vp_survival_banner();
    if (!round) return;
    x = (int)(x * hudlay_wk() + 0.5f); /* (the view's x in widescreen: G_VIEW divides by the squeeze, so its middle is x * wk) */
    char text[16];
    int len = snprintf(text, sizeof text, "ROUND %d", round);
    const uint32_t buf = GA(0x80191074u); /* textFormatBuffer */
    for (int i = 0; i <= len; i++) mem_w8(c, buf + (uint32_t)i, (uint8_t)text[i]);
    const int font = 6;
    int w = (int)guest_call(GA(0x8011355Cu), (uint32_t)font, buf, 0, 0); /* fontStringWidth */
    for (uint32_t o = 0; o < 12; o += 4) mem_w32(c, BANNER_SPRITES + o, mem_r32(c, GA(0x801871E4u) + 12u * (uint32_t)font + o)); /* fontDrawModes[6] */
    hudlay_force(G_VIEW);
    hudlay_prescale(x, y, k);
    guest_call(GA(0x80141938u), BANNER_OT, 2, 0, 0); /* ClearOTagR */
    guest_call7(GA(0x801139D8u), (uint32_t)font, (uint32_t)(x - w / 2), (uint32_t)y, BANNER_SPRITES, BANNER_SPRITES + 12, buf, BANNER_OT); /* fontSpritePrintXY */
    guest_call(GA(0x80141A30u), BANNER_OT + 4, 0, 0, 0); /* DrawOTag */
    hudlay_prescale(0, 0, 1);
    hudlay_force(-1);
}
/* co-op Survival: the split screens' HUDs (hudDrawSplitVertical 800F9084, hudDrawSplitHorizontal
 * 800F9D48; each player's view drawn in its own half) */
/* The split HUDs print no top message (retail: one message line, ticked by
 * the one-view HUD only). Here each half gets its own, as the one-view HUD's
 * (font 2, its green): a message posted for that player (a pickup, "Weapon
 * Bay Full", a combo: hudMessagePost while their pickup or buttons are
 * handled) for 2.5 seconds, else with computer cars "Enemies: N" (or
 * "Enemies Toasted!"). Survival: its own (hudMessageCurrent). */
static int duo_enemies(int p);
int vp_msg_player = -1; /* whose pickup or buttons are being handled (feat_Pickup, vp_MoveCodes), -1 none */
static struct { int id; uint64_t until; } split_msg[4];
void F_800FBBC4_orig(CPU *c);
void vp_HudMessagePost(CPU *c) /* hudMessagePost(message, priority) */
{
    int id = (int)c->r[4];
    F_800FBBC4_orig(c);
    if (id < 13 || id >= 37) return; /* the enemies' count: from the cars left */
    for (int p = 0; p < 4; p++)
        if (vp_msg_player == -1 || vp_msg_player == p) { split_msg[p].id = id; split_msg[p].until = vp_gv() + 150; }
}
static const char *port_message(int p);
static void hud_add_prim(CPU *c, uint32_t ot, uint32_t prim) /* libgpu addPrim */
{
    uint32_t tag = mem_r32(c, prim), head = mem_r32(c, ot);
    mem_w32(c, prim, (tag & 0xFF000000u) | (head & 0x00FFFFFFu));
    mem_w32(c, ot, (head & 0xFF000000u) | (prim & 0x00FFFFFFu));
}
/* The message line as the one-view HUD draws it: the text in its sprite
 * font (fontSpritePrintCenteredXY 80113A14, font 2 in the HUD's green) over
 * the record's bar (hudPlayerRecords[p]: the POLY_F4 and its DR_MODE, the
 * semi-transparent box 84..214 x 19..34 around text at 147, 30), as wide as
 * the text needs. The split HUD has its own copies per player in spare RAM:
 * the bar at DM_TEXT + 0x600, the text at + 0x680, its sprites at + 0x700. */
#define MSG34 0x80DD1400u /* players 3-4's message bar, text and sprites (0x300 each; high RAM, vplus/four.c's area) */
static uint32_t msg_text_buf(int p) { return p < 2 ? DM_TEXT + 0x680 + 0x40u * (uint32_t)p : MSG34 + 0x300u * (uint32_t)(p - 2) + 0x30; }
/* the message line's letters: the game's 30 sprites (hudMessageTextSprites[30]; the split HUD's copies the same 30).
 * A longer text wrote its letters past them, over hudLivesTextSprites ("Lives: N"), whose links then sent the HUD's
 * walk through the port's high RAM and RAM's first page: garbage VRAM copies every frame, 4 frames a second, once a
 * loop (decomp request 105: "SCOTT CAMPBELL LEFT: CPU DRIVES ON", 34). Every port message is cut to 30 */
enum { MSG_MAX = 30 };
static void message_line_draw(CPU *c, int p, int x, int y, uint32_t str)
{
    p &= 3;
    /* the split HUD has drawn its order table already (DrawOTag at its end): one of our own,
     * two entries at DM_TEXT + 0x660 (ClearOTagR 80141938, DrawOTag 80141A30) */
    const uint32_t ot = DM_TEXT + 0x660;
    uint32_t rec = RB_hudPlayerRecords + 1900u * (uint32_t)p;
    uint32_t bar = p < 2 ? DM_TEXT + 0x600 + 0x30u * (uint32_t)p : MSG34 + 0x300u * (uint32_t)(p - 2);
    uint32_t sprites = p < 2 ? DM_TEXT + 0x700 + 0x270u * (uint32_t)p : MSG34 + 0x300u * (uint32_t)(p - 2) + 0x70;
    guest_call(GA(0x80141938u), ot, 2, 0, 0);
    for (uint32_t o = 0; o < 612; o += 4) mem_w32(c, sprites + o, mem_r32(c, GA(0x80181E28u) + o)); /* hudMessageTextMode */
    uint32_t font_was = vp_font_color_get(2);
    guest_call(GA(0x80114284u), 2, 36, 150, 36);                                                   /* fontSetColor */
    int down = settings_video.hud_msg_classic ? 0 : 1; /* centered: the letters a unit down (the bar level with the radar) */
    guest_call7(GA(0x80113A14u), 2, (uint32_t)x, (uint32_t)(y + down), sprites, sprites + 12, str, ot);
    vp_font_color_put(2, font_was);
    int w = (int)guest_call(GA(0x8011355Cu), 2, str, 0, 0), hw = w / 2 + 8 > 65 ? w / 2 + 8 : 65; /* fontStringWidth */
    for (uint32_t o = 0; o < 36; o += 4) mem_w32(c, bar + o, mem_r32(c, rec + o));
    int cx = x + 2;
    const int16_t xy[8] = {(int16_t)(cx - hw), (int16_t)(y - 11), (int16_t)(cx + hw), (int16_t)(y - 11),
                           (int16_t)(cx - hw), (int16_t)(y + 4), (int16_t)(cx + hw), (int16_t)(y + 4)};
    for (int i = 0; i < 8; i++) mem_w16(c, bar + 8 + 2u * (uint32_t)i, (uint16_t)xy[i]);
    hud_add_prim(c, ot, bar);       /* drawn under the text (added after it) */
    hud_add_prim(c, ot, bar + 24);  /* its semi-transparency first */
    guest_call(GA(0x80141A30u), ot + 4, 0, 0, 0);
}
static void split_message_print(CPU *c, int player, int x, int y)
{
    if (settings_video.hud_no_messages) return;
    const char *mine = port_message(player);
    if (mine) {
        uint32_t buf = msg_text_buf(player & 3);
        int i = 0;
        for (; i < MSG_MAX && mine[i]; i++) mem_w8(c, buf + (uint32_t)i, (uint8_t)mine[i]);
        mem_w8(c, buf + (uint32_t)i, 0);
        message_line_draw(c, player, x, y, buf);
        return;
    }
    int id;
    if (ui_survival()) id = (int)mem_r32(c, GA(0x80183F00u));   /* hudMessageCurrent[0] */
    else if (vp_gv() < split_msg[player & 3].until) id = split_msg[player & 3].id;
    else if ((mem_r32(c, GA(0x8018101Cu)) > 0 || mem_r32(c, GA(0x80181014u)) > 1) && !vp_dm()) { /* the cars still to beat */
        int left = vp_duo() || mem_r32(c, GA(0x8018101Cu)) == 0 ? duo_enemies(player & 3) : (int)mem_r32(c, AI_REMAINING);
        id = left < 0 ? 0 : left > 12 ? 12 : left;
    } else return;
    if (id < 0 || id >= 37) return;
    uint32_t str = mem_r32(c, GA(0x8016286Cu) + 4u * (uint32_t)id); /* hudMessageStrs */
    if (!str) return;
    message_line_draw(c, player, x, y, str);
}
void F_800F9084_orig(CPU *c);
int hudlay_adaptive(void);                 /* hud_layout.c */
void hudlay_begin(int source, int view_w, int view_h);
void hudlay_source(int source);
void hudlay_force(int group);
void hudlay_player(int p);
void hudlay_mirror(void);
void hudstyle_draw(int player, int view_w, int view_h); /* hud_styles.c */
void hudstyle_marks(int player, int view_w, int view_h); /* hud_styles.c: the markers, the reticle, the hill */
void hudstyle_tm2_turbo(int player, int src);
void hudlay_end(void);
void hudlay_dividers(int kind);
int vp_hud_hidden(int player);
void four_hud_done(int who); /* vplus/four.c: after a view's HUD: views 3-4 and the mirrors when their turn comes */
static void vp_HudDraw_body(CPU *c);
static void vp_HudDrawSplitVertical_body(CPU *c);
static void vp_HudDrawSplitHorizontal_body(CPU *c);
static void vp_HudDrawSplitVertical_body(CPU *c)
{
    int player = (int)(c->r[4] & 1); /* before the call: it changes the registers */
    hud_select(player);
    if (vp_hud_hidden(player)) { if (player == 0) hudlay_dividers(1); four_hud_done(player); return; } /* watching: hidden (Triangle) */
    if (hudlay_adaptive()) { /* laid out for its view (hud_layout.c); the port's lines where the canonical layout has them */
        hudlay_begin(1, 160, 240);
        hudlay_player(player);
        F_800F9084_orig(c);
        hudlay_force(G_TM);
        split_message_print(c, player, 77, 16);
        mode_status_print(c, player, 79, 28);
        timer_print(c, player, 77, 28); /* (the bar's centre: hud_layout.c's tm_c) */
        hudlay_force(G_BL);
        score_line_print(c, player, 11, 222, 1);
        hudlay_force(-1);
        hud_extra_print(c, player, 79, 156, 150, 1);
        hudlay_force(G_C);
        survival_banner_print(c, 80, 90, 0.75f);
        hudstyle_draw(player, 160, 240);
        hudstyle_tm2_turbo(player, 1);
        hudstyle_marks(player, 160, 240);
        hudlay_end();
        if (player == 0) hudlay_dividers(1);
        four_hud_done(player); /* (the mirrors) */
        return;
    }
    F_800F9084_orig(c);
    split_message_print(c, player, 77, 16);
    mode_status_print(c, player, 79, 74); /* below the radar (side by side views are narrow) */
    score_line_print(c, player, 11, 222, 1);
    hud_extra_print(c, player, 79, 156, 150, 1);
    survival_banner_print(c, 80, 90, 0.75f);
    hudstyle_marks(player, 160, 240);
    four_hud_done(player);
}
int four_views(void);
void four_view_size(int player, int *w, int *h);
int four_divider_kind(void);
void F_800F9D48_orig(CPU *c);
static void vp_HudDrawSplitHorizontal_body(CPU *c)
{
    int who = (int)(c->r[4] & 3), player = who & 1;
    hud_select(who);
    if (vp_hud_hidden(who)) { /* watching: hidden (Triangle) */
        if (who == 0) hudlay_dividers(four_views() ? four_divider_kind() : 2);
        four_hud_done(who);
        return;
    }
    if (hudlay_adaptive() || four_views()) { /* laid out for its view (hud_layout.c); 3-4 players always */
        int w = 320, h = 120;
        if (four_views()) four_view_size(who, &w, &h);
        if (getenv("TM2PC_HUD_DEBUG")) { static int said[4]; if (!said[who]++) fprintf(stderr, "hud: player %d view %dx%d\n", who + 1, w, h); }
        hudlay_begin(2, w, h);
        hudlay_player(who);
        F_800F9D48_orig(c);
        hudlay_force(G_TM);
        split_message_print(c, who, 157, 16);
        mode_status_print(c, who, 159, 28);
        timer_print(c, who, 157, 28);
        hudlay_force(G_BL);
        score_line_print(c, who, 11, h - 13, 2);
        hudlay_force(-1);
        hud_extra_print(c, who, 159, 316, 76, 0); /* (every view: 3-4 players drew it in views 1-2 only, the user's 2026-11-18) */
        hudlay_force(G_C);
        survival_banner_print(c, w / 2, 40, w <= 160 ? 0.5f : 0.75f); /* (under the message bar; a quarter smaller) */
        hudstyle_draw(who, w, h);
        hudstyle_tm2_turbo(who, 2);
        hudstyle_marks(who, w, h);
        hudlay_end();
        if (who == 0) hudlay_dividers(four_views() ? four_divider_kind() : 2);
        four_hud_done(who);
        return;
    }
    F_800F9D48_orig(c);
    split_message_print(c, player, 157, 16);
    mode_status_print(c, player, 159, 28);
    score_line_print(c, player, 11, 107, 2);
    hud_extra_print(c, player, 159, 316, 76, 0);
    survival_banner_print(c, 160, 40, 0.75f);
    hudstyle_marks(player, 320, 120);
    four_hud_done(player); /* 3-4 players: views 3-4 after HUD 2 */
}
void F_800F8370_orig(CPU *c);
static void vp_HudDraw_body(CPU *c)
{
    static int off = -1;
    if (off < 0) off = getenv("TM2PC_HUD_OFF") != NULL;
    if (off || vp_hud_hidden(0)) return; /* (watching: hidden with Triangle) */
    /* messages hidden: their bar too (the record's first primitive, a POLY_F4 at
     * hudPlayerRecords 801828F0 + player * 1900: its corners collapsed while drawn) */
    int player = (int)(c->r[4] & 1);
    hud_select(player); /* this player's HUD settings */
    uint32_t bar = RB_hudPlayerRecords + 1900u * (uint32_t)player + 8, corners[4];
    int hide_bar = settings_video.hud_no_messages;
    for (int i = 0; hide_bar && i < 4; i++) {
        corners[i] = mem_r32(c, bar + 4u * (uint32_t)i);
        mem_w32(c, bar + 4u * (uint32_t)i, 0);
    }
    const char *mine = hide_bar ? NULL : port_message(player);
    uint32_t cur = mem_r32(c, GA(0x80183F00u)), slot = GA(0x8016286Cu) + 4u * (cur < 37 ? cur : 0), saved_str = mem_r32(c, slot);
    if (mine) { /* the port's message (a new Juggernaut, YOU KILLED X) in the message line */
        const uint32_t buf = DM_TEXT + 0x680 + 0x40u * (uint32_t)(player & 1);
        int i = 0;
        for (; i < MSG_MAX && mine[i]; i++) mem_w8(c, buf + (uint32_t)i, (uint8_t)mine[i]);
        mem_w8(c, buf + (uint32_t)i, 0);
        mem_w32(c, slot, buf);
    }
    if (!hide_bar) { /* the bar: 84..214, wider for a longer text */
        uint32_t str = mem_r32(c, slot);
        int w = str ? (int)guest_call(GA(0x8011355Cu), 2, str, 0, 0) : 0, hw = w / 2 + 8 > 75 ? w / 2 + 8 : 75; /* a little wider than the game's (65) */
        for (uint32_t i = 0; i < 4; i++) mem_w16(c, bar + 4 * i, (uint16_t)(int16_t)(149 + ((i & 1) ? hw : -hw)));
    }
    if (hudlay_adaptive()) { hudlay_begin(0, 320, 240); hudlay_player(player); } /* laid out by hud_layout.c */
    hudlay_mirror();
    score_player = player;
    F_800F8370_orig(c);
    if (mine) mem_w32(c, slot, saved_str);
    for (int i = 0; hide_bar && i < 4; i++) mem_w32(c, bar + 4u * (uint32_t)i, corners[i]);
    hudlay_force(G_TM);
    mode_status_print(c, player, 159, 48);
    timer_print(c, player, 149, 44); /* (the bar's centre in one view, x 149: hud_layout.c's tm_c) */
    hudlay_force(G_BL);
    /* (the score line: the game's own lives print, its text swapped, vp_FontSpritePrintCentered) */
    hudlay_force(-1);
    hud_extra_print(c, player, 159, 316, 150, 0);
    hudlay_force(G_C);
    survival_banner_print(c, 160, 90, 1.0f);
    { /* F3: the renderer's capacity (render_stats.c), under the message bar */
        char line[96];
        for (int m = 0; m < 2; m++)
            if (render_stats_line(line, sizeof line, m)) hud_text_print(c, 160, 62 + 10 * m, line, 1);
    }
    if (hudlay_adaptive()) { hudstyle_draw(player, 320, 240); hudstyle_tm2_turbo(player, 0); }
    hudstyle_marks(player, 320, 240);
    hudlay_end();
}

/* ---- Customize HUD (Options > Video; settings_video.hud_*) ----
 * At each round's start the radar and the full weapon list take the
 * settings (the game's per-player flags hudRadarVisible 80180A6C and
 * hudWeaponListVisible 80180A70; its own buttons still toggle them). The
 * target panel (hudDrawTargetPanel 800FD0D0) and the top message (its
 * fontSpritePrintCenteredXY 80113A14 with hudMessageTextMode 80181E28, one
 * view or split) are left out while hidden. Tests: TM2PC_HUD_OFF=1 draws no
 * HUD, TM2PC_HUD_LIST=1 the full weapon list (the Customize HUD preview's
 * pictures). */
int four_hud_narrow(int player); /* vplus/four.c */
void mirrors_round(void); /* mirrors.c: each player's mirror as the setting again */
static void hud_round(void)
{
    mirrors_round();
    /* the rear-view mirror (Customize HUD): on from the start in one view (the camera button still toggles it) */
    if (hud_of(0)->mirror && !mem_r32(&M.cpu, GA(0x801809E4u)) && !mem_r32(&M.cpu, GA(0x801809DCu)) &&
        !guest_call(GA(0x8011EDC4u), 0, 0, 0, 0)) /* (never in the attract demo: gameIsAttractDemo, the user's) */
        guest_call(GA(0x800D1BB8u), 0, 0, 0, 0); /* rtToggleRearView (allowed in one view) */
    for (int p = 0; p < 4; p++) { /* players 3-4: their bytes (room for 4); each player's own settings */
        int list = hud_of(p)->full_weapons || getenv("TM2PC_HUD_LIST");
        mem_w8(&M.cpu, GA(0x80180A6Cu) + (uint32_t)p, (uint8_t)!hud_of(p)->no_radar);
        mem_w8(&M.cpu, GA(0x80180A70u) + (uint32_t)p, (uint8_t)(list != 0));
    }
}
/* The arenas whole in every mode (the user's, 2026-11-05): the game loads other files for Paris, New York and
 * Antarctica in modes 2 (the co-op tournament) and 4 (two players' head to head): the PS1's split screen ones, New
 * York's glass building boarded up. The Challenge Match (mode 3, every player count, online too) has the whole ones;
 * here the file names are made as for it. (tests: TM2PC_CLASSIC_2P_MAPS=1, the game's) */
void F_8012A998_orig(CPU *c);
void F_8012AA5C_orig(CPU *c);
static void whole_map_name(CPU *c, void (*orig)(CPU *))
{
    uint32_t s = c->r[4];
    uint8_t mode = mem_r8(c, s);
    int swap = (mode == 2 || mode == 4) && !getenv("TM2PC_CLASSIC_2P_MAPS");
    if (swap) mem_w8(c, s, 3);
    orig(c);
    if (swap) mem_w8(c, s, mode);
    if (getenv("TM2PC_MAP_DEBUG")) { /* tests: the file name made */
        char name[64];
        for (int i = 0; i < 63; i++) if (!(name[i] = (char)mem_r8(c, c->r[2] + (uint32_t)i))) break;
        name[63] = 0;
        fprintf(stderr, "map: mode %d level %d: %s%s\n", mode, mem_r8(c, s + 24), name, swap ? " (the whole one)" : "");
    }
}
void vp_LevelDmdFileName(CPU *c) { whole_map_name(c, F_8012A998_orig); mod_arena_file_name(c, "DMD", "DMD"); }
void vp_LevelTmsFileName(CPU *c) { whole_map_name(c, F_8012AA5C_orig); mod_arena_file_name(c, "TMS", "TMS"); }
void F_800FD0D0_orig(CPU *c);
void vp_HudTargetPanel(CPU *c)
{
    if (settings_video.hud_no_target) return;
    F_800FD0D0_orig(c);
}
void F_80113A14_orig(CPU *c);
void vp_FontSpritePrintCentered(CPU *c) /* fontSpritePrintCenteredXY(font, x, y, mode, sprites, text, ot) */
{
    if (c->r[7] == GA(0x80181E28u) && settings_video.hud_no_messages) { c->r[2] = 0; return; }
    { /* the target panel's name, its car another player: the player's name (the user's 2026-11-14: as over the cars),
       * not the car's (a computer car keeps its character's name) */
        int interp_target_human(void); /* vplus/interp.c */
        void vp_human_name_upper(int h, char *out, size_t n); /* (below, with the kill feed) */
        static int car_name = -1; /* (tests: TM2PC_TARGET_CAR_NAME=1, the car's name as before) */
        if (car_name < 0) car_name = getenv("TM2PC_TARGET_CAR_NAME") != NULL;
        int th = car_name ? -1 : interp_target_human();
        uint32_t modes = RB_hudTargetNameModes;
        if (th >= 0 && c->r[7] >= modes && c->r[7] < modes + 0x30u) {
            char nm[24];
            vp_human_name_upper(th, nm, sizeof nm);
            const uint32_t buf = DM_TEXT + 0x100, arg = c->r[29] + 20; /* (the text: the 6th argument, on the stack) */
            /* as the panel's own long names (SWEETTOOTH): no spaces, in the room the game's longest name takes
             * (GRASSHOPPER); "SCOTT CAMPBELL" ran past the panel's left edge (the user's 2026-11-15) */
            int o = 0;
            for (int i = 0; nm[i]; i++) if (nm[i] != ' ') nm[o++] = nm[i];
            nm[o] = 0;
            static const char longest[] = "GRASSHOPPER";
            for (int i = 0; i < (int)sizeof longest; i++) mem_w8(c, buf + (uint32_t)i, (uint8_t)longest[i]);
            int room = (int)guest_call(GA(0x8011355Cu), c->r[4], buf, 0, 0); /* fontStringWidth (the call's font) */
            for (int i = 0; i < 24; i++) { mem_w8(c, buf + (uint32_t)i, (uint8_t)nm[i]); if (!nm[i]) break; }
            int width = (int)guest_call(GA(0x8011355Cu), c->r[4], buf, 0, 0);
            int hudlay_adaptive(void); /* hud_layout.c: the HUD laid out (else drawn as the game's: no scaling) */
            for (; !hudlay_adaptive() && width > room && o > 1; nm[--o] = 0) { /* (the game's own layout: as many letters as fit) */
                mem_w8(c, buf + (uint32_t)o - 1, 0);
                width = (int)guest_call(GA(0x8011355Cu), c->r[4], buf, 0, 0);
            }
            /* longer than that: the whole name, its letters shrunk to the room (scaled as the order table is walked,
             * hud_layout.c), about its middle; cut short it read "MIRANDAWA" (the user's 16-player take 2026-11-17) */
            uint32_t sprites = mem_r32(c, c->r[29] + 16);
            void hudlay_range_scale(uint32_t a, uint32_t b, int x, int y, float k); /* hud_layout.c */
            int cx = (int)c->r[5];
            uint32_t was = mem_r32(c, arg);
            mem_w32(c, arg, buf);
            F_80113A14_orig(c);
            mem_w32(c, arg, was);
            /* about the letters' own middle (the first letter's sprite: its top and height), so the smaller name sits in
             * the middle of the bar as a full-size one does (about the text's y + 6 it sat low: the user's 2026-11-17) */
            uint32_t first = sprites; /* (its letters from the buffer's start: the draw mode is the 4th argument) */
            int ly = (int16_t)(mem_r32(c, first + 8) >> 16), lh = (int)(mem_r32(c, first + 16) >> 16);
            hudlay_range_scale(sprites, sprites + 12u + 20u * 24u, cx, ly + lh / 2,
                               width > room && width > 0 ? (float)room / (float)width : 1.0f);
            static int dbg = -1; /* tests: TM2PC_TARGET_DEBUG, the name's line */
            if (dbg < 0) dbg = getenv("TM2PC_TARGET_DEBUG") != NULL;
            if (dbg) fprintf(stderr, "target name: y %d top %d h %d\n", (int)c->r[6], ly, lh);
            return;
        }
        if (c->r[7] >= modes && c->r[7] < modes + 0x30u) { /* a computer car's name: at its size (not a player's shrink left on) */
            void hudlay_range_scale(uint32_t a, uint32_t b, int x, int y, float k);
            uint32_t sprites = mem_r32(c, c->r[29] + 16);
            hudlay_range_scale(sprites, sprites, 0, 0, 1.0f);
        }
    }
    char text[16];
    /* (a co-op tournament's one view of player 2+, online: the game printed that player's settings lives, never used in
     * co-op (its lives a team pool, player 1's): "Lives: 2" all along, the host's "Lives: 0"; the user's 2026-11-09) */
    int coop_one = !vp_dm() && mem_r8(c, SETTINGS_BLOCK) == 2 && !settings_video.hud_no_lives; /* (one view: the team's pool) */
    if (coop_one) {
        int lv = mem_r8(c, SETTINGS_BLOCK + 20) - 1; /* (the pool's byte itself: online gameGetLives reads portHumanLives) */
        snprintf(text, sizeof text, "Lives: %d", lv < 0 || lv > 0x7E ? 0 : lv);
    }
    if (c->r[7] == GA(0x8018208Cu) && (coop_one || (vp_dm() && score_text(score_player, text, sizeof text)))) { /* the lives line: the score */
        const uint32_t buf = DM_TEXT + 0x40, arg = c->r[29] + 20; /* (the text: the 6th argument, on the stack) */
        for (int i = 0; i < 16; i++) { mem_w8(c, buf + (uint32_t)i, (uint8_t)text[i]); if (!text[i]) break; }
        uint32_t was = mem_r32(c, arg);
        mem_w32(c, arg, buf);
        uint32_t sprites = mem_r32(c, c->r[29] + 16);
        F_80113A14_orig(c);
        mem_w32(c, arg, was);
        score_dash_raise(sprites, text);
        return;
    }
    if (c->r[7] == GA(0x80181E28u) && !teams_on() && (vp_duo() || vp_versus_on())) {
        /* "Enemies: N" is every car still in but one, the same on every machine (the HUD's state is hashed online): a
         * player out, watching, saw one fewer than were left (the user's 2026-11-11). Their screen only: one more */
        int humans_local_count(void), humans_view_human(int view);
        const uint32_t arg = c->r[29] + 20, was = mem_r32(c, arg);
        int h = humans_view_human(0), n = -1, local = humans_local_count();
        char cur[16];
        for (int i = 0; i < 15; i++) { cur[i] = (char)mem_r8(c, was + (uint32_t)i); cur[i + 1] = 0; if (!cur[i]) break; }
        if ((local == 1 || (local < 0 && mem_r8(c, SETTINGS_BLOCK + 4) == 1)) && h >= 0 && h < CARS_HUMANS_MAX && coop_out[h] &&
            sscanf(cur, "Enemies: %d", &n) == 1 && n >= 0) {
            n = (int)guest_call(GA(0x8011E16Cu), 0, 0, 0, 0); /* (counted now: the message can be a frame behind; vp_AiCountAlive) */
            snprintf(text, sizeof text, "Enemies: %d", n + 1);
            static int said;
            if (!said++) fprintf(stderr, "enemies: player %d out, watching: %s on this screen (the shared %d)\n", h + 1, text, n);
            const uint32_t buf = DM_TEXT + 0x40;
            for (int i = 0; i < 16; i++) { mem_w8(c, buf + (uint32_t)i, (uint8_t)text[i]); if (!text[i]) break; }
            mem_w32(c, arg, buf);
            F_80113A14_orig(c);
            mem_w32(c, arg, was);
            return;
        }
    }
    F_80113A14_orig(c);
}

/* Co-op Survival runs in the challenge mode (3), which the session runner
 * gives one view (viewSetSplitMode 800D1C24 with 0; decomp session_runner.cpp:
 * modes 2 and 4 split, vertically on levels 2, 3 and 8): with two players,
 * the co-op tournament's split instead. */
void F_800D1C24_orig(CPU *c);
int four_humans(void);                   /* vplus/four.c: 3-4 players */
int four_split_mode(int level);
void four_present(void);
void vp_ViewSetSplitMode(CPU *c)
{
    int humans_local_count(void); /* vplus/humans.c */
    int local = humans_local_count(); /* (online: this machine's players' views only) */
    if (local == 1 || local == 0) { c->r[4] = 0; F_800D1C24_orig(c); return; } /* one player here: one view, theirs (none:
                                                                                 * watching the match, one view) */
    if (local == 2 && c->r[4] == 0) c->r[4] = 1; /* two here: the game's split (as the setting, below), whatever the match's mode */
    int four_vn(void);
    if ((c->r[4] == 0 || local > 2 || local < 0) && four_vn()) /* their views (four.c); offline (local -1) too: the co-op
        * tournament's own side-by-side split on levels 2, 3 and 8 left 3-4 players two views (the trailer's dry run 2026-11-16) */
        c->r[4] = (uint32_t)four_split_mode(mem_r8(c, GA(0x8016475Cu) + 24));
    if (c->r[4] == 0 && (ui_survival() || vp_duo()) && mem_r8(c, GA(0x8016475Cu) + 4) == 2) { /* 2 players and computer cars too */
        int level = mem_r8(c, GA(0x8016475Cu) + 24);
        c->r[4] = level == 2 || level == 3 || level == 8 ? 1 : 2;
    }
    uint32_t ra = c->r[31], rr = gr(ra);
    int hotkey = (rr >= 0x8011C530u && rr < 0x8011C8E4u) || gr_function(ra) == 0x8011C530u; /* carViewHotkeys: the player's own toggle */ /* retail-address */
    if (c->r[4] && !hotkey && !four_vn()) c->r[4] = settings_video.split == 1 ? 2 : 1; /* Split screen (Options > Video) */
    if (c->r[4] && hotkey && !four_vn()) { /* (switched in a battle: kept as the setting) */
        settings_video.split = c->r[4] == 2 ? 1 : 2;
        settings_save_section("video");
    }
    F_800D1C24_orig(c);
}

/* ---- computer cars fighting each other (decomp AI_VS_AI.md 10) ----
 * With teams, while spectating and with "Everyone equal" (computer cars with
 * computer cars as targets): their aimed shots steer at their own target,
 * not player 1 (weaponFindTargetAhead / Behind, the "AI owner" branch: the
 * target object's position, +0xA8); every one is updated each frame (the
 * "can't be seen" throttle of aiCarShouldUpdate off, broken-up wrecks still
 * off); and their hits on each other are not divided by 4 (feat_Damage). */
int vp_ai_vs_ai(void)
{
    const GameType *t = battle_rules();
    return t && !ui_survival() && (teams_on() || spectating || t->ai_targets == AI_TARGETS_EQUAL ||
                                   t->mode == MODE_JUGGERNAUT || t->mode == MODE_ODDBALL || t->mode == MODE_GUN_GAME);
}
int port_config_on(void); /* vplus/port_config.c */
static uint32_t ai_aim_target(int owner) /* the object an AI owner's shot steers at, 0: as retail */
{
    if (owner < 0 || owner >= cars_ai_max() || !ai_alive(owner)) return 0; /* (every computer car: 15 with 16 cars) */
    int t = (int)mem_r32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)owner);
    if (t >= 0 && t < cars_ai_max()) return ai_alive(t) ? mem_r32(&M.cpu, GA(0x80188B38u) + 4u * (uint32_t)t) : 0;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), best = -1; /* -1: the nearest human still in */
    uint32_t best_d = 0; /* (each axis / 16, squared in 32 bits: as the game's C does it, decomp/ TM2_PORT_AI_AIM) */
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) {
        if (mem_r8(&M.cpu, HUMAN_REC(i) + 0x9C)) continue;
        int32_t dx = (int32_t)(mem_r32(&M.cpu, HUMAN_REC(i) + 0x6FC) - mem_r32(&M.cpu, AI_REC(owner) + 0x73C)) / 16;
        int32_t dy = (int32_t)(mem_r32(&M.cpu, HUMAN_REC(i) + 0x700) - mem_r32(&M.cpu, AI_REC(owner) + 0x740)) / 16;
        uint32_t d = (uint32_t)dx * (uint32_t)dx + (uint32_t)dy * (uint32_t)dy;
        if (best < 0 || d < best_d) { best_d = d; best = i; }
    }
    return best < 0 ? 0 : guest_call(GA(0x8011CB50u), (uint32_t)best, 0, 0, 0); /* humanCarObject */
}
static int ai_aim(CPU *c) /* weaponFindTarget*(owner, out, mode): 1 when done here */
{
    if ((c->r[6] & 0xFF) || !vp_ai_vs_ai() || port_config_on()) return 0; /* mode 0: a computer car's shot (with the port's C: it does it) */
    uint32_t obj = ai_aim_target((int)c->r[4]);
    if (!obj) return 0;
    mem_w32(c, c->r[5], obj + 0xA8);
    return 1;
}
/* With teams a player's homing never picks a teammate: the game's search (weaponFindTargetAhead 80109F94 /
 * Behind 8010A2AC, decomp weapon_targets.c) without them. Each car's offset in the owner's frame (its matrix
 * at +0x80, 4.12); ahead: y >= 12, y >= |x|, y < 8001 (computer cars also z < 401); behind: -y > 11,
 * -y >= |x|, -y < 8001; the nearest; the computer cars first, the players only when none qualifies. */
static int mode_side(int slot); /* (below: Juggernaut, Hunted without teams: 1 the special cars, 0 the others; -1 no sides) */
static int team_aim(CPU *c, int behind, int ffa_dead) /* (ffa_dead: free for all, the game's pick was a dead player: again
                                                    * without the dead, no teams) */
{
    int owner = (int)c->r[4];
    int sides = !teams_on() && mode_side(-1) >= 0; /* Juggernaut, Hunted: the others lock only onto the special car (the user's) */
    /* co-op (the tournament's, Survival's with 2 or more): the players one side, the computer cars the other: a player's
     * homing never on another player (the user's online take: missiles, napalm on the teammates; their hits still hurt) */
    int coop = !teams_on() && !sides && (mem_r8(c, GA(0x8016475Cu)) == 2 || (ui_survival() && (int)mem_r32(c, GA(0x80181014u)) >= 2));
    if (!(c->r[6] & 0xFF) || (!ffa_dead && !teams_on() && !sides && !coop) || owner < 0 || owner >= CARS_HUMANS_MAX) return 0; /* a player's shot in a team game */
    uint32_t me = guest_call(GA(0x8011CB50u), (uint32_t)owner, 0, 0, 0); /* humanCarObject */
    if (!me) return 0;
    int humans = (int)mem_r32(c, GA(0x80181014u)), ais = (int)mem_r32(c, GA(0x8018101Cu));
    int mine = ffa_dead ? -1 : coop ? 0 : sides ? mode_side((int16_t)mem_r16(c, HUMAN_REC(owner) + 4)) : team_of_human(owner); /* (-1: no one's) */
    if (sides && mine == 1) return 0; /* (the Juggernaut, a target: any car, as the game's) */
    int16_t m[9];
    for (int i = 0; i < 9; i++) m[i] = (int16_t)mem_r16(c, me + 0x80 + 2u * (uint32_t)i);
    uint32_t best = 0;
    int best_d = 0;
    for (int pass = 0; pass < 2 && !best; pass++) {
        int n = pass ? humans : ais;
        for (int i = 0; i < n && i < (pass ? CARS_HUMANS_MAX : cars_ai_max()); i++) { /* (past the 9th computer car too: the user's) */
            uint32_t o;
            if (pass) {
                if (coop || i == owner || mem_r8(c, HUMAN_REC(i) + 0x9C) || (int32_t)mem_r32(c, HUMAN_REC(i) + 0x1F0) <= 0 || /* (dead: no) */
                    (sides ? mode_side((int16_t)mem_r16(c, HUMAN_REC(i) + 4)) : team_of_human(i)) == mine) continue;
                o = guest_call(GA(0x8011CB50u), (uint32_t)i, 0, 0, 0);
            } else {
                if ((int32_t)mem_r32(c, AI_REC(i) + 0x5A0) <= 0 ||
                    (!coop && (sides ? mode_side((int16_t)mem_r16(c, AI_REC(i) + 6)) : team_of_ai(i)) == mine)) continue;
                o = mem_r32(c, GA(0x80188B38u) + 4u * (uint32_t)i);
            }
            if (!o) continue;
            int d[3], l[3];
            for (int a = 0; a < 3; a++) d[a] = (int32_t)(mem_r32(c, o + 0xA8 + 4u * (uint32_t)a) - mem_r32(c, me + 0xA8 + 4u * (uint32_t)a));
            for (int r = 0; r < 3; r++) l[r] = (m[r * 3] * d[0] + m[r * 3 + 1] * d[1] + m[r * 3 + 2] * d[2]) / 4096;
            int y = behind ? -l[1] : l[1], x = l[0] < 0 ? -l[0] : l[0];
            if (y < (behind ? 12 : 12) || y < x || y >= 8001 || (!pass && !behind && l[2] >= 401)) continue;
            if (!best || y < best_d) { best = o; best_d = y; }
        }
    }
    mem_w32(c, c->r[5], best ? best + 0xA8 : 0);
    return 1;
}
/* the game's own pick for a player's shot (free for all) a dead or out player: weaponFindTarget* scans the players with
 * no health test (a wreck, a player out of lives: Phoenix's, KinggzZ's reports); then the search again without them */
static int aim_at_dead(CPU *c, uint32_t pos)
{
    int humans = (int)mem_r32(c, GA(0x80181014u));
    for (int i = 0; pos && i < humans && i < CARS_HUMANS_MAX; i++) {
        uint32_t o = guest_call(GA(0x8011CB50u), (uint32_t)i, 0, 0, 0); /* humanCarObject */
        if (o && o + 0xA8 == pos) return (int32_t)mem_r32(c, HUMAN_REC(i) + 0x1F0) <= 0 || mem_r8(c, HUMAN_REC(i) + 0x9C);
    }
    return 0;
}
/* With teams a computer car's aimed shot (mode 0) never locks onto a teammate: its own target record (any of the 15,
 * the decomp's portAiAim only the first 9: past them its retail fallback, the nearest player, a teammate: KinggzZ's
 * 2026-11-24 King of the Hill, his red team and Dark Tooth alone on blue, teammates' missiles and specials on him;
 * decomp request 123), else the nearest living player of another team, else no lock at all. State only: the same on
 * every machine. */
static int team_ai_aim(CPU *c)
{
    if ((c->r[6] & 0xFF) || !teams_on()) return 0;
    int owner = (int)c->r[4], n = (int)mem_r32(c, GA(0x8018101Cu));
    if (owner < 0 || owner >= n || owner >= cars_ai_max() || !ai_alive(owner)) return 0;
    int mine = team_of_ai(owner), t = (int32_t)mem_r32(c, GA(0x801879FCu) + 4u * (uint32_t)owner);
    uint32_t obj = 0;
    if (t >= 0 && t < n && t < cars_ai_max() && t != owner && ai_alive(t) && team_of_ai(t) != mine)
        obj = mem_r32(c, GA(0x80188B38u) + 4u * (uint32_t)t);
    if (!obj) {
        int humans = (int)mem_r32(c, GA(0x80181014u)), best = -1;
        uint32_t best_d = 0; /* (each axis / 16, squared in 32 bits: as portAiAim) */
        for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) {
            if (mem_r8(c, HUMAN_REC(i) + 0x9C) || (int32_t)mem_r32(c, HUMAN_REC(i) + 0x1F0) <= 0 || team_of_human(i) == mine) continue;
            int32_t dx = (int32_t)(mem_r32(c, HUMAN_REC(i) + 0x6FC) - mem_r32(c, AI_REC(owner) + 0x73C)) / 16;
            int32_t dy = (int32_t)(mem_r32(c, HUMAN_REC(i) + 0x700) - mem_r32(c, AI_REC(owner) + 0x740)) / 16;
            uint32_t d = (uint32_t)dx * (uint32_t)dx + (uint32_t)dy * (uint32_t)dy;
            if (best < 0 || d < best_d) { best_d = d; best = i; }
        }
        if (best >= 0) obj = guest_call(GA(0x8011CB50u), (uint32_t)best, 0, 0, 0); /* humanCarObject */
    }
    static int dbg = -1; /* tests: TM2PC_TEAM_AIM_DEBUG, each aimed shot whose target the decomp's portAiAim left out */
    if (dbg < 0) dbg = getenv("TM2PC_TEAM_AIM_DEBUG") != NULL;
    if (dbg && t >= 9) fprintf(stderr, "team aim: computer car %d, target record %d: %s\n", owner + 1, t, obj ? "aimed at it" : "no lock");
    mem_w32(c, c->r[5], obj ? obj + 0xA8 : 0);
    return 1;
}
static void find_target(CPU *c, int behind, void (*orig)(CPU *))
{
    if (team_ai_aim(c) || ai_aim(c) || team_aim(c, behind, 0)) return;
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    orig(c);
    if (!(a2 & 0xFF) || !aim_at_dead(c, mem_r32(c, a1))) return;
    uint32_t v0 = c->r[2];
    c->r[4] = a0, c->r[5] = a1, c->r[6] = a2;
    team_aim(c, behind, 1);
    c->r[2] = v0; /* (the callers ignore it) */
}
void F_80109F94_orig(CPU *c);
void vp_FindTargetAhead(CPU *c) { find_target(c, 0, F_80109F94_orig); }
void F_8010A2AC_orig(CPU *c);
void vp_FindTargetBehind(CPU *c) { find_target(c, 1, F_8010A2AC_orig); }
void F_8011B7D8_orig(CPU *c);
void vp_AiCarShouldUpdate(CPU *c)
{
    int i = (int)c->r[4];
    if (i >= 0 && i < cars_ai_max() && vp_ai_vs_ai() && !port_config_on()) /* (with the port's C: aiCarShouldUpdate does it, port_config.c) */ { c->r[2] = mem_r8(c, AI_REC(i) + 0x501) != 4; return; }
    F_8011B7D8_orig(c);
}

/* ---- the map's pickups look like what they give (decomp PICKUP_LOOKS.md 6) ----
 * A pickup on the map is a level node (pickupObjects 8018F6BC + 4n, n its
 * slot): its id (+2) is what a car gets, its one child the model drawn and
 * hit (kind 1: +0x18, kind 4: +0x14). A Challenge Match game type's turned-off
 * kinds become a kind that is on (vp_pickup_id): here the node takes that
 * kind's id and model (the model of a pickup of that kind on the level: they
 * share one) and its slot (80183F7C + 16n) the kind, so it looks and acts as
 * the new one. At each round's start, and for a pickup taken (hidden), again,
 * so it comes back as a new pick. Kinds with no model on the level (and
 * "nothing") keep the old look and change at collection. The port's special
 * (PICKUP_SPECIAL) looks like the HUD's SPECIAL globe: the decomp's look,
 * portPickupSpecial.model, built at each load (request 79). */
#define PICKUP_OBJECTS GA(0x8018F6BCu)
#define PICKUP_SLOTS GA(0x80183F7Cu)
static uint32_t pickup_model[32];   /* by kind - 400 (401..430), from the level */
static int16_t pickup_kind0[50];    /* each slot's kind as the level has it */
static uint32_t pickup_step, pickup_taken_step[50]; /* respawn steps counted; when each slot was taken (0: not) */
static uint32_t pickup_node[50];    /* its node (a new one: a new level) */
static int pickup_models_ready;
static int pickup_debug(void) /* tests: TM2PC_PICKUP_DEBUG (cached: per-step paths) */
{
    static int on = -1;
    if (on < 0) on = getenv("TM2PC_PICKUP_DEBUG") != NULL;
    return on;
}
static uint32_t pickup_child_at(uint32_t node) { return node + (mem_r8(&M.cpu, node) == 1 ? 0x18u : 0x14u); }
static uint32_t pickup_special_look(void) /* the special's look on this level (0: none, the old look) */
{
    static uint32_t at = 1;
    if (at == 1) at = port_sym("portPickupSpecial");
    return at ? mem_r32(&M.cpu, at) : 0; /* (its +0: the model) */
}
static void pickup_reroll(int n)
{
    uint32_t node = mem_r32(&M.cpu, PICKUP_OBJECTS + 4u * (uint32_t)n);
    if (!node || pickup_kind0[n] < 401 || pickup_kind0[n] > 430) return;
    int b = vp_pickup_id(pickup_kind0[n]), keep = b >= 440 ? 40 : 0, base = b - keep;
    /* a turned-off kind's stand-in with no look on this level (each level loads only some pickups' models): another
     * pick, up to 8, so the slot does not keep the old look (Holland's health pickup stayed one with health off, the
     * user's 2026-11-25; it gave a weapon when taken). The picks draw vp_rand: every machine the same */
    for (int tries = 0; tries < 8 && b != pickup_kind0[n] && base != PICKUP_SPECIAL && base >= 401 && base <= 430 &&
                        !pickup_model[base - 400]; tries++)
        b = vp_pickup_id(pickup_kind0[n]), keep = b >= 440 ? 40 : 0, base = b - keep;
    if (base == PICKUP_SPECIAL && pickup_special_look()) { /* a special: the globe (the slot keeps its kind) */
        mem_w16(&M.cpu, node + 2, (uint16_t)b);
        mem_w32(&M.cpu, pickup_child_at(node), pickup_special_look());
        return;
    }
    if (base == PICKUP_SPECIAL) keep = 0;
    if (base < 401 || base > 430 || !pickup_model[base - 400]) base = pickup_kind0[n], keep = 0; /* no look for it here */
    mem_w16(&M.cpu, node + 2, (uint16_t)(base + keep));
    mem_w32(&M.cpu, pickup_child_at(node), pickup_model[base - 400]);
    mem_w16(&M.cpu, PICKUP_SLOTS + 16u * (uint32_t)n, (uint16_t)base);
}
/* a match's load (vplus/four.c vp_SessionLoad): each slot's own kind read from the new level at its first round. The kinds
 * were read only when a slot's node moved: every arena's file loads at one address, so a slot of another arena at the
 * same place kept the last arena's kind, and each machine's from the arenas it had loaded before (online: pickups
 * rolled different kinds on different machines, more with every match: the re-syncs that grew as a session went on,
 * 2026-11-24's logs). With the network states too (vp_keep_register: a machine coming in takes the host's) */
void vp_pickups_loading(void)
{
    memset(pickup_node, 0, sizeof pickup_node);
    memset(pickup_kind0, 0, sizeof pickup_kind0);
    pickup_models_ready = 0;
}
static void pickups_round(void)
{
    pickup_step = 0; /* (a new round: no slot waiting) */
    memset(pickup_taken_step, 0, sizeof pickup_taken_step);
    if (!battle_rules()) { pickup_models_ready = 0; return; }
    memset(pickup_model, 0, sizeof pickup_model);
    for (int n = 0; n < 50; n++) { /* the level's models, and each slot's own kind */
        uint32_t node = mem_r32(&M.cpu, PICKUP_OBJECTS + 4u * (uint32_t)n);
        if (!node) { pickup_kind0[n] = 0; pickup_node[n] = 0; continue; }
        int id = mem_r16(&M.cpu, node + 2), base = id >= 441 && id <= 470 ? id - 40 : id;
        uint32_t child = mem_r32(&M.cpu, pickup_child_at(node));
        if (node != pickup_node[n]) { pickup_node[n] = node; pickup_kind0[n] = (int16_t)base; } /* a new level: its own kind */
        if (base >= 401 && base <= 430 && child && mem_r8(&M.cpu, child) != 8 && !pickup_model[base - 400])
            pickup_model[base - 400] = child;
    }
    if (!pickup_model[17]) pickup_model[17] = pickup_model[18]; /* the two health pickups share one */
    if (!pickup_model[18]) pickup_model[18] = pickup_model[17];
    pickup_models_ready = 1;
    for (int n = 0; n < 50; n++) pickup_reroll(n);
    if (pickup_debug())
        for (int n = 0; n < 50; n++) {
            uint32_t node = mem_r32(&M.cpu, PICKUP_OBJECTS + 4u * (uint32_t)n);
            if (node) fprintf(stderr, "pickups: slot %d kind %d -> %d (node %08x, model %08x; weapons %02x, special pickups %d, look %08x)\n", n,
                              pickup_kind0[n], mem_r16(&M.cpu, node + 2), node, mem_r32(&M.cpu, pickup_child_at(node)), battle_rules()->weapons,
                              battle_rules()->special_pickups && battle_rules()->specials, pickup_special_look());
        }
}
/* feat_Pickup (pickupApply's wrap) after the original: a pickup taken (its node hidden) is re-rolled */
void vp_pickup_taken(int slot)
{
    if (!battle_rules() || !pickup_models_ready || slot < 0 || slot >= 50) return;
    uint32_t node = mem_r32(&M.cpu, PICKUP_OBJECTS + 4u * (uint32_t)slot);
    if (node && !mem_r8(&M.cpu, node + (mem_r8(&M.cpu, node) == 1 ? 0x14u : 0x12u))) pickup_reroll(slot);
    pickup_taken_step[slot] = pickup_step ? pickup_step : 1; /* (its wait for the respawn starts) */
    if (pickup_debug()) fprintf(stderr, "pickups: step %u slot %d taken (player 1 at %d, %d, %d)\n", pickup_step, slot,
                                              (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x6FC), (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x700),
                                              (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x704));
}

/* No wasted pickup respawns (portBalance bit 23, request 67, BALANCE.md) also waits per slot: pickupRespawnTick (8010AED0) brings
 * one back every period (200 steps, 10 seconds), and a slot taken a moment before could come back at once (the user's
 * 2026-11-03: "a slot has to at least be empty for 10 seconds"). Here a slot taken less than a period ago looks
 * still there to the draw (its +2 active set for the call, then put back); with nothing to bring back yet but a slot
 * waiting, the draw is tried again at the next step, so that slot comes back when its period is up. Gameplay: the
 * steps and the slots' times are kept with the kill cam's machine and the network states (vp_keep_register). */
#define PICKUP_RESPAWN_TIMER GA(0x80180B40u) /* gp+364 */
#define PICKUP_COUNT GA(0x80180AECu)         /* gp+280 */
void F_8010AED0_orig(CPU *c);
static uint64_t pickup_seen; /* (tests) the slots active after the last draw */
static void pickup_seen_now(CPU *c, int count)
{
    pickup_seen = 0;
    for (int n = 0; n < count && n < 50; n++) pickup_seen |= (uint64_t)(mem_r16(c, PICKUP_SLOTS + 16u * (uint32_t)n + 2u) != 0) << n;
}
void vp_PickupRespawnTick(CPU *c)
{
    pickup_step++;
    int32_t period = (int32_t)mem_r32(c, PICKUP_RESPAWN_PERIOD), count = (int32_t)mem_r32(c, PICKUP_COUNT);
    if (pickup_debug()) { /* tests: a slot back by any other way than this draw */
        uint64_t now = 0, seen = pickup_seen;
        for (int n = 0; n < count && n < 50; n++) now |= (uint64_t)(mem_r16(c, PICKUP_SLOTS + 16u * (uint32_t)n + 2u) != 0) << n;
        for (int n = 0; n < count && n < 50; n++)
            if (((now & ~seen) >> n) & 1 && pickup_step > 2)
                fprintf(stderr, "pickups: step %u slot %d active again outside the draw (taken at %u)\n", pickup_step, n, pickup_taken_step[n]);
    }
    if (!(vp_balance_word() & 0x800000u) || (int32_t)mem_r32(c, PICKUP_RESPAWN_TIMER) < period || count <= 0) {
        F_8010AED0_orig(c);
        pickup_seen_now(c, count);
        return;
    }
    if (count > 50) count = 50;
    uint64_t held = 0; /* the slots made to look still there */
    int waiting = 0;
    for (int n = 0; n < count; n++) {
        uint32_t a = PICKUP_SLOTS + 16u * (uint32_t)n + 2u;
        if (mem_r16(c, a) || !pickup_taken_step[n] || pickup_step - pickup_taken_step[n] >= (uint32_t)period) continue;
        mem_w16(c, a, 1);
        held |= 1ull << n;
        waiting = 1;
    }
    uint64_t before = 0;
    for (int n = 0; n < count; n++) before |= (uint64_t)(mem_r16(c, PICKUP_SLOTS + 16u * (uint32_t)n + 2u) != 0) << n;
    F_8010AED0_orig(c);
    int brought = 0;
    for (int n = 0; n < count; n++) {
        uint32_t a = PICKUP_SLOTS + 16u * (uint32_t)n + 2u;
        if ((held >> n) & 1) mem_w16(c, a, 0);
        else if (!((before >> n) & 1) && mem_r16(c, a)) brought = 1 + n;
    }
    if (!brought && waiting) mem_w32(c, PICKUP_RESPAWN_TIMER, (uint32_t)period); /* (again at the next step) */
    pickup_seen_now(c, count);
    if (pickup_debug())
        fprintf(stderr, "pickups: step %u respawn draw, %s %d (taken at %u)%s\n", pickup_step, brought ? "back: slot" : "none back",
                brought - 1, brought ? pickup_taken_step[brought - 1] : 0, waiting ? " (a slot still waiting)" : "");
}

/* ---- Controls: jump on L1 + R1, energy attacks on the D-pad ----
 * carReadMoveCodes (800DECA8, decomp car_input.c) turns a human's D-pad
 * presses into the energy attacks: a move code sets the record's special
 * (+0x7D4: 100 freeze, 101 mines, 102 napalm, 103 shield, 104 jump, 105 invisibility) and +0xEC to fire it
 * (the rear fire: +0x7CD and +0xEA). Here, after it, a player's shortcut
 * does the same: L1 and R1 together jump; with energy attacks on the D-pad,
 * Up freezes, Down fires behind, Left drops mines, Right shields (the energy
 * cost and the game type's checks are the game's, as for a code). */
#include "controls/bindings.h"
void F_800DECA8_orig(CPU *c);
static uint16_t prev_easy[16], prev_pad[16]; /* (the energy attacks: a press is a change; by human) */
/* L1 + R1 jump and the weapon switch on the same shoulders: the game switches the moment one of them goes down
 * (weaponHumanCycleInput: then nothing more until both are up), so a jump's first shoulder switched the weapon
 * (Brimstone's report, 2026-11-23). The player's Jump-Weapon Switch Safety (Controls; 0 off, 1-3 game steps of 50 ms,
 * sent in their input: every machine the same): a lone shoulder's switch waits that many steps; the other shoulder in
 * time is a jump and nothing switches; let go sooner, the switch then (a tap). Both held: never a switch */
static int8_t jump_wait_dir[16];  /* (the waiting switch: 1 the 0x21 request (+0xE7), 2 the 0x20 (+0xE8); 0 none) */
static int32_t jump_wait_at[16];
uint16_t input_player_bits(int p); /* runtime/input.c: players 3-4's buttons (set = held) */
void vp_MoveCodes(CPU *c)
{
    uint32_t rec = c->r[4];
    int who = (int16_t)mem_r16(c, rec);
    extern int vp_msg_player;
    int humans_on(void), humans_slot(int h); uint16_t humans_easy(int h), humans_held(int h); /* vplus/humans.c */
    int on = humans_on();
    vp_msg_player = on ? humans_slot(who) : who >= 0 && who <= 3 ? who : -1; /* its messages (a combo, energy too low): this
                                                                                * player's screen (another machine's: none) */
    /* A battle's first frames: no weapon goes off from a press made before it (the user's 2026-11-03: a level started
     * with a carried fire missile already flying). The human's "ready to fire" (+0xED) and a waiting energy attack
     * (+0xEC) cleared, and the D-pad codes' history (+0xF1 presses, +0xF2 timer, +0xF4 the code: a tournament keeps the
     * record into the next level, a code half typed as a level ended finished by the next one's first press: the
     * user's missile and jump at a level's start); at the round's first frame its fire and rear fire too. Here, in the
     * game's own step, by the round's frames on the gameplay clock: every machine the same, caught-up steps too (at the
     * frame's start, as it was, a joiner catching up after the host's state skipped every other one: the states parted
     * at each Versus stage) */
    int rf = vp_round_frames();
    static int no_guard = -1; /* (tests: TM2PC_NO_START_GUARD; cached, a per-step path) */
    if (no_guard < 0) no_guard = getenv("TM2PC_NO_START_GUARD") != NULL;
    if (rf < 6 && !no_guard) {
        mem_w8(c, rec + 0xED, 0);
        mem_w8(c, rec + 0xEC, 0);
        mem_w8(c, rec + 0xF1, 0);
        mem_w8(c, rec + 0xF2, 0);
        mem_w32(c, rec + 0xF4, 0);
        if (rf == 0) { mem_w8(c, rec + 0x7CD, 0); mem_w8(c, rec + 0xEA, 0); }
    }
    static int cheat_dbg = -1; /* (tests: TM2PC_MOVE_DEBUG, the codes typed and the god mode's switch) */
    if (cheat_dbg < 0) cheat_dbg = getenv("TM2PC_MOVE_DEBUG") != NULL;
    uint32_t code_was = cheat_dbg ? mem_r32(c, rec + 0xF4) : 0;
    int god_was = cheat_dbg ? mem_r8(c, rec + 0x1F4) : 0;
    F_800DECA8_orig(c);
    if (cheat_dbg && mem_r32(c, rec + 0xF4) != code_was)
        fprintf(stderr, "controls: move code %08X\n", mem_r32(c, rec + 0xF4));
    if (cheat_dbg && mem_r8(c, rec + 0x1F4) != god_was)
        fprintf(stderr, "controls: damageable %d -> %d (the god mode code)\n", god_was, mem_r8(c, rec + 0x1F4));
    vp_msg_player = -1;
    int p = who;
    if (p < 0 || p > (on ? 15 : 3)) return;
    uint16_t easy = on ? humans_easy(p) : (uint16_t)killcam_input(2, p, input_easy_buttons(p)), pressed = (uint16_t)(easy & ~prev_easy[p]);
    /* (the controller's own Select, L1 and R1: the jump and Select's buttons, whatever actions the player gave them; the
     * input word has each action's Standard button, vplus/humans.c) */
    uint16_t pad = on ? (uint16_t)((easy & EASY_SELECT ? 0x0001 : 0) | (easy & EASY_L1 ? 0x0400 : 0) | (easy & EASY_R1 ? 0x0800 : 0)) : p < 2 ? (uint16_t)~M.pad_buttons[p] : (uint16_t)killcam_input(0, p, input_player_bits(p)),
             new_pad = (uint16_t)(pad & ~prev_pad[p]); /* set = held */
    int killcam_battle_frame(void); /* vplus/killcam.c */
    if (rf < 6 && !no_guard) pressed = 0, new_pad = 0; /* a battle's first frames: buttons held from before (the
                                                                * menus, the load) are not new presses (the user's jump
                                                                * as Amazonia started, 2026-11-04) */
    prev_easy[p] = easy;
    prev_pad[p] = pad;
    int special = 0;
    if (pad & 0x0001) pressed = 0; /* Select held: the D-pad is Select's (HUD style, mirror, list), not energy */
    int jump_on = on ? easy & 1 : bind_jump_combo(p), l1 = (pad & 0x0400) != 0, r1 = (pad & 0x0800) != 0;
    static int move_debug = -1; /* (tests: TM2PC_MOVE_DEBUG: the waits, and the selected weapon's changes) */
    if (move_debug < 0) move_debug = getenv("TM2PC_MOVE_DEBUG") != NULL;
    int wait = on ? (easy >> 2) & 3 : bind_jump_safety(p);
    if (jump_on && l1 && r1) { /* (both held: no weapon switch; one waiting, dropped: a jump) */
        if (jump_wait_dir[p] && move_debug) fprintf(stderr, "controls: player %d jump: the waiting weapon switch dropped\n", p + 1);
        jump_wait_dir[p] = 0;
        mem_w8(c, rec + 0xE7, 0);
        mem_w8(c, rec + 0xE8, 0);
        mem_w8(c, rec + 0xE9, 0); /* (the switch's latch: again once both are up) */
    } else if (jump_on && wait && jump_wait_dir[p]) { /* a lone shoulder's switch waiting */
        if ((l1 || r1) && rf - jump_wait_at[p] < wait) { /* (still held, still in time: not yet) */
            mem_w8(c, rec + 0xE7, 0);
            mem_w8(c, rec + 0xE8, 0);
        } else { /* (time up, or let go: the switch now, the latch armed all along) */
            mem_w8(c, rec + 0xE7, jump_wait_dir[p] == 1);
            mem_w8(c, rec + 0xE8, jump_wait_dir[p] == 2);
            if (move_debug) fprintf(stderr, "controls: player %d weapon switch after %d steps\n", p + 1, (int)(rf - jump_wait_at[p]));
            jump_wait_dir[p] = 0;
        }
    } else if (jump_on && wait && (l1 ^ r1) && (new_pad & 0x0C00) && mem_r8(c, rec + 0xE9) &&
               (mem_r8(c, rec + 0xE7) || mem_r8(c, rec + 0xE8))) { /* a lone shoulder's switch: waits */
        jump_wait_dir[p] = mem_r8(c, rec + 0xE7) ? 1 : 2;
        jump_wait_at[p] = rf;
        mem_w8(c, rec + 0xE7, 0);
        mem_w8(c, rec + 0xE8, 0);
        if (move_debug) fprintf(stderr, "controls: player %d weapon switch waits (%d steps)\n", p + 1, wait);
    } else jump_wait_dir[p] = 0;
    if (move_debug) {
        static int8_t seen_sel[16];
        int8_t sel = (int8_t)mem_r8(c, rec + 0x7D5);
        if (sel != seen_sel[p]) fprintf(stderr, "controls: player %d weapon %d (frame %d)\n", p + 1, sel, rf);
        seen_sel[p] = sel;
    }
    if (jump_on && l1 && r1 && (new_pad & 0x0C00)) special = 104; /* L1 + R1 */
    else if (pressed & 0x0010) special = 100; /* Up: freeze */
    else if (pressed & 0x0080) special = 101; /* Left: mines */
    else if (pressed & 0x0020) special = 103; /* Right: shield */
    else if (pressed & 0x1000) special = 104; /* its own button (Controls, none by default): jump */
    else if (pressed & 0x0100) special = 105; /* its own button (Controls): invisibility (the code Right, Down, Left, Up:
                                               * 105; 102 is Right, Left, Up, the energy napalm: Phoenix's "invisibility
                                               * key fires napalm", 2026-11-23) */
    else if (pressed & 0x2000) special = 102; /* its own button (Controls, none by default): the energy napalm (Gibbers's) */
    else if (pressed & 0x4000) special = 106; /* its own button (Controls, none by default): the volley */
    else if (pressed & 0x8000) { /* its own button (Controls, none by default): the freeze fired behind (the launch
                                  * follows the rear fire flag, weapon_launch.c's AIM; the user's 2026-11-25) */
        special = 100;
        mem_w8(c, rec + 0x7CD, 1);
    }
    else if (pressed & 0x0040) { /* Down: fire behind (the move code 3-4-2) */
        mem_w8(c, rec + 0x7CD, 1);
        mem_w8(c, rec + 0xEA, 1);
    }
    if (special) {
        mem_w8(c, rec + 0x7D4, (uint8_t)special);
        mem_w8(c, rec + 0xEC, 1);
        if (M.trace || getenv("TM2PC_MOVE_DEBUG")) fprintf(stderr, "controls: player %d energy attack %d\n", p + 1, special);
    }
}

/* ---- Deathmatch (a Challenge Match game type's mode) ----
 * Everyone comes back after the game type's respawn time (players and
 * computer cars: lives without end, the lives code above); the first car
 * (with teams: team) to the match's kills wins, or the most kills when its
 * time limit runs out: the round is ended then (rtScheduleEvent: 3 won when
 * the player, or their team, leads; else 1). The HUD's "Lives" line shows
 * the player's kills (hudLivesText 80180AC4 pointed at "Kills: N", spare RAM
 * at 807FF000; the game writes byte 7 from the lives at settings +20). */
#include "profile/feats.h"
static int vp_dm(void) /* the modes with respawns: Deathmatch, Juggernaut, Gun Game, Hunted */
{
    const GameType *t = battle_rules();
    return t && !ui_survival() && (t->mode == MODE_DEATHMATCH || t->mode == MODE_JUGGERNAUT || t->mode == MODE_GUN_GAME ||
                                   t->mode == MODE_ODDBALL || t->mode == MODE_KING_OF_THE_HILL);
}
static int car_score(const MatchCar *m) { return battle_rules()->mode == MODE_DEATHMATCH ? m->kills : m->points; }
int vp_deathmatch(void) { return vp_dm(); }
static uint64_t dm_start;
static int dm_ended;
#ifndef DM_TEXT
#define DM_TEXT 0x80DFF000u
#endif
static uint32_t lives_text_saved;
static int car_kills_team(int team, int *best_out, int *mine_out) /* a team's kills (team -1: all); the most by one car */
{
    int sum = 0, best = 0, mine = 0;
    for (int i = 0; i < match_cars(); i++) {
        const MatchCar *m = match_car(i);
        int t = m->human >= 0 ? battle_match()->player_team[m->human & 15] % BATTLE_TEAM_COLORS
              : m->ai >= 0 ? battle_match()->team[m->ai % BATTLE_OPPONENT_SLOTS] % BATTLE_TEAM_COLORS : -1;
        if (team >= 0 && t != team) continue;
        int sc = car_score(m);
        sum += sc;
        if (sc > best) best = sc;
        if (m->human == 0) mine = sc;
    }
    if (best_out) *best_out = best;
    if (mine_out) *mine_out = mine;
    return sum;
}
int vp_dm_team_winner(void) /* teams: the team with the most kills */
{
    int best = -1, best_k = -1;
    for (int k = 0; k < BATTLE_TEAM_COLORS; k++) {
        int kk = car_kills_team(k, NULL, NULL);
        if (kk > best_k) { best_k = kk; best = k; }
    }
    return best;
}
static void mode_round_start(void);
static int shim_slot = -1; /* (the Juggernaut's shimmer, below: the car hidden) */
static void mode_frame(void);
#define GUN_LEVELS gun_count /* the game type's list */
static int gun_seq[GUN_COUNT + 16], gun_count = 1; /* (GUN_x, or GUN_COUNT + type >> 4: a car's special) */
static uint64_t streak_last[CARS_HUMANS_MAX];     /* (the players' streaks, kill_feed_add: the last kill, the multi-kill, the spree,
                                     *  a spree's message waiting for the kill's own) */
static int streak_multi[CARS_HUMANS_MAX], streak_spree[CARS_HUMANS_MAX];
static char streak_queue[CARS_HUMANS_MAX][2][40]; /* (a kill's next messages, port_message) */
static int streak_queue_n[CARS_HUMANS_MAX];
static char streak_spree_msg[CARS_HUMANS_MAX][24]; /* (a spree's message until it shows) */
static void dm_round_start(void)
{
    memset(streak_last, 0, sizeof streak_last), memset(streak_multi, 0, sizeof streak_multi); /* (a new round: no streaks) */
    memset(streak_spree, 0, sizeof streak_spree), memset(streak_queue_n, 0, sizeof streak_queue_n);
    memset(streak_spree_msg, 0, sizeof streak_spree_msg);
    dm_start = vp_gv();
    dm_ended = 0;
    mode_round_start();
    if (!lives_text_saved) lives_text_saved = mem_r32(&M.cpu, GA(0x80180AC4u));
    if (vp_dm()) {
        mem_w8(&M.cpu, DM_TEXT, 0); /* the game's lives line blank: the score there is the port's (score_line_print) */
        mem_w32(&M.cpu, GA(0x80180AC4u), DM_TEXT);
    } else {
        mem_w32(&M.cpu, GA(0x80180AC4u), lives_text_saved); /* "Lives:" again */
    }
}
static void dm_frame(void)
{
    if (!vp_dm()) return;
    mode_frame();
    const MatchSetup *m = battle_match();
    int teams = battle_match_teams() >= 2, best = 0, mine = 0;
    car_kills_team(-1, &best, &mine);
    /* the HUD: the player's kills, "Kills: 12" (tens at byte 6, ones from the lives byte) */
    int k = battle_rules()->mode == MODE_GUN_GAME ? (mine + 1 > gun_count ? gun_count : mine + 1) : mine > 99 ? 99 : mine; /* Gun Game: the level, from 1 */
    if (k < -9) k = -9; /* (below 0: suicides in Deathmatch, a minus where the tens go) */
    mem_w8(&M.cpu, DM_TEXT + 6, (uint8_t)(k < 0 ? '-' : k >= 10 ? '0' + k / 10 : ' '));
    mem_w8(&M.cpu, GA(0x8016475Cu) + 20, (uint8_t)((k < 0 ? -k : k) % 10 + 1));
    if (dm_ended) return;
    int goal = battle_goal(m, battle_rules()->mode), off = mode_is(MODE_KING_OF_THE_HILL) || mode_is(MODE_ODDBALL) ? 100 : 10;
    goal = goal ? goal : m->time_limit ? 0 : off; /* both off: 10 (points: 100) */
    if (battle_rules()->mode == MODE_GUN_GAME) goal = GUN_LEVELS; /* through every weapon */
    int lead = 0, my_team = m->player_team[0] % BATTLE_TEAM_COLORS, reached = 0;
    if (teams) {
        int top = vp_dm_team_winner();
        lead = top == my_team;
        reached = goal > 0 && car_kills_team(top, NULL, NULL) >= goal;
    } else {
        lead = mine >= best;
        reached = goal > 0 && best >= goal;
    }
    int timeout = m->time_limit > 0 && vp_gv() - dm_start >= 3600ull * (uint64_t)m->time_limit;
    if (m->time_limit > 0 && !killcam_replaying) { /* the last seconds: a tick each (the menus' cursor), the explosion at 0 */
        uint64_t total = 3600ull * (uint64_t)m->time_limit, gone = vp_gv() - dm_start;
        int secs = gone >= total ? 0 : (int)((total - gone + 59) / 60);
        static int last_secs = -1;
        if (secs != last_secs && secs <= 5 && last_secs >= 0)
            guest_call(GA(0x800D3334u), secs ? 28 : 0x1D, 0, 0, 99);
        last_secs = secs;
    }
    if (!reached && !timeout) return;
    dm_ended = 1;
    fprintf(stderr, "%s: %s, the player %s\n", battle_mode_names[battle_rules()->mode], reached ? "the goal reached" : "time up",
            lead ? "wins" : "loses");
    if (!reached) killcam_forget(); /* time up: no kill cam (a kill that ends it has one) */
    vp_port_event = 1;
    /* (the goal reached by a kill: the round's end after its kill cam can start, 50 steps after the kill (killcam.c), as
     * Last Car Standing's last kill; a second only, the round was over first: no kill cam, the user's 2026-11-01) */
    guest_call(GA(0x800D181Cu), lead ? 3 : 1, reached && vp_on(VP_FINAL_KILL_CAM) ? 200 : 60, 0, 0);
}

/* ---- Hunted, Juggernaut, Gun Game (on the Deathmatch above) ----
 * Hunted: one car is the target and scores a point a second alive; its
 * killer becomes the target. Juggernaut: one car is the Juggernaut, takes
 * half damage and is the only one scoring (a point a kill); its killer
 * becomes the Juggernaut. In both the computer cars hunt it (it hunts the
 * nearest). Gun Game: each car has only the weapon of its level (the
 * seven in turn, as many as it wants, no specials; pickups give none), a
 * kill is the next level (its points); a kill with the last one wins.
 * Cars by their object slots (human +4, computer car +6). */
static int specials[4] = {-1, -1, -1, -1}, special_n = 1; /* the targets (Hunted: 1-4) or the Juggernaut, -1 none */
#define special_slot specials[0]
/* Hunted with teams, Swap team (the game type's hunted_teams): the targets (as many as the game type's) all on one team,
 * hunted_team; a kill of one by another team's car: the killer and others of its team at random the targets.
 * hunted_team -1: none yet (special_fill picks a team at random). */
static int hunted_team = -1;
static int teams_on(void);
static int team_hunt(void) { const GameType *t = battle_rules(); return mode_is(MODE_ODDBALL) && t && t->hunted_teams && teams_on(); }
static int is_special(int slot)
{
    for (int i = 0; i < special_n; i++) if (slot >= 0 && specials[i] == slot) return 1;
    return 0;
}
/* Juggernaut, Hunted without teams: two sides, the special cars (1) and the others (0) (the user's 2026-10-29: lock-on, and
 * in Juggernaut the others never hurt each other); -1: no sides (another mode, teams, or none special yet). slot -1: whether
 * there are sides */
static int mode_side(int slot)
{
    if (!(mode_is(MODE_JUGGERNAUT) || (mode_is(MODE_ODDBALL) && !teams_on()))) return -1;
    int any = 0;
    for (int i = 0; i < special_n; i++) any |= specials[i] >= 0;
    if (!any) return -1;
    return slot < 0 ? 0 : is_special(slot);
}
static uint64_t hunted_tick, mode_retarget;
/* Juggernaut: the damage each car did to the Juggernaut in its life (by slot): who takes over when it kills
 * itself (its own weapons, a fall) */
#define JUGG_SLOTS 32
static int slot_alive(int slot);
static int32_t jugg_damage[JUGG_SLOTS];
static int32_t jugg_frac; /* the Juggernaut's toughness: the part of a point of damage carried to its next hit */
static int jugg_top_damager(void)
{
    int best = -1;
    for (int s = 0; s < JUGG_SLOTS; s++)
        if (jugg_damage[s] > 0 && s != special_slot && slot_alive(s) && (best < 0 || jugg_damage[s] > jugg_damage[best])) best = s;
    return best;
}
int vp_suicide_costs_kill(void) /* profile/feat_hooks.c: Deathmatch: a car that kills itself loses a kill */
{
    return vp_dm() && mode_is(MODE_DEATHMATCH);
}
/* by GUN_x: the game's weapon kind (0: none, a ram kill), the kill's attack (feat_hooks.c A_x), the HUD's word */
static const int gun_kind_of[GUN_COUNT] = {1, 2, 6, 3, 7, 5, 4, 0, 0};
static const int gun_attack_of[GUN_COUNT] = {2, 3, 9, 7, 10, 5, 4, 11, 1};
/* a level (gun_seq): a weapon's kind (0: none), its kill's attack (a special level: A_SPECIAL, 12), the special's type */
static int gun_kind(int g) { return g < GUN_COUNT ? gun_kind_of[g] : 0; }
static int gun_attack(int g) { return g < GUN_COUNT ? gun_attack_of[g] : 12; }
static int gun_special_type(int g) { return g >= GUN_COUNT ? (g - GUN_COUNT) << 4 : 0; }
unsigned vp_arena_options(void) /* four.c, at the load: decomp request 38 (ARENA_SWITCHES.md)'s portArenaOptions (bit 0 hazards off, bit 1
                                 * destructibles off) from the game type; a tournament or Survival: 0, the arena as retail */
{
    const GameType *t = battle_rules();
    if (!t || ui_survival()) return 0;
    return (t->hazards ? 0u : 1u) | (t->destructibles ? 0u : 2u);
}
unsigned vp_gun_specials(void) /* four.c, at the load: the special levels (bits type >> 4), 0 when not Gun Game */
{
    const GameType *t = battle_rules();
    return mode_is(MODE_GUN_GAME) && t && port_sym("portCarSpecialAsSet") ? t->gun_specials & BATTLE_SPECIAL_AS : 0;
}
static int gun_level_given[2 * CARS_MAX]; /* by car (humans 0.., computer cars from CARS_HUMANS_MAX): the level whose ammo was given (+1) */
static void gun_round_start(void) /* the game type's list, in its order */
{
    const GameType *t = battle_rules();
    gun_count = 0;
    /* the weapons in the order chosen, the specials (if the build has them) after them (strongest first: before), then
     * the ram kill and the machine gun */
    unsigned specials = t && port_sym("portCarSpecialAsSet") ? t->gun_specials & BATTLE_SPECIAL_AS : 0;
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 0) == (t && t->gun_order)) { /* (the specials' pass) */
            for (int i = 0; i < BATTLE_SPECIAL_AS_COUNT; i++) { /* (strongest first, or weakest) */
                int ty = battle_special_order[t && t->gun_order ? i : BATTLE_SPECIAL_AS_COUNT - 1 - i];
                if ((specials >> ty) & 1) gun_seq[gun_count++] = GUN_COUNT + ty;
            }
            continue;
        }
        /* by their damage, weakest first (the user's 2026-11-17: the remote bomb earlier, napalm and homing at the end of
         * strongest first): homing 7 and fire 10 (Vanilla Plus), napalm, lightning (10 + 10), the ricochet (7 up as it
         * ages), the remote bomb (25 near), the power missile */
        static const int by_damage[GUN_WEAPONS] = {GUN_HOMING, GUN_FIRE, GUN_NAPALM, GUN_LIGHTNING, GUN_RICOCHET, GUN_REMOTE, GUN_POWER};
        for (int i = 0; i < GUN_WEAPONS; i++) {
            int g = by_damage[t && t->gun_order ? GUN_WEAPONS - 1 - i : i];
            if (g == GUN_LIGHTNING) continue; /* (not on every arena) */
            if (!t || (t->gun_levels >> g) & 1) gun_seq[gun_count++] = g;
        }
    }
    for (int g = GUN_WEAPONS; g < GUN_COUNT; g++)
        if (g != GUN_RAM && (!t || (t->gun_levels >> g) & 1)) gun_seq[gun_count++] = g;
    if (!gun_count) gun_seq[gun_count++] = GUN_FIRE;
    memset(gun_level_given, 0, sizeof gun_level_given);
    char list[200];
    int at = 0;
    for (int i = 0; i < gun_count && at < (int)sizeof list - 24; i++)
        at += snprintf(list + at, sizeof list - (size_t)at, "%s%s", i ? ", " : "",
                       gun_seq[i] < GUN_COUNT ? battle_gun_names[gun_seq[i]] : "special");
    fprintf(stderr, "gun game: %d levels: %s\n", gun_count, list);
}
static int mode_is(int mode) { const GameType *t = battle_rules(); return t && !ui_survival() && t->mode == mode; }
static int slot_count(int *slots) /* every car's slot */
{
    int n = 0, humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) slots[n++] = (int16_t)mem_r16(&M.cpu, HUMAN_REC(i) + 4);
    for (int k = 0; k < ais && k < cars_ai_max(); k++) slots[n++] = (int16_t)mem_r16(&M.cpu, AI_REC(k) + 6);
    return n;
}
static int slot_human(int slot) /* 0-3, or -1 */
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) if ((int16_t)mem_r16(&M.cpu, HUMAN_REC(i) + 4) == slot) return i;
    return -1;
}
static int slot_ai(int slot)
{
    int ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < ais && k < cars_ai_max(); k++) if ((int16_t)mem_r16(&M.cpu, AI_REC(k) + 6) == slot) return k;
    return -1;
}
static int slot_alive(int slot)
{
    int h = slot_human(slot), k = slot_ai(slot);
    return h >= 0 ? !mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C) : k >= 0 && ai_alive(k);
}
/* aiUpdateMood (800E8A84): with no human car alive in a custom match (every player out, watching), a computer car that
 * rolls "flee" (state 3) hunts instead: a hurt driver mostly flees its target, and with only computer cars left two hurt
 * ones fled each other for good (the user's 2026-10-30, Cyburbia's team match: Mr. Grimm and Outlaw 2 turning back and
 * forth). Retail always had a player alive to end it. No rand() of its own: the battle the same on every machine. */
/* And a car never flees twice in a row: a flee roll as a flee ends hunts instead, so a hurt car runs for one flee time,
 * then goes back on the offensive (the user's 2026-10-31: some matches might never end). */
void F_800E8A84_orig(CPU *c);
void vp_AiUpdateMood(CPU *c)
{
    uint32_t rec = c->r[4];
    int was = mem_r8(&M.cpu, rec + 0x01);
    F_800E8A84_orig(c);
    static int retail = -1; /* (tests: TM2PC_RETAIL_MOODS=1, the game's moods always) */
    if (retail < 0) retail = getenv("TM2PC_RETAIL_MOODS") != NULL;
    if (retail || !battle_rules() || mem_r8(&M.cpu, rec + 0x01) != 3) return;
    int again = was == 3 && mem_r16(&M.cpu, rec + 0x10) == 0; /* (the clock just reset: a new roll, flee after a flee) */
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int h = 0; h < humans && h < CARS_HUMANS_MAX && !again; h++)
        if (!mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C)) return; /* (a player alive: the game's own moods) */
    mem_w8(&M.cpu, rec + 0x01, 1);                                    /* hunt, */
    mem_w16(&M.cpu, rec + 0x12, mem_r16(&M.cpu, rec + 0x582));        /* for its hunt time */
    mem_w16(&M.cpu, rec + 0x10, 0);
}
static void slot_pos(int slot, double *x, double *y)
{
    int h = slot_human(slot), k = slot_ai(slot);
    uint32_t pos = h >= 0 ? HUMAN_REC(h) + 0x6FC : AI_REC(k < 0 ? 0 : k) + 0x73C;
    *x = (int32_t)mem_r32(&M.cpu, pos);
    *y = (int32_t)mem_r32(&M.cpu, pos + 4);
}
/* ---- the Juggernaut's car (the game type's: Minion by default; decomp request 32 step 3, CAR_GROUPS.md 8.6) ----
 * The car is loaded as one opponent more, the last (ui_AiSelect); after every bind its record is parked out of play
 * (portCarParkAi) and left out of the match (aiCarTotal ends before it). Each new Juggernaut takes the car over where
 * it is (portCarSetGroup: the cars exchange places, pickups kept, its HP its own or the car's full), the one before
 * getting its own car back first. Between frames (the battle frame), never inside a kill. Cars are known by their
 * slots here: a swap moves the record to the car's slot, and the Juggernaut, the tally and the kill credits follow. */
static int jc_opp = -1;    /* the opponent (AI record) holding the car while nobody drives it, -1 none */
static int jc_pending;     /* a new Juggernaut to put in it */
static int jc_driver = -1; /* the record in it: human i, AI 100 + j; -1 none */
int vp_jugg_car_swap(void) { return port_sym("portCarSetGroup") && port_sym("portCarParkAi") && port_sym("portCarBindGroup"); }
int vp_minion_opponent(void) { return jc_opp; }
void vp_jugg_car_set(int opp)
{
    jc_opp = opp;
    jc_driver = -1;
    jc_pending = 0;
}
static int jc_humans(void) { int h = four_humans(); return h > 2 ? h : mem_r8(&M.cpu, SETTINGS_BLOCK + 4); }
static int jc_group(int human, int i) { return human ? i + 1 : jc_humans() + i + 1; } /* (as the load numbers them) */
static int jc_slot(int human, int i) { return (int16_t)mem_r16(&M.cpu, human ? HUMAN_REC(i) + 4 : AI_REC(i) + 6); }
void vp_jugg_car_unbind(void) /* before every bind: every record its own car (each round starts so) */
{
    uint32_t t = port_sym("portCarBindGroup");
    if (t) for (uint32_t i = 0; i < 48; i++) mem_w8(&M.cpu, t + i, 0);
}
void vp_jugg_car_bound(void) /* after every bind: the car parked, out of the match */
{
    jc_driver = -1;
    jc_pending = 0;
    match_slots_home();
    if (jc_opp < 0) return;
    uint32_t total = GA(0x8018101Cu);
    if ((int)mem_r32(&M.cpu, total) != jc_opp + 1) {
        fprintf(stderr, "juggernaut car: record %d is not the last (%d records): off\n", jc_opp, (int)mem_r32(&M.cpu, total));
        jc_opp = -1;
        return;
    }
    guest_call(port_sym("portCarParkAi"), (uint32_t)jc_opp, 0, 0, 0);
    mem_w32(&M.cpu, total, (uint32_t)jc_opp);
    fprintf(stderr, "juggernaut car: record %d parked (group %d)\n", jc_opp, jc_group(0, jc_opp));
}
static void jc_swap(int human, int i, int group, int hp) /* portCarSetGroup, the parked record counted for it */
{
    uint32_t total = GA(0x8018101Cu), n = mem_r32(&M.cpu, total);
    int was = jc_slot(human, i);
    mem_w32(&M.cpu, total, (uint32_t)jc_opp + 1);
    int done = (int)guest_call(port_sym("portCarSetGroup"), (uint32_t)(human != 0), (uint32_t)i, (uint32_t)group, (uint32_t)hp);
    mem_w32(&M.cpu, total, n);
    int now = jc_slot(human, i);
    for (int s = 0; s < 4; s++) if (specials[s] == was) specials[s] = now;
    if (!killcam_replaying) match_slot_moved(was, now); /* (the tally: the present's, not a replay's) */
    if (!killcam_replaying) fprintf(stderr, "juggernaut car: %s %d to group %d: %s (slot %d to %d)\n", human ? "player" : "computer car",
                                  i + 1, group, done ? "done" : "nothing", was, now);
}
static void jc_frame(void) /* the battle frame: a new Juggernaut in the car */
{
    if (jc_opp < 0 || !jc_pending) return;
    jc_pending = 0;
    if (jc_driver >= 0) { /* the one before: its own car back (dead or alive; a wreck stays one) */
        int human = jc_driver < 100, i = human ? jc_driver : jc_driver - 100;
        jc_driver = -1;
        jc_swap(human, i, jc_group(human, i), -1);
    }
    int h = slot_human(special_slot), k = slot_ai(special_slot);
    if (special_slot < 0 || (h < 0 && k < 0)) return;
    jc_swap(h >= 0, h >= 0 ? h : k, jc_group(0, jc_opp), battle_rules()->jugg_full_heal ? 0x7FFF : -1);
    jc_driver = h >= 0 ? h : 100 + k;
}
static void hill_round_start(void);
static void hill_frame(void);
static void special_became(int slot);
static void hunted_team_became(int team, int killer);
static void special_fill(void) /* each missing target (or the Juggernaut): a car in play at random */
{
    if (team_hunt()) { /* Swap team: the hunted team with none in play (the start, or all out): a team with a car in play */
        int slots[CARS_HUMANS_MAX + CARS_MAX], n = slot_count(slots), live = 0, teams[CARS_HUMANS_MAX + CARS_MAX], m = 0;
        for (int i = 0; i < n; i++) if (slot_alive(slots[i]) && hunted_team >= 0 && vp_team_of_slot(slots[i]) == hunted_team) live = 1;
        if (!live) {
            for (int i = 0; i < n; i++) if (slot_alive(slots[i]) && vp_team_of_slot(slots[i]) >= 0) teams[m++] = vp_team_of_slot(slots[i]);
            if (m) hunted_team_became(teams[(int)vp_rand() % m], -1);
            return;
        }
    }
    for (int s = 0; s < special_n; s++) {
        if (specials[s] >= 0) continue;
        int slots[CARS_HUMANS_MAX + CARS_MAX], n = slot_count(slots), live[CARS_HUMANS_MAX + CARS_MAX], m = 0;
        for (int i = 0; i < n; i++) /* (Swap team: of the hunted team) */
            if (slot_alive(slots[i]) && !is_special(slots[i]) && (!team_hunt() || vp_team_of_slot(slots[i]) == hunted_team))
                live[m++] = slots[i];
        specials[s] = m ? live[(int)vp_rand() % m] : -1;
        const char *force = getenv("TM2PC_SPECIAL_SLOT"); /* (tests: this car slot first, while it is in play) */
        for (int i = 0; force && s == 0 && i < m; i++) if (live[i] == atoi(force)) specials[s] = live[i];
        if (specials[s] >= 0) special_became(specials[s]);
    }
}
static void mode_round_start(void)
{
    for (int i = 0; i < 4; i++) specials[i] = -1;
    hunted_team = -1;
    shim_slot = -1; /* (a new round: every car shown by the game) */
    special_n = mode_is(MODE_ODDBALL) ? battle_rules()->hunted_count : 1;
    if (special_n < 1) special_n = 1;
    if (special_n > 4) special_n = 4;
    hunted_tick = vp_gv();
    mode_retarget = 0;
    memset(jugg_damage, 0, sizeof jugg_damage);
    jugg_frac = 0;
    gun_round_start();
    hill_round_start();
    if (mode_is(MODE_JUGGERNAUT) || mode_is(MODE_ODDBALL)) special_fill();
}
static void kill_feed_add(int own, int killer, int attack);
/* A car destroyed: the feed, the tally and the mode's rules. In the kill cam's replays only the rules (who is the
 * Juggernaut or a target: kept with the kill cam's machine, so a change inside the replayed seconds plays again, the
 * Juggernaut's car handed over as it was); the feed, the tally and the messages are the present's. */
void vp_car_destroyed(int own, int killer, int attack)
{
    int replay = killcam_replaying;
    extern int rbs_resimulating; /* net/rb_state.c: a step run again (rollback): its feed was shown the first time */
    if (!replay && !rbs_resimulating) kill_feed_add(own, killer, attack); /* the feed in a Challenge Match; YOU KILLED X everywhere */
    if (!vp_dm()) return;
    int kill = killer >= 0 && killer != own;
    int before = special_slot;
    if (mode_is(MODE_GUN_GAME) && kill) { /* a kill with the level's weapon (or a ram kill, on its level) */
        int lvl = match_points_of(killer), g = gun_seq[lvl < 0 ? 0 : lvl >= gun_count ? gun_count - 1 : lvl];
        /* (a special level: the car has only the special besides the machine gun and ramming, and a special's kill is
         * told as its projectile's kind, Sweet Tooth's spiral a napalm, Minion's freeze a freeze: any other kill counts) */
        int ok = g >= GUN_COUNT ? attack != 1 && attack != 11 && attack != 0 : attack == gun_attack(g); /* (13, other: a taser by its shooter's own type) */
        if (ok && !replay) match_add_points(killer, 1);
    } else if (mode_is(MODE_JUGGERNAUT)) {
        if (own == special_slot) { /* the Juggernaut's killer takes its place; killed by itself: who hurt it most this life */
            special_slot = kill ? killer : jugg_top_damager();
            if (!kill && special_slot >= 0 && !replay) fprintf(stderr, "juggernaut: killed itself; car %d (most damage, %d) takes over\n",
                                                    special_slot, jugg_damage[special_slot]);
        }
        else if (kill && killer == special_slot && !replay) match_add_points(killer, 1);
    } else if (mode_is(MODE_ODDBALL)) { /* the target's killer is the target (not if already one) */
        if (team_hunt()) { /* Swap team: a target killed by another team's car: that team has the targets */
            int kt = kill ? vp_team_of_slot(killer) : -1;
            if (is_special(own) && kt >= 0 && kt != hunted_team) { hunted_team_became(kt, killer); return; }
            for (int i = 0; i < special_n; i++) if (own == specials[i]) specials[i] = -1; /* (another of its team: special_fill) */
            return;
        }
        int enemy = !teams_on() || vp_team_of_slot(killer) != vp_team_of_slot(own); /* (with teams: not a teammate) */
        for (int i = 0; i < special_n; i++)
            if (own == specials[i]) {
                specials[i] = kill && enemy && !is_special(killer) ? killer : -1;
                if (specials[i] >= 0) special_became(specials[i]);
            }
        return;
    }
    if (special_slot != before && special_slot >= 0) special_became(special_slot);
}
int vp_mode_damage(int victim_slot, int attacker_slot, int ram, int amount)
{
    if (!mode_is(MODE_JUGGERNAUT) || special_slot < 0) return amount;
    const GameType *t = battle_rules();
    if (victim_slot == special_slot && amount > 0) {
        /* the toughness on every hit, the fraction carried to the next: a 1-point hit (the machine gun's) was left whole, so
         * at 400% the gun still did its full damage (KinggzZ's, 2026-11-24); now each 4th bullet counts. (In the state:
         * the same on every machine, and again in a replay) */
        int h = t->jugg_health > 0 ? t->jugg_health : 100;
        int total = amount * 100 + jugg_frac;
        amount = total / h;
        jugg_frac = total % h;
    } else if (attacker_slot == special_slot && attacker_slot != victim_slot) {
        amount = amount * (ram ? t->jugg_ram : t->jugg_weapon) / 100;
        if (amount < 1) amount = 1;
    }
    if (victim_slot == special_slot && attacker_slot >= 0 && attacker_slot < JUGG_SLOTS && attacker_slot != victim_slot)
        jugg_damage[attacker_slot] += amount;
    return amount;
}
/* carGroundEffects (800ED128, decomp car_wheels.c): lava takes 3 HP every 61 ticks straight from the record, outside
 * carApplyDamage: the Juggernaut's toughness on it too, as on the burning (profile/feat_hooks.c). Sinking in deep water
 * (its HP drained as the car goes under) is left as it is: a way out of the arena, not damage. */
void F_800ED128_orig(CPU *c);
void vp_GroundEffects(CPU *c) /* carGroundEffects(car, human) */
{
    uint32_t rec = c->r[4];
    int human = (c->r[5] & 0xFF) != 0;
    uint32_t hp_at = rec + (human ? 0x1F0u : 0x5A0u), sink_at = rec + (human ? 0xC0u : 0x90u); /* (stats +0xA4, state +0x24) */
    int32_t before = (int32_t)mem_r32(c, hp_at);
    int sinking = mem_r8(c, sink_at);
    F_800ED128_orig(c);
    int32_t now = (int32_t)mem_r32(c, hp_at);
    if (now >= before || sinking || mem_r8(c, sink_at) || !mode_is(MODE_JUGGERNAUT)) return;
    int taken = vp_mode_damage((int16_t)mem_r16(c, rec + (human ? 4u : 6u)), -1, 0, before - now);
    if (taken != before - now) mem_w32(c, hp_at, (uint32_t)(before - taken));
}
static char kill_text[CARS_HUMANS_MAX][40];       /* (the kill feed's, below) */
static uint64_t kill_until[CARS_HUMANS_MAX];
static int flash_skip;
/* a new Juggernaut or target: full health (the game type's), the flash and the sound */
static void hunted_team_became(int team, int killer) /* Swap team: this team has the targets: the killer, others at random */
{
    hunted_team = team;
    for (int i = 0; i < special_n; i++) specials[i] = -1;
    if (killer >= 0 && slot_alive(killer)) specials[0] = killer;
    int slots[CARS_HUMANS_MAX + CARS_MAX], n = slot_count(slots), live[CARS_HUMANS_MAX + CARS_MAX], m = 0;
    for (int i = 0; i < n; i++) if (slot_alive(slots[i]) && slots[i] != killer && vp_team_of_slot(slots[i]) == team) live[m++] = slots[i];
    for (int s = 0; s < special_n && m; s++) {
        if (specials[s] >= 0) continue;
        int j = (int)(vp_rand() % (unsigned)m);
        specials[s] = live[j];
        live[j] = live[--m];
    }
    extern int rbs_resimulating; /* (rollback: a step run again showed its messages the first time) */
    if (killcam_replaying || rbs_resimulating) return;
    snprintf(flash_text, sizeof flash_text, "%s TEAM HAS THE TARGET%s", battle_team_names[team % BATTLE_TEAM_COLORS],
             special_n > 1 ? "S" : "");
    flash_until = vp_gv() + 150;
    flash_skip = -1;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int h = 0; h < humans && h < CARS_HUMANS_MAX; h++)
        if (vp_team_of_slot((int16_t)mem_r16(&M.cpu, HUMAN_REC(h) + 4)) == team) {
            if (is_special((int16_t)mem_r16(&M.cpu, HUMAN_REC(h) + 4))) snprintf(kill_text[h], sizeof kill_text[h], "YOU ARE A TARGET");
            else snprintf(kill_text[h], sizeof kill_text[h], "PROTECT YOUR TARGET%s", special_n > 1 ? "S" : "");
            kill_until[h] = vp_gv() + 300;
        }
    guest_call(GA(0x800D3334u), 0x1D, 0, 0, 99); /* the select explosion */
}
static void special_became(int slot)
{
    memset(jugg_damage, 0, sizeof jugg_damage);
    jugg_frac = 0;
    if (mode_is(MODE_JUGGERNAUT)) jc_pending = 1; /* into the Juggernaut's car (the next battle frame) */
    int h = slot_human(slot), k = slot_ai(slot);
    if (mode_is(MODE_JUGGERNAUT) && battle_rules()->jugg_full_heal) {
        if (h >= 0) mem_w32(&M.cpu, HUMAN_REC(h) + 0x1F0, (uint32_t)(int16_t)mem_r16(&M.cpu, HUMAN_REC(h) + 0x186));
        else if (k >= 0) mem_w32(&M.cpu, AI_REC(k) + 0x5A0, (uint32_t)(int16_t)mem_r16(&M.cpu, AI_REC(k) + 0x536));
    }
    extern int rbs_resimulating;
    if (killcam_replaying || rbs_resimulating) return; /* (a replay, a step run again: the rules only; the messages and the
                                                        * sound were the battle's) */
    snprintf(flash_text, sizeof flash_text, "NEW %s", mode_is(MODE_JUGGERNAUT) ? "JUGGERNAUT" : "TARGET");
    flash_until = vp_gv() + 150;
    flash_skip = h;
    if (h >= 0 && h < CARS_HUMANS_MAX) { /* the player who became it: their own message, 5 seconds */
        snprintf(kill_text[h], sizeof kill_text[h], "YOU ARE THE %s", mode_is(MODE_JUGGERNAUT) ? "JUGGERNAUT" : "TARGET");
        kill_until[h] = vp_gv() + 300;
    }
    guest_call(GA(0x800D3334u), 0x1D, 0, 0, 99); /* the select explosion (0x1B, 27, is the menus' music loop) */
}
/* A special level: the car fires that car's special, whatever its own (portCarSpecialAsSet, the decomp's request 35,
 * GUN_GAME_SPECIALS.md 4.5; between frames). Set at a new level with its ammo, and again whenever the build has the
 * slot back on its own (a respawn keeps it; a swap or a drop-in resets it). A weapon level: its own special back. */
static void gun_special_set(uint32_t rec, int human, int want, int fresh, const GameType *t)
{
    static uint32_t set_fn = 1, as_tab;
    if (set_fn == 1) { set_fn = port_sym("portCarSpecialAsSet"); as_tab = port_sym("portCarSpecialAs"); }
    if (!set_fn || !as_tab) return;
    int slot = (int16_t)mem_r16(&M.cpu, rec + (human ? 4u : 6u)), own = (int)mem_r32(&M.cpu, rec + (human ? 0x90u : 0x30u)) & 0xF0;
    if (slot < 0 || slot >= 64) return;
    if (want == own) want = -1; /* (its own special: as it is) */
    int cur = (int16_t)mem_r16(&M.cpu, as_tab + 2u * (uint32_t)slot);
    if (cur <= 0 || cur == own) cur = -1;
    if (!fresh && cur == want) return;
    int ammo = want < 0 && !fresh ? -1 : t->gun_infinite ? 9 : t->gun_ammo;
    int r = (int)guest_call(set_fn, (uint32_t)slot, (uint32_t)want, (uint32_t)ammo, 0);
    if (r != 1) {
        static uint64_t said;
        if (vp_gv() >= said + 600) { /* (once in a while: it is asked again each frame) */
            said = vp_gv();
            uint32_t psa = port_sym("portSpecialAs");
            fprintf(stderr, "gun game: car slot %d: special of type %02X refused (%d); load mask %04X, options %02X, spares %d from %d\n",
                    slot, want & 0xFF, r, mem_r32(&M.cpu, port_sym("portCarSpecialLoad")), mem_r32(&M.cpu, port_sym("portCarOptions")),
                    psa ? (int)mem_r32(&M.cpu, psa + 0x1C00) : -1, psa ? (int)mem_r32(&M.cpu, psa + 0x1C04) : -1);
        }
    }
}
/* The Juggernaut's invisibility (the game type's jugg_invisible): it shimmers, barely seen. Its car object is hidden
 * (+0x08, as the invisibility attack hides it at its 95) and its body node taken out of the world (carBodyObjectHide
 * 80114E88, by its car key), but for a frame now and then, at uneven times, with the attack's blue sparks round it
 * (fxSparkBlue 80111384) every few frames: a ripple in the air where it drives. The one before is shown again. */
static void shim_show(int slot, int show)
{
    int h = slot_human(slot), k = slot_ai(slot);
    if (h < 0 && k < 0) return;
    uint32_t obj = car_slot_object(slot);
    if (obj >= 0x80000000u && obj < 0x80000000u + GUEST_RAM_BYTES) mem_w32(&M.cpu, obj + 0x08, (uint32_t)show);
    uint32_t key_fn = port_sym("portCarKey"), type = mem_r32(&M.cpu, h >= 0 ? HUMAN_REC(h) + 0x90 : AI_REC(k) + 0x30) & 0xFF;
    uint32_t key = key_fn ? guest_call(key_fn, (uint32_t)slot, type, 0, 0) : type;
    guest_call(GA(show ? 0x80114E10u : 0x80114E88u), key, 0, 0, 0); /* carBodyObjectShow / Hide */
}
static int shim_seen = 1;
static void slot_name(int slot, char *out, size_t n);
void vp_shim_view(int view, int before) /* four.c, round each view's renderScene: a player who is the Juggernaut sees their own
                                         * car; every other view, hidden again (a player's car object is shown by the game's
                                         * own view code each frame, after the mode's frame) */
{
    if (shim_slot < 0 || shim_seen) return;
    int own = slot_human(shim_slot) == view;
    if (before) shim_show(shim_slot, own);
    else if (own) shim_show(shim_slot, 0);
}
static void jugg_shimmer_frame(void)
{
    const GameType *t = battle_rules();
    int want = t && t->jugg_invisible && special_slot >= 0 && slot_alive(special_slot) ? special_slot : -1;
    if (shim_slot != want) {
        if (shim_slot >= 0) shim_show(shim_slot, 1);
        shim_slot = want;
        if (want >= 0) { char nm[24]; slot_name(want, nm, sizeof nm); fprintf(stderr, "ghost juggernaut: %s (slot %d)\n", nm, want); }
    }
    if (want < 0) return;
    uint64_t f = vp_gv() / 3; /* (a step) */
    int seen = f % 37 == 0 || f % 23 == 0; /* (a glimpse at uneven times) */
    shim_show(want, seen);
    shim_seen = seen;
    if (f % 4 == 0) { /* the sparks, somewhere round it */
        double x, y;
        slot_pos(want, &x, &y);
        int h = slot_human(want), k = slot_ai(want);
        int32_t z = (int32_t)mem_r32(&M.cpu, h >= 0 ? HUMAN_REC(h) + 0x704 : AI_REC(k) + 0x744);
        const uint32_t at = DM_TEXT + 0x3E0; /* (a place for its argument) */
        mem_w32(&M.cpu, at, (uint32_t)(int32_t)(x + (int)(vp_rand() % 81) - 40));
        mem_w32(&M.cpu, at + 4, (uint32_t)(int32_t)(y + (int)(vp_rand() % 81) - 40));
        mem_w32(&M.cpu, at + 8, (uint32_t)(z + 10 + (int)(vp_rand() % 30)));
        guest_call(GA(0x80111384u), at, 1, 0, 0);
    }
}
static void gun_weapons(uint32_t rec, int human, int level, int car)
{
    const GameType *t = battle_rules();
    uint32_t block = rec + (human ? 0x7C8u : 0x808u);
    int g = gun_seq[level < 0 ? 0 : level >= gun_count ? gun_count - 1 : level], kind = gun_kind(g), special = gun_special_type(g);
    int fresh = gun_level_given[car] != level + 1; /* a new level: its ammo */
    gun_level_given[car] = level + 1;
    gun_special_set(rec, human, special ? special : -1, fresh, t);
    for (int k = special ? 1 : 0; k <= 7; k++) { /* (a special level: the special's count is its own, slot 0) */
        uint32_t at = block + 0x0E + 2u * (uint32_t)k;
        if (k != kind || !kind) mem_w16(&M.cpu, at, 0);
        else if (t->gun_infinite) mem_w16(&M.cpu, at, 9);
        else if (fresh) mem_w16(&M.cpu, at, (uint16_t)t->gun_ammo);
    }
    if (special && t->gun_infinite) mem_w16(&M.cpu, block + 0x0E, 9);
    if (kind || special) mem_w8(&M.cpu, block + 0x0D, (uint8_t)kind); /* (the level's weapon chosen; a special: slot 0) */
}
/* Gun Game: a computer car fires its level's weapon, not its own special. The game refills every computer car's
 * ammo each tick and fires the special when no weapon is chosen (aiWeaponFireDecide 80103CA0, then weaponTick's
 * refill): after the decision, "fire the special" becomes "fire the level's weapon" (a special level: its special). */
void F_80103CA0_orig(CPU *c);
void vp_AiFireDecide(CPU *c) /* aiWeaponFireDecide(ai record) */
{
    uint32_t rec = c->r[4];
    F_80103CA0_orig(c);
    if (!mode_is(MODE_GUN_GAME) || gun_count <= 0) return;
    int lvl = match_points_of((int16_t)mem_r16(&M.cpu, rec + 6));
    int g = gun_seq[lvl < 0 ? 0 : lvl >= gun_count ? gun_count - 1 : lvl], kind = gun_kind(g);
    if (gun_special_type(g)) return; /* (a special level: the special is the level's weapon) */
    const uint32_t FIRE = rec + 0x44 + 0x1A, FIRE_SPECIAL = rec + 0x44 + 0x1C, SELECTED = rec + 0x808 + 0x0D;
    if (mem_r8(&M.cpu, FIRE_SPECIAL)) {
        mem_w8(&M.cpu, FIRE_SPECIAL, 0);
        if (kind) mem_w8(&M.cpu, FIRE, 1);
    }
    if (kind) mem_w8(&M.cpu, SELECTED, (uint8_t)kind);
    else mem_w8(&M.cpu, FIRE, 0); /* the machine gun's level: its guns only (the game refills a computer car's pickups,
                                   * and their kills never counted there: it never won, Enough Boxes' 2026-11-22) */
}

/* Gun Game: a computer car fires its special only on a special's level (Twister's, Thumper's and Mr. Slam's AI fire
 * them up close, not through aiWeaponFireDecide: refused here, at weaponFireSpecial) */
int vp_gun_blocks_special(uint32_t car, int human)
{
    if (human || !mode_is(MODE_GUN_GAME) || gun_count <= 0) return 0;
    int lvl = match_points_of((int16_t)mem_r16(&M.cpu, car + 6));
    return !gun_special_type(gun_seq[lvl < 0 ? 0 : lvl >= gun_count ? gun_count - 1 : lvl]);
}

/* Gun Game with ammo: a weapon pickup refills the level's weapon (feat_Pickup) */
void vp_gun_resupply(uint32_t rec, int human)
{
    if (!mode_is(MODE_GUN_GAME) || battle_rules()->gun_infinite) return;
    int slot = (int16_t)mem_r16(&M.cpu, rec + (human ? 4u : 6u)), lvl = match_points_of(slot);
    int kind = gun_kind(gun_seq[lvl < 0 ? 0 : lvl >= gun_count ? gun_count - 1 : lvl]);
    if (kind) mem_w16(&M.cpu, rec + (human ? 0x7C8u : 0x808u) + 0x0E + 2u * (uint32_t)kind, (uint16_t)battle_rules()->gun_ammo);
}
/* Gun Game: a weapon pickup refills its taker's level weapon, so each view sees them as its player's (a weapon's
 * look, the special's globe; the machine gun and rams: as they are). Drawing only: round each view's renderScene
 * (four.c), the level's own models back after it (what is hit is the same on every machine). */
static uint32_t pickup_view_saved[50];
void vp_pickup_view(int view, int before)
{
    if (!mode_is(MODE_GUN_GAME) || gun_count <= 0 || !pickup_models_ready) return;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (view < 0 || view >= humans || view >= CARS_HUMANS_MAX) return;
    for (int n = 0; n < 50; n++) {
        uint32_t node = mem_r32(&M.cpu, PICKUP_OBJECTS + 4u * (uint32_t)n);
        if (!node || node != pickup_node[n]) continue;
        int weapon = 0;
        for (int w = 0; w < BATTLE_WEAPONS; w++) if (pickup_kind0[n] == PICKUP_ID[w]) weapon = 1;
        if (!weapon) continue;
        uint32_t at = pickup_child_at(node);
        if (!before) { if (pickup_view_saved[n]) mem_w32(&M.cpu, at, pickup_view_saved[n]); pickup_view_saved[n] = 0; continue; }
        int lvl = match_points_of((int16_t)mem_r16(&M.cpu, HUMAN_REC(view) + 4));
        int g = gun_seq[lvl < 0 ? 0 : lvl >= gun_count ? gun_count - 1 : lvl], kind = gun_kind(g);
        uint32_t look = gun_special_type(g) ? pickup_special_look() : 0;
        for (int w = 0; kind && w < BATTLE_WEAPONS; w++) if (WEAPON_ID[w] == kind) look = pickup_model[PICKUP_ID[w] - 400];
        if (!look) continue;
        pickup_view_saved[n] = mem_r32(&M.cpu, at);
        mem_w32(&M.cpu, at, look);
    }
}
static void mode_frame(void)
{
    if (mode_is(MODE_GUN_GAME)) {
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
        for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++)
            gun_weapons(HUMAN_REC(i), 1, match_points_of((int16_t)mem_r16(&M.cpu, HUMAN_REC(i) + 4)), i);
        for (int k = 0; k < ais && k < cars_ai_max(); k++)
            if (ai_alive(k)) gun_weapons(AI_REC(k), 0, match_points_of((int16_t)mem_r16(&M.cpu, AI_REC(k) + 6)), CARS_HUMANS_MAX + k);
        return;
    }
    if (mode_is(MODE_KING_OF_THE_HILL)) { hill_frame(); return; }
    if (!mode_is(MODE_JUGGERNAUT) && !mode_is(MODE_ODDBALL)) return;
    if (mode_is(MODE_JUGGERNAUT)) jc_frame();
    if (mode_is(MODE_JUGGERNAUT)) jugg_shimmer_frame();
    if (mode_is(MODE_JUGGERNAUT)) { /* the players: infinite weapons while the Juggernaut (the game type's) */
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
        for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++) {
            int jugg = (int16_t)mem_r16(&M.cpu, HUMAN_REC(i) + 4) == special_slot;
            mem_w32(&M.cpu, HUMAN_REC(i) + 0x7C8, (uint32_t)((jugg && battle_rules()->jugg_infinite) || battle_rules()->infinite));
        }
    }
    special_fill(); /* none (it went without a killer): a new one */
    if (mode_is(MODE_ODDBALL) && vp_gv() >= hunted_tick + 60) { /* the targets: a point a second alive */
        hunted_tick += 60;
        for (int i = 0; i < special_n; i++) if (specials[i] >= 0 && slot_alive(specials[i])) match_add_points(specials[i], 1);
    }
    if (mode_is(MODE_ODDBALL) && !battle_rules()->hunted_attack) { /* targets that may not fire: only the machine gun and ramming */
        int slots[CARS_HUMANS_MAX + CARS_MAX], n = slot_count(slots);
        for (int i = 0; i < n; i++) {
            if (!is_special(slots[i])) continue;
            int h = slot_human(slots[i]), k = slot_ai(slots[i]);
            uint32_t block = h >= 0 ? HUMAN_REC(h) + 0x7C8 : k >= 0 ? AI_REC(k) + 0x808 : 0;
            for (int w = 0; block && w <= 7; w++) mem_w16(&M.cpu, block + 0x0E + 2u * (uint32_t)w, 0);
        }
    }
    if (vp_gv() < mode_retarget) return;
    mode_retarget = vp_gv() + 1; /* every frame: the game's own targeting (the nearest car) would win in between */
    int ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < ais && k < cars_ai_max(); k++) { /* the others: the nearest special car; a special one: the nearest other */
        if (!ai_alive(k)) continue;
        mem_w8(&M.cpu, AI_REC(k) + 0x034, 1);
        int slots[CARS_HUMANS_MAX + CARS_MAX], n = slot_count(slots), own = (int16_t)mem_r16(&M.cpu, AI_REC(k) + 6), mine = is_special(own), target = -1;
        double kx, ky, best_d = 1e30;
        slot_pos(own, &kx, &ky);
        if (mode_is(MODE_ODDBALL) && teams_on()) { /* teams: a target's teammates guard it, the others hunt the other teams' */
            int team = vp_team_of_slot(own), guard = -1;
            double gx = 0, gy = 0, gd = 1e30;
            for (int i = 0; i < n; i++) { /* (the nearest target of its own team, not itself) */
                if (slots[i] == own || !is_special(slots[i]) || !slot_alive(slots[i]) || vp_team_of_slot(slots[i]) != team) continue;
                double x, y;
                slot_pos(slots[i], &x, &y);
                double d = (x - kx) * (x - kx) + (y - ky) * (y - ky);
                if (d < gd) { gd = d; guard = slots[i]; gx = x; gy = y; }
            }
            int pick = -1, hunting = 0;
            if (guard >= 0 && !mine) { /* a guard: the enemy nearest its target, if one is close to it (else: hunt as the others) */
                for (int i = 0; i < n; i++) {
                    if (!slot_alive(slots[i]) || vp_team_of_slot(slots[i]) == team) continue;
                    double x, y;
                    slot_pos(slots[i], &x, &y);
                    double d = (x - gx) * (x - gx) + (y - gy) * (y - gy);
                    if (d < best_d) { best_d = d; pick = slots[i]; }
                }
                if (best_d > 2500.0 * 2500.0) pick = -1; /* (none near it: the nearest enemy target, or stay near) */
                hunting = pick >= 0;
            }
            for (int pass = 0; pick < 0 && pass < 2; pass++) { /* the nearest other team's target; a target, or none: the nearest enemy */
                best_d = 1e30;
                for (int i = 0; i < n; i++) {
                    if (slots[i] == own || !slot_alive(slots[i]) || vp_team_of_slot(slots[i]) == team) continue;
                    if (pass == 0 && (mine || !is_special(slots[i]))) continue;
                    double x, y;
                    slot_pos(slots[i], &x, &y);
                    double d = (x - kx) * (x - kx) + (y - ky) * (y - ky);
                    if (d < best_d) { best_d = d; pick = slots[i]; }
                }
                hunting = pick >= 0 && pass == 0;
            }
            target = pick >= 0 ? slot_ai(pick) : -1; /* (a player: -1, the nearest player) */
            mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k, (uint32_t)target);
            if (hunting) mem_w8(&M.cpu, AI_REC(k) + 0x001, 1);
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (slots[i] == own || !slot_alive(slots[i]) || is_special(slots[i]) == mine) continue;
            double x, y;
            slot_pos(slots[i], &x, &y);
            double d = (x - kx) * (x - kx) + (y - ky) * (y - ky);
            if (d < best_d) { best_d = d; target = slot_ai(slots[i]); } /* a player: -1, the nearest player */
        }
        mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k, (uint32_t)target);
        /* and they all hunt it: mood 1, the only one that drives at the target (the game's pack director keeps about
         * three hunting and sends the others to the farthest or a random route node: decomp AI_VS_AI.md 7). A computer
         * Juggernaut hunts too (the user's 2026-11-03: it drove aimlessly; always on the offensive now) */
        if (!mine || mode_is(MODE_JUGGERNAUT)) mem_w8(&M.cpu, AI_REC(k) + 0x001, 1);
    }
}
/* the HUD's line for the mode (player p): who is the target or the Juggernaut, Gun Game's weapon */
static int hill_status(int p, char *out, size_t n);
static const char *const type_names[16] = {"", "SWEET TOOTH", "HAMMERHEAD", "OUTLAW 2", "WARTHOG", "MR. GRIMM", "GRASSHOPPER",
    "THUMPER", "SPECTRE", "ROADKILL", "TWISTER", "AXEL", "MR. SLAM", "SHADOW", "MINION", "DARK TOOTH"};
static int mode_status(int p, char *out, size_t n)
{
    if (!vp_dm()) return 0;
    if (mode_is(MODE_DEATHMATCH) || mode_is(MODE_GUN_GAME)) return 0; /* (each player's in their lives spot: score_line_print) */
    if (mode_is(MODE_KING_OF_THE_HILL)) return hill_status(p, out, n);
    if (!mode_is(MODE_JUGGERNAUT) && !mode_is(MODE_ODDBALL)) return 0;
    const char *what = mode_is(MODE_JUGGERNAUT) ? "JUGGERNAUT" : "TARGET";
    for (int i = 0; i < special_n; i++)
        if (specials[i] >= 0 && slot_human(specials[i]) == p) { snprintf(out, n, "%s: YOU", what); return 1; }
    if (mode_is(MODE_ODDBALL) && teams_on()) { /* teams: whose targets (Swap team: one team's), and guarding your own */
        int mine = vp_team_of_slot((int16_t)mem_r16(&M.cpu, HUMAN_REC(p) + 4)), own = 0, theirs = 0; /* (p: the human) */
        for (int i = 0; i < special_n; i++)
            if (specials[i] >= 0) { if (vp_team_of_slot(specials[i]) == mine) own++; else theirs++; }
        if (own && !theirs) snprintf(out, n, "PROTECT YOUR TARGET%s", own > 1 ? "S" : "");
        else if (own) snprintf(out, n, "TARGETS: %d YOURS, %d THEIRS", own, theirs);
        else if (team_hunt() && hunted_team >= 0) snprintf(out, n, "%s: %s TEAM", special_n > 1 ? "TARGETS" : "TARGET",
                                                          battle_team_names[hunted_team % BATTLE_TEAM_COLORS]);
        else if (theirs) snprintf(out, n, "%d %s%s", theirs, what, theirs > 1 ? "S" : "");
        else snprintf(out, n, "NO %s", what);
        return 1;
    }
    if (special_n > 1) {
        int alive = 0;
        for (int i = 0; i < special_n; i++) alive += specials[i] >= 0;
        snprintf(out, n, "%d %sS", alive, what);
        return 1;
    }
    int h = slot_human(special_slot), k = slot_ai(special_slot);
    if (special_slot < 0) snprintf(out, n, "NO %s", what);
    else if (h == p) snprintf(out, n, "%s: YOU", what);
    else if (h >= 0) snprintf(out, n, "%s: PLAYER %d", what, h + 1);
    else snprintf(out, n, "%s: %s", what, type_names[(mem_r32(&M.cpu, AI_REC(k < 0 ? 0 : k) + 0x30) >> 4) & 15]);
    return 1;
}
/* A line in the HUD's font (2) and green, centered on x or from x; a minus raised (the font's dash sits on the line,
 * as an underscore: "KILLS: -1" with it in the middle) */
static void hud_text_print(CPU *c, int x, int y, const char *text, int centered)
{
    int len = (int)strlen(text);
    const uint32_t buf = DM_TEXT + 0x40;
    for (int i = 0; i <= len; i++) mem_w8(c, buf + (uint32_t)i, (uint8_t)text[i]);
    uint32_t font_was = vp_font_color_get(2);
    guest_call(GA(0x80114284u), 2, 36, 150, 36);                    /* fontSetColor: the HUD's green */
    const char *minus = strchr(text, '-');
    if (!minus) {
        guest_call(GA(centered ? 0x801136B8u : 0x801134D8u), 2, (uint32_t)x, (uint32_t)y, buf); /* fontPrintCenteredXY / XY */
    } else {
        int w = centered ? (int)guest_call(GA(0x8011355Cu), 2, buf, 0, 0) : 0; /* fontStringWidth */
        int at = (int)(minus - text);
        const uint32_t cursor_x = GA(0x80180B64u);                    /* fontCursorX */
        mem_w8(c, buf + (uint32_t)at, 0);
        guest_call(GA(0x801134D8u), 2, (uint32_t)(x - (w >> 1)), (uint32_t)y, buf); /* fontPrintXY: before it */
        mem_w8(c, buf + (uint32_t)at, '-');
        mem_w8(c, buf + (uint32_t)at + 1, 0);
        guest_call(GA(0x801134D8u), 2, mem_r32(c, cursor_x), (uint32_t)(y - 3), buf + (uint32_t)at); /* the dash, raised */
        for (int i = at + 1; i <= len; i++) mem_w8(c, buf + (uint32_t)i, (uint8_t)text[i]);
        guest_call(GA(0x801134D8u), 2, mem_r32(c, cursor_x), (uint32_t)y, buf + (uint32_t)at + 1); /* after it */
    }
    vp_font_color_put(2, font_was);
}
static void mode_status_print(CPU *c, int p, int x, int y)
{
    char text[48];
    if (mode_is(MODE_JUGGERNAUT) || mode_is(MODE_ODDBALL) || mode_is(MODE_KING_OF_THE_HILL)) return; /* (in the message bar: port_message) */
    if (!mode_status(p, text, sizeof text)) return;
    hud_text_print(c, x, y, text, 1);
}
/* Each player's score in the lives spot (the game's "Lives:" line: font 2, its green; one view centered on x 50, y 212;
 * split screen from x 11 near the view's bottom, where the game's split HUD prints a co-op tournament's): Deathmatch
 * "Kills: N", Juggernaut "Kills: N" (those made as the Juggernaut: its points), Gun Game "Level: N" (one view: "of M"),
 * King of the Hill and Hunted "Points: N" */
/* The score line in the lives' place ("Kills: 12", "Level: 4", "Points: 30"; a co-op tournament's other views, "Lives: 2"),
 * printed as the game prints "Lives: N", with its sprite buffers, so the HUD layout places it as the lives in every mode,
 * view and HUD setting (hud_layout.c knows them by address; the user's: always where the lives are). One view: the game's
 * own print, its text swapped (vp_FontSpritePrintCentered). Split screen: as the co-op tournament's (hud_split.cpp:
 * fontSpritePrintXY with hudSplitVertLivesMode / hudSplitHorzLivesMode), which the game prints only in that mode.
 * The buffers hold 10 characters (a space takes one too): "Level: N" without "of M", "Points:100" without its space. */
static int score_text(int p, char *out, size_t n)
{
    if (getenv("TM2PC_TEST_SCORE") && vp_dm()) { snprintf(out, n < 11 ? n : 11, "%s", getenv("TM2PC_TEST_SCORE")); return 1; } /* tests: this text */
    if (!vp_dm()) {
        /* a co-op tournament (mode 2): the game prints the team's lives on player 1's view only (HUD_LIVES_HERE; with
         * portHumanOptions bit 1 every view prints its own); the others here, the same line (decomp request 75 (HIDDEN_AREAS.md)) */
        static uint32_t opts = 1;
        if (opts == 1) opts = port_sym("portHumanOptions");
        if (p <= 0 || mem_r8(&M.cpu, SETTINGS_BLOCK) != 2 || settings_video.hud_no_lives || (opts && (mem_r32(&M.cpu, opts) & 2u))) return 0;
        int lv = (int)(guest_call(GA(0x80129B20u), 0, 0, 0, 0) & 0xFF) - 1; /* (gameGetLives(0), as the game's: one less) */
        snprintf(out, n, "Lives: %d", lv < 0 || lv > 0x7E ? 0 : lv); /* (the pool used up: 0, not "-1", which the font's low
                                                                     * minus drew as "Lives: _" on Shadow's view) */
        return 1;
    }
    for (int i = 0; i < match_cars(); i++) {
        const MatchCar *m = match_car(i);
        if (m->human != p) continue;
        if (mode_is(MODE_GUN_GAME)) snprintf(out, n, "Level: %d", m->points + 1 > gun_count ? gun_count : m->points + 1);
        else if (mode_is(MODE_DEATHMATCH)) snprintf(out, n, "Kills: %d", m->kills);
        else if (mode_is(MODE_JUGGERNAUT)) snprintf(out, n, "Kills: %d", m->points);
        else if (mode_is(MODE_KING_OF_THE_HILL) || mode_is(MODE_ODDBALL)) snprintf(out, n, "Points: %d", m->points);
        else return 0;
        if (strlen(out) > 10) { char *sp = strchr(out, ' '); if (sp) memmove(sp, sp + 1, strlen(sp)); } /* (10 at most) */
        out[n > 10 ? 10 : n - 1] = 0;
        return 1;
    }
    return 0;
}
static void score_dash_raise(uint32_t sprites, const char *text) /* a minus ("Kills: -1"): the font's sits low, 3 units up */
{
    for (int i = 0; text[i] && i < 10; i++) /* (each character its packet, spaces too: fontSpritePrint) */
        if (text[i] == '-') {
            uint32_t at = sprites + 20u * (uint32_t)i + 10; /* (the sprite's y: tag, color, then x y) */
            mem_w16(&M.cpu, at, (uint16_t)((int16_t)mem_r16(&M.cpu, at) - 3));
        }
}
static void score_line_print(CPU *c, int p, int x, int y, int split) /* split: 1 side by side, 2 one above the other */
{
    char text[16];
    if (!score_text(p, text, sizeof text)) return;
    const uint32_t buf = DM_TEXT + 0x40, ot = DM_TEXT + 0x660, mode = GA(split == 1 ? 0x80182160u : 0x80182234u);
    for (int i = 0; i < 16; i++) { mem_w8(c, buf + (uint32_t)i, (uint8_t)text[i]); if (!text[i]) break; }
    for (uint32_t o = 0; o < 12; o += 4) mem_w32(c, mode + o, mem_r32(c, GA(0x80181E28u) + o)); /* font 2's draw mode (unset outside co-op) */
    uint32_t font_was = vp_font_color_get(2);
    guest_call(GA(0x80114284u), 2, 36, 150, 36); /* fontSetColor: the HUD's green */
    guest_call(GA(0x80141938u), ot, 2, 0, 0);    /* ClearOTagR */
    guest_call7(GA(0x801139D8u), 2, (uint32_t)x, (uint32_t)y, mode, mode + 12, buf, ot); /* fontSpritePrintXY */
    score_dash_raise(mode + 12, text);
    guest_call(GA(0x80141A30u), ot + 4, 0, 0, 0); /* DrawOTag (through the layout) */
    vp_font_color_put(2, font_was);
}

/* ---- the kill feed (a Challenge Match): "X <weapon> Y", the last four, 5 seconds each ----
 * The weapon as its HUD icon where the game has one (the HUD record's big icons, by weapon slot:
 * 0 the special, 1 fire, 2 homing, 3 remote, 4 power, 5 napalm, 6 ricochet, 7 lightning; 100 the mine and 101 the
 * freeze shot, their sprites in the arena's VRAM: decomp request 81), else a word (the machine gun, rams). Small, in
 * the view's top right corner. */
#define FEED_LINES 4
float hudlay_feed_k(void); /* hud_layout.c: the feed's scale, its texels whole pixels */
#define FEED_SCALE hudlay_feed_k()
void hudlay_prescale(int x, int y, float k); /* hud_layout.c */
int hudlay_feed_top(float width);
void hudlay_force(int group);
static char kill_text[CARS_HUMANS_MAX][40];
static uint64_t kill_until[CARS_HUMANS_MAX];
static char net_notice[40];       /* (online: "NEW HOST: X" over every view a while; this machine's only, the screen's) */
static uint64_t net_notice_until;
void vp_net_notice(const char *text)
{
    snprintf(net_notice, sizeof net_notice, "%s", text);
    net_notice_until = vp_gv() + 180;
}
static struct { char text[48], a[24], b[24]; int icon, ta, tb; uint64_t until; } feed[FEED_LINES]; /* icon: a weapon slot, -1 the text; ta, tb: teams (-1 none) */
/* The port's timed messages (the kill feed, the "X LEFT THE MATCH" notice, a flash) end at a time on the step clock
 * (vp_gv). Online a joiner takes the clock from the host's state: a new server's clock lower than the last one's left
 * the last server's feed and "GODSLAYER LEFT THE MATCH" up for good (Enough Boxes, 2026-11-24). When the clock goes
 * back, they are dropped. This machine's screen only (none of it is in the state). */
void vp_messages_clock_check(void)
{
    /* (a rollback puts the clock back a few steps too: only an end further off than any message lasts (5 s at most)
     * is the last timeline's) */
    const uint64_t now = vp_gv(), most = 600;
    for (int i = 0; i < FEED_LINES; i++)
        if (feed[i].until > now + most) feed[i].text[0] = 0, feed[i].until = 0;
    if (net_notice_until > now + most) net_notice[0] = 0, net_notice_until = 0;
    if (flash_until > now + most) flash_text[0] = 0, flash_until = 0;
    for (int p = 0; p < 4; p++) if (split_msg[p].until > now + most) split_msg[p].until = 0;
}
static const uint8_t *team_color(int t); /* (below) */
static int slot_team(int slot);
static const int attack_icon[] = {-1, -1, 1, 2, 4, 5, 101, 3, 100, 6, 7, -1, 0, -1}; /* by attack (A_*): the HUD icon's slot */
static const char *const attack_words[] = {"", "GUNNED", "FIRE", "HOMING", "POWER", "NAPALM", "FROZE", "REMOTE", "MINE",
                                           "RICOCHET", "LIGHTNING", "RAMMED", "SPECIAL", "DESTROYED"};
const char *ui_extra_player_name(int p); /* ui/options.c: players 3-4's profile names ("" a guest) */
static void slot_name(int slot, char *out, size_t n)
{
    int h = slot_human(slot), k = slot_ai(slot);
    const char *ui_match_human_name(int h); /* ui/options.c */
    int netplay_in_match(void);             /* net/game_tm2.c */
    if (h >= 0 && netplay_in_match()) snprintf(out, n, "%s", ui_match_human_name(h)); /* online: each player's own name (this
                                                       * PC's profile was every machine's player 1, the others PLAYER N) */
    else if (h == 0) snprintf(out, n, "%s", profile_signed_in() ? profile_name() : "PLAYER 1");
    else if (h == 1) snprintf(out, n, "%s", profile_p2_name()[0] ? profile_p2_name() : "PLAYER 2");
    else if (h >= 2) snprintf(out, n, "%s", ui_extra_player_name(h)[0] ? ui_extra_player_name(h) : h == 2 ? "PLAYER 3" : "PLAYER 4");
    else if (k >= 0) snprintf(out, n, "%s", type_names[(mem_r32(&M.cpu, AI_REC(k) + 0x30) >> 4) & 15]);
    else snprintf(out, n, "?");
    for (char *p = out; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
}
void vp_human_name_upper(int h, char *out, size_t n) /* player h's name as the feed and the markers have it, in capitals */
{
    const char *ui_match_human_name(int h); /* ui/options.c */
    int netplay_in_match(void);             /* net/game_tm2.c */
    if (netplay_in_match()) snprintf(out, n, "%s", ui_match_human_name(h) ? ui_match_human_name(h) : "PLAYER");
    else if (h == 0) snprintf(out, n, "%s", profile_signed_in() ? profile_name() : "PLAYER 1");
    else if (h == 1) snprintf(out, n, "%s", profile_p2_name()[0] ? profile_p2_name() : "PLAYER 2");
    else snprintf(out, n, "%s", h < 4 && ui_extra_player_name(h)[0] ? ui_extra_player_name(h) : h == 2 ? "PLAYER 3" : h == 3 ? "PLAYER 4" : "PLAYER");
    for (char *p = out; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
}
static void kill_feed_add(int own, int killer, int attack)
{
    char a[24], b[24];
    slot_name(own, b, sizeof b);
    if (killer >= 0 && killer != own) { /* (the log: who killed whom, with what, and their teams) */
        char k[24];
        slot_name(killer, k, sizeof k);
        fprintf(stderr, "kill: %s (team %d) killed %s (team %d), attack %d\n", k, vp_team_of_slot(killer), b, vp_team_of_slot(own), attack);
    }
    if (battle_rules()) { /* the feed (a Challenge Match): the oldest line on top, a new one under the others; a line gone,
                           * the ones under it move up (the draw skips the gone ones) */
        int n = 0;
        for (int i = 0; i < FEED_LINES; i++)
            if (feed[i].text[0] && vp_gv() < feed[i].until) feed[n++] = feed[i];
        if (n == FEED_LINES) { /* full: the oldest out */
            for (int i = 0; i + 1 < FEED_LINES; i++) feed[i] = feed[i + 1];
            n--;
        }
        for (int i = n; i < FEED_LINES; i++) feed[i].text[0] = 0;
        if (killer < 0 || killer == own) {
            snprintf(feed[n].text, sizeof feed[n].text, " %s", killer < 0 ? "WAS WRECKED" : "DESTROYED ITSELF"); /* (after the name) */
            snprintf(feed[n].b, sizeof feed[n].b, "%s", b);
            feed[n].a[0] = 0;
            feed[n].icon = -1;
        } else {
            slot_name(killer, a, sizeof a);
            int w = attack >= 0 && attack < (int)(sizeof attack_words / sizeof attack_words[0]) ? attack : 13;
            snprintf(feed[n].text, sizeof feed[n].text, "  %s  ", attack_words[w][0] ? attack_words[w] : "DESTROYED"); /* (between the names) */
            snprintf(feed[n].a, sizeof feed[n].a, "%s", a);
            snprintf(feed[n].b, sizeof feed[n].b, "%s", b);
            feed[n].icon = attack_icon[w];
            int hudstyle_feed_vram_icon(int which, int x, int cy, int w); /* hud_styles.c */
            if (feed[n].icon >= 100 && !hudstyle_feed_vram_icon(feed[n].icon - 100, 0, 0, 0)) feed[n].icon = -1; /* (not on
                                                                                    * this arena: the word) */
        }
        feed[n].ta = killer >= 0 ? slot_team(killer) : -1; /* with teams: each name in its team's color */
        feed[n].tb = slot_team(own);
        feed[n].until = vp_gv() + 300;
    }
    int h = killer >= 0 && killer != own ? slot_human(killer) : -1; /* a player's kill: "YOU KILLED X" (Vanilla Plus) */
    /* the user's streaks (Unreal Tournament's names): multi-kills, another enemy within 4 seconds of the last; sprees,
     * enemies killed without dying (a player's death, any cause, starts it over). A kill's messages in turn: its
     * streaks first, then YOU KILLED X (port_message shows the queue) */
    int victim = slot_human(own);
    if (victim >= 0 && victim < CARS_HUMANS_MAX) streak_spree[victim] = 0;
    if (h < 0 || h >= 4) return;
    const char *say[3];
    int n_say = 0;
    if (!teams_on() || slot_team(killer) != slot_team(own)) {
        streak_multi[h] = streak_last[h] && vp_gv() - streak_last[h] <= 240 ? streak_multi[h] + 1 : 1;
        streak_last[h] = vp_gv();
        int m = streak_multi[h], s = ++streak_spree[h];
        static const char *const multi[] = {"DOUBLE KILL!", "MULTI KILL!", "ULTRA KILL!", "MONSTER KILL!", "LUDICROUS KILL!",
                                            "HOLY SHIT!"};
        static const char *const spree[] = {"KILLING SPREE!", "RAMPAGE!", "DOMINATING!", "UNSTOPPABLE!", "GODLIKE!", "MASSACRE!"};
        if (m >= 2) fprintf(stderr, "multi kill: player %d, %d in a row\n", h + 1, m);
        if (s % 5 == 0 && s <= 30) fprintf(stderr, "spree: player %d, %d kills\n", h + 1, s);
        if (vp_on(VP_KILL_STREAKS)) {
            if (m >= 2) say[n_say++] = multi[m - 2 > 5 ? 5 : m - 2];
            if (s % 5 == 0 && s <= 30) snprintf(streak_spree_msg[h], sizeof streak_spree_msg[h], "%s", spree[s / 5 - 1]);
        }
    }
    if (streak_spree_msg[h][0]) say[n_say++] = streak_spree_msg[h]; /* (a spree not shown yet: kept through a newer kill) */
    char killed[40];
    snprintf(killed, sizeof killed, "YOU KILLED %s", b);
    if (vp_on(VP_KILL_MESSAGE)) say[n_say++] = killed;
    if (!n_say) return;
    /* the first now, the others after it: 2 seconds each when there are several, 4 alone (a new kill starts over) */
    int each = n_say > 1 ? 120 : 240;
    snprintf(kill_text[h], sizeof kill_text[h], "%s", say[0]);
    if (say[0] == streak_spree_msg[h]) streak_spree_msg[h][0] = 0;
    kill_until[h] = vp_gv() + (uint64_t)each;
    streak_queue_n[h] = 0;
    for (int i = 1; i < n_say; i++) snprintf(streak_queue[h][streak_queue_n[h]++], sizeof streak_queue[0][0], "%s", say[i]);
}
static void guest_text(CPU *c, uint32_t at, const char *text)
{
    for (int i = 0;; i++) { mem_w8(c, at + (uint32_t)i, (uint8_t)text[i]); if (!text[i]) break; }
}
void hudstyle_feed_icon(int player, int slot, int x, int cy, int w); /* hud_styles.c */
int hudstyle_feed_vram_icon(int which, int x, int cy, int w);
/* one piece of a feed line: the HUD's font as sprites into our order table, drawn through the layout */
static void feed_text(CPU *c, int font, int x, int y, uint32_t text, uint32_t ot, uint32_t sprites)
{
    for (uint32_t o = 0; o < 12; o += 4) mem_w32(c, sprites + o, mem_r32(c, GA(0x80181E28u) + o)); /* hudMessageTextMode: font 2's */
    guest_call(GA(0x80141938u), ot, 2, 0, 0); /* ClearOTagR */
    guest_call7(GA(0x801139D8u), (uint32_t)font, (uint32_t)x, (uint32_t)y, sprites, sprites + 12, text, ot); /* fontSpritePrintXY */
    guest_call(GA(0x80141A30u), ot + 4, 0, 0, 0);
}
/* the flash (a new Juggernaut), "YOU KILLED X" (Vanilla Plus) and the kill feed (small, the view's top right) */
static void hud_extra_print(CPU *c, int p, int x, int right, int big_y, int small)
{
    const uint32_t buf = DM_TEXT + 0x80;
    (void)x; (void)big_y; (void)small; /* the flash and YOU KILLED: the message line (port_message) */
    static int no_feed = -1; /* trailer footage: TM2PC_TRAILER_NO_FEED=1 hides the kill feed (the Hong Kong final) */
    if (no_feed < 0) no_feed = getenv("TM2PC_TRAILER_NO_FEED") != NULL;
    if (!battle_rules() || no_feed) return;
    /* small, in the top right corner (under the mirror): the HUD's font as sprites into an order table of
     * our own, drawn through the layout (scaled about the corner); white */
    const int font = 2;
    const uint32_t ot = DM_TEXT + 0x660, sprites = 0x80DF6000u; /* (high RAM: 12 + 20 per letter) */
    /* its widest line (the font's units, before the feed's scale): the feed goes under the message bar only when its
     * lines would reach the bar */
    int widest = 0;
    for (int i = 0; i < FEED_LINES; i++) {
        if (vp_gv() >= feed[i].until || !feed[i].text[0]) continue;
        int w = 0;
        const char *parts[3] = {feed[i].a, feed[i].icon >= 0 ? "" : feed[i].text, feed[i].b};
        for (int k = 0; k < 3; k++) {
            if (!parts[k][0]) continue;
            guest_text(c, buf + 0x40, parts[k]);
            w += (int)guest_call(GA(0x8011355Cu), (uint32_t)font, buf + 0x40, 0, 0); /* fontStringWidth */
        }
        if (feed[i].icon >= 0) w += 14 + 2 * 3; /* the icon and its gaps */
        if (w > widest) widest = w;
    }
    int top = hudlay_feed_top(widest * FEED_SCALE);
    if (getenv("TM2PC_FEED_DEBUG") && widest) fprintf(stderr, "feed: player %d widest %d top %d right %d\n", p, widest, top, right);
    hudlay_force(G_TR);
    hudlay_prescale(right, top, FEED_SCALE);
    void hudlay_feed_measure(int on), hudlay_feed_place(void);
    hudlay_feed_place();
    hudlay_feed_measure(1);
    uint32_t font_was = vp_font_color_get(font);
    guest_call(GA(0x80114284u), (uint32_t)font, 230, 230, 230);
    for (int i = 0, line = 0; i < FEED_LINES; i++) {
        if (vp_gv() >= feed[i].until || !feed[i].text[0]) continue;
        int y = top + 10 + 12 * line++; /* (12: at the feed's half size each line on whole pixels, the letters alike) */
        const uint8_t *ca = team_color(feed[i].ta), *cb = team_color(feed[i].tb);
        if (feed[i].icon >= 0) { /* KILLER <icon> VICTIM, right-aligned */
            const int icon_w = 14, gap = 3;
            if (cb) guest_call(GA(0x80114284u), (uint32_t)font, cb[0], cb[1], cb[2]);
            guest_text(c, buf + 0x40, feed[i].b);
            int wb = (int)guest_call(GA(0x8011355Cu), (uint32_t)font, buf + 0x40, 0, 0);
            feed_text(c, font, right - wb, y, buf + 0x40, ot, sprites);
            int xi = right - wb - gap - icon_w;
            if (feed[i].icon >= 100) hudstyle_feed_vram_icon(feed[i].icon - 100, xi, y - 4, icon_w);
            else hudstyle_feed_icon(p, feed[i].icon, xi, y - 4, icon_w);
            if (ca) guest_call(GA(0x80114284u), (uint32_t)font, ca[0], ca[1], ca[2]);
            else guest_call(GA(0x80114284u), (uint32_t)font, 230, 230, 230); /* (the icon's draw left the font as it was) */
            guest_text(c, buf + 0x40, feed[i].a);
            int wa = (int)guest_call(GA(0x8011355Cu), (uint32_t)font, buf + 0x40, 0, 0);
            feed_text(c, font, xi - gap - wa, y, buf + 0x40, ot, sprites);
            guest_call(GA(0x80114284u), (uint32_t)font, 230, 230, 230);
            continue;
        }
        /* KILLER WORD VICTIM (the machine gun, rams, freeze, mines), or VICTIM WAS WRECKED / DESTROYED ITSELF: right to
         * left, the names in their teams' colors, the words white */
        int xr = right;
        if (feed[i].a[0]) { /* the victim last */
            guest_call(GA(0x80114284u), (uint32_t)font, cb ? cb[0] : 230, cb ? cb[1] : 230, cb ? cb[2] : 230);
            guest_text(c, buf + 0x40, feed[i].b);
            xr -= (int)guest_call(GA(0x8011355Cu), (uint32_t)font, buf + 0x40, 0, 0);
            feed_text(c, font, xr, y, buf + 0x40, ot, sprites);
            guest_call(GA(0x80114284u), (uint32_t)font, 230, 230, 230);
        }
        guest_text(c, buf + 0x40, feed[i].text);
        xr -= (int)guest_call(GA(0x8011355Cu), (uint32_t)font, buf + 0x40, 0, 0); /* fontStringWidth */
        feed_text(c, font, xr, y, buf + 0x40, ot, sprites);
        const uint8_t *cn = feed[i].a[0] ? ca : cb; /* the name before the words: the killer's, or the one wrecked */
        guest_call(GA(0x80114284u), (uint32_t)font, cn ? cn[0] : 230, cn ? cn[1] : 230, cn ? cn[2] : 230);
        guest_text(c, buf + 0x40, feed[i].a[0] ? feed[i].a : feed[i].b);
        xr -= (int)guest_call(GA(0x8011355Cu), (uint32_t)font, buf + 0x40, 0, 0);
        feed_text(c, font, xr, y, buf + 0x40, ot, sprites);
        guest_call(GA(0x80114284u), (uint32_t)font, 230, 230, 230);
    }
    hudlay_prescale(0, 0, 1);
    hudlay_feed_measure(0);
    hudlay_force(-1);
    vp_font_color_put(font, font_was);
}
/* A time limit's countdown (MM:SS) under the message bar, through the layout (with the message line's group),
 * the HUD's green, a little smaller; under the mode's line when it has one */
static void timer_print(CPU *c, int p, int x, int y)
{
    const MatchSetup *m = battle_match();
    int up = ui_survival() && surv_clock_start; /* Survival: the run's time, counting up */
    if (!up && (!vp_dm() || m->time_limit <= 0 || dm_ended)) return;
    if (settings_video.hud_no_messages) return;
    uint64_t total = 3600ull * (uint64_t)m->time_limit, gone = vp_gv() - dm_start;
    int secs = up ? vp_survival_seconds() : gone >= total ? 0 : (int)((total - gone + 59) / 60);
    char text[16], st[48];
    snprintf(text, sizeof text, "%02d:%02d", secs / 60, secs % 60);
    if (mode_status(p, st, sizeof st) && !mode_is(MODE_JUGGERNAUT) && !mode_is(MODE_ODDBALL) && !mode_is(MODE_KING_OF_THE_HILL))
        y += 12; /* (under the mode's line; these three show theirs in the bar: right under it, the user's) */
    const uint32_t buf = DM_TEXT + 0xC0, ot = DM_TEXT + 0x660, sprites = 0x80DF6000u;
    guest_text(c, buf, text);
    const int font = 2;
    hudlay_force(G_TM);
    hudlay_prescale(x, y, 0.8f);
    uint32_t font_was = vp_font_color_get(font);
    if (secs <= 5 && !up) guest_call(GA(0x80114284u), (uint32_t)font, 200, 30, 30); /* the last seconds: red */
    else guest_call(GA(0x80114284u), (uint32_t)font, 36, 150, 36);
    for (uint32_t o = 0; o < 12; o += 4) mem_w32(c, sprites + o, mem_r32(c, GA(0x80181E28u) + o)); /* (font 2's draw mode) */
    guest_call(GA(0x80141938u), ot, 2, 0, 0); /* ClearOTagR */
    guest_call7(GA(0x80113A14u), (uint32_t)font, (uint32_t)x, (uint32_t)y, sprites, sprites + 12, buf, ot); /* centered on x,
                                                                                * as the bar's line (from x - w / 2 it sat right of it) */
    guest_call(GA(0x80141A30u), ot + 4, 0, 0, 0);
    vp_font_color_put(font, font_was);
    hudlay_prescale(0, 0, 1);
    hudlay_force(-1);
}
static int slot_team(int slot) /* a car's team (-1: no teams) */
{
    if (!teams_on() || slot < 0) return -1;
    int h = slot_human(slot), k = slot_ai(slot);
    return h >= 0 ? team_of_human(h) : k >= 0 ? team_of_ai(k) : -1;
}

/* ---- the radar's colors (hudRadarPlotCar 800FC928: a 2x2 blip, two LINE_F2s, by car type) ----
 * Juggernaut and Hunted: the special car red (and a 3x2 blip), the others
 * green; with teams each in its team's color. A car by its type (one of each
 * in a match). hudRadars 801837C8 + view * rc_hud_radar_bytes (0x370 retail): lines at +0x30 (16 bytes,
 * the view's at buf + 2n), count +0x360, buf +0x36C (the record's last fields: 0x10 and 4 from its end). */
#define RADAR_COUNT (rc_hud_radar_bytes - 0x10u)
#define RADAR_BUF (rc_hud_radar_bytes - 4u)
#define HILLS_MAX 3 /* King of the Hill (below) */
static int32_t hill_x[HILLS_MAX], hill_y[HILLS_MAX], hill_z[HILLS_MAX];
static int hill_holder[HILLS_MAX], hill_contested[HILLS_MAX], hill_count;
static const uint8_t team_rgb[BATTLE_TEAM_COLORS][3] = {{228, 24, 36}, {50, 110, 235}, {50, 190, 70}, {245, 220, 40}};
static const uint8_t *team_color(int t) { return t >= 0 && t < BATTLE_TEAM_COLORS ? team_rgb[t] : NULL; }
#define MARK_SLOTS 64
static int slot_of_type(int type)
{
    /* a car key (the 16-car variant: each slot's own, decomp portCarKey; a blip's "type" is one): its slot, as the keys are
     * this frame. Matched against the records' types it found none, so every blip stayed the game's green (the user's
     * Juggernaut never red, 2026-10-29) */
    if (type >= 0x10000) return (type >> 16) - 1 < MARK_SLOTS ? (type >> 16) - 1 : -1; /* the blip's own slot: its key alone
                                                                                     * matched another car's (the user's
                                                                                     * Outlaw 2 blue on the yellow team) */
    if (type >= 0x100) {
        static uint32_t key_fn = 1;
        static int key_of[MARK_SLOTS];
        static uint64_t keys_at = (uint64_t)-1;
        if (key_fn == 1) key_fn = port_sym("portCarKey");
        if (!key_fn) return -1;
        if (keys_at != vp_gv()) {
            keys_at = vp_gv();
            for (int s = 0; s < MARK_SLOTS; s++) key_of[s] = (int)guest_call(key_fn, (uint32_t)s, 0, 0, 0);
        }
        int key = type & 0xFFFF; /* (the blip's: (slot + 1) << 16 | key) */
        for (int s = 0; s < MARK_SLOTS; s++) if (key_of[s] == key) return s;
        return -1;
    }
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++)
        if ((int)mem_r32(&M.cpu, HUMAN_REC(i) + 0x90) == type) return (int16_t)mem_r16(&M.cpu, HUMAN_REC(i) + 4);
    for (int k = 0; k < ais && k < cars_ai_max(); k++)
        if ((int)mem_r32(&M.cpu, AI_REC(k) + 0x30) == type) return (int16_t)mem_r16(&M.cpu, AI_REC(k) + 6);
    return -1;
}
/* the car a blip is: with two of a type (decomp request 32), the one of that type at the radar owner's position plus
 * the blip's (each radar is its player's: from their car), else the type's only one */
static int slot_of_blip(int type, int view, const int32_t rel[3])
{
    if (type >= 0x100) return slot_of_type(type); /* (a car key: its slot exactly) */
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), best = -1;
    if (view < 0 || view >= humans || view >= CARS_HUMANS_MAX) return slot_of_type(type);
    int64_t best_d = INT64_MAX;
    for (int n = 0; n < humans + ais; n++) {
        int h = n < humans;
        if (h ? n >= CARS_HUMANS_MAX : n - humans >= cars_ai_max()) continue;
        uint32_t r = h ? HUMAN_REC(n) : AI_REC(n - humans);
        if ((int)mem_r32(&M.cpu, r + (h ? 0x90u : 0x30u)) != type) continue;
        int64_t d = 0;
        for (uint32_t a = 0; a < 2; a++) {
            int64_t e = (int64_t)(int32_t)mem_r32(&M.cpu, r + (h ? 0x6FCu : 0x73Cu) + 4 * a) -
                        (int32_t)mem_r32(&M.cpu, VIEW_REC(view) + 0x6FC + 4 * a) - rel[a];
            d += e * e;
        }
        if (d < best_d) { best_d = d; best = (int16_t)mem_r16(&M.cpu, r + (h ? 4u : 6u)); }
    }
    return best >= 0 ? best : slot_of_type(type);
}
int vp_hill_mark(int i, double *x, double *y, double *z, double *half, uint8_t rgb[3]); /* (below) */
static uint8_t blip_rgb[MARK_SLOTS][3], blip_seen[MARK_SLOTS]; /* each car's blip color as last drawn (by slot) */
void F_800FC928_orig(CPU *c);
void vp_RadarPlotCar(CPU *c) /* hudRadarPlotCar(type, pos, angle, view) */
{
    int type = (int)c->r[4], view = (int)(c->r[7] & 3);
    uint32_t radar = RB_hudRadars + rc_hud_radar_bytes * (uint32_t)view;
    int before = (int)mem_r32(c, radar + RADAR_COUNT);
    uint32_t angle = c->r[6], pos = c->r[5];
    const int32_t rel[3] = {(int32_t)mem_r32(c, pos), (int32_t)mem_r32(c, pos + 4), (int32_t)mem_r32(c, pos + 8)};
    F_800FC928_orig(c);
    int car_added = (int)mem_r32(c, radar + RADAR_COUNT) == before + 2;
    for (int l = 0; car_added && l < 2; l++) /* its lines LINE_F2 again: the game sets the code once (ai_route_setup.c), and
                                               * a hill's square (TILE, below) left in this place would draw a blip as a
                                               * square as wide as its second point's x */
        mem_w8(c, radar + 0x30 + 16u * (mem_r32(c, radar + RADAR_BUF) + 2u * (uint32_t)before + 2u * (uint32_t)l) + 7, 0x40);
    if (view == 0 && type > 0 && mem_r8(c, GA(0x8016475Cu)) == 3 && mem_r32(c, GA(0x80181014u)) == 2 &&
        type != (int)mem_r32(c, HUMAN_REC(0) + 0x90) && type != (int)mem_r32(c, HUMAN_REC(1) + 0x90)) {
        /* 2 players in the challenge mode (with computer cars, co-op Survival): this computer car on
         * player 2's radar too, from player 2 (pos is from player 1) */
        const uint32_t d2 = DM_TEXT + 0x340;
        for (int a = 0; a < 3; a++)
            mem_w32(c, d2 + 4u * (uint32_t)a, mem_r32(c, pos + 4u * (uint32_t)a) + mem_r32(c, HUMAN_REC(0) + 0x6FC + 4u * (uint32_t)a) -
                                                  mem_r32(c, HUMAN_REC(1) + 0x6FC + 4u * (uint32_t)a));
        guest_call(GA(0x800FC928u), (uint32_t)type, d2, mem_r32(c, HUMAN_REC(1) + 0x710), 1); /* through this wrap: its colors */
    }
    static uint64_t hill_marked[4];
    if (mode_is(MODE_KING_OF_THE_HILL) && hill_marked[view] != vp_gv()) { /* the hills: a mark each */
        hill_marked[view] = vp_gv();
        for (int h = 0; h < hill_count; h++) {
            int at = (int)mem_r32(c, radar + RADAR_COUNT);
            const uint32_t off = DM_TEXT + 0x300; /* the hill from the view's player */
            mem_w32(c, off, (uint32_t)(hill_x[h] - (int32_t)mem_r32(c, VIEW_REC(view) + 0x6FC)));
            mem_w32(c, off + 4, (uint32_t)(hill_y[h] - (int32_t)mem_r32(c, VIEW_REC(view) + 0x700)));
            mem_w32(c, off + 8, 0);
            guest_call(GA(0x800FC928u), 0, off, angle, (uint32_t)view); /* F_800FC928 through this wrap: type 0 is no car */
            if ((int)mem_r32(c, radar + RADAR_COUNT) != at + 2) continue;
            uint8_t rgb[3]; /* white while nobody holds it (contested too), else the holder's color: as the hill's box */
            double hx, hy, hz, half;
            vp_hill_mark(h, &hx, &hy, &hz, &half, rgb);
            uint32_t buf = mem_r32(c, radar + RADAR_BUF);
            uint32_t first = radar + 0x30 + 16u * (buf + 2u * (uint32_t)at);
            int x = (int16_t)mem_r16(c, first + 8), y = (int16_t)mem_r16(c, first + 10);
            for (int l = 0; l < 2; l++) { /* its two lines (LINE_F2, 3 words) made a square (TILE, 3 words), 6 wide */
                uint32_t line = radar + 0x30 + 16u * (buf + 2u * (uint32_t)at + 2u * (uint32_t)l);
                mem_w32(c, line + 4, (0x60u << 24) | ((uint32_t)rgb[2] << 16) | ((uint32_t)rgb[1] << 8) | rgb[0]);
                mem_w16(c, line + 8, (uint16_t)(x - 2));
                mem_w16(c, line + 10, (uint16_t)(y - 2));
                mem_w16(c, line + 12, 6);
                mem_w16(c, line + 14, 6);
            }
        }
    }
    if (type != 0 && car_added) { /* (the cars' markers, world_marks.c: each car in its blip's color) */
        int s = cars_duplicates() ? slot_of_blip(type, view, rel) : slot_of_type(type);
        uint32_t line = radar + 0x30 + 16u * (mem_r32(c, radar + RADAR_BUF) + 2u * (uint32_t)before);
        if (s >= 0 && s < MARK_SLOTS) for (int a = 0; a < 3; a++) blip_rgb[s][a] = mem_r8(c, line + 4 + (uint32_t)a), blip_seen[s] = 1;
    }
    if (!battle_rules() || ui_survival() || type == 0 || !car_added) return;
    int slot = cars_duplicates() ? slot_of_blip(type, view, rel) : slot_of_type(type), special = 0;
    for (int i = 0; i < special_n; i++) /* (any target, 1-4: the first may be out while the others are not) */
        special |= (mode_is(MODE_JUGGERNAUT) || mode_is(MODE_ODDBALL)) && specials[i] >= 0;
    if (mode_is(MODE_JUGGERNAUT) && shim_slot >= 0 && slot == shim_slot && !shim_seen && slot_human(slot) != view) {
        mem_w32(c, radar + RADAR_COUNT, (uint32_t)before); /* a ghost Juggernaut: off the others' radars but for its glimpses */
        return;
    }
    const uint8_t *rgb = NULL;
    static const uint8_t red[3] = {255, 40, 40}, green[3] = {60, 230, 60}, pink[3] = {255, 90, 220};
    if (getenv("TM2PC_RADAR_DEBUG") && vp_gv() % 120 == 0)
        fprintf(stderr, "radar: view %d type %d slot %d special %d (first %d)\n", view, type, slot, is_special(slot), special_n ? specials[0] : -1);
    if (teams_on() && slot >= 0 && mode_is(MODE_ODDBALL) && is_special(slot)) rgb = pink; /* Hunted with teams: the targets pink
                                                                                           * (the user's 2026-10-29) */
    else if (teams_on() && slot >= 0) rgb = team_rgb[vp_team_of_slot(slot) < 0 ? 0 : vp_team_of_slot(slot)]; /* (teams: the team's
                                                                                       * color, a target's blip wider) */
    else if (special) rgb = is_special(slot) ? red : green;
    if (!rgb) return;
    uint32_t buf = mem_r32(c, radar + RADAR_BUF);
    for (int l = 0; l < 2; l++) {
        uint32_t line = radar + 0x30 + 16u * (buf + 2u * (uint32_t)before + 2u * (uint32_t)l);
        mem_w8(c, line + 4, rgb[0]);
        mem_w8(c, line + 5, rgb[1]);
        mem_w8(c, line + 6, rgb[2]);
        if (special && is_special(slot)) mem_w16(c, line + 12, (uint16_t)(mem_r16(c, line + 8) + 2)); /* 3 wide */
    }
    if (slot >= 0 && slot < MARK_SLOTS) for (int a = 0; a < 3; a++) blip_rgb[slot][a] = rgb[a];
}

/* Gun Game: "Level: 3 of 7" where the one-view HUD has its Lives line (font 2, its green, centered at 50, 212):
 * the game's line holds 10 letters, so it is left empty and this is printed instead */
/* ---- King of the Hill ----
 * Hills (the game type's 1-3, koth_hills): each a circle of HILL_RADIUS at one of the arena's chosen spots (koth_spots
 * 0: vplus/hill_spots.c, 5-8 an arena: its landmarks and best battlegrounds) or at a route point (aiRouteNodes 801B1DE8,
 * 24 bytes: x, y s32, z s16) at random; never two on one spot or near each other. They move after the game type's
 * time (1-5 minutes, or never), each to a spot of its own. Each second the cars on a hill (alive, within the radius)
 * score a point if they are all one side (with teams one team, else one car); two sides: contested. The marks: a box
 * over each hill and a square on the radar (hud_styles.c, the radar's plot below): white free or contested, the
 * holder's color. The computer cars go for the nearest hill free or their own side's (on the route, mood 0, the next
 * route node picked nearest it: aiFollowRouteTarget's aim point) and hunt the holder of the nearest other one,
 * firing as ever at their targets. */
#define HILL_RADIUS 700
int hill_spots(int arena, const int32_t (**xyz)[3]); /* vplus/hill_spots.c: the arena's chosen spots (their count) */
static int hill_spot_of[HILLS_MAX]; /* the chosen spot each hill is on (-1: a route point) */
static uint64_t hill_moved, hill_tick;
static int hill_near(int h, int32_t x, int32_t y) /* another hill (or this one's last spot) too near for a new one */
{
    for (int o = 0; o < hill_count; o++) {
        double dx = (double)x - hill_x[o], dy = (double)y - hill_y[o];
        if (dx * dx + dy * dy < (4.0 * HILL_RADIUS) * (4.0 * HILL_RADIUS) && (o != h || hill_moved)) return 1;
    }
    return 0;
}
static void hill_place(int h)
{
    const GameType *t = battle_rules();
    int arena = mem_r8(&M.cpu, GA(0x8016475Cu) + 24);
    const int32_t (*spot)[3] = NULL;
    int spots = t ? hill_spots(arena, &spot) : 0, at = -1; /* (always the chosen spots: the user's 2026-10-30, the setting gone) */
    if (spots > 0) { /* a chosen spot no other hill has (nor this one had), at random */
        int free[16], m = 0;
        for (int i = 0; i < spots && i < 16; i++) {
            int used = 0;
            for (int o = 0; o < hill_count; o++) if (hill_spot_of[o] == i) used = 1;
            if (!used) free[m++] = i;
        }
        if (m) {
            at = free[(int)vp_rand() % m];
            hill_x[h] = spot[at][0], hill_y[h] = spot[at][1], hill_z[h] = spot[at][2];
        }
    }
    if (at < 0) { /* a route point, away from the other hills (the first of 40 tries that is) */
        int n = (int)mem_r32(&M.cpu, GA(0x80180AD8u));
        if (n <= 0) return;
        for (int tries = 0; tries < 40; tries++) {
            int i = (int)vp_rand() % (n > 1000 ? 1000 : n);
            int32_t x = (int32_t)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i);
            int32_t y = (int32_t)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i + 4);
            hill_x[h] = x, hill_y[h] = y, hill_z[h] = (int16_t)mem_r16(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i + 8);
            if (!hill_near(h, x, y)) break;
        }
    }
    hill_spot_of[h] = at;
    if (getenv("TM2PC_HILL_HERE") && h == 0) { /* tests: around player 1 */
        hill_x[0] = (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x6FC);
        hill_y[0] = (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x700);
        hill_z[0] = (int32_t)mem_r32(&M.cpu, HUMAN_REC(0) + 0x704);
    }
    if (getenv("TM2PC_HILL_SPOT") && h == 0 && spots > 0) { /* tests: hill 1 on this chosen spot (its index) */
        int i = atoi(getenv("TM2PC_HILL_SPOT")) % spots;
        hill_x[0] = spot[i][0], hill_y[0] = spot[i][1], hill_z[0] = spot[i][2], hill_spot_of[0] = i;
    }
    hill_holder[h] = -1;
    hill_contested[h] = 0;
    fprintf(stderr, "king of the hill: hill %d at %d, %d, %d (%s %d, arena %d)\n", h + 1, hill_x[h], hill_y[h], hill_z[h],
            at >= 0 ? "spot" : "route", at, arena);
}
static void hill_round_start(void)
{
    const char *dump = getenv("TM2PC_DUMP_ROUTE"); /* tests: the arena's route nodes (x y z), its hill spots chosen from them */
    if (dump) {
        FILE *f = fopen(dump, "w");
        int n = (int)mem_r32(&M.cpu, GA(0x80180AD8u));
        for (int i = 0; f && i < n && i < 2000; i++)
            fprintf(f, "%d %d %d\n", (int32_t)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i),
                    (int32_t)mem_r32(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i + 4),
                    (int16_t)mem_r16(&M.cpu, GA(0x801B1DE8u) + 24u * (uint32_t)i + 8));
        if (f) fclose(f);
    }
    const GameType *t = battle_rules();
    hill_count = 0;
    hill_moved = 0;
    hill_tick = vp_gv();
    if (!mode_is(MODE_KING_OF_THE_HILL)) return;
    int want = t && t->koth_hills >= 1 ? (t->koth_hills > HILLS_MAX ? HILLS_MAX : t->koth_hills) : 1;
    for (int h = 0; h < want; h++) {
        hill_spot_of[h] = -1;
        hill_x[h] = hill_y[h] = 0x40000000; /* (far: not near the ones placed before it) */
        hill_count = h + 1;
        hill_place(h);
    }
    hill_moved = vp_gv();
}
static int slot_side(int slot) /* a car's side: its team, or (no teams) itself */
{
    int t = vp_team_of_slot(slot);
    return t >= 0 ? 100 + t : slot;
}
static int on_hill(int h, int slot)
{
    double x, y;
    slot_pos(slot, &x, &y);
    if ((x - hill_x[h]) * (x - hill_x[h]) + (y - hill_y[h]) * (y - hill_y[h]) > (double)HILL_RADIUS * HILL_RADIUS) return 0;
    /* and inside its box's height (600 up, drawn by hud_styles.c; a little below for a slope): a car under it (New York's
     * street below a roof's hill) held it (the user's 2026-11-17) */
    int hu = slot_human(slot), k = slot_ai(slot);
    double z = (int32_t)mem_r32(&M.cpu, (hu >= 0 ? HUMAN_REC(hu) + 0x6FC : AI_REC(k < 0 ? 0 : k) + 0x73C) + 8);
    return z >= hill_z[h] - 200 && z <= hill_z[h] + 600;
}
static void hill_tour(void) /* tests: TM2PC_TOUR="x,y,z;x,y,z;...", player 1 moved to each in turn (150 VBlanks apart) */
{
    static int parsed, n, at = -1;
    static int32_t p[32][3];
    static uint64_t next;
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("TM2PC_TOUR");
        while (e && *e && n < 32) {
            if (sscanf(e, "%d,%d,%d", &p[n][0], &p[n][1], &p[n][2]) == 3) n++;
            e = strchr(e, ';');
            if (e) e++;
        }
        next = vp_gv() + 300;
    }
    if (!n || vp_gv() < next || at + 1 >= n) return;
    next = vp_gv() + 150;
    at++;
    uint32_t r = HUMAN_REC(0);
    double x, y, fx = 0, fy = 0; /* facing the arena's middle (0, 0) */
    int32_t px = p[at][0], py = p[at][1], pz = p[at][2];
    if (getenv("TM2PC_TOUR_LOOK")) { /* the hill on the spot; player 1 at a route point 1500-3500 away (as high), facing it */
        hill_x[0] = px, hill_y[0] = py, hill_z[0] = pz, hill_holder[0] = -1;
        int count = (int)mem_r32(&M.cpu, GA(0x80180AD8u)), best = -1;
        double best_s = 1e30;
        for (int i = 0; i < count && i < 140; i++) {
            uint32_t nd = GA(0x801B1DE8u) + 24u * (uint32_t)i;
            double dx = (int32_t)mem_r32(&M.cpu, nd) - px, dy = (int32_t)mem_r32(&M.cpu, nd + 4) - py;
            double dz = (int16_t)mem_r16(&M.cpu, nd + 8) - pz, d = sqrt(dx * dx + dy * dy);
            double s = fabs(d - 2400) + 4 * fabs(dz); /* (2400 away, as high as it) */
            if (d >= 1500 && d <= 3500 && s < best_s) { best_s = s; best = i; }
        }
        if (best >= 0) {
            uint32_t nd = GA(0x801B1DE8u) + 24u * (uint32_t)best;
            fx = px, fy = py;
            px = (int32_t)mem_r32(&M.cpu, nd), py = (int32_t)mem_r32(&M.cpu, nd + 4), pz = (int16_t)mem_r16(&M.cpu, nd + 8);
        }
    }
    x = px - fx, y = py - fy;
    mem_w32(&M.cpu, r + 0x6FC, (uint32_t)px);
    mem_w32(&M.cpu, r + 0x700, (uint32_t)py);
    mem_w32(&M.cpu, r + 0x704, (uint32_t)(pz + 60));
    for (uint32_t o = 0x6E4; o < 0x6FC; o += 2) mem_w16(&M.cpu, r + o, 0); /* (its speeds) */
    int yaw = (int)lround(atan2(-x, -y) * 2048 / 3.14159265) & 4095;
    mem_w32(&M.cpu, r + 0x710, (uint32_t)yaw);
    fprintf(stderr, "tour: spot %d at %d, %d, %d (VBlank %llu)\n", at, p[at][0], p[at][1], p[at][2], (unsigned long long)M.vblanks);
}
static void hill_frame(void)
{
    hill_tour();
    int move = battle_rules()->koth_move; /* minutes, 0: they stay */
    if (move > 0 && vp_gv() >= hill_moved + 3600ull * (uint64_t)move) { /* the hills move, after the game type's time */
        for (int h = 0; h < hill_count; h++) hill_place(h);
        hill_moved = vp_gv();
        snprintf(flash_text, sizeof flash_text, hill_count > 1 ? "THE HILLS MOVED" : "THE HILL MOVED");
        flash_until = vp_gv() + 120;
        guest_call(GA(0x800D3334u), 0x1D, 0, 0, 99); /* the select explosion */
    }
    int slots[CARS_HUMANS_MAX + CARS_MAX], n = slot_count(slots);
    for (int h = 0; h < hill_count; h++) {
        int side = -1, holder = -1, contested = 0;
        for (int i = 0; i < n; i++) {
            if (!slot_alive(slots[i]) || !on_hill(h, slots[i])) continue;
            if (side < 0) { side = slot_side(slots[i]); holder = slots[i]; }
            else if (slot_side(slots[i]) != side) contested = 1;
        }
        hill_holder[h] = contested ? -1 : holder;
        hill_contested[h] = contested;
    }
    if (vp_gv() >= hill_tick + 60) { /* a point a second for each hill held (two held: two), to its holder: with teams the
                                       * team's point however many of it are on the hill (its score the sum) */
        hill_tick += 60;
        for (int h = 0; h < hill_count; h++)
            if (hill_holder[h] >= 0) match_add_points(hill_holder[h], 1);
    }
    if (vp_gv() < hill_retarget) return;
    hill_retarget = vp_gv() + 30;
    int ais = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = 0; k < ais && k < cars_ai_max(); k++) { /* the nearest hill: free or its side's, its goal; else hunt its holder */
        hill_goal[k] = 0;
        if (!ai_alive(k)) continue;
        int own = (int16_t)mem_r16(&M.cpu, AI_REC(k) + 6), best = -1;
        double x, y, best_d = 0;
        slot_pos(own, &x, &y);
        for (int h = 0; h < hill_count; h++) {
            double d = (x - hill_x[h]) * (x - hill_x[h]) + (y - hill_y[h]) * (y - hill_y[h]);
            if (best < 0 || d < best_d) { best = h; best_d = d; }
        }
        if (best < 0) continue;
        if (hill_holder[best] >= 0 && slot_side(hill_holder[best]) != slot_side(own)) {
            mem_w8(&M.cpu, AI_REC(k) + 0x034, 1);
            mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)k, (uint32_t)slot_ai(hill_holder[best]));
        } else hill_goal[k] = (uint8_t)(best + 1);
    }
    static uint64_t dbg;
    if (getenv("TM2PC_HILL_DEBUG") && vp_gv() >= dbg + 600) { /* tests: how far the computer cars are from the hills */
        dbg = vp_gv();
        double sum = 0;
        int on = 0, alive = 0;
        for (int k = 0; k < ais && k < cars_ai_max(); k++) {
            if (!ai_alive(k)) continue;
            double best = -1;
            for (int h = 0; h < hill_count; h++) {
                double dx = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x73C) - hill_x[h], dy = (int32_t)mem_r32(&M.cpu, AI_REC(k) + 0x740) - hill_y[h];
                double d = sqrt(dx * dx + dy * dy);
                if (best < 0 || d < best) best = d;
            }
            sum += best; alive++; on += best <= HILL_RADIUS;
        }
        fprintf(stderr, "king of the hill: computer cars %.0f from a hill on average, %d of %d on one\n", alive ? sum / alive : 0, on, alive);
    }
}
/* the aim point of a computer car going for a hill: that hill */
void F_800F7D54_orig(CPU *c);
void vp_AiFollowRouteTarget(CPU *c)
{
    uint32_t ai = c->r[4], out = c->r[5];
    F_800F7D54_orig(c);
    if (!mode_is(MODE_KING_OF_THE_HILL) || getenv("TM2PC_HILL_OLD")) return; /* (tests: as before) */
    for (int k = 0; k < cars_ai_max(); k++)
        if (ai == AI_REC(k) && hill_goal[k] && hill_goal[k] <= hill_count) {
            mem_w32(c, out, (uint32_t)hill_x[hill_goal[k] - 1]);
            mem_w32(c, out + 4, (uint32_t)hill_y[hill_goal[k] - 1]);
            c->r[2] = (uint32_t)-1; /* (any of the next nodes) */
            return;
        }
}
static int hill_status(int p, char *out, size_t n)
{
    int me = (int16_t)mem_r16(&M.cpu, HUMAN_REC(p) + 4), mine = 0, contested = 0, free = 0;
    for (int h = 0; h < hill_count; h++) {
        if (hill_contested[h]) contested++;
        else if (hill_holder[h] < 0) free++;
        else if (slot_side(hill_holder[h]) == slot_side(me)) mine++;
    }
    if (hill_count <= 1) {
        if (hill_contested[0]) snprintf(out, n, "HILL CONTESTED");
        else if (hill_holder[0] < 0) snprintf(out, n, "HILL: FREE");
        else if (mine) snprintf(out, n, "YOU HOLD THE HILL");
        else {
            char who[24];
            slot_name(hill_holder[0], who, sizeof who);
            snprintf(out, n, "HILL: %s", who);
        }
    } else if (mine) snprintf(out, n, "YOU HOLD %d OF %d HILLS", mine, hill_count);
    else if (contested) snprintf(out, n, "%d HILL%s CONTESTED", contested, contested > 1 ? "S" : "");
    else if (free) snprintf(out, n, "HILLS: %d FREE", free);
    else snprintf(out, n, "HILLS: ALL HELD");
    return 1;
}

/* ---- for the marks over the views (world_marks.c) ---- */
#include "world_marks.h"
int vp_slot_human(int slot) { return slot_human(slot); }
int vp_mark_cars(WmCar *out, int max) /* the cars alive: place, health, color (team, else blip), name */
{
    int slots[CARS_HUMANS_MAX + CARS_MAX], n = slot_count(slots), got = 0;
    for (int i = 0; i < n && got < max; i++) {
        int s = slots[i], h = slot_human(s), k = slot_ai(s);
        if (!slot_alive(s) || (h < 0 && k < 0)) continue;
        WmCar *c = &out[got++];
        uint32_t rec = h >= 0 ? HUMAN_REC(h) : AI_REC(k), pos = rec + (h >= 0 ? 0x6FCu : 0x73Cu);
        c->slot = s;
        c->human = h;
        /* where it is drawn: its object's place (carSlotObjects 80187AD4, +0xA8; between steps the redraws' blend,
         * vplus/interp.c), else its record's */
        uint32_t obj = car_slot_object(s);
        if (obj >= 0x80000000u && obj < 0x80000000u + GUEST_RAM_BYTES) pos = obj + 0xA8;
        c->x = (int32_t)mem_r32(&M.cpu, pos);
        c->y = (int32_t)mem_r32(&M.cpu, pos + 4);
        c->z = (int32_t)mem_r32(&M.cpu, pos + 8);
        c->hp = (int32_t)mem_r32(&M.cpu, rec + (h >= 0 ? 0x1F0u : 0x5A0u)); /* (the stats block's hp, max: +0xA4, +0x3A) */
        c->max_hp = (int16_t)mem_r16(&M.cpu, rec + (h >= 0 ? 0x186u : 0x536u));
        const uint8_t *rgb = teams_on() && vp_team_of_slot(s) >= 0 ? team_rgb[vp_team_of_slot(s)]
                             : s >= 0 && s < MARK_SLOTS && blip_seen[s] ? blip_rgb[s] : NULL;
        static const uint8_t white[3] = {220, 220, 220};
        memcpy(c->rgb, rgb ? rgb : white, 3);
        slot_name(s, c->name, sizeof c->name);
        c->ghost = mode_is(MODE_JUGGERNAUT) && s == shim_slot && !shim_seen;
        /* (the Juggernaut too, as Hunted's target: the user's 2026-11-02; not the ghost one while hidden) */
        c->target = (mode_is(MODE_ODDBALL) || (mode_is(MODE_JUGGERNAUT) && !c->ghost)) && is_special(s);
        static const uint8_t platinum[3] = {196, 204, 216}; /* (was gold, 255 200 40: "a bit yellow", the user's 2026-11-03) */
        memcpy(c->target_rgb, teams_on() && vp_team_of_slot(s) >= 0 ? team_rgb[vp_team_of_slot(s)] : platinum, 3);
    }
    return got;
}
int vp_hill_mark(int i, double *x, double *y, double *z, double *half, uint8_t rgb[3]) /* King of the Hill: hill i and its color */
{
    if (!mode_is(MODE_KING_OF_THE_HILL) || !battle_rules() || i < 0 || i >= hill_count) return 0;
    *x = hill_x[i], *y = hill_y[i], *z = hill_z[i], *half = HILL_RADIUS;
    rgb[0] = rgb[1] = rgb[2] = 235; /* white while nobody holds it (contested too), else the holder's color */
    int holder = hill_holder[i];
    if (!hill_contested[i] && holder >= 0) {
        int t = vp_team_of_slot(holder);
        if (t >= 0) memcpy(rgb, team_rgb[t], 3);
        else if (holder < MARK_SLOTS && blip_seen[holder]) memcpy(rgb, blip_rgb[holder], 3);
        else { rgb[0] = 255; rgb[1] = 140; rgb[2] = 0; }
    }
    return 1;
}

/* ---- Controls: analog steering (decomp ANALOG_STEERING.md) ----
 * carThrottleSteer (800DD304) moves motion.steer toward +/- a limit made from
 * stats.steer_limit (+0x1B0: the air and handbrake cases from it), and the
 * yaw, the camera and the body roll follow steer. For a human with the stick
 * tilted part way, the limit is scaled by the tilt for the call (and put
 * back): a partial lock at the game's own ramp. At full tilt, and on the
 * D-pad or keyboard, the game's. */
void F_800DD304_orig(CPU *c);
void vp_ThrottleSteer(CPU *c) /* carThrottleSteer(car, human) */
{
    uint32_t rec = c->r[4];
    int human = (c->r[5] & 0xFF) != 0, p = human ? (int16_t)mem_r16(c, rec) : -1;
    int humans_on(void), humans_steer(int h); /* vplus/humans.c */
    int amount = p >= 0 && humans_on() ? humans_steer(p) : p >= 0 && p <= 3 ? (int)killcam_input(1, p, (uint32_t)input_steer_amount(p)) : 4096;
    if (amount >= 4096) { F_800DD304_orig(c); return; }
    uint16_t saved = mem_r16(c, rec + 0x1B0);
    int lim = (int16_t)saved * amount / 4096;
    mem_w16(c, rec + 0x1B0, (uint16_t)(lim < 1 ? 1 : lim));
    F_800DD304_orig(c);
    mem_w16(c, rec + 0x1B0, saved);
}

/* ---- Teams: team-colored cars (decomp TEAM_COLORS.md, way A) ----
 * sessionLoadArenaAndCars parses each car's TMS at 0x80019C40 with
 * bitmapParseContainer(buf, n), n = 1.. (the humans, then the opponents).
 * For a car on a team, its body CLUT (image 0) is copied, its paint entries
 * (team_paint.h) recolored to the team's hue, and uploaded with
 * vramUploadClut(256, 1, pal, 0, n), which moves slot n*20+0's CLUT to the
 * copy; assetInstallImage then points that car's primitives at it. */
#include "team_paint.h"
static const int team_hue[BATTLE_TEAM_COLORS] = {0, 225, 120, 55}; /* red, blue, green, yellow */
static uint16_t team_recolor(uint16_t e, int hue, int car) /* car: its strength (team_smin, team_vgain) */
{
    if (!(e & 0x7FFF)) return e; /* transparent stays */
    double r = (e & 31) / 31.0, g = ((e >> 5) & 31) / 31.0, b = ((e >> 10) & 31) / 31.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double s = mx > 0 ? (mx - mn) / mx : 0, v = mx;
    double smin = team_smin[car < 0 || car > 14 ? 14 : car] / 100.0;
    double floor_team = hue == 0 ? 0.68 : hue == 55 ? 0.72 : 0; /* red not pink, yellow not pale */
    if (smin < floor_team) smin = floor_team;
    s = smin + (1 - smin) * s;
    v *= team_vgain[car < 0 || car > 14 ? 14 : car] / 100.0;
    if (hue == 55) v *= 1.3; /* yellow: bright, not olive */
    if (v > 1) v = 1;
    double h = hue / 60.0, f = h - floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double rr, gg, bb;
    switch ((int)floor(h) % 6) {
    case 0: rr = v; gg = t; bb = p; break;
    case 1: rr = q; gg = v; bb = p; break;
    case 2: rr = p; gg = v; bb = t; break;
    case 3: rr = p; gg = q; bb = v; break;
    case 4: rr = t; gg = p; bb = v; break;
    default: rr = v; gg = p; bb = q; break;
    }
    uint16_t o = (uint16_t)((int)(rr * 31 + 0.5) | (int)(gg * 31 + 0.5) << 5 | (int)(bb * 31 + 0.5) << 10);
    if (!o) o = 0x0421;
    return (uint16_t)(o | (e & 0x8000));
}
static uint16_t team_accent_color(uint16_t e, int i, int car, int team) /* an accent entry for this team (Twister) */
{
    if (car < 0 || car > 14 || team < 0 || team > 3) return e;
    for (int a = 0; a < 2; a++)
        if ((team_accent[car][a][i >> 3] & (1 << (i & 7))) && team_accent_hue[car][a][team] >= 0)
            return team_recolor(e, team_accent_hue[car][a][team], car);
    return e;
}
/* A car's team at load by its group n (1..: humans, then opponents), -1 none */
/* a load group's team and car (human i = group i + 1; players 3-4 are groups 3-4, put first in the settings' opponent
 * list while the cars load: vp_SessionLoad; computer car j = group players + j + 1) */
static int team_of_group(int n)
{
    const GameType *t = battle_rules();
    int humans = mem_r8(&M.cpu, SETTINGS_BLOCK + 4), players = four_humans() > humans ? four_humans() : humans;
    if (!t || ui_survival() || battle_match_teams() < 2 || humans < 1 || humans > 16 || n < 1) return -1;
    if (n <= players) return battle_match()->player_team[(n - 1) & 15] % BATTLE_TEAM_COLORS;
    return team_of_ai(n - 1 - players);
}
static int car_of_group(int n) /* (during the load: the opponent list as vp_SessionLoad set it, through its accessor) */
{
    int humans = mem_r8(&M.cpu, SETTINGS_BLOCK + 4);
    int four_car_of(int player); /* vplus/four.c: humans 3..'s (the settings hold 2) */
    if (n <= humans) return n <= 2 ? (int8_t)mem_r8(&M.cpu, SETTINGS_BLOCK + 8 + (uint32_t)(n - 1)) : four_car_of(n - 1);
    return (int8_t)guest_call(GA(0x8013783Cu), SETTINGS_BLOCK, (uint32_t)(n - 1 - humans), 0, 0);
}
void F_80121A2C_orig(CPU *c);
/* A team car's palette: the car's own body palette (image 0's, slot n*20+0 of vramTexSlots 80181034: 20 bytes each,
 * x +12, y +16), written over where the game put it. (A copy elsewhere needed room the arenas' VRAM does not have:
 * the game's vramUploadClut takes a spot whose first pixel is free and writes the whole 256-wide row there, over
 * the start of another car's texture: Thumper's, Shadow's, scrambled in team games.) */
/* The group's body CLUT (image 0), repainted: with the decomp's texture banks live (TEXTURE_BANKS.md 6.5 item 6) its
 * own CLUT in its bank (portTexBanks.body_clut / body_bank, uploaded by portTexLoadImage); else where its first texture
 * slot put it (vramTexSlots, 20 bytes a slot: x +12, y +16; a group's slots group * stride: 20 retail, 32 with the
 * decomp's TM2_PORT_CARS) */
static int team_clut_upload(uint32_t pal, int group)
{
    static uint32_t tb = 1, up, stride;
    if (tb == 1) {
        tb = port_sym("portTexBanks");
        up = port_sym("portTexLoadImage");
        stride = port_sym("portCarDmdArea") ? 32 : 20;
    }
    int x, y, bank = 0;
    if (tb && up && mem_r32(&M.cpu, tb)) { /* banks live */
        uint16_t id = (uint16_t)mem_r16(&M.cpu, tb + 0x310 + 2u * (uint32_t)group);
        bank = mem_r8(&M.cpu, tb + 0x390 + (uint32_t)group);
        x = (id & 0x3F) * 16;
        y = id >> 6;
    } else {
        uint32_t slots = mem_r32(&M.cpu, GA(0x80181034u));
        if (!slots) return 0;
        x = (int)mem_r32(&M.cpu, slots + 20u * stride * (uint32_t)group + 12);
        y = (int)mem_r32(&M.cpu, slots + 20u * stride * (uint32_t)group + 16);
    }
    if (x < 0 || x > 1024 - 256 || y < 0 || y >= 512) return 0;
    const uint32_t rect = DM_TEXT + 0x3F0; /* a RECT: x, y, w, h */
    mem_w16(&M.cpu, rect, (uint16_t)x);
    mem_w16(&M.cpu, rect + 2, (uint16_t)y);
    mem_w16(&M.cpu, rect + 4, 256);
    mem_w16(&M.cpu, rect + 6, 1);
    if (bank) {
        guest_call(up, (uint32_t)bank, rect, pal, 0); /* portTexLoadImage: LoadImage into the bank */
    } else {
        guest_call(GA(0x801416F4u), rect, pal, 0, 0); /* LoadImage */
        guest_call(GA(0x801414CCu), 0, 0, 0, 0);      /* DrawSync */
    }
    if (getenv("TM2PC_TEAM_DEBUG")) fprintf(stderr, "team colors: palette over the car's own at %d,%d (bank %d)\n", x, y, bank);
    return 1;
}
/* Before the texture goes up: the parts that keep their own colors (team_split, team_masks.py) get copies
 * of the paint colors they share with the rest, in entries freed by merging near-identical colors; so the
 * paint can be recolored whole (Sweet Tooth's dots, not his head; Outlaw's panels, not its lights). The
 * body texture (image 0): its CLUT at body, then its pixel block (bnum, x, y, w, h; 8-bit pixels). */
static void team_split_pixels(int car, uint32_t body, uint32_t pix, int w, int h);
static void team_split_apply(uint32_t buf, int n)
{
    int team = team_of_group(n), car = car_of_group(n);
    if (team < 0 || car < 0 || car > 13) return;
    uint32_t body = buf + 4u * (4u + mem_r32(&M.cpu, buf + 12) + 1u + 5u);
    uint32_t pblk = body - 12 + mem_r32(&M.cpu, body - 12), pix = pblk + 12;
    int w = (int)mem_r16(&M.cpu, pblk + 8) * 2, h = (int)mem_r16(&M.cpu, pblk + 10);
    team_split_pixels(car, body, pix, w, h);
}
/* a body texture's pixels (128 x 128, 8-bit) and its palette (body): the painted pixels moved to their own entries */
static void team_split_pixels(int car, uint32_t body, uint32_t pix, int w, int h)
{
    const TeamSplit *sp = &team_split[car];
    if (!sp->n_merge && !sp->n_copy) return;
    if (w != 128 || h != 128) { fprintf(stderr, "team colors: car %d's texture is %dx%d, not split\n", car, w, h); return; }
    uint8_t map[256], cp[256];
    for (int i = 0; i < 256; i++) map[i] = cp[i] = (uint8_t)i;
    for (int i = 0; i < sp->n_merge; i++) map[sp->merge[i][0]] = sp->merge[i][1];
    for (int i = 0; i < sp->n_copy; i++) {
        cp[sp->copy[i][0]] = sp->copy[i][1];
        mem_w16(&M.cpu, body + 2u * sp->copy[i][1], mem_r16(&M.cpu, body + 2u * sp->copy[i][0]));
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t a = pix + (uint32_t)(y * w + x);
            uint8_t p = map[mem_r8(&M.cpu, a)];
            int in = 0, ex = 0, k = y * w + x;
            if (sp->keep >= 0) in = (team_keep[sp->keep][k >> 3] >> (k & 7)) & 1; /* a painted mask: pixel by pixel */
            else
                for (int r = 0; r < sp->n_rect; r++)
                    if (x >= sp->rect[r][0] && x < sp->rect[r][2] && y >= sp->rect[r][1] && y < sp->rect[r][3]) {
                        if (sp->rect[r][4]) ex = 1; else in = 1;
                    }
            if (in && !ex) p = cp[p];
            mem_w8(&M.cpu, a, p);
        }
    if (getenv("TM2PC_TEAM_DEBUG")) fprintf(stderr, "team colors: car %d split (%d merged, %d copies)\n", car, sp->n_merge, sp->n_copy);
}
/* tests: TM2PC_TEST_CS_TEAM=T (0 red .. 3 yellow), the car select's spinning cars in team T's colors (CARSEL.TMS: each
 * car's body palette at (768, 23 + car) and its 128 x 128 pixels the same as the battle's, decomp TEAM_COLORS.md 5.1):
 * the battle's split and recolor applied to the file before it is uploaded. A preview for the team colors' art only. */
static void team_carsel_test(uint32_t buf)
{
    static int team = -2;
    if (team == -2) team = getenv("TM2PC_TEST_CS_TEAM") ? atoi(getenv("TM2PC_TEST_CS_TEAM")) % BATTLE_TEAM_COLORS : -1;
    if (team < 0) return;
    for (uint32_t a = buf; a < buf + 0x100000u; a += 4) { /* (each image's palette block: 0x20C bytes, at 768, 23..35, 256 x 1) */
        if (mem_r32(&M.cpu, a) != 0x20Cu || mem_r32(&M.cpu, a + 8) != 0x10100u) continue;
        uint32_t xy = mem_r32(&M.cpu, a + 4);
        int car = (int)(xy >> 16) - 23;
        if ((xy & 0xFFFF) != 768 || car < 0 || car > 12) continue;
        uint32_t body = a + 12, pblk = a + 0x20C, pix = pblk + 12;
        int w = (int)mem_r16(&M.cpu, pblk + 8) * 2, h = (int)mem_r16(&M.cpu, pblk + 10);
        team_split_pixels(car, body, pix, w, h);
        for (int i = 0; i < 256; i++) {
            uint16_t e = mem_r16(&M.cpu, body + 2u * (uint32_t)i);
            if (axel_eye_entry(car, i)) continue;
            if (team_paint[car][i >> 3] & (1 << (i & 7))) e = team_recolor(e, team_hue[team], car);
            else e = team_accent_color(e, i, car, team);
            mem_w16(&M.cpu, body + 2u * (uint32_t)i, e);
        }
        fprintf(stderr, "team colors: car select car %d in team %d's colors (test)\n", car, team);
    }
}
void vp_BitmapParseContainer(CPU *c) /* bitmapParseContainer(buf, group) */
{
    uint32_t buf = c->r[4];
    int n = (int)c->r[5];
    if (buf == 0x80019C40u) last_group = n; /* (the Santa's group: the next) */
    int mod_cars_group_number(int g); /* vplus/mod_cars.c: the group's mod car number this load, -1 the disc's */
    const unsigned char *mod_car_team_paint(int number);
    int mod_number = buf == 0x80019C40u && n >= 1 ? mod_cars_group_number(n) : -1;
    if (buf == 0x80019C40u && n >= 1 && mod_number < 0) team_split_apply(buf, n); /* (a mod's texture: not the base's layout) */
    if (buf == 0x80019C40u && n == 0) team_carsel_test(buf);
    F_80121A2C_orig(c);
    void hdtex_arena_parsed(uint32_t buf);
    if (buf == 0x80019C40u && n == 0) hdtex_arena_parsed(buf); /* (the arena's textures: the PC's sharper ones) */
    if (buf != 0x80019C40u || n < 1) return;
    int team = team_of_group(n), car = car_of_group(n);
    /* a mod car (a skin or a character): the entries its mod.ini names (team_paint), else its own colors */
    const uint8_t *paint = mod_number >= 0 ? mod_car_team_paint(mod_number) : car >= 0 && car <= 13 ? team_paint[car] : NULL;
    if (team < 0 || !paint) return;
    if (mod_number >= 0) car = 14; /* (the recolor's strength: the general one; no accents, no Axel eyes) */
    uint32_t body = buf + 4u * (4u + mem_r32(&M.cpu, buf + 12) + 1u + 5u);
    const uint32_t pal = DM_TEXT + 0x400; /* 512 bytes */
    int changed = 0;
    for (int i = 0; i < 256; i++) {
        uint16_t e = mem_r16(&M.cpu, body + 2u * (uint32_t)i);
        if (axel_eye_entry(car, i)) {
        } else if (paint[i >> 3] & (1 << (i & 7))) { e = team_recolor(e, team_hue[team], car); changed++; }
        else if (mod_number < 0) e = team_accent_color(e, i, car, team);
        mem_w16(&M.cpu, pal + 2u * (uint32_t)i, e);
    }
    if (!team_clut_upload(pal, n)) fprintf(stderr, "team colors: no room in VRAM for car %d's palette\n", car);
    if (getenv("TM2PC_TEAM_DEBUG"))
        fprintf(stderr, "team colors: group %d car %d team %d, %d entries\n", n, car, team, changed);
}

/* ---- 2 players and computer cars (a Challenge Match) ----
 * The challenge mode with both players (as co-op Survival). The game would
 * end the round when either player is destroyed, and win it only when player
 * 1 lives and the computer cars are out; here the port decides: each side
 * (a team, or without teams each car) is in while it has a car alive, coming
 * back or (a player) with lives left; a player out of lives watches the
 * computer cars. One side left: the round ends, won when it is a player's
 * (rtScheduleEvent 3, else 1). The respawn modes end by their score (dm_frame). */
int vp_duo(void)
{
    return battle_rules() && !ui_survival() && mem_r8(&M.cpu, GA(0x8016475Cu)) == 3 && mem_r8(&M.cpu, GA(0x8016475Cu) + 4) >= 2; /* (2-16 players
                                                                                         * and computer cars: the settings' real count) */
}
static int duo_ended, duo_win_human = -1, duo_win_ai = -1, duo_win_team = -1;
/* ---- an online Versus Tournament (ui/options.c): each stage's knock-out order and the points so far ----
 * Kept with the network states (vp_keep_register): every machine has the host's (a machine coming back too). */
static struct {
    int on;
    uint64_t out_at[CARS_HUMANS_MAX]; /* this round: the step each player was knocked out (VBlank + 1; 0 still in): the
                                       * same step, the same place (both blown up together: both 2nd) */
    int points[CARS_HUMANS_MAX];      /* the tournament's */
    int last[CARS_HUMANS_MAX];        /* the last round's: each player's place (1..) and points won */
    int last_points[CARS_HUMANS_MAX];
    int boss_round, duel_w, duel_b;   /* a boss stage's second round: its winner against the boss (the lowest in points) */
    /* the computer cars, contestants as the players (the same ones every stage; the user's: they score as anyone) */
    uint64_t ai_out_at[16];
    int ai_points[16], ai_last[16], ai_last_points[16];
    int sat_out;                      /* (its start) the others out of it */
    uint64_t round_gv;                /* the round's start */
    int podium;                       /* the podium round (after the last stage): the players on it (bits, and the top
                                       * bit), the others parked; it ends at once (ui/options.c shows the scene after it) */
    int podium_ended;
    unsigned parked;                  /* the players parked in the last boss round (bits): damage on again next round */
    uint64_t parked_at;               /* (that round's end: the next round starts after it) */
    unsigned ai_left;                 /* the computer cars a player joining took the place of (bits): off the standings */
    unsigned ai_leaving;              /* (those leaving after this stage: off the standings once it is scored) */
    int joined[CARS_HUMANS_MAX];      /* a player who joined mid-tournament: the stage they joined at (1..; 0 from the start):
                                       * never the boss in it (the user's 2026-11-11: one full stage played first) */
} versus;
int vp_versus_ai_left(int k) { return k >= 0 && k < 16 && ((versus.ai_left >> k) & 1); }
int vp_versus_joined(int h) { return h >= 0 && h < CARS_HUMANS_MAX ? versus.joined[h] : 0; }
void vp_versus(int on) { memset(&versus, 0, sizeof versus); versus.on = on; versus.duel_w = versus.duel_b = -1; }
int vp_human_left_match(int h) { return h >= 0 && h < CARS_HUMANS_MAX && human_gone[h]; } /* (ui/options.c: online, left) */
void vp_versus_boss_round(int w, int b) /* (ui/options.c) the next round: player w against the boss, driven by b */
{
    versus.boss_round = 1, versus.duel_w = w, versus.duel_b = b, versus.sat_out = 0;
    fprintf(stderr, "versus: the boss round: player %d against player %d (the boss)\n", w + 1, b + 1);
}
/* the boss round over: 1 the winner beat the boss (+3), 2 the boss won (+10, a comeback; the user's 2026-11-05), 0 neither (both out together) */
int vp_versus_boss_result(void)
{
    if (!versus.on || !versus.boss_round) return 0;
    int w = versus.duel_w, b = versus.duel_b, r = 0;
    uint64_t ow = versus.out_at[w] ? versus.out_at[w] : UINT64_MAX, ob = versus.out_at[b] ? versus.out_at[b] : UINT64_MAX;
    memset(versus.last, 0, sizeof versus.last);
    memset(versus.last_points, 0, sizeof versus.last_points);
    memset(versus.ai_last, 0, sizeof versus.ai_last); /* (the computer cars sat it out: no place, no points; they showed the
                                                       * stage's again, the user's 2026-11-11 take) */
    memset(versus.ai_last_points, 0, sizeof versus.ai_last_points);
    if (ow > ob) r = 1, versus.points[w] += 3, versus.last_points[w] = 3, versus.last[w] = 1, versus.last[b] = 2;
    else if (ob > ow) r = 2, versus.points[b] += 10, versus.last_points[b] = 10, versus.last[b] = 1, versus.last[w] = 2;
    fprintf(stderr, "versus: the boss round: %s\n", r == 1 ? "the winner beat the boss (+3)" : r == 2 ? "the boss won (+10)" : "no one");
    versus.boss_round = versus.sat_out = 0;
    versus.parked_at = vp_gv();
    memset(versus.out_at, 0, sizeof versus.out_at);
    return r;
}
void vp_versus_podium(unsigned mask) /* (ui/options.c) the next round is the podium's: these players in it */
{
    /* (player 1 not in the top three: in the round all the same, not on the podium, hidden from the
     * scene. The game's won event needs player 1's record alive: with player 1 parked the round went on, its last
     * player was destroyed, the round lost, and its kill cam's last frame stayed over the whole scene) */
    if (!(mask & 1)) mask |= 1;
    versus.podium = (int)(mask | 0x80000000u), versus.podium_ended = 0, versus.sat_out = 0;
    fprintf(stderr, "versus: the podium round (players %x)\n", mask);
}
void vp_versus_podium_done(void) { versus.podium = versus.podium_ended = versus.sat_out = 0; }
int vp_versus_duelist(int h) { return versus.on && versus.boss_round && (h == versus.duel_w || h == versus.duel_b); }
int vp_versus_parked(int h) { return versus.on && versus.boss_round && h >= 0 && h < 32 && ((versus.parked >> h) & 1); }
int vp_versus_boss_driver(void) { return versus.on && versus.boss_round ? versus.duel_b : -1; }
static void versus_frame(void) /* the boss round: at its start every player not in it out of the arena (they watch) */
{
    if (versus.on && versus.podium && versus.sat_out && !versus.podium_ended && vp_gv() >= versus.round_gv + 4) {
        versus.podium_ended = 1; /* the podium round: over at once (its scene is drawn after it, from its state) */
        /* the round won, a tick from now: as the port's own end of a battle (vp_duo_frame), marked as the port's
         * (vp_ScheduleEvent lets no other end of a 2-player battle through: without it the round went on until a car
         * died, its kill cam's last frame over the whole scene) */
        duo_ended = 1;
        vp_port_event = 1;
        guest_call(GA(0x800D181Cu), 3, 1, 0, 0); /* rtScheduleEvent */
        fprintf(stderr, "versus: the podium round ends\n");
    }
    if (versus.on && versus.parked && !versus.boss_round && !versus.podium && versus.round_gv > versus.parked_at &&
        vp_gv() >= versus.round_gv + 2) {
        /* the round after a boss round: portHumanLeave cleared the parked players' damageable (stats +0xA8), and the
         * next load's carStatsLoad sets it only when stats +0x00 is 0 (it is 1 for a player's record): they drove
         * the next stage unhurt by anything, and it never ended (the 2026-11-08 trailer dry run, Paris) */
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
        for (int h = 0; h < humans && h < CARS_HUMANS_MAX; h++)
            if ((versus.parked >> h) & 1 && !mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C) && !mem_r32(&M.cpu, HUMAN_REC(h) + 0x1F4)) {
                mem_w32(&M.cpu, HUMAN_REC(h) + 0x1F4, 1);
                fprintf(stderr, "versus: player %d takes damage again (parked in the boss round)\n", h + 1);
            }
        versus.parked = 0;
    }
    if (!versus.on || !(versus.boss_round || versus.podium) || versus.sat_out || vp_gv() < versus.round_gv + 2) return;
    versus.sat_out = 1;
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    static uint32_t leave = 1;
    if (leave == 1) leave = port_sym("portHumanLeave");
    for (int h = 0; h < humans && h < CARS_HUMANS_MAX; h++) {
        if (versus.podium ? (versus.podium >> h) & 1 : h == versus.duel_w || h == versus.duel_b) continue;
        if (mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C)) continue;
        life[h].left = 0;
        /* parked, not destroyed (the user's 2026-11-07: they spawned and died at once, in the kill feed, and
         * "Enemies Remaining" counted them): the decomp's leave (HUMANS16_PLAN.md 8.1: the car parked, its shots and
         * every reference gone, the record vacant until the next load; in the rollback state) */
        int r = leave ? (int)guest_call(leave, (uint32_t)h, 0, 0, 0) : -1;
        if (r != h && versus.podium) { /* (refused: the last player when only computer cars made the top three. Left
                                        * where it is, not destroyed: out of lives, the screen stayed on that frame
                                        * through the scene. The scene hides every car not on the podium, and the
                                        * round ends at once) */
            fprintf(stderr, "versus: player %d stays in the podium round (the last player: not parked)\n", h + 1);
            continue;
        }
        if (r != h) { /* (refused: out the old way) */
            uint32_t id = h < 7 ? (uint32_t)h + 1 : h == 7 ? 9u : (uint32_t)h + 3; /* (its object id, as vp_human_left) */
            guest_call5(GA(0x800E5628u), id, 1000, 0, 0, (uint32_t)-1); /* carApplyDamage: out, no one's kill */
        } else {
            coop_out[h] = 1;        /* (not a driver: the enemies' count, the watching) */
            watch_at[h] = vp_gv();  /* their view: the round's two at once */
            if (!versus.podium) versus.parked |= 1u << h;
        }
        fprintf(stderr, "versus: player %d sits the %s round out (%s)\n", h + 1, versus.podium ? "podium" : "boss",
                r == h ? "parked" : "destroyed");
    }
}
int vp_versus_on(void) { return versus.on; }
int vp_versus_points(int h) { return h >= 0 && h < CARS_HUMANS_MAX ? versus.points[h] : 0; }
void vp_versus_last(int h, int *place, int *won)
{
    *place = h >= 0 && h < CARS_HUMANS_MAX ? versus.last[h] : 0;
    *won = h >= 0 && h < CARS_HUMANS_MAX ? versus.last_points[h] : 0;
}
/* a stage over: the players by place (those still in first, then the last knocked out), the places' points added
 * (the user's: by place only, no points for kills); a tie (two still in) shares the place */
void versus_ai_out(int k) /* (vp_AiCarDamageUpdate) computer car k out of the stage */
{
    if (versus.on && !versus.boss_round && k >= 0 && k < 16 && !versus.ai_out_at[k]) versus.ai_out_at[k] = vp_gv() + 1;
}
int vp_versus_ai_n(void) /* the stage's computer cars (the contestants after the players) */
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    return n < 0 ? 0 : n > 16 ? 16 : n > cars_ai_max() ? cars_ai_max() : n;
}
int vp_versus_ai_points(int k) { return k >= 0 && k < 16 ? versus.ai_points[k] : 0; }
/* (ui/options.c: the scoreboard held in a match) a contestant out of this round: knocked out, or sitting it out */
int vp_versus_out_now(int human, int ai)
{
    if (!versus.on) return 0;
    if (human >= 0 && human < CARS_HUMANS_MAX) return versus.out_at[human] != 0 || coop_out[human] || human_gone[human];
    if (ai >= 0 && ai < 16) return versus.ai_out_at[ai] != 0 || (versus.boss_round && ai < vp_versus_ai_n());
    return 0;
}
void vp_versus_ai_last(int k, int *place, int *won)
{
    *place = k >= 0 && k < 16 ? versus.ai_last[k] : 0;
    *won = k >= 0 && k < 16 ? versus.ai_last_points[k] : 0;
}
/* a stage over: everyone by place, the players and the computer cars alike (those still in first, then the last knocked
 * out), the places' points added (the user's: by place only, no points for kills); a tie (two still in) shares the place */
void vp_versus_score(int humans, const int *place_points, int n_points)
{
    if (!versus.on) return;
    if (humans > CARS_HUMANS_MAX) humans = CARS_HUMANS_MAX;
    int ais = vp_versus_ai_n(), n = humans + ais;
    /* a player who left (online): off the tournament's board (the user's, 2026-11-05): no place, no points, and not
     * counted in the others' places (those under them move up); back in the match, on the board again with theirs */
    for (int i = 0; i < n; i++) {
        int place = 1, ai = i >= humans, k = i - humans;
        if (!ai && human_gone[i]) {
            versus.last[i] = versus.last_points[i] = 0;
            fprintf(stderr, "versus: player %d left: off the board\n", i + 1);
            continue;
        }
        uint64_t t = ai ? versus.ai_out_at[k] : versus.out_at[i], mine = t ? t : UINT64_MAX; /* (still in: after everyone out) */
        for (int o = 0; o < n; o++) {
            if (o < humans && human_gone[o]) continue;
            uint64_t u = o >= humans ? versus.ai_out_at[o - humans] : versus.out_at[o], theirs = u ? u : UINT64_MAX;
            place += o != i && theirs > mine;
        }
        int won = place <= n_points ? place_points[place - 1] : 0;
        if (ai) {
            versus.ai_last[k] = place, versus.ai_last_points[k] = won, versus.ai_points[k] += won;
            fprintf(stderr, "versus: computer car %d place %d, +%d, %d points\n", k + 1, place, won, versus.ai_points[k]);
        } else {
            versus.last[i] = place, versus.last_points[i] = won, versus.points[i] += won;
            fprintf(stderr, "versus: player %d place %d, +%d, %d points\n", i + 1, place, won, versus.points[i]);
        }
    }
    memset(versus.out_at, 0, sizeof versus.out_at);
    memset(versus.ai_out_at, 0, sizeof versus.ai_out_at);
    for (int k = 0; k < 16; k++) /* (a computer car a player joining took the place of: its last stage scored, it leaves) */
        if ((versus.ai_leaving >> k) & 1) {
            versus.ai_points[k] = versus.ai_last[k] = versus.ai_last_points[k] = 0;
            fprintf(stderr, "versus: computer car %d leaves (a player joining has its place)\n", k + 1);
        }
    versus.ai_left |= versus.ai_leaving;
    versus.ai_leaving = 0;
}
static int duo_player_out(int i)
{
    if (coop_out[i]) return 1;
    coop_out[i] = 1;
    if (versus.on && !versus.out_at[i]) versus.out_at[i] = vp_gv() + 1;
    fprintf(stderr, "lives: player %d is out\n", i + 1);
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), others = 0;
    for (int o = 0; o < humans && o < CARS_HUMANS_MAX; o++) others |= o != i && !coop_out[o];
    if (others) watch_at[i] = vp_gv() + death_delay; /* another player still in: their wreck, then their view follows the
                                                       * drivers in (X next, O back) */
    return 1;
}
/* online (net/game_tm2.c): human h's machine left the match; at the same step on every machine, its car out for good:
 * destroyed (no one's kill) and no life left, as a player out of lives */
/* The spawn shield (state.energy_shield: human +0xAA, AI +0x07A) goes with the car when its driver changes (the state
 * block is copied, portHandOverCopy), so its end goes too: the time left on the old driver's timer, or (a shield
 * bubble's, ended by the game for the old record) a spawn shield's from now. Without it the shield never ended: the
 * car was invincible for good (a player leaving and rejoining online, Discord 2026-11-21). */
static uint64_t shield_end_moved(uint64_t old_end)
{
    int shield = battle_rules() && !ui_survival() ? battle_rules()->spawn_shield : 2;
    return old_end ? old_end : vp_gv() + 60u * (uint64_t)(shield > 0 ? shield : 2);
}

void vp_human_left(int h)
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (h < 0 || h >= humans || h >= CARS_HUMANS_MAX) return;
    if (human_gone[h] && mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C)) return; /* (gone, its car out already) */
    if (!human_gone[h]) gone_left[h] = life[h].left;
    human_gone[h] = 1; /* (a tournament's next level: its car is back, out again) */
    life[h].left = 0;
    fprintf(stderr, "lives: player %d left the match: the car is out\n", h + 1);
    if (!mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C)) { /* driving: destroyed (the game's death, then out) */
        uint32_t id = h < 7 ? (uint32_t)h + 1 : h == 7 ? 9u : (uint32_t)h + 3; /* (its object id: players.h TM2_HUMAN_ID) */
        guest_call5(GA(0x800E5628u), id, 1000, 0, 0, (uint32_t)-1); /* carApplyDamage: victim id, ..., no attacker */
    } else if (life[h].at) { /* down, coming back: not any more */
        life[h].at = watch_at[h] = 0;
        if (vp_duo()) duo_player_out(h);
    }
}
/* online (net/game_tm2.c): human h's machine left from a CPU / OPEN spot: at the same step on every machine a computer
 * driver takes the car over as it is (decomp request 50, portHumanHandOver: place, health, weapons), on the player's
 * team, with their lives left; human h's record vacant until they come back. 0: not done (the car down or coming
 * back, a co-op game, refused): the caller puts it out (vp_human_left) */
int vp_human_handover(int h)
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (h < 0 || h >= humans || h >= CARS_HUMANS_MAX || human_gone[h] || handed[h]) return 0;
    int ui_tour_on(void);
    if (ui_survival() || ui_tour_on()) return 0; /* (co-op: the computer cars are the enemy) */
    /* (decomp request 105: the garbage drawn after a hand-over was its notice, "X LEFT: CPU DRIVES ON", longer than the
     * message line's 30 letters, over the lives text's sprites: MSG_MAX. Tests: TM2PC_NET_NO_HANDOVER=1, the car out) */
    if (getenv("TM2PC_NET_NO_HANDOVER")) return 0;
    if (mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C) || life[h].at) return 0; /* (destroyed, or coming back: out, as before) */
    uint32_t f = port_sym("portHumanHandOver");
    if (!f) return 0;
    int team = team_of_human(h);
    int j = (int)guest_call(f, (uint32_t)h, (uint32_t)-1, 0, 0);
    if (j < 0 || j >= CARS_MAX || j >= cars_ai_max()) {
        fprintf(stderr, "lives: player %d's car to a computer driver: not possible (%d)\n", h + 1, j);
        return 0;
    }
    gone_left[h] = life[h].left;
    human_gone[h] = 1;
    coop_out[h] = 1; /* (out of the players' rules: the car is a computer car's now) */
    life[h].left = 0;
    life[h].at = watch_at[h] = 0;
    handed[h] = j + 1;
    ai_team_plus1[j] = teams_on() ? team + 1 : 0;
    ai_life[j].left = gone_left[h];
    ai_life[j].at = 0;
    ai_life[j].shield_off = mem_r8(&M.cpu, AI_REC(j) + 0x07A) ? shield_end_moved(life[h].shield_off) : 0; /* (a shield on) */
    life[h].shield_off = 0;
    mem_w32(&M.cpu, GA(0x801879FCu) + 4u * (uint32_t)j, (uint32_t)-1); /* (its target: the game's nearest player) */
    fprintf(stderr, "lives: player %d's car is a computer car now (record %d)\n", h + 1, j);
    return 1;
}
void humans_local_put(int h, int on); /* vplus/humans.c */
/* online: the computer cars a player dropping in may take the place of: the CPU / OPEN ones (after the match's fixed
 * ones, `fixed`), not one a leaver's car went to, nor one already taken; the highest record first (-1: none) */
static int dropin_bot(int fixed)
{
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = (n < cars_ai_max() ? n : cars_ai_max()) - 1; k >= fixed && k >= 0; k--) {
        int taken = dropped_bot[k] || ai_team_plus1[k];
        for (int h = 0; h < CARS_HUMANS_MAX && !taken; h++) taken = handed[h] == k + 1;
        if (!taken && (mem_r32(&M.cpu, AI_REC(k) + 0x30) & 0xFF) != 0xF0) return k;
    }
    return -1;
}
int vp_dropin_room(int fixed) /* (net/game_tm2.c, the host) how many could drop in now */
{
    int c = 0, n = (int)mem_r32(&M.cpu, GA(0x8018101Cu));
    for (int k = fixed; k < n && k < cars_ai_max(); k++) {
        int taken = dropped_bot[k] || ai_team_plus1[k];
        for (int h = 0; h < CARS_HUMANS_MAX && !taken; h++) taken = handed[h] == k + 1;
        c += !taken && (mem_r32(&M.cpu, AI_REC(k) + 0x30) & 0xFF) != 0xF0;
    }
    if (ui_survival() || !battle_rules()) return 0;
    return c;
}
/* online: a new player (human h, the next record) drops into the match on, at the same step on every machine: a CPU /
 * OPEN computer car leaves for good (portCarRemove) and the player starts on its car, fresh, at the start farthest
 * from the others (portCarStartHuman: decomp request 33, CAR_GROUPS.md 14), with the match's lives */
static int vp_human_dropin(int h, int fixed)
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (h != humans || h >= CARS_HUMANS_MAX) return 0;
    int j = dropin_bot(fixed);
    uint32_t rm = port_sym("portCarRemove"), free_of = port_sym("portCarFreeGroupOfType"), start = port_sym("portCarStartHuman");
    if (j < 0 || !rm || !free_of || !start) {
        fprintf(stderr, "lives: player %d cannot drop in (%s)\n", h + 1, j < 0 ? "no computer car to replace" : "not in this build");
        return 0;
    }
    uint32_t type = mem_r32(&M.cpu, AI_REC(j) + 0x30) & 0xFF;
    int r = (int)guest_call(rm, 1, (uint32_t)j, 0, 0);
    int g = r == 1 ? (int)guest_call(free_of, type, 0, 0, 0) : 0;
    humans_local_put(h, 1); /* (its machine, its lives: written before, as the decomp asks) */
    life[h].left = battle_lives() - 1;
    life[h].at = life[h].shield_off = watch_at[h] = 0;
    lives_hud_put(h);
    int i = g > 0 ? (int)guest_call(start, (uint32_t)-1, (uint32_t)g, 0, 0) : -1;
    if (i != h) {
        fprintf(stderr, "lives: player %d's drop-in failed (remove %d, group %d, start %d)\n", h + 1, r, g, i);
        return 0;
    }
    dropped_bot[j] = 1;
    ai_life[j].left = 0;
    ai_life[j].at = ai_life[j].shield_off = 0;
    human_gone[h] = coop_out[h] = handed[h] = back_wait[h] = 0;
    versus.points[h] = versus.last[h] = versus.last_points[h] = 0;
    fprintf(stderr, "lives: player %d drops in, on computer car %d's car (type %02x)\n", h + 1, j + 1, type);
    return 1;
}
/* online: a new player (human h) joins a Versus Tournament from its next stage, at the same step on every machine: no
 * car in this stage (they watch it out); the stages after with them in, in their own car, 0 points; a CPU / Open slot
 * theirs: that computer car leaves the standings after this stage (the user's 2026-11-11) */
static int vp_versus_join_next(int h)
{
    if (h < 0 || h >= CARS_HUMANS_MAX) return 0;
    int ui_versus_dropin(int h, int *stage); /* ui/options.c: the stages after with the player in */
    int stage = 0, k = ui_versus_dropin(h, &stage) - 1;
    versus.points[h] = versus.last[h] = versus.last_points[h] = 0;
    versus.out_at[h] = 0;
    if (k >= 0 && k < 16) versus.ai_leaving |= 1u << k;
    versus.joined[h] = stage + 1; /* (the boss: once a full stage is played, the end of the next one) */
    human_gone[h] = coop_out[h] = handed[h] = back_wait[h] = 0;
    fprintf(stderr, "versus: player %d joins from stage %d%s\n", h + 1, stage + 2, k >= 0 ? ", in a computer car's place" : "");
    return 1;
}
int vp_human_join(int h, int fixed) /* (net/game_tm2.c) human h's slot begins: back, or (a new record) dropping in */
{
    void vp_human_back(int h);
    if (h >= (int)mem_r32(&M.cpu, GA(0x80181014u))) {
        if (versus.on) return vp_versus_join_next(h) ? 3 : 0; /* (a Versus Tournament: from the next stage) */
        return vp_human_dropin(h, fixed) ? 2 : 0;
    }
    vp_human_back(h);
    return 1;
}
/* online: human h's machine came back (dropped, joined again): at the same step on every machine its car is in again,
 * away from the others, its lives as they were when it left */
void vp_human_back(int h)
{
    int humans = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (getenv("TM2PC_NET_DEBUG")) fprintf(stderr, "lives: back %d? humans %d gone %d dead %d\n", h, humans, h < CARS_HUMANS_MAX ? human_gone[h] : -1,
                                           h < humans ? mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C) : -1);
    if (h < 0 || h >= humans || h >= CARS_HUMANS_MAX || !human_gone[h]) return;
    if (handed[h]) { /* its car back from the computer driver, as it is (decomp request 50, portHumanReturn) */
        int j = handed[h] - 1;
        uint32_t f = port_sym("portHumanReturn");
        int left = ai_life[j].left;
        humans_local_put(h, 1); /* (the record's machine and lives: written before, as the decomp asks) */
        life[h].left = left;
        lives_hud_put(h);
        int r = f ? (int)guest_call(f, (uint32_t)h, (uint32_t)j, 0, 0) : -1;
        if (r == -13 && ai_life[j].at) { /* (TM2_HUMAN_ERR_DOWN: the car destroyed, coming back: theirs once it is, vp_ai_frame) */
            life[h].left = 0;
            humans_local_put(h, 0);
            if (!back_wait[h]) fprintf(stderr, "lives: player %d is back: their car once it comes back (computer record %d)\n", h + 1, j);
            back_wait[h] = 1;
            return;
        }
        back_wait[h] = 0;
        handed[h] = 0;
        ai_team_plus1[j] = 0;
        ai_life[j].left = 0;
        uint64_t shield_end = ai_life[j].shield_off;
        ai_life[j].at = ai_life[j].shield_off = 0;
        if (r < 0) { /* (destroyed for good, or another hand-over since: the computer records leave in reverse order;
                      * the player watches) */
            fprintf(stderr, "lives: player %d's car back from computer record %d: not possible (%d)\n", h + 1, j, r);
            life[h].left = 0;
            humans_local_put(h, 0); /* (still vacant) */
            return;
        }
        human_gone[h] = 0;
        coop_out[h] = 0;
        surv_down[h] = 0;
        life[h].left = left;
        life[h].at = 0;
        life[h].shield_off = mem_r8(&M.cpu, HUMAN_REC(h) + 0xAA) ? shield_end_moved(shield_end) : 0; /* (a shield on) */
        lives_hud_put(h);
        fprintf(stderr, "lives: player %d drives their car again (from computer record %d)\n", h + 1, j);
        return;
    }
    human_gone[h] = 0;
    coop_out[h] = 0;
    surv_down[h] = 0;
    life[h].left = gone_left[h];
    life[h].at = 0;
    lives_hud_put(h);
    respawn_human(h);
    fprintf(stderr, "lives: player %d is back in the match\n", h + 1);
}
static void duo_round_start(void)
{
    versus.round_gv = vp_gv();
    duo_ended = 0;
    duo_win_human = duo_win_ai = duo_win_team = -1;
}
static void vp_duo_frame(void)
{
    if (!vp_duo() || duo_ended || vp_dm()) return;
    int teams = teams_on(), sides[CARS_HUMANS_MAX + CARS_MAX], ns = 0, side_h[CARS_HUMANS_MAX + CARS_MAX], side_ai[CARS_HUMANS_MAX + CARS_MAX];
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), H = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (H < 2 || H > CARS_HUMANS_MAX) H = 2;
    for (int c = 0; c < H + n && c < H + cars_ai_max(); c++) { /* (every car: was 13, short of 16) */
        int in, side, h = c < H ? c : -1, k = c < H ? -1 : c - H;
        if (h >= 0) {
            in = !coop_out[h] && (!mem_r8(&M.cpu, HUMAN_REC(h) + 0x9C) || life[h].at);
            int ui_tour_on(void); /* ui/options.c: a custom tournament: the players together, as the tournament's co-op */
            side = teams ? 100 + team_of_human(h) : ui_tour_on() ? 300 : 200 + h;
        } else {
            in = ai_alive(k) || ai_life_pending(k) || (boss_coming() && k >= boss_first); /* (a boss on its way in) */
            side = teams ? 100 + team_of_ai(k) : k;
        }
        if (!in) continue;
        int j = 0;
        while (j < ns && sides[j] != side) j++;
        if (j == ns) { sides[ns] = side; side_h[ns] = h; side_ai[ns] = k; ns++; }
        else if (h >= 0 && side_h[j] < 0) side_h[j] = h; /* a team with a player in it: theirs */
    }
    if (ns > 1) return;
    if (ns == 1 && side_h[0] >= 0 && boss_hold()) return; /* the players' side left: a custom tournament's boss first */
    duo_ended = 1;
    duo_win_human = ns ? side_h[0] : -1;
    duo_win_ai = ns && duo_win_human < 0 ? side_ai[0] : -1;
    duo_win_team = ns && teams ? sides[0] - 100 : -1;
    fprintf(stderr, "2 players: the round is over (%s)\n", !ns ? "no one left" : duo_win_human >= 0 ? "a player's side won" : "the computer cars won");
    vp_port_event = 1;
    guest_call(GA(0x800D181Cu), duo_win_human >= 0 ? 3 : 1, 210, 0, 0);
}
/* roundIsWon (8011E214: the game's flag, set by worldUpdate once no computer car is left; carApplyDamage
 * does nothing while it is set): with the port's rules deciding the round (vp_duo), not won until they
 * say so; with no computer cars at all (3-4 players, the co-op duel) it is set at once, and no car could
 * be hurt */
void F_8011E214_orig(CPU *c);
void vp_RoundIsWon(CPU *c)
{
    F_8011E214_orig(c);
    if (c->r[2] && vp_duo() && !duo_ended) c->r[2] = 0;
}
/* the game's flag hidden while the port's rules decide the round (as vp_RoundIsWon): feat_hooks.c, a player's death */
int vp_round_won_hidden(void) { return vp_duo() && !duo_ended; }
int vp_duo_winner(int *human, int *ai) /* the results: who won (0: not a 2-player match with computer cars) */
{
    if (!vp_duo() || !duo_ended) return 0;
    *human = duo_win_team >= 0 ? -1 : duo_win_human;
    *ai = duo_win_team >= 0 ? -1 : duo_win_ai;
    return 1;
}
static int duo_team_won(void) { return duo_ended ? duo_win_team : -1; }
static int duo_enemies(int p) /* 2 players and computer cars: the cars player p still has to beat */
{
    int teams = teams_on(), n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), c = 0;
    int mine = teams ? team_of_human(p) : -1;
    for (int k = 0; k < n && k < cars_ai_max(); k++)
        if ((ai_alive(k) || ai_life_pending(k)) && (!teams || team_of_ai(k) != mine)) c++;
    int H = (int)mem_r32(&M.cpu, GA(0x80181014u));
    for (int o = 0; o < H && o < CARS_HUMANS_MAX; o++)
        if (o != p && !coop_out[o] && (!teams || team_of_human(o) != mine)) c++;
    return c;
}

/* Car select: a team's palette for car select's spinning car (decomp
 * TEAM_COLORS.md 5: its body CLUT is CARSEL's (768, 23 + car), the same
 * entries as the battle's, Minion's its own) */
void vp_team_palette(int car, int team, const uint16_t *in, uint16_t *out)
{
    const uint8_t *mask = car == 13 ? team_paint_minion_select : car >= 0 && car < 13 ? team_paint[car] : NULL;
    for (int i = 0; i < 256; i++)
        out[i] = axel_eye_entry(car, i) ? in[i]
                 : mask && (mask[i >> 3] & (1 << (i & 7))) ? team_recolor(in[i], team_hue[team % BATTLE_TEAM_COLORS], car)
                 : car < 13 ? team_accent_color(in[i], i, car, team % BATTLE_TEAM_COLORS) : in[i];
}

/* ---- Final showdown music (Custom Music: music_showdown) ----
 * A battle's final moments get the boss fight's music, as the Dark Tooth
 * stage does: audioAlternateMusicTrack (800D3944) plays its track 10
 * through the music manager (a custom file for that track, if set:
 * music.c). Once a round. The final moments: the last two cars (the co-op
 * tournament: the last enemy; Survival: the last car of the last round);
 * the respawn modes: a car or team one kill (point, Gun Game level) from
 * winning, or the time limit's last minute. Not in the tournament's last
 * level (Dark Tooth's), nor in a match with Dark Tooth. */
static void showdown_frame(void)
{
    static const char *alt_test; /* tests: TM2PC_TEST_ALT_MUSIC="V1,V2": the boss music switch at those VBlanks, as
                                  * in the Dark Tooth fight (its music flag set: audioSetDarkToothMusic) */
    static int alt_read;
    if (!alt_read) alt_read = 1, alt_test = getenv("TM2PC_TEST_ALT_MUSIC");
    if (alt_test && *alt_test && vp_gv() >= strtoull(alt_test, NULL, 10)) {
        fprintf(stderr, "test: the boss music switch at %llu\n", (unsigned long long)vp_gv());
        guest_call(GA(0x800D3850u), 0, 0, 0, 0);
        guest_call(GA(0x800D3944u), 0, 0, 0, 0);
        alt_test = strchr(alt_test, ',');
        if (alt_test) alt_test++;
    }
    if (showdown_on || !music_showdown() || !mem_r32(&M.cpu, GA(0x80180BCCu))) return;
    int mode = mem_r8(&M.cpu, GA(0x8016475Cu)), level = mem_r8(&M.cpu, GA(0x8016475Cu) + 24);
    if (mode < 1 || mode > 4 || ((mode == 1 || mode == 2) && level == 8)) return;
    const GameType *t = battle_rules();
    if (t && t->dark_tooth) return;
    const char *why = NULL;
    if (vp_dm()) {
        const MatchSetup *m = battle_match();
        int goal = battle_goal(m, t->mode), best = 0;
        goal = goal ? goal : m->time_limit ? 0 : mode_is(MODE_KING_OF_THE_HILL) || mode_is(MODE_ODDBALL) ? 100 : 10;
        if (mode_is(MODE_GUN_GAME)) goal = GUN_LEVELS;
        if (battle_match_teams() >= 2) best = car_kills_team(vp_dm_team_winner(), NULL, NULL);
        else car_kills_team(-1, &best, NULL);
        if (mode_is(MODE_JUGGERNAUT) || mode_is(MODE_ODDBALL)) /* these score points (the Juggernaut's kills, a target's seconds) */
            for (int i = 0, b = best = 0; i < match_cars(); i++) if ((b = match_car(i)->points) > best) best = b;
        if (goal > 0 && best >= goal - 1) why = "one from winning";
        else if (m->time_limit > 0 && vp_gv() - dm_start + 3600 >= 3600ull * (uint64_t)m->time_limit) why = "the last minute";
    } else {
        int humans = (int)mem_r32(&M.cpu, GA(0x80181014u)), n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), human_in = 0, ai_in = 0;
        if (n < 1) return; /* two players alone: a duel from the start */
        for (int i = 0; i < humans && i < CARS_HUMANS_MAX; i++)
            human_in += !coop_out[i] && (!mem_r8(&M.cpu, HUMAN_REC(i) + 0x9C) || life[i].at);
        for (int k = 0; k < n && k < cars_ai_max(); k++) ai_in += ai_alive(k) || ai_life_pending(k);
        if (!human_in) return;
        if (ui_survival()) {
            if (surv_round >= SURVIVAL_ROUNDS && surv_state == 2 && ai_in == 1) why = "the last car of the last round";
        } else if (human_in + ai_in == 2 || (mode == 2 && ai_in == 1)) {
            if (teams_on()) { /* the two left must be enemies */
                int enemy = 0;
                for (int k = 0; k < n && k < cars_ai_max(); k++) enemy += (ai_alive(k) || ai_life_pending(k)) && team_of_ai(k) != my_team();
                if (!enemy && human_in < 2) return;
            }
            why = ai_in == 1 ? "the last enemy" : "the last two cars";
        }
    }
    if (!why) return;
    showdown_on = 1;
    fprintf(stderr, "showdown: %s, the boss music\n", why);
    music_showdown_play(1); /* the boss track plays the showdown's own song, if it has one */
    guest_call(GA(0x800D3944u), 0, 0, 0, 0); /* audioAlternateMusicTrack: track 10 */
}

/* The port's messages for player p's message line (the HUD's top line, where
 * the game's pickups and "Enemies: N" go): the flash for everyone (a new
 * Juggernaut or target, the hill moved), else their "YOU KILLED X". */
static int mode_status(int p, char *out, size_t n);
static int flash_skip = -1; /* the player the flash is not for (the new Juggernaut: their own message instead) */
static const char *port_message(int view) /* (view: this machine's player; the messages are its human's) */
{
    int humans_of_slot(int L); /* vplus/humans.c */
    int p = humans_of_slot(view);
    if (p < 0 || p >= CARS_HUMANS_MAX) return NULL;
    /* waiting to come back, watching the others: the time left */
    if (life[p].at && !watch_at[p] && vp_gv() < life[p].at && respawn_extra() > 0) {
        static char wait[CARS_HUMANS_MAX][32];
        unsigned left = (unsigned)((life[p].at - vp_gv() + 59) / 60);
        snprintf(wait[p], sizeof wait[0], "RESPAWNING IN %u", left);
        return wait[p];
    }
    if (net_notice[0] && vp_gv() < net_notice_until) return net_notice; /* (online: the new host) */
    if (vp_gv() >= kill_until[p] && streak_queue_n[p]) { /* a kill's next message (a streak's, then YOU KILLED X) */
        int q = p;
        snprintf(kill_text[q], sizeof kill_text[0], "%s", streak_queue[q][0]);
        if (!strcmp(streak_queue[q][0], streak_spree_msg[q])) streak_spree_msg[q][0] = 0;
        for (int i = 1; i < streak_queue_n[q]; i++) memcpy(streak_queue[q][i - 1], streak_queue[q][i], sizeof streak_queue[0][0]);
        kill_until[q] = vp_gv() + (--streak_queue_n[q] ? 120 : 180);
    }
    if (vp_gv() < kill_until[p]) return kill_text[p]; /* (YOU ARE THE JUGGERNAUT, YOU KILLED X) */
    if (vp_gv() < flash_until && p != flash_skip) return flash_text;
    /* Juggernaut and Hunted: who it is, King of the Hill: who holds the hill; in place of the enemies (they come back anyway) */
    static char line[CARS_HUMANS_MAX][48];
    if ((mode_is(MODE_JUGGERNAUT) || mode_is(MODE_ODDBALL) || mode_is(MODE_KING_OF_THE_HILL)) && mode_status(p, line[p], sizeof line[0]))
        return line[p];
    return NULL;
}

/* The kill cam keeps these with the machine (killcam.c): the port's gameplay state outside guest RAM, so a
 * replay plays as the battle did */
void vp_keep_register(void)
{
#define KEEP(v) killcam_keep(&(v), sizeof(v))
    KEEP(spawn_taken); KEEP(spawn_count); KEEP(spawn_recent); KEEP(spawn_recent_at); KEEP(spawn_recent_n); KEEP(roulette_count); KEEP(roulette_w); KEEP(life); KEEP(lives_round); KEEP(vp_port_event);
    KEEP(spectating); KEEP(coop_out); KEEP(ai_life);
    KEEP(boss_first); KEEP(boss_state); KEEP(boss_until);
    KEEP(surv_clock_start); KEEP(surv_clock_end);
    KEEP(surv_round); KEEP(surv_state); KEEP(surv_until); KEEP(surv_down); KEEP(dm_start); KEEP(dm_ended);
    KEEP(gun_seq); KEEP(gun_count); KEEP(specials); KEEP(special_n); KEEP(hunted_team); KEEP(hunted_tick); KEEP(mode_retarget);
    KEEP(watch_at); KEEP(spectate_at); KEEP(gun_level_given); KEEP(hill_x); KEEP(hill_y); KEEP(hill_z); KEEP(hill_holder); KEEP(hill_contested);
    KEEP(hill_moved); KEEP(hill_tick); KEEP(hill_count); KEEP(hill_spot_of); KEEP(duo_ended); KEEP(duo_win_human); KEEP(duo_win_ai);
    KEEP(duo_win_team); KEEP(prev_easy); KEEP(prev_pad); KEEP(hill_goal); KEEP(jugg_damage); KEEP(jugg_frac); KEEP(jc_pending); KEEP(jc_driver);
    void feats_keep_register(void); /* profile/feat_hooks.c: the last hits */
    feats_keep_register();
    void ui_keep_register(void); /* ui/options.c: a Versus Tournament's totals (the honorable mentions), its computer cars */
    ui_keep_register();
    KEEP(spect_retarget); KEEP(team_next); KEEP(equal_next); KEEP(hill_retarget); KEEP(versus);
    KEEP(gu_target); KEEP(gu_node); KEEP(gu_points); KEEP(gu_hp); KEEP(gu_banned); KEEP(gu_best); KEEP(gu_until);
    KEEP(bf_last); KEEP(bf_ticks); KEEP(bf_refill_at); KEEP(bf_roulette_at); KEEP(win_at); KEEP(vs_at); KEEP(freeze_fired); KEEP(fired);
    KEEP(pickup_step); KEEP(pickup_taken_step); KEEP(pickup_kind0); KEEP(pickup_node); KEEP(pickup_model); KEEP(pickup_models_ready);
    KEEP(burn_wait); KEEP(jc_opp); /* (Deathmatch's computer cars wait the last death's burning: a machine in mid-match had 180, the
                                    * host 420; the Juggernaut car's parked record) */
    KEEP(test_round_gv); KEEP(test_round_fired); KEEP(test_round_n); /* (tests, trailer: the round's kills, as the host's) */
    KEEP(human_gone); KEEP(gone_left); KEEP(handed); KEEP(ai_team_plus1); KEEP(back_wait); KEEP(dropped_bot); /* (online: who left, who may come back) */
    KEEP(jump_wait_dir); KEEP(jump_wait_at); /* (L1 + R1 jump: a lone shoulder's weapon switch waiting) */
#undef KEEP
}

/* Amazonia's lava (decomp RENDER_ANSWERS R6.2): its tiles of 3,200 down to 200 meet at T-junctions at every band
 * boundary, and nothing is under them; retail's whole-pixel corners keep the cracks under half a pixel, the port's far
 * meshes (each rounded on its own) open thin slivers, and through them the view's clear showed: the arena's brown
 * (60, 45, 20), the user's "black triangles in the lava". The clear is only that (no fog uses it): the lava's own color
 * there (CLUT 1036's mean through its cycle), so a crack shows lava. */
void F_8015659C_orig(CPU *c);
void vp_ViewSetBgColor(CPU *c)
{
    if (c->r[4] == 60 && c->r[5] == 45 && c->r[6] == 20) { /* (Amazonia's only) */
        c->r[4] = 165, c->r[5] = 12, c->r[6] = 0;
        fprintf(stderr, "amazonia: the clear in the lava's color\n");
    }
    F_8015659C_orig(c);
}

/* The drawing's state when an order table first goes wrong (runtime/gpu.c: a list that loops; the players' broken
 * pictures and freezes online, 2026-11-22: Ryupichu, Slick, Phoenix, Da Toasty Mane): once a run, so a log says
 * which frame record the list was, how full each record's packets were, whether steps were being run again, a frame in
 * between drawn, the mirror on. Records (renderView0Frames, 6 of 0x80: +0 the order table, +4 / +8 / +C the packet
 * pointers), the three slots drawn into (D_80180EE4, renderBufferView1, renderBufferRearView). */
void vp_draw_report(uint32_t madr, const char *why)
{
    static int told;
    if (told++) return;
    int interp_redrawing(void), netplay_in_match(void), netplay_rollback(void), four_views(void);
    extern int rbs_resimulating;
    extern int32_t rbs_step_running;
    fprintf(stderr, "draw: %s (list %08X) at VBlank %llu: step %d%s%s%s%s, %d views, mirror %u (allowed %u)\n", why, madr,
            (unsigned long long)M.vblanks, (int)rbs_step_running, rbs_resimulating ? ", steps being run again" : "",
            interp_redrawing() ? ", a frame in between" : "", netplay_in_match() ? (netplay_rollback() ? ", online" : ", online lockstep") : "",
            feats_in_battle() ? ", in a battle" : "", four_views(), mem_r32(&M.cpu, GA(0x801809DCu)), mem_r32(&M.cpu, GA(0x801809E0u)));
    uint32_t recs = GA(0x8018118Cu);
    fprintf(stderr, "draw:   drawn into: view 0 %08X, view 1 %08X, mirror %08X\n", mem_r32(&M.cpu, GA(0x80180EE4u)),
            mem_r32(&M.cpu, GA(0x80180EE4u) + 4), mem_r32(&M.cpu, GA(0x80180EE4u) + 8));
    static const char *const names[6] = {"view 0 A", "view 0 B", "view 1 A", "view 1 B", "mirror A", "mirror B"};
    for (uint32_t k = 0; k < 6; k++) {
        uint32_t r = recs + 0x80u * k, ot = mem_r32(&M.cpu, r);
        fprintf(stderr, "draw:   %s %08X: order table %08X, packets %08X %08X %08X%s\n", names[k], r, ot, mem_r32(&M.cpu, r + 4),
                mem_r32(&M.cpu, r + 8), mem_r32(&M.cpu, r + 12), (ot & 0x1FFFFFFFu) <= (madr & 0x1FFFFFFFu) &&
                (madr & 0x1FFFFFFFu) < (ot & 0x1FFFFFFFu) + 0x4000u ? "  <- this list's" : "");
    }
}

/* A debris burst whose pieces' models are not in RAM (or not models): not drawn. fxDebrisDraw (8014C19C) walks each
 * piece's model's primitives; a model pointer gone bad (a co-op tournament's next level, Phoenix's 2026-11-22: reads at
 * FFFFFF0D, FFFFFF02 for ever) gave it billions of them and the game froze. Drawing only: the burst's record is left
 * as it is. Burst +4 the pieces' count, +0xE8 the pieces (20 bytes: model, x, y, z, shift); a model +4 vertices,
 * +8 normals, +12 primitives, +16 their count. */
void F_8014C19C_orig(CPU *c);
static int debris_ram(uint32_t p) { return p >= 0x80000000u && p < 0x80000000u + GUEST_RAM_BYTES; }
void vp_FxDebrisDraw(CPU *c)
{
    uint32_t obj = c->r[5];
    int ok = debris_ram(obj);
    int32_t n = ok ? (int32_t)mem_r32(c, obj + 4) : 0;
    if (n < 0 || n > 256) ok = 0;
    for (int32_t i = 0; ok && i < n; i++) {
        uint32_t model = mem_r32(c, obj + 0xE8 + 20u * (uint32_t)i);
        if (!debris_ram(model) || !debris_ram(model + 16) || !debris_ram(mem_r32(c, model + 4)) ||
            !debris_ram(mem_r32(c, model + 8)) || !debris_ram(mem_r32(c, model + 12)) || mem_r32(c, model + 16) > 4096)
            ok = 0;
    }
    if (!ok) {
        static int told;
        if (told++ < 3) fprintf(stderr, "debris: a burst at %08x with a bad model (%d pieces): not drawn\n", obj, (int)n);
        return;
    }
    F_8014C19C_orig(c);
}

/* ---- Burning wrecks: kept as the original keeps them ----
 * carWreckUpdate (800E75F4, decomp car_crash.c) keeps a computer car's burning wreck only while the car's target is
 * within 4320 of it, else blows it up at once. Retail's target is the nearest player; the port's targeting (computer
 * cars fighting each other, the modes' targets) points a dead car at another car, often far, so its wreck went at
 * once (the user's "most of the time there is no burning car left", 2026-11-04). Here a dead computer car's wreck is
 * judged by the nearest player (target -1, the game's own), its target put back after. */
void F_800E75F4_orig(CPU *c);
void vp_CarWreckUpdate(CPU *c)
{
    uint32_t car = c->r[4];
    int human = c->r[5] != 0, k = -1;
    if (!human && car >= AI_REC(0) && (car - AI_REC(0)) % AI_REC_SIZE == 0) k = (int)((car - AI_REC(0)) / AI_REC_SIZE);
    if (k < 0 || k >= cars_ai_max()) {
        F_800E75F4_orig(c);
        return;
    }
    uint32_t at = GA(0x801879FCu) + 4u * (uint32_t)k, target = mem_r32(c, at);
    mem_w32(c, at, (uint32_t)-1);
    F_800E75F4_orig(c);
    mem_w32(c, at, target);
}

/* The HUD on the 60 FPS frames in between (vplus/interp.c's redraws): a step's frame shows the picture drawn at the step
 * before (drawn, then the step run: the PS1's double buffer), the frames in between are drawn and shown at once; their
 * HUD read the records live, a step newer than the step frame after them: a weapon switched, picked up or used showed
 * new, new, then old for one frame (the user's 2026-11-05 take, Hong Kong; any HUD number too). Here the frames in
 * between draw the HUD from the human records as the last step frame's HUD was drawn: put in for the HUD's drawing
 * only, the live ones back right after (the game's state, the step, untouched). */
static uint8_t hud_snap_recs[CARS_HUMANS_MAX * HUMAN_REC_SIZE], hud_live_recs[CARS_HUMANS_MAX * HUMAN_REC_SIZE];
static int hud_snap_n;
static HudSettings hud_snap_set[4], hud_live_set[4];   /* each player's HUD settings as the step frame drew them */
static uint8_t hud_snap_vis[8], hud_live_vis[8];       /* hudRadarVisible (80180A6C), hudWeaponListVisible (80180A70) */
static int hud_snap_in(void)
{
    int interp_redrawing(void);
    int n = (int)mem_r32(&M.cpu, GA(0x80181014u));
    if (n < 1) return 0;
    if (n > CARS_HUMANS_MAX) n = CARS_HUMANS_MAX;
    size_t bytes = (size_t)n * HUMAN_REC_SIZE;
    /* the HUD's style and radar too (Select + Down / Left in a battle, four.c): changed in a step, the frames in
     * between showed the new style, then the step frame the old one again, then the new (Sage's flicker, 2026-11-22) */
    if (!interp_redrawing()) { /* a step frame: its HUD's records, kept */
        memcpy(hud_snap_recs, mem_direct(&M.cpu, HUMAN_REC(0), (int)bytes), bytes);
        memcpy(hud_snap_set, hud_players, sizeof hud_snap_set);
        for (int k = 0; k < 8; k++) hud_snap_vis[k] = mem_r8(&M.cpu, GA(0x80180A6Cu) + (uint32_t)k);
        hud_snap_n = n;
        return 0;
    }
    if (hud_snap_n != n || getenv("TM2PC_HUD_LIVE")) return 0; /* (tests: the live records, as before) */
    uint8_t *live = mem_direct(&M.cpu, HUMAN_REC(0), (int)bytes);
    memcpy(hud_live_recs, live, bytes);
    memcpy(live, hud_snap_recs, bytes);
    memcpy(hud_live_set, hud_players, sizeof hud_live_set);
    memcpy(hud_players, hud_snap_set, sizeof hud_snap_set);
    for (int k = 0; k < 8; k++) {
        hud_live_vis[k] = mem_r8(&M.cpu, GA(0x80180A6Cu) + (uint32_t)k);
        mem_w8(&M.cpu, GA(0x80180A6Cu) + (uint32_t)k, hud_snap_vis[k]);
    }
    return n;
}
static void hud_snap_out(int n)
{
    if (n <= 0) return;
    memcpy(mem_direct(&M.cpu, HUMAN_REC(0), (int)((size_t)n * HUMAN_REC_SIZE)), hud_live_recs, (size_t)n * HUMAN_REC_SIZE);
    memcpy(hud_players, hud_live_set, sizeof hud_live_set);
    for (int k = 0; k < 8; k++) mem_w8(&M.cpu, GA(0x80180A6Cu) + (uint32_t)k, hud_live_vis[k]);
}
/* The weapon the HUD shows when the selected one is used up: the game moves on to the next one only once the shot's
 * cooldown is over (weaponFireSelected: weaponCycleSelection(-1) then), so for that moment every style showed no
 * weapon at all, as if none were left; the TM2 list showed them all (Brimstone's report, 2026-11-23). Here, for the
 * HUD's drawing only, an empty selection reads as the one the game will pick (its own rule: the slots after it in
 * turn, wrapping, the first with ammo); the record's byte back right after (the step, firing's timing untouched) */
static int8_t hud_sel_was[CARS_HUMANS_MAX];
static int hud_sel_n;
static void hud_next_weapon_in(void)
{
    int n = (int)mem_r32(&M.cpu, GA(0x80181014u));
    hud_sel_n = n < 0 ? 0 : n > CARS_HUMANS_MAX ? CARS_HUMANS_MAX : n;
    for (int h = 0; h < hud_sel_n; h++) {
        uint32_t rec = HUMAN_REC(h);
        int8_t s = (int8_t)mem_r8(&M.cpu, rec + 0x7D5);
        hud_sel_was[h] = s;
        if (s >= 0 && s < 8 && (int16_t)mem_r16(&M.cpu, rec + 0x7D6 + 2u * (uint32_t)s) > 0) continue; /* (one with ammo) */
        int at = s >= 0 && s < 8 ? s : 0, start = at;
        do at = (at + 1) % 8;
        while (at != start && (int16_t)mem_r16(&M.cpu, rec + 0x7D6 + 2u * (uint32_t)at) <= 0);
        if ((int16_t)mem_r16(&M.cpu, rec + 0x7D6 + 2u * (uint32_t)at) > 0) {
            mem_w8(&M.cpu, rec + 0x7D5, (uint8_t)at);
            static int debug = -1; /* (tests: TM2PC_MOVE_DEBUG) */
            if (debug < 0) debug = getenv("TM2PC_MOVE_DEBUG") != NULL;
            if (debug) fprintf(stderr, "hud: human %d's weapon %d used up: weapon %d shown (frame %d, VBlank %llu)\n", h + 1, s, at, vp_round_frames(),
                           (unsigned long long)M.vblanks);
        }
    }
}
static void hud_next_weapon_out(void)
{
    for (int h = 0; h < hud_sel_n; h++) mem_w8(&M.cpu, HUMAN_REC(h) + 0x7D5, (uint8_t)hud_sel_was[h]);
    hud_sel_n = 0;
}
static int hud_snap_depth; /* (one swap, the outermost: a HUD drawer calling another must not take the snapshot as live) */
#define HUD_SNAPPED(body) { int outer = !hud_snap_depth++, n = outer ? hud_snap_in() : 0; if (outer) hud_next_weapon_in(); \
                            body(c); if (outer) hud_next_weapon_out(); hud_snap_out(n); hud_snap_depth--; }
void vp_HudDraw(CPU *c) HUD_SNAPPED(vp_HudDraw_body)
void vp_HudDrawSplitVertical(CPU *c) HUD_SNAPPED(vp_HudDrawSplitVertical_body)
void vp_HudDrawSplitHorizontal(CPU *c) HUD_SNAPPED(vp_HudDrawSplitHorizontal_body)
