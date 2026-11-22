/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_wallbreak.h
 *     Endless worlds: wild Beetles and raid Tunnellers break fortified walls.
 * @par Purpose:
 *     The user, 2026-11-15: "The beetles can break through fortified walls but only the ones outside of the rings of
 *     anchors against the dark. Breaking the fortified walls takes 1min." and the same for a raid's Tunnellers. A
 *     breakable wall is a keeper's fortified wall outside that keeper's anchor rings (world_dark_anchored: the Heart,
 *     portals, Treasuries, Guard Posts, lit watchfires). The breaker stands at it for WALLBREAK_TURNS; then the wall
 *     is dug out (open floor with rubble) and the breaker goes on through.
 */
/******************************************************************************/
#ifndef DK_WORLD_WALLBREAK_H
#define DK_WORLD_WALLBREAK_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define WALLBREAK_TURNS (60 * 20) /* one minute */

/** Do raid Tunnellers break fortified walls? Off since 2026-11-22 (test switch KFX_WORLD_RAIDWALLS=on). */
TbBool world_wallbreak_raids(void);
/** Do wild Beetles break fortified walls? Off since 2026-11-22 (test switch KFX_WORLD_BEETLEWALLS=on). */
TbBool world_wallbreak_beetles(void);
/** Is this engine slab a wall a Beetle or a raid's Tunneller may break? */
TbBool world_wallbreak_can(MapSlabCoord x, MapSlabCoord y);
/** The breaker works at the wall (x, y) beside it: 0 the wall can't be broken, 1 at work (the breaker is held where it
 *  stands), 2 the wall has just been dug through (open floor now). Called again and again while it works (once a second
 *  is enough); another wall, or the breaker moved away, starts the minute again. */
int world_wallbreak_work(struct Thing *breaker, MapSlabCoord x, MapSlabCoord y);
/** Is this creature breaking a wall now (held where it stands)? */
TbBool world_wallbreak_busy(const struct Thing *breaker);
/** instf_tunnel: is the subtile this creature strikes the wall it breaks on the timer? Then the strike is only its
 *  animation and sound (the wall takes no damage). */
TbBool world_wallbreak_strike_only(const struct Thing *thing, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** creature_follow_leader: while the leader breaks a wall, a follower near it wanders now and then instead of standing
 *  at its place behind the leader. True: handled (the follow state does nothing more this turn). */
TbBool world_wallbreak_follower_wanders(struct Thing *follower, const struct Thing *leader);
/** Once a turn (server or single player): raid Tunnellers that get no nearer and stand at a breakable wall on their
 *  way break it. */
void world_wallbreak_turn(void);
void world_wallbreak_reset(void);

#ifdef __cplusplus
}
#endif
#endif
