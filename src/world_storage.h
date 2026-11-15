/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_storage.h
 *     Endless world: the Storage Room, where a keeper's loot is kept.
 * @par Purpose:
 *     Loot (keepsakes no creature carries, a World Boss's spoils) is registered here with its keeper. It is kept in
 *     the keeper's Storage Room: a room kind named STORAGE when the configuration defines one, otherwise the room of
 *     the Dungeon Heart (its slabs around the Heart itself). One item a slab. Loot of a keeper lying on that keeper's
 *     own ground outside the storage is brought in every 256 turns; loot with no room left stays where it is. Raiders
 *     cannot rob it (yet). The registry is saved (XTR2 record "STR1").
 */
/******************************************************************************/
#ifndef DK_WORLD_STORAGE_H
#define DK_WORLD_STORAGE_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct Room;

#define WORLD_LOOT_MAX 1024

/** A keeper's storage: its Storage Room with the most room left, or its Heart's room (NULL: none). */
struct Room *world_storage_room(PlayerNumber plyr_idx);
/** How many loot items the keeper's storage holds now, and can hold. */
int world_storage_count(PlayerNumber plyr_idx, int *capacity);
/** Mark an object as loot belonging to a keeper (or to nobody: PLAYER_NEUTRAL) and bring it into that keeper's
 *  storage. False when it could not be registered (the registry is full). */
TbBool world_storage_add_loot(struct Thing *thing, PlayerNumber plyr_idx);
/** Whether an object is loot, and whose. */
TbBool world_storage_is_loot(const struct Thing *thing, PlayerNumber *owner);
/** Puts a keeper's loot item on a free slab of its storage: true when it is in storage now. */
TbBool world_storage_put(struct Thing *thing, PlayerNumber plyr_idx);
/** A loot item leaves the registry (it is used up, or deleted). */
void world_storage_forget(const struct Thing *thing);
/** Each world turn (server or single player): loot brought in; the test hook. */
void world_storage_turn(void);

void world_storage_reset(void);
size_t world_storage_save_size(void);
void world_storage_save(uint8_t *buf);
TbBool world_storage_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
