/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_consecrate.h
 *     Endless world: Consecrated Ground (2026-11-13).
 * @par Purpose:
 *     A hero Monk, or a heroes' chapel (a Temple of theirs, in an outpost or a fortress), sheds a small ring where a
 *     keeper's Hand does nothing: no drops, no pick-ups, no slaps and no spells, until the Monk dies or the chapel is
 *     broken. The ring is marked on the ground by white sparkles.
 */
/******************************************************************************/
#ifndef DK_WORLD_CONSECRATE_H
#define DK_WORLD_CONSECRATE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WORLD_RINGS_MAX 24

/** Is this subtile inside a consecrated ring, for a human keeper (whose Hand does nothing there)? */
TbBool world_consecrated_at(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** The simulation: the rings, from the Monks and chapels on the map (every 8 turns). */
void world_consecrate_turn(void);
/** This machine: the sparkles that mark the rings. */
void world_consecrate_local_turn(void);

#ifdef __cplusplus
}
#endif
#endif
