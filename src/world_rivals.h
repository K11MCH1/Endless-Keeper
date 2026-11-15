/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rivals.h
 *     Endless world: Rivals with Ambitions (2026-11-13).
 * @par Purpose:
 *     A rival keeper that a keeper wakes no longer dozes off again: it takes an ambition against the keeper who woke
 *     it, shown on its marker on the full-screen map: the Hoarder tunnels towards that keeper's far Treasuries, the
 *     Portal-grabber claims the neutral portals near that keeper's home, the Warlord trains on a countdown the keeper
 *     can see and then comes with everything.
 */
/******************************************************************************/
#ifndef DK_WORLD_RIVALS_H
#define DK_WORLD_RIVALS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WORLD_RIVALS_MAX 8   /* rivals holding an ambition at once (the page's open point) */

/** A world keeper (index k in the world's list, player slot) woke because waker came near: it may take an ambition. */
void world_rivals_keeper_woke(int k, PlayerNumber slot, PlayerNumber waker, int64_t heart_wx, int64_t heart_wy);
/** Does keeper k hold an ambition (then it doesn't go back to sleep)? */
TbBool world_rivals_holds(int k);
/** A keeper's slot was let go or it was destroyed: its ambition ends. */
void world_rivals_keeper_gone(int k);
/** Each world turn (the simulation). */
void world_rivals_turn(void);
/** The full-screen map: the label of rival i aimed at this machine's player, and its engine slab (false: none). */
TbBool world_rivals_map_label(int i, MapSlabCoord *ex, MapSlabCoord *ey, char *text, size_t size);

void world_rivals_reset(void);
size_t world_rivals_save_size(void);
void world_rivals_save(uint8_t *buf);
TbBool world_rivals_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
