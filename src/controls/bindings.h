/* Button bindings per action: each player's choice on top of the game's
 * controller layout (Keypad 1 and 2). See bindings.c. */
#pragma once

#define BIND_ACTIONS 12
#define BIND_BUTTONS 14   /* the game's button numbers 1..14 */
#define BIND_BUTTONS_ALL 16 /* and L3 (15), R3 (16): only for the song keys, the game has no use for them */

/* The rebindable actions, in the order the Controls page lists them. */
extern const char *const bind_action_names[BIND_ACTIONS];
extern const char *const bind_button_names[BIND_BUTTONS_ALL + 1];      /* [1..16]: PlayStation */
extern const char *const bind_button_names_xbox[BIND_BUTTONS_ALL + 1]; /* the same buttons on an Xbox pad */

/* The button (1..14) an action has in one of the game's four layouts. */
int bind_layout_button(int layout, int action);

/* The button an action has for a player: the player's own choice, or the
 * layout's. */
int bind_button(int player, int action);
/* Sets a player's button for an action (0: back to the layout's). */
void bind_set(int player, int action, int button);
/* Drops a player's own buttons (when the layout changes); the keyboard keys
 * are the player's own, apart from the layouts. */
void bind_reset(int player);
/* Whether a player has controller buttons of their own (the Controls page
 * shows the layout as Custom). */
int bind_is_custom(int player);

/* The keyboard key (SDL scancode, 0: none) a player has for an action;
 * setting one takes it from any other action that had it. */
int bind_key(int player, int action);
void bind_set_key(int player, int action, int scancode);
int bind_player_has_keys(int player);
const char *bind_key_name(int scancode); /* for the Controls page */

/* Next and previous song (custom music, music.c): a controller button and a
 * key, for player 1. */
#define BIND_SONGS 2   /* the extra controls: next and previous song (custom music), */
#define BIND_PTT 2     /* push to talk (online voice chat, held) */
#define BIND_EXTRAS 3
extern const char *const bind_extra_names[BIND_EXTRAS];
int bind_extra_button(int i);
void bind_set_extra_button(int i, int button);
int bind_extra_key(int i);
void bind_set_extra_key(int i, int scancode);

/* Each player's energy attacks on buttons of their own (the user's, 2026-10-28): a controller button (1..16, 0
 * none) and, for players 1-2, a key; they work with "Easy Energy Attacks" on (input_easy_buttons). */
#define BIND_ENERGY 5   /* freeze, rear fire, mines, shield, invisibility */
extern const char *const bind_energy_names[BIND_ENERGY];
int bind_energy_button(int player, int i);
void bind_set_energy_button(int player, int i, int button);
int bind_energy_key(int player, int i);
void bind_set_energy_key(int player, int i, int scancode);

/* Each player's controller type for the Controls page's button names:
 * 0 auto (from the connected controller), 1 PlayStation, 2 Xbox. */
int bind_pad_type(int player);
void bind_set_pad_type(int player, int type);

/* Each player's L1 + R1 jump and D-pad energy attacks (Up freeze, Down rear
 * fire, Left mines, Right shield; steering then on the stick only). */
int bind_jump_combo(int player);
void bind_set_jump_combo(int player, int on);
int bind_easy_energy(int player);
int bind_analog_steer(int player);   /* the stick turns as far as it is tilted (on by default) */
void bind_set_analog_steer(int player, int on);
int bind_steer_keep(int player);      /* in reverse, Left still turns the nose left (decomp request 66; off: retail) */
void bind_set_steer_keep(int player, int on);
void bind_set_easy_energy(int player, int on);

/* [controls] keys for the bindings and the players' controllers (settings.c):
 * player 2's bindings; player 1's p1_ keys of an older file are still read. */
int bind_ini_key(const char *key, const char *val);
void bind_ini_write(FILE *f);

/* Player 1's controls in the profile file (profile.c): layout, buttons,
 * keys and the song keys; bind_player1_defaults before reading a profile. */
void bind_player1_defaults(void);
int bind_profile_key(const char *key, const char *val);
void bind_profile_write(FILE *f);
/* Player 2's from a profile: layout and controller buttons (no keys). */
void bind_player2_defaults(void);
void bind_player_defaults(int p);                           /* players 3-4 (p 2-3) */
int bind_profile_key_p(int p, const char *key, const char *val);
int bind_layout(int player);                                /* the keypad layout 0-3 */
void bind_apply_as(int player);                             /* -1, or a player's buttons in the next layout apply */
int bind_profile_key_p2(const char *key, const char *val);
void bind_profile_write_p2(FILE *f);
