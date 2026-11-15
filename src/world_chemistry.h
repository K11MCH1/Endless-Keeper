/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_chemistry.h
 *     Endless world: Foul Chemistry - attacks react with each other.
 */
/******************************************************************************/
#ifndef DK_WORLD_CHEMISTRY_H
#define DK_WORLD_CHEMISTRY_H

#include "globals.h"
#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** A shot's damage on a creature, before it is dealt: ice and boulders, fire and ice, fire and water. */
HitPoints world_chemistry_shot_damage(const struct Thing *shotng, struct Thing *target, HitPoints dmg);
/** After a shot hit a creature (its damage dealt, its spell cast): lightning and water, ice and water. */
void world_chemistry_shot_hit(const struct Thing *shotng, struct Thing *shooter, struct Thing *target, HitPoints dmg);
/** Every turn: gas and fire. */
void world_chemistry_turn(void);
void world_chemistry_test_hook(void);

#ifdef __cplusplus
}
#endif
#endif
