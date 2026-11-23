/* Button bindings per action (bindings.h).
 *
 * The game turns a controller layout (settings block +38/+39, Keypad 1 and 2)
 * into action bindings when a race starts: sub_801206F4(layout, port) binds
 * each row of the 43-row table D_80162F64 ({action, button for layouts
 * 0..3}) with sub_80150F70(action, button, port) and makes the port's
 * configured bindings active with sub_80150FC0(port) (the decomp's
 * docs/OPTIONS_CONTROLS.md). The port wraps it: after the game's layout,
 * each action the player has given another button is bound again, and the
 * bindings made active once more.
 *
 * Rebindable are the twelve driving actions the car reads (car_input.c,
 * sub_800DE5F8): 0x16-0x21. The table below is D_80162F64's rows for them.
 * Player 1's are in the signed-in profile (profiles/<name>.ini, [controls]:
 * layout = standard, accelerate = square ... default: the layout's button;
 * accelerate_key = W ...; next_song ...), player 2's in tm2pc.ini
 * ([controls] p2_accelerate ...) until players 2-4 have profiles too. An
 * older tm2pc.ini's p1_ keys are still read, and move to the profile.
 *
 * Keyboard keys belong to actions too (p1_accelerate_key = W ...): input.c
 * presses the action's PS1 button while its key is held, so a key keeps its
 * action when the action gets another button. Player 1 starts with the
 * port's usual keys; player 2 with none (two players can share the keyboard
 * by giving player 2 keys). */
#include <stdio.h>
#include <SDL3/SDL.h>
#include <string.h>
#include "runtime/runtime.h"
#include "settings.h"
#include "bindings.h"

#define BIND_LAYOUT GA(0x801206F4u)   /* sub_801206F4(layout, port) */
#define BIND_ACTION GA(0x80150F70u)   /* sub_80150F70(action, button, port) */
#define BIND_COMMIT GA(0x80150FC0u)   /* sub_80150FC0(port) */

static const struct {
    unsigned char action;          /* the game's action number */
    const char *key;               /* in tm2pc.ini */
    unsigned char layout[4];       /* button in Standard, Run 'N Gun, Control Freak, Deadly Force */
} actions[BIND_ACTIONS] = {
    {0x18, "accelerate", {13, 10, 12, 12}},
    {0x19, "brake", {14, 9, 10, 10}},
    {0x17, "steer_left", {7, 7, 7, 7}},
    {0x16, "steer_right", {8, 8, 8, 8}},
    {0x1A, "forward", {5, 5, 5, 5}},
    {0x1B, "reverse", {6, 6, 6, 6}},
    {0x1C, "tight_turn", {12, 4, 4, 4}},
    {0x1D, "turbo", {11, 3, 11, 11}},
    {0x1F, "fire_weapon", {4, 12, 14, 13}},   /* the special weapon (L2 in Standard) */
    {0x1E, "machine_gun", {10, 13, 13, 14}},  /* R2 in Standard: checked in a race */
    /* the game's 0x21 ("cycle next", +1) goes up the TM2 list, 0x20 down it: named for what the player sees, the
     * next weapon the one below (on TM:Black and TM:2012 the one on the right). The game's layouts put 0x21 on R1
     * (up); the port's defaults trade them: L1 up / left, R1 down / right in every HUD style (the user's,
     * 2026-11-22; TooMuchCheez's report). Bound so after the game's layout (bind_LayoutApply). */
    {0x21, "previous_weapon", {3, 14, 3, 3}},
    {0x20, "next_weapon", {9, 11, 9, 9}},
};
enum { A_PREV = 10, A_NEXT = 11 }; /* (the two rows above) */

const char *const bind_action_names[BIND_ACTIONS] = {
    "Accelerate", "Brake", "Steer left", "Steer right", "Forward", "Reverse",
    "Tight turn", "Turbo", "Fire weapon", "Machine gun", "Previous weapon", "Next weapon",
};

const char *const bind_button_names[BIND_TOUCHPAD + 1] = {
    "-", "SELECT", "START", "L1", "L2", "UP", "DOWN", "LEFT", "RIGHT",
    "R1", "R2", "TRIANGLE", "CROSS", "SQUARE", "CIRCLE", "L3", "R3", "TOUCHPAD",
};
const char *const bind_button_names_xbox[BIND_TOUCHPAD + 1] = {
    "-", "VIEW", "MENU", "LB", "LT", "UP", "DOWN", "LEFT", "RIGHT",
    "RB", "RT", "Y", "A", "X", "B", "LS", "RS", "SHARE",
};
static const char *const button_keys[BIND_TOUCHPAD + 1] = {
    "default", "select", "start", "l1", "l2", "up", "down", "left", "right",
    "r1", "r2", "triangle", "cross", "square", "circle", "l3", "r3", "touchpad",
};

const char *const bind_extra_names[BIND_EXTRAS] = {"Next song", "Previous song", "Push to talk", "Scoreboard"};
static const char *const extra_keys[BIND_EXTRAS] = {"next_song", "previous_song", "push_to_talk", "scoreboard"}; /* (their ini names) */
static int extra_button[BIND_EXTRAS] = {0, 0, 0, BIND_TOUCHPAD}; /* the songs and talking: none by default (not by
                                                                  * accident); the scoreboard: the touchpad (the user's) */
static int extra_key[BIND_EXTRAS] = {SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_V, SDL_SCANCODE_TAB};
static int is_extra_key(const char *key) /* an extra control's ini name (or its _key): player 1's, unprefixed */
{
    for (int i = 0; i < BIND_EXTRAS; i++) {
        size_t n = strlen(extra_keys[i]);
        if (!strncmp(key, extra_keys[i], n) && (!key[n] || !strcmp(key + n, "_key"))) return 1;
    }
    return 0;
}
const char *const bind_energy_names[BIND_ENERGY] = {"Freeze", "Rear fire", "Mines", "Shield", "Invisibility"};
static const char *const energy_keys[BIND_ENERGY] = {"energy_freeze", "energy_rear_fire", "energy_mines", "energy_shield",
                                                     "energy_invisibility"}; /* (their ini names, after pN_) */
static int energy_button[4][BIND_ENERGY];        /* none by default: the D-pad has the first four */
static int energy_key[2][BIND_ENERGY];
static int pad_type[4];                          /* 0 auto, 1 PlayStation, 2 Xbox */
static int jump_combo[4], easy_energy[4];         /* L1 + R1 jumps; the D-pad's energy attacks (steering on the stick) */
static int analog_steer[4] = {1, 1, 1, 1};        /* the stick turns as far as it is tilted */
static int steer_keep[4] = {1, 1, 1, 1};          /* in reverse, Left still turns the nose left (decomp request 66, ANALOG_STEERING.md) */
static int keypad34[2];                           /* players 3-4's layouts (1-2's: settings_game.keypad) */
static int layout_of(int p) { return p < 2 ? settings_game.keypad[p] : keypad34[(p - 2) & 1]; }
int bind_layout(int player) { return player >= 0 && player <= 3 ? layout_of(player) : 0; }
int bind_analog_steer(int player) { return player >= 0 && player <= 3 && analog_steer[player]; }
void bind_set_analog_steer(int player, int on) { if (player >= 0 && player <= 3) analog_steer[player] = on != 0; }
int bind_steer_keep(int player) { return player >= 0 && player <= 3 && steer_keep[player]; }
void bind_set_steer_keep(int player, int on) { if (player >= 0 && player <= 3) steer_keep[player] = on != 0; }
int bind_jump_combo(int player) { return player >= 0 && player <= 3 && jump_combo[player]; }
/* with L1 + R1 jump: a lone shoulder's weapon switch waits this many game steps (0 off, 1-3: 50, 100, 150 ms) for the
 * other shoulder (then a jump, no switch) */
static int jump_safety[4] = {1, 1, 1, 1};
static const char *const jump_safety_keys[4] = {"off", "50", "100", "150"};
int bind_jump_safety(int player) { return player >= 0 && player <= 3 ? jump_safety[player] : 0; }
void bind_set_jump_safety(int player, int v) { if (player >= 0 && player <= 3 && v >= 0 && v <= 3) jump_safety[player] = v; }
void bind_set_jump_combo(int player, int on) { if (player >= 0 && player <= 3) jump_combo[player] = on != 0; }
int bind_easy_energy(int player) { return player >= 0 && player <= 3 && easy_energy[player]; }
void bind_set_easy_energy(int player, int on) { if (player >= 0 && player <= 3) easy_energy[player] = on != 0; }
static const char *const pad_type_keys[3] = {"auto", "playstation", "xbox"};
static const char *const layout_keys[4] = {"standard", "run_n_gun", "control_freak", "deadly_force"};

int bind_extra_button(int i) { return i >= 0 && i < BIND_EXTRAS ? extra_button[i] : 0; }
void bind_set_extra_button(int i, int button)
{
    if (i >= 0 && i < BIND_EXTRAS && button >= 0 && button <= BIND_TOUCHPAD) extra_button[i] = button;
}
int bind_extra_key(int i) { return i >= 0 && i < BIND_EXTRAS ? extra_key[i] : 0; }
/* an energy attack's button: its own, else (Easy Energy Attacks on) the first four's D-pad direction, Up freeze, Down
 * rear fire, Left mines, Right shield (5..8: UP, DOWN, LEFT, RIGHT), shown so on the Controls page (the user's) */
int bind_energy_button(int player, int i)
{
    if (player < 0 || player > 3 || i < 0 || i >= BIND_ENERGY) return 0;
    if (!energy_button[player][i] && i < 4 && easy_energy[player]) return 5 + i;
    return energy_button[player][i];
}
void bind_set_energy_button(int player, int i, int button)
{
    if (player >= 0 && player <= 3 && i >= 0 && i < BIND_ENERGY && button >= 0 && button <= BIND_BUTTONS_ALL)
        energy_button[player][i] = i < 4 && button == 5 + i ? 0 : button; /* (its D-pad direction: the default) */
}
int bind_energy_key(int player, int i)
{
    return player >= 0 && player <= 1 && i >= 0 && i < BIND_ENERGY ? energy_key[player][i] : 0;
}
int bind_pad_type(int player) { return player >= 0 && player <= 3 ? pad_type[player] : 0; }
void bind_set_pad_type(int player, int type)
{
    if (player >= 0 && player <= 3 && type >= 0 && type <= 2) pad_type[player] = type;
}

static unsigned char chosen[4][BIND_ACTIONS]; /* 0: the layout's button */
static const int default_keys[BIND_ACTIONS] = { /* player 1's */
    SDL_SCANCODE_W, SDL_SCANCODE_C, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_UP, SDL_SCANCODE_S,
    SDL_SCANCODE_X, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_Q, SDL_SCANCODE_E,
};
static int keys[2][BIND_ACTIONS] = {
    {SDL_SCANCODE_W, SDL_SCANCODE_C, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_UP, SDL_SCANCODE_S,
     SDL_SCANCODE_X, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_Q, SDL_SCANCODE_E},
    {0},
};

int bind_key(int player, int action)
{
    return player >= 0 && player <= 1 && action >= 0 && action < BIND_ACTIONS ? keys[player][action] : 0;
}

/* one key, one use: a key taken by another action or song key is freed */
static void free_key(int scancode)
{
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < BIND_ACTIONS; a++)
            if (scancode && keys[p][a] == scancode) keys[p][a] = 0;
    for (int i = 0; i < BIND_EXTRAS; i++)
        if (scancode && extra_key[i] == scancode) extra_key[i] = 0;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < BIND_ENERGY; i++)
            if (scancode && energy_key[p][i] == scancode) energy_key[p][i] = 0;
}

void bind_set_key(int player, int action, int scancode)
{
    if (player < 0 || player > 1 || action < 0 || action >= BIND_ACTIONS) return;
    free_key(scancode);
    keys[player][action] = scancode;
}

void bind_set_energy_key(int player, int i, int scancode)
{
    if (player < 0 || player > 1 || i < 0 || i >= BIND_ENERGY) return;
    free_key(scancode);
    energy_key[player][i] = scancode;
}

void bind_set_extra_key(int i, int scancode)
{
    if (i < 0 || i >= BIND_EXTRAS) return;
    free_key(scancode);
    extra_key[i] = scancode;
}

int bind_player_has_keys(int player)
{
    for (int a = 0; a < BIND_ACTIONS; a++)
        if (bind_key(player, a)) return 1;
    return 0;
}

const char *bind_key_name(int scancode)
{
    if (!scancode) return "-";
    switch (scancode) { /* told apart from the D-pad's UP, DOWN ... */
    case SDL_SCANCODE_UP: return "Up arrow";
    case SDL_SCANCODE_DOWN: return "Down arrow";
    case SDL_SCANCODE_LEFT: return "Left arrow";
    case SDL_SCANCODE_RIGHT: return "Right arrow";
    }
    const char *n = SDL_GetScancodeName((SDL_Scancode)scancode);
    return n && *n ? n : "?";
}

/* input.c */
const char *input_wanted(int player);
void input_set_wanted(int player, const char *name);

int bind_layout_button(int layout, int action)
{
    if (layout < 0 || layout > 3 || action < 0 || action >= BIND_ACTIONS) return 0;
    return actions[action].layout[layout];
}

int bind_button(int player, int action)
{
    if (player < 0 || player > 3 || action < 0 || action >= BIND_ACTIONS) return 0;
    return chosen[player][action] ? chosen[player][action]
                                  : bind_layout_button(layout_of(player), action);
}

void bind_set(int player, int action, int button)
{
    if (player < 0 || player > 3 || action < 0 || action >= BIND_ACTIONS) return;
    /* one button, one action: the action that had this button takes this one's old button (a key presses its
     * action's button, so two actions on one button fired together: Phoenix's machine gun and turbo, 2026-11-22) */
    int old = bind_button(player, action);
    for (int b = 0; b < BIND_ACTIONS; b++)
        if (b != action && button && bind_button(player, b) == button)
            chosen[player][b] = (unsigned char)(old == bind_layout_button(layout_of(player), b) ? 0 : old);
    /* the layout's own button is stored as "default" */
    chosen[player][action] = (unsigned char)(button == bind_layout_button(layout_of(player), action) ? 0 : button);
}

void bind_reset(int player)
{
    if (player >= 0 && player <= 3) memset(chosen[player], 0, sizeof chosen[player]);
}

int bind_is_custom(int player)
{
    if (player < 0 || player > 3) return 0;
    for (int a = 0; a < BIND_ACTIONS; a++)
        if (chosen[player][a]) return 1;
    return 0;
}

int bind_ini_key(const char *key, const char *val)
{
    /* p1_ / p2_: each player's L1 + R1 jump and D-pad energy attacks (yes / no) */
    if ((key[0] == 'p') && key[1] >= '1' && key[1] <= '4' && key[2] == '_') {
        int p = key[1] - '1', on = !strcmp(val, "yes") || !strcmp(val, "on") || !strcmp(val, "1");
        if (!strcmp(key + 3, "jump_l1_r1")) { jump_combo[p] = on; return 1; }
        if (!strcmp(key + 3, "jump_safety")) { /* off, 50, 100, 150 (ms) */
            for (int v = 0; v < 4; v++) if (!strcmp(val, jump_safety_keys[v])) jump_safety[p] = v;
            return 1;
        }
        if (!strcmp(key + 3, "energy_dpad")) { easy_energy[p] = on; return 1; }
        if (!strcmp(key + 3, "analog_steering")) { analog_steer[p] = on; return 1; }
        if (!strcmp(key + 3, "no_backwards_steering")) { steer_keep[p] = on; return 1; }
    }
    if (!strncmp(key, "player", 6) && key[6] >= '1' && key[6] <= '4' && !strcmp(key + 7, "_controller")) {
        input_set_wanted(key[6] - '1', val);
        return 1;
    }
    if (!strcmp(key, "player1_type") || !strcmp(key, "player2_type")) {
        for (int t = 0; t < 3; t++)
            if (!strcmp(val, pad_type_keys[t])) pad_type[key[6] - '1'] = t;
        return 1;
    }
    for (int i = 0; i < BIND_EXTRAS; i++) {
        const char *name = extra_keys[i];
        size_t n = strlen(name);
        if (strncmp(key, name, n)) continue;
        if (!key[n]) {
            for (int b = 0; b <= BIND_TOUCHPAD; b++)
                if (!strcmp(val, b ? button_keys[b] : "none")) extra_button[i] = b;
            return 1;
        }
        if (!strcmp(key + n, "_key")) {
            extra_key[i] = strcmp(val, "none") ? (int)SDL_GetScancodeFromName(val) : 0;
            return 1;
        }
    }
    if ((key[0] != 'p') || key[1] < '1' || key[1] > '4' || key[2] != '_') return 0;
    int p = key[1] - '1';
    for (int i = 0; i < BIND_ENERGY; i++) {
        size_t n = strlen(energy_keys[i]);
        if (strncmp(key + 3, energy_keys[i], n)) continue;
        if (!key[3 + n]) {
            for (int b = 0; b <= BIND_BUTTONS_ALL; b++)
                if (!strcmp(val, b ? button_keys[b] : "none")) energy_button[p][i] = b;
            return 1;
        }
        if (!strcmp(key + 3 + n, "_key")) {
            if (p < 2) energy_key[p][i] = strcmp(val, "none") ? (int)SDL_GetScancodeFromName(val) : 0;
            return 1;
        }
    }
    for (int a = 0; a < BIND_ACTIONS; a++) {
        size_t n = strlen(actions[a].key);
        if (strncmp(key + 3, actions[a].key, n)) continue;
        if (!key[3 + n]) {
            for (int b = 0; b <= BIND_BUTTONS; b++)
                if (!strcmp(val, button_keys[b])) chosen[p][a] = (unsigned char)b;
            return 1;
        }
        if (!strcmp(key + 3 + n, "_key")) {
            if (p < 2) keys[p][a] = strcmp(val, "none") ? (int)SDL_GetScancodeFromName(val) : 0; /* players 3-4: no keys */
            return 1;
        }
    }
    return 0;
}

static void write_energy(FILE *f, int p, const char *prefix, int with_keys)
{
    fprintf(f, "; the energy attacks' own buttons (with energy_dpad = yes): none, l3, r3 ...%s\n", with_keys ? "; and keys" : "");
    for (int i = 0; i < BIND_ENERGY; i++) {
        fprintf(f, "%s%s = %s\n", prefix, energy_keys[i], energy_button[p][i] ? button_keys[energy_button[p][i]] : "none");
        if (with_keys)
            fprintf(f, "%s%s_key = %s\n", prefix, energy_keys[i],
                    energy_key[p][i] ? SDL_GetScancodeName((SDL_Scancode)energy_key[p][i]) : "none");
    }
}

static void write_actions(FILE *f, int p, const char *prefix)
{
    for (int a = 0; a < BIND_ACTIONS; a++) {
        fprintf(f, "%s%s = %s\n", prefix, actions[a].key, button_keys[chosen[p][a]]);
        fprintf(f, "%s%s_key = %s\n", prefix, actions[a].key,
                keys[p][a] ? SDL_GetScancodeName((SDL_Scancode)keys[p][a]) : "none");
    }
    write_energy(f, p, prefix, 1);
}

void bind_ini_write(FILE *f)
{
    fprintf(f, "; each player's controller: auto (in the order found), none, or its name\n");
    fprintf(f, "player1_controller = %s\nplayer2_controller = %s\nplayer3_controller = %s\nplayer4_controller = %s\n",
            input_wanted(0), input_wanted(1), input_wanted(2), input_wanted(3));
    fprintf(f, "; the button names shown for each player: auto, playstation or xbox\n");
    fprintf(f, "player1_type = %s\nplayer2_type = %s\n", pad_type_keys[pad_type[0]], pad_type_keys[pad_type[1]]);
    fprintf(f, "; player 2's buttons per action: default (the keypad layout's) or select,\n"
               "; start, l1, l2, up, down, left, right, r1, r2, triangle, cross, square, circle;\n"
               "; and keyboard keys (a key name, or none). Player 1's are in the profile.\n");
    write_actions(f, 1, "p2_");
    fprintf(f, "p2_jump_l1_r1 = %s\np2_energy_dpad = %s\np2_analog_steering = %s\n", jump_combo[1] ? "yes" : "no",
            easy_energy[1] ? "yes" : "no", analog_steer[1] ? "yes" : "no");
    fprintf(f, "; in reverse, Left still turns the car's nose left on screen (no: backwards, as the original)\n"
               "p2_no_backwards_steering = %s\n", steer_keep[1] ? "yes" : "no");
    fprintf(f, "; with L1 + R1 jump, a lone L1 / R1 waits this long (ms) for the other before switching weapons: off, 50, 100, 150\n"
               "p2_jump_safety = %s\n", jump_safety_keys[jump_safety[1]]);
}

/* ---- player 1's, in the profile ---- */

void bind_player1_defaults(void)
{
    memset(chosen[0], 0, sizeof chosen[0]);
    for (int a = 0; a < BIND_ACTIONS; a++) {
        for (int b = 0; b < BIND_ACTIONS; b++) /* one key, one use */
            if (keys[1][b] == default_keys[a]) keys[1][b] = 0;
        keys[0][a] = default_keys[a];
    }
    extra_button[0] = extra_button[1] = extra_button[BIND_PTT] = 0;
    extra_button[BIND_SCOREBOARD] = BIND_TOUCHPAD;
    memset(energy_button[0], 0, sizeof energy_button[0]);
    memset(energy_key[0], 0, sizeof energy_key[0]);
    jump_combo[0] = easy_energy[0] = 0;
    jump_safety[0] = 1;
    analog_steer[0] = 1;
    steer_keep[0] = 1;
    extra_key[0] = SDL_SCANCODE_PAGEDOWN;
    extra_key[1] = SDL_SCANCODE_PAGEUP;
    extra_key[BIND_PTT] = SDL_SCANCODE_V;
    extra_key[BIND_SCOREBOARD] = SDL_SCANCODE_TAB;
    settings_game.keypad[0] = 0;
}

/* Player 2 from a profile (split-screen sign-in): its layout and controller
 * buttons; the keyboard stays player 1's (no keys). */
void bind_player2_defaults(void)
{
    memset(chosen[1], 0, sizeof chosen[1]);
    memset(keys[1], 0, sizeof keys[1]);
    memset(energy_button[1], 0, sizeof energy_button[1]);
    memset(energy_key[1], 0, sizeof energy_key[1]);
    settings_game.keypad[1] = 0;
    jump_combo[1] = easy_energy[1] = 0;
    jump_safety[1] = 1;
    analog_steer[1] = 1;
    steer_keep[1] = 1;
}

/* Player 2's controls into their profile: the layout and the buttons (the
 * keyboard and the song keys are player 1's). */
void bind_profile_write_p2(FILE *f)
{
    fprintf(f, "[controls]\n; the controls when this profile plays\n"
               "; layout: standard, run_n_gun, control_freak or deadly_force\n");
    fprintf(f, "layout = %s\n", layout_keys[settings_game.keypad[1]]);
    for (int a = 0; a < BIND_ACTIONS; a++)
        fprintf(f, "%s = %s\n", actions[a].key, button_keys[chosen[1][a]]);
    write_energy(f, 1, "", 0);
    fprintf(f, "; L1 + R1 jumps; energy attacks on the D-pad (steering on the stick): yes or no\n");
    fprintf(f, "jump_l1_r1 = %s\nenergy_dpad = %s\nanalog_steering = %s\n", jump_combo[1] ? "yes" : "no",
            easy_energy[1] ? "yes" : "no", analog_steer[1] ? "yes" : "no");
    fprintf(f, "; in reverse, Left still turns the car's nose left on screen (no: backwards, as the original)\n"
               "no_backwards_steering = %s\n", steer_keep[1] ? "yes" : "no");
    fprintf(f, "; with L1 + R1 jump, a lone L1 / R1 waits this long (ms) for the other before switching weapons: off, 50, 100, 150\n"
               "jump_safety = %s\n", jump_safety_keys[jump_safety[1]]);
}

int bind_profile_key_p2(const char *key, const char *val)
{
    if (!strcmp(key, "layout")) {
        for (int l = 0; l < 4; l++)
            if (!strcmp(val, layout_keys[l])) settings_game.keypad[1] = l;
        return 1;
    }
    size_t n = strlen(key);
    if (is_extra_key(key) || (n > 4 && !strcmp(key + n - 4, "_key")))
        return 1; /* the songs and the keyboard are player 1's */
    char p2[64];
    snprintf(p2, sizeof p2, "p2_%s", key);
    return bind_ini_key(p2, val);
}

/* Players 3 and 4 from a profile (the sign-in): its layout and controller
 * buttons, the jump, energy and steering choices (no keyboard). */
void bind_player_defaults(int p)
{
    if (p < 2 || p > 3) return;
    memset(chosen[p], 0, sizeof chosen[p]);
    memset(energy_button[p], 0, sizeof energy_button[p]);
    keypad34[p - 2] = 0;
    jump_combo[p] = easy_energy[p] = 0;
    jump_safety[p] = 1;
    analog_steer[p] = 1;
    steer_keep[p] = 1;
}
int bind_profile_key_p(int p, const char *key, const char *val)
{
    if (p < 2 || p > 3) return 0;
    if (!strcmp(key, "layout")) {
        for (int l = 0; l < 4; l++)
            if (!strcmp(val, layout_keys[l])) keypad34[p - 2] = l;
        return 1;
    }
    size_t n = strlen(key);
    if (is_extra_key(key) || (n > 4 && !strcmp(key + n - 4, "_key")))
        return 1;
    char pk[64];
    snprintf(pk, sizeof pk, "p%d_%s", p + 1, key);
    return bind_ini_key(pk, val);
}

int bind_profile_key(const char *key, const char *val)
{
    if (!strcmp(key, "layout")) {
        for (int l = 0; l < 4; l++)
            if (!strcmp(val, layout_keys[l])) settings_game.keypad[0] = l;
        return 1;
    }
    if (is_extra_key(key)) /* (push_to_talk went to p1_push_to_talk and was lost) */
        return bind_ini_key(key, val);
    char p1[64];
    snprintf(p1, sizeof p1, "p1_%s", key);
    return bind_ini_key(p1, val);
}

void bind_profile_write(FILE *f)
{
    fprintf(f, "[controls]\n; player 1's controls when signed in with this profile\n"
               "; layout: standard, run_n_gun, control_freak or deadly_force\n");
    fprintf(f, "layout = %s\n", layout_keys[settings_game.keypad[0]]);
    fprintf(f, "; next / previous song (custom music on random or in order), push to talk (online, held), the scoreboard\n"
               "; (in a battle, held; on any controller): a button (none, l3, r3, touchpad ...) and a key\n");
    for (int i = 0; i < BIND_EXTRAS; i++) {
        const char *name = extra_keys[i];
        fprintf(f, "%s = %s\n", name, extra_button[i] ? button_keys[extra_button[i]] : "none");
        fprintf(f, "%s_key = %s\n", name, extra_key[i] ? SDL_GetScancodeName((SDL_Scancode)extra_key[i]) : "none");
    }
    fprintf(f, "; each action's button: default (the layout's) or select, start, l1, l2, up, down,\n"
               "; left, right, r1, r2, triangle, cross, square, circle; and its keyboard key\n");
    write_actions(f, 0, "");
    fprintf(f, "; L1 + R1 jumps; energy attacks on the D-pad (steering on the stick): yes or no\n");
    fprintf(f, "jump_l1_r1 = %s\nenergy_dpad = %s\nanalog_steering = %s\n", jump_combo[0] ? "yes" : "no",
            easy_energy[0] ? "yes" : "no", analog_steer[0] ? "yes" : "no");
    fprintf(f, "; in reverse, Left still turns the car's nose left on screen (no: backwards, as the original)\n"
               "no_backwards_steering = %s\n", steer_keep[0] ? "yes" : "no");
    fprintf(f, "; with L1 + R1 jump, a lone L1 / R1 waits this long (ms) for the other before switching weapons: off, 50, 100, 150\n"
               "jump_safety = %s\n", jump_safety_keys[jump_safety[0]]);
}

/* sub_801206F4(layout, port), wrapped: the player's own buttons on top. */
void F_801206F4_orig(CPU *c);
static int apply_as = -1; /* players 3-4's buttons onto port 0 (vplus/four.c builds their table) */
void bind_apply_as(int player) { apply_as = player; }
void bind_LayoutApply(CPU *c)
{
    uint32_t port = c->r[5];
    F_801206F4_orig(c);
    if (port > 1 || apply_as == BIND_APPLY_BARE) /* (the bare layout: vplus/humans.c's shared table, no one's own buttons) */
        return;
    int who = apply_as >= 0 && apply_as <= 3 ? apply_as : (int)port;
    /* every action the player chose, and the weapon cycling's two always: the port's defaults are not the game's */
    for (int a = 0; a < BIND_ACTIONS; a++)
        if (chosen[who][a] || a == A_PREV || a == A_NEXT)
            guest_call(BIND_ACTION, actions[a].action, (uint32_t)bind_button(who, a), port, 0);
    guest_call(BIND_COMMIT, port, 0, 0, 0);
    if (M.trace) fprintf(stderr, "controls: player %u's buttons applied\n", port + 1);
}
