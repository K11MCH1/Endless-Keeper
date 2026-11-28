/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_genahead.h
 *     Endless world: chunks generated ahead on a worker thread (on by default; KFX_WORLD_GENAHEAD=0 turns it off).
 */
/******************************************************************************/
#ifndef DK_WORLD_GENAHEAD_H
#define DK_WORLD_GENAHEAD_H

#include "bflib_basics.h"
#include "world_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Is the switch on (read once)? */
TbBool wga_enabled(void);
/** A worker with a generator of its own for this world (the same seed, version and test setting as W.gen). */
void wga_start(uint64_t seed, int version, int rival_min_dist);
/** Stops the worker and forgets everything (level end, another world). */
void wga_stop(void);
/** Asks for a chunk's generator output: `full` keeps the whole WgChunk (a chunk about to be loaded), otherwise only
 *  its slabs and owners (a raid's landing search). Nothing waits; a full queue drops the request. */
void wga_request(int32_t cx, int32_t cy, TbBool full);
/** The whole generator output of a chunk, if the worker has it (it is then forgotten). */
TbBool wga_take_full(int32_t cx, int32_t cy, struct WgChunk *out);
/** The slabs and owners of a chunk, if the worker has them (full or not). */
TbBool wga_peek(int32_t cx, int32_t cy, uint8_t *slabs, uint8_t *owners);
/** Counters for the log: requests, results used, results generated, dropped. */
void wga_stats(unsigned long *requested, unsigned long *used, unsigned long *made, unsigned long *dropped);

#ifdef __cplusplus
}
#endif
#endif
