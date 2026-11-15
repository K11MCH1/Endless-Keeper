/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dominion.h
 *     Endless world: dead dominions.
 * @par Purpose:
 *     Ruins of keepers who died long ago, found by exploring: broken rooms nobody owns (claimable, and a claimed one
 *     works whether or not its keeper has learnt to build it), traps nobody owns that are still armed, and an empty,
 *     cracked pedestal where a Dungeon Heart once beat. The farther from the nearest start, the better its rooms.
 */
/******************************************************************************/
#ifndef DK_WORLD_DOMINION_H
#define DK_WORLD_DOMINION_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct OutpostOverlay;

/** The dead dominions' cells and things in world chunk (cx, cy) (one is decided the first time its ground is made).
 *  ov->set[i] is 1 where a dominion builds the cell, 2 where it only turns rock into earth (the land round its rooms
 *  and its ways out); returns how many cells it decides. */
int world_dominion_overlay(int32_t cx, int32_t cy, struct OutpostOverlay *ov);
/** Is there a dead dominion (or one not decided yet that may be there) in this box of world slabs? */
TbBool world_dominion_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2);
/** Is a dead dominion decided in this box (the places of regions not decided yet are not counted)? */
TbBool world_dominion_placed_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2);
/** Test hook (KFX_WORLD_DDSURVEY=range): decides every region within range slabs of the start and logs them. */
void world_dominion_survey(int64_t range);
/** Test hook (KFX_WORLD_TESTDOMINION="T"): the dominion nearest the start, see world_dominion.c. */
void world_dominion_test_turn(void);
void world_dominions_reset(void);
size_t world_dominions_save_size(void);
void world_dominions_save(uint8_t *buf);
TbBool world_dominions_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
