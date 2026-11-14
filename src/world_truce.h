/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_truce.h
 *     Endless worlds: truces and friendly fire.
 * @par Purpose:
 *     Keepers who are allied (a co-op party), or truced for a while (the keepers at a World Boss's fight, inside its
 *     arena), do not fight each other: whom a creature goes for is seen to elsewhere. This is about what they can
 *     still do to each other without meaning to, or meaning to: a Bile Demon's gas over a mixed crowd, a missile that
 *     meets a friend on its way to the enemy, a keeper's Lightning.
 *
 *     The variable is friendly fire. Off (the default): keepers who are allied or truced cannot hurt each other's
 *     creatures. On: they can, as the game has it without this.
 *
 *     Every way of doing harm asks here, so a truce of a new kind needs one line in world_truce.c and nothing else.
 */
/******************************************************************************/
#ifndef DK_WORLD_TRUCE_H
#define DK_WORLD_TRUCE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** Friendly fire: can keepers who are allied or truced hurt each other's creatures? Off in a new world. Saved with
 *  the world. Outside endless worlds the game's own rules hold (this answers true there). */
TbBool world_friendly_fire(void);
void world_set_friendly_fire(TbBool on);

/** Are these two different players at peace anywhere in the world: allied, as the game has it (in an endless world,
 *  the keepers of a co-op party)? A truce that holds only in a place (a World Boss's arena) is not asked here: it
 *  depends on where the creature is (world_truce_no_harm). */
TbBool world_truce_allied(PlayerNumber a, PlayerNumber b);

/** Must this damage leave the creature unharmed? Yes when friendly fire is off and `from` (the player whose blow,
 *  shot, area attack, trap or spell it is) is allied or truced with the creature's keeper. Never for the creature's
 *  own keeper, for the heroes or for damage that is nobody's. Asked where damage is dealt (apply_damage_to_thing). */
TbBool world_truce_no_harm(const struct Thing *victim, HitPoints dmg, PlayerNumber from);

/** Must `from`'s shots, area effects and keeper spells pass this creature by altogether? Yes when friendly fire is
 *  off and the two keepers are truced without being allied (the game's own rules for allies know nothing of such a
 *  truce, and would freeze, slow, push or turn the creature into a chicken as an enemy). Allies are left to the
 *  game's own rules here, which let an ally's helpful spells through; the harm those rules let through is stopped by
 *  world_truce_no_harm. Asked where a shot or an area effect picks what it hits (thing_is_shootable) and where a
 *  keeper's spell picks its target (can_cast_power_on_thing). */
TbBool world_truce_no_touch(const struct Thing *victim, PlayerNumber from);

/** Every turn (a test switch only; see world_truce.c). */
void world_truce_turn(void);
/** A new world or a load. */
void world_truce_reset(void);
size_t world_truce_save_size(void);
void world_truce_save(uint8_t *buf);
TbBool world_truce_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
