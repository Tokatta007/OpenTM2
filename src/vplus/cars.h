/* The cars of a match: how many there can be, and their records, in one place (requests 32-33 to the decomp:
 * up to 16 cars, any mix, 16 of one car included).
 *
 * CARS_MAX sizes every array: no match ever holds more. The records the build has are fewer today: the game's
 * layout (4 human records in the port's variant, 9 computer cars); decomp request 32 step 2 (TM2_PORT_CARS=16)
 * raises the computer cars, and a later request the humans (16 online). Loops over computer cars go to
 * cars_ai_count() (aiCarTotal, bounded by the records there are), never to a number. */
#pragma once
#include <stdint.h>

enum { CARS_MAX = 16 };       /* the most cars a match can hold, humans and computer cars together */
enum { CARS_HUMANS_MAX = 16 }; /* human records (the variant's TM2_MAX_HUMANS=16: every player of a match, local or another machine's) */

uint32_t car_slot_object(int slot); /* carSlotObjects 80187AD4: a car slot's world object, 0 out of range */
int cars_ai_max(void);   /* computer-car records the build has (9 today; 12 or more with request 32 step 2) */
int cars_ai_count(void); /* the match's computer cars: aiCarTotal, at most cars_ai_max() */
int cars_arena_room(int level);  /* cars an arena holds, players included (the game's limits today; 16 after step 2) */
int cars_arena_types(int level); /* different car types whose textures fit an arena's VRAM, players' included */
int cars_duplicates(void);       /* two cars of one type can play (decomp request 32 step 1 in the build) */
int cars_banks(void);            /* texture banks: every car its own textures and CLUTs (decomp request 32 step 4) */
int cars_arena_types_vram(int level); /* the types that fit the arena's VRAM itself (no banks) */

#define HUMAN_REC_SIZE 0x814u
#define AI_REC_SIZE 0x854u
#define HUMAN_REC(i) (RB_humanCars + (uint32_t)(i) * HUMAN_REC_SIZE)
int humans_view_human(int view); /* vplus/humans.c: the human a view (this machine's player) shows */
#define VIEW_REC(v) HUMAN_REC(humans_view_human(v)) /* the record of the human a view shows (offline: view = human) */
/* a word of a human record with its camera and view bytes zeroed: each machine runs its own players' cameras (decomp
 * HUMANS16_PLAN.md 7: +0x006..7, +0x00C..+0x08F, +0x10C..+0x113): not game state (the state hashes, online's check) */
static inline uint32_t human_rec_state_word(uint32_t off, uint32_t w)
{
    for (uint32_t b = 0; b < 4; b++) {
        uint32_t o = off + b;
        if ((o >= 0x006 && o < 0x008) || (o >= 0x00C && o < 0x090) || (o >= 0x10C && o < 0x114)) w &= ~(0xFFu << (8 * b));
    }
    return w;
}
#define AI_REC(i) (GA(0x80188B5Cu) + (uint32_t)(i) * AI_REC_SIZE)
