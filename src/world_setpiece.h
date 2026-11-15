/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_setpiece.h
 *     Endless world: set pieces.
 * @par Purpose:
 *     Places drawn by hand, set down in a generated world: small pieces, each for some biomes, circles and a weight,
 *     placed by seed, turned and mirrored. The first eight are built in; more come as files
 *     (levels/endless_cfgs/setpieces/<name>.txt, the same text as the built-in ones: see world_setpiece.c).
 */
/******************************************************************************/
#ifndef DK_WORLD_SETPIECE_H
#define DK_WORLD_SETPIECE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct OutpostOverlay;

/** The set pieces' cells and things in world chunk (cx, cy); ov->set[i] is 1 where a piece decides the cell, 2 where
 *  it only turns rock into earth (round it, and its ways out). Returns how many cells it decides. */
int world_setpiece_overlay(int32_t cx, int32_t cy, struct OutpostOverlay *ov);
/** Is there a set piece (or one not decided yet that may be there) in this box of world slabs? */
TbBool world_setpiece_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2);
/** Is a set piece decided in this box (the places of regions not decided yet are not counted)? */
TbBool world_setpiece_placed_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2);
/** Test hooks: KFX_WORLD_SPSURVEY=range (decides every region within range slabs and logs which pieces),
 *  KFX_WORLD_TESTSETPIECE="T[,name]" (see world_setpiece.c). */
void world_setpiece_survey(int64_t range);
void world_setpiece_test_turn(void);
/** A new world or a load: the pieces are read again (built-in and files) and nothing is decided. */
void world_setpieces_reset(void);
size_t world_setpieces_save_size(void);
void world_setpieces_save(uint8_t *buf);
TbBool world_setpieces_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
