/* Every human's input, one path for every player count, offline and online (decomp HUMANS16_PLAN.md H1-H2).
 *
 * With portHumanOptions bit 0 (INPUT_ALL) every human car reads its own input word (portHumanPad[h], padButtonsDecoded's
 * half layout, active high) through its own button table (portHumanBindings[h]); no human reads a pad port. Each game
 * step (hle_SetFrameStart) this writes every human's word and stick tilt (portHumanSteer[h]): a human of this machine's
 * from its local player's controller (slot L: the PC's player L+1), another machine's from the network; both through
 * the kill cam's input seam (killcam_input: the replays, and online every machine's agreed inputs). The map of which
 * human is whose (portHumanLocal: 1 + this machine's slot, or -1) is set for a match (humans_set_map; offline: every
 * human local, slot = human). Each human's button table: a local player's from their layout and own buttons (as the
 * game builds a pad's), another machine's as that machine sent it (humans_set_table). */
#include "../runtime/runtime.h"
#include "vplus.h"
#include "cars.h"
#include "../controls/bindings.h"
#include <stdlib.h>
#include <string.h>

enum { HUMANS_MAX = 16, OPT_INPUT_ALL = 1, OPT_NO_CHEATS = 8, OPT_RADAR_AI = 16 }; /* (portHumanOptions' bits) */
#define PAD_ACTIVE GA(0x801A4BECu)       /* padActiveBindings[2][128] */
#define PAD_CONFIGURED GA(0x801A4CECu)   /* padConfiguredBindings[2][128] */
#define HUMAN_TOTAL GA(0x80181014u)      /* humanCarTotal */

uint32_t killcam_input(int kind, int player, uint32_t live); /* vplus/killcam.c */
uint16_t input_player_bits(int p);                          /* runtime/input.c */
int input_steer_amount(int p);
uint16_t input_easy_buttons(int p);
int bind_layout(int player);                                /* controls/bindings.c */
int bind_jump_combo(int player), bind_steer_keep(int player); /* controls/bindings.c */
void bind_apply_as(int player);
int feats_in_battle(void);

static uint32_t a_local, a_pad, a_steer, a_options, a_layout, a_bindings, a_control;
static int8_t slot_of[HUMANS_MAX];      /* this machine's slot of each human (-1: another machine's) */
static int map_set, map_n;              /* (a match's map given; else every human local) */
static uint8_t table[HUMANS_MAX][128];  /* each human's button table for the match */
static int8_t table_given[HUMANS_MAX];  /* (another machine's, sent) */
static int tables_n;                   /* (this battle's humans whose tables are written: a player dropping in, theirs then) */
static uint16_t easy[HUMANS_MAX];       /* each human's easy energy buttons this step (vp_MoveCodes) */

static int syms(void)
{
    static int done;
    if (!done) {
        done = 1;
        a_local = port_sym("portHumanLocal");
        a_pad = port_sym("portHumanPad");
        a_steer = port_sym("portHumanSteer");
        a_options = port_sym("portHumanOptions");
        a_layout = port_sym("portHumanLayout");
        a_bindings = port_sym("portHumanBindings");
        a_control = port_sym("portHumanControl"); /* (decomp request 66, ANALOG_STEERING.md: bit 0, the turn's sign kept in reverse) */
    }
    return a_local && a_pad && a_options && a_bindings;
}
int humans_on(void) { return syms(); } /* (the build has the decomp's humans: every human's input through here) */

void humans_set_map(int n, const int8_t *slots) /* a match's map (online); n 0: none (offline: every human local) */
{
    map_set = n > 0;
    map_n = n;
    for (int h = 0; h < HUMANS_MAX; h++) slot_of[h] = map_set && h < n ? slots[h] : (int8_t)(h < 4 ? h : -1);
    if (syms()) { /* online: every player's radar the computer cars too (retail: player 1's only), no cheat codes */
        uint32_t o = mem_r32(&M.cpu, a_options) & ~(uint32_t)(OPT_NO_CHEATS | OPT_RADAR_AI);
        mem_w32(&M.cpu, a_options, o | (map_set ? OPT_NO_CHEATS | OPT_RADAR_AI : 0));
    }
    if (syms()) /* (in place before the match loads: its binding sets up only this machine's players' views) */
        for (int h = 0; h < HUMANS_MAX; h++)
            mem_w8(&M.cpu, a_local + (uint32_t)h, (uint8_t)(!map_set ? 0 : h < n && slot_of[h] >= 0 ? 1 + slot_of[h] : 0xFF));
}
void humans_local_put(int h, int on) /* human h's machine in the decomp's table again (on 0: another's, as a vacant
                                     * record's) */
{
    if (syms() && h >= 0 && h < HUMANS_MAX)
        mem_w8(&M.cpu, a_local + (uint32_t)h, (uint8_t)(!on ? 0xFF : !map_set ? 0 : h < map_n && slot_of[h] >= 0 ? 1 + slot_of[h] : 0xFF));
}
void humans_set_table(int h, const uint8_t t[128]);
void humans_tables_again(void) { tables_n = 0; } /* (a state from the host in place: its memory has the host's tables;
                                                  * every one written again, as every other machine has them) */
void humans_add(int h, int slot, const uint8_t t[128]) /* online: human h dropping into the match on (slot: this
                                                        * machine's, -1 another's with its button table t) */
{
    if (h < 0 || h >= HUMANS_MAX) return;
    if (map_set && h >= map_n) {
        for (int k = map_n; k < h; k++) slot_of[k] = -1;
        map_n = h + 1;
    }
    slot_of[h] = (int8_t)slot;
    if (slot < 0 && t) humans_set_table(h, t);
}
int humans_local_count(void) /* a match's players on this machine (online); -1: every human local (offline) */
{
    if (!map_set) return -1;
    int c = 0;
    for (int h = 0; h < map_n && h < HUMANS_MAX; h++) c += slot_of[h] >= 0;
    return c;
}
int humans_slot(int h) /* this machine's slot of human h (-1: another machine's); offline every human local */
{
    if (h < 0 || h >= HUMANS_MAX) return -1;
    return map_set ? slot_of[h] : h < 4 ? h : -1;
}
int humans_of_slot(int L)
{
    for (int h = 0; h < HUMANS_MAX; h++) if (humans_slot(h) == L) return h;
    return -1;
}
int humans_view_human(int view) { int h = humans_of_slot(view); return h < 0 ? 0 : h; }
void humans_set_table(int h, const uint8_t t[128]) /* another machine's player's button table (online): not used any more,
                                                   * every human plays with the bare Standard table (humans_frame) */
{
    (void)h;
    (void)t;
}
void humans_clear_tables(void) { memset(table_given, 0, sizeof table_given); }
/* local player L's button table: the game's for pad 0 as their layout and own buttons make it (padSetLayout 801206F4
 * with their buttons applied, then pad 0's put back) */
void humans_local_table(int L, uint8_t out[128])
{
    uint8_t act[128], conf[128];
    for (uint32_t i = 0; i < 128; i++) { act[i] = mem_r8(&M.cpu, PAD_ACTIVE + i); conf[i] = mem_r8(&M.cpu, PAD_CONFIGURED + i); }
    bind_apply_as(L);
    guest_call(GA(0x801206F4u), (uint32_t)bind_layout(L), 0, 0, 0);
    bind_apply_as(-1);
    for (uint32_t i = 0; i < 128; i++) out[i] = mem_r8(&M.cpu, PAD_ACTIVE + i);
    for (uint32_t i = 0; i < 128; i++) { mem_w8(&M.cpu, PAD_ACTIVE + i, act[i]); mem_w8(&M.cpu, PAD_CONFIGURED + i, conf[i]); }
}

/* a local player's buttons as the game's pad would have them (active low): pads 1-2 as the VBlank made them (the tests'
 * scripts in), 3-4 from their controllers */
static uint16_t local_pad(int L)
{
    if (L < 2) return M.pad_buttons[L];
    uint16_t b = (uint16_t)~input_player_bits(L);
    static int test_read;
    static uint16_t test_bits;
    if (!test_read) { test_read = 1; if (getenv("TM2PC_TEST_P34")) test_bits = (uint16_t)strtoul(getenv("TM2PC_TEST_P34"), 0, 16); }
    b &= (uint16_t)~test_bits; /* tests: players 3-4 hold these (active high bits) */
    uint16_t pad_script_bits(int p); /* runtime/hle.c: tests, TM2PC_PAD3_SCRIPT / PAD4 */
    b &= (uint16_t)~pad_script_bits(L);
    return b;
}

/* Every human plays with one table, the Standard layout's alone, on every machine; a player's own layout and buttons
 * are used only on their own machine, to turn the buttons they hold into the ones Standard has for the same actions
 * (the players' 2026-11-23 idea: send what a press does, not which button). Before, each machine read every player's
 * buttons through that player's own table, copied to it through the lobby: two copies that differed parted the games
 * (Enough Boxes and Tegu: a re-sync every 8 steps on Roof Tops and Cyburbia), and a table could not change during a
 * match. A player on Standard with no buttons of their own sends their buttons as they are. */
static const uint16_t button_bit[17] = { /* padButtonHeld's buttons 1..14 (and the port's L3, R3 15-16): PS1 bits */
    0, 0x0001, 0x0008, 0x0400, 0x0100, 0x0010, 0x0040, 0x0080, 0x0020, 0x0800, 0x0200, 0x1000, 0x4000, 0x8000, 0x2000,
    0x0002, 0x0004};
static uint8_t bare[128], own[4][128];
static uint16_t bare_used, own_used[4];
static int bare_ok, own_ok[4];
/* the actions a human's word is read for (humanCarReadInput's ACTION(), decomp car_input.c; carReadMoveCodes' D-pad
 * codes are 0x16-0x1B): only these turned. The table's other entries are the menus' (several share a button with
 * these, Square both accelerate and a menu's: turned too, a moved accelerate's old button still drove) */
static const uint8_t car_actions[] = {0x08, 0x09, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21,
                                      0x31, 0x39, 0x3A, 0x3B, 0x3C};
enum { CAR_ACTIONS = sizeof car_actions };
static uint16_t buttons_used(const uint8_t t[128])
{
    uint16_t u = 0;
    for (int i = 0; i < CAR_ACTIONS; i++) {
        int b = t[car_actions[i]];
        if (b >= 1 && b <= 16) u |= button_bit[b];
    }
    return u;
}
static void bare_table(uint8_t out[128]) /* the Standard layout's table, no one's own buttons */
{
    uint8_t act[128], conf[128];
    for (uint32_t i = 0; i < 128; i++) { act[i] = mem_r8(&M.cpu, PAD_ACTIVE + i); conf[i] = mem_r8(&M.cpu, PAD_CONFIGURED + i); }
    bind_apply_as(BIND_APPLY_BARE);
    guest_call(GA(0x801206F4u), 0, 0, 0, 0);
    bind_apply_as(-1);
    for (uint32_t i = 0; i < 128; i++) out[i] = mem_r8(&M.cpu, PAD_ACTIVE + i);
    for (uint32_t i = 0; i < 128; i++) { mem_w8(&M.cpu, PAD_ACTIVE + i, act[i]); mem_w8(&M.cpu, PAD_CONFIGURED + i, conf[i]); }
}
/* local player L's buttons held (PS1 bits, set = held) as Standard's: each action they hold, its Standard button; a
 * button no action uses (in their table or Standard's) as it is */
static uint16_t as_standard(int L, uint16_t mine)
{
    if (L < 0 || L > 3 || !own_ok[L] || !bare_ok) return mine;
    uint16_t out = (uint16_t)(mine & ~(own_used[L] | bare_used));
    for (int i = 0; i < CAR_ACTIONS; i++) {
        int a = car_actions[i], b = own[L][a], s = bare[a];
        if (b >= 1 && b <= 16 && (mine & button_bit[b]) && s >= 1 && s <= 16) out |= button_bit[s];
    }
    return out;
}
void humans_controls_changed(void) { memset(own_ok, 0, sizeof own_ok); } /* (Controls: at once, from the next step) */

void humans_frame(void) /* each step (hle_SetFrameStart, after the kill cam's frame): every human's word and tilt */
{
    int battle = feats_in_battle();
    if (!battle) { tables_n = 0; memset(own_ok, 0, sizeof own_ok); return; } /* (each battle: its players' own tables again) */
    if (!syms()) return;
    int n = (int)mem_r32(&M.cpu, HUMAN_TOTAL);
    if (n < 0) n = 0;
    if (n > HUMANS_MAX) n = HUMANS_MAX;
    mem_w32(&M.cpu, a_options, mem_r32(&M.cpu, a_options) | OPT_INPUT_ALL);
    for (int h = 0; h < HUMANS_MAX; h++) /* (1 + slot: this machine's; -1 another's or none) */
        mem_w8(&M.cpu, a_local + (uint32_t)h, (uint8_t)(h < n && humans_slot(h) >= 0 ? 1 + humans_slot(h) : 0xFF));
    if (!bare_ok) { /* the one table every human plays with: the Standard layout alone */
        bare_table(bare);
        bare_used = buttons_used(bare);
        bare_ok = 1;
    }
    for (int L = 0; L < 4; L++) /* this PC's players' own tables (to turn their buttons into actions); again after a
                                 * change in Controls (humans_controls_changed: at once, the next step) */
        if (!own_ok[L] && humans_of_slot(L) >= 0 && humans_of_slot(L) < n) {
            humans_local_table(L, own[L]);
            own_used[L] = buttons_used(own[L]);
            own_ok[L] = 1;
        }
    if (tables_n < n) { /* the button tables, once a battle (after sessionRun built them from the layouts); a player
                         * dropping in, theirs when their record is there: every human the bare Standard table, on every
                         * machine (each player's own buttons are turned into it on their own machine, below) */
        int from = tables_n;
        tables_n = n;
        for (int h = from; h < n; h++) {
            memcpy(table[h], bare, 128);
            table_given[h] = 1;
            for (uint32_t i = 0; i < 128; i++) mem_w8(&M.cpu, a_bindings + (uint32_t)h * 128 + i, table[h][i]);
        }
    }
    /* the C builds a human's table from the layout again when the human moves into another car (a takeover: the
     * Juggernaut's forced car; a rejoin): the player's own buttons were lost until the next match (the user's L2 / R2,
     * 2026-10-29). Each step, a table that is not ours is ours again */
    for (int h = 0; h < n && h < tables_n; h++) {
        if (!table_given[h]) continue;
        uint32_t row = a_bindings + (uint32_t)h * 128, i = 0;
        while (i < 128 && mem_r8(&M.cpu, row + i) == table[h][i]) i++;
        if (i == 128) continue;
        for (i = 0; i < 128; i++) mem_w8(&M.cpu, row + i, table[h][i]);
        static int told;
        if (told++ < 20) fprintf(stderr, "controls: human %d's buttons put back (the game had rebuilt them from the layout)\n", h + 1);
    }
    for (int h = 0; h < n; h++) {
        int L = humans_slot(h);
        uint16_t mine = L >= 0 ? (uint16_t)~local_pad(L) : 0; /* (this PC's player: their buttons, set = held) */
        uint16_t raw = (uint16_t)killcam_input(0, h, L >= 0 ? (uint16_t)~as_standard(L, mine) : 0xFFFFu); /* (active low) */
        uint16_t held = (uint16_t)~raw;
        mem_w16(&M.cpu, a_pad + 2u * (uint32_t)h, (uint16_t)((held >> 8) | (held << 8))); /* padButtonsDecoded's half layout */
        uint32_t steer = killcam_input(1, h, L >= 0 ? (uint32_t)input_steer_amount(L) : 4096u);
        if (a_steer) mem_w16(&M.cpu, a_steer + 2u * (uint32_t)h, (uint16_t)steer);
        /* the easy energy buttons, and bit 0 the player's L1 + R1 jump, bit 1 their steering in reverse "like the screen",
         * bits 2-3 their jump's weapon switch safety (0 off, 1-3 steps), and the controller's own Select, L1 and R1 held
         * (EASY_SELECT, EASY_L1, EASY_R1: the jump and Select's buttons are those, whatever actions they have) (their
         * controls' choices, in the input: every machine and every re-run the same) */
        int bind_jump_safety(int player);
        easy[h] = (uint16_t)killcam_input(2, h, L >= 0 ? (uint32_t)(input_easy_buttons(L) | (bind_jump_combo(L) ? 1 : 0) |
                                                                    (bind_steer_keep(L) ? 2 : 0) |
                                                                    (uint32_t)(bind_jump_safety(L) & 3) << 2 |
                                                                    (mine & 0x0001 ? EASY_SELECT : 0) |
                                                                    (mine & 0x0400 ? EASY_L1 : 0) |
                                                                    (mine & 0x0800 ? EASY_R1 : 0)) : 0);
        if (a_control) mem_w8(&M.cpu, a_control + (uint32_t)h, (uint8_t)((mem_r8(&M.cpu, a_control + (uint32_t)h) & ~1u) | ((easy[h] >> 1) & 1)));
    }
}
int humans_steer(int h) /* human h's stick tilt this step (0..4096) */
{
    if (!syms() || !a_steer || h < 0 || h >= HUMANS_MAX) return 4096;
    int s = (int16_t)mem_r16(&M.cpu, a_steer + 2u * (uint32_t)h);
    return s <= 0 || s > 4096 ? 4096 : s;
}
uint16_t humans_easy(int h) { return h >= 0 && h < HUMANS_MAX ? easy[h] : 0; }
/* every human's input of a step as it was written (net/rb_state.c: a step re-run with the same input): the input
 * words and stick tilts in guest RAM, the easy energy buttons */
enum { INPUTS_BYTES = HUMANS_MAX * 2 * 3 };
size_t humans_inputs_size(void) { return INPUTS_BYTES; }
void humans_rb_register(void) /* (net/rb_state.c) a rewind past a newcomer's step writes the tables again */
{
    void rbs_keep(void *p, size_t n);
    rbs_keep(&tables_n, sizeof tables_n);
}
void humans_inputs_get(uint8_t *out)
{
    if (!syms()) { memset(out, 0, INPUTS_BYTES); return; }
    for (uint32_t i = 0; i < HUMANS_MAX * 2; i++) {
        out[i] = (uint8_t)mem_r8(&M.cpu, a_pad + i);
        out[HUMANS_MAX * 2 + i] = a_steer ? (uint8_t)mem_r8(&M.cpu, a_steer + i) : 0;
    }
    memcpy(out + HUMANS_MAX * 4, easy, sizeof easy);
}
void humans_inputs_put(const uint8_t *in)
{
    if (!syms()) return;
    for (uint32_t i = 0; i < HUMANS_MAX * 2; i++) {
        mem_w8(&M.cpu, a_pad + i, in[i]);
        if (a_steer) mem_w8(&M.cpu, a_steer + i, in[HUMANS_MAX * 2 + i]);
    }
    memcpy(easy, in + HUMANS_MAX * 4, sizeof easy);
}
uint16_t humans_held(int h) /* human h's buttons this step (PS1 bits, set = held) */
{
    if (!syms() || h < 0 || h >= HUMANS_MAX) return 0;
    uint16_t w = mem_r16(&M.cpu, a_pad + 2u * (uint32_t)h);
    return (uint16_t)((w >> 8) | (w << 8));
}
