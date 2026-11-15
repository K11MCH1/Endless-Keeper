/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_hunt.h
 *     Endless world: The Hunt (2026-11-13).
 * @par Purpose:
 *     Every wild kind has a ladder of five ranks, climbed by hunting that kind at night. Kill enough of a kind after dusk
 *     and its Alpha comes for you the following night: two levels above its pack, with one trick of its own, out of a
 *     burrow near your dungeon, digging for your Heart as far as your walls. Each Alpha killed raises your rank with
 *     its kind: more gold from its dead, then fewer of its packs near your Heart, at last a tame one in your Lair.
 */
/******************************************************************************/
#ifndef DK_WORLD_HUNT_H
#define DK_WORLD_HUNT_H

#include "bflib_basics.h"
#include "globals.h"

struct Thing;

#ifdef __cplusplus
extern "C" {
#endif

/** A creature really died (kill_creature, after Misdeeds): a wild one killed by a keeper's creature at night counts. */
void world_hunt_kill(struct Thing *victim, struct Thing *killertng, PlayerNumber killer_plyr_idx, unsigned long death_flags);
/** A wild pack of this kind is about to appear here: false when a keeper's rank thins it out. */
TbBool world_hunt_pack_allowed(ThingModel model, MapSlabCoord x, MapSlabCoord y);
/** Each world turn (the simulation). */
void world_hunt_turn(void);

void world_hunt_reset(void);
size_t world_hunt_save_size(void);
void world_hunt_save(uint8_t *buf);
TbBool world_hunt_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
