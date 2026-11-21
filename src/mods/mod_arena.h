/* Mod arenas: one level number for all of them, the mod chosen by the port (mod_arena.c) */
#ifndef TM2PC_MOD_ARENA_H
#define TM2PC_MOD_ARENA_H
#include <stdint.h>

#define MOD_ARENA_LEVEL 13   /* the session's level for a mod arena (retail's are 1..12) */
#define MOD_ARENA_PLAYS_AS 7 /* gameGetLevel's answer meanwhile: Holland, which no per-level rule singles out */

struct CPU;
int mod_arena_choose(const char *id);   /* by id or uid: 1 if found and read */
int mod_arena_on(void);                 /* the session's level is the chosen mod arena */
const char *mod_arena_name(void);
const char *mod_arena_music(void);      /* its own music file, "" none */
const char *mod_arena_dir(void);        /* its folder (a slash at the end), "" none */
int mod_arena_test_level(void);         /* tests: TM2PC_TEST_MOD_ARENA=<id>: MOD_ARENA_LEVEL, else 0 */
int mod_arena_file(struct CPU *c, uint32_t dest, const char *name);       /* (vp_FileLoad) */
void mod_arena_file_name(struct CPU *c, const char *dir, const char *ext); /* (the level's DMD / TMS names) */

#endif
