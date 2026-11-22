/* Players 3 to 16 in a battle: human records 2..15 (the decomp's TM2_PORT_HUMANS=16, HUMANS16_PLAN.md).
 *
 * In every mode (a Challenge Match, Survival, the tournaments) the settings carry the real count of humans and humans
 * 3..'s cars are in the decomp's sessionPortHumanCars; its load (sessionLoadArenaAndCars) and binding
 * (sessionBindCars) make every human, load groups 1..N, then the computer cars. The port sets the variant's switches
 * for the load (portCarOptions and the rest, below), the views on this machine (offline every player's, online this
 * machine's players'), and players 3-4's pad tables for the old per-port reads (padCheckAction through port 0 set to
 * that player's for the moment). Sound: their engine and gun loops go to the shared list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/runtime.h"
#include "runtime/rebase_table.h"
#include "vplus.h"
#include "settings.h"
#include "profile/profile.h"
#include "cars.h" /* the car records and counts */
#include "profile/battle.h"
#include "mods/mods.h"

#define SETTINGS GA(0x8016475Cu)
#define PAD_LATCHED GA(0x80180D34u) /* padButtonsLatched: port 0 the low half, port 1 the high */

static int four_n;          /* humans in the battle: 3 to 16 (0: the game's 1-2) */
static int8_t four_car[14]; /* humans 3..16's cars (car select indices; [h - 2]) */

void port_config_sync(void); /* vplus/port_config.c */
void four_setup_cars(int humans, const int8_t *cars) /* humans 3..: cars[h - 2] (NULL or -1: car select's) */
{
    four_n = humans > 2 && humans <= 16 ? humans : 0;
    for (int i = 0; i < 14; i++) four_car[i] = cars ? cars[i] : -1;
    if (cars) /* (a car number: his second phase off; car select's mod_cars_human_number sets it after) */
        for (int h = 2; h < 16; h++) mod_dt2_human(h, cars[h - 2]);
    port_config_sync(); /* (the game's C sees the count before the views are made: decomp/ overlay) */
    if (four_n) fprintf(stderr, "four players: %d humans, cars %d and %d\n", four_n, four_car[0], four_car[1]);
}
void four_setup(int humans, int car3, int car4)
{
    int8_t cars[14];
    memset(cars, -1, sizeof cars);
    cars[0] = (int8_t)car3;
    cars[1] = (int8_t)car4;
    four_setup_cars(humans, cars);
}
int four_humans(void) { return four_n; }
/* the views on this machine: offline every player's (3-4: four_n); online this machine's players' only (more than 2: the
 * quarters; 1-2: the game's own one or two views, none of these) */
int humans_local_count(void); /* vplus/humans.c */
int four_vn(void)
{
    int local = humans_local_count();
    return local < 0 ? four_n : local > 2 ? local : 0;
}

/* The tournament with 3-16 players: tournamentOpponentCount(settings, level) is a level's cars, the humans included
 * (the menus take the settings' humans off for the opponents: the menu's 2, then the real count once loaded; one
 * player: the count less 1 computer cars). Any number of players faces the computer cars one player has (the user's,
 * 2026-10-27: no extra cars for players 3-4; their lives per level stay as they were, vplus/fixes.c). As the arena has
 * room: its cars (cars_arena_room: the game's 8-11 today, 16 with decomp request 32 step 2), the settings' opponent
 * slots (10, more with step 2) and the 13 cars to draw from (with duplicates: no limit). */
void F_8012AFE8_orig(CPU *c);
void vp_TournamentOpponentCount(CPU *c)
{
    int level = (int)c->r[5], menu_humans = mem_r8(c, c->r[4] + 4); /* (the settings' humans: the menu's 2, then N) */
    F_8012AFE8_orig(c);
    /* 2 players' co-op tournament (mode 2): 1 player's count, as 3-4 players get, always (retail takes each human off the
     * level's cars: Moscow 5 alone, 4 in co-op, the PS1's limit; the user's 2026-11-10: not an option) */
    int n = four_n ? four_n : menu_humans == 2 && mem_r8(c, c->r[4]) == 2 ? 2 : 0;
    if (!n || level < 1 || level > 8) return;
    int computer = (int)c->r[2] - 1, most = cars_arena_room(level) - n;
    int slots = cars_ai_max() > 9 ? CARS_MAX - 2 : 10; /* (the settings' list: players 3-4 and the computer cars) */
    if (most > slots - (n - 2)) most = slots - (n - 2);
    if (!cars_duplicates() && most > 13 - n) most = 13 - n;
    if (most > cars_arena_types(level) - n) most = cars_arena_types(level) - n; /* (the game draws different cars:
                                                                                         * as many types as the arena's VRAM holds) */
    if (computer > most) computer = most;
    if (computer < 1) computer = 1;
    c->r[2] = (uint32_t)(menu_humans + computer); /* (the menus take the settings' humans off: the computer cars left) */
    fprintf(stderr, "tournament: level %d, %d players, %d computer cars\n", level, n, computer);
}
int four_car_of(int player) { return player >= 2 && player < 16 ? four_car[player - 2] : -1; }

void F_8011A268_orig(CPU *c);
void vp_HumanCarsSetCount(CPU *c) /* humanCarsSetCount(n): the session's 2 is 3 or 4 (the tournaments' way) */
{
    if (four_n && c->r[4] == 2) c->r[4] = (uint32_t)four_n;
    F_8011A268_orig(c);
}

/* The load (sessionLoadArenaAndCars(settings)): players 3-4's cars first among the opponents (the load groups come out
 * as with 4 humans), through the settings' accessors (opponent slots 9..15: the decomp's sessionPortOpponentCars, request
 * 32). The variant's switches (portCarOptions), set before the load and kept for the match: the car DMDs high (bit 1)
 * always; more than 11 cars also starts spread and the raised weapon pools (bits 2-3). */
void F_8012BAFC_orig(CPU *c);
/* after the load, "LOADING" (the game's, at the top) held a second: on the PS1 the disc kept it up a while; on PC the load
 * is instant (the user's, 2026-10-17). Online: where every machine will wait for the others to be ready. (Scripted
 * runs: none, so the tests' timings stay.) */
static void load_hold(void)
{
    if (!getenv("TM2PC_TEST_LOAD_HOLD") && (!M.live_input || getenv("TM2PC_UI_SCRIPT"))) return; /* (tests: the hold seen) */
    for (int i = 0; i < 60; i++) rt_vblank();
}
/* the computer cars: the settings' opponents (every mode counts only them: vp_TournamentOpponentCount's too) */
static int four_computer(uint32_t s)
{
    int opps = mem_r8(&M.cpu, s + 5);
    return opps < 0 ? 0 : opps;
}
int four_computer_cars(void) { return four_computer(SETTINGS); } /* (the line-up) */
void vp_SessionLoad(CPU *c)
{
    uint32_t s = c->r[4], opt = port_sym("portCarOptions");
    if (getenv("TM2PC_TEST_P1_CAR")) mem_w8(c, s + 8, (uint8_t)atoi(getenv("TM2PC_TEST_P1_CAR"))); /* tests: player 1's car */
    uint32_t dtp = port_sym("portDtPlayer"); /* a player's Dark Tooth (decomp request 36, DARK_TOOTH_PLAYER.md 3): Minion's
                                              * engine (0x41; Sweet Tooth's is 0x3C), the rest the decomp's defaults (the
                                              * user's: retail balance, YOU WON only, missiles forward) */
    if (dtp) mem_w32(c, dtp + 0x0C, 0x41);
    /* 3-16 humans the decomp's way (HUMANS16_PLAN.md H4), in every mode (a Challenge Match, Survival, the tournaments):
     * the settings' humans the real count, humans 3..'s cars in sessionPortHumanCars; its load and binding make every
     * human (groups 1..N, then the computer cars) */
    int native = four_n != 0;
    if (native) {
        mem_w8(c, s + 4, (uint8_t)four_n);
        uint32_t hc = port_sym("sessionPortHumanCars");
        for (int h = 2; hc && h < four_n; h++) mem_w8(c, hc + (uint32_t)(h - 2), (uint8_t)four_car[h - 2]);
    }
    int opps = four_computer(s), humans = mem_r8(c, s + 4);
    /* Dark Tooth's second phase (decomp request 118, DARK_TOOTH_PLAYER.md, portDtPlayer.phase2 +0x28, bit h): each human who chose it in car
     * select (LOOK_DT2) and drives Dark Tooth; read by the decomp at every start, restart and respawn. Game state: the
     * same on every machine (online from the lobby's skins). */
    if (dtp) {
        int mod_cars_human_car(int h); /* mod_cars.c */
        unsigned mask = 0, want = mod_dt2_mask();
        if (getenv("TM2PC_TEST_DT2")) want |= (unsigned)strtoul(getenv("TM2PC_TEST_DT2"), NULL, 16); /* tests: these humans */
        for (int h = 0; h < humans && h < 16; h++)
            if ((want >> h & 1) && mod_cars_human_car(h) == 14) mask |= 1u << h;
        mem_w32(c, dtp + 0x28, mask);
        if (mask) fprintf(stderr, "dark tooth: second phase for humans %X\n", mask);
    }
    int slots = cars_ai_max() > 9 ? CARS_MAX - humans : 10; /* (the settings' opponent slots: retail's 10) */
    int had = mem_r8(c, s + 5);
    if (opps > slots) opps = slots < 0 ? 0 : slots;
    int total = humans + opps;
    /* bit 1 (the car DMDs in the variant's portCarDmdArea) always: the host's own place for them above 2 MB
     * (vp_FileLoad) is the variant's port memory now; bits 2-3 (starts spread, raised pools) past 11 cars, else as
     * retail. (Bit 0, repeats in the game's pickers: the port picks itself.) */
    int ui_survival_boss(void); /* ui/options.c: Survival's last car is Dark Tooth */
    int vp_dt_player_offered(void); /* fixes.c: Dark Tooth in car select (bit 8) */
    /* bit 5, texture banks (decomp request 32 step 4): each car's textures and CLUTs in a bank of its own, when the
     * match needs them: more types than the arena's VRAM holds, or a type twice (each car its own paint and fade);
     * else as retail (TM2PC_TEX_BANKS=1: always, 0: never) */
    int banks = 0, dup_types = 0;
    if (cars_banks()) {
        int used[256] = {0}, types = 0, twice = 0, cars[CARS_MAX + 4], nc = 0;
        for (int i = 0; i < humans && i < 2; i++) cars[nc++] = mem_r8(c, s + 8 + (uint32_t)i);
        for (int i = 2; native && i < humans; i++) cars[nc++] = four_car[i - 2];
        for (int i = 0; i < opps && nc < CARS_MAX + 4; i++) cars[nc++] = (int)(guest_call(GA(0x8013783Cu), s, (uint32_t)i, 0, 0) & 0xFF);
        for (int i = 0; i < nc; i++) { int t = cars[i] & 0xFF; if (used[t]++) twice = 1; else types++; }
        if (getenv("TM2PC_LOAD_DEBUG")) { /* tests: the cars loaded */
            fprintf(stderr, "cars loaded:");
            for (int i = 0; i < nc; i++) fprintf(stderr, " %d", cars[i]);
            fputc(10, stderr);
        }
        const char *e = getenv("TM2PC_TEX_BANKS");
        banks = (e && e[0] == '1') || twice || types > cars_arena_types_vram(mem_r8(c, s + 24)) ||
                ui_survival_boss(); /* (Dark Tooth's 31 images: beside him only a few types fit the VRAM) */
        for (int i = 0; i < nc; i++) banks |= (cars[i] & 0xFF) == 14; /* (a player's Dark Tooth, car 14: the same) */
        for (int i = 0; i < nc; i++) banks |= (cars[i] & 0xFF) >= 0x20; /* (a computer car on a mod: its own textures, its
                                                                           * base's type) */
        if (banks) fprintf(stderr, "cars: texture banks (%d types%s)\n", types, twice ? ", a type twice" : "");
        dup_types = twice;
    }
    /* bit 6: Dark Tooth's fight (his weapons, his second stage, rows 240..255 free) on any arena: Survival with him
     * (decomp request 33-2e, CAR_GROUPS.md 11.2; it replaces the overlay's portConfig.boss_anywhere) */
    /* bit 4: each car's engine (and flame) its own voice, the far engines dropping first when the 24 voices run out
     * (decomp request 33-3): with a type twice or more than 11 cars; else retail's calls */
    int sound = port_sym("portCarSoundFrame") && (dup_types || total > 11);
    /* Gun Game's special levels (decomp request 35, GUN_GAME_SPECIALS.md 4.4-4.5): the car files of the specials whose
     * cars are not in the match loaded as spares (portCarSpecialLoad; never cleared by the C: written every load), the
     * raised pools (bit 3: every car may fire one missile special) and banks (the spares' textures, any arena) */
    unsigned vp_gun_specials(void); /* fixes.c: the game type's special levels (0: none, or not Gun Game) */
    unsigned vp_arena_options(void); /* fixes.c: hazards / destructibles off (decomp request 38, ARENA_SWITCHES.md, latched by roundStart) */
    uint32_t arena_opt = port_sym("portArenaOptions");
    if (arena_opt) {
        mem_w32(c, arena_opt, vp_arena_options());
        if (vp_arena_options()) fprintf(stderr, "arena: hazards %s, destructibles %s\n", vp_arena_options() & 1 ? "off" : "on",
                                        vp_arena_options() & 2 ? "off" : "on");
    }
    uint32_t balance = port_sym("portBalance"); /* decomp requests 40-41: the balancing rules (latched by roundStart) */
    if (balance) {
        mem_w32(c, balance, vp_balance_word());
        fprintf(stderr, "balance: %05X\n", vp_balance_word());
    }
    uint32_t visual = port_sym("portVisual"); /* decomp request 42 (CUSTOM_TOURNAMENT_ANSWERS.md): the taser and the lightning by depth, the taser lower */
    if (visual) mem_w32(c, visual, vp_visual_word());
    /* bit 10: computer Dark Tooths ordinary (his first phase, no boss parts; decomp request 42, CUSTOM_TOURNAMENT_ANSWERS.md): a Challenge Match's or a
     * custom tournament's regular Dark Tooth; never with his boss fight in the match (Survival's, a stage's boss) */
    int dt_ordinary = 0, dts = 0;
    for (int i = 0; i < opps; i++) dts += (guest_call(GA(0x8013783Cu), s, (uint32_t)i, 0, 0) & 0xFF) == 14;
    dt_ordinary = dts > 0;
    /* (Challenge Match only, mode 3: the tournament's own Dark Tooth, level 8 and a 2-player game, keeps his whole
     * fight; the decomp's caveat: bit 10 makes every unmarked computer Dark Tooth ordinary) */
    dt_ordinary = dt_ordinary && !ui_survival_boss() && mem_r8(c, s) == 3;
    if (dt_ordinary) fprintf(stderr, "cars: Dark Tooth as an ordinary opponent (%d)\n", dts);
    unsigned specials = vp_gun_specials();
    uint32_t spec_load = port_sym("portCarSpecialLoad");
    if (spec_load) mem_w32(c, spec_load, specials & BATTLE_SPECIAL_FILES);
    if (specials && cars_banks()) banks = 1;
    if (specials) fprintf(stderr, "gun game: special levels %04X (car files %04X)\n", specials, specials & BATTLE_SPECIAL_FILES);
    if (opt) mem_w32(c, opt, 0x2u | (total > 11 ? 0xCu : 0u) | (specials || dts > 1 ? 0x8u : 0u) | (sound ? 0x10u : 0u) | (banks ? 0x20u : 0u) |
                             (ui_survival_boss() ? 0x40u : 0u) | (vp_dt_player_offered() ? 0x100u : 0u) |
                             (dt_ordinary ? 0x400u : 0u));
    if (total > 11) fprintf(stderr, "cars: %d in the match (the variant's 16-car switches on)\n", total);
    if (native && had != opps) mem_w8(c, s + 5, (uint8_t)opps); /* (the room: humans and computer cars 16 at most) */
    if (native && getenv("TM2PC_LOAD_DEBUG")) {
        uint32_t hc = port_sym("sessionPortHumanCars");
        fprintf(stderr, "load: mode %d humans %d opponents %d cars %d %d | %d %d (table %08X)\n", mem_r8(c, s), mem_r8(c, s + 4),
                mem_r8(c, s + 5), mem_r8(c, s + 8), mem_r8(c, s + 9), hc ? mem_r8(c, hc) : -1, hc ? mem_r8(c, hc + 1) : -1, hc);
    }
    int netplay_in_match(void);
    void mod_cars_load(int humans); /* vplus/mod_cars.c: the cars on mods, their files (online: the server's mods only) */
    int mods_online(void); /* mods/mods.c: a server's set in force (net/lobby_mods.c) */
    mod_cars_load(netplay_in_match() && !mods_online() ? -1 : mem_r8(c, s + 4));
    uint32_t carsel = port_sym("portCarSel"); /* car select's carousel copies: each machine's own last car select, read by
                                                * no step; emptied before an online match, as rollback hashes them */
    if (carsel && netplay_in_match()) for (uint32_t w = 0; w < 0x7E0; w += 4) mem_w32(c, carsel + w, 0);
    F_8012BAFC_orig(c);
    load_hold();
}

/* the globe between tournament levels (screenWorldGlobe 801334E8): held 2 seconds on the place it turned to, as the
 * PS1 seemed to (the disc loading the next level behind it; on PC the load is instant: the user's, 2026-10-17) */
void F_801334E8_orig(CPU *c);
void vp_GlobeScreen(CPU *c)
{
    F_801334E8_orig(c);
    if (!getenv("TM2PC_TEST_LOAD_HOLD") && (!M.live_input || getenv("TM2PC_UI_SCRIPT"))) return; /* (tests: as before) */
    uint32_t next = c->r[2];
    for (int i = 0; i < 120; i++) rt_vblank();
    c->r[2] = next;
}

/* ---- input ---- */
uint16_t input_player_bits(int p); /* runtime/input.c: a player's PS1 buttons (set = held) */
static int pad_as = -1; /* reading the actions of player 3 or 4 */
uint32_t killcam_input(int kind, int player, uint32_t live);
static uint32_t player_latch(int p) /* the game's latch layout (padDecode): the raw pad bytes swapped */
{
    uint16_t vp_weapon_buttons(int p, uint16_t bits);
    uint16_t b = (uint16_t)killcam_input(0, p, vp_weapon_buttons(p, input_player_bits(p)));
    static int test_read;
    static uint16_t test_bits;
    if (!test_read) { test_read = 1; if (getenv("TM2PC_TEST_P34")) test_bits = (uint16_t)strtoul(getenv("TM2PC_TEST_P34"), 0, 16); }
    b |= test_bits;
    return (uint32_t)(uint16_t)((b >> 8) | (b << 8));
}

void F_800DE5F8_orig(CPU *c);
void vp_HumanCarReadInput(CPU *c) /* humanCarReadInput(car) */
{
    int p = (int16_t)mem_r16(c, c->r[4]);
    if (!four_n || p < 2 || p > 3) { F_800DE5F8_orig(c); return; }
    pad_as = p;
    F_800DE5F8_orig(c);
    pad_as = -1;
}

/* Players 3-4's buttons per action: the game's table for port 0
 * (padActiveBindings[0], 128 bytes) as their layout and own buttons make it
 * (the layout apply 801206F4 with their buttons, then port 0's put back),
 * once a battle. */
#define PAD_ACTIVE GA(0x801A4BECu)       /* padActiveBindings[2][128] */
#define PAD_CONFIGURED GA(0x801A4CECu)   /* padConfiguredBindings[2][128] */
int bind_layout(int player);
void bind_apply_as(int player);
static uint8_t pad_table[4][128];
static int pad_table_made;
static void pad_tables(void)
{
    uint8_t act[128], conf[128];
    for (uint32_t i = 0; i < 128; i++) { act[i] = mem_r8(&M.cpu, PAD_ACTIVE + i); conf[i] = mem_r8(&M.cpu, PAD_CONFIGURED + i); }
    for (int p = 2; p < 4; p++) {
        bind_apply_as(p);
        guest_call(GA(0x801206F4u), (uint32_t)bind_layout(p), 0, 0, 0);
        bind_apply_as(-1);
        for (uint32_t i = 0; i < 128; i++) pad_table[p][i] = mem_r8(&M.cpu, PAD_ACTIVE + i);
    }
    for (uint32_t i = 0; i < 128; i++) { mem_w8(&M.cpu, PAD_ACTIVE + i, act[i]); mem_w8(&M.cpu, PAD_CONFIGURED + i, conf[i]); }
    pad_table_made = 1;
}

void F_80150CFC_orig(CPU *c);
void vp_PadCheckAction(CPU *c) /* padCheckAction(action, port) */
{
    if (pad_as < 0 || (int16_t)c->r[5] < 2) { F_80150CFC_orig(c); return; }
    if (!pad_table_made) pad_tables();
    uint32_t saved = mem_r32(c, PAD_LATCHED);
    uint8_t act[128];
    for (uint32_t i = 0; i < 128; i++) { act[i] = mem_r8(c, PAD_ACTIVE + i); mem_w8(c, PAD_ACTIVE + i, pad_table[pad_as][i]); }
    mem_w32(c, PAD_LATCHED, player_latch(pad_as)); /* as port 0, with this player's buttons */
    c->r[5] = 0;
    F_80150CFC_orig(c);
    mem_w32(c, PAD_LATCHED, saved);
    for (uint32_t i = 0; i < 128; i++) mem_w8(c, PAD_ACTIVE + i, act[i]);
}

/* ---- sound: players 3 and 4's loops in the shared list (as the computer cars'), instance 20 + side ---- */
void F_800D3244_orig(CPU *c);
void vp_SoundStartPlayerLoop(CPU *c) /* soundStartPlayerLoop(type, vol, pan, level, side) */
{
    int side = (int)mem_r32(c, c->r[29] + 16);
    extern int killcam_replaying;
    /* the kill cam's replay: a player's own turbo loops (0, and 32: its exhaust's flame, the burn / lava loop; full
     * volume, the player's feedback, wherever the camera is) not played: the replay started 32 every frame and it
     * looped on through every pass of the kill cam (the user's, 2026-11-06); the guns, the explosions and the other
     * cars' sounds play as they were. TM2PC_SOUND_TRACE logs the starts. */
    static int trace = -1;
    if (trace < 0) trace = getenv("TM2PC_SOUND_TRACE") != NULL;
    if (trace) fprintf(stderr, "sound %u: player loop %u side %d%s\n", (unsigned)M.vblanks, (unsigned)c->r[4], side,
                       killcam_replaying ? " (kill cam)" : "");
    if (killcam_replaying && (c->r[4] == 0 || c->r[4] == 32)) { c->r[2] = 0; return; }
    if (side < 2) { F_800D3244_orig(c); return; }
    c->r[2] = guest_call5(GA(0x800D32BCu), c->r[4], c->r[5], c->r[6], c->r[7], (uint32_t)(20 + side));
}
void F_800D328C_orig(CPU *c);
void vp_SoundStopPlayerLoop(CPU *c) /* soundStopPlayerLoop(type, side) */
{
    int side = (int)c->r[5];
    if (side < 2) { F_800D328C_orig(c); return; }
    c->r[2] = guest_call(GA(0x800D3304u), c->r[4], (uint32_t)(20 + side), 0, 0);
}

/* ---- views: one per player ----
 * 4 players: quarters. 3 players: player 1 a half (the top, or the left on
 * the arenas the game splits side by side), players 2-3 the other half's
 * quarters. The game runs as in a horizontal split (viewSplitScreenMode 2:
 * two views, their HUDs); the views are made again at the arena's setup with
 * the port's rows of the view-mode table (moved, room for 16: rows 9-12 the
 * quarters, decomp FOUR_PLAYERS.md 4.5: 160x120 with mode 1's angles, H 114).
 * Views 3-4 have their own frame records and order tables (high RAM) and are
 * drawn after view 2 in each frame (rtMainLoop's view-2 branch: ClearOTagR,
 * viewProc, the packet cursor carried on, effects, renderScene), shown after
 * HUD 2. The one packet buffer of a frame is 4 times as big (every frame
 * record's). A camera's chase offsets go by its view's shape (a quarter as
 * one view). */
#define VIEW_SPLIT GA(0x801809E4u)       /* viewSplitScreenMode */
#define VIEW0_FRAMES GA(0x8018118Cu)     /* renderView0Frames: view 1's pair, then view 2's, the mirror's (0x80 each) */
#define VIEW1_FRAMES GA(0x8018128Cu)
#define SLOT_VIEW0 GA(0x80180EE4u)       /* the frame record view 1 draws into */
#define SLOT_VIEW1 GA(0x80180EE8u)       /* renderBufferView1 */
#define PACKET_SIZE (4u * 0x17CF0u)  /* retail 0x17CF0 (these five: the same in the decomp overlay's port_hooks.h) */
#define PACKET_A 0x80D00000u         /* high RAM: over the car models' (from 0x80200000) */
#define PACKET_B 0x80D60000u
#define OT_BASE 0x80DC0000u          /* views 3-4: two order tables each (4096 entries) */
#define FRAMES_BASE 0x80DD0000u      /* views 2-4: a pair of frame records each (0x100 a view; slot 0, view 2, the C's) */
static int four_left;                /* 3 players: player 1 the left half (else the top) */
static int view_mode[4], view_shape[4]; /* each view's row; its chase offsets: 0 one view, 1 side by side, 2 top/bottom */
static uint32_t view_rec[4];         /* views 3-4: the frame record of this frame */

int port_config_on(void); /* vplus/port_config.c */
static void quarter_rows(void) /* rows 9-12: the quarters, from row 1 (one view) */
{
    static const int qx[4] = {0, 160, 0, 160}, qy[4] = {0, 0, 120, 120};
    for (int q = 0; q < 4; q++) {
        uint32_t row = RB_viewModeTable + 0x60u * (uint32_t)(9 + q), one = RB_viewModeTable + 0x60u;
        for (uint32_t k = 0; k < 0x60; k += 4) mem_w32(&M.cpu, row + k, mem_r32(&M.cpu, one + k));
        mem_w32(&M.cpu, row + 0x08, 114); /* H: mode 1's at half size */
        mem_w32(&M.cpu, row + 0x0C, 114);
        mem_w32(&M.cpu, row + 0x18, 160);
        mem_w32(&M.cpu, row + 0x1C, 120);
        mem_w32(&M.cpu, row + 0x20, (uint32_t)qx[q]);
        mem_w32(&M.cpu, row + 0x24, (uint32_t)qy[q]);
    }
}

void F_800D1A84_orig(CPU *c);
void vp_DoubleBufferInit(CPU *c) /* rtDoubleBufferInit: the frame records */
{
    F_800D1A84_orig(c);
    if (port_config_on()) return; /* the port's C does it (portViewBuffersInit, after this in shellInitMachine) */
    uint32_t v0 = c->r[2];
    for (uint32_t k = 0; k < 6; k++) { /* view 1, view 2, mirror: A then B */
        uint32_t rec = VIEW0_FRAMES + 0x80u * k, pk = (k & 1) ? PACKET_B : PACKET_A;
        mem_w32(c, rec + 4, pk);
        mem_w32(c, rec + 0xC, pk + PACKET_SIZE);
    }
    for (uint32_t v = 0; v < 2; v++) /* views 3-4: as view 2's, with their own order tables */
        for (uint32_t b = 0; b < 2; b++) {
            uint32_t rec = FRAMES_BASE + 0x100u * (v + 1) + 0x80u * b; /* (slot 0: the C's view 2, unused here) */
            for (uint32_t k = 0; k < 0x80; k += 4) mem_w32(c, rec + k, mem_r32(c, VIEW1_FRAMES + 0x80u * b + k));
            mem_w32(c, rec + 0, OT_BASE + 0x4000u * (2 * v + b));
        }
    if (RB_VIEW_ROWS > 12) quarter_rows();
    c->r[2] = v0;
}

static void four_radars(void);
static void four_views_create(void)
{
    if (four_vn() == 4) { view_mode[0] = 9; view_mode[1] = 10; view_mode[2] = 11; view_mode[3] = 12; }
    else if (four_left) { view_mode[0] = 5; view_mode[1] = 10; view_mode[2] = 12; }
    else { view_mode[0] = 2; view_mode[1] = 11; view_mode[2] = 12; }
    for (int k = 0; k < four_vn(); k++) {
        view_shape[k] = view_mode[k] == 5 ? 1 : view_mode[k] == 2 ? 2 : 0;
        uint32_t env = k == 0 ? VIEW0_FRAMES : k == 1 ? VIEW1_FRAMES : FRAMES_BASE + 0x100u * (uint32_t)(k - 1);
        guest_call(GA(0x80119CA0u), env, (uint32_t)view_mode[k], (uint32_t)k, 0); /* viewCreateWithCamera */
    }
}

int four_views(void) { return four_vn() && mem_r32(&M.cpu, VIEW_SPLIT) == 2 ? four_vn() : 0; }
int four_player1_left(void) { return four_left; }
/* a player's view in the screen's 640 x 480 (3-4 players' views): 0 if not a view of its own */
int four_view_box(int p, float *x, float *y, float *w, float *h)
{
    if (!four_views() || p < 0 || p >= four_vn()) return 0;
    int m = view_mode[p];
    if (m == 2) { *x = 0; *y = 0; *w = 640; *h = 240; }      /* player 1 on top (3 players) */
    else if (m == 5) { *x = 0; *y = 0; *w = 320; *h = 480; } /* player 1 left */
    else if (m >= 9 && m <= 12) { *x = (m - 9) & 1 ? 320.0f : 0; *y = m >= 11 ? 240.0f : 0; *w = 320; *h = 240; }
    else return 0;
    return 1;
}

void F_8012BD94_orig(CPU *c);
void wide_rows_apply(void); /* vplus/wide.c */
void vp_SessionBind(CPU *c) /* sessionBindCars(settings): the arena (and its views), humans, then opponents */
{
    wide_rows_apply(); /* widescreen: the view-mode rows before the views are made */
    vp_jugg_car_unbind(); /* (every round: each car its own; the Juggernaut's car parked after) */
    if (!four_n) { F_8012BD94_orig(c); vp_jugg_car_bound(); return; }
    /* the decomp's way (every mode): its binding makes every human and computer car */
    F_8012BD94_orig(c);
    if (mem_r32(c, VIEW_SPLIT) == 2) four_views_create();
    pad_table_made = 0; /* (players 3-4's buttons: made at their first read, each match) */
    for (int i = 2; i < four_n && i < 4; i++) /* (pad configurations: players 3-4's player 1's; every human reads its word) */
        mem_w32(c, RB_padConfigs + 4u * (uint32_t)i, mem_r32(c, RB_padConfigs));
    fprintf(stderr, "players: %d humans, %d views here\n", four_n, four_views());
    vp_jugg_car_bound();
}

int four_split_mode(int level) /* the session's split (vp_ViewSetSplitMode): 2; the side-by-side arenas noted */
{
    (void)level;
    four_left = settings_video.split != 1;
    port_config_sync();
    return 2;
}

void F_80156344_orig(CPU *c);
void vp_ViewSetFollowObj(CPU *c) /* viewSetFollowObj(sky, split): view 2's copy of the sky; views 3-4's too */
{
    uint32_t sky = c->r[4];
    F_80156344_orig(c);
    for (int k = 2; k < four_vn() && !port_config_on(); k++) { /* (with the port's C: viewSetFollowObj makes them) */
        uint32_t o = guest_call(GA(0x8014952Cu), 0, 0, 0, 0); /* csAlloc */
        mem_w32(c, o, 0xFFF);
        guest_call(GA(0x801495A8u), o, mem_r32(c, sky + 0x11C), 0, 0); /* csSetModel */
        mem_w32(c, RB_viewFollowObjects + 4u * (uint32_t)k, o);
    }
}

/* streaksDraw (800D8230) draws the weapons' streaks, arcs and flames of a view, and also runs two updates
 * (weaponDropsUpdate, weaponFlameUpdate): once per view in the game, so once with one player and twice in
 * split screen. Views 3-4 draw theirs, the updates staying the game's (the decomp overlay's streaksDraw does
 * the same: one rule for every player count). */
static int streaks_draw_only;
void F_80109D58_orig(CPU *c);
void vp_WeaponDropsUpdate(CPU *c) { if (!streaks_draw_only) F_80109D58_orig(c); }
void F_8010A7A0_orig(CPU *c);
void vp_WeaponFlameUpdate(CPU *c) { if (!streaks_draw_only) F_8010A7A0_orig(c); }
void F_80152284_orig(CPU *c);
void four_streaks_draw_only(int on) { streaks_draw_only = on; } /* vplus/interp.c's redraws */
void interp_capture(int view); /* vplus/interp.c */
void vp_RenderScene(CPU *c) /* renderScene(rec, view): after view 2's in the main loop, views 3-4 */
{
    uint32_t ra = c->r[31];
    int view = (int)c->r[5];
    if (view == 0 && ra >= GA(0x800D1318u) && ra < GA(0x800D1A08u)) interp_capture(0); /* a step's poses (view 1's) */
    else if (view >= 1) interp_capture(view); /* (the step's next view: after the world's step) */
    void wm_capture(int view); /* vplus/world_marks.c: the cars where this view draws them (the marks over it) */
    uint32_t rec0 = c->r[4];
    int mirror = rec0 == 0x80DF5000u || rec0 == GA(0x8018138Cu) || rec0 == GA(0x8018140Cu); /* (a rear-view mirror's: mirrors.c, the game's) */
    if (!mirror) wm_capture(view);
    if (view >= 2 && view < 4) view_rec[view] = c->r[4]; /* (for four_present: whoever draws view 3-4, the record) */
    void vp_shim_view(int view, int before); /* (fixes.c: the Juggernaut's own car, in its own view) */
    void vp_pickup_view(int view, int before); /* (fixes.c: Gun Game's pickups as this view's player's weapon) */
    vp_shim_view(view, 1);
    vp_pickup_view(view, 1);
    static int pkt_log = -1;
    if (pkt_log < 0) pkt_log = getenv("TM2PC_PKT_LOG") != NULL;
    uint32_t pkt_from = pkt_log ? mem_r32(c, rec0 + 8) : 0;
    /* Holland's rear-view mirror at the original's detail bands (no far LOD, near scale 1, no budget): at Far and
     * Farthest the whole grass field's finest beams were drawn again in the mirror, and the frames dropped (the user's,
     * 2026-11-04; measured at 1920x1080, uncapped, Farthest, in the field: VBlanks over 19 ms in 70 s, mirror off 35,
     * on 591, these bands 33; near scale 2: 226). The main view keeps its grass; the mirror's shows near the car.
     * Every other arena's mirror as its view (Los Angeles: 50 with the mirror, 33 without). TM2PC_MIRROR_FULL: tests */
    static int mirror_full = -1;
    if (mirror_full < 0) mirror_full = getenv("TM2PC_MIRROR_FULL") != NULL;
    uint32_t far_lod = 0, near_scale = 0, budget = 0;
    static uint32_t near_word = 1, budget_word = 1;
    if (near_word == 1) near_word = port_sym("portFarLodNearScale"), budget_word = port_sym("portFarLodBudget");
    int lod_off = mirror && !mirror_full && rc_port_far_lod && mem_r8(c, GA(0x8016475Cu) + 24) == 7; /* (7: Holland) */
    if (lod_off) {
        far_lod = mem_r32(c, rc_port_far_lod), mem_w32(c, rc_port_far_lod, 0);
        if (near_word) near_scale = mem_r32(c, near_word), mem_w32(c, near_word, 1);
        if (budget_word) budget = mem_r32(c, budget_word), mem_w32(c, budget_word, 0);
    }
    /* the mirror with no polygon cut by the near pass (portVisual bit 8, decomp RENDER_ANSWERS R10.1): its cut pieces'
     * whole-pixel corners beside the precise ones left cracks 360..560 behind the car, the clear color through them
     * (Amazonia's tunnel in red, the user's 2026-11-04). View 0 has that band under the car; Wobble None sets it for
     * every view (fixes.c) */
    extern int gte_precise_on; /* runtime/gte.c: 1 Smooth, 2 None */
    static uint32_t vis_word = 1;
    if (vis_word == 1) vis_word = port_sym("portVisual");
    uint32_t vis_was = vis_word ? mem_r32(c, vis_word) : 0;
    unsigned vp_visual_test_mask(void); /* vplus.c: tests */
    int whole_near = mirror && vis_word && gte_precise_on >= 1 && !(vis_was & 0x100u) && (vp_visual_test_mask() & 0x100u);
    if (whole_near) mem_w32(c, vis_word, vis_was | 0x100u);
    F_80152284_orig(c);
    if (whole_near) mem_w32(c, vis_word, mem_r32(c, vis_word) & ~0x100u);
    if (lod_off) {
        mem_w32(c, rc_port_far_lod, far_lod);
        if (near_word) mem_w32(c, near_word, near_scale);
        if (budget_word) mem_w32(c, budget_word, budget);
    }
    /* the C emitter's subpixel places of this view into the precise table now, the ring empty for the next view: drained
     * only as the frame was sent, the main view and the rear-view mirror together filled it (Holland's field at Farthest,
     * 12,288 beam corners a view, 16,380 entries: the rest fell back to whole pixels without depth, the dark rectangles
     * in the tall grass, worse the more grass was on screen; the user's 2026-11-03) */
    void port_precise_ring(void); /* vplus/port_config.c */
    port_precise_ring();
    if (pkt_log) /* tests: the packet space a view's world took, and what was left of its record's */
        fprintf(stderr, "pkt: %llu view %d rec %08X used %u left %d\n", (unsigned long long)M.vblanks, view, rec0,
                mem_r32(c, rec0 + 8) - pkt_from, (int)(mem_r32(c, rec0 + 0xC) - mem_r32(c, rec0 + 8)));
    vp_pickup_view(view, 0);
    vp_shim_view(view, 0);
    void wm_capture_gte(int view); /* (its projection as the GTE had it) */
    if (!mirror) wm_capture_gte(view);
    void hudstyle_world_marks(int view, uint32_t rec); /* (vplus/hud_styles.c: King of the Hill's box, in its world) */
    if (!mirror) hudstyle_world_marks(view, rec0);
    if (port_config_on()) return; /* the port's C draws views 2..N (portViewsDraw) */
    if (view != 1 || ra < GA(0x800D1318u) || ra >= GA(0x800D1A08u) || four_views() < 3) return;
    four_radars(); /* after the world's step, before the HUDs */
    uint32_t prev = mem_r32(c, SLOT_VIEW1), b = mem_r32(c, SLOT_VIEW0) == VIEW0_FRAMES ? 0 : 1;
    for (int k = 2; k < four_vn(); k++) { /* as the game's view 2 */
        uint32_t rec = FRAMES_BASE + 0x100u * (uint32_t)(k - 1) + 0x80u * b;
        guest_call(GA(0x80141938u), mem_r32(c, rec), 4096, 0, 0);  /* ClearOTagR */
        guest_call(GA(0x80155E80u), (uint32_t)k, 0, 0, 0);          /* viewProc */
        mem_w32(c, rec + 8, mem_r32(c, prev + 8));              /* the frame's packets go on */
        streaks_draw_only = 1;                                  /* streaksDraw: its drawing, not its updates */
        guest_call(GA(0x800D8230u), rec, (uint32_t)k, 0, 0);
        streaks_draw_only = 0;
        guest_call(GA(0x8014B484u), rec, 0, 0, 0);                  /* debrisDraw */
        guest_call(GA(0x80110BE4u), rec, (uint32_t)k, 0, 0);        /* fxSpritesDraw */
        guest_call(GA(0x80152284u), rec, (uint32_t)k, 0, 0);        /* renderScene */
        prev = rec;
    }
}

uint32_t four_view_rec(int k) { return k >= 2 && k < 4 && four_views() > k ? view_rec[k] : 0; }
void mirrors_present(void); /* mirrors.c */
static void four_present_views(void);
void four_present(void) /* after HUD 2: views 3-4 and their HUDs on the screen, then the players' mirrors */
{
    four_present_views();
    mirrors_present();
}
/* After view who's HUD (the HUD wraps, fixes.c). The play build: after HUD 2, views 3-4 and their HUDs, then
 * the mirrors. With the port's C (portViewsPresent shows every view and its HUD): the mirrors after the last
 * view's HUD. */
void four_hud_done(int who)
{
    if (!port_config_on()) {
        if (who == 1) four_present();
        return;
    }
    if (who == (four_views() ? four_vn() - 1 : 1)) mirrors_present();
}
static void four_present_views(void)
{
    if (four_views() < 3) return;
    uint32_t first = mem_r32(&M.cpu, SLOT_VIEW0) == VIEW0_FRAMES;
    for (int k = 2; k < four_vn(); k++) {
        if (!view_rec[k]) continue;
        guest_call(GA(0x80141AA4u), view_rec[k] + 0x10, 0, 0, 0);                        /* PutDrawEnv */
        guest_call(GA(0x80141A30u), mem_r32(&M.cpu, view_rec[k]) + 4095u * 4, 0, 0, 0); /* DrawOTag */
        guest_call(GA(0x800F9D48u), (uint32_t)k, first, 0, 0);                           /* hudDrawSplitHorizontal */
    }
}

/* (A player out watches as every player does, fixes.c's watching: their wreck for the game's delay first. The copy of
 * another player's camera views 3-4 had took over at once, before it: the user's 2026-11-15 take, players 3 and 4
 * never saw their wreck.) */
int vp_player_out(int p); /* vplus/fixes.c */
int humans_of_slot(int L); /* vplus/humans.c: view -> human */

void F_80102BE8_orig(CPU *c);
static void camera_update(CPU *c);
void vp_watch_view(int i, int enter); /* fixes.c: the game's watched car, player i's while their camera runs */
void vp_CarCameraUpdate(CPU *c) /* viewCarCameraUpdate(car, i): the chase offsets of its view's shape */
{
    int i = (int)c->r[5];
    vp_watch_view(i, 1);
    camera_update(c);
    vp_watch_view(i, 0);
}
static void camera_update(CPU *c)
{
    int i = (int)c->r[5];
    if (port_config_on()) { /* the port's C does the guard and the shapes (decomp/ overlay): the watching here */
        F_80102BE8_orig(c);
        return;
    }
    if (i >= 2 && !mem_r32(c, RB_viewCameraObjects + 4u * (uint32_t)(i & 3))) return; /* no view (viewSetRotation has no guard) */
    if (!four_views() || i < 0 || i > 3) { F_80102BE8_orig(c); return; }
    uint32_t saved = mem_r32(c, VIEW_SPLIT);
    mem_w32(c, VIEW_SPLIT, (uint32_t)view_shape[i]);
    F_80102BE8_orig(c);
    mem_w32(c, VIEW_SPLIT, saved);
}

void F_8011C530_orig(CPU *c);
void mirrors_toggle(int p); /* mirrors.c */
void vp_CarViewHotkeys(CPU *c) /* carViewHotkeys(car): its split toggle with 3 players flips player 1's half (top or left); none with 4 */
{
    uint32_t car = c->r[4];
    uint32_t before = mem_r32(c, VIEW_SPLIT);
    uint32_t radar = mem_r32(c, GA(0x80180A6Cu)), list = mem_r32(c, GA(0x80180A70u)); /* hudRadarVisible, hudWeaponListVisible */
    if (getenv("TM2PC_HOTKEY_DEBUG"))
        for (uint32_t k = 0xD4; k <= 0xE6; k++)
            if (mem_r8(c, car + k)) fprintf(stderr, "hotkeys %llu: player %u key +0x%X\n", (unsigned long long)M.vblanks, (car - RB_humanCars) / HUMAN_REC_SIZE, k);
    int humans_slot(int h); /* vplus/humans.c: the human's player on this machine (-1: another machine's) */
    int who = humans_slot((int)((car - RB_humanCars) / HUMAN_REC_SIZE)); /* (the keys: this machine's player's screen) */
    static uint8_t rear_held[4];
    /* (online: a step run again (rollback) replays the press: only the live step's counts. Each replay toggled again,
     * and its rtToggleRearView picked the mirror's frame record against a restored flag: the mirror's order table
     * drawn into the frame on screen, garbage and a freeze (Terror, Sage, 2026-11-22). The mirror is this machine's
     * own: net/rb_state.c keeps it across a batch, mirrors_rear_pair its record) */
    extern int rbs_resimulating;
    int again = rbs_resimulating;
    if (before != 0 && who >= 0 && who < 4) { /* split screen (the game has no mirror there): the rear-view key (+0xD9), each player's own */
        uint8_t held = mem_r8(c, car + 0xD9);
        if (!again && held && !rear_held[who] && !mem_r8(c, car + 0xA7) && !mem_r8(c, car + 0x9C)) mirrors_toggle(who);
        if (!again) rear_held[who] = held;
    } else if (before == 0 && who == 0) { /* one view: the key goes Full, Small, Off (the game's own only on and off) */
        uint8_t held = mem_r8(c, car + 0xD9);
        if (again) held = rear_held[0]; /* (a re-run: nothing new) */
        if (held && !rear_held[0] && !mem_r8(c, car + 0xA7) && !mem_r8(c, car + 0x9C)) {
            int mirror_mode(int p);
            mirrors_toggle(0);
            int on = mem_r32(c, GA(0x801809DCu)) != 0, want = mirror_mode(0) != 0; /* rtRearViewActive */
            if (on != want) guest_call(GA(0x800D1BB8u), 0, 0, 0, 0); /* rtToggleRearView */
        }
        rear_held[0] = held;
    }
    /* the game's own toggle (one view): not again (the port's above), never in a re-run. The key byte only hidden from
     * it and put back after: the byte is state, the same on every machine (each derives it from the inputs; carViewHotkeys
     * runs for this machine's humans only). Cleared for good, the presser's machine parted from the others at the next
     * hash (Terror, Sage: "Connection trouble" after the mirror) */
    int rear_key = before == 0 && who == 0 ? mem_r8(c, car + 0xD9) : 0;
    if (rear_key) mem_w8(c, car + 0xD9, 0);
    F_8011C530_orig(c);
    if (rear_key) mem_w8(c, car + 0xD9, (uint8_t)rear_key);
    if (getenv("TM2PC_HOTKEY_DEBUG") && (mem_r32(c, GA(0x80180A6Cu)) != radar || mem_r32(c, GA(0x80180A70u)) != list))
        fprintf(stderr, "hotkeys %llu: radar %08x -> %08x, list %08x -> %08x\n", (unsigned long long)M.vblanks, radar, mem_r32(c, GA(0x80180A6Cu)), list, mem_r32(c, GA(0x80180A70u)));
    if (!hud_of(who >= 0 && who < 4 ? who : 0)->buttons) { /* HUD buttons in battle off: the HUD menu's settings stay */
        mem_w32(c, GA(0x80180A6Cu), radar);
        mem_w32(c, GA(0x80180A70u), list);
        /* except: Select + Left turns the radar on and off (the game's radar key, +0xDA); Select + Down the next HUD
         * style (TM2 Icon, TM2 List, TM:Black, TM:2012; the weapon list key, +0xDB), kept */
        static uint8_t radar_held[4], list_held[4];
        if (who >= 0 && who < 4 && !mem_r8(c, car + 0xA7) && !mem_r8(c, car + 0x9C)) {
            uint8_t rk = mem_r8(c, car + 0xDA), lk = mem_r8(c, car + 0xDB);
            if (rk && !radar_held[who])
                mem_w8(c, GA(0x80180A6Cu) + (uint32_t)who, (uint8_t)!mem_r8(c, GA(0x80180A6Cu) + (uint32_t)who)); /* hudRadarVisible */
            if (lk && !list_held[who]) {
                HudSettings *h = &hud_players[who];
                int i = (h->style % 3 ? h->style % 3 + 1 : h->full_weapons != 0) + 1;
                h->style = i % 4 < 2 ? 0 : i % 4 - 1;
                if (i % 4 < 2) h->full_weapons = i % 4;
                mem_w8(c, GA(0x80180A70u) + (uint32_t)who, (uint8_t)(i % 4 == 1)); /* hudWeaponListVisible */
                if (profile_hud_owner(who)[0]) profile_hud_save(who);
                fprintf(stderr, "hud: player %d style %d\n", who + 1, i % 4);
            }
            radar_held[who] = rk;
            list_held[who] = lk;
        }
    }
    if (!four_vn() || mem_r32(c, VIEW_SPLIT) == before) return;
    guest_call(GA(0x800D1C24u), before, 0, 0, 0); /* viewSetSplitMode back */
    if (four_vn() == 3) {
        four_left = !four_left;
        settings_video.split = four_left ? 2 : 1; /* (kept as the setting) */
        settings_save_section("video");
        port_config_sync();
        fprintf(stderr, "four players: player 1 %s\n", four_left ? "left" : "top");
    }
    four_views_create();
    guest_call(GA(0x800FB0B0u), 0, 0, 0, 0);      /* hudInitPanels */
}

/* ---- HUDs: one per player ----
 * Each player's HUD is the horizontal split's (hudDrawSplitHorizontal, its
 * record and radar; records 3-4 copies of record 2 as the game sets it up:
 * the split HUDs use the same numbers), laid out for its view by
 * hud_layout.c. */
#define HUD_RECORD 1900u
#define HUD_RADAR rc_hud_radar_bytes /* (0x70 + 0x40 a blip: 0x370 retail, the 16-car variant's 15 blips 0x430) */
int four_hud_narrow(int player); /* a player's view is 160 wide */
int four_hud_narrow(int player)
{
    return four_views() && player >= 0 && player < four_vn() && view_shape[player] != 2;
}
void four_view_size(int player, int *w, int *h) /* a player's view, in the game's units */
{
    int m = player >= 0 && player < 4 ? view_mode[player] : 2;
    *w = m == 2 ? 320 : 160;
    *h = m == 5 ? 240 : 120;
}
int four_divider_kind(void) /* hud_layout.c's thin dividers: 4 quarters, 3 player 1 on top, 5 player 1 left */
{
    return four_views() == 4 ? 4 : four_views() == 3 ? (four_left ? 5 : 3) : 0;
}

static void hud_copies(void) /* records and radars 3-4: record 2's, as the game set it up */
{
    for (int k = 2; k < four_vn(); k++) {
        for (uint32_t i = 0; i < HUD_RECORD; i += 4)
            mem_w32(&M.cpu, RB_hudPlayerRecords + HUD_RECORD * (uint32_t)k + i, mem_r32(&M.cpu, RB_hudPlayerRecords + HUD_RECORD + i));
        for (uint32_t i = 0; i < HUD_RADAR; i += 4)
            mem_w32(&M.cpu, RB_hudRadars + HUD_RADAR * (uint32_t)k + i, mem_r32(&M.cpu, RB_hudRadars + HUD_RADAR + i));
        mem_w8(&M.cpu, GA(0x80180A70u) + (uint32_t)k, mem_r8(&M.cpu, GA(0x80180A71u))); /* the weapon list: as player 2's */
    }
}
/* hudRadarArrowsLoad(id, src): the radar's edge arrows, parsed for HUDs 1-2 into each direction's sprites;
 * HUDs 3-4 get the same (moved arrays, rebase_arrays.tsv; the decomp's variant parses all four itself) */
void F_800FD00C_orig(CPU *c);
void vp_HudRadarArrowsLoad(CPU *c)
{
    F_800FD00C_orig(c);
    if (rc_addrmap_count) return;
    const uint32_t arrays[4] = {RB_hudRadarArrowUpSprites, RB_hudRadarArrowDownSprites, RB_hudRadarArrowLeftSprites,
                                RB_hudRadarArrowRightSprites};
    for (int a = 0; a < 4; a++)
        for (uint32_t k = 2; k < 4; k++)
            for (uint32_t i = 0; i < 8; i += 4) mem_w32(&M.cpu, arrays[a] + 8u * k + i, mem_r32(&M.cpu, arrays[a] + i));
}
void F_800FB0B0_orig(CPU *c);
void vp_HudInitPanels(CPU *c) { F_800FB0B0_orig(c); if (four_vn()) hud_copies(); }
void F_800FAA0C_orig(CPU *c);
void vp_HudInitIcons(CPU *c) { F_800FAA0C_orig(c); if (four_vn()) hud_copies(); }
void F_800FC5C4_orig(CPU *c);
void vp_HudRadarInit(CPU *c) { F_800FC5C4_orig(c); if (four_vn()) hud_copies(); }

/* The radars with 3-4 players: the game plots player 2 on radar 1, player
 * 1 on radar 2 and the computer cars on radar 1 (and on radar 2 in the
 * challenge mode, the port's: vp_RadarPlotCar). Here the rest, once a frame
 * after the world's step: players 3-4 on every other player's radar, and on
 * radars 3-4 everyone else (no rand(): the game's stream stays as it is).
 * A quarter's radar starts hidden (it would cover half the view; the
 * radar key shows it). */
/* Players 3-4's radars (and the pairs with them): every player on every other player's radar, every computer
 * car on every player's radar but player 1's (the game's), and player 2's in co-op (the game's too). The game's
 * rule for players 1-2: a car within 400 of the radar owner's height on every frame, a higher or lower one on 1
 * frame in 5 (rand() % 5 == 0, the game's random numbers: a faint dot). In the hybrid the game's C does this
 * (src/port/port_radar.c in decomp/). Tests: TM2PC_TEST_SOLID_RADAR, every dot every frame in both builds. */
static int radar_solid(void)
{
    static int solid = -1;
    if (solid < 0) solid = getenv("TM2PC_TEST_SOLID_RADAR") != NULL;
    return solid;
}
static void plot(int type, uint32_t from_rec, uint32_t pos, int radar)
{
    const uint32_t d = 0x80DD1200u + 16u * (uint32_t)radar;
    int32_t dz = (int32_t)mem_r32(&M.cpu, pos + 8) - (int32_t)mem_r32(&M.cpu, from_rec + 0x6FC + 8);
    if ((dz < 0 ? -dz : dz) >= 400 && !radar_solid() && guest_call(GA(0x80138888u), 0, 0, 0, 0) % 5 != 0) return; /* rand */
    for (uint32_t a = 0; a < 3; a++)
        mem_w32(&M.cpu, d + 4 * a, mem_r32(&M.cpu, pos + 4 * a) - mem_r32(&M.cpu, from_rec + 0x6FC + 4 * a));
    guest_call(GA(0x800FC928u), (uint32_t)type, d, mem_r32(&M.cpu, from_rec + 0x710), (uint32_t)radar); /* hudRadarPlotCar */
}
static void four_radars(void)
{
    if (port_config_on()) return; /* the game's C does it (worldUpdate: portRadarPlot) */
    const uint32_t ai = GA(0x80188B5Cu);
    int n = (int)mem_r32(&M.cpu, GA(0x8018101Cu)), coop = mem_r32(&M.cpu, GA(0x80180FC4u)) == 2; /* gameMode */
    for (int j = 0; j < four_n; j++) { /* radar j */
        uint32_t rj = HUMAN_REC(j);
        for (int i = 0; i < four_n; i++) {
            if (i == j || (i < 2 && j < 2)) continue; /* 1 and 2: the game's */
            uint32_t ri = HUMAN_REC(i);
            if ((int32_t)mem_r32(&M.cpu, ri + 0x1F0) > 0) plot((int)mem_r32(&M.cpu, ri + 0x90), rj, ri + 0x6FC, j);
        }
        if (j == 0 || (j == 1 && coop)) continue; /* the computer cars on these: the game's */
        for (int k = 0; k < n && k < cars_ai_max(); k++) {
            uint32_t rk = ai + AI_REC_SIZE * (uint32_t)k;
            if ((int32_t)mem_r32(&M.cpu, rk + 0x5A0) > 0) plot((int)mem_r32(&M.cpu, rk + 0x30), rj, rk + 0x73C, j);
        }
    }
}
