/* Rollback's states and re-runs (rb_state.c). */
#ifndef TM2_NET_RB_STATE_H
#define TM2_NET_RB_STATE_H

#include <stdint.h>
#include <stdio.h>

extern int rbs_resimulating;  /* steps being run again: the frame hook runs only the step's own work */
int rbs_available(void);      /* the build has the decomp's rollback pieces and the regions */
void rbs_save(int32_t step);  /* the state now: the start of step */
int rbs_load(int32_t step);   /* 1: the state kept for step is the current one */
uint32_t rbs_hash(void);      /* the current state's hash (the regions' hash rows, the nodes, the port's own) */
/* steps from..to-1 again from the state kept for from, nothing drawn; inputs(ctx, f) writes step f's input before it
 * runs. Returns the steps run (fewer: the round ended), -1 no state for from */
int rbs_rerun(int32_t from, int32_t to, void (*inputs)(void *ctx, int32_t step), void *ctx);
/* a batch of steps run again (the rollback engine's): begin (sound and palette uploads held), each step (after its input
 * is written; 1: the round ended in it, stop), end (the sounds whose cause is gone stopped, the palettes uploaded) */
void rbs_batch_begin(void);
int rbs_rerun_step(int32_t step);
void rbs_batch_end(void);
void rbs_live_step(void);     /* hle_SetFrameStart: a live step ran (not one run again) */
/* the main loop's iteration ended (pauseMenu, after the frame went to the GPU): with STEP_SPLIT the state there is the
 * next step's start, and nothing of this iteration's drawing is still to be read */
void rbs_iteration_end(void);
void rbs_keep(void *p, size_t n); /* port state only a rollback puts back (not the kill cam's: profile/feat_hooks.c) */
void rbs_live(void *p, size_t n); /* host state live across a batch (real time: the Stats screen's seconds), 8 bytes at most */
void rbs_new_round(void);
void rbs_dmd_again(void);      /* the models' list made again at its next use (every machine at the same step) */
int rbs_dmd_list(uint32_t *buf); /* the models' mutable node fields: (address, bytes) pairs at *buf, their count */
void rbs_debug_blocks(int32_t step);
void rbs_diff_begin(int32_t step); /* a resync: this machine's own state of step kept (before the host's is put in) */
void rbs_diff_end(int step);       /* and what differs from the host's, logged by name */
void rbs_hash_dump(const char *path); /* tests: every hashed word to a file */
void rbs_span_hashes(FILE *f, int32_t step); /* tests: each hashed span's hash */ /* tests: each kept block's hash and the guest regions', one line */     /* a round starts: the states kept are the round before's, none to go back to */

#endif
