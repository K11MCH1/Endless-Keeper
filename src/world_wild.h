/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_wild.h
 *     Endless world: wild creatures, spawned and despawned the way Minecraft spawns monsters.
 */
/******************************************************************************/
#ifndef DK_WORLD_WILD_H
#define DK_WORLD_WILD_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** Once per game turn in endless levels (from world_process_turn): spawning and despawning. */
void world_wild_turn(void);

/** Is this a wild creature? (Those aren't frozen with their chunk: they despawn when it leaves the window.) */
TbBool world_wild_creature(const struct Thing *thing);
/** The level (1-based) of a keeper's strongest creature; 1 for none. */
int world_keeper_top_level(PlayerNumber plyr_idx);

#ifdef __cplusplus
}
#endif
#endif
