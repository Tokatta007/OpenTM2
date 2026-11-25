/* Vanilla Plus: our changes to the original game, each one switchable.
 *
 * Every change is registered in vplus.c with an id, a name and a category
 * (the Options > Gameplay page will list them by category) and is listed in
 * docs/VANILLA_PLUS.md. The preset sets all of them at once: Vanilla Plus
 * (all on, the default) or Original (all off); single changes can then be
 * switched in tm2pc.ini. Online play will force the Vanilla Plus set.
 *
 * Code that implements a change asks vp_on(VP_x) each time it runs, so a
 * change can be switched while the game runs. */
#pragma once
#include <stdio.h>
#include <stdint.h>

typedef enum { /* in the order the Options page lists them (Balancing, the longest, last) */
    VP_CAT_CAR_SELECT,
    VP_CAT_MENUS,
    VP_CAT_BATTLE,          /* what a battle shows: kill messages, the kill cam, Santa */
    VP_CAT_BUG_FIX,
    VP_CAT_BALANCING,       /* "Global Balancing": the rules for everyone */
    VP_CAT_CHARACTERS,      /* "Character Balancing": each car's own */
    VP_CAT_COUNT
} VpCategory;

typedef enum {
    VP_PAUSE_QUIT_BOOM,     /* pause menu Quit plays the menus' low boom too */
    VP_AXEL_EYES,           /* Axel's eyes: white with a brown center, not solid red */
    VP_LA_QUAKE_FINALE,     /* Los Angeles quakes: every one ends with its 20-frame burst (retail: a byte timer cuts late ones) */
    VP_SECRET_CARS_LAST,    /* car select: Sweet Tooth and Minion at the end of the carousel */
    VP_PASSWORDS_OFF,       /* the port's 1 Player menu, without Password (and none in the pause menu) */
    VP_CARSEL_PORTRAITS,    /* car select: a row of every driver's portrait, the chosen one highlighted */
    VP_CARSEL_MULTI,        /* car select with 2-4 players: 0 one after another, 1 split screen, 2 shared screen */
    VP_DUPLICATE_CARS,      /* players may drive the same car */
    VP_CARSEL_SOUNDS,       /* car select: the highlighted driver's special weapon sound (as Twisted Metal 1) */
    VP_KILL_MESSAGE,        /* a Challenge Match: "YOU KILLED X" when a player destroys a car */
    VP_KILL_STREAKS,        /* multi-kills and sprees announced (DOUBLE KILL!, KILLING SPREE!...), before YOU KILLED X */
    VP_NO_MENU_TIMEOUT,     /* menus wait (no going back to the title after a while without input) */
    VP_LIVES_PER_LEVEL,     /* tournament: 3 lives at the start of every level (co-op too, shared for now) */
    VP_NO_ROUND_RESTARTS,   /* a player's lost life brings back only their car (tournament and Challenge Match) */
    VP_HIDE_LOCKED_DRIVERS, /* car select's portrait bars: the secret drivers only once unlocked */
    VP_CARINFO_STATS,       /* Car Info pictures: Vanilla Plus's armor, special weapon and speed ratings */
    VP_COOP_SPECTATE,       /* co-op tournament: a player out of lives watches; the level is lost when both are out */
    VP_SANTA,               /* Antarctica: the PC version's Santa as a pedestrian (ui/santa.*, tools/art/santa.py) */
    VP_FINAL_KILL_CAM,      /* a match's last kill shown again from several cameras (vplus/killcam.c) */
    VP_SPAWN_SPACING,       /* a car never starts or respawns on top of another (fixes.c vp_StartNodePick) */
    VP_PHOTO_REWIND,        /* photo mode goes back up to 30 seconds (vplus/killcam.c: snapshots every 2 s) */
    /* the balancing rules (decomp requests 40-41, its portBalance word: vp_balance_word) */
    VP_BAL_MISSILES,        /* fire missile 10, homing 7 */
    VP_BAL_SHIELD,          /* a shield cast while shielded is refused, no cost */
    VP_BAL_MINES,           /* mines permanent, 5 per driver */
    VP_BAL_FREEZE,          /* ice freezes: half as long each time within 1 s of the last (request 41, BALANCE.md) */
    VP_BAL_RICOCHET,        /* a ricochet bomb cannot hit its owner before it has cleared the car (request 41, BALANCE.md) */
    VP_BAL_GUNS,            /* machine guns: every bullet that connects does half damage, no coin flip (round 3) */
    /* each car's (Character Balancing) */
    VP_BAL_HAMMERHEAD,      /* ram x1.2, crush 15 + 5 on top, recharge 35 s, brakes 2, slide speed 60 */
    VP_BAL_AXEL,            /* no crush, ram x1.1 */
    VP_BAL_MINION,          /* no crush, ram x1.1, recharge 30 s */
    VP_BAL_TWISTER,         /* no weapons during the tornado, recharge 30 s, no ram damage taken, the last hit throws */
    VP_BAL_GRASSHOPPER,     /* leap 18, no ram damage taken, touching counts, clean landing, free energy jump */
    VP_BAL_OUTLAW2,         /* the taser's second chance; recharge 45 s */
    VP_BAL_GRIMM,           /* recharge 60 s */
    VP_BAL_THUMPER,         /* recharge 75 s; no second special while his flame burns (bits 22, 14) */
    VP_BAL_SHADOW,          /* the slope bug fixed: 28 at most */
    /* the decomp's portVisual word (request 42, CUSTOM_TOURNAMENT_ANSWERS.md): vp_visual_word */
    VP_TASER_LOW,           /* the taser starts at Outlaw 2's muzzle, not above her roof */
    VP_CAMERA_WALLS,        /* the chase camera stays inside the arena's walls (decomp request 63, FAR_LOD.md, portVisual bit 5) */
    VP_BAL_PICKUPS,         /* a pickup's respawn never wasted on one still there (decomp request 67, BALANCE.md, portBalance bit 23) */
    VP_CARSEL_BORDERS,      /* 2-4 player car select: the big portraits framed and tagged in the player's color */
    VP_BAL_SLAM,            /* Mr. Slam: no pickups or special while his arm swings (decomp request 91, portBalance bit 26) */
    VP_AI_UNJAM,            /* computer cars jammed by cars back out (decomp request 91, portBalance bit 27) */
    VP_STEADY_SHADOW,       /* car shadows tilted with the car only, no suspension rocking (request 92, RENDER_ANSWERS.md, portVisual bit 7) */
    VP_BAL_ROADKILL,        /* Roadkill's boomerang always caught on its way back (decomp request 95, portBalance bit 28) */
    VP_GLACIER_FIRE,        /* Antarctica's fire missile out of the mark that hides it (decomp request 120, RETAIL_BUGS.md); online always */
    VP_BAL_GUN_RANGE,       /* machine-gun bullets fly 25% farther (decomp request 121, BALANCE.md 25.2, portBalance bit 9) */
    VP_WARTHOG_TRIM,        /* Warthog's side: the gray strip above his side windows as the body's camo, as the PC version */
    VP_BAL_PICKUP_PASS,     /* every shot passes pickups (the ricochet bomb trapped by one by a wall: decomp request 124, portBalance2 bit 0) */
    VP_COUNT
} VpChangeId;

typedef struct {
    const char *key;        /* tm2pc.ini name */
    const char *name;       /* as the options page shows it */
    VpCategory category;
    const char *description;
    const char *const *values; /* a change with more than on/off: its values' ini names (value 0 = the original) */
    int value_count;
    int plus_value;            /* its value in the Vanilla Plus preset */
    int plus_off;              /* an on/off change the Vanilla Plus preset leaves off (the player turns it on) */
} VpChange;

extern const VpChange vp_changes[VP_COUNT];
extern const VpChangeId vp_menu_order[VP_COUNT]; /* the Options page's (and tm2pc.ini's) order inside each category */
extern const char *const vp_category_names[VP_CAT_COUNT];

int vp_on(VpChangeId id);
int vp_retired(int id); /* taken out of the game: never on, not offered */
unsigned vp_balance_word(void);
unsigned vp_balance_word2(void); /* the decomp's portBalance2 bits (request 124) */ /* the balancing rules on, as the decomp's portBalance bits (requests 40-41) */
unsigned vp_visual_word(void);  /* the decomp's portVisual bits (request 42, CUSTOM_TOURNAMENT_ANSWERS.md) */
int vp_jugg_car_swap(void);    /* fixes.c: the build can give the Juggernaut a car (decomp request 32 step 3) */
int vp_minion_opponent(void);  /* the opponent loaded as the Juggernaut's car (parked), -1 none */
void vp_jugg_car_set(int opp); /* ui_AiSelect: that opponent, or -1 */
void vp_jugg_car_unbind(void); /* four.c: before every bind */
void vp_jugg_car_bound(void);  /* after every bind */
int vp_value(VpChangeId id);        /* for a change with values (0 = the original) */
void vp_set(VpChangeId id, int on);
void vp_preset(int vanilla_plus);   /* 1: all on, 0: Original (all off) */

/* The [vanilla_plus] section of the settings file (settings.c). */
/* Car select's carousel: the car it is on, -1 when not known (fixes.c). */
uint32_t vp_font_color_get(int font);           /* fixes.c: a font's color (r g b), to put back after drawing */
void vp_font_color_put(int font, uint32_t rgb);
int vp_draw_distance(void);   /* fixes.c: the draw distance in use (0-2): online always Farthest, the same for everyone */
int vp_carousel_car(void);
void vp_carousel_forget(void);
void vp_carsel_sound(int car);
void vp_team_palette(int car, int team, const unsigned short *in, unsigned short *out); /* car select: a team's body palette */ /* VP_CARSEL_SOUNDS: car select is on this car now (-1: none, stop) */

void vp_ini_begin(void);
void vp_ini_key(const char *key, const char *val);
void vp_ini_end(void);
void vp_ini_write(FILE *f);

/* vplus/humans.c: a human's easy-energy input word, besides the energy buttons (0x10-0x80, 0x100) and the choices in
 * bits 0-3: the controller's own Select, L1 and R1 held (the input word has each action's Standard button instead) */
enum { EASY_SELECT = 0x200, EASY_L1 = 0x400, EASY_R1 = 0x800 };
