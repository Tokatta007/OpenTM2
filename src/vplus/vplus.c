/* Vanilla Plus registry and its settings (see vplus.h).
 *
 * Its section of tm2pc.ini (read by settings.c):
 *
 *   [vanilla_plus]
 *   preset = vanilla_plus        ; or original: every change off
 *   pause_quit_boom = on         ; overrides the preset for one change
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vplus.h"
#include "cars.h"

const char *const vp_category_names[VP_CAT_COUNT] = {
    [VP_CAT_BALANCING] = "Global Balancing",
    [VP_CAT_CHARACTERS] = "Character Balancing",
    [VP_CAT_BUG_FIX] = "Bug fixes",
    [VP_CAT_MENUS] = "Menus",
    [VP_CAT_CAR_SELECT] = "Car Select",
    [VP_CAT_BATTLE] = "Battle",
};

/* the user's order (2026-10-28): like options side by side; the characters as car select has them (Roadkill first) */
const VpChangeId vp_menu_order[VP_COUNT] = {
    VP_SECRET_CARS_LAST, VP_HIDE_LOCKED_DRIVERS, VP_CARSEL_PORTRAITS, VP_CARINFO_STATS, VP_CARSEL_SOUNDS,
    VP_CARSEL_MULTI, VP_CARSEL_BORDERS, VP_DUPLICATE_CARS,
    VP_NO_MENU_TIMEOUT, VP_PASSWORDS_OFF, VP_PHOTO_REWIND,
    VP_KILL_MESSAGE, VP_KILL_STREAKS, VP_FINAL_KILL_CAM, VP_SANTA,
    VP_PAUSE_QUIT_BOOM, VP_AXEL_EYES, VP_WARTHOG_TRIM, VP_LA_QUAKE_FINALE, VP_TASER_LOW, VP_CAMERA_WALLS,
    VP_STEADY_SHADOW, VP_GLACIER_FIRE,
    VP_LIVES_PER_LEVEL, VP_COOP_SPECTATE, VP_NO_ROUND_RESTARTS, VP_SPAWN_SPACING, VP_AI_UNJAM, VP_BAL_PICKUPS, VP_BAL_MISSILES,
    VP_BAL_GUNS, VP_BAL_GUN_RANGE, VP_BAL_SHIELD, VP_BAL_MINES, VP_BAL_FREEZE, VP_BAL_RICOCHET,
    VP_BAL_ROADKILL, VP_BAL_TWISTER, VP_BAL_AXEL, VP_BAL_SHADOW, VP_BAL_HAMMERHEAD, VP_BAL_OUTLAW2, VP_BAL_GRIMM, VP_BAL_GRASSHOPPER,
    VP_BAL_THUMPER, VP_BAL_SLAM, VP_BAL_MINION,
};

static const char *const carsel_multi_values[3] = {"original", "split", "shared"};
static const char *const kill_cam_values[4] = {"off", "1", "2", "3"}; /* (the final kill cam's shots; an older "on": 3) */

const VpChange vp_changes[VP_COUNT] = {
    [VP_PAUSE_QUIT_BOOM] = { "pause_quit_boom", "Pause menu boom", VP_CAT_BUG_FIX,
        "Resume and Quit in the pause menu get the same deep explosion as the other menus." },
    [VP_AXEL_EYES] = { "axel_eyes", "Axel's eyes", VP_CAT_BUG_FIX,
        "Axel's eyes are white with a brown center instead of solid red." },
    [VP_WARTHOG_TRIM] = { "warthog_trim", "Warthog's side trim", VP_CAT_BUG_FIX,
        "The gray strip above Warthog's side windows is painted like the rest of his body, as the PC version fixed it." },
    [VP_LA_QUAKE_FINALE] = { "la_quake_finale", "Full quake finale", VP_CAT_BUG_FIX,
        "Every Los Angeles quake ends with its full burst of explosions, not only the early ones." },
    [VP_SECRET_CARS_LAST] = { "secret_cars_last", "Secret cars last", VP_CAT_CAR_SELECT,
        "Secret characters come last in car select." },
    [VP_CARSEL_PORTRAITS] = { "carsel_portraits", "Car select portrait bar", VP_CAT_CAR_SELECT,
        "New list of drivers' portraits on the car select menu." },
    [VP_CARSEL_MULTI] = { "carsel_multi", "New multiplayer car select", VP_CAT_CAR_SELECT,
        "Original: cars picked one at a time. Split: splitscreen car select. Shared: shared screen car select.",
        carsel_multi_values, 3, 1 },
    [VP_DUPLICATE_CARS] = { "duplicate_cars", "Allow duplicate cars", VP_CAT_CAR_SELECT,
        "Players may pick the same vehicle." },
    [VP_CARSEL_SOUNDS] = { "carsel_sounds", "Car select special sounds", VP_CAT_CAR_SELECT,
        "Highlighting a driver in the car select menu plays their special weapon sound, like in Twisted Metal 1." },
    [VP_KILL_MESSAGE] = { "kill_message", "Kill message", VP_CAT_BATTLE,
        "Destroying a car shows YOU KILLED and its driver, in every battle." },
    [VP_KILL_STREAKS] = { "kill_streaks", "Kill streaks", VP_CAT_BATTLE,
        "Quick kills and kills without dying are announced: Double Kill, Multi Kill... Killing Spree, Rampage..." },
    [VP_NO_MENU_TIMEOUT] = { "no_menu_timeout", "No menu timeout", VP_CAT_MENUS,
        "Menus wait for you instead of going back to the title after a while." },
    [VP_LIVES_PER_LEVEL] = { "lives_per_level", "Tournament lives per level", VP_CAT_BALANCING,
        "2 extra lives at the start of every level (3 players: 1, 4 players: 0).\nOff: 3 lives for the whole run." },
    [VP_NO_ROUND_RESTARTS] = { "no_round_restarts", "No round restart on death", VP_CAT_BALANCING,
        "A player losing a life now respawns, instead of resetting the match." },
    [VP_HIDE_LOCKED_DRIVERS] = { "hide_locked_drivers", "Hide locked drivers", VP_CAT_CAR_SELECT,
        "The secret drivers stay out of the car select portraits until they are unlocked." },
    [VP_CARINFO_STATS] = { "carinfo_stats", "New car info stats", VP_CAT_CAR_SELECT,
        "Car Info screens now correctly show each car's stats, based on the game code's actual measurements (armor, special, speed)." },
    [VP_SANTA] = { "santa", "Antarctica Santa", VP_CAT_BATTLE,
        "Santa from the PC version stands in Antarctica as a pedestrian." },
    [VP_FINAL_KILL_CAM] = { "final_kill_cam", "Final kill cam", VP_CAT_BATTLE,
        "A battle's last kill replayed before the results: 3, 2 or 1 shots from different cameras.", kill_cam_values, 4, 3 },
    [VP_SPAWN_SPACING] = { "spawn_spacing", "Spread-out spawns", VP_CAT_BALANCING,
        "A car never starts or respawns on top of another: it starts at the clearest spot instead." },
    [VP_COOP_SPECTATE] = { "coop_spectate", "Modern tournament lives", VP_CAT_BALANCING,
        "Game over only when every player is out of lives; those out watch until the rest win or lose.\nOff: one death with no lives left is game over." },
    [VP_PHOTO_REWIND] = { "photo_rewind", "Photo mode rewind", VP_CAT_MENUS,
        "Photo mode can go back up to 30 seconds (L2 and R2, X plays from there). Uses about 150 MB.",
        NULL, 0, 0, 1 },
    [VP_BAL_MISSILES] = { "bal_missiles", "Missile damage swap", VP_CAT_BALANCING,
        "Fire missiles do 10 damage and homing missiles 7, as in the later games (originally 7 and 10)." },
    [VP_BAL_SHIELD] = { "bal_shield", "No wasted shield", VP_CAT_BALANCING,
        "Casting the shield while one is already up is refused and costs nothing (originally it took the energy)." },
    [VP_BAL_MINES] = { "bal_mines", "Permanent mines", VP_CAT_BALANCING,
        "Mines stay until something drives over them. Each driver keeps at most 5: a 6th removes their oldest." },
    [VP_BAL_FREEZE] = { "bal_freeze", "Freeze diminishing returns", VP_CAT_BALANCING,
        "A car frozen again while frozen, or within a second of thawing, is frozen half as long as the last time." },
    [VP_BAL_RICOCHET] = { "bal_ricochet", "Safe ricochet launch", VP_CAT_BALANCING,
        "A ricochet bomb fired while driving fast no longer hits its own car before it gets clear." },
    [VP_BAL_GUNS] = { "bal_guns", "No chance machine guns", VP_CAT_BALANCING,
        "Every machine-gun bullet that connects does damage; the original flipped a coin per bullet. The same damage on average." },
    [VP_BAL_HAMMERHEAD] = { "bal_hammerhead", "Hammerhead balancing", VP_CAT_CHARACTERS,
        "Rams: 100% to 120% damage. Crush: 15 to 20 damage. Recharge: 60 to 35 seconds. Brakes: 1 to 2. Grip speed: 25 to 60." },
    [VP_BAL_AXEL] = { "bal_axel", "Axel balancing", VP_CAT_CHARACTERS,
        "No longer an automatic 15 damage on each ram (Hammerhead's special for free). Instead, his rams do 10% more damage." },
    [VP_BAL_MINION] = { "bal_minion", "Minion balancing", VP_CAT_CHARACTERS,
        "No automatic 15 damage on each ram (Hammerhead's special for free). Minion's rams do 10% more damage. Recharge 18 to 30 seconds." },
    [VP_BAL_TWISTER] = { "bal_twister", "Twister balancing", VP_CAT_CHARACTERS,
        "No weapons while her tornado spins, no ram damage to her during it, recharge 18 to 30 seconds, the last hit throws." },
    [VP_BAL_GRASSHOPPER] = { "bal_grasshopper", "Grasshopper balancing", VP_CAT_CHARACTERS,
        "Damage 15 to 20. Her special pushes the car it hits. Takes no ram damage while using her special. Lands cleanly. Jumps cost no energy." },
    [VP_BAL_OUTLAW2] = { "bal_outlaw2", "Outlaw 2 balancing", VP_CAT_CHARACTERS,
        "A taser shot that missed can still connect while it lasts (one hit of 10). Recharge 55 to 45 seconds." },
    [VP_BAL_GRIMM] = { "bal_grimm", "Mr. Grimm balancing", VP_CAT_CHARACTERS,
        "Special recharges in 60 seconds instead of 70." },
    [VP_BAL_THUMPER] = { "bal_thumper", "Thumper balancing", VP_CAT_CHARACTERS,
        "Special recharges in 75 seconds instead of 80. No second special while his flame still burns (it cost energy for nothing)." },
    [VP_BAL_SHADOW] = { "bal_shadow", "Shadow balancing", VP_CAT_CHARACTERS,
        "Fixes Shadow's special doing extra damage on slopes. Damage 25 to 28, the same everywhere." },
    [VP_TASER_LOW] = { "taser_low", "Taser Visual Fix", VP_CAT_BUG_FIX,
        "Outlaw 2's taser starts lower, from her light bar, so it connects with her car." },
    [VP_CAMERA_WALLS] = { "camera_walls", "Camera inside the arena", VP_CAT_BUG_FIX,
        "The camera behind your car stops at walls and keeps off them, instead of going through them.",
        NULL, 0, 0, 1 /* (off in Vanilla Plus: the user's 2026-11-03, the camera through walls is fine) */ },
    [VP_BAL_PICKUPS] = { "bal_pickups", "No wasted pickup respawns", VP_CAT_BALANCING,
        "Every 10 seconds a pickup that was taken comes back, still at random. The original sometimes picked a powerup still there and brought nothing." },
    [VP_CARSEL_BORDERS] = { "carsel_borders", "Color borders in car select", VP_CAT_CAR_SELECT,
        "With 2-4 players, each big portrait gets a border and a P1-P4 tag in the player's color." },
    [VP_BAL_ROADKILL] = { "bal_roadkill", "Roadkill balancing", VP_CAT_CHARACTERS,
        "His boomerang always gives its charge back when it returns to his car. The original sometimes let it vanish next to him." },
    [VP_BAL_GUN_RANGE] = { "bal_gun_range", "Machine gun range", VP_CAT_BALANCING,
        "Machine-gun bullets fly 25% farther before they fade, as in the PC version." },
    [VP_BAL_SLAM] = { "bal_slam", "Mr. Slam balancing", VP_CAT_CHARACTERS,
        "No pickups or special while his arm swings. His shield and machine guns still work." },
    [VP_AI_UNJAM] = { "ai_unjam", "Computer cars unjam", VP_CAT_BALANCING,
        "Computer cars stuck against other cars back out and take another way, instead of piling up. On levels with "
        "broken route data (Roof Tops), they follow the level's routes instead of driving into roof edges." },
    [VP_STEADY_SHADOW] = { "steady_shadow", "Steady car shadows", VP_CAT_BUG_FIX,
        "Car shadows follow the car's tilt without rocking with its suspension, so they no longer grow and shrink." },
    [VP_GLACIER_FIRE] = { "glacier_fire", "Antarctica's fire missile", VP_CAT_BUG_FIX,
        "The fire missile pickup on Antarctica's ice floes can be picked up. The original placed it inside an "
        "invisible box that kept every car from collecting it. Always on online." },
    [VP_PASSWORDS_OFF] = { "passwords_off", "Remove password menu", VP_CAT_MENUS,
        "Saving and progression has made passwords obsolete. Feel free to turn it back on for nostalgia." },
};

static unsigned char enabled[VP_COUNT];
static int preset_vanilla_plus = 1;
static int initialized;

static void init(void)
{
    if (!initialized) {
        initialized = 1;
        vp_preset(1);
    }
}

/* A change taken out of the game (its code kept, to try again later): never on, not in the options or the ini, an old
 * ini's line ignored. Camera inside the arena (requests 63, 96, 100): the user's 2026-11-03, the camera jumped in tight
 * spaces; the camera through walls is fine. */
int vp_retired(int id) { return id == VP_CAMERA_WALLS; }

int ui_net_kill_cam(void); /* ui/options.c: online, the host's final kill cam (-1: this machine's own) */
int vp_on(VpChangeId id)
{
    if (vp_retired(id)) return 0;
    init();
    if (id == VP_FINAL_KILL_CAM && ui_net_kill_cam() >= 0) return ui_net_kill_cam() > 0;
    /* Duplicate cars: only with the decomp's car groups in the build (request 32 step 1: every car its own group
     * and key). Without them two cars of one type share one model's per-type tables (decomp SURVIVAL.md 2): one
     * of them is not drawn, not hit and not solid. */
    return id < VP_COUNT && enabled[id] && (id != VP_DUPLICATE_CARS || cars_duplicates());
}

int vp_value(VpChangeId id)
{
    init();
    if (id == VP_FINAL_KILL_CAM && ui_net_kill_cam() >= 0) return ui_net_kill_cam();
    return id < VP_COUNT ? enabled[id] : 0;
}

void vp_set(VpChangeId id, int on)
{
    init();
    if (id < VP_COUNT)
        enabled[id] = (unsigned char)(vp_changes[id].values ? on : on != 0);
}

void vp_preset(int vanilla_plus)
{
    initialized = 1;
    preset_vanilla_plus = vanilla_plus;
    memset(enabled, vanilla_plus ? 1 : 0, sizeof enabled);
    for (int i = 0; i < VP_COUNT && vanilla_plus; i++)
        if (vp_changes[i].values) enabled[i] = (unsigned char)vp_changes[i].plus_value;
        else if (vp_changes[i].plus_off) enabled[i] = 0;
}


/* ---- [vanilla_plus] section of the settings file (settings.c) ---- */

static int overrides[VP_COUNT]; /* the preset applies first, wherever it appears */

static int parse_bool(const char *v, int *out)
{
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) { *out = 1; return 1; }
    if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) { *out = 0; return 1; }
    return 0;
}

void vp_ini_begin(void)
{
    init();
    for (int i = 0; i < VP_COUNT; i++) overrides[i] = -1;
}

void vp_ini_key(const char *key, const char *val)
{
    int b;
    if (!strcmp(key, "preset")) {
        if (!strcmp(val, "original")) vp_preset(0);
        else if (!strcmp(val, "vanilla_plus")) vp_preset(1);
        return;
    }
    for (int i = 0; i < VP_COUNT; i++) {
        if (strcmp(key, vp_changes[i].key)) continue;
        if (vp_retired(i)) return; /* (an old ini's line) */
        if (vp_changes[i].values) {
            int found = 0;
            for (int v = 0; v < vp_changes[i].value_count; v++)
                if (!strcmp(val, vp_changes[i].values[v])) { overrides[i] = v; found = 1; }
            if (!found && parse_bool(val, &b)) overrides[i] = b ? vp_changes[i].plus_value : 0; /* from an older on/off */
        } else if (parse_bool(val, &b)) {
            overrides[i] = b;
        }
    }
}

void vp_ini_end(void)
{
    for (int i = 0; i < VP_COUNT; i++)
        if (overrides[i] >= 0)
            enabled[i] = (unsigned char)overrides[i];
}

void vp_ini_write(FILE *f)
{
    init();
    fprintf(f, "[vanilla_plus]\n"
               "; vanilla_plus: all changes on; original: all off. Lines below override single changes.\n"
               "preset = %s\n", preset_vanilla_plus ? "vanilla_plus" : "original");
    for (int cat = 0; cat < VP_CAT_COUNT; cat++)
        for (int k = 0; k < VP_COUNT; k++) {
            int i = vp_menu_order[k];
            if (vp_changes[i].category == cat && !vp_retired(i)) {
                fprintf(f, "\n; %s: %s\n; ", vp_category_names[cat], vp_changes[i].name);
                for (const char *d = vp_changes[i].description; *d; d++) /* a line break: another comment line */
                    if (*d == '\n') fputs("\n; ", f);
                    else fputc(*d, f);
                fprintf(f, "\n%s = %s\n", vp_changes[i].key,
                        vp_changes[i].values ? vp_changes[i].values[enabled[i] % vp_changes[i].value_count]
                                             : enabled[i] ? "on" : "off");
            }
        }
}

/* The decomp's portBalance bits (BALANCE.md 2.1, 12, 13): bits 9 (the machine guns' range) and 14 (Thumper's flame) since request 121; 11 (a borrowing
 * Hammerhead) goes with Hammerhead, 12 (Twister's throw) and 15 (her weapons locked) with Twister, 13 (Grasshopper's
 * landing) and 19 (her free energy jump) with Grasshopper. Written at each match's load (four.c); the C copies it at the round's start. */
unsigned vp_balance_word(void)
{
    static const struct { VpChangeId id; unsigned bits; } map[] = {
        {VP_BAL_MISSILES, 0x0001}, {VP_BAL_SHIELD, 0x0002}, {VP_BAL_MINES, 0x0004}, {VP_BAL_HAMMERHEAD, 0x1000808},
        {VP_BAL_AXEL, 0x0010}, {VP_BAL_MINION, 0x0020}, {VP_BAL_TWISTER, 0x9040}, {VP_BAL_GRASSHOPPER, 0x80082080u}, /* (bit 31, request 103: the leap pushes 300) */
        {VP_BAL_SHADOW, 0x0400}, {VP_BAL_OUTLAW2, 0x10000}, {VP_BAL_FREEZE, 0x20000},
        {VP_BAL_RICOCHET, 0x40000}, {VP_BAL_GUNS, 0x100000}, {VP_BAL_GUN_RANGE, 0x0200}, /* (request 121: bit 9 the range, bit 14 Thumper's flame) */
        /* Hammerhead's brakes 2 and slide speed 60: bit 24 (decomp request 77, BALANCE.md 17) */
        /* the recharges: Outlaw 2 bit 8, Mr. Grimm bit 21, Thumper bit 22 (decomp request 45, PLAYTEST_ANSWERS.md, BALANCE.md 14.1) */
        {VP_BAL_OUTLAW2, 0x0100}, {VP_BAL_GRIMM, 0x200000}, {VP_BAL_THUMPER, 0x404000},
        {VP_BAL_PICKUPS, 0x800000}, /* (request 67, BALANCE.md: the respawn's draw among the taken pickups that can show) */
        {VP_BAL_SLAM, 0x4000000}, {VP_AI_UNJAM, 0x28000000}, /* (request 91, BALANCE.md 19-20; bit 29 request 101, 22: the routes on Roof Tops) */
        {VP_BAL_ROADKILL, 0x10000000}, /* (request 95, BALANCE.md 21: the boomerang caught within 20 of its thrower) */
    };
    unsigned w = 0;
    int netplay_in_match(void); /* net/game_tm2.c */
    int online = netplay_in_match(); /* (online: every balancing rule, one ruleset for all, the user's; local play: each switch) */
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (online || vp_on(map[i].id)) w |= map[i].bits;
    w |= 0x40000000u; /* bit 30 always (request 102, BALANCE.md 23): an explosion far from the last one heard sounds too,
                       * inside the arena-wide cooldown (the user's missing explosion sounds, 2026-11-04): a fix, no option */
    if (getenv("TM2PC_TEST_PRE42")) w &= 0x3DFF; /* tests: the rules before requests 41-42 (the old baselines) */
    if (getenv("TM2PC_BAL_OFF")) w &= ~(unsigned)strtoul(getenv("TM2PC_BAL_OFF"), NULL, 16); /* tests: these bits off (hex) */
    return w;
}

/* The decomp's portVisual bits (request 42, CUSTOM_TOURNAMENT_ANSWERS.md, port_visual.h): 0 the taser and the lightning sorted by depth, 1 the taser
 * from the muzzle. Written at each match's load (four.c); read live. */
/* tests (decomp request 105's run 1): TM2PC_TEST_VISUAL_MASK=hex, only these portVisual bits ever on */
unsigned vp_visual_test_mask(void)
{
    static int read;
    static unsigned m = ~0u;
    if (!read) {
        read = 1;
        if (getenv("TM2PC_TEST_VISUAL_MASK")) m = (unsigned)strtoul(getenv("TM2PC_TEST_VISUAL_MASK"), NULL, 16);
    }
    return m;
}
unsigned vp_visual_word(void)
{
    if (getenv("TM2PC_TEST_PRE42")) return 0; /* tests: as before request 42 (CUSTOM_TOURNAMENT_ANSWERS.md) */
    /* bit 0 always: the taser and the lightning hidden by walls, no option (the user's, 2026-10-17: vanilla drew them
     * over everything, a fix nobody will miss) */
    /* bit 2 always: each view walks its own car first, so a full mesh queue or packet buffer drops far things, never
     * the player's car (decomp request 45, PLAYTEST_ANSWERS.md 3.3) */
    /* bits 3 and 4 always (request 46): each car's shadow sorted behind its own car (the user's on Sweet Tooth), and a new
     * bullet's first streak from the muzzle where it is now (not behind a fast car) */
    /* bit 5 (request 63, FAR_LOD.md): the chase camera kept inside the walls; bit 7 (request 92, RENDER_ANSWERS.md, RENDER_ANSWERS R7): steady shadows */
    return (1u | (vp_on(VP_TASER_LOW) ? 2u : 0u) | 4u | 8u | 16u | (vp_on(VP_CAMERA_WALLS) ? 32u : 0u) |
            (vp_on(VP_STEADY_SHADOW) ? 0x80u : 0u)) & vp_visual_test_mask();
}
