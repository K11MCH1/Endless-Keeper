/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_scars.h
 *     Endless world: Scars (2026-11-13).
 * @par Purpose:
 *     Losing a fight without losing the dungeon: a room wrecked by raiders is desecrated (half efficiency until its
 *     slabs are laid again and a creature prays in a Temple); a Heart beaten below a quarter of its health keeps a crack
 *     (a tenth off its health) until it is mended with a hero's heart-shard or a heavy sum of gold dropped on it.
 */
/******************************************************************************/
#ifndef DK_WORLD_SCARS_H
#define DK_WORLD_SCARS_H

#include "bflib_basics.h"
#include "globals.h"

struct Thing;
struct Room;

#ifdef __cplusplus
extern "C" {
#endif

/** A hero is about to tear out a slab of this room (instf_attack_room_slab). */
void world_scars_room_hit(struct Room *room, const struct Thing *attacker);
/** A room's efficiency in percent of what it would be (50 while desecrated). */
int world_scars_room_percent(const struct Room *room);
/** A heroes heart fell: the keeper whose creature stood nearest gets a heart-shard. */
void world_scars_heroes_heart_fell(const struct Thing *heartng);
/** Each world turn (the simulation). */
void world_scars_turn(void);

void world_scars_reset(void);
size_t world_scars_save_size(void);
void world_scars_save(uint8_t *buf);
TbBool world_scars_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
