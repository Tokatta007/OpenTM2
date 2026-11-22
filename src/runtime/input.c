/* Live controller input: keyboard and gamepads through SDL3 (the SDL that
 * PSY-Z links), presented to the game as PS1 digital pads.
 *
 * Gamepads (any SDL gamepad, including DualShock 4 / DualSense): the face
 * buttons, shoulders, triggers, D-pad, Start and Back/Share map to the same
 * PS1 buttons by position; the left stick also works as the D-pad. Every
 * connected gamepad is opened; two of them are players 1 and 2. Which ones
 * is chosen on the Controls page (by name, kept in tm2pc.ini: [controls]
 * player1_controller / player2_controller); "auto" takes the gamepads in the
 * order they were found. Each player's pad shows its
 * player number (DualSense player LEDs) and a light bar color: red for
 * player 1, blue for player 2 (green and yellow are kept for 3 and 4).
 *
 * Keyboard: the driving keys belong to actions (controls/bindings.c, chosen
 * on the Controls page, for either player): a key presses whatever PS1
 * button that action has. Fixed keys for the menus: the arrow keys (D-pad),
 * Enter (Start), Backspace (Select), X (Cross), C (Circle), Z (Square). */
#include <SDL3/SDL.h>
#include <string.h>
#include "runtime.h"
#include "controls/bindings.h"

/* PS1 pad bits (raw halfword, set = pressed here; inverted when stored) */
enum {
    PSX_SELECT = 1 << 0, PSX_L3 = 1 << 1, PSX_R3 = 1 << 2, PSX_START = 1 << 3,
    PSX_UP = 1 << 4, PSX_RIGHT = 1 << 5, PSX_DOWN = 1 << 6, PSX_LEFT = 1 << 7,
    PSX_L2 = 1 << 8, PSX_R2 = 1 << 9, PSX_L1 = 1 << 10, PSX_R1 = 1 << 11,
    PSX_TRIANGLE = 1 << 12, PSX_CIRCLE = 1 << 13, PSX_CROSS = 1 << 14, PSX_SQUARE = 1 << 15,
};

/* the game's button numbers (bindings.h) as PS1 pad bits */
static const uint16_t button_bits[BIND_TOUCHPAD + 1] = {
    0, PSX_SELECT, PSX_START, PSX_L1, PSX_L2, PSX_UP, PSX_DOWN, PSX_LEFT, PSX_RIGHT,
    PSX_R1, PSX_R2, PSX_TRIANGLE, PSX_CROSS, PSX_SQUARE, PSX_CIRCLE, PSX_L3, PSX_R3, 0, /* (the touchpad: no PS1 bit) */
};

static const struct { SDL_Scancode key; uint16_t bits; } menu_keys[] = {
    {SDL_SCANCODE_UP, PSX_UP},           {SDL_SCANCODE_DOWN, PSX_DOWN},
    {SDL_SCANCODE_LEFT, PSX_LEFT},       {SDL_SCANCODE_RIGHT, PSX_RIGHT},
    {SDL_SCANCODE_RETURN, PSX_START},    {SDL_SCANCODE_BACKSPACE, PSX_SELECT},
    {SDL_SCANCODE_X, PSX_CROSS},         {SDL_SCANCODE_C, PSX_CIRCLE},
    {SDL_SCANCODE_Z, PSX_SQUARE},
};

#define STICK_DEADZONE 12000
#define TRIGGER_THRESHOLD 8000
#define MAX_PADS 8

static SDL_Gamepad *all[MAX_PADS];  /* every connected gamepad */
static int all_count;
#define PLAYERS 4
static SDL_Gamepad *pads[PLAYERS];  /* players 1-4 (among all) */
static char wanted[PLAYERS][128];   /* "" (auto), "none" or a gamepad's name */
static int pad_taken(SDL_Gamepad *g, int except) /* another player has it */
{
    for (int q = 0; q < PLAYERS; q++)
        if (q != except && pads[q] == g) return 1;
    return 0;
}

static void mark_players(void)
{
    static const Uint8 color[4][3] = {{255, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 200, 0}};
    for (int i = 0; i < all_count; i++)
        SDL_SetGamepadPlayerIndex(all[i], -1);
    for (int p = 0; p < PLAYERS; p++)
        if (pads[p]) {
            SDL_SetGamepadPlayerIndex(pads[p], p);
            SDL_SetGamepadLED(pads[p], color[p][0], color[p][1], color[p][2]);
        }
    M.pad_connected[1] = pads[1] != 0 || bind_player_has_keys(1);
}

static int is_open(SDL_Gamepad *g)
{
    for (int i = 0; i < all_count; i++)
        if (all[i] == g) return 1;
    return 0;
}

/* Players' gamepads from their choices: a named one if it is connected,
 * otherwise (auto) the first one not taken. */
static void assign(void)
{
    SDL_Gamepad *before[PLAYERS];
    for (int p = 0; p < PLAYERS; p++) before[p] = pads[p];
    for (int p = 0; p < PLAYERS; p++) {
        if (pads[p] && !is_open(pads[p])) pads[p] = NULL;
        if (!strcmp(wanted[p], "none")) pads[p] = NULL;
        else if (wanted[p][0] && (!pads[p] || strcmp(SDL_GetGamepadName(pads[p]), wanted[p]))) {
            pads[p] = NULL;
            for (int i = 0; i < all_count && !pads[p]; i++)
                if (!pad_taken(all[i], p) && !strcmp(SDL_GetGamepadName(all[i]), wanted[p]))
                    pads[p] = all[i];
        }
    }
    for (int p = 0; p < PLAYERS; p++)
        if (!pads[p] && !wanted[p][0])
            for (int i = 0; i < all_count && !pads[p]; i++)
                if (!pad_taken(all[i], p)) pads[p] = all[i];
    for (int p = 0; p < PLAYERS; p++)
        if (pads[p] != before[p])
            fprintf(stderr, "player %d: %s\n", p + 1, pads[p] ? SDL_GetGamepadName(pads[p]) : "no controller");
    mark_players();
}

static void open_gamepads(void)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < all_count;) /* drop the unplugged */
        if (!SDL_GamepadConnected(all[i])) {
            SDL_CloseGamepad(all[i]);
            all[i] = all[--all_count];
        } else {
            i++;
        }
    for (int i = 0; ids && i < n && all_count < MAX_PADS; i++) {
        /* Virtual devices (e.g. Logitech G HUB's mouse "XInput Controller",
         * vendor 046D product 0000) report no product id: skip them. */
        if (SDL_GetGamepadProductForID(ids[i]) == 0)
            continue;
        int open = 0;
        for (int k = 0; k < all_count; k++)
            if (SDL_GetGamepadID(all[k]) == ids[i]) open = 1;
        if (open) continue;
        SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
        if (g) {
            all[all_count++] = g;
            fprintf(stderr, "controller connected: %s\n", SDL_GetGamepadName(g));
        }
    }
    SDL_free(ids);
    assign();
}

/* --list-controllers: what SDL sees, then exit. */
void input_list(void)
{
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    printf("%d controller(s)\n", n);
    for (int i = 0; i < n; i++) {
        SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
        printf("  %d: %s | type %s | vendor %04X product %04X | %s\n", i + 1,
               SDL_GetGamepadNameForID(ids[i]),
               SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(ids[i])),
               SDL_GetGamepadVendorForID(ids[i]), SDL_GetGamepadProductForID(ids[i]),
               g && SDL_GetGamepadPath(g) ? SDL_GetGamepadPath(g) : "");
        if (g) SDL_CloseGamepad(g);
    }
    SDL_free(ids);
}

/* The Controllers page's Recheck: every gamepad closed and SDL's gamepad
 * system restarted, so devices it missed while running are enumerated anew. */
void input_recheck(void)
{
    for (int i = 0; i < all_count; i++) SDL_CloseGamepad(all[i]);
    all_count = 0;
    for (int p = 0; p < PLAYERS; p++) pads[p] = NULL;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    SDL_PumpEvents();
    open_gamepads();
    fprintf(stderr, "controllers: rechecked, %d connected\n", all_count);
}

void input_init(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    open_gamepads();
}

static uint16_t gamepad_bits_ex(SDL_Gamepad *g, int stick_dpad)
{
    static const struct { SDL_GamepadButton b; uint16_t bits; } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PSX_CROSS},         {SDL_GAMEPAD_BUTTON_EAST, PSX_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PSX_SQUARE},         {SDL_GAMEPAD_BUTTON_NORTH, PSX_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PSX_L1},    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PSX_R1},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PSX_UP},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PSX_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PSX_LEFT},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PSX_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, PSX_START},         {SDL_GAMEPAD_BUTTON_BACK, PSX_SELECT},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, PSX_L3},       {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PSX_R3},
    };
    uint16_t bits = 0;
    if (!g) return 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GetGamepadButton(g, map[i].b)) bits |= map[i].bits;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_THRESHOLD) bits |= PSX_L2;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_THRESHOLD) bits |= PSX_R2;
    if (!stick_dpad) return bits;
    int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (x < -STICK_DEADZONE) bits |= PSX_LEFT;
    if (x > STICK_DEADZONE) bits |= PSX_RIGHT;
    if (y < -STICK_DEADZONE) bits |= PSX_UP;
    if (y > STICK_DEADZONE) bits |= PSX_DOWN;
    return bits;
}
static uint16_t gamepad_bits(SDL_Gamepad *g) { return gamepad_bits_ex(g, 1); } /* (the left stick also as the D-pad) */
/* the touchpad held (BIND_TOUCHPAD): a PS4/PS5 pad's; on a pad without one, its Share (Capture) button */
static bool gamepad_touchpad(SDL_Gamepad *g)
{
    if (!g) return 0;
    return SDL_GamepadHasButton(g, SDL_GAMEPAD_BUTTON_TOUCHPAD) ? SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_TOUCHPAD)
                                                                : SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_MISC1);
}
uint16_t input_button_bit(int b) { return b > 0 && b <= BIND_TOUCHPAD ? button_bits[b] : 0; } /* a game button's PS1 bit */
/* an extra control's button (1..17) held on a pad (bits: its gamepad_bits) */
static bool extra_button_held(SDL_Gamepad *g, uint16_t bits, int b)
{
    if (b == BIND_TOUCHPAD) return gamepad_touchpad(g);
    return b > 0 && b <= BIND_BUTTONS_ALL && (bits & button_bits[b]);
}

/* A player's driving keys, as the PS1 buttons of their actions. */
static uint16_t action_key_bits(const bool *keys, int player)
{
    uint16_t bits = 0;
    for (int a = 0; keys && a < BIND_ACTIONS; a++) {
        int k = bind_key(player, a);
        if (k > 0 && k < SDL_SCANCODE_COUNT && keys[k]) bits |= button_bits[bind_button(player, a)];
    }
    return bits;
}

/* A PS1 pad cannot press opposite directions at once. Left and right both held: the newer one (the user's, 2026-11-21).
 * They cancelled out, so a keyboard player rolling from A to D, or a stick resting the other way from the D-pad, lost
 * the steering until the first came up: "it wouldn't listen, then hooks back in". Both pressed in one go: neither.
 * Per player, from the left/right held at the last call (the same answer when called again with the same buttons).
 * Up and down both held: neither, as before (they are the gas and reverse: no newer one to prefer). */
static uint16_t one_direction(int p, uint16_t b)
{
    static uint16_t was[PLAYERS], pick[PLAYERS];
    const uint16_t LR = PSX_LEFT | PSX_RIGHT;
    uint16_t lr = b & LR;
    if (p >= 0 && p < PLAYERS) {
        if (lr != LR) pick[p] = lr;
        else if (was[p] == PSX_LEFT) pick[p] = PSX_RIGHT;   /* (right pressed while left held: right) */
        else if (was[p] == PSX_RIGHT) pick[p] = PSX_LEFT;
        else if (was[p] == 0) pick[p] = 0;                  /* (both at once: neither) */
        was[p] = lr;
        b = (uint16_t)((b & ~LR) | pick[p]);
    } else if (lr == LR) b &= ~LR;
    if ((b & PSX_UP) && (b & PSX_DOWN)) b &= ~(PSX_UP | PSX_DOWN);
    return b;
}

/* The keyboard as the game and the menus see it: no key while Escape's "Quit?" is up (ui/options.c, its own keys) */
int input_keys_off;
static const bool *input_keys(int *n)
{
    static bool none[SDL_SCANCODE_COUNT];
    const bool *k = SDL_GetKeyboardState(n);
    return k && input_keys_off ? none : k;
}

/* Once per VBlank: fill M.pad_buttons (active-low) from the devices. */
static uint16_t player_pad_bits(int p);
void input_poll(void)
{
    static int frames;
    if (++frames % 60 == 0)
        open_gamepads(); /* pick up controllers plugged in while running */
    SDL_UpdateGamepads();
    const bool *keys = input_keys(0);
    /* next / previous song (custom music set to random or order): their keys,
     * and their buttons on player 1's controller (Page Down / Up, R3 / L3) */
    static bool song_held[BIND_SONGS];
    uint16_t pad1 = gamepad_bits(pads[0]);
    for (int i = 0; i < BIND_SONGS; i++) {
        int k = bind_extra_key(i), b = bind_extra_button(i);
        bool held = (keys && k > 0 && k < SDL_SCANCODE_COUNT && keys[k]) || extra_button_held(pads[0], pad1, b);
        if (held && !song_held[i]) cd_audio_skip(i ? -1 : 1);
        song_held[i] = held;
    }
    uint16_t p1 = player_pad_bits(0) | action_key_bits(keys, 0);
    uint16_t p2 = player_pad_bits(1) | action_key_bits(keys, 1);
    for (unsigned i = 0; keys && i < sizeof menu_keys / sizeof menu_keys[0]; i++)
        if (keys[menu_keys[i].key]) p1 |= menu_keys[i].bits;
    M.pad_connected[1] = pads[1] != 0 || bind_player_has_keys(1); /* keys can change on the Controls page */
    M.pad_buttons[0] = (uint16_t)~one_direction(0, p1);
    M.pad_buttons[1] = M.pad_connected[1] ? (uint16_t)~one_direction(1, p2) : 0xFFFF;
}

/* A player's controller in a battle: with energy attacks on the D-pad
 * (Controls), its D-pad is not steering (the stick is): input_easy_buttons. */
int feats_in_battle(void); /* profile/feat_hooks.c: a match is running */
static uint16_t player_pad_bits(int p)
{
    uint16_t bits = gamepad_bits(pads[p]);
    if (!pads[p] || !bind_easy_energy(p) || !feats_in_battle()) return bits; /* menus (car select): the D-pad steers them */
    if (bits & PSX_SELECT) return bits; /* Select held: Select + the D-pad (the HUD's buttons), as without energy attacks */
    int x = SDL_GetGamepadAxis(pads[p], SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pads[p], SDL_GAMEPAD_AXIS_LEFTY);
    bits &= (uint16_t)~(PSX_UP | PSX_DOWN | PSX_LEFT | PSX_RIGHT);
    if (x < -STICK_DEADZONE) bits |= PSX_LEFT;
    if (x > STICK_DEADZONE) bits |= PSX_RIGHT;
    if (y < -STICK_DEADZONE) bits |= PSX_UP;
    if (y > STICK_DEADZONE) bits |= PSX_DOWN;
    return bits;
}
/* Players 3 and 4 in a battle (vplus/four.c): their controller's PS1 buttons (set = held). */
uint16_t input_player_bits(int p)
{
    return p >= 0 && p < PLAYERS ? one_direction(p, player_pad_bits(p)) : 0;
}
/* How far a player steers, 0..4096 of full lock: the left stick's tilt past the deadzone (analog
 * steering on), else 4096 (the D-pad, the keyboard: full lock as the game's). */
int input_steer_amount(int p)
{
    if (p < 0 || p > 3 || !pads[p] || !bind_analog_steer(p)) return 4096;
    if (SDL_GetGamepadButton(pads[p], SDL_GAMEPAD_BUTTON_DPAD_LEFT) || SDL_GetGamepadButton(pads[p], SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        return 4096;
    int x = SDL_GetGamepadAxis(pads[p], SDL_GAMEPAD_AXIS_LEFTX);
    if (x < 0) x = -x;
    if (x <= STICK_DEADZONE) return 4096; /* not the stick: no steering from it */
    int a = (int)((x - STICK_DEADZONE) * 4096L / (32767 - STICK_DEADZONE));
    return a < 400 ? 400 : a > 4096 ? 4096 : a;
}
uint16_t input_easy_buttons(int p) /* the D-pad of a player with energy attacks on it (PSX_UP.. bits; 0x100 invisibility) */
{
    if (p < 0 || p > 3 || !bind_easy_energy(p)) return 0;
    uint16_t pad = pads[p] ? gamepad_bits(pads[p]) : 0;
    if (pad & PSX_SELECT) return 0; /* Select held: the D-pad is for Select's buttons */
    uint16_t bits = 0;
    /* each attack's button (by default the D-pad's direction: bind_energy_button) and key (Controls) */
    static const uint16_t as[BIND_ENERGY] = {PSX_UP, PSX_DOWN, PSX_LEFT, PSX_RIGHT, 0x100};
    static const SDL_GamepadButton dpad[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                              SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    const bool *keys = input_keys(0);
    for (int i = 0; i < BIND_ENERGY; i++) {
        int b = bind_energy_button(p, i), k = bind_energy_key(p, i);
        int held = b >= 5 && b <= 8 ? pads[p] && SDL_GetGamepadButton(pads[p], dpad[b - 5]) /* (the D-pad itself, not the stick) */
                                    : b && (pad & button_bits[b]);
        if (held || (keys && k > 0 && k < SDL_SCANCODE_COUNT && keys[k])) bits |= as[i];
    }
    return bits;
}

/* For the port's own menus (src/ui): the PS1 buttons held on the keyboard
 * and every gamepad (PSX_* bits as in the enum above, set = held). *menu_key
 * is set while the menu key (F1) is held; in a battle the pause menu's
 * Options is the controller's way. Called while the game is stopped, so it reads the devices itself. */
static int menu_player; /* whose controller the port's menus read */
void input_set_menu_player(int player) { menu_player = player == 1 ? 1 : 0; }
static int last_menu_pad = -1; /* the gamepad that last pressed a button in the port's menus */
int input_last_menu_controller(void) { return last_menu_pad < all_count ? last_menu_pad : -1; }

uint16_t input_menu_buttons(int *menu_key)
{
    static int calls;
    if (++calls % 60 == 0) open_gamepads(); /* plugged in while a menu is up (as input_poll in the game) */
    SDL_UpdateGamepads();
    const bool *keys = input_keys(0);
    uint16_t bits = 0;
    static uint16_t held[MAX_PADS];
    *menu_key = keys && keys[SDL_SCANCODE_F1];
    for (int i = 0; i < all_count; i++) { /* only the menu player's controller (player 1's; player 2's in a pause they opened) */
        if (all[i] != pads[menu_player] && pads[menu_player]) continue;
        uint16_t g = gamepad_bits(all[i]);
        if (g & ~held[i]) last_menu_pad = i;
        held[i] = g;
        bits |= g;
    }
    for (unsigned i = 0; keys && i < sizeof menu_keys / sizeof menu_keys[0]; i++)
        if (keys[menu_keys[i].key]) bits |= menu_keys[i].bits;
    for (int a = 0; keys && a < BIND_ACTIONS; a++) { /* Triangle: the key of the action on Triangle in a battle (player 1's
                                                     * Controls; by default Left Ctrl), not a key of its own (the user's
                                                     * online playtest 2026-11-01: the lobby's car select out of reach) */
        int k = bind_key(0, a);
        if (button_bits[bind_button(0, a)] == PSX_TRIANGLE && k > 0 && k < SDL_SCANCODE_COUNT && keys[k]) bits |= PSX_TRIANGLE;
    }
    return bits;
}

/* an extra control (bindings.h) held: its key, or its button on player 1's controller */
static bool input_extra_held(int i, const bool *keys, uint16_t pad1)
{
    int k = bind_extra_key(i), b = bind_extra_button(i);
    return (keys && k > 0 && k < SDL_SCANCODE_COUNT && keys[k]) || extra_button_held(pads[0], pad1, b);
}
/* push to talk (online voice chat): held now (each frame in a battle; the menus read it themselves) */
int input_ptt_held(void)
{
    static long long test = -2; /* tests: TM2PC_TEST_PTT=V, held from VBlank V on */
    if (test == -2) test = getenv("TM2PC_TEST_PTT") ? atoll(getenv("TM2PC_TEST_PTT")) : -1;
    if (test >= 0 && M.vblanks >= (uint64_t)test) return 1;
    SDL_UpdateGamepads();
    return input_extra_held(BIND_PTT, input_keys(0), gamepad_bits(pads[0]));
}
/* The scoreboard in a battle: its key (Tab by default) or its button (the touchpad by default; player 1's Controls) held
 * on any controller (each player's view shows it). */
int input_scoreboard_held(void)
{
    static long long test = -2; /* tests: TM2PC_TEST_SCOREBOARD=V, held from VBlank V on */
    if (test == -2) test = getenv("TM2PC_TEST_SCOREBOARD") ? atoll(getenv("TM2PC_TEST_SCOREBOARD")) : -1;
    if (test >= 0 && M.vblanks >= (uint64_t)test) return 1;
    SDL_UpdateGamepads();
    const bool *keys = input_keys(0);
    int k = bind_extra_key(BIND_SCOREBOARD), b = bind_extra_button(BIND_SCOREBOARD);
    if (keys && k > 0 && k < SDL_SCANCODE_COUNT && keys[k]) return 1;
    for (int i = 0; b && i < all_count; i++)
        if (extra_button_held(all[i], b == BIND_TOUCHPAD ? 0 : gamepad_bits_ex(all[i], 0), b)) return 1;
    return 0;
}

/* Any gamepad's touchpad held (the Controls page's capture: not a PS1 button) */
int input_pad_touchpad(void)
{
    for (int i = 0; i < all_count; i++)
        if (gamepad_touchpad(all[i])) return 1;
    return 0;
}

/* ---- for the Controls pages ---- */

/* The PS1 buttons held on the gamepads only (no keyboard). */
uint16_t input_pad_buttons(void)
{
    SDL_UpdateGamepads();
    uint16_t bits = 0;
    for (int i = 0; i < all_count; i++) bits |= gamepad_bits(all[i]);
    return bits;
}

/* A key pressed since the last call (its scancode), or 0. F1, Escape and
 * the F-keys do not count. */
int input_new_key(void)
{
    static bool before[SDL_SCANCODE_COUNT];
    int n = 0, found = 0;
    const bool *keys = input_keys(&n);
    for (int k = 1; keys && k < n && k < SDL_SCANCODE_COUNT; k++) {
        if (keys[k] && !before[k] && !found && k != SDL_SCANCODE_ESCAPE &&
            !(k >= SDL_SCANCODE_F1 && k <= SDL_SCANCODE_F12))
            found = k;
        before[k] = keys[k];
    }
    return found;
}

int input_controller_count(void) { return all_count; }

/* One controller's PS1 buttons (the split-screen sign-in reads each). */
uint16_t input_controller_buttons(int i)
{
    SDL_UpdateGamepads();
    return i >= 0 && i < all_count ? gamepad_bits(all[i]) : 0;
}

/* A player's controller's buttons only (no keyboard). */
/* photo mode's controller: the player's, else the one that last pressed a button in a menu (the pause menu that
 * opened photo mode), else the first */
static SDL_Gamepad *photo_pad(int player)
{
    if (player >= 0 && player < PLAYERS && pads[player]) return pads[player];
    if (last_menu_pad >= 0 && last_menu_pad < all_count) return all[last_menu_pad];
    return all_count ? all[0] : NULL;
}
void input_player_axes(int player, int ax[6]) /* photo mode: LX LY RX RY LT RT of a player's controller */
{
    static const SDL_GamepadAxis axes[6] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                            SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
    /* every controller (as photo mode's buttons): each axis the one pushed furthest. Player 1's alone read nothing for
     * the user (the controller showing up twice, its sticks on the other) */
    SDL_UpdateGamepads();
    (void)photo_pad;
    for (int i = 0; i < 6; i++) {
        int best = 0;
        for (int k = 0; k < all_count; k++) {
            int v = SDL_GetGamepadAxis(all[k], axes[i]);
            if (abs(v) > abs(best)) best = v;
        }
        ax[i] = best;
    }
    (void)player;
    const char *t = getenv("TM2PC_TEST_AXES"); /* tests: "LX,LY,RX,RY" held */
    if (t) sscanf(t, "%d,%d,%d,%d", &ax[0], &ax[1], &ax[2], &ax[3]);
}
/* photo mode's buttons: the controller's real D-pad (the left stick is not folded into it: the sticks move the
 * camera) and for player 1 the keyboard's menu keys */
uint16_t input_player_dpad_buttons(int player)
{
    SDL_UpdateGamepads();
    uint16_t bits = 0;
    for (int k = 0; k < all_count; k++) bits |= gamepad_bits_ex(all[k], 0); /* (every controller, as the buttons) */
    const bool *keys = input_keys(0);
    for (unsigned i = 0; player == 0 && keys && i < sizeof menu_keys / sizeof menu_keys[0]; i++)
        if (keys[menu_keys[i].key]) bits |= menu_keys[i].bits;
    return bits;
}
uint16_t input_player_pad_buttons(int player)
{
    SDL_UpdateGamepads();
    return player >= 0 && player < PLAYERS ? gamepad_bits(pads[player]) : 0;
}

/* A player's menu buttons: their controller, and for player 1 the keyboard's
 * menu keys too. */
uint16_t input_player_menu_buttons(int player)
{
    SDL_UpdateGamepads();
    uint16_t bits = player >= 0 && player < PLAYERS ? gamepad_bits(pads[player]) : 0;
    const bool *keys = input_keys(0);
    for (unsigned i = 0; player == 0 && keys && i < sizeof menu_keys / sizeof menu_keys[0]; i++)
        if (keys[menu_keys[i].key]) bits |= menu_keys[i].bits;
    return bits;
}

/* What a player's controller is: 1 PlayStation, 2 Xbox, 0 other or none. */
int input_player_pad_kind(int player)
{
    if (player < 0 || player >= PLAYERS || !pads[player]) return 0;
    switch (SDL_GetGamepadType(pads[player])) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 1;
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return 2;
    default: return 0;
    }
}
const char *input_controller_name(int i) { return i >= 0 && i < all_count ? SDL_GetGamepadName(all[i]) : ""; }

/* Something on the gamepad is pressed (for spotting which one it is). */
int input_controller_active(int i)
{
    if (i < 0 || i >= all_count) return 0;
    if (gamepad_bits(all[i])) return 1;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(all[i], (SDL_GamepadButton)b)) return 1;
    return 0;
}

/* The player (0, 1) a gamepad is, or -1; the gamepad a player has, or -1. */
int input_controller_player(int i)
{
    for (int p = 0; p < PLAYERS; p++)
        if (i >= 0 && i < all_count && pads[p] == all[i]) return p;
    return -1;
}

int input_player_controller(int player)
{
    if (player < 0 || player >= PLAYERS) return -1;
    for (int i = 0; i < all_count; i++)
        if (pads[player] == all[i]) return i;
    return -1;
}

/* Gives a player a gamepad (-1: none). If the other player had it, they
 * swap. */
void input_set_player_controller(int player, int i)
{
    if (player < 0 || player >= PLAYERS) return;
    SDL_Gamepad *g = i >= 0 && i < all_count ? all[i] : NULL;
    for (int q = 0; q < PLAYERS; q++)
        if (g && q != player && pads[q] == g) { /* they swap */
            pads[q] = pads[player];
            snprintf(wanted[q], sizeof wanted[0], "%s", pads[q] ? SDL_GetGamepadName(pads[q]) : "none");
        }
    pads[player] = g;
    snprintf(wanted[player], sizeof wanted[0], "%s", g ? SDL_GetGamepadName(g) : "none");
    mark_players();
}

/* [controls] player1_controller / player2_controller (settings.c): auto,
 * none or a gamepad's name. */
const char *input_wanted(int player) { return wanted[player][0] ? wanted[player] : "auto"; }
void input_set_wanted(int player, const char *name)
{
    if (player < 0 || player >= PLAYERS) return;
    snprintf(wanted[player], sizeof wanted[0], "%s", strcmp(name, "auto") ? name : "");
}
