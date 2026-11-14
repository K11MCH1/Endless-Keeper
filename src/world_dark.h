/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dark.h
 *     Endless world: the dark takes it back.
 * @par Purpose:
 *     Claimed land needs an anchor: the Dungeon Heart (20 slabs), a claimed portal (12), a Treasury or a Guard Post
 *     (10), or a lit watchfire (its light, 6). Floor of a keeper that no anchor of that keeper reaches is on its own:
 *     it piles up neglect while no imp of its keeper is within 3 slabs of it, and an imp nearby clears it. After three
 *     days of neglect it turns back into bare path, and the keeper's reinforced walls beside it that no longer touch
 *     the keeper's floor go back to earth. A day before, the keeper is warned.
 *     The loaded world is gone through one chunk a turn. Nothing ages under the caretaker (an away keeper) or in a
 *     frozen chunk (it isn't loaded). Neglect is saved (XTR2 record "DRK1").
 */
/******************************************************************************/
#ifndef DK_WORLD_DARK_H
#define DK_WORLD_DARK_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DARK_DAYS 3
#define DARK_REACH_HEART 20
#define DARK_REACH_PORTAL 12
#define DARK_REACH_ROOM 10      /* a Treasury, a Guard Post */
#define DARK_IMP_SLABS 3
#define DARK_SLOTS 32768        /* neglected slabs kept at once (the world over) */

/** Is this engine slab within the reach of one of the keeper's anchors? */
TbBool world_dark_anchored(PlayerNumber plyr_idx, MapSlabCoord x, MapSlabCoord y);
/** Each world turn (server or single player): one loaded chunk gone through; the test hook. */
void world_dark_turn(void);

void world_dark_reset(void);
size_t world_dark_save_size(void);
void world_dark_save(uint8_t *buf);
TbBool world_dark_load(const uint8_t *buf, size_t len);

/** A keeper's anchors (Heart, portals, Treasuries, Guard Posts): engine slab and reach; the count (at most max). */
int world_dark_anchor_list(PlayerNumber p, MapSlabCoord *x, MapSlabCoord *y, int *r, int max);

#ifdef __cplusplus
}
#endif
#endif
