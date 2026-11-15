/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_overgrowth.h
 *     Endless world: the Overgrowth (generator version 8's biome): dug earth that nobody owns closes again at dawn.
 */
/******************************************************************************/
#ifndef DK_WORLD_OVERGROWTH_H
#define DK_WORLD_OVERGROWTH_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Once a game turn on the simulating machine (endless worlds of version 8). */
void world_overgrowth_turn(void);
/** A new world, a load, the end. */
void world_overgrowth_reset(void);

#ifdef __cplusplus
}
#endif
#endif
