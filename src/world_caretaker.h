/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_caretaker.h
 *     Endless worlds: the caretaker that keeps an offline player's dungeon.
 */
/******************************************************************************/
#ifndef DK_WORLD_CARETAKER_H
#define DK_WORLD_CARETAKER_H

#include "bflib_basics.h"
#include "globals.h"

struct Thing;

#ifdef __cplusplus
extern "C" {
#endif

/** A player left (or the world was loaded while they were away): a caretaker keeps their dungeon. */
TbBool world_caretaker_start(PlayerNumber plyr_idx);
/** The player is back: the caretaker goes. */
void world_caretaker_end(PlayerNumber plyr_idx);
/** A dungeon held as an away player's (world_caretaker_active is true) with no computer taking the seat: a keeper at
 *  a World Boss fight (world_boss.c). */
void world_caretaker_hold(PlayerNumber plyr_idx, TbBool on);
/** Is a caretaker keeping this dungeon (its imps dig nothing, its gold is held)? */
TbBool world_caretaker_active(PlayerNumber plyr_idx);
/** Endless worlds (2026-11-13): a creature of a dungeon the caretaker keeps gains no experience and no levels. */
TbBool world_creature_growth_frozen(const struct Thing *thing);
/** Every turn on the server (or the one machine): the gold held. */
void world_caretaker_turn(void);
/** A new world or a load: no caretakers yet. */
void world_caretaker_reset(void);
/** In single player, would this machine's packets be dropped (no player, not allocated, computer-controlled)? */
TbBool world_local_input_lost(void);

#ifdef __cplusplus
}
#endif
#endif
